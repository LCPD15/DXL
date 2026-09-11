#include "FreezeWatchdog.h"

#include <d3d12.h>
#include <tlhelp32.h>

#include "HookTeardown.h"
#include "../common/Log.h"

namespace DXL {

namespace {

int64_t Qpc() noexcept {
	LARGE_INTEGER value{};
	QueryPerformanceCounter(&value);
	return value.QuadPart;
}

// DRED 里的操作码 -> 人能读的名字。
//
// **这张表是整件事的意义所在。** 设备挂了之后 GetDeviceRemovedReason 只会给出
// 0x887A0006（"命令有问题"），谁的命令、哪一条，一个字都不说 —— 前三次 GPU 级
// 崩溃我都只能靠"改一处试一次"去猜。DRED 的面包屑记录了每条命令列表**实际执行到
// 哪一条命令**，配上资源名就能直接点名。
const wchar_t* BreadcrumbOpName(D3D12_AUTO_BREADCRUMB_OP op) noexcept {
	switch (op) {
	case D3D12_AUTO_BREADCRUMB_OP_SETMARKER: return L"SetMarker";
	case D3D12_AUTO_BREADCRUMB_OP_BEGINEVENT: return L"BeginEvent";
	case D3D12_AUTO_BREADCRUMB_OP_ENDEVENT: return L"EndEvent";
	case D3D12_AUTO_BREADCRUMB_OP_DRAWINSTANCED: return L"DrawInstanced";
	case D3D12_AUTO_BREADCRUMB_OP_DRAWINDEXEDINSTANCED:
		return L"DrawIndexedInstanced";
	case D3D12_AUTO_BREADCRUMB_OP_EXECUTEINDIRECT: return L"ExecuteIndirect";
	case D3D12_AUTO_BREADCRUMB_OP_DISPATCH: return L"Dispatch";
	case D3D12_AUTO_BREADCRUMB_OP_COPYBUFFERREGION: return L"CopyBufferRegion";
	case D3D12_AUTO_BREADCRUMB_OP_COPYTEXTUREREGION: return L"CopyTextureRegion";
	case D3D12_AUTO_BREADCRUMB_OP_COPYRESOURCE: return L"CopyResource";
	case D3D12_AUTO_BREADCRUMB_OP_COPYTILES: return L"CopyTiles";
	case D3D12_AUTO_BREADCRUMB_OP_RESOLVESUBRESOURCE: return L"ResolveSubresource";
	case D3D12_AUTO_BREADCRUMB_OP_CLEARRENDERTARGETVIEW:
		return L"ClearRenderTargetView";
	case D3D12_AUTO_BREADCRUMB_OP_CLEARUNORDEREDACCESSVIEW:
		return L"ClearUnorderedAccessView";
	case D3D12_AUTO_BREADCRUMB_OP_CLEARDEPTHSTENCILVIEW:
		return L"ClearDepthStencilView";
	case D3D12_AUTO_BREADCRUMB_OP_RESOURCEBARRIER: return L"ResourceBarrier";
	case D3D12_AUTO_BREADCRUMB_OP_EXECUTEBUNDLE: return L"ExecuteBundle";
	case D3D12_AUTO_BREADCRUMB_OP_PRESENT: return L"Present";
	case D3D12_AUTO_BREADCRUMB_OP_RESOLVEQUERYDATA: return L"ResolveQueryData";
	case D3D12_AUTO_BREADCRUMB_OP_BEGINSUBMISSION: return L"BeginSubmission";
	case D3D12_AUTO_BREADCRUMB_OP_ENDSUBMISSION: return L"EndSubmission";
	case D3D12_AUTO_BREADCRUMB_OP_DECODEFRAME: return L"DecodeFrame";
	case D3D12_AUTO_BREADCRUMB_OP_PROCESSFRAMES: return L"ProcessFrames";
	case D3D12_AUTO_BREADCRUMB_OP_DISPATCHRAYS: return L"DispatchRays";
	case D3D12_AUTO_BREADCRUMB_OP_BUILDRAYTRACINGACCELERATIONSTRUCTURE:
		return L"BuildRTAS";
	case D3D12_AUTO_BREADCRUMB_OP_INITIALIZEEXTENSIONCOMMAND:
		return L"InitExtensionCommand";
	case D3D12_AUTO_BREADCRUMB_OP_EXECUTEEXTENSIONCOMMAND:
		return L"ExecExtensionCommand";
	case D3D12_AUTO_BREADCRUMB_OP_SETPIPELINESTATE1: return L"SetPipelineState1";
	case D3D12_AUTO_BREADCRUMB_OP_DISPATCHMESH: return L"DispatchMesh";
	case D3D12_AUTO_BREADCRUMB_OP_BARRIER: return L"Barrier(增强)";
	// **这一条是上次真崩溃时打成 "(其它)" 的那个。** 每条列表的第 0 条都是它，
	// 也就是"列表开始执行"。看到它停在 [0] 的意思是：这条列表**根本没开始跑**，
	// 它是排在真凶后面被连坐的，不是凶手本身。当时那三行"卡住的列表"因此全是误导。
	case D3D12_AUTO_BREADCRUMB_OP_BEGIN_COMMAND_LIST:
		return L"BeginCommandList（列表开始）";
	case D3D12_AUTO_BREADCRUMB_OP_DISPATCHGRAPH: return L"DispatchGraph";
	case D3D12_AUTO_BREADCRUMB_OP_SETPROGRAM: return L"SetProgram";
	case D3D12_AUTO_BREADCRUMB_OP_ESTIMATEMOTION: return L"EstimateMotion";
	case D3D12_AUTO_BREADCRUMB_OP_RESOLVEMOTIONVECTORHEAP:
		return L"ResolveMotionVectorHeap";
	case D3D12_AUTO_BREADCRUMB_OP_EXECUTEMETACOMMAND: return L"ExecuteMetaCommand";
	default: return nullptr;   // 调用方负责把数字打出来 —— "(其它)" 等于没说
	}
}

const wchar_t* AllocationTypeName(D3D12_DRED_ALLOCATION_TYPE type) noexcept {
	switch (type) {
	case D3D12_DRED_ALLOCATION_TYPE_COMMAND_QUEUE: return L"命令队列";
	case D3D12_DRED_ALLOCATION_TYPE_COMMAND_ALLOCATOR: return L"命令分配器";
	case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_STATE: return L"PSO";
	case D3D12_DRED_ALLOCATION_TYPE_COMMAND_LIST: return L"命令列表";
	case D3D12_DRED_ALLOCATION_TYPE_FENCE: return L"围栏";
	case D3D12_DRED_ALLOCATION_TYPE_DESCRIPTOR_HEAP: return L"描述符堆";
	case D3D12_DRED_ALLOCATION_TYPE_HEAP: return L"堆";
	case D3D12_DRED_ALLOCATION_TYPE_QUERY_HEAP: return L"查询堆";
	case D3D12_DRED_ALLOCATION_TYPE_COMMAND_SIGNATURE: return L"命令签名";
	case D3D12_DRED_ALLOCATION_TYPE_PIPELINE_LIBRARY: return L"管线库";
	case D3D12_DRED_ALLOCATION_TYPE_RESOURCE: return L"资源";
	case D3D12_DRED_ALLOCATION_TYPE_PASS: return L"Pass";
	default: return L"(其它)";
	}
}

// 把一个代码地址解析成 "模块名+偏移"。
//
// 刻意不用 dbghelp/符号：我们要区分的是"停在谁的代码里"（我们 / nvngx_dlssnr /
// sl.interposer / 游戏 / d3d12 / 驱动），模块名就足够回答，而引入 dbghelp 会给
// 被注入的进程增加依赖和一把全局锁。
void DescribeAddress(void* address, wchar_t* out, size_t capacity) noexcept {
	out[0] = L'\0';
	if (!address) {
		_snwprintf_s(out, capacity, _TRUNCATE, L"(null)");
		return;
	}
	MEMORY_BASIC_INFORMATION info{};
	if (!VirtualQuery(address, &info, sizeof(info)) || !info.AllocationBase) {
		_snwprintf_s(out, capacity, _TRUNCATE, L"%p (未映射)", address);
		return;
	}
	wchar_t path[MAX_PATH]{};
	const HMODULE module = reinterpret_cast<HMODULE>(info.AllocationBase);
	if (GetModuleFileNameW(module, path, MAX_PATH)) {
		const wchar_t* leaf = wcsrchr(path, L'\\');
		_snwprintf_s(out, capacity, _TRUNCATE, L"%s+0x%llX", leaf ? leaf + 1 : path,
			(unsigned long long)(reinterpret_cast<uint8_t*>(address) -
				reinterpret_cast<uint8_t*>(info.AllocationBase)));
	} else {
		_snwprintf_s(out, capacity, _TRUNCATE, L"%p (匿名 base=%p)", address,
			info.AllocationBase);
	}
}

}  // namespace

const wchar_t* StageName(Stage stage) noexcept {
	switch (stage) {
	case Stage::None:             return L"—";
	case Stage::PresentEnter:     return L"进入 Present";
	case Stage::ReloadSettings:   return L"重读设置";
	case Stage::DepthInstall:     return L"安装深度探测";
	case Stage::FiltersEnter:     return L"进入滤镜链";
	case Stage::NgxCoreInit:      return L"NGX core 初始化";
	case Stage::UpscalerInit:     return L"SR 初始化";
	case Stage::NrInit:           return L"NR 初始化";
	case Stage::GetBackBuffer:    return L"取 backbuffer";
	case Stage::NrPrepare:        return L"NR Prepare";
	case Stage::NrClaimSlot:      return L"NR 取命令槽（等 fence）";
	case Stage::NrRecord:         return L"NR 录命令";
	case Stage::NrEvaluate:       return L"NR EvaluateFeature";
	case Stage::NrSubmit:         return L"NR 提交";
	case Stage::SrPrepare:        return L"SR Prepare";
	case Stage::SrClaimSlot:      return L"SR 取命令槽（等 fence）";
	case Stage::SrRecord:         return L"SR 录命令";
	case Stage::SrEvaluate:       return L"SR EvaluateFeature";
	case Stage::SrSubmit:         return L"SR 提交";
	case Stage::FiltersExit:      return L"离开滤镜链";
	case Stage::DepthEndFrame:    return L"深度 EndFrame";
	case Stage::PresentOriginal:  return L"游戏原本的 Present";
	case Stage::PresentExit:      return L"离开 Present";
	case Stage::NrEvalEnter:       return L"@eval 进入";
	case Stage::NrEvalEncode:     return L"@eval 编码";
	case Stage::NrEvalNgx:        return L"@eval DLSSNR";
	case Stage::NrEvalDecode:     return L"@eval 还原";
	case Stage::NrEvalWriteBack:  return L"@eval 写回";
	case Stage::NrEvalExit:       return L"@eval 离开";
	case Stage::SwapChainCreated: return L"swapchain 创建";
	case Stage::ResizeBuffers:    return L"ResizeBuffers";
	case Stage::SettingsReload:   return L"命令线程改设置";
	case Stage::CommandThread:    return L"命令线程";
	}
	return L"?";
}

FreezeWatchdog& FreezeWatchdog::Get() noexcept {
	static FreezeWatchdog instance;
	return instance;
}

FreezeWatchdog::FreezeWatchdog() noexcept {
	LARGE_INTEGER frequency{};
	QueryPerformanceFrequency(&frequency);
	_qpcFreq = frequency.QuadPart ? frequency.QuadPart : 1;
}

FreezeWatchdog::~FreezeWatchdog() {
	Stop();
}

void FreezeWatchdog::Mark(Stage stage, uint64_t detail) noexcept {
	_frames.Push(stage, detail);
}

void FreezeWatchdog::MarkEvent(Stage stage, uint64_t detail) noexcept {
	// 稀有事件同时进两个环：事件环用来长期保留，帧环用来看它插在哪一帧中间
	_events.Push(stage, detail);
	_frames.Push(stage, detail);
}

void FreezeWatchdog::PresentEnter() noexcept {
	_presentThreadId.store(GetCurrentThreadId(), std::memory_order_relaxed);
	_presentEnterQpc.store(Qpc(), std::memory_order_relaxed);
	_inPresent.store(true, std::memory_order_relaxed);
	Mark(Stage::PresentEnter);
}

void FreezeWatchdog::PresentExit() noexcept {
	Mark(Stage::PresentExit);
	_inPresent.store(false, std::memory_order_relaxed);
	// 放在最后：看门狗用它判断"有没有前进"，必须在面包屑写完之后才自增
	_presentSeq.fetch_add(1, std::memory_order_release);
}

void FreezeWatchdog::Start(uint32_t stallMs) noexcept {
	if (_thread || !stallMs) return;
	_stallMs = stallMs;
	_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!_stopEvent) return;
	_thread = CreateThread(nullptr, 0, &ThreadMain, this, 0, &_watchdogThreadId);
	if (!_thread) {
		CloseHandle(_stopEvent);
		_stopEvent = nullptr;
		return;
	}
	D5_LOG_INFO(L"卡顿看门狗已启动（阈值 %u ms）", stallMs);
}

