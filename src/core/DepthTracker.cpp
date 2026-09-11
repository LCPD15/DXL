#include "DepthTracker.h"

#include "CommandListTracker.h"
#include "HookTeardown.h"

#include "../common/Log.h"
#include "D3D12VTableLayout.h"
#include "NgxEavesdrop.h"

namespace DXL {

namespace {

using CreateDepthStencilViewFn = void(STDMETHODCALLTYPE*)(
	ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*,
	D3D12_CPU_DESCRIPTOR_HANDLE);
using OMSetRenderTargetsFn = void(STDMETHODCALLTYPE*)(
	ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
	const D3D12_CPU_DESCRIPTOR_HANDLE*);
using ClearDepthStencilViewFn = void(STDMETHODCALLTYPE*)(
	ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE,
	D3D12_CLEAR_FLAGS, FLOAT, UINT8, UINT, const D3D12_RECT*);
using SetDescriptorHeapsFn = void(STDMETHODCALLTYPE*)(
	ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap* const*);
using SetPipelineStateFn = void(STDMETHODCALLTYPE*)(
	ID3D12GraphicsCommandList*, ID3D12PipelineState*);
using SetRootSignatureFn = void(STDMETHODCALLTYPE*)(
	ID3D12GraphicsCommandList*, ID3D12RootSignature*);
using ResourceBarrierFn = void(STDMETHODCALLTYPE*)(
	ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);

CreateDepthStencilViewFn g_originalCreateDepthStencilView = nullptr;
OMSetRenderTargetsFn g_originalOMSetRenderTargets = nullptr;
ClearDepthStencilViewFn g_originalClearDepthStencilView = nullptr;
ResourceBarrierFn g_originalResourceBarrier = nullptr;
SetDescriptorHeapsFn g_originalSetDescriptorHeaps = nullptr;
SetPipelineStateFn g_originalSetPipelineState = nullptr;
SetRootSignatureFn g_originalSetComputeRootSignature = nullptr;
SetRootSignatureFn g_originalSetGraphicsRootSignature = nullptr;

// 深度格式才算候选。TYPELESS 也要算：很多引擎用 typeless 资源配 typed 视图。
bool IsDepthFormat(DXGI_FORMAT format) noexcept {
	switch (format) {
	case DXGI_FORMAT_D32_FLOAT:
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_D16_UNORM:
	case DXGI_FORMAT_R32_TYPELESS:
	case DXGI_FORMAT_R32G8X24_TYPELESS:
	case DXGI_FORMAT_R24G8_TYPELESS:
	case DXGI_FORMAT_R16_TYPELESS:
		return true;
	default:
		return false;
	}
}

void* PatchVTableSlot(void* instance, size_t index, void* replacement) noexcept {
	void** vtable = *reinterpret_cast<void***>(instance);
	DWORD oldProtect = 0;
	if (!VirtualProtect(&vtable[index], sizeof(void*),
		PAGE_READWRITE, &oldProtect)) {
		return nullptr;
	}
	void* original = vtable[index];
	vtable[index] = replacement;
	VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);
	FlushInstructionCache(GetCurrentProcess(), &vtable[index], sizeof(void*));
	// 深度探测的四个补丁也进退出还原登记簿 —— 见 HookTeardown.h 顶部说明。
	RecordVtablePatch(&vtable[index], original);
	return original;
}

// 当前这条命令列表绑的是哪个深度候选。
// 用 thread_local：一条命令列表同一时刻只会被一个线程录制，所以线程本地就等价于
// 每条列表一份，而且没有锁。
thread_local int t_boundDepth = -1;

}  // namespace

DepthTracker& GetDepthTracker() noexcept {
	static DepthTracker instance;
	return instance;
}

