#include "NgxEavesdrop.h"
#include "NgxModuleIdentity.h"
#include "FrameGenSwapChains.h"
#include "EvaluateGpuGate.h"
#include "NrRouteProbe.h"
#include "NgxGuideFormat.h"
#include "D3D12Validation.h"
#include "GpuEventScope.h"

#include <nvsdk_ngx.h>
#include <d3d12.h>
#include <psapi.h>
#include <intrin.h>
#include <cwctype>
#include <mutex>

#include "../common/Log.h"
#include "CommandListTracker.h"
#include "HookTeardown.h"
#include "IatPatch.h"
#include "MinHook.h"

namespace DXL {

namespace {

/* ---------------- 我们要盯的东西 ---------------- */

// 会去调 NGX 的模块。链式补丁只跟这些，不给整个进程的模块都打补丁 ——
// 补得越少越安全，而且这几个已经覆盖了 Streamline 的整条链。
bool IsInterestingModule(const wchar_t* leaf) noexcept {
	if (!leaf) return false;
	auto contains = [leaf](const wchar_t* needle) {
		for (const wchar_t* s = leaf; *s; ++s) {
			const wchar_t* a = s;
			const wchar_t* b = needle;
			while (*a && *b && towlower(*a) == towlower(*b)) { ++a; ++b; }
			if (!*b) return true;
		}
		return false;
	};
	// FG 模块**不打补丁**。对 nvngx_dlssg / sl.dlss_g 的 IAT patch + CreateFeature
	// 包装被证明干扰 FG（生化9 开 FG 黑屏，diagEavesdrop=false 则正常）。而且：
	//   · FG 的 evaluate 走 _nvngx 内部指针表，不经过 GetProcAddress —— patch 了也
	//     捞不到（FrameGenMarkWrapper 从没旁听到 FG evaluate）；
	//   · FG 的 CreateFeature(FrameGeneration) 由游戏 exe 通过 GetProcAddress 调用，
	//     靠游戏 exe 的补丁检测，不依赖 FG 模块的补丁。
	// 所以对 FG 模块打补丁纯粹是"往 FG 的地盘插一脚却什么都没拿到"，反而污染它。
	if (contains(L"nvngx_dlssg") || contains(L"sl.dlss_g")) return false;
	// Unity games can statically import UnityPlayer.dll and only call UnityMain
	// from the EXE. The engine DLL, not the EXE, then loads Streamline. Patch that
	// exact engine entry before UnityMain runs; broad patching of unrelated game
	// plugins is unnecessary. FG exclusions above remain authoritative.
	return _wcsicmp(leaf, L"UnityPlayer.dll") == 0 ||
		contains(L"sl.") || contains(L"nvngx") || contains(L"dlss");
}

// 我们要包装的 NGX 函数名。只有 Evaluate 会被换掉；另外两个只是记一笔，
// 让日志能说清"游戏什么时候建了它的 DLSS feature"。
constexpr char FN_EVALUATE[] = "NVSDK_NGX_D3D12_EvaluateFeature";
constexpr char FN_CREATE[] = "NVSDK_NGX_D3D12_CreateFeature";
constexpr char FN_INIT_PROJECT[] = "NVSDK_NGX_D3D12_Init_with_ProjectID";
constexpr char FN_INIT_EXT[] = "NVSDK_NGX_D3D12_Init_Ext";
// Shutdown 也要包一层：游戏**调用**它 = 开始拆图形栈，是比窗口销毁早得多的
// 退出信号（DS2 实测：日志在正常旁听输出中戛然而止，WM_NCDESTROY 和看门狗
// 两个触发点都没来得及跑 —— 崩在窗口销毁之前）。包装里先 RequestTeardown()
// 把钩子撤下来，再转发给真函数。
constexpr char FN_SHUTDOWN[] = "NVSDK_NGX_D3D12_Shutdown";

/* ---------------- 原函数指针 ---------------- */

using LoadLibraryWFn = HMODULE(WINAPI*)(LPCWSTR);
using LoadLibraryAFn = HMODULE(WINAPI*)(LPCSTR);
using LoadLibraryExWFn = HMODULE(WINAPI*)(LPCWSTR, HANDLE, DWORD);
using LoadLibraryExAFn = HMODULE(WINAPI*)(LPCSTR, HANDLE, DWORD);
using GetProcAddressFn = FARPROC(WINAPI*)(HMODULE, LPCSTR);
using EvaluateFeatureFn = NVSDK_NGX_Result(NVSDK_CONV*)(
	ID3D12GraphicsCommandList*, const NVSDK_NGX_Handle*,
	const NVSDK_NGX_Parameter*, PFN_NVSDK_NGX_ProgressCallback);
// Shutdown 无参数：NVSDK_NGX_Result NVSDK_CONV NVSDK_NGX_D3D12_Shutdown(void)
using ShutdownFn = NVSDK_NGX_Result(NVSDK_CONV*)();

// 每个被打了补丁的模块都有自己的一份"原函数"。**不能只存一份全局的** ——
// 不同模块的 IAT 里这些函数的地址可能不同（api-set 转发、不同的 kernel32 导出桩），
// 混用会把 A 模块的调用转给 B 模块的原函数。
struct PatchedModule {
	HMODULE module = nullptr;
	LoadLibraryWFn loadLibraryW = nullptr;
	LoadLibraryAFn loadLibraryA = nullptr;
	LoadLibraryExWFn loadLibraryExW = nullptr;
	LoadLibraryExAFn loadLibraryExA = nullptr;
	GetProcAddressFn getProcAddress = nullptr;
};

constexpr uint32_t MAX_PATCHED = 32;
PatchedModule g_patched[MAX_PATCHED]{};
std::atomic<uint32_t> g_patchedCount{ 0 };
std::mutex g_patchMutex;
HMODULE g_selfModule = nullptr;

// 真的 EvaluateFeature —— **必须一个真地址对应一个包装**。
//
// 上一版只存了一个全局指针，后写覆盖前写，结果画面被撕成条带错位。原因：
// 每个 NGX snippet（nvngx_dlss / nvngx_dlssg / nvngx_dlssd）都各自导出一份
// `NVSDK_NGX_D3D12_EvaluateFeature`，Streamline 会逐个解析。实测鬼武者一次会话里
// 出现了**七个不同的真地址**（其中一个还是堆上的跳板，说明链上还有别的 hooker）。
// 全存进一个变量，游戏拿着"给 DLSS-SR 的包装"去调，就被转发进了 DLSS-G 或
// Ray Reconstruction 的 evaluate —— 换了个神经网络处理这一帧，张量布局对不上，
// 出来就是条带错位的花屏。
//
// 所以做一张小表：真地址 -> 槽位，每个槽位有自己的包装函数。
constexpr uint32_t MAX_TARGETS = 8;
std::atomic<void*> g_realEvaluate[MAX_TARGETS]{};
// The export address remains the identity after an inline hook replaces the
// forwarding address with its trampoline. IAT and cached callers share a slot.
std::atomic<void*> g_targetAddress[MAX_TARGETS]{};
std::atomic<bool> g_nativeUpscalerTarget[MAX_TARGETS]{};
std::atomic<uint32_t> g_targetCount{ 0 };
std::atomic<uint64_t> g_enteredEvaluate[MAX_TARGETS]{};
std::mutex g_targetMutex;
bool g_exportAttempted[MAX_TARGETS]{}; // guarded by g_targetMutex
HMODULE g_retainedExportModules[MAX_TARGETS]{};

// 真的 Shutdown。不需要按真地址分槽 —— 它只做一件事（先拆钩子再转发），
// 八个真地址都转发回各自的真函数也正确；但 Shutdown 通常只有一个调用方，
// 存一份 + 打日志说清来源就够。拿不到原值就原样放过（不包）。
std::mutex g_shutdownMutex;
ShutdownFn g_realShutdown = nullptr;
bool g_shutdownWrapped = false;

// 抄下来的那一帧。用互斥而不是原子：字段太多，撕裂的快照比没有更糟。
std::mutex g_frameMutex;
NgxEavesdropFrame g_frame;
std::atomic<uint64_t> g_frameSeq{ 0 };
// 旁听抓到第一帧的时刻（GetTickCount64 毫秒，0 = 还没抓到）。见 MsSinceFirstEvaluate。
std::atomic<uint64_t> g_firstFrameAtMs{ 0 };

/* ---------------- 资源状态观察 + 拷贝 ---------------- */

ID3D12Device* g_device = nullptr;

// 我们盯着状态的那几个资源。只有这几个，所以线性查找 + 原子就够，不用锁 ——
// 写它的是游戏的渲染线程（ResourceBarrier hook 里，非常热），读它的是 evaluate。
constexpr uint32_t MAX_WATCHED = 6;
struct WatchedState {
	std::atomic<ID3D12Resource*> resource{ nullptr };
	std::atomic<uint32_t> state{ 0 };
	// 最后一次 barrier 是录在哪条命令列表上的。见 NgxEavesdrop::NoteBarrier 的说明 ——
	// 它回答"我们能不能不碰游戏的列表就插进 DLSS 之前"这个问题。
	std::atomic<void*> lastList{ nullptr };
	std::atomic<bool> known{ false };
};
WatchedState g_watched[MAX_WATCHED];

// 我们自己的副本
std::mutex g_captureMutex;
ID3D12Resource* g_depthCopy = nullptr;
ID3D12Resource* g_motionCopy = nullptr;
uint32_t g_copyWidth = 0;
uint32_t g_copyHeight = 0;
uint32_t g_copyDepthWidth = 0, g_copyDepthHeight = 0;
DXGI_FORMAT g_depthCopyFormat = DXGI_FORMAT_UNKNOWN;
DXGI_FORMAT g_motionCopyFormat = DXGI_FORMAT_UNKNOWN;
std::atomic<uint64_t> g_capturedSeq{ 0 };
std::atomic<float> g_capturedMvScaleX{ 0.0f };
std::atomic<float> g_capturedMvScaleY{ 0.0f };
// 副本常驻这个状态：NR 读它时正好要这个，所以拷完就还原回来
constexpr D3D12_RESOURCE_STATES COPY_RESIDENT_STATE =
	D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

// 把一个资源加入观察名单（幂等）。
void WatchResource(ID3D12Resource* resource) noexcept {
	if (!resource) return;
	for (uint32_t i = 0; i < MAX_WATCHED; ++i) {
		if (g_watched[i].resource.load(std::memory_order_relaxed) == resource) return;
	}
	for (uint32_t i = 0; i < MAX_WATCHED; ++i) {
		ID3D12Resource* expected = nullptr;
		if (g_watched[i].resource.compare_exchange_strong(expected, resource)) return;
	}
}

// 从名单里摘掉一个资源。
//
// 只给诊断夹具用：夹具释放它的假资源之后，名单里那个指针就悬空了。ObservedState
// 只比指针不解引用，所以不会崩，但**地址可能被后来的分配复用** —— 那时我们会拿一个
// 几分钟前的状态去给一个毫不相干的资源下 barrier。这是那种"偶尔闪一下"的鬼故事。
void UnwatchResource(ID3D12Resource* resource) noexcept {
	if (!resource) return;
	for (uint32_t i = 0; i < MAX_WATCHED; ++i) {
		if (g_watched[i].resource.load(std::memory_order_relaxed) != resource) {
			continue;
		}
		g_watched[i].known.store(false, std::memory_order_release);
		g_watched[i].state.store(0, std::memory_order_relaxed);
		g_watched[i].resource.store(nullptr, std::memory_order_release);
		return;
	}
}

// 观察到的状态。没观察到就返回 false —— 调用方必须放弃这一路，不能猜。
bool ObservedState(ID3D12Resource* resource, D3D12_RESOURCE_STATES& state) noexcept {
	for (uint32_t i = 0; i < MAX_WATCHED; ++i) {
		if (g_watched[i].resource.load(std::memory_order_relaxed) != resource) continue;
		if (!g_watched[i].known.load(std::memory_order_acquire)) return false;
		state = static_cast<D3D12_RESOURCE_STATES>(
			g_watched[i].state.load(std::memory_order_relaxed));
		return true;
	}
	return false;
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
	D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) noexcept {
	if (before == after) return;
	D3D12_RESOURCE_BARRIER barrier{};
	barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
	barrier.Transition.pResource = resource;
	barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	barrier.Transition.StateBefore = before;
	barrier.Transition.StateAfter = after;
	list->ResourceBarrier(1, &barrier);
}

ID3D12Resource* CreateCopyTexture(
	uint32_t width, uint32_t height, DXGI_FORMAT format,
	const wchar_t* name) noexcept {
	if (!g_device) return nullptr;
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	heap.CreationNodeMask = 1;
	heap.VisibleNodeMask = 1;
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = width;
	desc.Height = height;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = NgxGuideFormat(format);
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	ID3D12Resource* resource = nullptr;
	const HRESULT hr = g_device->CreateCommittedResource(
		&heap, D3D12_HEAP_FLAG_NONE, &desc, COPY_RESIDENT_STATE, nullptr,
		IID_PPV_ARGS(&resource));
	if (FAILED(hr)) {
		D5_LOG_ERROR(L"旁听：建副本纹理 %s (%ux%u fmt=%u) 失败 0x%08X",
			name, width, height, (unsigned)format, hr);
		return nullptr;
	}
	resource->SetName(name);
	return resource;
}

// 在**游戏的**命令列表上把深度/矢量拷进我们的副本。
//
// 只做 barrier + CopyResource。源的状态必须是**观察到的**，没观察到就跳过那一路。
void CaptureOnGameList(
	ID3D12GraphicsCommandList* list, const NgxEavesdropFrame& frame) noexcept {
	if (!list || !g_device) return;
	if (!frame.motionVectors && !frame.depth) return;

	// 尺寸以矢量为准（深度通常同尺寸）。变了就重建副本。
	const uint32_t width = frame.motionWidth ? frame.motionWidth : frame.depthWidth;
	const uint32_t height = frame.motionHeight ? frame.motionHeight : frame.depthHeight;
	if (!width || !height) return;

	std::lock_guard<std::mutex> guard(g_captureMutex);
	const bool resized = width != g_copyWidth || height != g_copyHeight ||
		frame.depthWidth != g_copyDepthWidth || frame.depthHeight != g_copyDepthHeight ||
		DXGI_FORMAT(frame.depthFormat) != g_depthCopyFormat ||
		DXGI_FORMAT(frame.motionFormat) != g_motionCopyFormat;
	if (resized) {
		if (g_depthCopy) { g_depthCopy->Release(); g_depthCopy = nullptr; }
		if (g_motionCopy) { g_motionCopy->Release(); g_motionCopy = nullptr; }
		g_copyWidth = width;
		g_copyHeight = height;
		g_copyDepthWidth = frame.depthWidth; g_copyDepthHeight = frame.depthHeight;
		g_depthCopyFormat = DXGI_FORMAT(frame.depthFormat);
		g_motionCopyFormat = DXGI_FORMAT(frame.motionFormat);
		if (frame.depth && frame.depthWidth) {
			g_depthCopy = CreateCopyTexture(frame.depthWidth, frame.depthHeight,
				g_depthCopyFormat, L"D5Q.Eaves.Depth");
		}
		if (frame.motionVectors && frame.motionWidth) {
			g_motionCopy = CreateCopyTexture(frame.motionWidth, frame.motionHeight,
				g_motionCopyFormat, L"D5Q.Eaves.Motion");
		}
		D5_LOG_INFO(L"旁听：NGX 可读副本 深度=%p(%ux%u fmt=%u->%u) 矢量=%p(%ux%u fmt=%u->%u)",
			g_depthCopy, frame.depthWidth, frame.depthHeight, frame.depthFormat,
			(unsigned)NgxGuideFormat(g_depthCopyFormat),
			g_motionCopy, frame.motionWidth, frame.motionHeight, frame.motionFormat,
			(unsigned)NgxGuideFormat(g_motionCopyFormat));
	}

	uint32_t copied = 0;
	struct Pair { ID3D12Resource* src; ID3D12Resource* dst; const wchar_t* what; };
	const Pair pairs[]{
		{ frame.motionVectors, g_motionCopy, L"矢量" },
		{ frame.depth, g_depthCopy, L"深度" },
	};
	for (const Pair& pair : pairs) {
		if (!pair.src || !pair.dst) continue;
		D3D12_RESOURCE_STATES srcState{};
		if (!ObservedState(pair.src, srcState)) {
			// 还没从 barrier 里见过它的状态。第一帧必然如此 —— 不猜，下一帧再说。
			static uint32_t complained = 0;
			if (complained++ < 4) {
				D5_LOG_INFO(L"旁听：还没观察到%s资源的状态，这一帧不拷"
					L"（等游戏下次对它下 barrier）", pair.what);
			}
			continue;
		}
		Barrier(list, pair.src, srcState, D3D12_RESOURCE_STATE_COPY_SOURCE);
		Barrier(list, pair.dst, COPY_RESIDENT_STATE, D3D12_RESOURCE_STATE_COPY_DEST);
		list->CopyResource(pair.dst, pair.src);
		Barrier(list, pair.dst, D3D12_RESOURCE_STATE_COPY_DEST, COPY_RESIDENT_STATE);
		Barrier(list, pair.src, D3D12_RESOURCE_STATE_COPY_SOURCE, srcState);
		++copied;
	}

	if (copied == 2) {
		g_capturedMvScaleX.store(frame.mvScaleX, std::memory_order_relaxed);
		g_capturedMvScaleY.store(frame.mvScaleY, std::memory_order_relaxed);
		g_capturedSeq.store(frame.frameSeq, std::memory_order_release);

		// **第一次拷成功必须留一条日志。**
		// "看到"和"拷到"是两个不同的数，而只有"拷到 > 0"才说明真深度/真矢量已经
		// 安全地在我们自己的纹理里了 —— 这是把 DLSSNR 挪到游戏 evaluate 点的前提。
		// 上一次排查时状态块因为别的 bug 发不出来，日志里又只有"这一帧不拷"，
		// 结论只能靠推理别处的日志反推出来。别再让人推。
		static bool announced = false;
		if (!announced) {
			announced = true;
			D5_LOG_INFO(L"旁听：**第一次拷贝成功**（第 %llu 帧，拷了 %u 路）—— "
				L"真深度/真矢量已经在我们自己的纹理里（%ux%u）",
				(unsigned long long)frame.frameSeq, copied,
				g_copyWidth, g_copyHeight);
		}
	}
}

/* ---------------- 在 evaluate 点处理游戏的颜色 ---------------- */

std::atomic<NgxEavesdrop::PreEvaluateHook> g_preEvaluateHook{ nullptr };
std::atomic<uint64_t> g_preEvaluateFrames{ 0 };
std::atomic<uint64_t> g_preEvaluateStateMisses{ 0 };
// 认准一个槽位就不再换。
//
// 为什么必须锁定：同一个进程里可能有多条 evaluate 走这里 —— DLSS-SR 和 Ray
// Reconstruction 的参数集都含 color+depth+mvec，筛选条件挡不住第二条。不锁定的话
// 一帧里会把 DLSSNR 跑两遍（两套不同的输入），时域累积就废了，画面还会闪。
// -1 = 还没认。
std::atomic<int32_t> g_preEvaluateSlot{ -1 };

// **必须等游戏的 DLSS 稳定下来才能动手。**
//
// 实测出来的复现条件（用户给的，非常精确）：
//   · 进游戏**之前**就在设置里开着 DLSS  -> 读完盘就崩，游戏场景一眼都看不到
//   · 进游戏之后再在菜单里开 DLSS        -> 一切正常，debug 视图也正常
//
// 两者的区别只有一个：第一种情况下我们从**加载画面**就开始处理了。实测那 60 秒里
// 游戏 present 了 3710 帧全黑画面、重建过 swapchain、而且每一帧的 reset 都在变 ——
// 加载/过场期间引擎在重建渲染目标、重置 DLSS 历史，这是它自己最不稳定的一段。
// 我们在那段时间往它的命令列表里插 165MB 神经网络的活，是在赌它不介意。
//
// 判据用游戏自己给的 `reset` 标志：reset=1 的意思就是"历史无效，重新开始"，
// 场景切换和加载时它就是 1。连续若干帧 reset=0 且资源指针/子矩形都没变，
// 才认为它稳了。这同时对画质也是对的 —— 那些帧我们的时域历史本来也是废的。
constexpr uint64_t SETTLE_FRAMES = 60;      // 换链后稳定这么多帧才开工（≈1 秒）
std::atomic<uint64_t> g_settleCount{ 0 };
uint32_t g_settleWidth = 0;
uint32_t g_settleHeight = 0;
bool g_settleAnnounced = false;

// 返回 true = 可以开工了。
bool GameDlssSettled(const NgxEavesdropFrame& frame, bool confirmedNativeSr = false) noexcept {
	if (g_settleWidth != frame.renderWidth || g_settleHeight != frame.renderHeight) {
		g_settleWidth = frame.renderWidth; g_settleHeight = frame.renderHeight;
		g_settleCount.store(0); g_settleAnnounced = false;
		NgxEavesdrop::Get().NoteResetFrames();
	}
	// Successful native SR supplies this call's live output and guides. A reset
	// invalidates temporal history, not the current inputs. Forward it to NR on
	// this very frame instead of imposing the legacy 60-frame blackout. Render
	// dimension changes above still request a history reset, without a countdown.
	if (confirmedNativeSr) {
		if (frame.reset) {
			static uint64_t resets = 0;
			const auto n = ++resets;
			if (n <= 5 || n % 600 == 0) D5_LOG_INFO(
				L"NR native continuity: reset forwarded without settle pause frame=%llu total=%llu",
				(unsigned long long)frame.frameSeq, (unsigned long long)n);
		}
		return true;
	}
	// 换链信号：深度缓冲变化（深度追踪器发现新候选）。
	//
	// 进场景实测（core-3632）：swapchain 不重建、color/depth/mvec 渲染目标不变、
	// DLSS reset=0、output 指针也不变，唯一的变化是深度缓冲在剧烈重建（深度追踪器
	// 疯狂发现新候选）。所以深度缓冲变化才是唯一可靠的进场景信号，触发暂停 30 帧 +
	// 连续 10 帧 reset（由 NoteDepthChanged 置位）。

	// 深度重建后的暂停（时间基准 1 秒）：暂停期间 NR 不处理。
	if (NgxEavesdrop::Get().PauseActive()) {
		return false;
	}

	// frame.reset（游戏自己给的 reset 标志）：保留原有"重新 settle 暂停"行为。
	if (frame.reset) {
		if (g_settleCount.load(std::memory_order_relaxed) >= SETTLE_FRAMES &&
			g_settleAnnounced) {
			D5_LOG_INFO(L"DLSS5@evaluate：游戏重置了历史（reset=1 %ux%u），"
				L"暂停 %llu 帧等它稳定",
				frame.renderWidth, frame.renderHeight,
				(unsigned long long)SETTLE_FRAMES);
			g_settleAnnounced = false;
		}
		g_settleCount.store(0, std::memory_order_relaxed);
		g_settleWidth = frame.renderWidth;
		g_settleHeight = frame.renderHeight;
		return false;
	}
	const uint64_t n = g_settleCount.fetch_add(1, std::memory_order_relaxed) + 1;
	if (n < SETTLE_FRAMES) return false;
	if (!g_settleAnnounced) {
		g_settleAnnounced = true;
		D5_LOG_INFO(L"DLSS5@evaluate：游戏的 DLSS 已连续 %llu 帧稳定"
			L"（reset=0，目标 %ux%u 没变）—— 开始处理",
			(unsigned long long)SETTLE_FRAMES,
			frame.renderWidth, frame.renderHeight);
	}
	return true;
}

void RunPreEvaluateHook(
	uint32_t slot,
	ID3D12GraphicsCommandList* list,
	const NgxEavesdropFrame& frame,
	bool confirmedNativeSr = false) noexcept {
	const NgxEavesdrop::PreEvaluateHook hook =
		g_preEvaluateHook.load(std::memory_order_acquire);
	if (!hook || !list) return;

	const int32_t locked = g_preEvaluateSlot.load(std::memory_order_relaxed);
	if (locked >= 0 && uint32_t(locked) != slot) return;

	// 等游戏自己稳定。放在锁定判断之后、状态检查之前 —— 加载期间连状态都在变，
	// 那些"还没观察到状态"的日志本来也是这段时间刷出来的。
	if (!GameDlssSettled(frame, confirmedNativeSr)) return;

	// **SR→NR：颜色是 SR 输出（frame.output），状态是 NGX 契约的 UAV**。
	// DLSS SR 是 compute 写的，写完 output 留下 UNORDERED_ACCESS；这个状态我们观察
	// 不到（SR evaluate 内部的 barrier 不经过我们的 hook），但它是契约，不是猜。
	if (!frame.output) {
		static uint32_t complained = 0;
		if (complained++ < 4) {
			D5_LOG_INFO(L"DLSS5@evaluate：参数里没有 SR 输出，这一帧不处理");
		}
		return;
	}

	// **状态全部信 NGX 契约（文档化，不是猜）。**
	//
	// DLSS evaluate 时：
	// - SR 输出（frame.output）：UNORDERED_ACCESS —— DLSS SR 是 compute 写的，写完
	//   留下 UAV（OptiScaler OutputResourceBarrier 的默认值）。
	// - 输入（深度/矢量）：NON_PIXEL_SHADER_RESOURCE —— NGX 要求输入在 evaluate 时
	//   是这个状态，游戏在调用 DLSS 前已经把输入转好了。
	//
	// 之前用 ObservedState 观察游戏的 ResourceBarrier，但生化9 观察不到（矢量的
	// barrier 不经过我们的 hook，或指针每帧变填满 MAX_WATCHED 观察名单），导致 NR
	// 从不执行。OptiScaler 明确注释：NGX requires its inputs in
	// NON_PIXEL_SHADER_RESOURCE at evaluate time, which is a documented contract
	// rather than a guess about any one game's frame graph —— 直接信契约才对。
	if (!frame.motionVectors) {
		static uint32_t complained = 0;
		if (complained++ < 4) {
			D5_LOG_INFO(L"DLSS5@evaluate：参数里没有矢量，这一帧不处理");
		}
		return;
	}
	const uint32_t colorStateOut = uint32_t(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
	// Borrow the original guides at the native NGX call boundary, as in the
	// user's working 80d3bbcb build. This is the required compute-read access,
	// not an observation of the resource's complete state mask. ExecuteOnList
	// must leave these guides untouched: no copy and no transition. In particular,
	// a global barrier observation may belong to a different recording list.
	const uint32_t motionStateOut = uint32_t(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
	const uint32_t depthStateOut = frame.depth ? motionStateOut : 0u;

	// 打一次确认实际用的状态，方便和崩溃现场对齐（观察到的 vs 契约回退）。
	{
		static bool told = false;
		if (!told) {
			told = true;
			D5_LOG_WARN(L"DLSS5@evaluate（SR→NR）：颜色=UAV(0x%X) 矢量=0x%X 深度=0x%X"
				L"（原生 SR guides，借用只读；不拷贝、不转换 guides 状态）",
				(unsigned)colorStateOut, (unsigned)motionStateOut,
				(unsigned)depthStateOut);
		}
	}
	if (!hook(list, frame, colorStateOut, motionStateOut, depthStateOut)) {
		return;
	}

	if (locked < 0) {
		g_preEvaluateSlot.store(int32_t(slot), std::memory_order_relaxed);
		D5_LOG_INFO(L"DLSS5@evaluate：认定槽位 %u 为处理点（SR 输出 %ux%u fmt=%u）",
			slot, frame.outputWidth, frame.outputHeight, frame.outputFormat);
	}
	g_preEvaluateFrames.fetch_add(1, std::memory_order_relaxed);
}

// 找某个模块的补丁记录。没有就返回 nullptr。
// 调用方**不能**持有 g_patchMutex（这里只读，靠 count 的 acquire 语义）。
PatchedModule* FindPatched(void* returnAddressModuleHint) noexcept {
	// 直接按模块句柄找。调用方传的是自己所属模块的句柄。
	const uint32_t count = g_patchedCount.load(std::memory_order_acquire);
	for (uint32_t i = 0; i < count; ++i) {
		if (g_patched[i].module == returnAddressModuleHint) return &g_patched[i];
	}
	return nullptr;
}

// 从返回地址反查是哪个模块在调我们。IAT 补丁是按模块装的，但 hook 函数只有一份，
// 所以必须靠这个把调用归属到正确的"原函数"上。
HMODULE CallerModule(void* returnAddress) noexcept {
	HMODULE module = nullptr;
	GetModuleHandleExW(
		GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
		GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
		reinterpret_cast<LPCWSTR>(returnAddress), &module);
	return module;
}

void PatchModule(HMODULE module) noexcept;

std::atomic<HMODULE> g_observedFrameGenModule{nullptr};

std::wstring ModulePath(HMODULE module) {
    if (!module) return {};
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(module, path.data(), DWORD(path.size()));
    if (!n || n >= path.size()) return {};
    path.resize(n); return path;
}

void NoteFrameGenModule(HMODULE module, NgxModuleKind kind) {
    if (IsFrameGen(kind)) g_observedFrameGenModule.store(module);
}

// Known FG DLLs can be preloaded; presence only enables the buffer-count
// heuristic. Cached FG uses the full model path, never a shared .bin basename.
bool HasFrameGenModule() noexcept {
    if (GetModuleHandleW(L"nvngx_dlssg.dll") || GetModuleHandleW(L"sl.dlss_g.dll")) return true;
    const HMODULE cached = g_observedFrameGenModule.load();
    HMODULE alive = nullptr;
    if (!cached || !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(cached), &alive) || alive != cached) return false;
    return IsFrameGen(ClassifyNgxModule(ModulePath(alive)));
}

void OnModuleLoaded(HMODULE module) noexcept {
    if (!module || module == g_selfModule) return;
    const auto path = ModulePath(module);
    const auto kind = ClassifyNgxModule(path);
    if (IsFrameGen(kind)) {
        NoteFrameGenModule(module, kind);
        D5_LOG_INFO(L"NGX module excluded from hooks: kind=%s path=%s", NgxModuleKindName(kind), path.c_str());
        return;
    }
    const wchar_t* leaf = wcsrchr(path.c_str(), L'\\');
    leaf = leaf ? leaf + 1 : path.c_str();
    if (!IsNativeUpscaler(kind) && !IsInterestingModule(leaf)) return;
    PatchModule(module);
}

/* ---------------- hook ---------------- */

// 这几个 hook 的骨架都一样：按调用方模块找到它自己的原函数，转发，然后看结果。
// 找不到原函数就退回进程级的 LoadLibraryW —— 那说明归属判断出了偏差，
// 宁可少一次链式补丁，也不能把调用弄丢。

HMODULE WINAPI HookedLoadLibraryW(LPCWSTR name) {
	PatchedModule* owner = FindPatched(CallerModule(_ReturnAddress()));
	const LoadLibraryWFn original =
		owner && owner->loadLibraryW ? owner->loadLibraryW : &LoadLibraryW;
	const HMODULE result = original(name);
	if (result) OnModuleLoaded(result);
	return result;
}

HMODULE WINAPI HookedLoadLibraryA(LPCSTR name) {
	PatchedModule* owner = FindPatched(CallerModule(_ReturnAddress()));
	const LoadLibraryAFn original =
		owner && owner->loadLibraryA ? owner->loadLibraryA : &LoadLibraryA;
	const HMODULE result = original(name);
	if (result) OnModuleLoaded(result);
	return result;
}

HMODULE WINAPI HookedLoadLibraryExW(LPCWSTR name, HANDLE file, DWORD flags) {
	PatchedModule* owner = FindPatched(CallerModule(_ReturnAddress()));
	const LoadLibraryExWFn original =
		owner && owner->loadLibraryExW ? owner->loadLibraryExW : &LoadLibraryExW;
	const HMODULE result = original(name, file, flags);
	// 只当作"资源加载"的那几种标志不会真的初始化模块，别去碰
	constexpr DWORD DATA_ONLY = LOAD_LIBRARY_AS_DATAFILE |
		LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE;
	if (result && !(flags & DATA_ONLY)) OnModuleLoaded(result);
	return result;
}

HMODULE WINAPI HookedLoadLibraryExA(LPCSTR name, HANDLE file, DWORD flags) {
	PatchedModule* owner = FindPatched(CallerModule(_ReturnAddress()));
	const LoadLibraryExAFn original =
		owner && owner->loadLibraryExA ? owner->loadLibraryExA : &LoadLibraryExA;
	const HMODULE result = original(name, file, flags);
	constexpr DWORD DATA_ONLY = LOAD_LIBRARY_AS_DATAFILE |
		LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE;
	if (result && !(flags & DATA_ONLY)) OnModuleLoaded(result);
	return result;
}

// 读一个资源的尺寸/格式。拿不到就全 0。
void DescribeResource(ID3D12Resource* resource,
	uint32_t& width, uint32_t& height, uint32_t& format) noexcept {
	if (!resource) return;
	const D3D12_RESOURCE_DESC desc = resource->GetDesc();
	width = uint32_t(desc.Width);
	height = desc.Height;
	format = uint32_t(desc.Format);
}

// 抄一帧。所有 Get 失败都无所谓 —— 拿到多少算多少，缺的字段保持 0。
// 返回 false = 这一路不是 DLSS-SR，调用方不要再往下做。
bool SnapshotParameters(
	const NVSDK_NGX_Parameter* parameters, NgxEavesdropFrame& out) noexcept {
	if (!parameters) return false;
	// Get 不是 const 成员，而游戏给的是 const 指针（NGX 的签名就这样）。
	auto* p = const_cast<NVSDK_NGX_Parameter*>(parameters);

	NgxEavesdropFrame frame;
	auto res = [p](const char* key) -> ID3D12Resource* {
		void* value = nullptr;
		if (NVSDK_NGX_FAILED(p->Get(key, &value))) return nullptr;
		return static_cast<ID3D12Resource*>(value);
	};
	auto flt = [p](const char* key) -> float {
		float value = 0.0f;
		p->Get(key, &value);
		return value;
	};
	auto integer = [p](const char* key) -> int {
		int value = 0;
		p->Get(key, &value);
		return value;
	};

	frame.color = res(NVSDK_NGX_Parameter_Color);
	frame.depth = res(NVSDK_NGX_Parameter_Depth);
	frame.motionVectors = res(NVSDK_NGX_Parameter_MotionVectors);
	frame.output = res(NVSDK_NGX_Parameter_Output);
	frame.exposure = res(NVSDK_NGX_Parameter_ExposureTexture);
	frame.biasColorMask =
		res(NVSDK_NGX_Parameter_DLSS_Input_Bias_Current_Color_Mask);
	frame.jitterX = flt(NVSDK_NGX_Parameter_Jitter_Offset_X);
	frame.jitterY = flt(NVSDK_NGX_Parameter_Jitter_Offset_Y);
	frame.mvScaleX = flt(NVSDK_NGX_Parameter_MV_Scale_X);
	frame.mvScaleY = flt(NVSDK_NGX_Parameter_MV_Scale_Y);
	frame.preExposure = flt(NVSDK_NGX_Parameter_DLSS_Pre_Exposure);
	frame.frameTimeDeltaMs = flt(NVSDK_NGX_Parameter_FrameTimeDeltaInMsec);
	frame.reset = integer(NVSDK_NGX_Parameter_Reset);
	// DLSS 的 create flags。bit 3 = DepthInverted。
	// 用 Get 的成功与否来判断"游戏到底有没有告诉我们"—— 拿不到时**不能**当成 0，
	// 那会把 reversed-Z 的游戏（绝大多数）判成不反转，深度直接用反。
	{
		int flags = 0;
		if (!NVSDK_NGX_FAILED(p->Get(
			NVSDK_NGX_Parameter_DLSS_Feature_Create_Flags, &flags))) {
			frame.hasDepthInverted = true;
            frame.colorHdrKnown = true;
            frame.colorIsHdr = (flags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) != 0;
			frame.depthInverted =
				(flags & NVSDK_NGX_DLSS_Feature_Flags_DepthInverted) != 0;
            // Explicit successful metadata only. A missing flag is never treated as LDR.
            static std::atomic<unsigned> reports{0};
            if (reports.fetch_add(1, std::memory_order_relaxed) < 3)
                D5_LOG_INFO(L"NGX colour metadata: reported createFlags=0x%X HDR=%u; explicit color contract",
                    flags, (flags & NVSDK_NGX_DLSS_Feature_Flags_IsHDR) ? 1u : 0u);
		}
	}
	frame.renderWidth =
		uint32_t(integer(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width));
	frame.renderHeight =
		uint32_t(integer(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height));
	// **只认 DLSS-SR 的那一路。** 同一个进程里 DLSS-G（补帧）和 Ray Reconstruction
	// 也会走 EvaluateFeature，但它们的参数集完全不同（没有我们要的 color+depth+mvec
	// 三件套）。不筛一下的话，它们会把好的 SR 快照覆盖成半空的。
	if (!frame.color || !frame.depth || !frame.motionVectors) return false;

	// 资源自己的尺寸/格式。子矩形可能比资源小，两个都要记 —— 建副本要用资源尺寸。
	DescribeResource(frame.color, frame.colorWidth, frame.colorHeight,
		frame.colorFormat);
	DescribeResource(frame.depth, frame.depthWidth, frame.depthHeight,
		frame.depthFormat);
	DescribeResource(frame.motionVectors, frame.motionWidth, frame.motionHeight,
		frame.motionFormat);
	// SR 输出：SR→NR 时用它做 NR 的颜色输入，同样要观察状态和记录尺寸。
	if (frame.output) {
		DescribeResource(frame.output, frame.outputWidth, frame.outputHeight,
			frame.outputFormat);
		WatchResource(frame.output);
	}
	// 游戏曝光纹理（1x1，游戏填它当前用的曝光）。用它算白点才对。
	if (frame.exposure) {
		DescribeResource(frame.exposure, frame.exposureWidth,
			frame.exposureHeight, frame.exposureFormat);
		WatchResource(frame.exposure);
	}
	// 加入状态观察名单。从现在起游戏对它们下的每个 barrier 我们都会记下来 ——
	// 这是安全拷贝的前提（不能猜状态）。
	// 颜色也要观察：在 evaluate 点跑 DLSSNR 要读它、还要写回去。
	WatchResource(frame.color);
	WatchResource(frame.depth);
	WatchResource(frame.motionVectors);

	frame.frameSeq = g_frameSeq.fetch_add(1, std::memory_order_relaxed) + 1;
	// 记录第一帧的时刻（只记一次）。NR 初始化靠它延迟到游戏 DLSS 稳定之后。
	{
		uint64_t zero = 0;
		g_firstFrameAtMs.compare_exchange_strong(zero, GetTickCount64(),
			std::memory_order_relaxed, std::memory_order_relaxed);
	}

	{
		std::lock_guard<std::mutex> guard(g_frameMutex);
		g_frame = frame;
	}
	out = frame;

	// 只在第一帧和之后每 600 帧打一次：这条日志是用来确认"到底抄到了什么"的，
	// 每帧打会把日志冲爆（深度候选那次已经教过一遍）。
	if (frame.frameSeq == 1 || frame.frameSeq % 600 == 0) {
		D5_LOG_INFO(L"旁听到游戏的 DLSS 第 %llu 帧：color=%p depth=%p mvec=%p "
			L"output=%p 子矩形=%ux%u jitter=(%.4f, %.4f) mvScale=(%.2f, %.2f) "
			// 中文用 %s + 宽字面量。上一版写成 %hs 加 UTF-8 的 "是"，
			// 日志里变成 "æ¯" —— 看不懂的诊断等于没有诊断。
			L"reset=%d 深度反转=%s preExposure=%.4f dt=%.2fms exposure=%p(%ux%u fmt=%u) mask=%p",
			(unsigned long long)frame.frameSeq, frame.color, frame.depth,
			frame.motionVectors, frame.output, frame.renderWidth,
			frame.renderHeight, frame.jitterX, frame.jitterY, frame.mvScaleX,
			frame.mvScaleY, frame.reset,
			frame.hasDepthInverted ? (frame.depthInverted ? L"是" : L"否")
				: L"游戏没说",
			frame.preExposure,
			frame.frameTimeDeltaMs, frame.exposure, frame.exposureWidth,
			frame.exposureHeight, frame.exposureFormat, frame.biasColorMask);
	}
	return true;
}

// Only a call that actually reaches an SR/RR snippet is an upscaler call.
// The NGX core also receives FG/other features and may retain generic SR keys.
// Collect the real snippet arguments, but run NR after the outer core returns.
NVSDK_NGX_Result ForwardConfirmedUpscaler(uint32_t slot,
	ID3D12GraphicsCommandList* commandList, const NVSDK_NGX_Handle* handle,
	const NVSDK_NGX_Parameter* parameters, PFN_NVSDK_NGX_ProgressCallback callback) {
	auto real = reinterpret_cast<EvaluateFeatureFn>(g_realEvaluate[slot].load());
	if (!real) return NVSDK_NGX_Result_Fail;
	struct Call {
		NgxEavesdropFrame frame{};
		ID3D12GraphicsCommandList* srList = nullptr;
		const NVSDK_NGX_Handle* srHandle = nullptr;
		uint32_t srSlot = 0, leaves = 0;
		bool collecting = true, captured = false;
	};
	static thread_local Call* current = nullptr;
	const bool native = g_nativeUpscalerTarget[slot].load();
	auto observe = [&](Call& call) {
		if (!native || !call.collecting) return;
		++call.leaves;
		if (call.leaves == 1) {
			call.captured = SnapshotParameters(parameters, call.frame);
			call.srList = commandList; call.srHandle = handle; call.srSlot = slot;
		}
	};
	if (current) {
		observe(*current);
		GpuEventScope gpu(native ? commandList : nullptr, "NRFG/native-upscaler");
		return real(commandList, handle, parameters, callback);
	}
	Call call;
	struct ContextScope {
		Call*& target;
		ContextScope(Call*& p, Call& c) : target(p) { target = &c; }
		~ContextScope() { target = nullptr; }
	} context(current, call);
	observe(call);
	CommandListStateScope gameState(commandList);
	NVSDK_NGX_Result result;
	{
		GpuEventScope gpu(commandList, native ? "NRFG/native-upscaler" : "NRFG/NGX-core");
		D3D12Validation::Scope phase(L"original NGX Evaluate");
		result = real(commandList, handle, parameters, callback);
	}
	call.collecting = false;
	const bool confirmed = call.captured && call.leaves == 1 && call.srList == commandList;
	static std::atomic<uint64_t> ignored{0}, confirmedCount{0}, preRejected{0}, heaplessAccepted{0};
	if (!confirmed) {
		gameState.CancelRestore(); // FG/other features receive no state replay or NR work.
		const auto n = ++ignored;
		if (n <= 3 || n % 600 == 0) D5_LOG_INFO(
			L"NRFG ignored NGX call: outer=%u handle=%p nativeLeaves=%u captured=%d sameList=%d total=%llu",
			slot, handle, call.leaves, call.captured, call.srList == commandList, (unsigned long long)n);
		return result;
	}
	NrRouteProbe::Get().Observe(commandList, call.frame.output, call.srHandle,
		GetTickCount64(), NVSDK_NGX_SUCCEED(result) && gameState.Ready() &&
		!NgxEavesdrop::Get().CapturePaused());
	static std::mutex recording;
	if (NVSDK_NGX_FAILED(result) || !gameState.Ready() || NgxEavesdrop::Get().CapturePaused()) {
		const auto n = ++preRejected;
		if (n <= 5 || n % 600 == 0) {
			const auto& saved = gameState.SavedState();
			const auto& tracker = CommandListTracker::Get();
			D5_LOG_WARN(L"NRFG pre-NR rejected: total=%llu frame=%llu outer=%u native=%u "
				L"list=%p type=%u srResult=0x%08X srFailed=%d stateReady=%d capturePaused=%d "
				L"rootsReady=%d tracked=%d trackedCount=%u capacity=%u resetObserved=%d "
				L"savedHeaps=%u computeRoot=%p graphicsRoot=%p pso=%p outputFmt=%u output=%ux%u",
				(unsigned long long)n, (unsigned long long)call.frame.frameSeq,
				slot, call.srSlot, commandList, unsigned(commandList->GetType()),
				unsigned(result), NVSDK_NGX_FAILED(result) ? 1 : 0, gameState.Ready() ? 1 : 0,
				NgxEavesdrop::Get().CapturePaused() ? 1 : 0,
				CommandListTracker::rootsReady.load() ? 1 : 0,
				tracker.IsTracked(commandList) ? 1 : 0, tracker.TrackedCount(), tracker.Capacity(),
				saved.resetObserved ? 1 : 0,
				saved.heapCount, saved.computeRootSignature, saved.graphicsRootSignature,
				saved.pipelineState, call.frame.outputFormat, call.frame.outputWidth, call.frame.outputHeight);
		}
		return result;
	}
	if (gameState.SavedState().KnownEmptyDescriptorHeaps()) ++heaplessAccepted;
	if (!gameState.SavedState().computeRootSignature) {
		static std::atomic<uint64_t> emptyStateAccepted{0};
		const auto n = ++emptyStateAccepted;
		if (n <= 3 || n % 600 == 0) D5_LOG_INFO(
			L"NRFG known-empty compute state accepted: list=%p frame=%llu total=%llu resetObserved=1 heaps=%u",
			commandList, (unsigned long long)call.frame.frameSeq, (unsigned long long)n,
			gameState.SavedState().heapCount);
	}
	// Serialize CPU access too; a contending worker must not drop NR just because
	// the preceding worker is finishing its recording or waiting on the GPU.
	std::lock_guard<std::mutex> recordLock(recording);
	{
		GpuEventScope gpu(commandList, "NRFG/NR-post-upscaler");
		// The core callback acquires the GPU gate only after checking the NR
		// switch and input eligibility. NR-off frames must not stall on a fence.
		RunPreEvaluateHook(call.srSlot, commandList, call.frame, true);
	}
	const auto n = ++confirmedCount;
	if (n <= 3 || n % 600 == 0) {
		const auto s = EvaluateGpuGate::Get().Snapshot();
		D5_LOG_INFO(
		L"NRFG confirmed upscaler: outer=%u native=%u handle=%p list=%p type=%u frame=%llu "
		L"confirmed=%llu admitted=%llu ignored=%llu FGguard=%d waits=%llu waitMs=%llu maxWaitMs=%llu unsubmitted=%llu timeout=%llu "
		L"preRejected=%llu heaplessAccepted=%llu nrRecorded=%llu",
		slot, call.srSlot, call.srHandle, commandList, unsigned(commandList->GetType()),
		(unsigned long long)call.frame.frameSeq, (unsigned long long)n,
		(unsigned long long)s.admitted, (unsigned long long)ignored.load(),
		NgxEavesdrop::Get().FrameGenerationActive(0), (unsigned long long)s.waits,
		(unsigned long long)s.waitMs, (unsigned long long)s.maxWaitMs,
		(unsigned long long)s.unsubmitted, (unsigned long long)s.timeouts,
		(unsigned long long)preRejected.load(), (unsigned long long)heaplessAccepted.load(),
		(unsigned long long)g_preEvaluateFrames.load());
	}
	return result;
}

// 所有槽位共用这一段逻辑，slot 决定转发给谁。
NVSDK_NGX_Result ForwardEvaluate(
	uint32_t slot,
	ID3D12GraphicsCommandList* commandList,
	const NVSDK_NGX_Handle* handle,
	const NVSDK_NGX_Parameter* parameters,
	PFN_NVSDK_NGX_ProgressCallback callback) {
	const auto entered = g_enteredEvaluate[slot].fetch_add(1, std::memory_order_relaxed);
	if (!entered) D5_LOG_INFO(L"NGX Evaluate entered: slot=%u nativeUpscaler=%u",
		slot, g_nativeUpscalerTarget[slot].load() ? 1 : 0);
	if (IsTeardownRequested()) {
		const auto real = reinterpret_cast<EvaluateFeatureFn>(g_realEvaluate[slot].load(std::memory_order_acquire));
		return real ? real(commandList, handle, parameters, callback) : NVSDK_NGX_Result_Fail;
	}
	if (NgxEavesdrop::Get().EvaluateMode())
		return ForwardConfirmedUpscaler(slot, commandList, handle, parameters, callback);
	auto real = reinterpret_cast<EvaluateFeatureFn>(
		g_realEvaluate[slot].load(std::memory_order_acquire));
	if (!real) {
		// 不该发生：拿不到真函数就什么都别做，返回失败比崩掉好
		D5_LOG_ERROR(L"旁听：槽位 %u 的真 EvaluateFeature 是空的，放弃这一帧", slot);
		return NVSDK_NGX_Result_Fail;
	}

	const bool paused = NgxEavesdrop::Get().CapturePaused();
	// Core and snippet entry points can be nested. Observe the outer call once.
	static thread_local bool inEvaluate = false;
	if (inEvaluate) return real(commandList, handle, parameters, callback);
	struct Scope { bool& flag; Scope(bool& f) : flag(f) { flag = true; }
		~Scope() { flag = false; } } scope(inEvaluate);

	// **串行化「拷贝 + NR」（SR 不串行）。**
	//
	// 鬼武者多线程渲染会**并发**调 evaluate（不同帧、不同 worker 线程），而旁听副本
	// （g_motionCopy/g_depthCopy）和 NR 的内部纹理（_colorIn/_output/_decoded）是
	// 全局单份 —— 两个线程一个在 transition 副本、一个在 DLSSNR 读副本，GPU 状态
	// 冲突 → hang（实测跑 30 秒约 4800 帧后偶发卡死，所有线程停在 ntdll 等 GPU）。
	// 用一个原子标志，同一时刻只让一个帧做「拷贝 + NR」，并发帧跳过（只转 SR，
	// 不降噪但不崩）。
	static std::atomic<bool> s_evalBusy{ false };
	const bool nrTurn = !s_evalBusy.exchange(true);

	// Snapshot the native SR arguments without changing its parameter bag.
	// Evaluate NR borrows the guides directly. Only the non-FG Present route
	// needs retained copies; it consumes them after this call has returned.
	NgxEavesdropFrame frame;
	const bool haveFrame = SnapshotParameters(parameters, frame);
	const bool gpuTurn = nrTurn && haveFrame && !paused &&
		(!NgxEavesdrop::Get().EvaluateMode() ||
		 EvaluateGpuGate::Get().Begin(commandList));
	static std::atomic<uint64_t> admitted{0}, skipped{0};
	if (haveFrame && NgxEavesdrop::Get().EvaluateMode()) {
		if (gpuTurn) ++admitted; else ++skipped;
		if (frame.frameSeq % 600 == 0) D5_LOG_INFO(
			L"NR GPU admission: accepted=%llu skipped=%llu NR=%llu FG=%d",
			(unsigned long long)admitted.load(), (unsigned long long)skipped.load(),
			(unsigned long long)g_preEvaluateFrames.load(),
			NgxEavesdrop::Get().FrameGenerationActive(0) ? 1 : 0);
	}
	// Protect the game record on skipped frames too: otherwise SR's temporary
	// bindings from a skipped frame would become the next frame's "game" state.
	CommandListStateScope gameState(commandList, haveFrame && NgxEavesdrop::Get().EvaluateMode());
	if (gpuTurn && NgxEavesdrop::Get().EvaluateMode() && !gameState.Ready()) {
		static unsigned untracked = 0;
		if (++untracked <= 3) D5_LOG_WARN(L"SR→NR：SR 前的游戏绑定不完整，跳过 NR（不回放 SR 内部绑定）");
	}
	if (gpuTurn && !NgxEavesdrop::Get().EvaluateMode() &&
		!NgxEavesdrop::Get().FrameGenerationActive(0)) {
		D3D12Validation::Scope capturePhase(L"NR guide capture before SR");
		CaptureOnGameList(commandList, frame);
	}

	// 真 SR evaluate。
	NVSDK_NGX_Result result;
	{
		D3D12Validation::Scope srPhase(L"original SR Evaluate");
		result = real(commandList, handle, parameters, callback);
	}

	// **SR→NR：在真 SR evaluate 之后跑 NR**（不是之前）。
	//
	// 之前是 NR→SR（NR 处理 SR 输入 frame.color），实测两个问题：SR 输入是组合读
	// 状态（多列表并发读，原地读写无法安全介入）、NR 加的细节被 SR 神经网络超分
	// 洗掉 → 没效果。改成 SR→NR 后，NR 处理 SR 输出 frame.output（状态是 NGX 契约
	// 的 UAV，干净），效果直接落在最终画面，且 NR 在 FG 之前 → 与 FG 共存。
	if (nrTurn) {
		if (gpuTurn && gameState.Ready() && !NVSDK_NGX_FAILED(result)) {
			static bool announcedEnvelope = false;
			if (!announcedEnvelope) {
				announcedEnvelope = true;
				D5_LOG_INFO(L"SR→NR：已保存 SR 之前的游戏绑定；SR/NR 内部绑定不写入游戏状态记录");
			}
			RunPreEvaluateHook(slot, commandList, frame);
		}
		// 只有抢到锁的帧释放；没抢到（nrTurn=false）的是别人的锁，不能动。
		s_evalBusy.store(false);
	}

	return result;
}

// 每个槽位一个真函数入口。用模板生成，避免手写八份一样的代码。
template <uint32_t SLOT>
NVSDK_NGX_Result NVSDK_CONV EvaluateWrapper(
	ID3D12GraphicsCommandList* commandList,
	const NVSDK_NGX_Handle* handle,
	const NVSDK_NGX_Parameter* parameters,
	PFN_NVSDK_NGX_ProgressCallback callback) {
	return ForwardEvaluate(SLOT, commandList, handle, parameters, callback);
}

void* const g_wrappers[MAX_TARGETS]{
	reinterpret_cast<void*>(&EvaluateWrapper<0>),
	reinterpret_cast<void*>(&EvaluateWrapper<1>),
	reinterpret_cast<void*>(&EvaluateWrapper<2>),
	reinterpret_cast<void*>(&EvaluateWrapper<3>),
	reinterpret_cast<void*>(&EvaluateWrapper<4>),
	reinterpret_cast<void*>(&EvaluateWrapper<5>),
	reinterpret_cast<void*>(&EvaluateWrapper<6>),
	reinterpret_cast<void*>(&EvaluateWrapper<7>),
};

// Shutdown 的包装：游戏调它 = 开始拆图形栈。先撤钩子再转发 ——
// 这是三个退出触发点里最早的一个（另两个：WM_NCDESTROY、看门狗）。
// NGX Shutdown 之后游戏马上要释放 device/swapchain，那正是还挂着的
// vtable 钩子最危险的时刻（DS2 退出崩的现场）。
NVSDK_NGX_Result NVSDK_CONV ShutdownWrapper() {
	D5_LOG_INFO(L"旁听：游戏调用了 NGX Shutdown —— 开始拆图形栈，"
		L"先把钩子撤下来再转发（退出触发点 #0，早于窗口销毁）");
	RequestTeardown();
	auto real = reinterpret_cast<ShutdownFn>(
		g_realShutdown);
	if (!real) {
		D5_LOG_ERROR(L"旁听：真 Shutdown 是空的（不该发生），直接返回成功");
		return NVSDK_NGX_Result_Success;
	}
	return real();
}

// 给一个真地址找/分配槽位。返回对应的包装，装不下就返回 nullptr（调用方原样放过）。
void* WrapperForTarget(void* real, bool nativeUpscaler, uint32_t* assignedSlot = nullptr) noexcept {
	std::lock_guard<std::mutex> lock(g_targetMutex);
	const uint32_t count = g_targetCount.load(std::memory_order_acquire);
	for (uint32_t i = 0; i < count; ++i) {
		if (g_targetAddress[i].load(std::memory_order_relaxed) == real ||
			g_realEvaluate[i].load(std::memory_order_relaxed) == real) {
			if (nativeUpscaler) g_nativeUpscalerTarget[i].store(true);
			if (assignedSlot) *assignedSlot = i;
			return g_wrappers[i];
		}
	}
	if (count >= MAX_TARGETS) return nullptr;
	g_targetAddress[count].store(real, std::memory_order_relaxed);
	g_nativeUpscalerTarget[count].store(nativeUpscaler);
	g_realEvaluate[count].store(real, std::memory_order_release);
	g_targetCount.store(count + 1, std::memory_order_release);
	if (assignedSlot) *assignedSlot = count;
	return g_wrappers[count];
}

// Call only on an ordinary worker, never from LoadLibrary/DllMain callbacks:
// MinHook may suspend threads while enabling a hook. Only the precise native
// D3D12 SR/RR export is eligible; generic NGX, FG and Vulkan are untouched.
void AttachKnownUpscalerExport(HMODULE module, const std::wstring& path) noexcept {
	const auto kind = ClassifyNgxModule(path);
	if (!IsNativeUpscaler(kind) || IsTeardownRequested()) return;
	HMODULE retained = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
		reinterpret_cast<LPCWSTR>(module), &retained) || retained != module) {
		if (retained) FreeLibrary(retained);
		return;
	}
	// The enumerated image may have unloaded before we acquired the reference.
	if (ModulePath(retained) != path) { FreeLibrary(retained); return; }
	// Production calls originate in our unpatched core. The synthetic loader
	// tests deliberately patch their own EXE too, so honor its saved GPA value.
	const auto* owner = FindPatched(CallerModule(reinterpret_cast<void*>(&AttachKnownUpscalerExport)));
	const auto rawLookup = owner && owner->getProcAddress ? owner->getProcAddress : &GetProcAddress;
	void* target = reinterpret_cast<void*>(rawLookup(module, FN_EVALUATE));
	// Reject forwarded exports, including native-looking DLLs forwarding to FG.
	if (!target || CallerModule(target) != module) {
		FreeLibrary(retained);
		return;
	}
	uint32_t slot = MAX_TARGETS;
	if (!WrapperForTarget(target, true, &slot)) {
		FreeLibrary(retained);
		return;
	}
	std::unique_lock<std::mutex> guard(g_targetMutex);
	if (g_exportAttempted[slot] || IsTeardownRequested()) {
		guard.unlock();
		FreeLibrary(retained);
		return;
	}
	g_exportAttempted[slot] = true;
	const auto initialized = MH_Initialize();
	void* trampoline = nullptr;
	const auto created = initialized == MH_OK || initialized == MH_ERROR_ALREADY_INITIALIZED
		? MH_CreateHook(target, g_wrappers[slot], &trampoline) : initialized;
	if (created != MH_OK || !trampoline) {
		D5_LOG_WARN(L"NGX SR export recovery unavailable: status=%d path=%s", int(created), path.c_str());
		guard.unlock();
		FreeLibrary(retained);
		return;
	}
	// Publish a valid trampoline before enabling. Existing GPA wrappers can
	// already be executing on other threads. Even on enable failure retain the
	// trampoline and module: removing them would invalidate those in-flight calls.
	g_retainedExportModules[slot] = retained;
	g_realEvaluate[slot].store(trampoline, std::memory_order_release);
	const auto enabled = IsTeardownRequested() ? MH_ERROR_DISABLED : MH_EnableHook(target);
	if (enabled == MH_OK) {
		NgxEavesdrop::Get().NoteHookAttached();
		D5_LOG_INFO(L"NGX SR export recovered: slot=%u kind=%s target=%p trampoline=%p path=%s",
			slot, NgxModuleKindName(kind), target, trampoline, path.c_str());
	} else {
		D5_LOG_WARN(L"NGX SR export enable unavailable: status=%d slot=%u path=%s", int(enabled), slot, path.c_str());
	}
}

FARPROC WINAPI HookedGetProcAddress(HMODULE module, LPCSTR name) {
	PatchedModule* owner = FindPatched(CallerModule(_ReturnAddress()));
	const GetProcAddressFn original =
		owner && owner->getProcAddress ? owner->getProcAddress : &GetProcAddress;
	const FARPROC result = original(module, name);

	// name 可能是序号（高位为 0 的整数），那种不用管
	if (!result || !name || (reinterpret_cast<uintptr_t>(name) >> 16) == 0) {
		return result;
	}
	if (strncmp(name, "NVSDK_NGX_", 10) != 0) return result;

    const auto targetPath = ModulePath(module);
    const auto targetKind = ClassifyNgxModule(targetPath);
    const HMODULE addressModule = CallerModule(reinterpret_cast<void*>(result));
    const auto addressPath = addressModule == module ? targetPath : ModulePath(addressModule);
    const auto addressKind = ClassifyNgxModule(addressPath);
    // Exclude FG both by requested module and resolved export owner. This also
    // covers cached FG and forwarded exports. No FG Create/Evaluate wrapping.
    if (IsFrameGen(targetKind) || IsFrameGen(addressKind)) {
        NoteFrameGenModule(module, targetKind);
        NoteFrameGenModule(addressModule, addressKind);
        if (strcmp(name, FN_EVALUATE) == 0)
            D5_LOG_INFO(L"NGX Evaluate passthrough FG: kind=%s owner=%s path=%s", NgxModuleKindName(targetKind), NgxModuleKindName(addressKind), targetPath.c_str());
        return result;
    }
    NgxEavesdrop::Get().NoteNgxLookup(name);
    if (strcmp(name, FN_EVALUATE) == 0) {
        for (uint32_t i = 0; i < MAX_TARGETS; ++i) {
            if (reinterpret_cast<void*>(result) == g_wrappers[i]) return result;
        }
        const bool nativeUpscaler = IsNativeUpscaler(targetKind) || IsNativeUpscaler(addressKind);
        uint32_t slot = MAX_TARGETS;
        void* wrapper = WrapperForTarget(reinterpret_cast<void*>(result), nativeUpscaler, &slot);
        if (!wrapper) {
            D5_LOG_WARN(L"NGX Evaluate slots full (%u): forwarding path=%s", MAX_TARGETS, targetPath.c_str());
            return result;
        }
        NgxEavesdrop::Get().NoteHookAttached();
        D5_LOG_INFO(L"NGX Evaluate attached: slot=%u nativeUpscaler=%u kind=%s real=%p path=%s",
            slot, nativeUpscaler ? 1 : 0, NgxModuleKindName(targetKind), result, targetPath.c_str());
        if (addressModule != module)
            D5_LOG_INFO(L"NGX Evaluate resolved owner: slot=%u kind=%s path=%s", slot, NgxModuleKindName(addressKind), addressPath.c_str());
        return reinterpret_cast<FARPROC>(wrapper);
    }
	if (strcmp(name, FN_CREATE) == 0) {
		// **不再包装 CreateFeature：直接返回真地址。**
		//
		// CreateFeature 包装被证明干扰 FG：排除 FG 模块 + 旁听退场 + 深度追踪器退场
		// 之后仍黑屏，嫌疑集中到"非 FG 模块的 CreateFeature 被包装"——sl.interposer
		// 就在 FG 的调用链上（游戏 exe → sl.interposer → sl.dlss_g → nvngx_dlssg），
		// 它的 CreateFeature 被换成我们的包装地址就可能干扰 FG feature 初始化。
		//
		// FG 检测已经改用 swapchain BufferCount（见 HookedCreateSwapChainForHwnd），
		// 不再依赖 CreateFeature 判据，所以这里可以直接砍掉包装，让所有模块的
		// CreateFeature 都拿真地址。
		return result;
	}
	if (strcmp(name, FN_SHUTDOWN) == 0) {
		// 只包一次：真函数指针只能存第一份，第二次拿到的已经是我们的包装 ——
		// 存进去再转发就是无限递归（和 Evaluate 的幂等同一个理由）。
		if (reinterpret_cast<void*>(result) ==
			reinterpret_cast<void*>(&ShutdownWrapper)) {
			return result;
		}
		std::lock_guard<std::mutex> guard(g_shutdownMutex);
		if (!g_shutdownWrapped) {
			g_realShutdown = reinterpret_cast<ShutdownFn>(result);
			g_shutdownWrapped = true;
			D5_LOG_INFO(L"旁听：Shutdown 已包上（真地址 %p）—— "
				L"游戏调它时先把钩子撤下来", result);
		}
		return reinterpret_cast<FARPROC>(&ShutdownWrapper);
	}
	return result;
}

/* ---------------- 安装 ---------------- */

void PatchModule(HMODULE module) noexcept {
	std::lock_guard<std::mutex> guard(g_patchMutex);

	const uint32_t count = g_patchedCount.load(std::memory_order_relaxed);
	for (uint32_t i = 0; i < count; ++i) {
		if (g_patched[i].module == module) return;   // 已经打过
	}
	if (count >= MAX_PATCHED) return;

	PatchedModule entry;
	entry.module = module;
	// 每个函数各自记原值。任何一个没有静态导入都无所谓 —— 有几个补几个。
	Iat::Patch(module, "LoadLibraryW", &HookedLoadLibraryW,
		reinterpret_cast<void**>(&entry.loadLibraryW));
	Iat::Patch(module, "LoadLibraryA", &HookedLoadLibraryA,
		reinterpret_cast<void**>(&entry.loadLibraryA));
	Iat::Patch(module, "LoadLibraryExW", &HookedLoadLibraryExW,
		reinterpret_cast<void**>(&entry.loadLibraryExW));
	Iat::Patch(module, "LoadLibraryExA", &HookedLoadLibraryExA,
		reinterpret_cast<void**>(&entry.loadLibraryExA));
	Iat::Patch(module, "GetProcAddress", &HookedGetProcAddress,
		reinterpret_cast<void**>(&entry.getProcAddress));

	const bool any = entry.loadLibraryW || entry.loadLibraryA ||
		entry.loadLibraryExW || entry.loadLibraryExA || entry.getProcAddress;
	if (!any) return;   // 这个模块没有静态导入加载器函数，跳过

	g_patched[count] = entry;
	// 先写好内容再让 count 可见，否则别的线程可能看到半个条目
	g_patchedCount.store(count + 1, std::memory_order_release);

	wchar_t path[MAX_PATH]{};
	GetModuleFileNameW(module, path, MAX_PATH);
	D5_LOG_INFO(L"旁听：已给模块打上加载器补丁（LLW=%d LLA=%d LLExW=%d LLExA=%d "
		L"GPA=%d）<- %s",
		entry.loadLibraryW ? 1 : 0, entry.loadLibraryA ? 1 : 0,
		entry.loadLibraryExW ? 1 : 0, entry.loadLibraryExA ? 1 : 0,
		entry.getProcAddress ? 1 : 0, path);
	NgxEavesdrop::Get().NoteModulePatched();
}

}  // namespace