void FreezeWatchdog::Stop() noexcept {
	if (_stopEvent) SetEvent(_stopEvent);
	if (_thread) {
		WaitForSingleObject(_thread, 2000);
		CloseHandle(_thread);
		_thread = nullptr;
	}
	if (_stopEvent) {
		CloseHandle(_stopEvent);
		_stopEvent = nullptr;
	}
}

DWORD WINAPI FreezeWatchdog::ThreadMain(void* self) noexcept {
	static_cast<FreezeWatchdog*>(self)->Run();
	return 0;
}

void FreezeWatchdog::Run() noexcept {
	uint64_t lastSeq = _presentSeq.load(std::memory_order_acquire);
	int64_t lastProgress = Qpc();
	bool dumped = false;

	for (;;) {
		if (WaitForSingleObject(_stopEvent, 200) == WAIT_OBJECT_0) return;

		// **设备丢失要单独查，不能只等 present 卡住。**
		//
		// 这两件事只是**经常**一起出现，不是必然：设备挂了之后游戏完全可能继续
		// 调 Present（每次都失败），那样 present 序号一直在涨，卡顿判定永远不触发，
		// 于是一次 GPU 级故障可以全程不留任何日志。实测夹具里就是这样 ——
		// 主动 RemoveDevice 之后一行都没打出来。
		// 而 DRED 的数据在设备移除之后就有效了，**早读早留证据**。
		if (_device && !_deviceLossReported) {
			const HRESULT removed = _device->GetDeviceRemovedReason();
			if (removed != S_OK) {
				_deviceLossReported = true;
				D5_LOG_ERROR(L"================ 设备丢失 ================");
				D5_LOG_ERROR(L"GetDeviceRemovedReason = 0x%08X（%s）—— "
					L"GPU 侧出事了，画面从这一刻起不会再更新", removed,
					removed == DXGI_ERROR_DEVICE_HUNG
						? L"DEVICE_HUNG：提交的命令有问题（我们的或游戏的）"
					: removed == DXGI_ERROR_DEVICE_RESET
						? L"DEVICE_RESET：驱动 TDR 复位"
					: removed == DXGI_ERROR_DEVICE_REMOVED
						? L"DEVICE_REMOVED：设备被移除/驱动更新"
					: removed == DXGI_ERROR_DRIVER_INTERNAL_ERROR
						? L"DRIVER_INTERNAL_ERROR：驱动内部错误"
						: L"其它");
				DumpDred();
				DumpRing(_frames.slots, 96,
					_frames.next.load(std::memory_order_relaxed),
					L"设备丢失前最近几帧的每一步");
				D5_LOG_ERROR(L"============== 设备丢失转储结束 ==============");
			}
		}

		const uint64_t seq = _presentSeq.load(std::memory_order_acquire);
		const int64_t now = Qpc();

		// 一帧都还没 present 过就无从谈"卡住"。早注入时我们比游戏创建 swapchain
		// 早好几秒，这里不挡住的话每次早注入都会误报一次 —— 实测就是这样。
		if (!seq) {
			lastProgress = now;
			continue;
		}

		if (seq != lastSeq) {
			lastSeq = seq;
			lastProgress = now;
			if (dumped) {
				D5_LOG_INFO(L"present 恢复了（累计卡住 %u 次）",
					_stallCount.load(std::memory_order_relaxed));
			}
			dumped = false;
			continue;
		}

		const uint32_t stalledMs =
			uint32_t((now - lastProgress) * 1000 / _qpcFreq);
		if (stalledMs < _stallMs || dumped) continue;

        // Destroying a rendering HWND is also normal during a DLSS/API/display
        // switch. A missing window is not evidence that the process is exiting.
        // Keep hooks alive for its replacement; real NGX shutdown / process
        // detach still request the normal teardown.
        const HWND window = _window.load(std::memory_order_relaxed);
        if (window && !IsWindow(window)) {
            dumped = true;
            D5_LOG_INFO(L"Present idle %u ms: tracked window was destroyed; awaiting replacement, hooks retained", stalledMs);
            continue;
        }

		// 一次卡顿只转储一次，否则日志会被刷爆而且看不出第一现场
		dumped = true;
		_stallCount.fetch_add(1, std::memory_order_relaxed);
		Dump(stalledMs, seq);
	}
}