int DepthTracker::FindOrAddCandidate(ID3D12Resource* resource) noexcept {
	if (!resource) return -1;
	// **帧生成开启时深度追踪器整体退场。**
	//
	// 深度追踪器唯一的消费者是 NR 的"真深度"输入；而 FG 与 NR 冲突、检测到 FG 时
	// NR 已被禁用 —— 这份观察没有下家了。更糟的是 FG 为插帧会创建大量深度/矢量
	// 缓冲，每个都进来报到，候选表爆炸 + LogDepthCandidates 每帧狂打印（同步 fflush
	// 落盘），实测 16 秒刷 6 万行日志把 present 线程拖垮 → FG 插帧时序错乱 → 黑屏 +
	// present 卡死（生化9 开 FG 复现）。所以 FG 开启时直接不观察。
	if (NgxEavesdrop::Get().FrameGenerationActive(0)) return -1;
	const uint32_t count = _count.load(std::memory_order_acquire);
	for (uint32_t i = 0; i < count; ++i) {
		if (_entries[i].resource == resource) return int(i);
	}
	const D3D12_RESOURCE_DESC desc = resource->GetDesc();
	if (!IsDepthFormat(desc.Format)) return -1;

	// 先捡被 EndFrame 清空的槽位再往后扩，否则表会被"曾经存在过的深度缓冲"填满
	uint32_t slot = count;
	for (uint32_t i = 0; i < count; ++i) {
		if (!_entries[i].resource) {
			slot = i;
			break;
		}
	}
	if (slot == count && count >= MAX_CANDIDATES) {
		// 表满了：淘汰一个上一帧既没被绑定也没被清除的（也就是没在用的）。
		// 找不到可淘汰的就只能放弃这一个。
		slot = MAX_CANDIDATES;
		for (uint32_t i = 0; i < MAX_CANDIDATES; ++i) {
			if (!_entries[i].bindsLastFrame && !_entries[i].clearsLastFrame &&
				_entries[i].resource != _selected) {
				slot = i;
				break;
			}
		}
		if (slot == MAX_CANDIDATES) return -1;
		// 这个槽位换了资源，之前指向它的 DSV 映射全部失效
		const uint32_t dsvCount = _dsvCount.load(std::memory_order_acquire);
		for (uint32_t i = 0; i < dsvCount; ++i) {
			if (_dsvMap[i].candidate == int(slot)) _dsvMap[i].candidate = -1;
		}
	}

	Entry& entry = _entries[slot];
	entry.resource = resource;
	entry.width = uint32_t(desc.Width);
	entry.height = desc.Height;
	entry.format = desc.Format;
	entry.binds.store(0, std::memory_order_relaxed);
	entry.clears.store(0, std::memory_order_relaxed);
	entry.bindsLastFrame = 0;
	entry.clearsLastFrame = 0;
	entry.staleFrames = 0;
	entry.stateKnown.store(false, std::memory_order_relaxed);
	if (slot == count) _count.store(count + 1, std::memory_order_release);
	D5_LOG_INFO(L"发现深度缓冲候选 #%u: %ux%u fmt=%u%s", slot,
		entry.width, entry.height, (unsigned)entry.format,
		slot == count ? L"" : L"（淘汰了一个未使用的槽位）");
	// **密集度判据**：短时间内发现大量新候选 = 换场景（进/退场景），触发 NR 暂停 +
	// reset。移动时深度缓冲也在轮换（fmt=39/53 交替），但那是稀疏的（每 1~2 秒 1~2
	// 个），不算换场景 —— 否则移动时频繁暂停导致画面"一会有一会没 NR"。进场景实测
	// 是 0.4 秒内 ~26 个新候选（密集爆发），移动小爆发最多 0.8 秒 ~13 个。
	constexpr uint64_t BURST_WINDOW_MS = 500;
	constexpr uint32_t BURST_THRESHOLD = 15;
	const uint64_t now = GetTickCount64();
	if (now - _burstWindowStart > BURST_WINDOW_MS) {
		_burstWindowStart = now;
		_burstCount = 0;
	}
	if (++_burstCount >= BURST_THRESHOLD) {
		NgxEavesdrop::Get().NoteDepthChanged();
		_burstCount = 0;
	}
	return int(slot);
}

int DepthTracker::CandidateForHandle(SIZE_T handle) const noexcept {
	const uint32_t count = _dsvCount.load(std::memory_order_acquire);
	for (uint32_t i = 0; i < count; ++i) {
		if (_dsvMap[i].handle == handle) return _dsvMap[i].candidate;
	}
	return -1;
}