NgxEavesdrop& NgxEavesdrop::Get() noexcept {
	static NgxEavesdrop instance;
	return instance;
}

void NgxEavesdrop::NoteModulePatched() noexcept {
	_modulesPatched.fetch_add(1, std::memory_order_relaxed);
}

void NgxEavesdrop::NoteNgxLookup(const char* name) noexcept {
	const uint32_t nth = _ngxLookups.fetch_add(1, std::memory_order_relaxed) + 1;
	// 前若干次全记下来：游戏用的是哪一套 NGX API，看这几行就清楚了
	if (nth <= 24) D5_LOG_INFO(L"旁听：游戏解析 NGX 函数 %hs", name);
}

void NgxEavesdrop::Install(HMODULE selfModule, bool enabled) noexcept {
	if (_installed) return;
	if (!enabled) {
		D5_LOG_INFO(L"旁听未启用（settings.json 里设 diagEavesdrop=true 才装）");
		return;
	}
	_installed = true;
	g_selfModule = selfModule;

	// 根模块就是游戏 exe。挂起启动注入时它一行代码都还没跑，所以之后它加载
	// Streamline 的每一步我们都能接上。
	const HMODULE exe = GetModuleHandleW(nullptr);
	PatchModule(exe);

	// 已经加载过的、够格的模块也补一遍。晚注入时这是唯一的机会（虽然那时候
	// 大概已经太晚 —— 游戏早把地址解析完了）；早注入时这里通常什么也不做。
	HMODULE modules[512];
	DWORD needed = 0;
	if (EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) {
		const DWORD count = (std::min)(DWORD(_countof(modules)), needed / DWORD(sizeof(HMODULE)));
		for (DWORD i = 0; i < count; ++i) OnModuleLoaded(modules[i]);
	}

	D5_LOG_INFO(L"旁听已安装：打了 %u 个模块的补丁。"
		L"接下来等游戏解析 NVSDK_NGX_* 函数。",
		_modulesPatched.load(std::memory_order_relaxed));
}