void FreezeWatchdog::Dump(uint32_t stalledMs, uint64_t presentSeq) noexcept {
	const bool inPresent = _inPresent.load(std::memory_order_relaxed);
	// 最近一颗面包屑就是"控制权停在哪"
	const uint64_t next = _frames.next.load(std::memory_order_relaxed);
	const Stage lastStage =
		next ? _frames.slots[(next - 1) % 96].stage : Stage::None;
	_lastStallStage.store(lastStage, std::memory_order_relaxed);

	D5_LOG_ERROR(L"================ 卡顿转储 ================");
	D5_LOG_ERROR(L"present 已经 %u ms 没有前进（present 序号停在 %llu，"
		L"present 线程 %lu）", stalledMs, (unsigned long long)presentSeq,
		_presentThreadId.load(std::memory_order_relaxed));

	// 这是整个工具里最重要的一行判断：到底是谁卡住的
	if (!inPresent) {
		D5_LOG_ERROR(L"CPU 状态：已从 Present 返回（最后一步：%s）。"
			L"这不代表 GPU 已完成 NR；GPU 根因需结合 DRED 与校验信息判断。", StageName(lastStage));
	} else if (lastStage == Stage::PresentOriginal) {
		D5_LOG_ERROR(L"CPU 状态：停在游戏原本的 Present。NR 命令提交不等于 GPU 执行完成；"
			L"仍需排查 NR、游戏和驱动的 GPU 工作。");
	} else {
		D5_LOG_ERROR(L"CPU 状态：最后记录到 NR/Core 阶段 %s；"
			L"此标记用于定位调用进度，不能单独确认 GPU 根因。", StageName(lastStage));
	}

	if (_device) {
		const HRESULT removed = _device->GetDeviceRemovedReason();
		if (removed == S_OK) {
			D5_LOG_ERROR(L"GetDeviceRemovedReason = S_OK（设备还在）");
		} else {
			D5_LOG_ERROR(L"GetDeviceRemovedReason = 0x%08X —— 设备已经丢了，"
				L"这就是画面定住的直接原因", removed);
			DumpDred();
		}
	}

	// 反复重建 swapchain 是个有明确含义的模式，直接替读日志的人把话说出来。
	// 实测踩过：判定行说"不是我们"（控制权确实在游戏的重试循环里），但根因是我们
	// AddRef 持住了 swapchain，害得它永远建不出新链。光看判定行会走错方向。
	{
		const uint64_t eventNext = _events.next.load(std::memory_order_relaxed);
		const uint64_t total = eventNext < 64 ? eventNext : 64;
		uint32_t recreates = 0;
		for (uint64_t i = 0; i < total; ++i) {
			if (_events.slots[(eventNext - total + i) % 64].stage ==
				Stage::SwapChainCreated) {
				++recreates;
			}
		}
		if (recreates >= 8) {
			D5_LOG_ERROR(L"注意：最近的事件里有 %u 次 swapchain 创建 —— 游戏在**反复"
				L"重建交换链**。这几乎总是因为有人还持着旧 swapchain 的引用（DXGI "
				L"不允许一个 HWND 上有两个 swapchain），游戏于是一直失败一直重试。"
				L"**先怀疑我们自己有没有 AddRef 过 swapchain。**", recreates);
		}
	}

	// 事件环先打：排查"改设置就卡"时，最先要看的就是那一刻游戏做了什么
	DumpRing(_events.slots, 64, _events.next.load(std::memory_order_relaxed),
		L"本次会话的稀有事件（重建 swapchain / ResizeBuffers / 改设置）");
	DumpRing(_frames.slots, 96, next, L"最近几帧的每一步");
	DumpThreads();
	D5_LOG_ERROR(L"============== 卡顿转储结束 ==============");
}