/* ============================ hook 实现 ============================ */

struct DepthTrackerHooks {
	static void STDMETHODCALLTYPE CreateDepthStencilView(
		ID3D12Device* device,
		ID3D12Resource* resource,
		const D3D12_DEPTH_STENCIL_VIEW_DESC* desc,
		D3D12_CPU_DESCRIPTOR_HANDLE handle) {
		DepthTracker& tracker = GetDepthTracker();
		if (resource) {
			const int candidate = tracker.FindOrAddCandidate(resource);
			if (candidate >= 0) {
				// 同一个句柄可能被复用指向别的资源，覆盖即可
				const uint32_t count =
					tracker._dsvCount.load(std::memory_order_acquire);
				uint32_t slot = count;
				for (uint32_t i = 0; i < count; ++i) {
					if (tracker._dsvMap[i].handle == handle.ptr) {
						slot = i;
						break;
					}
				}
				if (slot < DepthTracker::MAX_DSV) {
					tracker._dsvMap[slot].handle = handle.ptr;
					tracker._dsvMap[slot].candidate = candidate;
					if (slot == count) {
						tracker._dsvCount.store(count + 1,
							std::memory_order_release);
					}
				}
			}
		}
		g_originalCreateDepthStencilView(device, resource, desc, handle);
	}

	static void STDMETHODCALLTYPE OMSetRenderTargets(
		ID3D12GraphicsCommandList* commandList,
		UINT renderTargetCount,
		const D3D12_CPU_DESCRIPTOR_HANDLE* renderTargets,
		BOOL singleHandleToDescriptorRange,
		const D3D12_CPU_DESCRIPTOR_HANDLE* depthStencil) {
		DepthTracker& tracker = GetDepthTracker();
		if (depthStencil) {
			const int candidate = tracker.CandidateForHandle(depthStencil->ptr);
			t_boundDepth = candidate;
			if (candidate >= 0) {
				tracker._entries[candidate].binds.fetch_add(
					1, std::memory_order_relaxed);
			}
		} else {
			t_boundDepth = -1;
		}
		g_originalOMSetRenderTargets(commandList, renderTargetCount, renderTargets,
			singleHandleToDescriptorRange, depthStencil);
	}

	static void STDMETHODCALLTYPE ClearDepthStencilView(
		ID3D12GraphicsCommandList* commandList,
		D3D12_CPU_DESCRIPTOR_HANDLE depthStencil,
		D3D12_CLEAR_FLAGS flags,
		FLOAT depth,
		UINT8 stencil,
		UINT rectCount,
		const D3D12_RECT* rects) {
		DepthTracker& tracker = GetDepthTracker();
		const int candidate = tracker.CandidateForHandle(depthStencil.ptr);
		if (candidate >= 0) {
			tracker._entries[candidate].clears.fetch_add(
				1, std::memory_order_relaxed);
			// 能被 Clear 就说明此刻它一定处于 DEPTH_WRITE（D3D12 的硬要求）。
			// 这是少数几个我们能确知资源状态的时刻。
			tracker._entries[candidate].state.store(
				D3D12_RESOURCE_STATE_DEPTH_WRITE, std::memory_order_relaxed);
			tracker._entries[candidate].stateKnown.store(
				true, std::memory_order_relaxed);
		}
		g_originalClearDepthStencilView(commandList, depthStencil, flags, depth,
			stencil, rectCount, rects);
	}

	// ---- 下面四个只是**记账**，不改变任何行为 ----
	//
	// D3D12 的命令列表没有 getter，所以"游戏当前绑了什么"只能靠 hook setter 记下来。
	// 在游戏的命令列表上插计算 pass 之后要把这些原样放回去，否则游戏后面的绘制会拿着
	// 指向旧描述符堆的根表句柄去采样 → GPU 越界 → 闪退（实测过）。
	// 见 CommandListTracker.h。

	static void STDMETHODCALLTYPE SetDescriptorHeaps(
		ID3D12GraphicsCommandList* commandList,
		UINT numHeaps,
		ID3D12DescriptorHeap* const* heaps) {
		CommandListTracker::Get().NoteDescriptorHeaps(
			commandList, numHeaps, heaps);
		g_originalSetDescriptorHeaps(commandList, numHeaps, heaps);
	}