void NgxEavesdrop::SetDevice(ID3D12Device* device) noexcept {
	g_device = device;
}

void NgxEavesdrop::PollKnownUpscalerExports() noexcept {
	if (!_installed || IsTeardownRequested()) return;
	// Serialize/throttle discovery independently of render and loader callbacks.
	static std::mutex pollMutex;
	std::unique_lock<std::mutex> pollGuard(pollMutex, std::try_to_lock);
	if (!pollGuard.owns_lock()) return;
	static uint64_t nextPoll = 0;
	const auto now = GetTickCount64();
	if (now < nextPoll) return;
	nextPoll = now + 1000;
	HMODULE modules[1024]{};
	DWORD needed = 0;
	if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed)) return;
	const auto count = (std::min)(DWORD(_countof(modules)), needed / DWORD(sizeof(HMODULE)));
	for (DWORD i = 0; i < count && !IsTeardownRequested(); ++i) {
		if (modules[i] == g_selfModule) continue;
		const auto path = ModulePath(modules[i]);
		const auto kind = ClassifyNgxModule(path);
		if (IsFrameGen(kind)) NoteFrameGenModule(modules[i], kind);
		else if (IsNativeUpscaler(kind)) AttachKnownUpscalerExport(modules[i], path);
	}
}

void NgxEavesdrop::NoteBarrier(
	ID3D12Resource* resource, uint32_t stateAfter, void* list) noexcept {
	if (!resource) return;
	// 这个函数跑在游戏每一次 ResourceBarrier 上，非常热 —— 只做几次原子比较。
	for (uint32_t i = 0; i < MAX_WATCHED; ++i) {
		if (g_watched[i].resource.load(std::memory_order_relaxed) != resource) continue;
		g_watched[i].state.store(stateAfter, std::memory_order_relaxed);
		if (list) {
			g_watched[i].lastList.store(list, std::memory_order_relaxed);
		}
		g_watched[i].known.store(true, std::memory_order_release);
		return;
	}
}