void FreezeWatchdog::DumpRing(const Crumb* slots, uint32_t capacity,
	uint64_t next, const wchar_t* title) noexcept {
	if (!next) {
		D5_LOG_ERROR(L"%s：（空）", title);
		return;
	}
	const uint64_t total = next < capacity ? next : capacity;
	const int64_t nowQpc = Qpc();
	D5_LOG_ERROR(L"%s —— %llu 条（距今毫秒 / 阶段 / 线程 / 附加值）：",
		title, (unsigned long long)total);
	for (uint64_t i = 0; i < total; ++i) {
		const Crumb& crumb = slots[(next - total + i) % capacity];
		const double agoMs =
			double(nowQpc - crumb.qpc) * 1000.0 / double(_qpcFreq);
		D5_LOG_ERROR(L"  -%9.1f ms  %-26s  tid=%-6lu  %llu",
			agoMs, StageName(crumb.stage), crumb.threadId,
			(unsigned long long)crumb.detail);
	}
}

// **设备挂了之后唯一能点名的东西。**
//
// GetDeviceRemovedReason 只给一个 0x887A0006（"命令有问题"）—— 谁的命令、
// 哪一条，一个字都不说。这个项目已经因此有过三次"改一处、跑一次、再猜"的循环。
// DRED 记录的是每条命令列表**实际执行到第几条命令**，以及页错误的虚拟地址落在
// 哪个（可能已经释放的）资源上。资源都 SetName 过的话，这份输出直接指出凶手。
//
// 前提：进程启动早期（游戏建设备之前）调过 EnableDred()。没调过的话这里
// 拿不到接口，会明确说"没开"而不是静默什么都不打 —— 一个不说话的诊断等于没有。
void FreezeWatchdog::DumpDred() noexcept {
	if (!_device) return;
	ID3D12DeviceRemovedExtendedData1* dred = nullptr;
	if (FAILED(_device->QueryInterface(
			__uuidof(ID3D12DeviceRemovedExtendedData1),
			reinterpret_cast<void**>(&dred))) || !dred) {
		D5_LOG_ERROR(L"DRED：拿不到接口 —— 这台机器的 D3D12 太老，"
			L"或者注入太晚（必须在游戏建设备之前开）。");
		return;
	}

	D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 crumbs{};
	if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput1(&crumbs))) {
		// **先数一遍，再挑着打。**
		//
		// 上一版把遍历截在 16 条列表，而真实游戏一次转储正好有 16 条以上 ——
		// 于是游戏的**主渲染列表**（几百条命令、装着我们插进去的 dispatch 的那条）
		// 一次都没被打印过，而我据此得出了"没有任何列表执行到一半"的结论。
		// **诊断的覆盖范围本身必须先被检查**：截断的输出和完整的输出长得一样。
		uint32_t total = 0;
		uint32_t partial = 0;
		uint32_t notStarted = 0;
		uint32_t finished = 0;
		for (const D3D12_AUTO_BREADCRUMB_NODE1* node = crumbs.pHeadAutoBreadcrumbNode;
			node && total < 512; node = node->pNext, ++total) {
			const uint32_t done = node->pLastBreadcrumbValue
				? *node->pLastBreadcrumbValue : 0;
			if (!node->BreadcrumbCount || done >= node->BreadcrumbCount) ++finished;
			else if (done == 0) ++notStarted;
			else ++partial;
		}
		D5_LOG_ERROR(L"DRED 面包屑：共 %u 条命令列表 —— 跑完 %u 条，"
			L"**执行到一半 %u 条**，一条没开始 %u 条。"
			L"%s", total, finished, partial, notStarted,
			partial ? L"下面逐条打「执行到一半」的那些：**真凶在里面**"
				: L"没有任何列表卡在中途 —— 那么问题不在某条命令上，"
				  L"而在队列层面（GPU 侧的等待/超时）");

		// **只打执行到一半的那些**（真凶），加上前几条完整的做参照。
		// 全打会有几百条，把关键信息埋掉；只打摘要又会漏掉真凶 —— 这是折中。
		uint32_t listIndex = 0;
		uint32_t printed = 0;
		for (const D3D12_AUTO_BREADCRUMB_NODE1* node = crumbs.pHeadAutoBreadcrumbNode;
			node && listIndex < 512; node = node->pNext, ++listIndex) {
			const uint32_t doneNow = node->pLastBreadcrumbValue
				? *node->pLastBreadcrumbValue : 0;
			const bool isPartial = node->BreadcrumbCount &&
				doneNow > 0 && doneNow < node->BreadcrumbCount;
			// 有卡在中途的就只打那些；一条都没有时才退回打前 16 条看看形状。
			if (partial ? !isPartial : (printed >= 16)) continue;
			++printed;
			const uint32_t done = node->pLastBreadcrumbValue
				? *node->pLastBreadcrumbValue : 0;
			D5_LOG_ERROR(L"DRED 面包屑 #%u：列表「%s」队列「%s」—— "
				L"录了 %u 条命令，**执行完了 %u 条**%s",
				listIndex,
				node->pCommandListDebugNameW ? node->pCommandListDebugNameW
					: L"(无名)",
				node->pCommandQueueDebugNameW ? node->pCommandQueueDebugNameW
					: L"(无名)",
				node->BreadcrumbCount, done,
				done == 0 && node->BreadcrumbCount
					? L" ← 一条都没执行：**排在真凶后面被连坐的**，不是它"
				: done < node->BreadcrumbCount
					? L" ← **执行到一半停住了，凶手就在这条里**" : L"");
			if (done >= node->BreadcrumbCount || !node->pCommandHistory) continue;
			// Preserve any game/NGX PIX labels; an opcode alone cannot identify its owner.
			for (UINT c = 0; node->pBreadcrumbContexts && c < node->BreadcrumbContextsCount; ++c) {
				const auto& context = node->pBreadcrumbContexts[c];
				D5_LOG_ERROR(L"    GPU context [%u]: %s", context.BreadcrumbIndex,
					context.pContextString ? context.pContextString : L"(empty)");
			}
			// 只打卡住点前后几条 —— 一条列表可能有上千条命令，全打没人看得完，
			// 而有用的信息只在"最后执行完的那条"和"下一条"之间。
			// 短列表整条打出来。上次只打了 done±几条，结果看到的是
			// "[0] (其它) / [1] Dispatch"，既不知道 [0] 是什么，也不知道
			// 这条列表总共想干什么 —— 信息量刚好差在够不够下结论上。
			const uint32_t from = node->BreadcrumbCount <= 48
				? 0 : (done > 6 ? done - 6 : 0);
			const uint32_t to = node->BreadcrumbCount <= 48
				? node->BreadcrumbCount
				: (done + 4 < node->BreadcrumbCount
					? done + 4 : node->BreadcrumbCount);
			for (uint32_t i = from; i < to; ++i) {
				const D3D12_AUTO_BREADCRUMB_OP op = node->pCommandHistory[i];
				const wchar_t* name = BreadcrumbOpName(op);
				if (name) {
					D5_LOG_ERROR(L"    [%u] %s%s", i, name,
						i == done ? L"  ← **下一条就是它，没执行完**" : L"");
				} else {
					D5_LOG_ERROR(L"    [%u] op=%d（这个操作码还没映射）%s", i, int(op),
						i == done ? L"  ← **下一条就是它，没执行完**" : L"");
				}
			}
		}
		if (!crumbs.pHeadAutoBreadcrumbNode) {
			D5_LOG_ERROR(_dredEnabled
				? L"DRED：面包屑是空的，但 DRED 是开着的 —— "
					L"**这次不是命令执行出错导致的移除**"
					L"（主动 RemoveDevice、驱动内部错误、外部复位都是这种）。"
				: L"DRED：面包屑是空的，而且 DRED **没开成**（注入太晚）—— "
					L"这份转储说不了什么，先解决注入时机。");
		}
	}

	D3D12_DRED_PAGE_FAULT_OUTPUT1 fault{};
	if (SUCCEEDED(dred->GetPageFaultAllocationOutput1(&fault))) {
		if (!fault.PageFaultVA) {
			D5_LOG_ERROR(L"DRED：没有记录到页错误地址。不能据此排除无效描述符或越界；"
				L"需结合命令、资源生命周期和 D3D12 校验继续定位。");
		} else {
			D5_LOG_ERROR(L"DRED 页错误地址：0x%llX —— "
				L"**有人访问了不该访问的显存**",
				(unsigned long long)fault.PageFaultVA);
			auto dumpNodes = [](const D3D12_DRED_ALLOCATION_NODE1* head,
				const wchar_t* what) {
				uint32_t n = 0;
				for (const D3D12_DRED_ALLOCATION_NODE1* node = head;
					node && n < 12; node = node->pNext, ++n) {
					D5_LOG_ERROR(L"    %s：%s（%s）", what,
						node->ObjectNameW ? node->ObjectNameW : L"(无名)",
						AllocationTypeName(node->AllocationType));
				}
				if (!head) D5_LOG_ERROR(L"    %s：（无）", what);
			};
			// **已释放的那一列是最有价值的**：命令列表引用了一个已经被销毁的资源，
			// 是这类崩溃里最常见的一种，而且用别的手段几乎查不出来。
			dumpNodes(fault.pHeadExistingAllocationNode, L"这个地址上还活着的对象");
			dumpNodes(fault.pHeadRecentFreedAllocationNode,
				L"**最近刚释放**的对象");
		}
	}
	dred->Release();
}