	static void STDMETHODCALLTYPE SetPipelineState(
		ID3D12GraphicsCommandList* commandList, ID3D12PipelineState* pso) {
		CommandListTracker::Get().NotePipelineState(commandList, pso);
		g_originalSetPipelineState(commandList, pso);
	}

	static void STDMETHODCALLTYPE SetComputeRootSignature(
		ID3D12GraphicsCommandList* commandList, ID3D12RootSignature* sig) {
		CommandListTracker::Get().NoteComputeRootSignature(commandList, sig);
		g_originalSetComputeRootSignature(commandList, sig);
	}

	static void STDMETHODCALLTYPE SetGraphicsRootSignature(
		ID3D12GraphicsCommandList* commandList, ID3D12RootSignature* sig) {
		CommandListTracker::Get().NoteGraphicsRootSignature(commandList, sig);
		g_originalSetGraphicsRootSignature(commandList, sig);
	}

	// 跟踪深度资源的状态迁移。present 时要拷它，必须知道它此刻是什么状态。
	static void STDMETHODCALLTYPE ResourceBarrier(
		ID3D12GraphicsCommandList* commandList,
		UINT barrierCount,
		const D3D12_RESOURCE_BARRIER* barriers) {
		if (barriers) {
			DepthTracker& tracker = GetDepthTracker();
			const uint32_t count = tracker._count.load(std::memory_order_acquire);
			for (UINT i = 0; i < barrierCount; ++i) {
				const D3D12_RESOURCE_BARRIER& barrier = barriers[i];
				if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
				// 旁听那边也要知道资源状态 —— 它要在游戏的命令列表上拷深度/矢量，
				// 而 CopyResource 前的 transition 不能猜 before 状态。
				NgxEavesdrop::Get().NoteBarrier(
					barrier.Transition.pResource, barrier.Transition.StateAfter,
					commandList);
				for (uint32_t c = 0; c < count; ++c) {
					if (tracker._entries[c].resource ==
						barrier.Transition.pResource) {
						tracker._entries[c].state.store(
							barrier.Transition.StateAfter,
							std::memory_order_relaxed);
						tracker._entries[c].stateKnown.store(
							true, std::memory_order_relaxed);
						break;
					}
				}
			}
		}
		g_originalResourceBarrier(commandList, barrierCount, barriers);
	}
};

/* ============================ 安装 ============================ */