void* NgxEavesdrop::LastBarrierList(ID3D12Resource* resource) const noexcept {
	if (!resource) return nullptr;
	for (uint32_t i = 0; i < MAX_WATCHED; ++i) {
		if (g_watched[i].resource.load(std::memory_order_relaxed) != resource) continue;
		return g_watched[i].lastList.load(std::memory_order_relaxed);
	}
	return nullptr;
}

bool NgxEavesdrop::ResourceState(ID3D12Resource* resource, uint32_t& state) const noexcept {
	D3D12_RESOURCE_STATES observed{};
	if (!ObservedState(resource, observed)) return false;
	state = uint32_t(observed);
	return true;
}

bool NgxEavesdrop::SelfTestPreEvaluate(
	ID3D12Device* device, ID3D12CommandQueue* queue,
	bool lieAboutState) noexcept {
	const PreEvaluateHook hook = g_preEvaluateHook.load(std::memory_order_acquire);
	if (!device || !queue || !hook) {
		D5_LOG_WARN(L"夹具：evaluate 点自检跑不了（device=%p queue=%p hook=%p）",
			device, queue, (void*)hook);
		return false;
	}

	// 假的"游戏渲染分辨率"。刻意不用 backbuffer 尺寸 —— 这条路的全部意义就是
	// 处理一个和 backbuffer 不同的分辨率。
	constexpr uint32_t W = 1286;
	constexpr uint32_t H = 724;

	// 颜色要能当 render target —— 夹具要把它清成一个**已知的 HDR 值**，
	// 否则回读到的一律是 0，验不了"压进 0..1 再还原"这条往返到底准不准。
	auto make = [&](DXGI_FORMAT format, const wchar_t* name,
		bool renderTarget = false) -> ID3D12Resource* {
		D3D12_HEAP_PROPERTIES heap{};
		heap.Type = D3D12_HEAP_TYPE_DEFAULT;
		heap.CreationNodeMask = 1;
		heap.VisibleNodeMask = 1;
		D3D12_RESOURCE_DESC desc{};
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Width = W;
		desc.Height = H;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = format;
		desc.SampleDesc.Count = 1;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		if (renderTarget) desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
		ID3D12Resource* resource = nullptr;
		// 从 COMMON 起步：接下来那条 barrier 既做真的迁移，也让 NoteBarrier
		// **观察到**状态 —— 被测代码的前提就是"状态是观察来的，不是猜的"。
		if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE,
			&desc, D3D12_RESOURCE_STATE_COMMON, nullptr,
			IID_PPV_ARGS(&resource)))) {
			return nullptr;
		}
		resource->SetName(name);
		return resource;
	};

	// **颜色刻意用 R11G11B10_FLOAT。** 这不是随便挑的：鬼武者（RE Engine）的 DLSS
	// 输入颜色就是这个格式（fmt=26），而上一版的手写格式白名单里没有它 ——
	// evaluate 点那条路因此一帧都没跑起来。夹具用真实游戏的格式，这个 bug 才有回归测试。
	constexpr DXGI_FORMAT COLOR_FORMAT = DXGI_FORMAT_R11G11B10_FLOAT;
	ID3D12Resource* color = make(COLOR_FORMAT, L"D5Q.Test.Color", true);
	ID3D12Resource* depth = make(DXGI_FORMAT_R32_FLOAT, L"D5Q.Test.Depth");
	ID3D12Resource* motion = make(DXGI_FORMAT_R16G16_FLOAT, L"D5Q.Test.Motion");
	ID3D12CommandAllocator* allocator = nullptr;
	ID3D12GraphicsCommandList* list = nullptr;
	ID3D12Fence* fence = nullptr;
	bool ok = color && depth && motion &&
		SUCCEEDED(device->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
		SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
			allocator, nullptr, IID_PPV_ARGS(&list))) &&
		SUCCEEDED(device->CreateFence(
			0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
	if (list) list->SetName(L"D5Q.SelfTest.List");

	if (ok) {
		// 先登记再下 barrier：NoteBarrier 只记名单上的资源
		WatchResource(color);
		WatchResource(depth);
		WatchResource(motion);
		// **照真实游戏来：组合读状态。**
		//
		// 实测 RE Engine 在 evaluate 点把颜色/矢量放在 0xC0（非像素读|像素读），
		// 深度在 0xE0。夹具以前只用 0x40，于是"多余的 transition"这条路根本没被
		// 走到过 —— 而那正是真游戏里让设备挂掉的东西。
		// **夹具的输入不像真实输入，它就测不到真实的 bug。**
		constexpr D3D12_RESOURCE_STATES READY =
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

		// 先把颜色清成一个**已知的线性 HDR 值**（4, 5, 3）。
		// 这是这个夹具能验证"压进 0..1 再还原"的关键：回读到的处理后数值应该回到
		// 4/5/3 附近。之前颜色是全 0，往返对不对完全看不出来。
		ID3D12DescriptorHeap* rtvHeap = nullptr;
		D3D12_DESCRIPTOR_HEAP_DESC rtvDesc{};
		rtvDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
		rtvDesc.NumDescriptors = 1;
		if (SUCCEEDED(device->CreateDescriptorHeap(
				&rtvDesc, IID_PPV_ARGS(&rtvHeap)))) {
			const D3D12_CPU_DESCRIPTOR_HANDLE rtv =
				rtvHeap->GetCPUDescriptorHandleForHeapStart();
			device->CreateRenderTargetView(color, nullptr, rtv);
			Barrier(list, color, D3D12_RESOURCE_STATE_COMMON,
				D3D12_RESOURCE_STATE_RENDER_TARGET);
			const float hdr[4]{ 4.0f, 5.0f, 3.0f, 1.0f };
			list->ClearRenderTargetView(rtv, hdr, 0, nullptr);
			Barrier(list, color, D3D12_RESOURCE_STATE_RENDER_TARGET, READY);
			D5_LOG_INFO(L"夹具：测试颜色已清成线性 HDR (4, 5, 3) —— "
				L"处理后回读应该回到这附近，偏离多少就是往返的误差");
		} else {
			Barrier(list, color, D3D12_RESOURCE_STATE_COMMON, READY);
		}
		Barrier(list, depth, D3D12_RESOURCE_STATE_COMMON, READY);
		Barrier(list, motion, D3D12_RESOURCE_STATE_COMMON, READY);

		NgxEavesdropFrame frame;
		frame.color = color;
		frame.depth = depth;
		frame.motionVectors = motion;
		frame.colorWidth = W;
		frame.colorHeight = H;
		frame.colorFormat = uint32_t(COLOR_FORMAT);
		frame.depthWidth = frame.motionWidth = W;
		frame.depthHeight = frame.motionHeight = H;
		frame.depthFormat = uint32_t(DXGI_FORMAT_R32_FLOAT);
		frame.motionFormat = uint32_t(DXGI_FORMAT_R16G16_FLOAT);
		frame.renderWidth = W;
		frame.renderHeight = H;
		frame.mvScaleX = float(W);
		frame.mvScaleY = float(H);
		// **reset 必须是 0，否则新加的"等游戏稳定"闸门会直接挡掉这个夹具。**
		// 合成帧本来更适合 reset=1，但那个标志现在参与开工判定；这里更重要的是
		// 让夹具走**和真实游戏一样的那条闸门**，而不是绕过它。
		frame.reset = 0;
		frame.frameSeq = 1;

		// 顺手把闸门本身测了：拿同一帧连喂 SETTLE_FRAMES-1 次，**每一次都必须说
		// "还不行"**；下面 RunPreEvaluateHook 里的那一次正好是第 SETTLE_FRAMES 次，
		// 它必须放行。只验证"最后放行了"是不够的 —— 那种测试对阈值写成 1 也一样通过。
		if (!lieAboutState) {
			bool prematureOpen = false;
			// 循环 SETTLE_FRAMES 次而不是 -1 次：**第一次调用是"认目标"，不计数**
			// （g_settleColor 初始是空，第一帧一定判定为换了目标而清零）。
			// 所以真正的计数从第 2 次开始，下面 hook 里那次正好是第 SETTLE_FRAMES 次。
			// 这个 off-by-one 是夹具第一次跑就抓出来的 —— 它自己也得被测。
			for (uint64_t i = 0; i < SETTLE_FRAMES; ++i) {
				if (GameDlssSettled(frame)) { prematureOpen = true; break; }
			}
			if (prematureOpen) {
				D5_LOG_ERROR(L"夹具：**稳定闸门提前放行了** —— 阈值没起作用");
			} else {
				D5_LOG_INFO(L"夹具：稳定闸门前 %llu 次调用都正确挡住了",
					(unsigned long long)SETTLE_FRAMES);
			}
		}

		// **先在这条列表上绑一个假的描述符堆**，这样"状态还原"才有东西可还。
		//
		// 不这么做的话夹具的列表上什么都没绑，Snapshot 返回空、Restore 是空动作 ——
		// 又是一个不会失败的测试。而"没还描述符堆"正是上一轮让游戏闪退的原因，
		// 这条路必须能被验证。
		ID3D12DescriptorHeap* dummyHeap = nullptr;
		D3D12_DESCRIPTOR_HEAP_DESC dummyDesc{};
		dummyDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
		dummyDesc.NumDescriptors = 1;
		dummyDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
		if (SUCCEEDED(device->CreateDescriptorHeap(
				&dummyDesc, IID_PPV_ARGS(&dummyHeap)))) {
			dummyHeap->SetName(L"D5Q.Test.GameHeap");
			// 走真实路径（会经过我们 hook 的 setter，从而被记账）
			list->SetDescriptorHeaps(1, &dummyHeap);
		}

		bool ran = false;
		if (lieAboutState) {
			// 反向验证用：谎报颜色的 before 状态。资源真的在
			// NON_PIXEL_SHADER_RESOURCE，我们说它在 COPY_DEST —— 调试层必须报错。
			// 这一路**直接调 hook**，绕开 RunPreEvaluateHook，因为后者只会用观察到的
			// 真状态（那正是它的职责）。
			constexpr uint32_t LIE = uint32_t(D3D12_RESOURCE_STATE_COPY_DEST);
			D5_LOG_WARN(L"夹具：**故意谎报**颜色的 before 状态（真=0x%X 报=0x%X）—— "
				L"调试层现在应该报错。不报错说明这个夹具没在验证任何东西。",
				(unsigned)READY, LIE);
			ran = hook(list, frame, LIE, uint32_t(READY), uint32_t(READY));
		} else {
			// 正常一路走**完整的分发路径**（RunPreEvaluateHook），这样 ObservedState
			// 查表、槽位锁定、计数器都一起验到了 —— 而不只是验 ExecuteOnList。
			const uint64_t before = g_preEvaluateFrames.load(std::memory_order_relaxed);
			RunPreEvaluateHook(0, list, frame);
			ran = g_preEvaluateFrames.load(std::memory_order_relaxed) > before;
		}
		// **验证状态还原**：处理完之后，记账里那条列表绑的应该还是我们那个假堆。
		// 记账是跟着真实的 setter 走的，所以"记的还是假堆"就等价于"Restore 真的
		// 把假堆重新绑回去了"—— 而 D3D12 没有 getter，这是唯一能自动验的办法。
		if (dummyHeap && ran) {
			const CommandListState after =
				CommandListTracker::Get().Snapshot(list);
			const bool restored =
				after.heapCount == 1 && after.heaps[0] == dummyHeap;
			if (restored) {
				D5_LOG_INFO(L"夹具：**状态还原验证通过** —— "
					L"我们的 dispatch 换过描述符堆，处理完游戏那个堆被放回去了");
			} else {
				D5_LOG_ERROR(L"夹具：**状态还原失败**！处理完之后绑的不是游戏的堆"
					L"（记到 %u 个，第一个 %p，应该是 %p）。"
					L"真游戏里这就是 GPU 越界 -> 闪退。",
					after.heapCount, after.heaps[0], dummyHeap);
			}
		}
		// **不能在这里释放 dummyHeap** —— 命令列表还引用着它，而列表还没执行。
		// 提前释放会让调试层报「object referenced in the command list was deleted
		// prior to closing/executing」。挪到下面 fence 等完之后。

		// 夹具认下的槽位不能留着 —— 真游戏里 DLSS 未必落在槽位 0，留着会让真正的
		// 那一路被当成"另一条 evaluate"而永远跳过
		g_preEvaluateSlot.store(-1, std::memory_order_relaxed);

		// 把资源还回 COMMON，免得释放时状态不对（调试层会为此报警）
		Barrier(list, color, READY, D3D12_RESOURCE_STATE_COMMON);
		Barrier(list, depth, READY, D3D12_RESOURCE_STATE_COMMON);
		Barrier(list, motion, READY, D3D12_RESOURCE_STATE_COMMON);

		if (SUCCEEDED(list->Close())) {
			ID3D12CommandList* lists[]{ list };
			queue->ExecuteCommandLists(1, lists);
			queue->Signal(fence, 1);
			// 夹具是一次性的，等它做完再释放资源。**只有夹具才允许这样阻塞**。
			HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
			if (event && SUCCEEDED(fence->SetEventOnCompletion(1, event))) {
				WaitForSingleObject(event, 5000);
			}
			if (event) CloseHandle(event);
		} else {
			ok = false;
		}
		D5_LOG_INFO(L"夹具：evaluate 点自检 —— 处理回调%s（%ux%u）。"
			L"接下来看测试目标有没有打出 [D3D12 ERROR]：**一条都不该有**。",
			ran ? L"跑成功了" : L"**没跑**（看上面的原因）", W, H);
		// fence 已经等过了，GPU 一定做完了 —— 这里释放才安全
		if (rtvHeap) rtvHeap->Release();
		if (dummyHeap) dummyHeap->Release();
	} else {
		D5_LOG_ERROR(L"夹具：evaluate 点自检的资源建不出来");
	}

	// 先摘出观察名单再释放 —— 否则名单里留着悬空指针，地址被复用之后我们会拿
	// 陈旧状态去给别的资源下 barrier
	UnwatchResource(color);
	UnwatchResource(depth);
	UnwatchResource(motion);

	if (fence) fence->Release();
	if (list) list->Release();
	if (allocator) allocator->Release();
	if (motion) motion->Release();
	if (depth) depth->Release();
	if (color) color->Release();
	return ok;
}

void NgxEavesdrop::SetPreEvaluateHook(PreEvaluateHook hook) noexcept {
	g_preEvaluateHook.store(hook, std::memory_order_release);
}

uint64_t NgxEavesdrop::PreEvaluateFrames() const noexcept {
	return g_preEvaluateFrames.load(std::memory_order_relaxed);
}

uint64_t NgxEavesdrop::PreEvaluateStateMisses() const noexcept {
	return g_preEvaluateStateMisses.load(std::memory_order_relaxed);
}

NgxCapturedTextures NgxEavesdrop::Captured() const noexcept {
	NgxCapturedTextures out;
	std::lock_guard<std::mutex> guard(g_captureMutex);
	out.depth = g_depthCopy;
	out.motionVectors = g_motionCopy;
	out.width = g_copyWidth;
	out.height = g_copyHeight;
	out.mvScaleX = g_capturedMvScaleX.load(std::memory_order_relaxed);
	out.mvScaleY = g_capturedMvScaleY.load(std::memory_order_relaxed);
	out.frameSeq = g_capturedSeq.load(std::memory_order_acquire);
	return out;
}

NgxEavesdropFrame NgxEavesdrop::LatestFrame() const noexcept {
	std::lock_guard<std::mutex> guard(g_frameMutex);
	return g_frame;
}

uint64_t NgxEavesdrop::EvaluateSeen() const noexcept {
	return g_frameSeq.load(std::memory_order_relaxed);
}

uint64_t NgxEavesdrop::MsSinceFirstEvaluate() const noexcept {
	const uint64_t first = g_firstFrameAtMs.load(std::memory_order_relaxed);
	if (!first) return 0;
	const uint64_t now = GetTickCount64();
	return now >= first ? now - first : 0;
}

bool NgxEavesdrop::FrameGenerationModuleLoaded() const noexcept {
    return HasFrameGenModule();
}

bool NgxEavesdrop::FrameGenerationActive(uint64_t /*windowMs*/) const noexcept {
    const bool active = FrameGenSwapChains::Get().HasCandidate() && HasFrameGenModule();
    static std::atomic<bool> last{false};
    if (last.exchange(active) != active)
        D5_LOG_INFO(L"FG Present guard: active=%u candidateChains=%u (live chains + module heuristic)",
            active ? 1 : 0, FrameGenSwapChains::Get().CandidateCount());
    return active;
}

}  // namespace DXL