void FreezeWatchdog::DumpThreads() noexcept {
	HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);	if (snapshot == INVALID_HANDLE_VALUE) return;

	struct Sample {
		DWORD threadId;
		void* rip;
		DWORD suspendCount;
	};
	Sample samples[128]{};
	uint32_t sampleCount = 0;

	const DWORD selfPid = GetCurrentProcessId();
	const DWORD selfTid = GetCurrentThreadId();

	// 先只做"挂起 → 取 RIP → 立刻恢复"，**不在挂起期间做任何可能取 loader 锁的事**
	// （GetModuleFileNameW 就会）。被挂起的线程如果正好持有 loader 锁，我们再去取
	// 就是自己造一个死锁。所以解析地址留到全部恢复之后。
	THREADENTRY32 entry{};
	entry.dwSize = sizeof(entry);
	if (Thread32First(snapshot, &entry)) {
		do {
			if (entry.th32OwnerProcessID != selfPid) continue;
			if (entry.th32ThreadID == selfTid) continue;   // 看门狗自己
			if (sampleCount >= 128) break;

			HANDLE thread = OpenThread(
				THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE,
				entry.th32ThreadID);
			if (!thread) continue;

			const DWORD suspendCount = SuspendThread(thread);
			if (suspendCount != DWORD(-1)) {
				CONTEXT context{};
				context.ContextFlags = CONTEXT_CONTROL;
				if (GetThreadContext(thread, &context)) {
					samples[sampleCount].threadId = entry.th32ThreadID;
					samples[sampleCount].rip = reinterpret_cast<void*>(context.Rip);
					samples[sampleCount].suspendCount = suspendCount;
					++sampleCount;
				}
				ResumeThread(thread);
			}
			CloseHandle(thread);
		} while (Thread32Next(snapshot, &entry));
	}
	CloseHandle(snapshot);

	const DWORD presentTid = _presentThreadId.load(std::memory_order_relaxed);
	D5_LOG_ERROR(L"进程内 %u 个线程停在哪（RIP -> 模块+偏移）：", sampleCount);
	for (uint32_t i = 0; i < sampleCount; ++i) {
		wchar_t where[320]{};
		DescribeAddress(samples[i].rip, where, 320);
		D5_LOG_ERROR(L"  tid=%-6lu %s%s", samples[i].threadId, where,
			samples[i].threadId == presentTid ? L"   <<< present 线程" : L"");
	}
}

}  // namespace DXL