bool DepthTracker::Install(
	ID3D12Device* device, ID3D12GraphicsCommandList* probeCommandList) noexcept {
	if (_installed) return true;
	if (!device || !probeCommandList) return false;

	// 索引数错一位就是在游戏里调错函数，先自检
	if (!VTableCheck::VerifyDevice(device) ||
		!VTableCheck::VerifyCommandList(probeCommandList)) {
		D5_LOG_ERROR(L"vtable 布局和预期不符，不安装深度探测（其他功能不受影响）");
		return false;
	}

	g_originalCreateDepthStencilView =
		reinterpret_cast<CreateDepthStencilViewFn>(PatchVTableSlot(
			device, VT_DEVICE_CREATE_DEPTH_STENCIL_VIEW,
			reinterpret_cast<void*>(&DepthTrackerHooks::CreateDepthStencilView)));
	g_originalOMSetRenderTargets =
		reinterpret_cast<OMSetRenderTargetsFn>(PatchVTableSlot(
			probeCommandList, VT_LIST_OM_SET_RENDER_TARGETS,
			reinterpret_cast<void*>(&DepthTrackerHooks::OMSetRenderTargets)));
	g_originalClearDepthStencilView =
		reinterpret_cast<ClearDepthStencilViewFn>(PatchVTableSlot(
			probeCommandList, VT_LIST_CLEAR_DEPTH_STENCIL_VIEW,
			reinterpret_cast<void*>(&DepthTrackerHooks::ClearDepthStencilView)));
	g_originalResourceBarrier =
		reinterpret_cast<ResourceBarrierFn>(PatchVTableSlot(
			probeCommandList, VT_LIST_RESOURCE_BARRIER,
			reinterpret_cast<void*>(&DepthTrackerHooks::ResourceBarrier)));

	g_originalSetDescriptorHeaps =
		reinterpret_cast<SetDescriptorHeapsFn>(PatchVTableSlot(
			probeCommandList, VT_LIST_SET_DESCRIPTOR_HEAPS,
			reinterpret_cast<void*>(&DepthTrackerHooks::SetDescriptorHeaps)));
	g_originalSetPipelineState =
		reinterpret_cast<SetPipelineStateFn>(PatchVTableSlot(
			probeCommandList, VT_LIST_SET_PIPELINE_STATE,
			reinterpret_cast<void*>(&DepthTrackerHooks::SetPipelineState)));
	g_originalSetComputeRootSignature =
		reinterpret_cast<SetRootSignatureFn>(PatchVTableSlot(
			probeCommandList, VT_LIST_SET_COMPUTE_ROOT_SIGNATURE,
			reinterpret_cast<void*>(&DepthTrackerHooks::SetComputeRootSignature)));
	g_originalSetGraphicsRootSignature =
		reinterpret_cast<SetRootSignatureFn>(PatchVTableSlot(
			probeCommandList, VT_LIST_SET_GRAPHICS_ROOT_SIGNATURE,
			reinterpret_cast<void*>(&DepthTrackerHooks::SetGraphicsRootSignature)));

	const bool rootsReady = CommandListTracker::InstallRootHooks(probeCommandList, &PatchVTableSlot);
	if (!rootsReady || !g_originalSetDescriptorHeaps || !g_originalSetPipelineState ||
		!g_originalSetComputeRootSignature || !g_originalSetGraphicsRootSignature) {
		CommandListTracker::rootsReady.store(false);
		D5_LOG_ERROR(L"命令列表状态记账的 hook 没装全：heaps=%p pso=%p "
			L"computeSig=%p graphicsSig=%p —— evaluate 点那条路会因此拒绝执行",
			g_originalSetDescriptorHeaps, g_originalSetPipelineState,
			g_originalSetComputeRootSignature, g_originalSetGraphicsRootSignature);
	}

	if (!g_originalCreateDepthStencilView || !g_originalOMSetRenderTargets ||
		!g_originalClearDepthStencilView || !g_originalResourceBarrier) {
		D5_LOG_ERROR(L"深度探测的 hook 没装全：dsv=%p omset=%p clear=%p barrier=%p",
			g_originalCreateDepthStencilView, g_originalOMSetRenderTargets,
			g_originalClearDepthStencilView, g_originalResourceBarrier);
		return false;
	}

	_installed = true;
	D5_LOG_INFO(L"深度探测已安装");
	return true;
}

void DepthTracker::Invalidate() noexcept {
	const uint32_t count = _count.load(std::memory_order_acquire);
	if (!count) return;
	D5_LOG_INFO(L"深度候选表作废（%u 条）—— 游戏重建了 swapchain / 改了分辨率，"
		L"旧的深度缓冲已经不存在了", count);
	// 不动 _count：条目就地清空，槽位留给 FindOrAddCandidate 复用。
	// 直接把 _count 归零会和正在跑的 hook 线程赛跑（它们按 _count 读表）。
	for (uint32_t i = 0; i < count; ++i) {
		Entry& entry = _entries[i];
		entry.resource = nullptr;
		entry.width = 0;
		entry.height = 0;
		entry.format = DXGI_FORMAT_UNKNOWN;
		entry.binds.store(0, std::memory_order_relaxed);
		entry.clears.store(0, std::memory_order_relaxed);
		entry.bindsLastFrame = 0;
		entry.clearsLastFrame = 0;
		entry.staleFrames = 0;
		entry.stateKnown.store(false, std::memory_order_relaxed);
	}
	const uint32_t dsvCount = _dsvCount.load(std::memory_order_acquire);
	for (uint32_t i = 0; i < dsvCount; ++i) _dsvMap[i].candidate = -1;
	_selected = nullptr;
	_selectedIndex = -1;
	_selectedWidth = 0;
	_selectedHeight = 0;
	_selectedFormat = DXGI_FORMAT_UNKNOWN;
}

/* ============================ 每帧选择 ============================ */

void DepthTracker::EndFrame(
	uint32_t renderWidth, uint32_t renderHeight) noexcept {
	// FG 开启时退场（同 FindOrAddCandidate 的说明）：不归档、不淘汰、不挑选，也就
	// 不会触发 LogDepthCandidates 的刷屏。候选表原地保留但不增长、不再有消费者。
	if (NgxEavesdrop::Get().FrameGenerationActive(0)) return;
	const uint32_t count = _count.load(std::memory_order_acquire);

	// 归档本帧计数，并给一直没被用到的条目累计"陈旧帧数"
	for (uint32_t i = 0; i < count; ++i) {
		Entry& entry = _entries[i];
		entry.bindsLastFrame = entry.binds.exchange(0, std::memory_order_relaxed);
		entry.clearsLastFrame = entry.clears.exchange(0, std::memory_order_relaxed);
		if (entry.bindsLastFrame || entry.clearsLastFrame) {
			entry.staleFrames = 0;
			continue;
		}
		if (!entry.resource) continue;
		if (++entry.staleFrames < MAX_STALE_FRAMES) continue;

		// 长期没人用 —— 大概率已经被游戏销毁了。丢掉它，连带清掉指向它的 DSV
		// 映射。留着的唯一后果是：新资源分配到同一地址时，我们会拿旧尺寸去认它。
		D5_LOG_INFO(L"深度候选 #%u（%ux%u）连续 %u 帧没被使用，丢弃",
			i, entry.width, entry.height, MAX_STALE_FRAMES);
		const uint32_t dsvCount = _dsvCount.load(std::memory_order_acquire);
		for (uint32_t d = 0; d < dsvCount; ++d) {
			if (_dsvMap[d].candidate == int(i)) _dsvMap[d].candidate = -1;
		}
		entry.resource = nullptr;
		entry.width = 0;
		entry.height = 0;
		entry.format = DXGI_FORMAT_UNKNOWN;
		entry.staleFrames = 0;
		entry.stateKnown.store(false, std::memory_order_relaxed);
		if (_selectedIndex == int(i)) {
			_selected = nullptr;
			_selectedIndex = -1;
		}
	}

	// 手动指定优先
	if (_manualIndex >= 0 && uint32_t(_manualIndex) < count) {
		const Entry& entry = _entries[_manualIndex];
		_selected = entry.resource;
		_selectedWidth = entry.width;
		_selectedHeight = entry.height;
		_selectedFormat = entry.format;
		_selectedIndex = _manualIndex;
		return;
	}

	// 自动挑选。评分从高到低：
	//   1. 尺寸正好等于渲染分辨率 —— 最强的信号，主场景深度必然如此
	//   2. 本帧被绑定过 —— 没用到的不算
	//   3. 绑定次数多 —— 主深度会被反复绑（深度预通道、主通道、后处理…）
	// 正方形且边长是 2 的幂的基本是影子贴图，明确降权。
	int best = -1;
	int64_t bestScore = -1;
	for (uint32_t i = 0; i < count; ++i) {
		const Entry& entry = _entries[i];
		if (!entry.bindsLastFrame && !entry.clearsLastFrame) continue;

		// **渲染分辨率已知时，宽度对不上的深度直接跳过。**
		// 换链 / 分辨率切换的过渡期（鬼武者实测：启动时 1080p↔4K 来回切，4K 深度
		// 被绑而 1080p 还没被绑），选错尺寸的深度会让 NR 用错尺寸提交 → ACCESS_LOST。
		// 宽度匹配才可能是主深度（高度可因 letterbox 之类不同）；宽度都不匹配的
		// 一定不是当前渲染分辨率的主深度。宁可没有深度（降级零矢量），也不选错。
		if (renderWidth && entry.width != renderWidth) continue;

		int64_t score = 0;
		if (renderWidth && renderHeight &&
			entry.width == renderWidth && entry.height == renderHeight) {
			score += 1000000;
		} else if (renderWidth && entry.width == renderWidth) {
			score += 200000;   // 宽度对得上，高度可能因为 letterbox 之类不同
		}
		const bool squarePowerOfTwo = entry.width == entry.height &&
			entry.width && (entry.width & (entry.width - 1)) == 0;
		if (squarePowerOfTwo) score -= 500000;   // 影子贴图的典型特征

		score += int64_t(entry.bindsLastFrame) * 10;
		score += int64_t(entry.clearsLastFrame);
		// 平局时**优先保持上一帧选中的那个**。
		// 实测（鬼武者/RE Engine）：它每帧都用新分配的深度缓冲，于是表里同时存在
		// 几十个尺寸/绑定次数完全一样的候选，评分打平，选中的那个每帧乱跳。
		// 加一点滞后既能在真有更好候选时让位（尺寸匹配是 100 万量级），
		// 又能避免纯平局下的抖动。
		if (int(i) == _selectedIndex) score += 5000;

		if (score > bestScore) {
			bestScore = score;
			best = int(i);
		}
	}

	if (best < 0) {
		_selected = nullptr;
		_selectedIndex = -1;
		return;
	}
	// **只在选中的「尺寸/格式」变化时才打日志，不看下标。**
	// 实测（鬼武者/RE Engine）它每帧都换一块新分配的深度缓冲，所以下标每帧都变；
	// 按下标打日志会每秒刷几十行，一次会话 360KB 日志里绝大部分是这一句，把真正
	// 有用的信息全冲掉。有意义的不变量是尺寸 —— 它一直稳定在渲染分辨率上。
	if (_entries[best].width != _selectedWidth ||
		_entries[best].height != _selectedHeight ||
		_entries[best].format != _selectedFormat) {
		D5_LOG_INFO(L"主深度 -> %ux%u fmt=%u（候选 #%d，绑定 %u 次 清除 %u 次）",
			_entries[best].width, _entries[best].height,
			(unsigned)_entries[best].format, best, _entries[best].bindsLastFrame,
			_entries[best].clearsLastFrame);
	}
	_selected = _entries[best].resource;
	_selectedWidth = _entries[best].width;
	_selectedHeight = _entries[best].height;
	_selectedFormat = _entries[best].format;
	_selectedIndex = best;
}

bool DepthTracker::SelectedState(D3D12_RESOURCE_STATES& state) const noexcept {
	if (_selectedIndex < 0) return false;
	const Entry& entry = _entries[_selectedIndex];
	if (!entry.stateKnown.load(std::memory_order_relaxed)) return false;
	state = static_cast<D3D12_RESOURCE_STATES>(
		entry.state.load(std::memory_order_relaxed));
	return true;
}

uint32_t DepthTracker::Snapshot(
	DepthCandidate* out, uint32_t capacity) const noexcept {
	if (!out || !capacity) return 0;
	const uint32_t count = _count.load(std::memory_order_acquire);
	uint32_t written = 0;
	for (uint32_t i = 0; i < count && written < capacity; ++i) {
		const Entry& entry = _entries[i];
		// **跳过空槽。** Invalidate() 故意不把 _count 归零（归零会和正在按 _count
		// 读表的 hook 线程赛跑），所以作废之后表里留着一批 resource == nullptr 的
		// 槽位等着被复用。上一版原样抄下去，日志里就出现三十行
		// "深度候选 #N [选中] 0x0 fmt=0" —— 而且每一行都标着 [选中]，
		// 因为 nullptr == Selected() 也成立。
		if (!entry.resource) continue;
		out[written].resource = entry.resource;
		out[written].width = entry.width;
		out[written].height = entry.height;
		out[written].format = entry.format;
		out[written].bindsLastFrame = entry.bindsLastFrame;
		out[written].clearsLastFrame = entry.clearsLastFrame;
		out[written].state = static_cast<D3D12_RESOURCE_STATES>(
			entry.state.load(std::memory_order_relaxed));
		out[written].stateKnown = entry.stateKnown.load(std::memory_order_relaxed);
		++written;
	}
	return written;
}

}  // namespace DXL
