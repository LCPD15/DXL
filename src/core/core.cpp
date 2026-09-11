// DXL core：被注入到游戏进程里的那一半。
//
// 里程碑 2a 的目标是把**接管 present 路径**这件事做扎实并可验证，再往上叠 NGX：
//   1. 拿到 DXGI 的 vtable 并打补丁（Present / Present1 / CreateSwapChainForHwnd）
//   2. 识别游戏用的渲染 API
//   3. 抓住 D3D12 的 command queue（NGX 提交工作必须要它，而 GetDevice 拿不到）
//   4. 把状态发布到共享内存，UI 侧实时可见
//
// hook 手法：vtable 打补丁。DXGI 的 swapchain / factory 实现来自 dxgi.dll，同一进程
// 内所有实例共享 vtable，所以在**任意一个**实例上打补丁，对进程里所有实例（包括打
// 补丁之前就建好的）都生效。vtable 在 .rdata 上是 copy-on-write，只影响本进程。
// 这比 inline hook 简单得多，也不需要引入 MinHook。
//
// 从哪里取 vtable 分两种情况，区别很重要：
//   · 早注入：只 hook DXGI **工厂**（CreateDXGIFactory1 不建设备），然后等游戏自己
//     创建 swapchain，从**它的**对象上取 vtable。我们一个设备都不建。
//   · 晚注入：游戏的 swapchain 早就建好了，工厂 hook 永远不会触发，只能自己造一个
//     临时的 D3D11 设备 + swapchain 把 vtable 读出来。
// 早注入以前也走"自己造设备"，但那意味着在游戏初始化图形栈**之前**抢先建一个真的
// D3D11 设备。OptiScaler 为此专门改过（v0.7.0-pre66，明确点名 Capcom 游戏），而
// 鬼武者就是 Capcom 的 RE Engine —— 这是"早注入才出问题"的一个具体嫌疑。

#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <psapi.h>
#include <atomic>
#include <cstdio>
#include <string>
#include <type_traits>

#include "../common/IpcProtocol.h"
#include "../common/StartupDiagnostics.h"
#include "../common/Log.h"
#include "../common/SemanticExtension.h"
#include "../common/SettingsReader.h"
#include "DepthTracker.h"
#include "DlssNrFilter.h"
#include "DlssNrFilter11.h"
#include "SegMaskFilter.h"
#include "CommandListTracker.h"
#include <cmath>
#include "HookTeardown.h"
#include "DlssSrUpscaler.h"
#include "FreezeWatchdog.h"
#include "NgxCallerProbe.h"
#include "NgxEavesdrop.h"
#include "EvaluateGpuGate.h"
#include "NrRouteProbe.h"
#include "D3D12Validation.h"
#include "ChainInject.h"
#include "NgxSession.h"
#include "SwapChainScaler.h"
#include "ReUiBackend.h"
#include "LegacyGraphics.h"
#include "PresentationOwner.h"
#include "DirectQueueCandidate.h"
#include "PresentInlineHooks.h"
#include "NvSmoothMotion.h"
#include "imgui.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "psapi.lib")

using namespace DXL;
using Ipc::FeatureState;
using Ipc::GraphicsApi;

namespace {

/* ============================ vtable 索引 ============================ */
// COM ABI 稳定，硬编码索引是这类 hook 的常规做法。
// IDXGISwapChain: IUnknown(0-2) IDXGIObject(3-6) IDXGIDeviceSubObject(7)
constexpr size_t VT_SWAPCHAIN_PRESENT = 8;
constexpr size_t VT_SWAPCHAIN_GET_BUFFER = 9;
// GetDesc(12) ResizeBuffers(13) ResizeTarget(14)
constexpr size_t VT_SWAPCHAIN_GET_DESC = 12;
constexpr size_t VT_SWAPCHAIN_RESIZE_BUFFERS = 13;
constexpr size_t VT_SWAPCHAIN_RESIZE_BUFFERS1 = 39;
// IDXGISwapChain1 继续排：GetDesc1(18) GetFullscreenDesc(19) GetHwnd(20)
// GetCoreWindow(21) Present1(22)
constexpr size_t VT_SWAPCHAIN_GET_DESC1 = 18;
constexpr size_t VT_SWAPCHAIN_PRESENT1 = 22;
// IDXGIFactory2: ... IDXGIFactory(7-11) IDXGIFactory1(12-13)
// IsWindowedStereoEnabled(14) CreateSwapChainForHwnd(15)
constexpr size_t VT_FACTORY_CREATE_SWAPCHAIN_FOR_HWND = 15;
// IDXGIFactory: EnumAdapters(7) MakeWindowAssociation(8) GetWindowAssociation(9)
// CreateSwapChain(10) —— 老接口，不少游戏还在用
constexpr size_t VT_FACTORY_CREATE_SWAPCHAIN = 10;
// ID3D12CommandQueue: IUnknown(0-2) ID3D12Object(3-6) ID3D12DeviceChild(7)
// ID3D12Pageable(无方法) UpdateTileMappings(8) CopyTileMappings(9)
// ExecuteCommandLists(10)
constexpr size_t VT_QUEUE_EXECUTE_COMMAND_LISTS = 10;

using PresentFn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*, UINT, UINT);
using Present1Fn = HRESULT(STDMETHODCALLTYPE*)(
	IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
using CreateSwapChainForHwndFn = HRESULT(STDMETHODCALLTYPE*)(
	IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
	const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
using CreateSwapChainFn = HRESULT(STDMETHODCALLTYPE*)(
	IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
using ExecuteCommandListsFn = void(STDMETHODCALLTYPE*)(
	ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);
using GetBufferFn = HRESULT(STDMETHODCALLTYPE*)(
	IDXGISwapChain*, UINT, REFIID, void**);
using GetDescFn = HRESULT(STDMETHODCALLTYPE*)(
	IDXGISwapChain*, DXGI_SWAP_CHAIN_DESC*);
using GetDesc1Fn = HRESULT(STDMETHODCALLTYPE*)(
	IDXGISwapChain1*, DXGI_SWAP_CHAIN_DESC1*);
using ResizeBuffersFn = HRESULT(STDMETHODCALLTYPE*)(
	IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

PresentFn g_originalPresent = nullptr;
Present1Fn g_originalPresent1 = nullptr;
CreateSwapChainForHwndFn g_originalCreateSwapChainForHwnd = nullptr;
CreateSwapChainFn g_originalCreateSwapChain = nullptr;
// swapchain 的 vtable 打过补丁了没有。原函数指针只能存第一次拿到的那份，
// 重复打补丁会把我们自己的 hook 当成"原函数"存进去，直接无限递归。
// 用原子是因为写它的是游戏线程（工厂 hook 里），读它的是我们的 InitThread。
std::atomic<bool> g_swapChainHooked{ false };
// 我们自己正在造探测 swapchain。
//
// 探测设备内部走的是 IDXGIFactory::CreateSwapChain，所以只要工厂 hook 已经装上，
// 我们自己的 8x8 探测链就会撞进自己的 hook 里，在日志上留下一条"游戏走的是老接口
// CreateSwapChain（8x8）"的假线索。晚注入靠调换顺序能避开，但 45 秒兜底那条路
// 躲不掉（工厂 hook 早在 45 秒前就装好了），所以还是要有这个开关。
// 只有我们自己那一个线程会用，用原子纯粹是为了写得干净。
std::atomic<bool> g_creatingProbe{ false };
ExecuteCommandListsFn g_originalExecuteCommandLists = nullptr;
GetBufferFn g_originalGetBuffer = nullptr;
GetDescFn g_originalGetDesc = nullptr;
GetDesc1Fn g_originalGetDesc1 = nullptr;
ResizeBuffersFn g_originalResizeBuffers = nullptr;
using ResizeBuffers1Fn = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain3*, UINT, UINT, UINT,
    DXGI_FORMAT, UINT, const UINT*, IUnknown* const*);
ResizeBuffers1Fn g_originalResizeBuffers1 = nullptr;

// 定义在"安装 hook"一节。工厂的 hook 要用它，而工厂 hook 在文件里更靠前。
bool PatchSwapChainVTable(IDXGISwapChain* swapChain) noexcept;
void TryInstallDepthTracker() noexcept;

template<class Fn>
Fn SwapOriginal(IUnknown* object, size_t index, Fn fallback) noexcept {
    void** slot = *reinterpret_cast<void***>(object) + index;
    std::lock_guard<std::mutex> lock(VtablePatchMutex());
    for (const auto& entry : VtablePatchRegistry())
        if (entry.slot == slot) return reinterpret_cast<Fn>(entry.original);
    return fallback;
}

/* ============================ 运行时状态 ============================ */

struct State {
	// 从 CreateSwapChainForHwnd 抓到的。D3D12 下 pDevice 参数就是 command queue。
	ID3D12CommandQueue* capturedQueue = nullptr;
	// A retained, device-validated submission queue can bootstrap native SR NR.
	// It is not proof of a swapchain's presentation/FG queue ownership.
	DirectQueueCandidate directQueueCandidate;
	bool queueHookInstalled = false;
	ID3D12Device* device12 = nullptr;
	ID3D11Device* device11 = nullptr;
	GraphicsApi api = GraphicsApi::Unknown;
	uint32_t width = 0;
	uint32_t height = 0;
	std::atomic<uint64_t> presentCount{ 0 };
	std::atomic<bool> enabled{ true };
	bool apiLogged = false;
	LARGE_INTEGER qpcFreq{};
	LARGE_INTEGER lastPresentQpc{};
	float frameMs = 0.0f;

	HMODULE selfModule = nullptr;
	DlssSrUpscaler upscaler;
	DlssNrFilter nrFilter;
	DlssNrFilter11 nrFilter11;
	SegMaskFilter segMask;
	bool segMaskTried = false, nr11Tried = false;
    bool semanticAvailable = false;
	SrSettings srSettings;
	NrSettings nrSettings;
	std::atomic<bool> settingsDirty{ true };
	// srSettings 由 present 线程写，命令线程直接读它是数据竞争 —— 实测会读到旧值，
	// 导致自动触发真超分时好时坏。把命令线程需要的那一位单独做成原子镜像。
	std::atomic<bool> wantsUpscale{ false };
	bool upscalerInitialized = false;
	// NR 的初始化包含加载 165MB 的 snippet，失败了别每帧重试
	std::atomic<bool> nrInitialized{ false };
	std::atomic<bool> nrTried{ false };
	// NR 初始化的跨线程互斥：present 路径（关 FG）和 swapchain 工厂（开 FG）都可能
	// 触发 TryInitNr，而这个初始化会 LoadLibrary + NGX Init_Ext，绝不能跑两遍。
	std::atomic<bool> nrInitBusy{ false };
	bool ngxTried = false;
	bool depthTrackerTried = false;
	// 我们是否赶在游戏创建 swapchain 之前就在场。CreateSwapChainForHwnd 的 hook
	// 被触发过就说明是。真超分和原生深度都取决于这一位。
	bool injectedEarly = false;
	// 注入方**声明**这次是早注入（见 IpcProtocol.h 的 EARLY_INJECT_EVENT_BASE）。
	// 和上面那位的区别：这一位是"意图"，上面那位是"结果"。两者不一致就说明
	// 早注入没达成目的（比如游戏用了我们没 hook 到的建链路径）。
	bool earlyInjectRequested = false;
	// 游戏无视了我们谎报的 swapchain 尺寸（按自己的分辨率渲染）。
	// 判据是主深度缓冲远大于代理尺寸 —— 见 IpcProtocol.h 里的说明。
	std::atomic<bool> proxySizeIgnored{ false };
	// **这个游戏被证明不看 swapchain 的 desc，代理这条路对它永久禁用。**
	// 一旦置上，SetupScaler 就再也不建代理，然后我们主动 nudge 一次窗口，
	// 让游戏把 backbuffer 重建成真的 —— 见 BanProxyAndRecover。
	std::atomic<bool> proxyBanned{ false };
	// 正在 nudge 窗口、等游戏重建 backbuffer 把代理建起来。
	// **这个标志存在的唯一目的是别在这两秒里说谎**：那期间"代理没建起来"是暂时的，
	// 而"降级 DLAA（注入太晚）"那条提示是一次性的、会一直挂在界面上不撤。
	std::atomic<bool> proxyNudgeInFlight{ false };

	/* ---- 诊断开关。都只在 settings.json 里改，UI 上不暴露 ---- */
	// 卡顿看门狗的阈值毫秒，0 = 关。
	uint32_t watchdogMs = 3000;
	// **夹具**：到第 N 次 present 时故意在滤镜链里睡 10 秒，用来验证看门狗真的能
	// 指认"是我们卡的"以及指对阶段。不验证过的诊断工具不能拿去查真问题 —— 它给出
	// 错误结论的代价比没有工具更大。默认 0 = 关。
	uint32_t stallAtPresent = 0;
	// 深度探测要 hook ID3D12Device 第 21 项和命令列表第 26/46/47 项，是我们对
	// 游戏侵入最深的地方（进程里每一次 draw 都会过我们的手）。排查"是谁把游戏搞卡了"
	// 时把它关掉，就能把嫌疑范围一次砍掉一半。
	bool disableDepthHooks = false;
	// 完全跳过 swapchain 的 vtable 补丁（Present/GetBuffer/GetDesc/ResizeBuffers）。
	// present hook 是整条"每帧回调"链的总开关 —— 关了它，OnPresent（含深度追踪、
	// 滤镜）全部不跑。排查"游戏重建 swapchain 后设备丢失"时用它和 diagNoDepthHooks、
	// diagEavesdrop 做二分，定位是 present hook / 深度追踪 / 旁听哪一路踩的雷。
	bool noPresentHook = false;
	// 只跳过 UI 叠加层（UiFramePresent → ReUi::FramePresent），保留 present hook 的
	// vtable 补丁 + OnPresent。用于在"present hook 链"内部再二分：区分是 vtable 补丁
	// 本身、还是 UI 每帧往游戏队列提交命令（画 imgui 到 backbuffer）踩的雷。
	bool noUiPresent = false;
	// 换链暂停到这一刻（GetTickCount64 毫秒）。游戏重建 swapchain / ResizeBuffers 时，
	// 我们往游戏自己的命令队列提交的**任何**工作（UI 的 imgui 命令、DLSSNR 的 present
	// 提交）都会撞上 DXGI 的换链同步窗口 —— MHW 实测在"第 2 次创建 swapchain"后
	// ~100ms 触发 ACCESS_LOST（先 UI、后 NR，两处都要避开）。重建后暂停一小段让
	// DXGI 把新链稳定下来，再恢复提交。0 = 不暂停。
	uint64_t uiPauseUntil = 0;
	// 换链后 UI 提交按**帧数**暂停（不同于上面按真实时间）。鬼武者实测：换链到 4K 时
	// present 会停约 3 秒（游戏重建渲染管线），这期间真实时间在走、uiPauseUntil 已
	// 过期，而 toast 的倒计时用的是 present 时间（g_uiClock）根本没走 —— 恢复后
	// toast 还活着、UI 提交照发，撞上游戏 4K 初始化的窗口 → ACCESS_LOST。所以 UI 的
	// 换链保护要用 present 帧数（present 停了就不递减），恢复后才开始数。
	int uiPauseFrames = 0;
	// **夹具**：故意像上一版那样 AddRef 持住 swapchain，复现"游戏再也建不出新
	// swapchain"的冻屏 bug。用来证明 --recreate-swapchain 那个夹具真的能抓到它 ——
	// 一个不会在 bug 面前失败的回归测试是没有意义的。默认 0 = 关。
	bool holdSwapChainRef = false;
	IDXGISwapChain* diagHeldSwapChain = nullptr;
	// 明知会弄坏游戏画面也硬上 NGX。只给排查用，默认关。
	bool allowNgxCoexist = false;
	// 旁听游戏自己的 DLSS 调用。**默认关** —— 见 NgxEavesdrop.h 的说明。
	bool eavesdrop = false;
	// FG（帧生成）检测的 swapchain BufferCount 阈值。开 FG 会多一块 backbuffer，
	// 关 FG 是三重缓冲=3。实测：生化9 开 FG=6、关 FG=3；鬼武者 开 FG=4、关 FG=3。
	// 默认 4 = "开了 FG 的下界"（两个游戏都覆盖）。
	// 个别游戏非 FG 也用四重缓冲（=4）会被误判成 FG → 误禁用 NR + 弹一次提示，
	// 玩家可把这个值调大到 5/6；反之开 FG 却漏检（崩/卡死）时可调小到 3。
	// 见配置文件里的 fgBufferCountThreshold 键。
	uint32_t fgBufferCountThreshold = 4;
	// 游戏内浮层的快捷键。mods 的位：1=Alt 2=Ctrl 4=Shift。
	// **只在这里读一次，中途改不生效** —— 低级钩子装上去就不重装了。
	// 默认 Alt+0：字母键容易撞游戏的物品栏/地图（用户实测踩过）。
	// 数字键在游戏里几乎不作快捷键。
	// 热键（虚拟键码，配置文件里的 hkEnable / hkUi，可在面板里重绑）。
	int uiLanguage = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE ? 0 : 2;
	int hkToggleEnabled = VK_DELETE;
	int hkEnableMods = 0;
	uint64_t hotkeyRevision = 0;
	int hkToggleUi = VK_END;
	int hkUiMods = 0;
	// DLSSNR 参数版本号：**只在浮层改参数（ApplyOverlaySetting）时 +1**。
	// UI 拿它区分"core 被游戏内改了"和"UI 自己推送的回声"（见 IpcProtocol）。
	std::atomic<uint32_t> nrParamVersion{ 0 };
	// 浮层参数的落盘防抖：改参数置脏 + 记时间，present 线程每帧检查
	// （拖滑条时 ApplyOverlaySetting 每帧都来，直接写盘一次拖动几十次写）。
	bool paramsDirty = false;
	uint64_t paramsChangedAt = 0;
	// 把 DLSSNR 挪到游戏的 DLSS evaluate 点上跑（真矢量唯一能真正用上的方式）。
	//
	// **默认关**，而且和 present 路径二选一：开了之后 present 路径完全不碰 NR。
	// 原因见 DlssNrFilter::ExecuteOnList 上面那段 —— 它跑在游戏的渲染线程上，
	// 而且会往游戏自己的颜色缓冲里写。要求 eavesdrop 也开着（没有旁听就没有接入点）。
	//
	// 这个不能是 atomic：present 线程和渲染线程都读它，但它只在 ReloadSettings
	// 那一刻变，而那也是 present 线程。用普通 bool 的代价是渲染线程可能晚一帧看到
	// 新值 —— 完全可以接受，比引入同步简单得多。
	std::atomic<bool> nrAtEvaluate{ false };
	// 配置里 dlss5AtEvaluate 的**原始意图**（不是运行时 canAtEvaluate）。
	//
	// 为什么必须单独存：nrAtEvaluate 存的是 canAtEvaluate = wantAtEvaluate &&
	// eavesdrop && nr.enabled，而 F8 开关 NR 会改 nr.enabled → canAtEvaluate 跟着变。
	// 落盘时如果拿 nrAtEvaluate 写回 dlss5AtEvaluate，就会"按 F8 关一次 NR"顺手把
	// dlss5AtEvaluate 写成 false，之后再也回不到 evaluate 点（正是这次"开 FG 失效"
	// 的真凶）。所以落盘要写这个原始意图，不能被运行时状态污染。
	bool wantAtEvaluate = true;
	bool nrAutoRoute = true;
	bool nrAutomaticHandoffBlocked = false; // route diagnostic, under g_nrStateMutex
	// 夹具：第 N 次 present 时造一张假的"游戏矢量"（1286x724，和真实游戏一致），
	// 让矢量重采样这条路在**开着调试层的测试目标**上真的跑起来。0 = 关。
	uint32_t fakeMotionAt = 0;
	ID3D12Resource* fakeMotion = nullptr;
	// 夹具：第 N 次 present 主动移除设备，验证转储路径。0 = 关。
	uint32_t removeDeviceAt = 0;
	// 夹具：第 N 次 present 时假装"游戏无视了我们报的 swapchain 尺寸"，
	// 用来在夹具上跑撤代理的恢复路径。见 diagBanProxyAt。
	uint32_t banProxyAt = 0;
	// 夹具：DLSS5 处理到第 N 帧时回读处理前后的像素值。0 = 关。
	uint32_t dumpPixelsAt = 0;
	// 夹具：第 N 次 present 时用假资源把 evaluate 点那条路走一遍。0 = 关。
	// **负数 = 在第 |N| 次 present 时故意谎报资源状态**，用来证明这个夹具真的能
	// 抓到错（在 bug 面前不会失败的测试没有意义）。
	int selfTestAtEvaluate = 0;

	// 真超分用的代理 backbuffer。只对 scaledSwapChain 这一个 swapchain 生效 ——
	// vtable 是全进程共享的，所以每个 hook 都必须先核对 this 指针。
	SwapChainScaler scaler;
	IDXGISwapChain* scaledSwapChain = nullptr;
	// 供命令线程读的镜像：scaler 本身由 present 线程写，跨线程读它不安全
	std::atomic<bool> scalerActive{ false };
	// 游戏的输出窗口和是否窗口化，每次 present 从 desc 抄一份普通值。
	//
	// **这里以前存的是 swapchain 指针，而且 AddRef 了 —— 那是个会把游戏冻住的 bug。**
	// DXGI 不允许同一个 HWND 上存在两个 swapchain。玩家在游戏里改 DLSS 时，游戏要
	// 销毁旧 swapchain 再建新的；只要我们还持有一份引用，旧对象就不会被销毁，
	// HWND 上的关联也就不会解除，于是它的 CreateSwapChainForHwnd 一直失败、一直重试。
	// 鬼武者实测：**连续 3333 次重建 swapchain、再也不 present，画面就此冻住**。
	// 我们只需要 HWND 和 Windowed 两个值，抄下来就行，绝对不要持有对象。
	HWND trackedWindow = nullptr;
	std::atomic<bool> trackedWindowed{ true };
	// GetDesc 要报告的代理尺寸，缓存一份避免每次调用都走 scaler
	DXGI_SWAP_CHAIN_DESC1 proxyDesc{};
};

State& g_state = *new State; // Explicit cleanup; unfinished GPU work outlives CRT static destruction.
std::recursive_mutex g_nrStateMutex;
bool UsesD3D11Bridge(GraphicsApi api) noexcept {
    return api == GraphicsApi::D3D11 || api == GraphicsApi::D3D9 || api == GraphicsApi::OpenGL;
}
// Select the implementation that owns the active NR textures.
const DlssNrFilter& ActiveNrFilter() noexcept {
    return UsesD3D11Bridge(g_state.api) ? g_state.nrFilter11.Filter12ForStatus() : g_state.nrFilter;
}
void UpdateSemanticMask() noexcept {
    const bool active = g_state.enabled.load() && g_state.nrSettings.enabled && g_state.semanticAvailable && g_state.nrSettings.semanticMask;
    g_state.segMask.SetActive(active);
    g_state.segMask.SetDebugView(false); // Preview is rendered in ImGui, no external layered window.
    if (!active) return;
    const bool bridge = UsesD3D11Bridge(g_state.api);
    auto* device = bridge ? g_state.nrFilter11.BridgeDevice12() : g_state.device12;
    auto* queue = bridge ? g_state.nrFilter11.BridgeQueue12() : g_state.capturedQueue;
    if (!device || !queue) return;
    if (!g_state.segMaskTried) {
        g_state.segMaskTried = true;
        g_state.segMask.Initialize(device, queue, g_state.selfModule);
    }
    g_state.segMask.SetSemanticGroups(g_state.nrSettings.semanticEnabled);
    DlssNrFilter::SemanticMaskProvider provider{};
    provider.ctx = &g_state.segMask;
    provider.snapshot = [](void* ctx, SemanticMaskSnapshot& out) noexcept {
        return static_cast<SegMaskFilter*>(ctx)->CopyLatest(out);
    };
    provider.capture = [](void* ctx, ID3D12GraphicsCommandList* list, ID3D12Resource* source,
                          D3D12_RESOURCE_STATES state, bool flipY) noexcept {
        return static_cast<SegMaskFilter*>(ctx)->RecordFrame(list, source, state, flipY);
    };
    provider.submitted = [](void* ctx, ID3D12CommandQueue* q, UINT count, ID3D12CommandList* const* lists) noexcept {
        static_cast<SegMaskFilter*>(ctx)->NotifySubmitted(q, count, lists);
    };
    if (bridge) g_state.nrFilter11.SetSemanticMaskProvider(provider);
    else g_state.nrFilter.SetSemanticMaskProvider(provider);
}
void CleanupDxl() noexcept {
    StopLegacyGraphicsHooks();
    std::lock_guard<std::recursive_mutex> lock(g_nrStateMutex);
    g_state.directQueueCandidate.Clear();
    CommandListTracker::Get().SetResetObserver(nullptr, nullptr);
    g_state.segMask.SetActive(false);
    const bool evaluateIdle = EvaluateGpuGate::Get().DrainForExit();
    if (!evaluateIdle) g_state.segMask.RetainForExit();
    g_state.segMask.TeardownForExit();
    ReUi::Shutdown();
    if (evaluateIdle) {
        g_state.nrFilter11.TeardownForExit();
        g_state.nrFilter.TeardownForExit();
    } else {
        // State is process-lifetime storage: no CRT destructor can undo this
        // retention while an unsubmitted/unfinished game list references NR.
        D5_LOG_WARN(L"DXL exit: external NR recording has not retired; retaining NR/mask GPU resources");
    }
    D3D12Validation::Detach();
}
// 注入成功的时刻（DllMain DLL_PROCESS_ATTACH 记录，GetTickCount64 毫秒）。NR 初始化
// 延迟 3 秒用它做基准。
std::atomic<uint64_t> g_injectAtMs{ 0 };
StartupDiagnostics::Writer g_startupDiagnostics{};
// Keep the small diagnostic mapping until process exit so a failed startup or
// DLL detach remains observable by the launcher. It is never accessed per frame.

/* ============================ 状态发布 ============================ */

class StatusPublisher {
public:
	bool Open() noexcept {
		wchar_t name[128]{};
		_snwprintf_s(name, _TRUNCATE, L"%s.%lu",
			Ipc::STATUS_MEMORY_BASE, GetCurrentProcessId());
		_mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr,
			PAGE_READWRITE, 0, sizeof(Ipc::Status), name);
		if (!_mapping) {
			const DWORD error = GetLastError();
			g_startupDiagnostics.StatusResult(false, error);
			D5_LOG_ERROR(L"CreateFileMapping failed: %lu", error);
			return false;
		}
		_status = static_cast<Ipc::Status*>(MapViewOfFile(
			_mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Ipc::Status)));
		if (!_status) {
			const DWORD error = GetLastError();
			g_startupDiagnostics.StatusResult(false, error);
			D5_LOG_ERROR(L"MapViewOfFile failed: %lu", error);
			return false;
		}
		ZeroMemory(_status, sizeof(Ipc::Status));
		_status->magic = Ipc::MAGIC;
		_status->version = Ipc::VERSION;
		_status->gamePid = GetCurrentProcessId();
		D5_LOG_INFO(L"status shared memory: %s", name);
		return true;
	}

	// seqlock：写前后各自增一次，UI 读到奇数就重读
	void Publish(const char* message = nullptr) noexcept {
        std::lock_guard<std::recursive_mutex> lock(g_nrStateMutex);
		if (!_status) return;
		++_status->sequence;
		MemoryBarrier();

		_status->api = static_cast<uint32_t>(g_state.api);
		_status->hooked = g_originalPresent ? 1u : 0u;
		_status->srState = static_cast<uint32_t>(
			// 被「游戏自己在跑 DLSS」拦下时报**不可用**而不是「已关闭」——
			// 用户明明开了，说「已关闭」等于把我们的拒绝说成是他的选择。
			NgxSession::Get().IsBlockedByGameNgx() ? FeatureState::Unavailable :
			!g_state.srSettings.enabled ? FeatureState::Disabled :
			g_state.api != GraphicsApi::D3D12 ? FeatureState::Unavailable :
			g_state.upscaler.EvaluateCount() ? FeatureState::Active :
			g_state.upscaler.FailureCount() ? FeatureState::Failed :
			FeatureState::Standby);
		_status->nrState = static_cast<uint32_t>(
			!g_state.nrSettings.enabled ? FeatureState::Disabled :
			(g_state.api != GraphicsApi::D3D12 && !UsesD3D11Bridge(g_state.api)) ? FeatureState::Unavailable :
			g_state.nrAutomaticHandoffBlocked && g_state.enabled.load() &&
				g_state.nrAtEvaluate && g_state.nrAutoRoute ? FeatureState::Standby :
			ActiveNrFilter().EvaluateCount() ? FeatureState::Active :
			ActiveNrFilter().IsDisabled() ? FeatureState::Failed :
			(UsesD3D11Bridge(g_state.api) ? g_state.nr11Tried && !g_state.nrFilter11.IsInitialized() : g_state.nrTried && !g_state.nrInitialized)
				? FeatureState::Unavailable
				: FeatureState::Standby);
		_status->fgState = static_cast<uint32_t>(FeatureState::Unavailable);
		{
			FreezeWatchdog& watchdog = FreezeWatchdog::Get();
			_status->stallCount = watchdog.StallCount();
			_status->lastStallStage =
				static_cast<uint32_t>(watchdog.LastStallStage());
			_status->srSkippedFrames = g_state.upscaler.SkippedFrames();
			_status->nrSkippedFrames = ActiveNrFilter().SkippedFrames();
		}
		// 总开关的当前状态。UI 不再自己记一份 —— 两边不一致时按一下 Alt+D 会反着来。
		_status->masterEnabled =
			g_state.enabled.load(std::memory_order_acquire) ? 1u : 0u;
		// 当前 DLSSNR 参数（v11）。浮层改的参数只落在内存里，这块把它们带回 UI，
		// 否则工具界面上显示的还是旧值 —— 两边不同步（用户实测踩过）。
		{
			const NrSettings& n = g_state.nrSettings;
			_status->nrParamIntensity = n.intensity;
			_status->nrParamLocalTone = n.localTone;
			_status->nrParamLocalStructure = n.localStructure;
			_status->nrParamSkinStructure = n.skinStructure;
			_status->nrParamPreset = uint32_t(n.preset);
			_status->nrParamStyle = uint32_t(n.style);
			_status->nrParamAutoMask = n.autoMask ? 1u : 0u;
			_status->nrParamUiCorrection = n.uiCorrection ? 1u : 0u;
			// 处理分辨率 —— 加浮层同步时漏了它（见 IpcProtocol 的注释）。
			_status->nrParamRenderScale = n.renderScale;
            _status->nrParamColourStrength = n.colourStrength;
            _status->nrParamSelfLayers = n.selfLayers;
            _status->nrParamTrueLayers = n.trueLayers;
            _status->nrParamOpticalFlow = n.opticalFlow;
            _status->nrParamOpticalFlowQuality = n.opticalQuality;
            _status->nrParamSemanticMask = n.semanticMask;
            _status->nrParamSemanticEnabled = n.semanticEnabled;
            _status->nrParamSemanticBg = n.semanticBgIntensity;
            _status->nrParamSemanticFlipY = n.semanticFlipY;
            _status->nrParamSemanticFeather = n.semanticFeather;
            _status->nrParamSemanticDebug = n.semanticDebugView;
            for (int g = 0; g < SEM_GROUP_COUNT; ++g) _status->nrParamSemanticIntensity[g] = n.semanticIntensity[g];
            const auto optical = ActiveNrFilter().OpticalStatus();
            _status->nrMotionSource = optical.active ? 2 : ActiveNrFilter().UsingExternalMotion() ? 1 : 0;
            _status->nrOpticalFlowMs = optical.active ? float(optical.gpuMs) : 0;
            _status->nrRoute = g_state.nrAutomaticHandoffBlocked && g_state.enabled.load() &&
                g_state.nrSettings.enabled && g_state.nrAtEvaluate && g_state.nrAutoRoute ? 3 :
                ActiveNrFilter().Mode() == NrMode::AtEvaluate ? 2 :
                ActiveNrFilter().Mode() == NrMode::Present ? 1 : 0;
			// 版本号只在浮层改参数时变（见 ApplyOverlaySetting）。
			// UI 只在版本号变化时采纳 core 报的值 —— 回声（UI 自己推送产生的）
			// 版本号不变，采纳的话会把本地正在编辑的值抢回旧值。
			_status->nrParamVersion =
				g_state.nrParamVersion.load(std::memory_order_acquire);
		}
		{
			NgxEavesdrop& eaves = NgxEavesdrop::Get();
			_status->eavesdropModules = eaves.ModulesPatched();
			_status->eavesdropLookups = eaves.NgxLookups();
			_status->eavesdropFrames = eaves.EvaluateSeen();
			// **这个游戏自己在用 DLSS 吗。** 解析过 NVSDK_NGX_* 就算 ——
			// 比"真的 evaluate 过"更早为真，而 UI 需要在玩家勾选我们的超分**之前**
			// 就知道会不会撞车（两套 DLSS 同时跑会崩）。
			if (eaves.NgxLookups() > 0 || eaves.EvaluateSeen() > 0) {
				_status->gameDlssSeen = 1;
			}
			const NgxEavesdropFrame frame = eaves.LatestFrame();
			_status->eavesdropRenderWidth = frame.renderWidth;
			_status->eavesdropRenderHeight = frame.renderHeight;
			_status->eavesdropHasDepth = frame.depth ? 1u : 0u;
			_status->eavesdropHasMotion = frame.motionVectors ? 1u : 0u;
			_status->eavesdropJitterX = frame.jitterX;
			_status->eavesdropJitterY = frame.jitterY;
			const NgxCapturedTextures captured = eaves.Captured();
			_status->eavesdropCapturedFrames = captured.frameSeq;
			_status->eavesdropCapturedWidth = captured.width;
			_status->eavesdropCapturedHeight = captured.height;
			_status->eavesdropMvScaleX = captured.mvScaleX;
			_status->eavesdropMvScaleY = captured.mvScaleY;
			_status->nrAtEvaluateFrames = eaves.PreEvaluateFrames();
			_status->nrAtEvaluateWanted = g_state.nrAtEvaluate ? 1u : 0u;
			// 一帧都没处理到的话，说清楚卡在哪一步。**顺序 = 从最具体到最笼统**：
			// 滤镜给的原因（格式/尺寸/NGX 报错）比"状态还没观察到"具体得多，而后者在
			// 头几帧本来就必然发生，先报它会把真原因盖住。
			_status->nrAtEvaluateBlocked =
				uint32_t(Ipc::NrAtEvaluateBlock::None);
			if (g_state.nrAtEvaluate && _status->nrAtEvaluateFrames == 0) {
				const uint32_t filterBlock = ActiveNrFilter().LastBlock();
				if (eaves.EvaluateSeen() == 0) {
					// **判据是“抄到过几帧”，不是“挂上了几个钩子”。** 实测踩过：一次运行挂上了
						// 1 个钩子，但游戏真正那条调用路径在我们进来之前就把函数指针取走了 ——
						// 抄到 0 帧，而“钩子数 > 0”让这条原因没能报出来，UI 上又变成了
						// “注入成功、零报错、毫无效果”。
					// 放在最前面 —— 它比任何别的原因都更具体，而且用户能直接处理
					// （改用工具启动游戏，别注入已经跑起来的进程）。
					_status->nrAtEvaluateBlocked = uint32_t(
						Ipc::NrAtEvaluateBlock::EavesdropNotAttached);
				} else if (!g_state.nrInitialized) {
					_status->nrAtEvaluateBlocked =
						uint32_t(Ipc::NrAtEvaluateBlock::NrNotReady);
				} else if (filterBlock != uint32_t(Ipc::NrAtEvaluateBlock::None)) {
					_status->nrAtEvaluateBlocked = filterBlock;
				} else if (eaves.PreEvaluateStateMisses() > 0) {
					_status->nrAtEvaluateBlocked =
						uint32_t(Ipc::NrAtEvaluateBlock::StateUnobserved);
				}
			}
			// 走 evaluate 点时，"用上真矢量"不再看 SetExternalMotion —— 那是
			// present 路径的开关。那条路上矢量是**直接**喂原资源的，所以判据就是
			// "这条路真的跑起来了"。
			_status->nrUsingRealMotion = ActiveNrFilter().Mode() == NrMode::AtEvaluate
				? (_status->nrAtEvaluateFrames > 0 ? 1u : 0u)
				: (ActiveNrFilter().UsingExternalMotion() ? 1u : 0u);
		}
		DepthTracker& depth = GetDepthTracker();
		_status->nativeDepth = depth.Selected() ? 1u : 0u;
		_status->nativeMotion = 0;
		_status->injectedEarly = g_state.injectedEarly ? 1u : 0u;
		_status->proxyActive =
			g_state.scalerActive.load(std::memory_order_acquire) ? 1u : 0u;
		_status->depthCandidates = depth.CandidateCount();
		_status->depthSelected = depth.Selected() ? 1u : 0xFFFFFFFFu;
		_status->proxySizeIgnored =
			g_state.proxySizeIgnored.load(std::memory_order_acquire) ? 1u : 0u;
		if (g_state.upscaler.IsReady()) {
			_status->renderWidth = g_state.upscaler.RenderWidth();
			_status->renderHeight = g_state.upscaler.RenderHeight();
			_status->outputWidth = g_state.upscaler.OutputWidth();
			_status->outputHeight = g_state.upscaler.OutputHeight();
		} else {
			_status->renderWidth = g_state.width;
			_status->renderHeight = g_state.height;
			_status->outputWidth = g_state.width;
			_status->outputHeight = g_state.height;
		}
		_status->evaluateCount = g_state.upscaler.EvaluateCount();
		_status->evaluateFailures = g_state.upscaler.FailureCount();
		_status->nrEvaluateCount = ActiveNrFilter().EvaluateCount();
		_status->nrEvaluateFailures = ActiveNrFilter().FailureCount();
		_status->presentCount = g_state.presentCount.load(std::memory_order_relaxed);
		_status->frameMs = g_state.frameMs;
		_status->nrGpuMs = ActiveNrFilter().TimingSamples()
			? ActiveNrFilter().RecentGpuMs() : 0.0f;
		_status->nrGpuMsWorst = ActiveNrFilter().WorstGpuMs();
		if (message) {
			strncpy_s(_status->message, message, _TRUNCATE);
		}

		MemoryBarrier();
		++_status->sequence;
	}

private:
	HANDLE _mapping = nullptr;
	Ipc::Status* _status = nullptr;
};

StatusPublisher g_publisher;

// "真超分降级成 DLAA"那条提示只说一次。**放成全局是为了能撤**：代理后来建起来了
// 就要把它清掉并改口，否则界面上一直挂着一句已经不成立的话（见 RunFilters 里
// 那段说明）。函数内的 static 做不到这一点。
bool g_srDegradeWarned = false;

/* ============================ 设置 ============================ */

static bool WriteDefaultSettingsNearCore() noexcept {
	const auto config = SettingsReader::ConfigRoot(g_state.selfModule) / L"profiles" / L"default.json";
	const std::wstring path = config.wstring();
	std::error_code createError;
	std::filesystem::create_directories(config.parent_path(), createError);
	std::error_code ec;
	if (std::filesystem::exists(path, ec) && !ec) {
		// 已有非空配置，别动。0 字节多半是上次写坏了一半，落下去重写。
		const uintmax_t sz = std::filesystem::file_size(path, ec);
		if (!ec && sz > 0) return true;
	}

	static const char kDefaultConfig[] =
		"{\n"
		"  \"dlss5Enable\": true,\n"
		"  \"dlss5AtEvaluate\": true,\n"
		"  \"nrAutoRoute\": true,\n"
		"  \"diagEavesdrop\": true,\n"
		"  \"nrUseRealDepth\": true,\n"
		"  \"nrUseRealMotion\": true,\n"
		"  \"nrOpticalFlow\": true,\n"
		"  \"nrOpticalFlowQuality\": 1,\n"
		"  \"nrRenderScale\": 1.0,\n"
		"  \"nrPreset\": 0,\n"
		"  \"nrStyle\": 2,\n"
		"  \"nrIntensity\": 1.0,\n"
		"  \"nrLocalTone\": 1.0,\n"
		"  \"nrColourStrength\": 1.0,\n"
		"  \"nrSelfLayers\": 1.0,\n"
		"  \"nrTrueLayers\": 1,\n"
		"  \"nrLocalStructure\": 1.0,\n"
		"  \"nrSkinStructure\": 0.6,\n"
		"  \"nrAutoMask\": true,\n"
		"  \"nrUiCorrection\": true,\n"
		"  \"nrDebugView\": 0,\n"
		"  \"mvScale\": 1.0,\n"
		"  \"depthInverted\": false,\n"
		"  \"hkEnable\": 46,\n"
		"  \"hkEnableMods\": 0,\n"
		"  \"hkUi\": 35,\n"
		"  \"hkUiMods\": 0,\n"
		"  \"hotkeyRevision\": 0,\n"
		"  \"srEnable\": false\n"
		"}\n";
	FILE* f = nullptr;
	if (_wfopen_s(&f, path.c_str(), L"wb") != 0 || !f) return false;
	const size_t n = strlen(kDefaultConfig);
	const bool ok = fwrite(kDefaultConfig, 1, n, f) == n;
	fclose(f);
	return ok;
}

void ReloadSettings() noexcept {
	std::lock_guard<std::recursive_mutex> nrLock(g_nrStateMutex);
	SettingsReader reader;
	const std::filesystem::path path =
		SettingsReader::FindPath(g_state.selfModule);
	if (!reader.Load(path)) {
		D5_LOG_WARN(L"settings.json 读不到 (%s)，用默认值", path.c_str());
		g_state.srSettings = SrSettings{};
		return;
	}
	// **"到底读的是哪个文件"永远不该是个谜。**
	//
	// core 找的是 `profiles\<小写完整exe名>.json`，找不到退 `default.json`，
	// 再找不到退 `settings.json`。而 UI 侧的文件名一度是按 profile.id 拼的
	// （id 去掉了 `.exe`）—— 两边规则不一致时，玩家在界面上改的东西静默不生效，
	// 而日志里一个字都看不出来。只在路径变化时打，别每次 reload 都刷一条。
	{
		static std::wstring lastPath;
		if (lastPath != path.wstring()) {
			lastPath = path.wstring();
			D5_LOG_INFO(L"设置文件：%s", path.c_str());
		}
	}
	// 诊断覆盖层。**UI 不写这个文件**，所以手写的诊断键不会被"从工具启动"时的
	// 那次重写抹掉 —— 实测踩过一次：profiles\<exe>.json 是 UI 生成的，启动时会被
	// settings.json 整个覆盖，手加的键连一局都活不过，那一局什么都没测到。
	const std::filesystem::path diagPath =
		SettingsReader::FindDiagPath(g_state.selfModule);
	if (reader.LoadDiagOverlay(diagPath)) {
		D5_LOG_WARN(L"诊断覆盖已生效：%s（里面的键会盖掉主配置）", diagPath.c_str());
	}
	// 浮层参数的持久层（第二个覆盖，盖在主配置之上）。**参数收归 core 自己持久化**
	// （见 SettingsReader::FindParamsPath 的说明）—— UI 的扁平文件里就算还有参数键，
	// 这一层的键也优先；UI 的 persistAll 再怎么重写 `<exe>.json` 都盖不到参数。
	if (const std::filesystem::path paramsPath =
			SettingsReader::FindParamsPath(g_state.selfModule);
		!paramsPath.empty()) {
		if (reader.LoadParamsOverlay(paramsPath)) {
			D5_LOG_INFO(L"浮层参数已从持久层读取：%s（里面的键盖掉主配置）",
				paramsPath.c_str());
		}
	}
    g_state.uiLanguage = reader.GetInt("overlayLang", PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_CHINESE ? 0 : 2);
    g_state.hkToggleEnabled = std::clamp(reader.GetInt("hkEnable", VK_DELETE), 1, 255);
    g_state.hkEnableMods = reader.GetInt("hkEnableMods", 0) & 15;
    g_state.hkToggleUi = std::clamp(reader.GetInt("hkUi", reader.GetInt("overlayVk", VK_END)), 1, 255);
    g_state.hkUiMods = reader.GetInt("hkUiMods", reader.GetInt("overlayMods", 0)) & 15;
    g_state.hotkeyRevision = reader.GetUInt64("hotkeyRevision", 0);
    if (g_state.hotkeyRevision == 0) {
        if (g_state.hkToggleEnabled == VK_F8 && g_state.hkEnableMods == 0) g_state.hkToggleEnabled = VK_DELETE;
        if (g_state.hkToggleUi == '0' && g_state.hkUiMods == 1) {
            g_state.hkToggleUi = VK_END; g_state.hkUiMods = 0;
        }
    }
	SrSettings settings;
	settings.enabled = reader.GetBool("srEnable", false);
	settings.mode = reader.IsString("srMode", "upscale")
		? SrMode::Upscale : SrMode::Dlaa;
	settings.sharpness = reader.GetFloat("sharpness", 0.0f);

	// 质量档决定渲染倍率，也决定 NGX 用哪个模型；只有 custom 才看滑块
	const std::string quality = reader.GetString("srQuality", "quality");
	if (quality == "custom") {
		settings.quality = SrQuality::Custom;
		settings.inputMultiplier = reader.GetFloat("srInputMultiplier", 0.67f);
	} else {
		settings.quality =
			quality == "ultra_perf" ? SrQuality::UltraPerformance :
			quality == "performance" ? SrQuality::Performance :
			quality == "balanced" ? SrQuality::Balanced : SrQuality::Quality;
		settings.inputMultiplier = QualityMultiplier(settings.quality);
	}
	if (settings.mode == SrMode::Dlaa) settings.inputMultiplier = 1.0f;

	g_state.srSettings = settings;
	g_state.wantsUpscale.store(
		settings.enabled && settings.mode == SrMode::Upscale,
		std::memory_order_release);
	D5_LOG_INFO(L"设置: srEnable=%d mode=%s quality=%hs 倍率=%.3f 锐化=%.2f",
		settings.enabled ? 1 : 0,
		settings.mode == SrMode::Upscale ? L"upscale" : L"dlaa",
		quality.c_str(), settings.inputMultiplier, settings.sharpness);

	// 诊断开关只在第一次读设置时生效：hook 装上去就拆不掉了，中途改没有意义，
	// 反而会让"我到底测的是哪个配置"变得说不清。
	static bool diagnosticsRead = false;
	if (!diagnosticsRead) {
		diagnosticsRead = true;
		// **总开关的初值跟着配置走，之后以运行时（Alt+D）为准。**
		//
		// 只在第一次读设置时取：不然玩家在游戏里按了 Alt+D，随手在界面上拖个滑块
		// 触发一次 ReloadSettings，刚按的开关就被文件里的旧值冲掉了。
		// 默认 true —— 本 mod 的产品定位是"进游戏默认开 DLSS5"，由玩家按 F8 关，
		// 面板会把这次选择存回配置，下一局就按存的来。
		g_state.enabled.store(reader.GetBool("masterEnabled", true),
			std::memory_order_release);
		D5_LOG_INFO(L"总开关初值 = %s（来自配置 masterEnabled；之后以 Alt+D 为准）",
			g_state.enabled.load(std::memory_order_acquire) ? L"开" : L"关");
		g_state.watchdogMs = uint32_t(reader.GetInt("diagWatchdogMs", 3000));
		g_state.disableDepthHooks = reader.GetBool("diagNoDepthHooks", false);
		g_state.noPresentHook = reader.GetBool("diagNoPresentHook", false);
		g_state.noUiPresent = reader.GetBool("diagNoUiPresent", false);
		g_state.stallAtPresent = uint32_t(reader.GetInt("diagStallAtPresent", 0));
		g_state.holdSwapChainRef = reader.GetBool("diagHoldSwapChainRef", false);
		g_state.allowNgxCoexist = reader.GetBool("diagAllowNgxCoexist", false);
		g_state.eavesdrop = reader.GetBool("diagEavesdrop", true);
		// FG 检测的 BufferCount 阈值（见 State 里字段说明）。可调：误判→调大，漏检→调小。
		wchar_t fgHostPath[MAX_PATH]{};
		const DWORD fgHostLength = GetModuleFileNameW(nullptr, fgHostPath, MAX_PATH);
		const unsigned fgDefault = DefaultFgBufferThreshold(fgHostLength && fgHostLength < MAX_PATH
			? std::wstring_view(fgHostPath, fgHostLength) : std::wstring_view{});
		g_state.fgBufferCountThreshold = NormalizeFgBufferThreshold(
			reader.GetFloat("fgBufferCountThreshold", float(fgDefault)), fgDefault);
		// 阈值也同步给旁听：OnModuleLoaded 里"FG 模块晚于 swapchain 加载"的延迟检测要用。
		NgxEavesdrop::Get().SetFgBufferCountThreshold(g_state.fgBufferCountThreshold);
        D5_LOG_INFO(L"FG detection threshold: %u buffers (game default=%u; startup setting; loaded FG module also required)",
            g_state.fgBufferCountThreshold, fgDefault);
		g_state.selfTestAtEvaluate = reader.GetInt("diagSelfTestAtEvaluate", 0);
		// 第 N 帧把处理前后的真实像素值抄回来打进日志。默认 0 = 关。
		// 这是分辨"输入量程不对"和"滤镜输出了个常数"的唯一办法 —— 画面上两者一样。
		g_state.dumpPixelsAt = uint32_t(reader.GetInt("diagNrDumpPixels", 0));
		g_state.fakeMotionAt = uint32_t(reader.GetInt("diagFakeMotion", 0));
		// **夹具：第 N 次 present 主动把设备干掉。**
		//
		// 用来验证"设备挂了之后那份转储到底会不会打、打出来长什么样"。
		// 上一版加 DRED 的时候我差点又只验证了"开关打开了"这一半 ——
		// 转储路径本身从来没跑过一次。**诊断代码也必须被测。**
		g_state.removeDeviceAt = uint32_t(reader.GetInt("diagRemoveDevice", 0));
		if (g_state.removeDeviceAt) {
			D5_LOG_WARN(L"诊断夹具：第 %u 次 present 时主动移除设备（测转储路径）",
				g_state.removeDeviceAt);
		}
		// **夹具：第 N 次 present 时假装"这个游戏无视了我们报的尺寸"。**
		//
		// 真触发条件（游戏的主深度比代理大得多）只在 RE Engine 那类引擎上成立，
		// 夹具自己是老实按 swapchain 尺寸渲染的 —— 也就是说撤代理那条恢复路径
		// （禁用代理 + 另起线程 nudge 窗口 + 等游戏重建 backbuffer）在真游戏上
		// 第一次跑就是玩家在跑。它还涉及跨线程动窗口，是能把游戏卡死的那类操作。
		// 所以给它一个能在夹具上按下去的按钮。
		g_state.banProxyAt = uint32_t(reader.GetInt("diagBanProxyAt", 0));
		if (g_state.banProxyAt) {
			D5_LOG_WARN(L"诊断夹具：第 %u 次 present 时假装代理尺寸被无视，走撤代理恢复",
				g_state.banProxyAt);
		}
		if (g_state.fakeMotionAt) {
			D5_LOG_WARN(L"诊断夹具：第 %u 次 present 起用假矢量（1286x724）驱动重采样",
				g_state.fakeMotionAt);
		}
		if (g_state.dumpPixelsAt) {
			D5_LOG_WARN(L"诊断夹具：DLSS5 处理到第 %u 帧时回读像素值",
				g_state.dumpPixelsAt);
		}
	if (g_state.selfTestAtEvaluate) {
			D5_LOG_WARN(L"诊断夹具：第 %d 次 present 时会用假资源把 evaluate 点那条路"
				L"走一遍，让 D3D12 调试层挑 barrier/格式的错%s",
				g_state.selfTestAtEvaluate < 0 ? -g_state.selfTestAtEvaluate
					: g_state.selfTestAtEvaluate,
				g_state.selfTestAtEvaluate < 0
					? L"（负数：故意谎报状态，验证夹具本身抓得住错）" : L"");
		}
		if (g_state.holdSwapChainRef) {
			D5_LOG_WARN(L"诊断夹具：会 AddRef 持住 swapchain —— 这会让游戏改画质时"
				L"再也建不出新 swapchain（故意复现已修的冻屏 bug）");
		}
		if (g_state.stallAtPresent) {
			D5_LOG_WARN(L"诊断夹具：第 %u 次 present 时会故意卡 10 秒",
				g_state.stallAtPresent);
		}
		if (g_state.disableDepthHooks) {
			D5_LOG_WARN(L"诊断：depth hook 被 diagNoDepthHooks 关掉了 —— "
				L"原生深度不可用，这是刻意的");
		}
	}

	NrSettings nr;
	nr.enabled = reader.GetBool("dlss5Enable", false);
	nr.preset = reader.GetInt("nrPreset", 0);
	// 默认 2 = Cinematic（用户拍板的新游戏默认：电影风格）。
	nr.style = reader.GetInt("nrStyle", 2);
	nr.intensity = reader.GetFloat("nrIntensity", 1.0f);
	nr.localTone = reader.GetFloat("nrLocalTone", 1.0f);
	nr.colourStrength = reader.GetFloat("nrColourStrength", 1.0f);
	if (!std::isfinite(nr.colourStrength)) nr.colourStrength = 1.0f;
	nr.colourStrength = std::clamp(nr.colourStrength, 0.0f, 1.0f);
	nr.selfLayers = reader.GetFloat("nrSelfLayers", 1.0f);
	nr.selfLayers = std::isfinite(nr.selfLayers) ? std::clamp(nr.selfLayers, 1.0f, 3.0f) : 1.0f;
	nr.trueLayers = std::clamp(reader.GetInt("nrTrueLayers", 1), 1, 5);
	nr.localStructure = reader.GetFloat("nrLocalStructure", 1.0f);
	// 默认 0.6（用户拍板的新游戏默认）；-1 是旧默认（"不干预"），别再当默认用。
	nr.skinStructure = reader.GetFloat("nrSkinStructure", 0.6f);
	// 两个遮罩默认开（用户拍的板）：语义遮罩是画质的主力，UI 修正开着无副作用。
	nr.autoMask = reader.GetBool("nrAutoMask", true);
	nr.uiCorrection = reader.GetBool("nrUiCorrection", true);
	// 白点：多少线性数值算漫反射白。编码时除以它。见 NrSettings::toneScale。
	// <= 0 = 自动（游戏曝光 → 场景亮度 → 保底 4.0）。手动白点已废除，不再暴露给玩家。
	nr.toneScale = reader.GetFloat("nrToneScale", 0.0f);
	// Only the legacy pure-gamma encoder uses this exponent; <=1 disables it.
	nr.toneGamma = reader.GetFloat("nrToneGamma", 2.2f);
	// Default Hybrid+sRGB; true selects the legacy clamp+power encoder.
	nr.pureGamma = reader.GetBool("nrPureGamma", false);
	// 诊断分半：跳过 DLSSNR 那次 EvaluateFeature，别的一步不改。
	// Output should match within texture quantization error.
	nr.skipNgx = reader.GetBool("diagNrSkipNgx", false);
	nr.skipWriteBack = reader.GetBool("diagNrSkipWriteBack", false);
	nr.dryRun = reader.GetBool("diagNrDryRun", false);
	// 降分辨率跑 DLSSNR。1.0 = 全分辨率。开销只跟像素数走（强度归零也照样掉帧），
	// 所以这是这条路上唯一能省性能的旋钮。
	nr.renderScale = reader.GetFloat("nrRenderScale", 1.0f);
	if (nr.dryRun) {
		D5_LOG_WARN(L"诊断：diagNrDryRun=1 —— 进 evaluate 钩子、做状态记账、"
			L"立刻还原走人，**一条 GPU 命令都不下**。画面必然毫无变化。");
	}
	if (nr.skipWriteBack) {
		D5_LOG_WARN(L"诊断：diagNrSkipWriteBack=1 —— 结果不写回游戏的颜色缓冲，"
			L"画面必然毫无变化。只用来定位崩溃。");
	}
	if (nr.skipNgx) {
		D5_LOG_WARN(L"诊断：diagNrSkipNgx=1 —— DLSSNR 不会真的跑，"
			L"这一档只用来定位崩溃，不要拿它评画质。");
	}
	// A/B 开关：单独关掉真矢量 / 真深度，用来判断画面变化到底来自哪一路
	nr.useRealMotion = reader.GetBool("nrUseRealMotion", true);
	nr.opticalFlow = reader.GetBool("nrOpticalFlow", true);
	nr.opticalQuality = std::clamp(reader.GetInt("nrOpticalFlowQuality", 1), 0, 2);
	nr.useRealDepth = reader.GetBool("nrUseRealDepth", true);
	// debug 视图：0=关 1=深度 2=矢量。开着时不跑 DLSSNR，画面就是那张假彩色图。
	{
		const int view = reader.GetInt("nrDebugView", 0);
		nr.debugView = view == 1 ? NrSettings::DebugView::Depth
			: view == 2 ? NrSettings::DebugView::Motion
			: view == 3 ? NrSettings::DebugView::Encoded
			: view == 4 ? NrSettings::DebugView::Diff
			: NrSettings::DebugView::Off;
	}
	nr.debugGain = reader.GetFloat("nrDebugGain", 0.0f);
	const auto unit = [](float v) { return std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 1.0f; };
	nr.controlMask = reader.GetBool("nrControlMask", false);
	nr.controlMaskR = unit(reader.GetFloat("nrControlMaskR", 1));
	nr.controlMaskG = unit(reader.GetFloat("nrControlMaskG", 1));
	nr.controlMaskB = unit(reader.GetFloat("nrControlMaskB", 1));
	nr.controlMaskA = unit(reader.GetFloat("nrControlMaskA", 1));
	g_state.semanticAvailable = SemanticExtensionInstalled(SemanticModuleFolder(g_state.selfModule));
    nr.semanticMask = g_state.semanticAvailable && reader.GetBool("nrSemanticMask", false);
	for (int g = 0; g < SEM_GROUP_COUNT; ++g) {
		char key[32]; snprintf(key, sizeof key, "nrSemInt%d", g);
		nr.semanticIntensity[g] = unit(reader.GetFloat(key, 0.0f));
	}
	nr.semanticEnabled = uint32_t(reader.GetInt("nrSemOn", 1)) & SEM_OBJECT_GROUP_MASK;
	nr.semanticBgIntensity = unit(reader.GetFloat("nrSemBgInt", 1));
	nr.semanticDebugView = reader.GetBool("nrSemanticDebugView", false);
    nr.semanticFlipY = reader.GetBool("nrSemanticFlipY", false);
    nr.semanticFeather = std::clamp(reader.GetFloat("nrSemanticFeather", 8.0f), 0.0f, 8.0f);
    if (!std::isfinite(nr.semanticFeather)) nr.semanticFeather = 8.0f;
	g_state.nrSettings = nr;

	// evaluate 点那条路要求旁听也开着 —— 没有旁听就没有接入点。这里不静默降级，
	// 明确说清楚，否则用户会以为开关生效了而画面没变是"没效果"。
	const bool wantAtEvaluate = reader.GetBool("dlss5AtEvaluate", true);
	// 保存原始意图（落盘时写回，不能被 canAtEvaluate 污染）。见 State.wantAtEvaluate。
	g_state.wantAtEvaluate = wantAtEvaluate;
	const bool canAtEvaluate = wantAtEvaluate && g_state.eavesdrop;
	if (wantAtEvaluate && !canAtEvaluate) {
		D5_LOG_WARN(L"dlss5AtEvaluate 开着但条件不足（旁听=%d DLSS5=%d）—— "
			L"回到 present 路径。evaluate 点那条路必须同时开旁听和 DLSS5。",
			g_state.eavesdrop ? 1 : 0, nr.enabled ? 1 : 0);
	}
	if (canAtEvaluate != g_state.nrAtEvaluate) {
		// **这条路的现状要说全，包括代价。** "present（拿不到能用的矢量）"这句话
		// 现在是错的：矢量能拿到（旁听 + 重采样），拿不到的是"低分辨率"。
		D5_LOG_INFO(L"DLSS5 处理点：%s", canAtEvaluate
			? L"SR→NR：游戏超分之后、FG 之前（输出分辨率，GPU fence 保护）"
			: L"**游戏的 DLSS 之后**（present 上处理 backbuffer）。"
			  L"矢量/深度走旁听 + 重采样，是真的；代价是①按输出分辨率算，"
			  L"DLSSNR 比在渲染分辨率上贵（1920x1080 对 1286x724 是 2.2 倍像素）"
			  L"②HUD 已经合上去了，会被一起滤到");

	}
	g_state.nrAtEvaluate = canAtEvaluate;
	NgxEavesdrop::Get().SetEvaluateMode(canAtEvaluate);
	g_state.nrAutoRoute = reader.GetBool("nrAutoRoute", true);
	NrRouteProbe::Get().Enable(canAtEvaluate && g_state.nrAutoRoute);

	// 像素回读在这里 arm，**两条路共用** —— 这样 backbuffer 那条路（画面是好的）和
	// evaluate 那条路（画面是灰的）能量出来直接对比。改一次设置就重新 arm 一次。
	// ArmPixelDump 只写三个普通字段，和 nrAtEvaluate 同一类的跨线程读写，诊断可接受。
	if (g_state.dumpPixelsAt) {
		g_state.nrFilter.ArmPixelDump(g_state.dumpPixelsAt);
	}

	D5_LOG_INFO(L"设置: dlss5Enable=%d preset=%d style=%d 强度=%.2f "
		L"局部色调=%.2f 局部结构=%.2f 皮肤结构=%.2f atEvaluate=%d "
		L"真矢量=%d 真深度=%d 曲线=%s debug视图=%s",
		nr.enabled ? 1 : 0, nr.preset, nr.style, nr.intensity,
		nr.localTone, nr.localStructure, nr.skinStructure,
		canAtEvaluate ? 1 : 0,
		nr.useRealMotion ? 1 : 0, nr.useRealDepth ? 1 : 0,
		nr.pureGamma ? L"clamp+power" : L"Hybrid+sRGB",
		nr.debugView == NrSettings::DebugView::Depth ? L"深度"
			: nr.debugView == NrSettings::DebugView::Motion ? L"矢量"
			: nr.debugView == NrSettings::DebugView::Encoded ? L"编码后的输入"
			: nr.debugView == NrSettings::DebugView::Diff ? L"差值" : L"关");
}

/* ============================ 工具 ============================ */

// 往 vtable 里换一个函数指针，返回原来的那个
//
// **成功的补丁要登记（RecordVtablePatch）。** 游戏退出时 RequestTeardown 会
// 反序把原函数写回这些槽位 —— 街霸6退出崩溃的根治（见 HookTeardown.h 顶部）：
// 没有这一步的话，teardown 撞上还挂着的钩子就是 0xC0000005。
void* PatchVTable(void* instance, size_t index, void* replacement) noexcept {
	void** vtable = *reinterpret_cast<void***>(instance);
	DWORD oldProtect = 0;
	if (!VirtualProtect(&vtable[index], sizeof(void*),
		PAGE_READWRITE, &oldProtect)) {
		D5_LOG_ERROR(L"VirtualProtect on vtable[%zu] failed: %lu",
			index, GetLastError());
		return nullptr;
	}
	void* original = vtable[index];
	vtable[index] = replacement;
	VirtualProtect(&vtable[index], sizeof(void*), oldProtect, &oldProtect);
	RecordVtablePatch(&vtable[index], original);
	return original;
}

/* ==================== command queue 的迟到兜底捕获 ==================== */

// 正常路径是 hook CreateSwapChainForHwnd 拿 pDevice。但注入真实游戏时，swapchain
// 早就建好了，那条路已经过去了 —— 而 IDXGISwapChain::GetDevice 给的是
// ID3D12Device，拿不到队列。所以再加一条路：ID3D12CommandQueue 的 vtable 也是
// 全进程共享的，用游戏自己的 device 造一个临时队列读出 vtable 打上补丁，之后
// 游戏任何队列的 ExecuteCommandLists 都会经过我们。
void STDMETHODCALLTYPE HookedExecuteCommandLists(
	ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
	if (IsTeardownRequested()) {
		g_originalExecuteCommandLists(queue, count, lists);
		return;
	}
	// Retain the candidate while the caller guarantees its lifetime. Present
	// may consume it after this thread has released its last game-owned reference.
	g_state.directQueueCandidate.Observe(queue);
	g_originalExecuteCommandLists(queue, count, lists);
	EvaluateGpuGate::Get().Submitted(queue, count, lists);
	NrRouteProbe::Get().Submitted(queue, count, lists);
	g_state.segMask.NotifySubmitted(queue, count, lists);
}

void InstallQueueHook(ID3D12Device* device) noexcept {
	if (g_state.queueHookInstalled || !device) return;
	if (!g_state.directQueueCandidate.SetDevice(device)) return;
	g_state.queueHookInstalled = true;   // 无论成败都只试一次

	D3D12_COMMAND_QUEUE_DESC desc{};
	desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	ID3D12CommandQueue* probe = nullptr;
	if (FAILED(device->CreateCommandQueue(&desc, IID_PPV_ARGS(&probe))) || !probe) {
		D5_LOG_WARN(L"无法创建探测用队列，迟到捕获不可用");
		return;
	}
	g_originalExecuteCommandLists =
		reinterpret_cast<ExecuteCommandListsFn>(PatchVTable(
			probe, VT_QUEUE_EXECUTE_COMMAND_LISTS, &HookedExecuteCommandLists));
	probe->Release();
	EvaluateGpuGate::Get().SetSubmissionHookReady(g_originalExecuteCommandLists != nullptr);
	NrRouteProbe::Get().SetSubmissionHookReady(g_originalExecuteCommandLists != nullptr);

	if (g_originalExecuteCommandLists) {
		D5_LOG_INFO(L"ExecuteCommandLists hook 已装：%p",
			g_originalExecuteCommandLists);
	}
}

// Bootstrap independently of the NR route. Native SR can be active before NR
// has initialized; waiting for a Present NR frame creates an initialization
// cycle. This candidate must never be installed as a chain's UI/FG queue.
void AdoptFallbackQueue() noexcept {
	if (g_state.capturedQueue || !g_state.device12) return;
	ID3D12CommandQueue* queue = g_state.directQueueCandidate.Acquire();
	if (!queue) return;
	g_state.capturedQueue = queue;
	D5_LOG_INFO(L"D3D12 initialization queue adopted: %p (same-device DIRECT submission; not a presentation-queue claim)",
		queue);
}

/* ============ 让游戏自己重建 swapchain（真超分的迟到补救） ============ */

// 真超分要求我们在游戏建渲染资源之前就把代理 backbuffer 交给它。迟到注入时那一刻
// 已经过去了，只能等游戏下一次 ResizeBuffers / 重建 swapchain。
//
// 我们没法代替游戏调 ResizeBuffers —— 那要求先释放所有 backbuffer 引用，而引用在
// 游戏手里。唯一能从外部触发的办法：**改一下游戏窗口的尺寸**，游戏收到 WM_SIZE
// 之后自己去 ResizeBuffers，然后我们把尺寸改回去。
//
// 这是启发式的，明确的失败情形：
//   · 独占全屏 —— 改窗口尺寸没意义甚至会引起模式切换，直接跳过
//   · 不响应 WM_SIZE 的游戏 —— 什么也不会发生，只是没生效，不会更坏
// 目标分辨率取自窗口客户区（见 SetupScaler），所以"临时改小再改回来"的最终状态
// 自动是对的。
bool TryNudgeWindowForResize(bool wantScaler = true) noexcept {
	// 建代理这个方向要压住"注入太晚"那条提示：nudge 期间代理确实还没建起来，
	// 但那是暂时的，说出去就是假话（而那条提示是一次性的，不会自己撤）。
	struct NudgeFlag {
		bool owned;
		explicit NudgeFlag(bool want) : owned(want) {
			if (owned) {
				g_state.proxyNudgeInFlight.store(true, std::memory_order_release);
			}
		}
		~NudgeFlag() {
			if (owned) {
				g_state.proxyNudgeInFlight.store(false, std::memory_order_release);
			}
		}
	} nudgeFlag(wantScaler);

	// 用 present 时抄下来的普通值，不碰 swapchain 对象本身 —— 持有它的引用会让
	// 游戏再也建不出新的 swapchain（见 State::trackedWindow 的说明）。
	const HWND window = g_state.trackedWindow;
	if (!window) {
		D5_LOG_WARN(L"还没见到 swapchain，无法触发重建");
		return false;
	}
	if (!g_state.trackedWindowed.load(std::memory_order_relaxed)) {
		D5_LOG_WARN(L"游戏在独占全屏，不能改窗口尺寸来触发重建。"
			L"请在游戏里改一次分辨率，或用 --wait 在启动前注入。");
		return false;
	}
	if (!IsWindow(window)) return false;

	RECT rect{};
	if (!GetWindowRect(window, &rect)) return false;
	const int width = rect.right - rect.left;
	const int height = rect.bottom - rect.top;
	if (width <= 8 || height <= 8) return false;

	D5_LOG_INFO(L"临时把窗口从 %dx%d 改小 2px 再改回，触发游戏重建 swapchain",
		width, height);

	// 只动尺寸，不动位置和 Z 序。
	// **不能用 SWP_ASYNCWINDOWPOS**：两次调用（改小、改回）会被排队，第二次可能
	// 在第一次被处理之前就到，窗口直接跳到最终尺寸 —— 净变化为零，游戏根本收不到
	// WM_SIZE。实测就是这样时好时坏的。
	const UINT flags =
		SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOOWNERZORDER;

	// 等某个条件成立，超时返回 false
	auto waitFor = [](auto predicate, int maxMilliseconds) {
		for (int elapsed = 0; elapsed < maxMilliseconds; elapsed += 25) {
			if (predicate()) return true;
			Sleep(25);
		}
		return predicate();
	};
	auto clientHeight = [window]() -> int {
		RECT r{};
		return GetClientRect(window, &r) ? int(r.bottom - r.top) : -1;
	};
	// 等的是"代理变成期望的状态"。**两个方向都要支持**：正常情况我们 nudge 是为了
	// 把代理建起来（wantScaler=true）；代理被证明不适用、要撤掉时是反方向
	// （wantScaler=false，见 BanProxyAndRecover）。写死成"等它变 true"的话，
	// 撤代理那次永远等到超时，然后打一条完全误导人的"这个游戏可能不响应 WM_SIZE"。
	auto scalerReady = [wantScaler]() {
		return g_state.scalerActive.load(std::memory_order_acquire) == wantScaler;
	};

	const int heightBefore = clientHeight();
	if (!SetWindowPos(window, nullptr, 0, 0, width, height - 2, flags)) {
		D5_LOG_WARN(L"SetWindowPos 失败: %lu", GetLastError());
		return false;
	}
	// 先确认窗口真的变小了，再确认游戏已经跟着重建过（这一步建出来的代理是
	// 按缩小后的尺寸算的，不要紧 —— 下面改回去会重建成正确的）
	if (!waitFor([&] { return clientHeight() != heightBefore; }, 1000)) {
		D5_LOG_WARN(L"窗口尺寸没有实际变化，无法触发游戏重建");
		return false;
	}
	waitFor(scalerReady, 2000);

	// 改回原尺寸，游戏再重建一次，这次目标分辨率是对的
	SetWindowPos(window, nullptr, 0, 0, width, height, flags);
	waitFor([&] { return clientHeight() == heightBefore; }, 1000);
	// 游戏处理 WM_SIZE 的延迟差别很大（要等 GPU 空闲、放引用、重建），
	// 实测有超过 1 秒的，所以给足时间
	const bool ok = waitFor(scalerReady, 3000);
	if (ok) {
		D5_LOG_INFO(wantScaler ? L"触发成功，真超分已生效"
			: L"触发成功，代理已撤掉，画面回到游戏自己的分辨率");
	} else {
		D5_LOG_WARN(L"改完窗口尺寸后代理仍未%s —— 这个游戏可能不响应 WM_SIZE。"
			L"请在游戏里手动改一次分辨率，或用 --wait 在启动前注入。",
			wantScaler ? L"建立" : L"撤掉");
	}
	return ok;
}

// 代理这条路对这个游戏不成立时的自救。
//
// **代理不能直接 Release** —— 游戏还持着从我们这儿拿到的那些 backbuffer 和
// 它们的 RTV。但游戏自己重建 backbuffer 时**必须**先放掉（DXGI 的 ResizeBuffers
// 就是这么规定的），所以只要让它重建一次，代理就能安全地换回真 backbuffer。
// 触发重建用的是和"真超分要生效"完全同一条路：临时改一下窗口尺寸。
//
// **必须另起一个线程。** 调用点在 present 里（游戏的渲染线程），而 SetWindowPos
// 要等窗口拥有者线程处理消息 —— 游戏的消息线程此刻很可能正等着渲染线程，
// 在这里直接调就是互锁。nudge 本身还要睡好几秒，更不能占着 present。
void BanProxyAndRecover() noexcept {
	if (g_state.proxyBanned.exchange(true)) return;
	D5_LOG_WARN(L"代理已对本进程永久禁用，正在让游戏重建 backbuffer 以恢复画面。");
	const HANDLE thread = CreateThread(nullptr, 0, [](LPVOID) -> DWORD {
		// 让当前这一帧先走完，别和 present 抢窗口消息
		Sleep(200);
		for (int attempt = 1; attempt <= 3; ++attempt) {
			if (TryNudgeWindowForResize(false)) return 0;
			if (!g_state.scalerActive.load(std::memory_order_acquire)) return 0;
			Sleep(300);
		}
		D5_LOG_ERROR(L"撤代理失败：游戏没有重建 backbuffer，画面会一直是左上角裁切。"
			L"请在游戏里手动改一次分辨率，或重启游戏并改用 DLAA。");
		g_publisher.Publish(
			"撤代理失败 —— 请在游戏里改一次分辨率，或重启游戏并改用 DLAA。");
		return 0;
	}, nullptr, 0, nullptr);
	if (thread) {
		CloseHandle(thread);
	} else {
		D5_LOG_ERROR(L"撤代理的线程创建失败（%lu）", GetLastError());
	}
}

// A Vulkan presentation layer can expose a DXGI chain backed by D3D12. Classify
// the actual chain on every presentation; an NGX export lookup says nothing
// about which device owns this backbuffer.
static constexpr GUID NR_PRESENT_QUEUE = {0x30478f2c,0xf6c4,0x4388,{0xa9,0x66,0x0d,0xe8,0x63,0x7e,0xb5,0xa1}};
bool SameComObject(IUnknown* left, IUnknown* right) noexcept {
    if (left == right) return true;
    if (!left || !right) return false;
    IUnknown* a = nullptr; IUnknown* b = nullptr;
    left->QueryInterface(IID_PPV_ARGS(&a));
    right->QueryInterface(IID_PPV_ARGS(&b));
    const bool same = a && a == b;
    if (a) a->Release();
    if (b) b->Release();
    return same;
}
bool AcceptDxgiCreation(IUnknown* candidate, HWND window, UINT width, UINT height) noexcept {
    std::lock_guard<std::recursive_mutex> lock(g_nrStateMutex);
    if (!AcceptDxgiPresentation(g_state.api, window, width, height)) return false;
    if (!g_state.device12 && !g_state.device11) return true;
    // Reject unrelated devices before factory-side scaler/capture state changes,
    // just as Present rejects their backbuffers before NR or UI submission.
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Device* device12 = nullptr;
    ID3D11Device* device11 = nullptr;
    if (candidate && SUCCEEDED(candidate->QueryInterface(IID_PPV_ARGS(&queue)))) {
        queue->GetDevice(IID_PPV_ARGS(&device12));
        queue->Release();
    } else if (candidate) {
        candidate->QueryInterface(IID_PPV_ARGS(&device11));
    }
    const bool accepted =
        (device12 && g_state.device12 && SameComObject(device12, g_state.device12)) ||
        (device11 && g_state.device11 && SameComObject(device11, g_state.device11));
    if (device12) device12->Release();
    if (device11) device11->Release();
    return accepted;
}

bool DetectApiFromSwapChain(IDXGISwapChain* swapChain) noexcept {
    std::lock_guard<std::recursive_mutex> lock(g_nrStateMutex);
	// 尺寸每帧都要更新。原来只在首次探测时读一次，结果玩家在游戏里改了分辨率之后
	// UI 上还显示改之前的值 —— 而这一行恰恰是玩家用来判断真超分有没有生效的。
	// 走原始的 GetDesc，绕开我们自己给代理尺寸打的谎。
	//
	// 顺便把窗口和"是不是独占全屏"抄成普通值。
	// **绝对不能 AddRef 这个 swapchain。**详见 State::trackedWindow 上面那段。
    DXGI_SWAP_CHAIN_DESC current{};
    const auto getDesc = SwapOriginal(swapChain, VT_SWAPCHAIN_GET_DESC, g_originalGetDesc);
    if (!getDesc || FAILED(getDesc(swapChain, &current)) ||
        !AcceptDxgiPresentation(g_state.api, current.OutputWindow,
            current.BufferDesc.Width, current.BufferDesc.Height)) return false;

	// 夹具：故意复现上一版的引用泄漏
	if (g_state.holdSwapChainRef && g_state.diagHeldSwapChain != swapChain) {
		if (g_state.diagHeldSwapChain) g_state.diagHeldSwapChain->Release();
		swapChain->AddRef();
		g_state.diagHeldSwapChain = swapChain;
	}

    ID3D12Device* incoming12 = nullptr;
    ID3D11Device* incoming11 = nullptr;
    swapChain->GetDevice(IID_PPV_ARGS(&incoming12));
    if (!incoming12) swapChain->GetDevice(IID_PPV_ARGS(&incoming11));
    const bool otherDevice =
        (g_state.device12 && !SameComObject(g_state.device12, incoming12)) ||
        (g_state.device11 && !SameComObject(g_state.device11, incoming11));
    if (otherDevice) {
        // Filter textures, NGX features and the Evaluate fence belong to one
        // device. Reusing them on another device is invalid. Keep the existing
        // resources alive and forward this chain until explicit retirement is
        // implemented; a later Present on the original device can resume.
        static IUnknown* lastRejectedDevice = nullptr; // diagnostic identity only
        IUnknown* incoming = incoming12 ? static_cast<IUnknown*>(incoming12) : incoming11;
        if (incoming != lastRejectedDevice) {
            lastRejectedDevice = incoming;
            D5_LOG_WARN(L"Presentation device changed: chain=%p old12=%p new12=%p old11=%p new11=%p; forwarding without old-device NR resources",
                swapChain, g_state.device12, incoming12, g_state.device11, incoming11);
            g_publisher.Publish("呈现设备已重建：等待兼容设备，当前帧保留游戏原始画面");
        }
        if (incoming12) incoming12->Release();
        if (incoming11) incoming11->Release();
        return false;
    }
    if (!incoming12 && !incoming11) return false;
    if (!g_state.device12 && incoming12) { g_state.device12 = incoming12; incoming12 = nullptr; }
    if (!g_state.device11 && incoming11) { g_state.device11 = incoming11; incoming11 = nullptr; }
    if (incoming12) incoming12->Release();
    if (incoming11) incoming11->Release();

    ID3D12CommandQueue* presentQueue = nullptr;
    UINT queueBytes = sizeof(presentQueue);
    if (g_state.device12 && SUCCEEDED(swapChain->GetPrivateData(NR_PRESENT_QUEUE,
            &queueBytes, &presentQueue)) && presentQueue) {
        ID3D12Device* queueDevice = nullptr;
        presentQueue->GetDevice(IID_PPV_ARGS(&queueDevice));
        const bool compatible = SameComObject(queueDevice, g_state.device12) &&
            presentQueue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT;
        if (queueDevice) queueDevice->Release();
        if (!compatible) { presentQueue->Release(); return false; }
        if (!SameComObject(presentQueue, g_state.capturedQueue)) {
            if (g_state.nrInitialized && (!EvaluateGpuGate::Get().IsIdle() ||
                    !g_state.nrFilter.SetPresentQueue(presentQueue))) {
                presentQueue->Release(); return false;
            }
            D5_LOG_INFO(L"Presentation queue adopted: chain=%p old=%p new=%p device=%p",
                swapChain, g_state.capturedQueue, presentQueue, g_state.device12);
            if (g_state.capturedQueue) g_state.capturedQueue->Release();
            g_state.capturedQueue = presentQueue; presentQueue = nullptr;
            NgxEavesdrop::Get().NoteSwapChainRebuilt();
        }
        if (presentQueue) presentQueue->Release();
    }

    // Queue initialization must run even while the native-SR/FG route bypasses
    // RunFilters. The factory-recorded presentation queue takes precedence.
    AdoptFallbackQueue();

    // Ownership was accepted for both the window and its actual device/queue.
    // Rejected auxiliary chains must not redirect input or the watchdog.
    g_state.width = current.BufferDesc.Width;
    g_state.height = current.BufferDesc.Height;
    g_state.trackedWindow = current.OutputWindow;
    FreezeWatchdog::Get().SetWatchedWindow(current.OutputWindow);
    g_state.trackedWindowed.store(current.Windowed != FALSE, std::memory_order_relaxed);

	if (g_state.apiLogged) return true;

	DXGI_SWAP_CHAIN_DESC desc{};
	if (SUCCEEDED(swapChain->GetDesc(&desc))) {
		g_state.width = desc.BufferDesc.Width;
		g_state.height = desc.BufferDesc.Height;
	}

	// GetDevice 对 D3D12 交换链返回 ID3D12Device，对 D3D11 返回 ID3D11Device。
	// device12 可能已经在 HookedCreateSwapChainForHwnd 里从队列拿到了，别覆盖掉
	// 那个引用。
	if (g_state.device12) {
		g_state.api = GraphicsApi::D3D12;
	}

	if (g_state.device11) {
		g_state.api = GraphicsApi::D3D11;
		ChainInjectStopAfterGraphics();
	}
	if (g_state.device12) {
		// 拿到 D3D12 设备 = 本进程就是渲染进程，不是启动壳。
		// 之后它拉起的子进程（崩溃上报器之类）不该再被注入。
		ChainInjectStopAfterGraphics();
		FreezeWatchdog::Get().SetDevice(g_state.device12);
		// 旁听要用它建副本纹理
		NgxEavesdrop::Get().SetDevice(g_state.device12);
		D3D12Validation::Attach(g_state.device12);
		EvaluateGpuGate::Get().Initialize(g_state.device12);
		InstallQueueHook(g_state.device12);
		// FG can bypass OnPresent from the first frame. Native SR still needs
		// command-list state tracking before its NR callback can be admitted.
		TryInstallDepthTracker();
	}

	const wchar_t* apiName =
		g_state.api == GraphicsApi::D3D12 ? L"D3D12" : g_state.api == GraphicsApi::D3D11 ? L"D3D11" : L"unknown";
	D5_LOG_INFO(L"present hooked: api=%s %ux%u format=%u queue=%p",
		apiName, g_state.width, g_state.height,
		(unsigned)desc.BufferDesc.Format, g_state.capturedQueue);
	D5_LOG_INFO(L"游戏的 swapchain: this=%p vtable=%p", swapChain,
		*reinterpret_cast<void**>(swapChain));

	if (g_state.api == GraphicsApi::D3D12 && !g_state.capturedQueue) {
		// swapchain 早于我们的 hook 创建（注入真实游戏时的常态），或者走的是
		// CreateSwapChain 而不是 CreateSwapChainForHwnd。改走兜底路径。
		D5_LOG_INFO(L"CreateSwapChainForHwnd 没抓到队列，装 ExecuteCommandLists "
			L"hook 走兜底捕获");
		InstallQueueHook(g_state.device12);
	}

	char message[256]{};
	if (g_state.api == GraphicsApi::D3D12 || g_state.api == GraphicsApi::D3D11) {
		_snprintf_s(message, _TRUNCATE, "已接管 present：%ls %ux%u",
			apiName, g_state.width, g_state.height);
	} else {
		// 说清楚而不是静静地什么都不做 —— 用户看到"已注入"却毫无效果会以为是坏了
		_snprintf_s(message, _TRUNCATE,
			"已接管 present，但这个进程用的是 %ls，目前只支持 D3D11/D3D12，所有功能都不会生效",
			apiName);
	}
	g_state.apiLogged = true;
	g_publisher.Publish(message);
    return g_state.api == GraphicsApi::D3D12 || g_state.api == GraphicsApi::D3D11;
}

// 装深度探测。需要一条命令列表来做 vtable 自检 —— 建一条临时的就行，
// vtable 是全类共享的，补丁在对象释放后依然有效。
void TryInstallDepthTracker() noexcept {
	if (g_state.depthTrackerTried || !g_state.device12) return;
	g_state.depthTrackerTried = true;
	if (g_state.disableDepthHooks) return;
	D5_EVENT(DepthInstall);

	ID3D12CommandAllocator* allocator = nullptr;
	ID3D12GraphicsCommandList* list = nullptr;
	if (SUCCEEDED(g_state.device12->CreateCommandAllocator(
			D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) &&
		SUCCEEDED(g_state.device12->CreateCommandList(
			0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr,
			IID_PPV_ARGS(&list)))) {
		list->Close();
		list->SetName(L"D5Q.DepthProbe.List");
		GetDepthTracker().Install(g_state.device12, list);
	} else {
		D5_LOG_WARN(L"建不出自检用的命令列表，深度探测不安装");
	}
	if (list) list->Release();
	if (allocator) allocator->Release();
}

// 把候选列表打到日志。启发式必然有挑错的游戏，这份清单是排查的起点。
//
// 自带去重：**只有这张表的"实质内容"变了才真的打**。
// 判据刻意不含下标、也不含选中的指针 —— RE Engine 里几十个同尺寸候选评分打平，
// 选中的那个每帧都在换（下标和指针都在跳，尺寸/格式其实一直没变）。上一版按指针
// 判断，实测一次运行 4865 行日志里有 4300 行是这张表，真正重要的几十行全被冲掉了。
void LogDepthCandidates() noexcept {
	// FG 开启时彻底不打印 —— 深度追踪器已经退场（见 DepthTracker::FindOrAddCandidate
	// 的说明），候选表不再增长；这里再兜底一次，防止 FG 开启瞬间候选表残留的旧条目
	// 在 signature 变化时刷屏。
	if (NgxEavesdrop::Get().FrameGenerationActive(0)) return;
	DepthTracker& tracker = GetDepthTracker();
	DepthCandidate candidates[DepthTracker::MAX_CANDIDATES]{};
	const uint32_t count = tracker.Snapshot(
		candidates, DepthTracker::MAX_CANDIDATES);

	ID3D12Resource* const selected = tracker.Selected();
	uint32_t signature = count * 2654435761u;
	for (uint32_t i = 0; i < count; ++i) {
		const DepthCandidate& c = candidates[i];
		// 每个候选只按尺寸+格式参与签名，且**用异或累加**（与顺序无关）——
		// 同一批候选换了个排列顺序不算变化。
		signature ^= (c.width * 73856093u) ^ (c.height * 19349663u) ^
			(uint32_t(c.format) * 83492791u);
		if (c.resource && c.resource == selected) signature += 0x9E3779B9u;
	}
	static uint32_t lastSignature = 0;
	static bool everLogged = false;
	if (everLogged && signature == lastSignature) return;
	// **限流兜底**：换链/分辨率切换的过渡期（鬼武者启动 1080p↔4K 实测），选中状态
	// 可能每帧跳、签名每帧变，这里每帧打印会刷屏（同步 fflush）拖垮 present 线程。
	// 两次真正打印之间至少间隔 1 秒，正常游戏（换链才变）几乎不受影响。
	static ULONGLONG lastLogMs = 0;
	const ULONGLONG now = GetTickCount64();
	if (everLogged && (now - lastLogMs) < 1000) return;
	lastSignature = signature;
	lastLogMs = now;
	everLogged = true;

	if (!count) {
		D5_LOG_INFO(L"深度候选：还没发现任何深度缓冲");
		return;
	}
	for (uint32_t i = 0; i < count; ++i) {
		const DepthCandidate& c = candidates[i];
		D5_LOG_INFO(L"深度候选 #%u%s %ux%u fmt=%u 绑定=%u 清除=%u 状态=%s(0x%X)",
			i, (c.resource && c.resource == selected) ? L" [选中]" : L"      ",
			c.width, c.height, (unsigned)c.format,
			c.bindsLastFrame, c.clearsLastFrame,
			c.stateKnown ? L"已知" : L"未知", (unsigned)c.state);
	}
}

// 夹具：造一张假的"游戏运动矢量"。
//
// 为什么需要：矢量重采样那条路只有在游戏真的跑 DLSS 时才有输入，而测试目标不跑 DLSS。
// 没有它，那段 dispatch + barrier 就只能第一次上真游戏才被执行 —— 而未经调试层检验的
// GPU 代码正是这个项目栽过跟头的地方（上一次是 SetDescriptorHeaps 没还回去，直接闪退）。
//
// 尺寸刻意用 1286x724：和鬼武者实测的渲染分辨率一致，这样重采样比例也是真实的。
ID3D12Resource* CreateFakeMotion() noexcept {
	if (!g_state.device12) return nullptr;
	D3D12_HEAP_PROPERTIES heap{};
	heap.Type = D3D12_HEAP_TYPE_DEFAULT;
	heap.CreationNodeMask = 1;
	heap.VisibleNodeMask = 1;
	D3D12_RESOURCE_DESC desc{};
	desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	desc.Width = 1286;
	desc.Height = 724;
	desc.DepthOrArraySize = 1;
	desc.MipLevels = 1;
	desc.Format = DXGI_FORMAT_R16G16_FLOAT;
	desc.SampleDesc.Count = 1;
	desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
	// 常驻 NON_PIXEL_SHADER_RESOURCE —— 和旁听副本一样，重采样直接当 SRV 读
	ID3D12Resource* resource = nullptr;
	if (FAILED(g_state.device12->CreateCommittedResource(
			&heap, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, nullptr,
			IID_PPV_ARGS(&resource)))) {
		D5_LOG_ERROR(L"夹具：假矢量纹理建不出来");
		return nullptr;
	}
	resource->SetName(L"D5Q.Test.FakeMotion");
	D5_LOG_INFO(L"夹具：假矢量已建（1286x724 R16G16_FLOAT，内容未初始化 —— "
		L"这个夹具验的是 barrier/dispatch 合不合法，不是画面对不对）");
	return resource;
}

// 在游戏的 DLSS evaluate **之前**跑一次 DLSSNR，就地把它即将上采样的颜色处理掉。
//
// 这是"真矢量"唯一能真正用上的地方：这里颜色和运动矢量同分辨率（游戏的渲染分辨率），
// 而 present 那边颜色已经是 backbuffer 尺寸、矢量还是渲染尺寸，喂进去是错位鬼影。
// 顺带两个好处：颜色是 UI 合成之前的（不会再"增强"UI 文字）；处理完写回游戏的颜色
// 缓冲，效果自然流进它自己的 DLSS，等于免费蹭到它的时域累积。
//
// **跑在游戏的渲染线程上**，不是 present 线程。所以只有 g_state.nrAtEvaluate 为真
// 时才装这个 hook，而那一位同时也让 present 路径完全不碰 NR —— 二选一，不并发。
static void TryInitNr() noexcept;
static void ApplyPendingSettings() noexcept {
    if (!g_state.settingsDirty.load(std::memory_order_acquire)) return;
    std::lock_guard<std::recursive_mutex> lock(g_nrStateMutex);
    if (g_state.settingsDirty.exchange(false)) {
        D5_EVENT(ReloadSettings);
        ReloadSettings();
        UpdateSemanticMask();
    }
}
bool RunNrAtEvaluate(
	ID3D12GraphicsCommandList* list,
	const NgxEavesdropFrame& frame,
	uint32_t colorState,
	uint32_t motionState,
	uint32_t depthState) noexcept {
	std::lock_guard<std::recursive_mutex> nrLock(g_nrStateMutex);
    ApplyPendingSettings();
	// Count every confirmed-SR callback, including ordinary off/menu frames.
	// Bounded summaries distinguish admission holes from actual route pauses
	// without logging each frame or treating a loaded FG DLL as enabled FG.
	enum class Attempt { MasterOff, NrOff, Dormant, Initialize, Inputs, GpuGate, Prepare, Filter, Applied, Count };
	struct AttemptCounts { uint64_t calls = 0, reason[size_t(Attempt::Count)]{}; };
	static AttemptCounts attempts;
	Attempt outcome = Attempt::MasterOff;
	struct ReportAttempt {
		AttemptCounts& counts; Attempt& outcome; uint64_t frame;
		~ReportAttempt() {
			++counts.reason[size_t(outcome)];
			if (++counts.calls % 600 != 0) return;
			const auto* n = counts.reason;
			D5_LOG_INFO(L"NR callback continuity: SRframe=%llu calls=%llu recorded=%llu "
				L"masterOff=%llu nrOff=%llu dormant=%llu initialize=%llu inputs=%llu gpuGate=%llu prepare=%llu filterSkipped=%llu",
				(unsigned long long)frame, (unsigned long long)counts.calls,
				(unsigned long long)n[size_t(Attempt::Applied)], (unsigned long long)n[size_t(Attempt::MasterOff)],
				(unsigned long long)n[size_t(Attempt::NrOff)], (unsigned long long)n[size_t(Attempt::Dormant)],
				(unsigned long long)n[size_t(Attempt::Initialize)], (unsigned long long)n[size_t(Attempt::Inputs)],
				(unsigned long long)n[size_t(Attempt::GpuGate)], (unsigned long long)n[size_t(Attempt::Prepare)],
				(unsigned long long)n[size_t(Attempt::Filter)]);
		}
	} reportAttempt{attempts, outcome, frame.frameSeq};
	// **总开关必须在这里也生效。** 上一版漏了，后果是 Alt+D 和「断开」对这条路
	// 完全没反应 —— 玩家没有任何办法在游戏里把它关掉。present 路径在 OnPresent
	// 开头就检查了，这条路跑在另一个线程上，所以要单独查一次。
	if (!g_state.enabled.load(std::memory_order_relaxed)) return false;
	outcome = Attempt::NrOff;
	if (!g_state.nrAtEvaluate || !g_state.nrSettings.enabled) return false;
	outcome = Attempt::Dormant;
	if (g_state.nrAutoRoute && !NrRouteProbe::Get().UseEvaluate(GetTickCount64(),
		NgxEavesdrop::Get().FrameGenerationActive(0))) return false;
	outcome = Attempt::Initialize;
	TryInitNr();
	if (!g_state.nrInitialized) return false;
	outcome = Attempt::Inputs;
    // FG deliberately bypasses OnPresent/RunFilters. Semantic input capture
    // belongs to the active NR route and must also be wired in Evaluate.
    UpdateSemanticMask();
	// Eligibility comes from a successful native SR call and the GPU gate below.
	// Startup/camera-cut history resets are passed to NR, not counted as lost frames.
	// SR→NR：颜色是 SR 输出（frame.output），不是 SR 输入（frame.color）。
	if (!frame.output || !frame.outputWidth || !frame.outputHeight) return false;

	// Use the current native SR guides while their lifetime is guaranteed by this call.
	// Combined read states are valid; the SR->NR path neither copies nor transitions them.
	if (!frame.motionVectors) return false;

	// NR owns one serial layer chain/scratch set. Reuse it only after the previous list's
	// ACTUAL queue fence completes, before Prepare can release/rebuild anything.
	// Keep this after the enabled/eligibility checks: F8-off has no GPU wait.
	auto& gate = EvaluateGpuGate::Get();
	outcome = Attempt::GpuGate;
	if (!gate.Begin(list)) {
		static uint64_t skipped = 0;
		const auto n = ++skipped;
		if (n <= 5 || n % 600 == 0) {
			const auto s = gate.Snapshot();
			D5_LOG_WARN(L"NRFG admission skipped: frame=%llu total=%llu unsubmitted=%llu timeout=%llu unavailable=%llu errors=%llu",
				(unsigned long long)frame.frameSeq, (unsigned long long)n,
				(unsigned long long)s.unsubmitted, (unsigned long long)s.timeouts,
				(unsigned long long)s.unavailable, (unsigned long long)s.errors);
		}
		return false;
	}

	// 尺寸/格式跟着 SR 输出走。游戏改画质档时它会变，Prepare 自己会重建。
	// 第一次会同步创建 feature（实测约 1.8 秒，含加载 165MB 的 snippet）—— 那一下
	// 会顿在游戏的渲染线程上，只发生一次，先记下来别当成 bug。
	outcome = Attempt::Prepare;
	if (!g_state.nrFilter.Prepare(frame.outputWidth, frame.outputHeight,
		DXGI_FORMAT(frame.outputFormat), g_state.nrSettings,
		NrMode::AtEvaluate, g_state.nrAutoRoute)) {
		return false;
	}

	NrEvaluateInput input;
	input.color = frame.output;
	input.colorState = D3D12_RESOURCE_STATES(colorState);
	input.motion = frame.motionVectors;
	input.motionState = D3D12_RESOURCE_STATES(motionState);
	if (frame.depth) {
		input.depth = frame.depth;
		input.depthState = D3D12_RESOURCE_STATES(depthState);
	}
	input.depthInverted = frame.depthInverted;
	input.hasDepthInverted = frame.hasDepthInverted;
    input.colorHdrKnown = frame.colorHdrKnown;
    input.colorIsHdr = frame.colorIsHdr;
	// SR 输出是全尺寸（输出分辨率），子矩形就用输出尺寸。
	input.subrectWidth = frame.outputWidth;
	input.subrectHeight = frame.outputHeight;
	// 矢量/深度是渲染分辨率（DLSS 的输入），子矩形必须用渲染尺寸 —— 不能用输出
	// 尺寸。之前用输出分辨率（4K）灌了 MVec/Depth 的子矩形，模型按 4K 采样
	// 2228x1254 的矢量 → 时域累积错位 → 画面糊、效果弱。这里用游戏报的渲染
	// subrect，限制在矢量纹理尺寸内（动态分辨率时游戏可能只渲染进大纹理的一角）。
	{
		uint32_t guideW = frame.motionWidth;
		uint32_t guideH = frame.motionHeight;
		if (frame.renderWidth && frame.renderWidth < guideW) guideW = frame.renderWidth;
		if (frame.renderHeight && frame.renderHeight < guideH) guideH = frame.renderHeight;
		input.guideSubrectWidth = guideW;
		input.guideSubrectHeight = guideH;
	}
	input.mvScaleX = frame.mvScaleX;
	input.mvScaleY = frame.mvScaleY;
	input.jitterX = frame.jitterX;
	input.jitterY = frame.jitterY;
	input.preExposure = frame.preExposure;
	// 游戏曝光纹理 + 格式，用它算白点（"画面正中亮度"只代表局部，高光过曝）。
	input.exposure = frame.exposure;
	input.exposureFormat = frame.exposureFormat;
	uint32_t exposureState = 0;
	input.exposureStateKnown = NgxEavesdrop::Get().ResourceState(frame.exposure, exposureState);
	input.exposureState = D3D12_RESOURCE_STATES(exposureState);
	// 游戏说要重置时域累积，我们也跟着重置 —— 它比我们更清楚什么时候切了镜头
	// **另外：swapchain 重建（换链）时，DLSSNR 的时域历史已失效，必须强制 reset ——
	// 且要连续多帧 reset（TakeResetFrame 每次消耗一帧），单次 reset 不够清空 DLSSNR
	// 内部的时域历史，累积错乱跑十几秒后 DEVICE_HUNG。**
	input.reset = frame.reset != 0 || NgxEavesdrop::Get().TakeResetFrame();
	static uint64_t lastNrFrame = 0;
	input.reset = input.reset || (lastNrFrame && frame.frameSeq != lastNrFrame + 1);

	// 诊断：进场景/改分辨率时，深度/矢量/SR 输出的尺寸会变。变了就记下来，
	// 方便和崩溃现场（DEVICE_HUNG 卡在 DLSSNR dispatch）对齐——这决定了崩的是
	// 「状态写错」还是「尺寸不匹配」。
	{
		static uint32_t lastDepthW = 0, lastDepthH = 0;
		static uint32_t lastMotionW = 0, lastMotionH = 0;
		static uint32_t lastOutW = 0, lastOutH = 0;
		static uint32_t logged = 0;
		if (logged < 20 &&
			(frame.depthWidth != lastDepthW || frame.depthHeight != lastDepthH ||
			 frame.motionWidth != lastMotionW || frame.motionHeight != lastMotionH ||
			 frame.outputWidth != lastOutW || frame.outputHeight != lastOutH)) {
			++logged;
			D5_LOG_WARN(L"DLSS5@evaluate 尺寸变化：输出=%ux%u 矢量=%ux%u 深度=%ux%u"
				L"（guide=%ux%u）",
				frame.outputWidth, frame.outputHeight,
				frame.motionWidth, frame.motionHeight,
				frame.depthWidth, frame.depthHeight,
				input.guideSubrectWidth, input.guideSubrectHeight);
			lastDepthW = frame.depthWidth; lastDepthH = frame.depthHeight;
			lastMotionW = frame.motionWidth; lastMotionH = frame.motionHeight;
			lastOutW = frame.outputWidth; lastOutH = frame.outputHeight;
		}
	}

	outcome = Attempt::Filter;
	const bool ran = g_state.nrFilter.ExecuteOnList(list, input);
	if (ran) outcome = Attempt::Applied;
	if (!ran) {
		static uint64_t failed = 0;
		const auto n = ++failed;
		if (n <= 5 || n % 600 == 0) D5_LOG_WARN(
			L"NRFG filter did not execute: SRframe=%llu total=%llu block=%u NRframes=%llu outputFmt=%u",
			(unsigned long long)frame.frameSeq, (unsigned long long)n, g_state.nrFilter.LastBlock(),
			(unsigned long long)g_state.nrFilter.EvaluateCount(), frame.outputFormat);
	}
	if (ran) NrRouteProbe::Get().Applied(GetTickCount64());
	if (ran) {
		// FG may bypass RunFilters altogether. Clear a menu handoff hold here
		// only after real SR->NR recording succeeds, under the same NR lock.
		if (g_state.nrAutomaticHandoffBlocked) {
			g_state.nrAutomaticHandoffBlocked = false;
			D5_LOG_INFO(L"NR automatic Present handoff hold cleared: native SR->NR recording resumed");
			g_publisher.Publish("NR resumed: native SR->NR recording is active again.");
		}
		lastNrFrame = frame.frameSeq;
		static uint64_t reported = 0;
		if (reported++ == 0 || reported % 600 == 0) D5_LOG_INFO(
			L"NRFG direct route: SR->NR recorded=%llu frame=%llu output=%p "
			L"motion=%p depth=%p guides=borrowed copies=0 FGDetected=%d reset=%d",
			(unsigned long long)reported, (unsigned long long)frame.frameSeq,
			frame.output, frame.motionVectors, frame.depth,
			NgxEavesdrop::Get().FrameGenerationActive(0) ? 1 : 0, input.reset ? 1 : 0);
	}
	return ran;
}

// 给 Prepare 计时。
//
// 为什么专门盯这一步：Prepare 在尺寸/预设变化时会**同步销毁并重建 NGX feature**，
// 而它跑在游戏的 present 线程上。首次创建实测要 1.8 秒（含加载 165MB 的 snippet），
// 重建也要几百毫秒。玩家在游戏里改 DLSS 画质档 → 游戏的渲染分辨率变了 → 我们的
// 输入尺寸变了 → 正好触发重建。也就是说"一改 DLSS 就顿一下"完全可以是我们自己
// 造成的，和游戏没关系。
//
// 先把它量出来记下来：超过阈值就带上新旧尺寸写日志 + 落一颗事件面包屑。
// 有了数字才能决定要不要把初始化/重建挪到工作线程（那是个大改动，不能凭猜就动）。
template <typename Fn>
bool TimedPrepare(const char* what, Stage stage,
	uint32_t width, uint32_t height, Fn&& prepare) {
	LARGE_INTEGER before{};
	QueryPerformanceCounter(&before);
	const bool result = prepare();
	LARGE_INTEGER after{};
	QueryPerformanceCounter(&after);
	if (!g_state.qpcFreq.QuadPart) return result;

	const double ms = double(after.QuadPart - before.QuadPart) * 1000.0 /
		double(g_state.qpcFreq.QuadPart);
	// 60fps 一帧 16.7ms。超过 50ms 玩家一定看得见，这时候必须留下证据。
	if (ms >= 50.0) {
		D5_LOG_WARN(L"%hs Prepare 占用了 %.0f ms（%ux%u）—— 这一帧卡在 present 线程上。"
			L"通常是尺寸或预设变了导致 NGX feature 重建。",
			what, ms, width, height);
		FreezeWatchdog::Get().MarkEvent(stage, uint64_t(ms));
	}
	return result;
}

// NR 初始化（加载 165MB snippet + NGX Init_Ext，只试一次）。
//
// 为什么必须独立出来、且能在 FG 下触发：
// NR 现在跑在 evaluate 点（SR→NR），而它的初始化之前只挂在 present 路径的
// RunFilters 里。FG 检测到后 present 链纯转发（不跑 OnPresent/RunFilters），
// "开着 FG 启动游戏"时 nrInitialized 永远是 false → RunNrAtEvaluate 第一行
// 就 return false → NR 一帧都没跑。所以初始化必须和 present 路径解耦：present
// 路径（关 FG）和 swapchain 工厂（开 FG）都调用它，用原子互斥保证只跑一次。
//
// 总开关关着时不做 —— "关"就应该是真的什么都不碰。初始化本身不碰 FG 资源，
// FG 下也能安全初始化。
static void TryInitNr() noexcept {
	std::lock_guard<std::recursive_mutex> nrLock(g_nrStateMutex);
	if (!g_state.device12) return;
	if (!g_state.enabled.load(std::memory_order_relaxed)) return;
	if (!g_state.nrSettings.enabled) return;
	AdoptFallbackQueue();
	if (!g_state.capturedQueue) return;
	// **注入成功后延迟 5 秒再初始化 NR。**
	//
	// 3 秒实测不够（开 NR 启动时主菜单仍失效、进场景仍崩），加长到 5 秒。上一行的
	// nrSettings.enabled 检查已经保证"配置文件 dlss5Enable=false 就不启动"。
	constexpr uint64_t NR_INIT_DELAY_MS = 5000;
	const uint64_t injectAt = g_injectAtMs.load(std::memory_order_relaxed);
	const uint64_t nowMs = GetTickCount64();
	if (!injectAt || (nowMs >= injectAt && nowMs - injectAt < NR_INIT_DELAY_MS)) return;
	// Display-buffer NR is standalone: games with SR disabled never evaluate SR.
	// Only the SR->NR route needs a native SR call before it can process an image.
	if (g_state.nrAtEvaluate && !g_state.nrAutoRoute && !NgxEavesdrop::Get().EvaluateSeen()) return;
	if (g_state.nrTried) return;                       // 已试过（含失败）
	if (g_state.nrInitBusy.exchange(true)) return;      // 另一个线程正在初始化
	if (g_state.nrTried) {                              // 抢到锁后复查
		g_state.nrInitBusy.store(false);
		return;
	}
	g_state.nrTried = true;
	D5_EVENT(NrInit);
	g_state.nrInitialized = g_state.nrFilter.Initialize(
		g_state.device12, g_state.capturedQueue, g_state.selfModule);
	if (!g_state.nrInitialized) {
		g_publisher.Publish(g_state.nrFilter.LastError());
	}
	g_state.nrInitBusy.store(false);
}

// 每帧的滤镜链。任何一步不满足就安静跳过 —— 绝不能因为我们的功能没准备好
// 而影响游戏正常出图。
//
// 链路顺序是 **NR 在 SR 之前**，理由有两条：
//   1. 真超分时 NR 就跑在游戏的低渲染分辨率上，天然便宜。原生 4K 跑一遍 DLSSNR
//      非常贵，这正是"工作分辨率倍率"想解决的问题，串在放大之前就自动解决了。
//   2. 先增强再放大和真实的 ray reconstruction 管线同序。
// Store the queue on the real non-FG chain; GetPrivateData returns an AddRef'd
// interface. No swapchain reference is retained by the mod across ResizeBuffers.
void RememberPresentQueue(IDXGISwapChain* chain, IUnknown* device) {
	if (!chain || !device) return;
	ID3D12CommandQueue* queue = nullptr;
	if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&queue)))) {
		chain->SetPrivateDataInterface(NR_PRESENT_QUEUE, queue); queue->Release();
	}
}
void RunFilters11(IDXGISwapChain* swapChain) noexcept {
    if (!g_state.enabled.load() || !g_state.nrSettings.enabled || !g_state.device11) return;
    if (GetTickCount64() < g_state.uiPauseUntil) return;
    if (!g_state.nr11Tried) {
        g_state.nr11Tried = true;
        if (!g_state.nrFilter11.Initialize(g_state.device11, g_state.selfModule))
            g_publisher.Publish(g_state.nrFilter11.LastError());
    }
    if (!g_state.nrFilter11.IsInitialized() || g_state.nrFilter11.IsDisabled()) return;
    UpdateSemanticMask();
    ID3D11Texture2D* backbuffer = nullptr;
    UINT index = 0;
    IDXGISwapChain3* chain3 = nullptr;
    if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&chain3)))) {
        index = chain3->GetCurrentBackBufferIndex(); chain3->Release();
    }
    if (SUCCEEDED(swapChain->GetBuffer(index, IID_PPV_ARGS(&backbuffer)))) {
        g_state.nrFilter11.Execute(backbuffer, g_state.nrSettings);
        backbuffer->Release();
    }
}
void RunFilters(IDXGISwapChain* swapChain) noexcept {
	std::lock_guard<std::recursive_mutex> nrLock(g_nrStateMutex);
	const bool handoffWasBlocked = g_state.nrAutomaticHandoffBlocked;
	g_state.nrAutomaticHandoffBlocked = false;
	const bool master = g_state.enabled.load(std::memory_order_relaxed);

	// **总开关关掉时不能无条件 return。**
	//
	// 真超分的代理 backbuffer 一旦交给游戏，游戏画的就是那张小图，而**我们是
	// 唯一把它放大回真 backbuffer 的人**。这时候直接 return，真 backbuffer 就
	// 再也没人写 —— 玩家看到的是画面冻在按下 Alt+D 的那一帧（街霸 6 实测）。
	// 所以代理还活着的时候，总开关只关 DLSSNR，缩放这一环必须继续跑。
	// （代理拆不掉：游戏还持着那些 RTV。要彻底关掉只能重启游戏。）
    g_state.segMask.SetActive(master && g_state.nrSettings.enabled && g_state.nrSettings.semanticMask);
    if (g_state.api == GraphicsApi::D3D11) { RunFilters11(swapChain); return; }
    UpdateSemanticMask();

	const bool proxyLive = g_state.scalerActive.load(std::memory_order_acquire);
	if (!master && !proxyLive) return;

	// FG Present protection uses live high-buffer-count chains plus a known FG
	// module. FG Evaluate is never wrapped. A game may retain this chain after
	// disabling FG, so an option toggle alone cannot safely release the guard.
	// Confirmed SR -> NR remains available independently of this Present guard.
	const bool fgActive =
		NgxEavesdrop::Get().FrameGenerationActive(1000);
	if (fgActive && !g_state.nrAtEvaluate) return;
	const bool autoRoute = g_state.nrAtEvaluate && g_state.nrAutoRoute;
	UINT handoffBuffers = 0;
	const bool uncertainHandoff = autoRoute && NgxEavesdrop::Get().FrameGenerationModuleLoaded() &&
		FrameGenSwapChains::Get().AutomaticHandoffUncertain(swapChain, &handoffBuffers);
	const auto routeStatus = NrRouteProbe::Get().Snapshot(GetTickCount64(), fgActive, uncertainHandoff);
	g_state.nrAutomaticHandoffBlocked = master && g_state.nrSettings.enabled && routeStatus.handoffBlocked;
	// A menu can stop SR while retaining FG's resources/queue ownership. The
	// user threshold is a heuristic override, never proof that this handoff is
	// safe. Preserve native SR->NR and standalone Present; hold only an automatic
	// handoff after native SR has actually been observed on an uncertain chain.
	if (g_state.nrAutomaticHandoffBlocked && !handoffWasBlocked) {
		D5_LOG_WARN(L"NR automatic Present handoff held: native SR paused/empty; "
			L"FG module loaded, chain=%p buffers=%u configuredThreshold=%u. "
			L"FG enabled state is unknown; retaining SR->NR until SR resumes or chain uncertainty clears.",
			swapChain, handoffBuffers, NgxEavesdrop::Get().FgBufferCountThreshold());
		g_publisher.Publish("NR paused: waiting for native SR. FG state unknown; automatic Present handoff is unverified on this chain.");
	} else if (!g_state.nrAutomaticHandoffBlocked && handoffWasBlocked) {
		D5_LOG_INFO(L"NR automatic Present handoff hold cleared: chain=%p buffers=%u nativeRecent=%u dormant=%u",
			swapChain, handoffBuffers, routeStatus.nativeRecent, routeStatus.dormant);
		g_publisher.Publish(master && g_state.nrSettings.enabled
			? "NR automatic handoff hold cleared: native SR returned or current chain uncertainty cleared."
			: "NR automatic handoff hold cleared: NR is disabled.");
	}
	const bool presentNr = !fgActive && (!g_state.nrAtEvaluate || (autoRoute && routeStatus.present)) &&
		EvaluateGpuGate::Get().IsIdle();
	if (!presentNr && !proxyLive && !g_state.srSettings.enabled) return;


	D5_STAGE(FiltersEnter);
	if (!proxyLive && !g_state.srSettings.enabled && !g_state.nrSettings.enabled) {
		return;
	}

	if (!g_state.device12) return;
	AdoptFallbackQueue();
	if (!g_state.capturedQueue) return;

	// NGX core 的初始化**只在需要 DLSS SR 时**才做。
	//
	// 为什么要分开：NVSDK_NGX_D3D12_Init* 是每进程每设备一次的，游戏自己已经有
	// 活着的 NGX 会话时我们再初始化一次会把它弄坏（游戏的 DLSS 从此失效，且只能
	// 重启游戏）。而 DLSS5/DLSSNR 走的是 snippet 自己的 Init_Ext + 我们自带的参数
	// 容器，完全不碰 core —— 所以「只开 DLSS5，超分用游戏原生的」这个组合是可行的，
	// 前提就是这里不要多此一举地初始化 core。
	const bool needsNgxCore = g_state.srSettings.enabled;
	if (needsNgxCore && !g_state.ngxTried) {
		g_state.ngxTried = true;
		D5_EVENT(NgxCoreInit);
		if (!NgxSession::Get().Initialize(g_state.device12, g_state.selfModule,
				g_state.allowNgxCoexist) ||
			!NgxSession::Get().IsSuperSamplingAvailable()) {
			g_publisher.Publish(NgxSession::Get().UnavailableReason());
			// 被"游戏自己在跑 DLSS"拦下时，把 SR 这一路彻底停掉：既不再重试，也不要
			// 让状态栏挂着"待生效"让人以为还有希望。DLSS5 不受影响 —— 它走 snippet
			// 自己的 Init_Ext，不碰 NGX core，可以和游戏原生 DLSS 共存。
			if (NgxSession::Get().IsBlockedByGameNgx()) {
				g_state.srSettings.enabled = false;
				g_state.wantsUpscale.store(false, std::memory_order_release);
			}
		}
	}
	// SR 需要 core；NR 不需要，所以这里不能整体 return
	const bool srUsable = NgxSession::Get().IsInitialized() &&
		NgxSession::Get().IsSuperSamplingAvailable();
	if (!srUsable && !proxyLive && !g_state.nrSettings.enabled) return;

	if (g_state.srSettings.enabled && srUsable && !g_state.upscalerInitialized) {
		D5_EVENT(UpscalerInit);
		g_state.upscalerInitialized = g_state.upscaler.Initialize(
			g_state.device12, g_state.capturedQueue);
		if (!g_state.upscalerInitialized) {
			g_publisher.Publish(g_state.upscaler.LastError());
			return;
		}
	}

	// NR 初始化。独立成 TryInitNr：present 路径和 swapchain 工厂（FG 下）都能触发。
	TryInitNr();

	// 当前 backbuffer。注意这里必须走**原始**的 GetBuffer：我们的 hook 会把
	// 代理纹理交出去，而这一步要的是真正要呈现的那块。
	D5_STAGE(GetBackBuffer);
	IDXGISwapChain3* swapChain3 = nullptr;
	if (FAILED(swapChain->QueryInterface(
		__uuidof(IDXGISwapChain3), reinterpret_cast<void**>(&swapChain3)))) {
		return;
	}
	const UINT index = swapChain3->GetCurrentBackBufferIndex();
	ID3D12Resource* presentTarget = nullptr;
	const bool gotBuffer = g_originalGetBuffer
		? SUCCEEDED(g_originalGetBuffer(
			swapChain3, index, __uuidof(ID3D12Resource),
			reinterpret_cast<void**>(&presentTarget)))
		: SUCCEEDED(swapChain3->GetBuffer(index, IID_PPV_ARGS(&presentTarget)));
	swapChain3->Release();
	if (!gotBuffer || !presentTarget) return;

	// 真超分时游戏画的是代理纹理，NGX 的输入取它
	const bool viaProxy =
		swapChain == g_state.scaledSwapChain && g_state.scaler.IsActive();
	ID3D12Resource* colorSource =
		viaProxy ? g_state.scaler.ProxyBuffer(index) : presentTarget;

	if (!colorSource) {
		presentTarget->Release();
		return;
	}

	const D3D12_RESOURCE_DESC targetDesc = presentTarget->GetDesc();
	const D3D12_RESOURCE_DESC sourceDesc = colorSource->GetDesc();
	const GpuImage backBufferImage{
		presentTarget, D3D12_RESOURCE_STATE_PRESENT };
	// 代理纹理游戏也当 backbuffer 用，同样处于 PRESENT(== COMMON)
	GpuImage current{ colorSource, D3D12_RESOURCE_STATE_PRESENT };

	// ---- NR：同分辨率，跑在游戏的渲染分辨率上 ----
	// 要求真超分但代理没建起来（注入晚于 swapchain 创建，真实游戏里最常见的情况）
	// 时 render 就等于 output，此时必须按 DLAA 去建 feature —— 否则会拿一个上采样
	// 档位去跑 1:1 分辨率，参数自相矛盾。
	SrSettings effectiveSr = g_state.srSettings;

	// 代理一旦建立，SR 就**必须**跑：它是这条链上唯一会缩放的环节，游戏画在小代理
	// 上的图只能靠它送上屏幕。用户此时把 SR 关掉的话，画面会冻在残影上（实测过）。
	// 代理没法安全拆掉（游戏还持有那些 RTV），所以只能继续跑。
	if (viaProxy && !effectiveSr.enabled) {
		effectiveSr.enabled = true;
		effectiveSr.mode = SrMode::Upscale;
		static bool warned = false;
		if (!warned) {
			warned = true;
			D5_LOG_WARN(L"代理 backbuffer 已建立，SR 不能关 —— 它是唯一的缩放环节。"
				L"继续按真超分运行。要彻底关掉请重启游戏。");
			g_publisher.Publish(
				"代理已建立，DLSS SR 无法中途关闭（它是唯一的缩放环节）。"
				"要关掉请重启游戏。");
		}
	}

	// 必须同时判 enabled：只看 mode 的话，玩家把 SR 关掉但配置里 srMode 还是
	// upscale（很常见，关开关不会改档位）时也会走进来，于是状态栏挂着一句
	// "真超分未生效，已降级 DLAA（注入太晚）"—— 而其实用户根本没开真超分。
	// 鬼武者那份日志里就是这样，白白误导了一轮。
	if (effectiveSr.enabled && effectiveSr.mode == SrMode::Upscale && !viaProxy) {
		effectiveSr.mode = SrMode::Dlaa;
		effectiveSr.quality = SrQuality::Quality;
		effectiveSr.inputMultiplier = 1.0f;
		// **nudge 还在飞的时候一个字都别说。**
		// 玩家刚按 Alt+D 打开总开关，我们正在 nudge 窗口让游戏重建 backbuffer ——
		// 那两秒里"代理没建起来"是事实但是暂时的，而这条提示是一次性的、
		// 不会自己撤。实测就在这里挂上了一句"降级 DLAA（注入太晚）"，
		// 而两秒后真超分明明生效了，界面上那句话还在。
		if (!g_state.proxyNudgeInFlight.load(std::memory_order_acquire) &&
			!g_srDegradeWarned) {
			g_srDegradeWarned = true;
			if (g_state.proxyBanned.load(std::memory_order_acquire)) {
				// 别再说"注入太晚"——这一局代理是我们**主动撤掉**的
				D5_LOG_WARN(L"真超分对这个游戏不可用（它按自己的分辨率渲染，"
					L"无视 swapchain 尺寸），已按 DLAA 运行。");
				g_publisher.Publish("真超分对这个游戏不可用，已按 DLAA 运行。");
			} else {
				D5_LOG_WARN(L"选了真超分但代理 backbuffer 没建起来（注入晚于 swapchain "
					L"创建？），本次按 DLAA 运行。真超分必须在游戏创建 swapchain 之前注入。");
				g_publisher.Publish("真超分未生效，已降级 DLAA（注入太晚）");
			}
		}
	}

	D5_STAGE(SrPrepare);
	const bool srWillRun = effectiveSr.enabled &&
		g_state.upscalerInitialized &&
		TimedPrepare("DLSS SR", Stage::SrPrepare, (uint32_t)sourceDesc.Width,
			(uint32_t)sourceDesc.Height, [&] {
				return g_state.upscaler.Prepare(
					(uint32_t)sourceDesc.Width, (uint32_t)sourceDesc.Height,
					(uint32_t)targetDesc.Width, (uint32_t)targetDesc.Height,
					targetDesc.Format, effectiveSr);
			});

	// 诊断夹具（默认关）。位置在 SrPrepare 之后、NrPrepare 之前，所以转储里最后
	// 一颗面包屑应该正好是 SrPrepare —— 验证"指认的阶段对不对"，而不只是"有没有
	// 报警"。
	if (g_state.stallAtPresent &&
		g_state.presentCount.load(std::memory_order_relaxed) ==
			g_state.stallAtPresent) {
		D5_LOG_WARN(L"诊断夹具触发：在滤镜链里睡 10 秒");
		Sleep(10000);
	}

	// 夹具：主动把设备干掉，验证设备移除后的转储路径（含 DRED）真的会打东西出来。
	if (g_state.removeDeviceAt &&
		g_state.presentCount.load(std::memory_order_relaxed) ==
			g_state.removeDeviceAt) {
		ID3D12Device5* device5 = nullptr;
		if (g_state.device12 && SUCCEEDED(g_state.device12->QueryInterface(
				__uuidof(ID3D12Device5), reinterpret_cast<void**>(&device5)))) {
			D5_LOG_WARN(L"诊断夹具触发：主动移除设备。接下来**必须**看到"
				L"「卡顿转储」+「DRED」两段输出。");
			device5->RemoveDevice();
			device5->Release();
		} else {
			D5_LOG_ERROR(L"诊断夹具：拿不到 ID3D12Device5，移除不了设备");
		}
	}

	// 把旁听到的真运动矢量交给 NR。
	//
	// **尺寸不符不再是死路。** 游戏的矢量在它自己的渲染分辨率上（开着 DLSS Quality 时
	// 1286x724），而这条路的 NR 跑在 backbuffer（1920x1080）上。以前的做法是尺寸不符
	// 就退回零矢量 —— 抄了一万多帧真矢量然后全扔掉。现在交给滤镜自己用计算着色器
	// 重采样上来（见 DlssNrFilter::CanResampleMotion，它会先确认矢量是归一化 UV 约定）。
	//
	// 深度**仍然要求尺寸一致**：深度不是 UV 空间的量，重采样深度值本身没问题，
	// 但它和颜色的对应关系是按渲染分辨率建立的，缩放之后 disocclusion 判定会错位。
	// 没验证过的事不做。
	if (master && g_state.nrInitialized && presentNr) {
		ID3D12CommandQueue* presentQueue = nullptr;
		UINT queueBytes = sizeof(presentQueue);
		if (FAILED(swapChain->GetPrivateData(NR_PRESENT_QUEUE, &queueBytes, &presentQueue))) {
			presentQueue = g_state.capturedQueue;
			if (presentQueue) presentQueue->AddRef();
		}
		const bool queueReady = g_state.nrFilter.SetPresentQueue(presentQueue);
		if (presentQueue) presentQueue->Release();
		if (!queueReady) { presentTarget->Release(); return; }
		// No active native SR: its old captured guides do not describe this image.
		const NgxCapturedTextures captured = autoRoute ? NgxCapturedTextures{} : NgxEavesdrop::Get().Captured();
		const bool haveMotion = captured.motionVectors && captured.frameSeq;
		const bool sameSize = captured.width == (uint32_t)sourceDesc.Width &&
			captured.height == (uint32_t)sourceDesc.Height;
		static int reported = 0;
		if (captured.frameSeq && reported < 2) {
			++reported;
			D5_LOG_INFO(L"旁听矢量 %ux%u vs NR 工作分辨率 %ux%u -> %s",
				captured.width, captured.height,
				(uint32_t)sourceDesc.Width, (uint32_t)sourceDesc.Height,
				sameSize ? L"尺寸相符，直接用"
					: L"尺寸不符，**用计算着色器重采样上来**");
		}
		// 假矢量夹具开着时别覆盖它。**这一条是被咬出来的**：这段每帧都跑，
		// 旁听关着时会把 _externalMotion 重置成 nullptr —— 夹具在 present 末尾设的
		// 假矢量就这么被冲掉了，于是"调试层 0 报错"其实是因为那段 dispatch 一次都没跑。
		// 一个不会失败的测试比没有测试更坏，因为它会让人以为验过了。
		if (!g_state.fakeMotion) {
			g_state.nrFilter.SetExternalMotion(
				haveMotion ? captured.motionVectors : nullptr,
				captured.width, captured.height,
				captured.mvScaleX, captured.mvScaleY);
		}
		// **深度不再卡在"尺寸必须一致"上。** 深度值本身与分辨率无关（同一个视锥的
		// NDC 深度），所以重采样到工作分辨率是几何正确的 —— 只是必须最近邻，
		// 双线性会在轮廓上插出场景里不存在的中间深度。滤镜那边负责。
		// 以前这里传 nullptr，等于抄到了真深度然后原地扔掉。
		g_state.nrFilter.SetExternalDepth(
			haveMotion ? captured.depth : nullptr,
			captured.width, captured.height);
	}

	// 开了 evaluate 点那条路时，present 路径**完全不碰** NR —— 那个滤镜此刻正被
	// 游戏的渲染线程用着，两边同时用同一个 feature/命令槽必然出事。
	//
	// `master` 也是条件之一：总开关关掉但代理还活着时，这条路只剩"把代理放大回
	// 真 backbuffer"要做，DLSSNR 必须跳过 —— 否则"关"了还在处理画面。
	D5_STAGE(NrPrepare);
	// 换链暂停期：NR 的 present 提交（EvaluateFeature 在游戏队列上执行）和 UI 一样
	// 会撞 DXGI 换链窗口（MHW 实测 ACCESS_LOST）。暂停期内跳过 NR，只等新链稳定。
	// 和 UI 一样补一道 present 帧数保护：uiPauseUntil 是真实时间，游戏 present 停
	// 几秒（鬼武者切 4K）时它早过期了，uiPauseFrames（present 停了不递减）才靠得住。
	const bool nrPaused = (g_state.uiPauseUntil &&
			GetTickCount64() < g_state.uiPauseUntil) ||
		g_state.uiPauseFrames > 0;
	NrSettings presentSettings = g_state.nrSettings;
	// Legacy zero-motion fallback resets NR each frame. Optical fallback owns its
	// history resets and retains temporal history only while flow is available.
	if (autoRoute) presentSettings.forceReset = !presentSettings.opticalFlow;
	if (!nrPaused && master && g_state.nrInitialized && presentNr &&
		TimedPrepare("DLSS5", Stage::NrPrepare,
		(uint32_t)sourceDesc.Width, (uint32_t)sourceDesc.Height, [&] {
			return g_state.nrFilter.Prepare(
				(uint32_t)sourceDesc.Width, (uint32_t)sourceDesc.Height,
				sourceDesc.Format, presentSettings, NrMode::Present, g_state.nrAutoRoute);
		})) {
		// SR 接在后面时把结果留在交接纹理里，否则直接落到 backbuffer。
		// 代理生效时 NR 的尺寸是渲染分辨率，和 backbuffer 不一致，**只能**交给
		// SR —— 直接拷会变成左上角一小块。
		if (viaProxy && !srWillRun) {
			// 代理生效但 SR 起不来 = **画面会全黑**（没人把代理放大回真 backbuffer）。
			// 这是死局，只能告诉玩家原因 —— 代理拆不掉（游戏还持着那些 RTV）。
			// 上一版每帧刷一条 warn（实测 1595 条），而且 UI 上什么都不显示，
			// 玩家只看到黑屏。现在限流 + 发到状态块。
			static uint32_t blackFrames = 0;
			if (blackFrames++ == 0) {
				D5_LOG_ERROR(L"代理 backbuffer 已生效但 SR 没运行 —— 真 backbuffer 不会"
					L"有人写，**画面会全黑**。原因通常是 NGX 不可用（游戏自己在跑 "
					L"DLSS）。代理已经交给游戏了，拆不掉，只能重启游戏。");
				g_publisher.Publish(
					"画面全黑：代理已建立但 DLSS SR 起不来（NGX 不可用）。"
					"请重启游戏，并在游戏里关掉 DLSS 后再用真超分，或只开 DLSS5。");
			} else if (blackFrames % 600 == 0) {
				D5_LOG_ERROR(L"仍在黑屏状态（已 %u 帧）", blackFrames);
			}
		} else {
			const GpuImage nrDest =
				srWillRun ? g_state.nrFilter.Handoff() : backBufferImage;
			if (g_state.nrFilter.Execute(current, nrDest)) {
				static uint64_t displayFrames = 0;
				if (++displayFrames <= 3 || displayFrames % 600 == 0) {
					D5_LOG_INFO(L"NR display route active: source=%p dest=%p actualBackBuffer=%p "
						L"size=%llux%u format=%u nativeSRSeen=%llu frames=%llu auto=%d srRecent=%d sampledEmpty=%d samples=%llu",
						current.resource, nrDest.resource, presentTarget, targetDesc.Width,
						targetDesc.Height, (unsigned)targetDesc.Format,
						(unsigned long long)NgxEavesdrop::Get().EvaluateSeen(),
						(unsigned long long)displayFrames, autoRoute, routeStatus.nativeRecent,
						routeStatus.dormant, (unsigned long long)routeStatus.samples);
				}
				current = nrDest;
			}
		}
	}

	// ---- SR：放大到输出分辨率 ----
	if (srWillRun) {
		g_state.upscaler.Execute(current, backBufferImage);
	} else if (current.resource != presentTarget &&
		current.resource != colorSource) {
		// NR 单独生效且源是代理纹理这种组合不该出现（代理只在真超分下存在），
		// 真出现了说明链路接错了，记一次别静默。
		D5_LOG_WARN(L"滤镜链结束时结果没有落到 backbuffer，这一帧丢弃");
	}

	presentTarget->Release();
	D5_STAGE(FiltersExit);
}

/* ============================ in-game UI ============================ */
namespace {

struct UiToast {
    char text[192];
    double born;   // QPC seconds
    double until;
};
UiToast g_toasts[6];
int g_toastHead = 0;
double g_uiClock = 0.0;   // seconds since first use (QPC)

std::atomic<bool> g_uiOpen{ false };
bool g_uiInitialised = false;
bool g_uiReadyToast = false;
bool g_lastUiEnabled = true;
int  g_lastUiInputs = 1;   // 1 = native, 0 = zero
bool g_uiSavePending = false;
double g_lastSave = 0.0;

void UiPushToastFmt(double seconds, const char* fmt, va_list ap) {
    UiToast& t = g_toasts[g_toastHead];
    g_toastHead = (g_toastHead + 1) % (int)(sizeof(g_toasts) / sizeof(g_toasts[0]));
    vsnprintf(t.text, sizeof(t.text), fmt, ap);
    t.born = g_uiClock;
    t.until = g_uiClock + seconds;
}

void UiPushToast(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    UiPushToastFmt(4.5, fmt, ap);
    va_end(ap);
}

// 启动/注入成功的提示比普通提示多显示 3 秒
void UiPushToastLong(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    UiPushToastFmt(4.5 + 3.0, fmt, ap);
    va_end(ap);
}

// --- settings persistence: rewrite every UI key into the file core reads ----
// 单次读改写：已存在的键原地替换，缺的键（例如旧版配置文件）自动补在对象开头，
// 文件不存在则先让自愈逻辑生成一份。绝不静默失败 —— 首写成功会记一条日志，
// 下次启动 ReloadSettings 也会把"设置文件:"路径打进日志，方便核对读写的文件
// 是否同一份（历史上踩过 core 被同名模块去重加载到别的目录导致读写分家）。
static std::wstring UiSettingsPath() {
    const auto path = SettingsReader::FindParamsPath(g_state.selfModule);
    return path.wstring();
}

static bool UiLoadText(const std::wstring& path, std::string& out) {
    out.clear();
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return false;
    char buf[8192]; size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    fclose(f);
    return !out.empty();
}

// 在 text 里写入 key=value：存在则替换值，不存在则插到 '{' 之后。
static void UiApplyKey(std::string& text, const std::string& key,
                       const std::string& value) {
    const std::string needle = "\"" + key + "\":";
    const size_t at = text.find(needle);
    if (at != std::string::npos) {
        size_t head = at + needle.size();
        while (head < text.size() &&
               (text[head] == ' ' || text[head] == '\t')) ++head;
        const size_t tail = text.find_first_of(",}\n", head);
        if (tail != std::string::npos) {
            text.replace(head, tail - head, value);
            return;
        }
    }
    const size_t open = text.find('{');
    if (open != std::string::npos) {
        const size_t next = text.find_first_not_of(" \t\r\n", open + 1);
        const bool empty = next != std::string::npos && text[next] == '}';
        text.insert(open + 1, "\n  " + needle + " " + value + (empty ? "" : ","));
    }
}

static void UiSaveSettings() {
	std::lock_guard<std::recursive_mutex> nrLock(g_nrStateMutex);
    // **串行化文件 IO**：present 线程（UiFramePresent 防抖落盘）和 F8 独立线程
    // （F8 线程也可直接落盘）都可能调它，并发写同一份 json 会损坏。
    static std::mutex saveMutex;
    std::lock_guard<std::mutex> guard(saveMutex);

    const std::wstring path = UiSettingsPath();
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);

    std::string text;
    if (!UiLoadText(path, text)) text = "{\n}\n";
    if (text.find('{') == std::string::npos) return;

    const NrSettings& nr = g_state.nrSettings;
    const auto asBool = [](bool b) { return b ? "true" : "false"; };
    const auto asFlt = [](float v) {
        char b[32]; snprintf(b, sizeof(b), "%.4f", (double)v); return std::string(b);
    };
    // 写回的是**配置原始意图**，不是运行时 canAtEvaluate（否则 F8 关 NR 会顺手把
    // dlss5AtEvaluate 写成 false，NR 再也回不到 evaluate 点）。
    UiApplyKey(text, "nrUseRealDepth", asBool(nr.useRealDepth));
    UiApplyKey(text, "nrUseRealMotion", asBool(nr.useRealMotion));
    UiApplyKey(text, "nrOpticalFlow", asBool(nr.opticalFlow));
    UiApplyKey(text, "nrOpticalFlowQuality", std::to_string(nr.opticalQuality));
    UiApplyKey(text, "nrPreset", std::to_string(nr.preset));
    UiApplyKey(text, "nrStyle", std::to_string(nr.style));
    UiApplyKey(text, "nrIntensity", asFlt(nr.intensity));
    UiApplyKey(text, "nrLocalTone", asFlt(nr.localTone));
    UiApplyKey(text, "nrColourStrength", asFlt(nr.colourStrength));
    UiApplyKey(text, "nrSelfLayers", asFlt(std::clamp(nr.selfLayers, 1.0f, 3.0f)));
    UiApplyKey(text, "nrTrueLayers", std::to_string(std::clamp(nr.trueLayers, 1, 5)));
    UiApplyKey(text, "nrLocalStructure", asFlt(nr.localStructure));
    UiApplyKey(text, "nrSkinStructure", asFlt(nr.skinStructure));
    UiApplyKey(text, "nrAutoMask", asBool(nr.autoMask));
    UiApplyKey(text, "nrUiCorrection", asBool(nr.uiCorrection));
    UiApplyKey(text, "nrRenderScale", asFlt(nr.renderScale));
    UiApplyKey(text, "nrToneScale", asFlt(nr.toneScale));
    UiApplyKey(text, "nrDebugView",
               std::to_string(static_cast<int>(nr.debugView)));
    UiApplyKey(text, "mvScale", asFlt(nr.mvScale));
    UiApplyKey(text, "depthInverted", asBool(nr.depthInverted));
    UiApplyKey(text, "hkEnable", std::to_string(g_state.hkToggleEnabled));
    UiApplyKey(text, "hkEnableMods", std::to_string(g_state.hkEnableMods));
    UiApplyKey(text, "hotkeyRevision", std::to_string(g_state.hotkeyRevision));
    UiApplyKey(text, "hkUi", std::to_string(g_state.hkToggleUi));
    UiApplyKey(text, "hkUiMods", std::to_string(g_state.hkUiMods));
    UiApplyKey(text, "nrControlMask", asBool(nr.controlMask));
    UiApplyKey(text, "nrControlMaskR", asFlt(nr.controlMaskR));
    UiApplyKey(text, "nrControlMaskG", asFlt(nr.controlMaskG));
    UiApplyKey(text, "nrControlMaskB", asFlt(nr.controlMaskB));
    UiApplyKey(text, "nrControlMaskA", asFlt(nr.controlMaskA));
    UiApplyKey(text, "nrSemanticMask", asBool(nr.semanticMask));
    UiApplyKey(text, "nrSemOn", std::to_string(nr.semanticEnabled));
    UiApplyKey(text, "nrSemBgInt", asFlt(nr.semanticBgIntensity));
    UiApplyKey(text, "nrSemanticDebugView", asBool(nr.semanticDebugView));
    UiApplyKey(text, "nrSemanticFlipY", asBool(nr.semanticFlipY));
    UiApplyKey(text, "nrSemanticFeather", asFlt(nr.semanticFeather));
    for (int g = 0; g < SEM_GROUP_COUNT; ++g)
        UiApplyKey(text, "nrSemInt" + std::to_string(g), asFlt(nr.semanticIntensity[g]));

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"wb") == 0 && f) {
        fwrite(text.data(), 1, text.size(), f);
        fclose(f);
        static bool savedOnce = false;
        if (!savedOnce) {
            savedOnce = true;
            D5_LOG_INFO(L"UI 参数已写入: %s", path.c_str());
        }
    }
}

// RTX ON accent
static const ImVec4 RTX(0.463f, 0.725f, 0.0f, 1.0f);   // #76B900

static const char* kPresetNames[] = { "0 Quality", "1", "2", "3", "4" };
static const char* kStyleNames[]  = { "Default", "Natural", "Cinematic" };

static void UiMarkSave() {
    ++g_state.nrParamVersion;
    g_uiSavePending = true;
}

// 产品版本号（面板标题行下方显示）
static const char* kUiVersion = "0.3";

// ---- 中英双语参数说明（复用自早期插件版，参考 NVIDIA DLSS5 文章措辞）----
static const char* UiText(const char* zh, const char* en) { return g_state.uiLanguage == 2 ? en : zh; }
struct UiHelpText { const char* en; const char* zh; };
static void UiItemHelp(const char* text) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", text);
    }
}

static void UiItemHelp(const UiHelpText& text) { UiItemHelp(UiText(text.zh,text.en)); }

static const UiHelpText kHelpToggle = {"Toggle the neural-render filter.\nDLSS5 takes one colour frame plus motion vectors and outputs a deterministic, temporally-stable neural pass.", "开关神经网络渲染滤镜。\n以单帧色彩与运动矢量作为输入，输出确定性、时间稳定的神经渲染结果。"};
static const UiHelpText kHelpInputs = {"Input source for DLSS5.\nNative: depth + motion captured from the game's DLSS.\nZero: empty buffers (safe fallback when nothing is captured).", "输入源。\nNative：旁听自游戏 DLSS 的真深度/运动矢量。\nZero：空缓冲（没有捕获到任何输入时的安全兜底）。"};
static const UiHelpText kHelpPreset = {"Neural model weight set.\nDifferent presets tune the balance between detail recovery and temporal stability (0 = quality-leaning default).", "神经网络模型档位。\n不同预设调整细节重建与时间稳定的平衡（0 = 偏质量的默认档）。"};
static const UiHelpText kHelpStyle = {"Overall look of the neural pass.\nDefault: balanced. Natural: keeps the engine frame's original feel, softer grain. Cinematic: stronger contrast and film-like materials.", "神经渲染的整体观感。\nDefault 均衡；Natural 更贴近原始渲染、更自然柔和；Cinematic 对比与质感更强、更具电影感。"};
static const UiHelpText kHelpIntensity = {"Overall strength of the neural enhancements.\n0 = filter output equals the input frame; 1 = full effect.", "神经增强的整体强度。\n0 时输出等于输入画面，1 为完整效果。"};
static const UiHelpText kHelpTone = {"DLSSNR local tone strength. Higher values allow a stronger tone response.\nUse Colour Strength for original-colour restoration.", "NR 局部色调强度；还原原生颜色请使用 Colour Strength。"};
static const UiHelpText kHelpStructure = {"Controls HIGH-frequency detail: ambient occlusion, contact shadows, reflections and subsurface scattering.\nRaise it for crisper, more grounded materials.", "控制高频细节：环境光遮蔽、接触阴影、反射与次表面散射。\n调高可让材质更清晰扎实。"};
static const UiHelpText kHelpSkin = {"Skin-specific enhancement amount.\nAdds natural subsurface scattering so skin reacts to light with realistic warm tones. 0 leaves skin untouched.", "针对皮肤的处理强度。\n为皮肤增加自然的次表面散射，受光时呈现真实的暖调质感。0 不改动皮肤。"};
static const UiHelpText kHelpAutoMask = {"Semantic AI mask that recognises scene objects.\nLimits enhancements to the intended targets (e.g. boost environment without altering characters, or vice-versa).", "语义 AI 遮罩：自动识别场景对象，\n让增强只作用于预设目标区域（如只增强环境而不改动角色，反之亦可）。"};
static const UiHelpText kHelpUiCorrection = {"Keeps UI / HUD / text crisp.\nExcludes interface regions from the neural pass so menus and overlays do not get smoothed or distorted.", "保持 UI/HUD/文字清晰。\n将界面区域排除在神经渲染之外，避免菜单与叠层被平滑或扭曲。"};
static const UiHelpText kHelpRenderScale = {"Internal compute resolution of the neural pass.\n1.0 = full resolution; lower reduces NR cost. Applies live with or without DLSS SR.", "NR 内部计算比例，开关 DLSS SR 均可实时调节。\n降低比例可节省 NR 开销；输出分辨率不变，NR 细节与色调可能变化。"};
#if 0  // —— 手动白点帮助文本（已废除，保留备用）——
static const UiHelpText kHelpWhitePoint = {"Paper white: how many linear units count as diffuse white.\nThe HDR input is divided by this before feeding the filter. 0 = auto (measured from the scene). Higher = darker, lower = brighter. Default 203.", "纸白：多少线性数值算漫反射白。\nHDR 输入除以它之后才喂给滤镜。0 = 自动（从场景测）。越大越暗、越小越亮。默认 203。"};
#endif
static const UiHelpText kHelpDebugView = {"Overlay a live view of the filter inputs / output on screen.\n0 Off | 1 Depth | 2 Motion vectors | 3 Encoded preview | 4 Diff.\nEncoded has no automatic contrast stretch. On the SR route, the preview still passes through the game's tone mapping and colour grading.", "在屏幕上叠加显示滤镜输入/输出的可视化。\n0 关 | 1 深度 | 2 运动矢量 | 3 编码输入预览 | 4 差值。\nEncoded 不再自动拉伸对比度；SR 路径的预览仍经过游戏后续色调映射和调色。"};
static const UiHelpText kHelpHotkeyRow = {"Click, then press the new key. Esc = cancel.", "点击后按下新按键即可重绑。Esc 取消。"};

// ---- hotkeys: display name + in-panel rebinding ----
static const char* VkName(int vk, char* out, size_t cap) {
    if (!out || !cap) return "";
    static const struct { int vk; const char* name; } kMap[] = {
        { VK_ESCAPE, "Esc" }, { VK_TAB, "Tab" }, { VK_BACK, "Backspace" },
        { VK_RETURN, "Enter" }, { VK_SPACE, "Space" },
        { VK_INSERT, "Ins" }, { VK_DELETE, "Del" }, { VK_HOME, "Home" },
        { VK_END, "End" }, { VK_PRIOR, "PgUp" }, { VK_NEXT, "PgDn" },
        { VK_LEFT, "Left" }, { VK_RIGHT, "Right" }, { VK_UP, "Up" },
        { VK_DOWN, "Down" }, { VK_CAPITAL, "Caps Lock" },
        { VK_NUMLOCK, "Num Lock" }, { VK_SCROLL, "Scroll Lock" },
        { VK_ADD, "Numpad +" }, { VK_SUBTRACT, "Numpad -" },
        { VK_MULTIPLY, "Numpad *" }, { VK_DIVIDE, "Numpad /" },
        { VK_DECIMAL, "Numpad ." }, { VK_OEM_1, ";" }, { VK_OEM_PLUS, "=" },
        { VK_OEM_COMMA, "," }, { VK_OEM_MINUS, "-" }, { VK_OEM_PERIOD, "." },
        { VK_OEM_2, "/" }, { VK_OEM_3, "`" }, { VK_OEM_4, "[" },
        { VK_OEM_5, "\\" }, { VK_OEM_6, "]" }, { VK_OEM_7, "'" },
    };
    for (const auto& e : kMap) {
        if (e.vk == vk) { snprintf(out, cap, "%s", e.name); return out; }
    }
    if (vk >= VK_F1 && vk <= VK_F24) {
        snprintf(out, cap, "F%d", vk - VK_F1 + 1); return out;
    }
    if (vk >= '0' && vk <= '9') { snprintf(out, cap, "%c", (char)vk); return out; }
    if (vk >= 'A' && vk <= 'Z') { snprintf(out, cap, "%c", (char)vk); return out; }
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        snprintf(out, cap, "Numpad %d", vk - VK_NUMPAD0); return out;
    }
    snprintf(out, cap, "Key %02X", (unsigned)(vk & 0xFF));
    return out;
}

// 0 = nothing held, VK_ESCAPE = cancel, anything else = the captured key.
// (Retired: capture now uses per-frame edge tracking so a key that was already
// held when the user clicked "Change" is not captured by accident.)
static bool g_capPrev[256] = {};
static int g_capArmedFor = 0;

// which hotkey is waiting for a key press: 0 none, 1 = DLSS5 toggle, 2 = panel
static int g_hkCapture = 0;

static void UiToggleEnabled() {
	std::lock_guard<std::recursive_mutex> nrLock(g_nrStateMutex);
    const bool en = !(g_state.nrSettings.enabled &&
                      g_state.enabled.load(std::memory_order_relaxed));
    g_state.enabled.store(en, std::memory_order_relaxed);
    g_state.nrSettings.enabled = en;
    if (UsesD3D11Bridge(g_state.api)) g_state.nrFilter11.InvalidateOpticalHistory();
    else g_state.nrFilter.InvalidateOpticalHistory();
    UiMarkSave();
    // toast 由 UiFramePresent 的统一 enable-edge 检测发，避免重复
}

static void UiTogglePanel() {
    g_uiOpen.store(!g_uiOpen.load(std::memory_order_relaxed),
                   std::memory_order_relaxed);
}

// Present-thread close: release input immediately, including when no toast is
// active and the next frame will skip the overlay entirely.
static void UiClosePanel() {
    g_uiOpen.store(false, std::memory_order_relaxed);
    g_hkCapture = 0;
    g_capArmedFor = 0;
    ReUi::SetUiOpen(&g_uiOpen);
}

// F8 has one independent polling source, including while Present is paused.
// NR state changes use the same mutex as Evaluate, Present and the ImGui panel.
// All visual feedback now comes from ImGui's enable-edge detector.
static std::atomic<bool> g_nrHotkeyRunning{ false };

static int CurrentHotkeyModifiers() noexcept {
    return ((GetAsyncKeyState(VK_MENU) & 0x8000) ? 1 : 0) |
        ((GetAsyncKeyState(VK_CONTROL) & 0x8000) ? 2 : 0) |
        ((GetAsyncKeyState(VK_SHIFT) & 0x8000) ? 4 : 0) |
        (((GetAsyncKeyState(VK_LWIN) | GetAsyncKeyState(VK_RWIN)) & 0x8000) ? 8 : 0);
}
static void MarkHotkeyRevision() noexcept {
    FILETIME time{}; GetSystemTimeAsFileTime(&time);
    ULARGE_INTEGER ticks{}; ticks.LowPart = time.dwLowDateTime; ticks.HighPart = time.dwHighDateTime;
    const uint64_t unixMs = (ticks.QuadPart - 116444736000000000ULL) / 10000;
    g_state.hotkeyRevision = std::max(g_state.hotkeyRevision + 1, unixMs);
}
static void NrHotkeyThread() {
    ULONGLONG nextDiscovery = 0;
    int vk = 0, modifiers = 0;
    bool prev = false;
    while (g_nrHotkeyRunning.load(std::memory_order_acquire) && !IsTeardownRequested()) {
        const ULONGLONG now = GetTickCount64();
        if (now >= nextDiscovery) {
            nextDiscovery = now + 1000;
            NgxEavesdrop::Get().PollKnownUpscalerExports();
            bool legacyDiscovery = false;
            {
                std::lock_guard<std::recursive_mutex> lock(g_nrStateMutex);
                legacyDiscovery = g_state.api == GraphicsApi::Unknown || g_state.api == GraphicsApi::D3D9;
            }
            PollLegacyGraphicsHooks(legacyDiscovery);
        }
        {
            std::lock_guard<std::recursive_mutex> lock(g_nrStateMutex);
            const int cur = g_state.hkToggleEnabled;
            if (cur != vk || modifiers != g_state.hkEnableMods) {
                vk = cur; modifiers = g_state.hkEnableMods;
                prev = (GetAsyncKeyState(vk) & 0x8000) != 0;
            }
            const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
            DWORD foregroundPid = 0;
            GetWindowThreadProcessId(GetForegroundWindow(), &foregroundPid);
            if (down && !prev && !g_hkCapture && foregroundPid == GetCurrentProcessId() &&
                    CurrentHotkeyModifiers() == modifiers) {
                UiToggleEnabled();
                if (NgxEavesdrop::Get().FrameGenerationActive(0)) {
                    g_uiSavePending = false; UiSaveSettings();
                }
            }
            prev = down;
        }
        Sleep(20);
    }
}

static void StartNrHotkeyMonitor() {
    if (g_nrHotkeyRunning.exchange(true)) return;  // 已经在跑
    try {
        std::thread(NrHotkeyThread).detach();
    } catch (...) {
        g_nrHotkeyRunning.store(false);  // 启动失败复位，允许下次重试
    }
}

static void UiBuildPanel(bool escapePressed) {
	std::lock_guard<std::recursive_mutex> nrLock(g_nrStateMutex);
    NrSettings& nr = g_state.nrSettings;
    bool changed = false;

    if (escapePressed) {
        if (ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup)) {
            // The backend polls mouse input only. Forward one Escape pulse to
            // ImGui so an open combo consumes this press before the panel does.
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, true);
            ImGui::GetIO().AddKeyEvent(ImGuiKey_Escape, false);
        } else {
            UiClosePanel();
            return;
        }
    }

    ImGui::SetNextWindowBgAlpha(0.88f);
    // 默认停靠：屏幕上方靠右 —— 窗口右缘约在 85% 宽度处（留出右边距，不贴边），
    // 顶部约 8% 高度。只在该会话首次打开时生效，拖动后的位置会被记住。
    {
        const ImVec2 vs = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowPos(
            ImVec2(vs.x * 0.85f, vs.y * 0.08f),
            ImGuiCond_Once, ImVec2(1.0f, 0.0f));   // pivot = 右上角
    }
    bool panelOpen = g_uiOpen.load(std::memory_order_relaxed);
    const bool visible = ImGui::Begin("DXL - DLSS eXtended Loader", &panelOpen,
                                     ImGuiWindowFlags_AlwaysAutoResize);
    if (!panelOpen) UiClosePanel();
    if (!visible || !panelOpen) {
        ImGui::End();
        return;
    }

    ImGui::TextColored(RTX,
        "DXL - DLSS eXtended Loader  by LCPD15");
    ImGui::TextDisabled("Version %s", kUiVersion);
    ImGui::Separator();

    // master switch (mirrors the DLSS5 toggle hotkey / SetEnabled)
    const bool en = nr.enabled && g_state.enabled.load(std::memory_order_relaxed);
    if (ImGui::Button(en ? "DLSS5: ON" : "DLSS5: OFF")) {
        UiToggleEnabled();
    }
    UiItemHelp(kHelpToggle);
    ImGui::SameLine();
    char enKey[48];
    ImGui::TextDisabled("(%s)",
        VkName(g_state.hkToggleEnabled, enKey, sizeof enKey));

    // input source combo: native vs zero
    int inputs = (nr.useRealDepth && nr.useRealMotion) ? 1 : 0;
    const char* kInputs[] = { "Synthetic guides (optical / zero)", "Native depth + motion" };
    if (ImGui::Combo("Inputs", &inputs, kInputs, 2)) {
        const bool native = inputs != 0;
        g_state.nrSettings.useRealDepth = native;
        g_state.nrSettings.useRealMotion = native;
        UiPushToast("Inputs: %s",
            native ? "native depth+motion" : "zero depth+motion (fallback)");
        UiMarkSave();
    }
    UiItemHelp(kHelpInputs);

    bool optical = nr.opticalFlow;
    if (ImGui::Checkbox("Auto optical flow", &optical)) {
        g_state.nrSettings.opticalFlow = optical;
        if (UsesD3D11Bridge(g_state.api)) g_state.nrFilter11.InvalidateOpticalHistory();
        else g_state.nrFilter.InvalidateOpticalHistory();
        UiMarkSave();
    }
    UiItemHelp(UiText("Estimate motion only when native motion is unavailable or disabled.\nNative motion always takes priority. No optical-flow GPU work while idle.\n仅在没有可用原生矢量时计算光流；原生矢量优先，闲置时不执行光流。自动保存。", "Estimate motion only when native motion is unavailable or disabled.\nNative motion always takes priority. No optical-flow GPU work while idle."));
    ImGui::BeginDisabled(!optical);
    int flowQuality = nr.opticalQuality;
    const char* flowQualities[] = {"Performance", "Balanced", "Quality"};
    if (ImGui::Combo("Optical flow quality", &flowQuality, flowQualities, 3)) {
        g_state.nrSettings.opticalQuality = flowQuality;
        UiMarkSave();
    }
    ImGui::EndDisabled();
    UiItemHelp(UiText("Higher quality uses more pixels for motion estimation.\nLower quality saves GPU work but may miss small objects and fine motion.\n高档提高光流计算分辨率；低档更省性能，小物体和细微运动精度较低。自动保存。", "Higher quality uses more pixels for motion estimation.\nLower quality saves GPU work but may miss small objects and fine motion."));

    ImGui::Separator();

    // status readouts
    const auto captured = NgxEavesdrop::Get().Captured();
    const bool haveCap = captured.frameSeq != 0 && !(g_state.nrAtEvaluate && g_state.nrAutoRoute);
    const auto native = NgxEavesdrop::Get().LatestFrame();
    const bool atEvaluate = ActiveNrFilter().Mode() == NrMode::AtEvaluate;
    const bool haveDepth = atEvaluate ? native.frameSeq && native.depth : haveCap && captured.depth;
    const bool haveMotion = atEvaluate ? native.frameSeq && native.motionVectors : haveCap && captured.motionVectors;
    const bool initTried = UsesD3D11Bridge(g_state.api) ? g_state.nr11Tried : g_state.nrTried.load();
    const bool initialized = UsesD3D11Bridge(g_state.api) ? g_state.nrFilter11.IsInitialized() : g_state.nrInitialized.load();
    const char* state = !initTried ? "waiting..." : initialized ? (ActiveNrFilter().IsReady() ? "ready" : "degraded") : "init failed";
    ImGui::Text("Filter: %s   @ %ux%u", state,
                ActiveNrFilter().Width(), ActiveNrFilter().Height());
    if (g_state.nrAutomaticHandoffBlocked && g_state.enabled.load() &&
        g_state.nrSettings.enabled && g_state.nrAtEvaluate && g_state.nrAutoRoute) {
        ImGui::TextColored(ImVec4(1.0f,.75f,.25f,1.0f),"%s",UiText(
            "NR 暂停：等待游戏 SR 恢复", "NR paused: waiting for native SR"));
        ImGui::TextWrapped("%s",UiText(
            "FG 状态未知，当前呈现链的自动回退未验证；SR 恢复或切换到普通呈现链后自动继续。",
            "FG state is unknown; automatic fallback on this chain is unverified. NR resumes when SR returns or a normal presentation chain replaces it."));
    } else ImGui::Text("Route: %s", atEvaluate ? "SR -> NR" : "Present (display image)");
    if (g_state.nrAutoRoute && g_state.nrAtEvaluate) ImGui::TextDisabled("Automatic route switching");
    ImGui::Text("Render(DLSS) res: %ux%u", native.renderWidth, native.renderHeight);
    ImGui::Text("Depth:  %s", (nr.useRealDepth && haveDepth)
                   ? "native" : "zero");
    if (haveDepth)
        ImGui::SameLine(), ImGui::TextDisabled("(%ux%u)",
            atEvaluate ? native.depthWidth : captured.width, atEvaluate ? native.depthHeight : captured.height);
    const auto opticalStatus = ActiveNrFilter().OpticalStatus();
    ImGui::Text("Motion: %s", opticalStatus.active ? "optical flow (FidelityFX)" : (nr.useRealMotion && haveMotion)
                   ? "native" : "zero");
    if (haveMotion && !opticalStatus.active)
        ImGui::SameLine(), ImGui::TextDisabled("(%ux%u)",
            atEvaluate ? native.motionWidth : captured.width, atEvaluate ? native.motionHeight : captured.height);
    if (opticalStatus.active)
        ImGui::TextDisabled("Flow: %ux%u  GPU %.3f ms  calls %llu", opticalStatus.width, opticalStatus.height,
            opticalStatus.gpuMs, (unsigned long long)opticalStatus.dispatches);
    else if (optical && opticalStatus.error)
        ImGui::TextDisabled("Flow unavailable: %s", opticalStatus.error);
    ImGui::Text("Frames: %llu  skipped: %llu",
                (unsigned long long)ActiveNrFilter().EvaluateCount(),
                (unsigned long long)ActiveNrFilter().SkippedFrames());
    if (g_state.frameMs > 0.0f) {
        const float pct = g_state.frameMs / 16.6f * 100.0f;
        ImGui::Text("Present frame: %.2f ms  (%.0f%% of 16.6ms)",
                    g_state.frameMs, pct);
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Tuning", ImGuiTreeNodeFlags_DefaultOpen)) {
        int preset = nr.preset;
        if (ImGui::Combo("Preset", &preset, kPresetNames,
                         IM_ARRAYSIZE(kPresetNames))) {
            g_state.nrSettings.preset = preset; changed = true;
        }
        UiItemHelp(kHelpPreset);
        int style = nr.style;
        if (ImGui::Combo("Style", &style, kStyleNames, IM_ARRAYSIZE(kStyleNames))) {
            g_state.nrSettings.style = style; changed = true;
        }
        UiItemHelp(kHelpStyle);
        float intensity = nr.intensity;
        if (ImGui::SliderFloat("Intensity", &intensity, 0.0f, 1.0f, "%.2f")) {
            g_state.nrSettings.intensity = intensity; changed = true;
        }
        UiItemHelp(kHelpIntensity);
        float colour = nr.colourStrength;
        if (ImGui::SliderFloat("Colour Strength", &colour, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
            g_state.nrSettings.colourStrength = colour; changed = true;
        }
        UiItemHelp(UiText("0 = original game colours; 1 = current NR colours.\nNR brightness and detail remain. Saved automatically.\n0 保留游戏原始颜色比例，1 保持当前效果；保留 NR 明暗与细节，自动保存。", "0 = original game colours; 1 = current NR colours.\nNR brightness and detail remain. Saved automatically."));
        float tone = nr.localTone;
        if (ImGui::SliderFloat("Local Tone", &tone, 0.0f, 2.0f, "%.2f")) {
            g_state.nrSettings.localTone = tone; changed = true;
        }
        UiItemHelp(kHelpTone);
        float structure = nr.localStructure;
        if (ImGui::SliderFloat("Local Structure", &structure, 0.0f, 2.0f, "%.2f")) {
            g_state.nrSettings.localStructure = structure; changed = true;
        }
        UiItemHelp(kHelpStructure);
        float skin = nr.skinStructure < 0.0f ? 0.0f : nr.skinStructure;
        if (ImGui::SliderFloat("Skin Strength", &skin, 0.0f, 1.0f, "%.2f")) {
            g_state.nrSettings.skinStructure = skin; changed = true;
        }
        UiItemHelp(kHelpSkin);
        bool am = nr.autoMask;
        if (ImGui::Checkbox("Auto Mask", &am)) {
            g_state.nrSettings.autoMask = am; changed = true;
        }
        UiItemHelp(kHelpAutoMask);
        bool ui = nr.uiCorrection;
        if (ImGui::Checkbox("UI Correction", &ui)) {
            g_state.nrSettings.uiCorrection = ui; changed = true;
        }
        UiItemHelp(kHelpUiCorrection);
        static float rs = nr.renderScale;
        static bool editingScale = false;
        if (!editingScale) rs = nr.renderScale;
        const bool editedScale = ImGui::SliderFloat("NR Render Scale", &rs, 0.5f, 1.0f, "%.2f");
        editingScale = ImGui::IsItemActive();
        if (ImGui::IsItemDeactivatedAfterEdit() || (editedScale && !editingScale)) {
            g_state.nrSettings.renderScale = rs; changed = true;
        }
        UiItemHelp(kHelpRenderScale);
        float selfLayers = nr.selfLayers;
        if (ImGui::SliderFloat("Self Layers", &selfLayers, 1.0f, 3.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) {
            g_state.nrSettings.selfLayers = selfLayers; changed = true;
        }
        UiItemHelp(UiText("NR 自叠层：1.00 保持当前效果；1.00–3.00 连续调节 NR 与原图的差值增益。\n不增加模型运算，可与真叠层组合，自动保存。\nContinuous NR residual gain from 1.00 to 3.00; no extra model evaluations.", "Continuous NR residual gain from 1.00 to 3.00; no extra model evaluations."));
        int trueLayers = nr.trueLayers;
        if (ImGui::SliderInt("True NR Layers", &trueLayers, 1, 5, "%d", ImGuiSliderFlags_AlwaysClamp)) {
            g_state.nrSettings.trueLayers = trueLayers; changed = true;
        }
        UiItemHelp(UiText("真 NR 叠层：1–5 次串行模型运算，上一层输出交给下一层，每层独立时域历史。\n增加 GPU 耗时和显存，切换时需要重建。自动保存。\nSerial NR passes with independent histories; higher GPU time and memory use.", "Serial NR passes with independent histories; higher GPU time and memory use."));
#if 0  // —— 手动白点（已废除，保留备用）：改回自动白点（游戏曝光/场景亮度）——
        float wp = nr.toneScale < 0.0f ? 0.0f : nr.toneScale;
        if (ImGui::SliderFloat("White Point (paper white)", &wp, 0.0f, 500.0f,
                               "%.0f")) {
            g_state.nrSettings.toneScale = wp; changed = true;
        }
        UiItemHelp(kHelpWhitePoint);
#endif
    }

    ImGui::Separator();
    // debug view cycle (0..4)
    int dbg = static_cast<int>(nr.debugView);
    const char* kDbg[] = { "Off", "Depth", "Motion", "Encoded", "Diff" };
    if (ImGui::Combo("Debug view", &dbg, kDbg, IM_ARRAYSIZE(kDbg))) {
        g_state.nrSettings.debugView = static_cast<NrSettings::DebugView>(dbg);
        UiPushToast("Debug view: %s", kDbg[dbg]);
        UiMarkSave();
    }
    UiItemHelp(kHelpDebugView);

    ImGui::Separator();
    if (ImGui::CollapsingHeader(UiText("语义蒙板（实验性功能）/ Semantic Mask (Experimental)", "Semantic Mask (Experimental)"))) {
        if (!g_state.semanticAvailable) {
            nr.semanticMask = false;
            ImGui::TextWrapped(UiText("语义组件缺失，请重新解压完整包。\nSemantic components missing; extract the complete package again.", "Semantic Mask components are missing; extract the complete package again."));
            ImGui::TextWrapped(UiText("请在 DXL 的额外功能页面安装语义扩展，然后重启游戏。\nInstall the semantic extension from DXL > Extras, then restart the game.", "Restart the game after restoring the Semantic Mask components."));
        } else {
            if (ImGui::Checkbox(UiText("语义蒙板 / Semantic Mask", "Semantic Mask"), &nr.semanticMask)) UiMarkSave();
            UiItemHelp(UiText("R: 区域强度 / selected region strength.\nG/B/A: 背景强度 / background strength.\n0 = 0, 1 = 255.", "R: selected region strength.\nG/B/A: background strength.\n0 = 0, 1 = 255."));
            if (ImGui::SliderFloat(UiText("背景 / Background (G/B/A)", "Background (G/B/A)"), &nr.semanticBgIntensity, 0.0f, 1.0f, "%.2f", ImGuiSliderFlags_AlwaysClamp)) UiMarkSave();
            const char* groups[SEM_OBJECT_GROUP_COUNT] = {
                UiText("人物\nPeople", "People"), UiText("车辆\nVehicles", "Vehicles"), UiText("动物\nAnimals", "Animals"), UiText("街道设施\nStreet objects", "Street objects"), UiText("运动用品\nSports", "Sports"), UiText("食物\nFood", "Food"), UiText("餐具\nTableware", "Tableware"),
                UiText("家具\nFurniture", "Furniture"), UiText("电子设备\nElectronics", "Electronics"), UiText("家电\nAppliances", "Appliances"), UiText("配饰\nAccessories", "Accessories"), UiText("其他物品\nOther objects", "Other objects")
            };
            constexpr int columns = 3;
            const float gridWidth = std::max(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 25.5f);
            if (ImGui::BeginTable("SemanticGroups", columns,
                    ImGuiTableFlags_BordersInner | ImGuiTableFlags_SizingStretchSame, ImVec2(gridWidth, 0))) {
                for (int g = 0; g < SEM_OBJECT_GROUP_COUNT; ++g) {
                    ImGui::TableNextColumn();
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg,
                        ((g / columns + g % columns) & 1) ? IM_COL32(45,60,75,90) : IM_COL32(25,35,45,70));
                    ImGui::PushID(g);
                    bool on = (nr.semanticEnabled & (1u << g)) != 0;
                    if (ImGui::Checkbox(groups[g], &on)) {
                        nr.semanticEnabled = on ? nr.semanticEnabled | (1u << g) : nr.semanticEnabled & ~(1u << g);
                        UiMarkSave();
                    }
                    if (on) {
                        ImGui::SetNextItemWidth(-FLT_MIN);
                        if (ImGui::SliderFloat("##strength", &nr.semanticIntensity[g], 0.0f, 1.0f,
                                "%.2f", ImGuiSliderFlags_AlwaysClamp)) UiMarkSave();
                        UiItemHelp(UiText("区域强度 / Region strength: 0 - 1", "Region strength: 0 - 1"));
                    }
                    ImGui::PopID();
                }
                ImGui::EndTable();
            }
            ImGui::TextDisabled(UiText("语义蒙板：物体识别。/ Semantic Mask: object recognition.", "Semantic Mask: object recognition."));
            if (nr.semanticMask) {
                if (g_state.segMask.IsReady()) ImGui::Text(UiText("推理 / Inference: %.2f ms", "Inference: %.2f ms"), g_state.segMask.LastInferenceMs());
                else ImGui::TextWrapped(UiText("模型加载中或不可用，使用背景强度。\nModel loading/unavailable: using background strength.", "Model loading/unavailable: using background strength."));
            }
            if (ImGui::Checkbox(UiText("SR 输入上下翻转 / Flip SR semantic input", "Flip SR semantic input"), &nr.semanticFlipY)) UiMarkSave();
            ImGui::TextDisabled(UiText("仅用于倒置的 SR 纹理；Present 自动保持原向。\nSemantic input only; Present remains upright.", "For inverted SR textures; Present remains upright."));
            if (ImGui::SliderFloat(UiText("边缘羽化 / Mask feather", "Mask feather"), &nr.semanticFeather, 0.0f, 8.0f, "%.1f", ImGuiSliderFlags_AlwaysClamp)) UiMarkSave();
            ImGui::TextDisabled(UiText("模型输入 / Model input: 640 x 640 (fixed)", "Model input: 640 x 640 (fixed)"));
            if (ImGui::Checkbox(UiText("蒙板预览 / Mask preview (R)", "Mask preview (R)"), &nr.semanticDebugView)) UiMarkSave();
            if (nr.semanticDebugView) {
                static SemanticMaskSnapshot snapshot;
                g_state.segMask.CopyLatest(snapshot);
                const bool fresh = snapshot.Valid(GetTickCount64());
                constexpr unsigned w = 160;
                const unsigned h = fresh ? std::clamp(unsigned(uint64_t(w)*snapshot.height/snapshot.width), 1u, 120u) : 90u;
                std::vector<uint8_t> pixels(size_t(w)*h*4);
                ComposeSemanticMask(pixels.data(), w*4, w, h, fresh ? &snapshot : nullptr,
                    nr.semanticBgIntensity, nr.semanticIntensity, nr.semanticEnabled, nr.semanticFeather,
                    fresh && snapshot.sourceFlipY);
                const ImVec2 origin = ImGui::GetCursorScreenPos();
                const float cell = 2.0f;
                auto* draw = ImGui::GetWindowDrawList();
                for (unsigned y = 0; y < h; ++y) for (unsigned x = 0; x < w;) {
                    const unsigned red = pixels[(y*w+x)*4];
                    unsigned end=x+1;
                    while (end<w && pixels[(y*w+end)*4]==red) ++end;
                    draw->AddRectFilled(ImVec2(origin.x+x*cell, origin.y+y*cell),
                        ImVec2(origin.x+end*cell, origin.y+(y+1)*cell), IM_COL32(red,red,red,255));
                    x=end;
                }
                ImGui::Dummy(ImVec2(w*cell,h*cell));
                ImGui::TextDisabled(fresh ? UiText("实时 R 通道 / Fresh R channel", "Fresh R channel") : UiText("无新结果：回退背景 / Background fallback", "Background fallback"));
            }
        }
    }

    if (ImGui::CollapsingHeader("Hotkeys", ImGuiTreeNodeFlags_DefaultOpen)) {
        auto row = [](const char* label, int* target, int id) {
            ImGui::Text("%s", label);
            ImGui::SameLine();
            char kb[48];
            const bool capturing = g_hkCapture == id;
            VkName(*target, kb, sizeof kb);
            std::string fullKey;
            const int modifiers = id == 1 ? g_state.hkEnableMods : g_state.hkUiMods;
            if (modifiers & 1) fullKey += "Alt+";
            if (modifiers & 2) fullKey += "Ctrl+";
            if (modifiers & 4) fullKey += "Shift+";
            if (modifiers & 8) fullKey += "Win+";
            fullKey += kb;
            const char* caption = capturing ? "Press a key..." : fullKey.c_str();
            if (ImGui::Button(caption, ImVec2(230, 0))) {
                g_hkCapture = capturing ? 0 : id;   // click again to cancel
            }
            if (ImGui::IsItemHovered()) UiItemHelp(kHelpHotkeyRow);
        };
        row("DLSS5 on/off", &g_state.hkToggleEnabled, 1);
        row("Panel on/off", &g_state.hkToggleUi, 2);
    }

    ImGui::Separator();
    if (ImGui::CollapsingHeader("Frame Gen (NVIDIA Smooth Motion)")) {
        // NVIDIA Smooth Motion（驱动级 AI 插帧）开关。它不碰游戏渲染管线，能和
        // DLSS5 共存。开关是驱动 per-app profile 设置，改完必须重启游戏才生效。
        // -3=未读 -2=NVAPI 不可用 0=关 1=开
        static int smState = -3;
        if (smState == -3) {
            // 首次打开面板时读一次显卡 + 驱动版本 + 当前开关，便于诊断。
            char gpuName[64]{};
            if (NvSmoothMotion::ReadGpuName(gpuName)) {
                D5_LOG_INFO(L"NVAPI GPU：[%hs]", gpuName);
            }
            unsigned int drvVer = 0;
            char drvBranch[64]{};
            if (NvSmoothMotion::ReadDriverVersion(&drvVer, drvBranch)) {
                D5_LOG_INFO(L"NVAPI 驱动版本：0x%08X，分支 [%hs]", drvVer, drvBranch);
            } else {
                D5_LOG_WARN(L"NVAPI 读取驱动版本失败（非 NVIDIA 显卡或驱动异常）");
            }
            int smErr = 0;
            smState = NvSmoothMotion::GetSmoothMotion(&smErr);
            D5_LOG_INFO(L"NVAPI GetSetting(Smooth Motion 0xB0D384C0) 返回 0x%08X", smErr);
            if (smState == -1) smState = -2;
        }
        bool smOn = (smState == 1);
        if (ImGui::Checkbox(UiText("Smooth Motion (AI frame gen)  平滑运动(AI 插帧)", "Smooth Motion (AI frame gen)"), &smOn)) {
            int nvErr = 0;
            const NvSmoothMotion::SmResult r = NvSmoothMotion::SetSmoothMotion(smOn, &nvErr);
            if (r == NvSmoothMotion::SmResult::Ok) {
                smState = smOn ? 1 : 0;
                UiPushToast(smOn
                    ? UiText("Smooth Motion ON — restart the game to apply / 已开启，重启游戏生效", "Smooth Motion ON — restart the game to apply")
                    : UiText("Smooth Motion OFF — restart the game to apply / 已关闭，重启游戏生效", "Smooth Motion OFF — restart the game to apply"));
            } else if (r == NvSmoothMotion::SmResult::NvApiUnavailable) {
                D5_LOG_WARN(L"NVAPI Smooth Motion 切换失败：nvapi64 不可用/初始化失败");
                UiPushToast(UiText("Smooth Motion unavailable / 不可用（需 NVIDIA 显卡 + 新驱动）", "Smooth Motion unavailable (requires a supported NVIDIA GPU and driver)"));
            } else if (r == NvSmoothMotion::SmResult::ProfileUnavailable) {
                D5_LOG_WARN(L"NVAPI Smooth Motion 切换失败：找不到游戏 profile 且无法创建");
                UiPushToast(UiText("Game profile not found / 找不到游戏配置（请在 NVIDIA App 中添加此游戏）", "Game profile not found (add this game in NVIDIA App)"));
            } else {
                D5_LOG_WARN(L"NVAPI Smooth Motion 切换失败：SetSetting/SaveSettings 失败，NVAPI 返回 0x%08X", nvErr);
                UiPushToast(UiText("Smooth Motion toggle failed / 切换失败", "Smooth Motion toggle failed"));
            }
        }
        ImGui::TextDisabled("Driver-level AI frame gen — coexists with DLSS5");
        ImGui::TextDisabled(UiText("驱动级 AI 插帧，可与 DLSS5 共存（改完重启游戏生效）", "Restart the game after changing Smooth Motion."));
    }

    char uiKey[48], enKey2[48];
    ImGui::TextDisabled("%s = panel   %s = DLSS5 on/off   Esc = close",
        VkName(g_state.hkToggleUi, uiKey, sizeof uiKey),
        VkName(g_state.hkToggleEnabled, enKey2, sizeof enKey2));
    ImGui::End();

    if (changed) UiMarkSave();
}

}  // namespace

namespace {

// Bottom-LEFT RTX-style toast stack, drawn with the 3x toast font (the old
// bottom-right spot is usually covered by the Steam popup). New toasts go at
// the bottom, older ones stack upward.
void UiDrawToasts() {
    ImFont* tf = ReUi::FontToast();
    if (!tf) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const ImVec2 vs = ImGui::GetIO().DisplaySize;
    const float padX = 20.0f;
    const float padY = 6.0f;
    const float corner = 8.0f;
    float y = vs.y - 16.0f;
    for (int i = 0; i < (int)(sizeof(g_toasts) / sizeof(g_toasts[0])); ++i) {
        const UiToast& t = g_toasts[i];
        if (t.until <= 0.0) continue;
        const float age = (float)(g_uiClock - t.born);
        if (age < 0.0f || age > (float)(t.until - t.born)) { continue; }
        float alpha = 1.0f;
        if (age > (float)(t.until - t.born) - 0.6f)
            alpha = ((float)(t.until - t.born) - age) / 0.6f;
        const float ts = ReUi::ToastFontSize();
        const ImVec2 sz = tf->CalcTextSizeA(ts, FLT_MAX, 0.0f, t.text, nullptr);
        const float w = sz.x + padX * 2.0f;
        const float h = sz.y + padY * 2.0f;
        const ImVec2 p0(14.0f, y - h);
        const ImVec2 p1(14.0f + w, y);
        dl->AddRectFilled(p0, p1, IM_COL32(10, 12, 8, (int)(235 * alpha)), corner);
        dl->AddRect(p0, p1, IM_COL32(0x76, 0xB9, 0x00, (int)(255 * alpha)), corner);
        dl->AddText(tf, ts,
                    ImVec2(p0.x + padX, p0.y + padY),
                    IM_COL32(0x76, 0xB9, 0x00, (int)(255 * alpha)), t.text);
        y -= h + 8.0f;
    }
}

bool UiAnyToastActive() {
    for (const auto& t : g_toasts) {
        if (t.until > 0.0 && g_uiClock < t.until) return true;
    }
    return false;
}

// Input and persistence remain active even when legacy capture is disabled.
bool UiAdvanceFrameInput() noexcept {
    UpdateSemanticMask();
    LARGE_INTEGER q{}, f{};
    QueryPerformanceCounter(&q);
    QueryPerformanceFrequency(&f);
    if (f.QuadPart) g_uiClock = double(q.QuadPart) / double(f.QuadPart);

    // uiPauseFrames 每帧递减（present 帧），不管这一帧 UI 是否真的提交 —— 否则
    // 换链后若 toast 恰好过期、UI 跳过提交，帧数会一直卡着，之后玩家按 F9 开面板
    // 还要多等 30 帧才显示。
    if (g_state.uiPauseFrames > 0) {
        --g_state.uiPauseFrames;
    }

    // rebind capture runs first; while armed the two hotkeys do nothing
    static bool prevUiKey = false;
    static int previousUiVk = 0, previousUiMods = 0;
    const auto uiKeyHeld = [] {
        return (GetAsyncKeyState(g_state.hkToggleUi) & 0x8000) != 0 &&
            CurrentHotkeyModifiers() == g_state.hkUiMods;
    };
    if (previousUiVk != g_state.hkToggleUi || previousUiMods != g_state.hkUiMods) {
        previousUiVk = g_state.hkToggleUi;
        previousUiMods = g_state.hkUiMods;
        prevUiKey = uiKeyHeld();
    }
    const bool keyFocused = !g_state.trackedWindow ||
        GetForegroundWindow() == g_state.trackedWindow;
    static bool prevEscape = false;
    const bool escapeDown = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
    const bool escapePressed = escapeDown && !prevEscape;
    prevEscape = escapeDown;
    // Remember capture before it consumes Escape: cancelling a binding must
    // never also close the panel on that same press.
    const bool wasCapturing = g_hkCapture != 0;
    if (g_hkCapture && keyFocused) {
        // (失焦时保持待绑状态但不扫描按键，避免在别的窗口里误绑)
        if (g_capArmedFor != g_hkCapture) {
            g_capArmedFor = g_hkCapture;
            for (int v = 0; v < 256; ++v)
                g_capPrev[v] = (GetAsyncKeyState(v) & 0x8000) != 0;
        }
        // find the first key that just went down (edge), ignoring modifiers
        int found = 0;
        for (int vk = 0x08; vk <= 0xFE; ++vk) {
            const bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
            const bool modifier = vk == VK_SHIFT || vk == VK_CONTROL ||
                vk == VK_MENU || (vk >= VK_LSHIFT && vk <= VK_RMENU) ||
                vk == VK_LWIN || vk == VK_RWIN;
            if (modifier) { g_capPrev[vk] = down; continue; }
            if (down && !g_capPrev[vk]) found = vk;
            g_capPrev[vk] = down;
        }
        if (found == VK_ESCAPE) {
            g_hkCapture = 0;
            g_capArmedFor = 0;
            UiPushToast("Hotkey unchanged");
        } else if (found != 0) {
            const char* label = g_hkCapture == 1 ? "DLSS5 on/off" : "Panel";
            int* slot = g_hkCapture == 1
                ? &g_state.hkToggleEnabled : &g_state.hkToggleUi;
            char kb[48];
            VkName(found, kb, sizeof kb);
            *slot = found;
            if (g_hkCapture == 2) g_state.hkUiMods = CurrentHotkeyModifiers();
            else g_state.hkEnableMods = CurrentHotkeyModifiers();
            MarkHotkeyRevision();
            g_hkCapture = 0;
            g_capArmedFor = 0;
            UiPushToast("Hotkey %s: %s", label, kb);
            UiMarkSave();
        }
        // Capturing a key must not also activate that key on the next frame.
        prevUiKey = uiKeyHeld();
    } else if (keyFocused) {
        // custom hotkeys (config hkEnable/hkUi, rebindable in the panel)
        const bool uiKey = uiKeyHeld();
        if (uiKey && !prevUiKey) UiTogglePanel();
        prevUiKey = uiKey;
        // F8（切 NR）已经挪到独立的 NrHotkeyThread 里（始终运行），不再在这里轮询——
        // 这样 FG 换链后（present 链没挂新 swapchain 的 hook）F8 依然能切 NR。
    } else prevUiKey = uiKeyHeld();

    // DLSS5 enable edge (F8 / UI / SetEnabled) -> toast
    const bool en = g_state.nrSettings.enabled &&
                    g_state.enabled.load(std::memory_order_relaxed);
    if (en != g_lastUiEnabled) {
        g_lastUiEnabled = en;
        UiPushToast("DLSS5 %s", en ? "ON" : "OFF");
    }

    // one-shot "ready" toast once the filter is up
    if (!g_uiReadyToast && ActiveNrFilter().IsReady()) {
        g_uiReadyToast = true;
        UiPushToast("DLSS5 NR ready");
    }

    // debounced settings persistence
    if (g_uiSavePending && (g_uiClock - g_lastSave) > 0.8) {
        g_uiSavePending = false;
        g_lastSave = g_uiClock;
        UiSaveSettings();
    }

    // Closing by hotkey must release cursor ownership even when capture stops.
    ReUi::SetUiOpen(&g_uiOpen);
    return keyFocused && escapePressed && !wasCapturing;
}

bool UiMayDraw() noexcept {
    const bool open = g_uiOpen.load(std::memory_order_relaxed);
    if (!open && !UiAnyToastActive()) return false;

    // 游戏刚重建 swapchain / ResizeBuffers：换链的同步窗口里，UI 往游戏队列提交
    // imgui 命令会触发设备丢失（MHW 实测）。暂停一小段，等 DXGI 把新链稳定下来。
    if (g_state.uiPauseUntil &&
        GetTickCount64() < g_state.uiPauseUntil) {
        return false;
    }

    // 按 present 帧数的换链保护：鬼武者切 4K 时 present 会停约 3 秒，上面的
    // uiPauseUntil（真实时间）早就过期了，但 toast 倒计时用的是 present 时间
    // （没走），恢复后 UI 提交照发 → 撞窗。这里用帧数（present 停了就不递减），
    // 恢复后再等满 30 帧才放行 UI 提交。（递减已提到函数开头每帧做，这里只检查）
    if (g_state.uiPauseFrames > 0) {
        return false;
    }

    return true;
}

// Called after NR and before the native presentation.
void UiFramePresent(IDXGISwapChain* swapChain) noexcept {
    std::lock_guard<std::recursive_mutex> nrLock(g_nrStateMutex);
    // A queue cached at startup is not the queue presenting FG frames. Only
    // render on a chain whose actual factory queue we recorded successfully.
    DXGI_SWAP_CHAIN_DESC desc{};
    if (FAILED(swapChain->GetDesc(&desc))) return;
    if (!AcceptDxgiPresentation(g_state.api, desc.OutputWindow,
            desc.BufferDesc.Width, desc.BufferDesc.Height)) return;
    bool ready = false;
    if (g_state.api == GraphicsApi::D3D11 && g_state.device11) {
        ID3D11DeviceContext* context = nullptr;
        g_state.device11->GetImmediateContext(&context);
        ready = ReUi::InitOnce11(g_state.device11, context, desc.OutputWindow, desc.BufferDesc.Format);
        if (context) context->Release();
    } else {
        ID3D12CommandQueue* queue = nullptr;
        UINT bytes = sizeof(queue);
        if (SUCCEEDED(swapChain->GetPrivateData(NR_PRESENT_QUEUE, &bytes, &queue)) && queue) {
            ID3D12Device* device = nullptr;
            if (SUCCEEDED(queue->GetDevice(IID_PPV_ARGS(&device)))) {
                ready = ReUi::InitOnce(device, queue, desc.OutputWindow, desc.BufferDesc.Format);
                device->Release();
            }
            queue->Release();
        } else {
            static unsigned missingQueueReports = 0;
            static IDXGISwapChain* lastMissingQueueChain = nullptr;
            if (lastMissingQueueChain != swapChain && missingQueueReports < 4) {
                lastMissingQueueChain = swapChain;
                ++missingQueueReports;
                D5_LOG_INFO(L"ImGui waiting for confirmed presentation queue: chain=%p hwnd=%p; "
                    L"late-load submission queue alone does not establish FG/UI ownership",
                    swapChain, desc.OutputWindow);
            }
        }
    }
    if (!ready) return;
    ReUi::SetUiOpen(&g_uiOpen);
    if (!g_uiInitialised) {
        g_uiInitialised = true;
        UiPushToastLong("DXL ready - DLSS eXtended Loader");
    }
    if (!g_uiInitialised) return;

    const bool closeOnEscape = UiAdvanceFrameInput();
    if (!UiMayDraw()) return;
    ReUi::FramePresent(swapChain, [closeOnEscape] {
        UiDrawToasts();
        if (g_uiOpen.load(std::memory_order_relaxed)) UiBuildPanel(closeOnEscape);
    });
}

bool g_legacyCloseOnEscape = false;
bool BeginLegacyFrame(GraphicsApi api, HWND window) noexcept {
    std::lock_guard<std::recursive_mutex> lock(g_nrStateMutex);
    if (IsTeardownRequested()) return false;
    // Do not redirect a process already presenting through native DXGI.
    if (g_state.api != GraphicsApi::Unknown && !UsesD3D11Bridge(g_state.api)) return false;
    if (g_state.api == GraphicsApi::D3D11) return false;
    if (g_state.api != api) {
        g_state.api = api;
        g_state.apiLogged = true;
        ChainInjectStopAfterGraphics();
        D5_LOG_INFO(L"Legacy presentation active: %ls", api == GraphicsApi::D3D9 ? L"D3D9" : L"OpenGL");
    }
    g_state.trackedWindow = window;
    const auto count = g_state.presentCount.fetch_add(1) + 1;
    LARGE_INTEGER now{}; QueryPerformanceCounter(&now);
    if (g_state.lastPresentQpc.QuadPart && g_state.qpcFreq.QuadPart) {
        const float ms = float(double(now.QuadPart - g_state.lastPresentQpc.QuadPart) * 1000.0 / g_state.qpcFreq.QuadPart);
        g_state.frameMs = g_state.frameMs > 0 ? g_state.frameMs + (ms - g_state.frameMs) * 0.1f : ms;
    }
    g_state.lastPresentQpc = now;
    ApplyPendingSettings();
    g_legacyCloseOnEscape = UiAdvanceFrameInput() || g_legacyCloseOnEscape;
    if (count == 1 || count % 120 == 0) g_publisher.Publish();
    return (g_state.enabled.load() && g_state.nrSettings.enabled) ||
        (!g_state.noUiPresent && (g_uiOpen.load() || UiAnyToastActive()));
}

bool ProcessLegacyFrame(ID3D11Device* device, ID3D11Texture2D* image,
                        GraphicsApi api, HWND window) noexcept {
    std::lock_guard<std::recursive_mutex> lock(g_nrStateMutex);
    if (!device || !image || IsTeardownRequested() || g_state.api != api) return false;
    if (g_state.device11 && g_state.device11 != device) return false;
    if (!g_state.device11) { g_state.device11 = device; device->AddRef(); }
    D3D11_TEXTURE2D_DESC desc{}; image->GetDesc(&desc);
    g_state.width = desc.Width; g_state.height = desc.Height;
    bool processed = false;
    if (g_state.enabled.load() && g_state.nrSettings.enabled) {
        if (!g_state.nr11Tried) {
            g_state.nr11Tried = true;
            if (!g_state.nrFilter11.Initialize(device, g_state.selfModule))
                g_publisher.Publish(g_state.nrFilter11.LastError());
        }
        if (g_state.nrFilter11.IsInitialized() && !g_state.nrFilter11.IsDisabled()) {
            UpdateSemanticMask();
            processed = g_state.nrFilter11.Execute(image, g_state.nrSettings);
        }
    }
    if (!g_state.noUiPresent) {
        ID3D11DeviceContext* context = nullptr; device->GetImmediateContext(&context);
        const bool ready = ReUi::InitOnce11(device, context, window, desc.Format);
        if (context) context->Release();
        if (ready) {
            ReUi::SetUiOpen(&g_uiOpen);
            if (!g_uiInitialised) {
                g_uiInitialised = true;
                UiPushToastLong("DXL ready - DLSS eXtended Loader");
            }
            if (UiMayDraw()) {
                ReUi::FrameTexture11(image, [] {
                    UiDrawToasts();
                    if (g_uiOpen.load()) UiBuildPanel(g_legacyCloseOnEscape);
                    g_legacyCloseOnEscape = false;
                });
                processed = true;
            }
        }
    }
    return processed;
}

}  // namespace

void OnPresent(IDXGISwapChain* swapChain) noexcept {
	// HookedPresent/Present1 already accepted the shared NR/UI owner.

	const uint64_t count = g_state.presentCount.fetch_add(1) + 1;

	// 帧时间
	LARGE_INTEGER now{};
	QueryPerformanceCounter(&now);
	if (g_state.lastPresentQpc.QuadPart && g_state.qpcFreq.QuadPart) {
		const double ms = double(now.QuadPart - g_state.lastPresentQpc.QuadPart) *
			1000.0 / double(g_state.qpcFreq.QuadPart);
		// 轻度平滑，UI 上读起来稳定些
		g_state.frameMs = g_state.frameMs > 0.0f ?
			g_state.frameMs + float(ms - g_state.frameMs) * 0.1f : float(ms);
	}
	g_state.lastPresentQpc = now;

    ApplyPendingSettings();

	TryInstallDepthTracker();
	RunFilters(swapChain);
	D5_STAGE(DepthEndFrame);

	// 夹具：把"代理尺寸被无视"的恢复路径在夹具上走一遍。见 diagBanProxyAt。
	if (g_state.banProxyAt &&
		g_state.presentCount.load(std::memory_order_relaxed) ==
			g_state.banProxyAt &&
		g_state.scalerActive.load(std::memory_order_acquire)) {
		D5_LOG_WARN(L"诊断夹具触发：假装代理尺寸被无视，开始撤代理");
		g_state.proxySizeIgnored.store(true, std::memory_order_release);
		BanProxyAndRecover();
	}

	// 归档本帧的深度使用情况并重新挑选主深度。渲染分辨率是最强的筛选条件，
	// 所以用超分那边算出来的 render 尺寸；还没准备好就退回 swapchain 尺寸。
	DepthTracker& depthTracker = GetDepthTracker();
	depthTracker.EndFrame(
		g_state.upscaler.IsReady() ? g_state.upscaler.RenderWidth() : g_state.width,
		g_state.upscaler.IsReady() ? g_state.upscaler.RenderHeight() : g_state.height);

	// 代理生效时，游戏的主深度缓冲**应该**和代理一样大 —— 因为它本该按我们报的
	// 尺寸渲染。深度明显更大就说明它压根没看 swapchain 的 desc，而是按自己配置里
	// 的分辨率渲染，于是全分辨率的画面被写进了小代理，只有左上角装得进去。
	// 这种情况下真超分不可能正确，必须告诉用户而不是给他一张裁切放大的画面。
	if (g_state.scalerActive.load(std::memory_order_acquire) &&
		depthTracker.Selected() && g_state.scaler.ProxyWidth()) {
		const uint32_t depthWidth = depthTracker.SelectedWidth();
		const uint32_t proxyWidth = g_state.scaler.ProxyWidth();
		// 留 15% 余量：有些引擎会把深度对齐到 8/16 的倍数
		const bool ignored = depthWidth > proxyWidth + proxyWidth / 7;
		if (ignored && !g_state.proxySizeIgnored.exchange(true)) {
			D5_LOG_ERROR(L"这个游戏无视了我们报的 swapchain 尺寸：代理是 %ux%u，"
				L"但它的主深度缓冲是 %ux%u —— 说明它按自己配置里的分辨率渲染。"
				L"真超分在这个引擎上无法正确工作，画面会是左上角的裁切。"
				L"请改用 DLAA 并重启游戏。",
				proxyWidth, g_state.scaler.ProxyHeight(),
				depthWidth, depthTracker.SelectedHeight());
			g_publisher.Publish(
				"这个游戏按自己的分辨率渲染，无视了真超分改的尺寸 —— "
				"正在自动撤掉代理、恢复正常画面（这一局的真超分不可用）。");
			BanProxyAndRecover();
		}
	}

	// 「谁在调 NGX」探针。跑两次：
	//   · 首帧 —— 那时游戏的 DLSS 可能还没起来，作为基线
	//   · 大约 30 秒后 —— 玩家这时通常已经进了游戏、画质设置也应用完了，
	//     游戏自己的 DLSS snippet 该加载的都加载了
	// 只读不写：枚举模块 + 扫导入表 + 打日志。下一步要旁听游戏的 DLSS 调用，
	// 而拦截点取决于"谁在调、静态导入还是 GetProcAddress"，这个只能实测。
	if (count == 1) {
		LogNgxCallers(L"首帧");
	} else if (count == 1800) {
		LogNgxCallers(L"约 30 秒后（游戏的 DLSS 此时应该已经跑起来）");
	}

	if (count % 120 == 0) {
		g_publisher.Publish();
	}

	// 夹具：用假矢量把"重采样"那条路在开着调试层的测试目标上跑起来。
	// 测试目标不跑 DLSS，所以没有真矢量可抄 —— 没有这个夹具，那段 dispatch 只能
	// 第一次上真游戏才被执行。而未经调试层检验的 GPU 代码正是这个项目栽过跟头的地方。
	if (g_state.fakeMotionAt && count >= g_state.fakeMotionAt &&
		g_state.nrInitialized) {
		if (!g_state.fakeMotion) g_state.fakeMotion = CreateFakeMotion();
		if (g_state.fakeMotion) {
			// scale 故意设成等于纹理尺寸 —— 那正是 CanResampleMotion 认的 UV 约定
			g_state.nrFilter.SetExternalMotion(
				g_state.fakeMotion, 1286, 724, 1286.0f, 724.0f);
		}
	}

	// 夹具：把 evaluate 点那条路在**开着调试层的测试目标**上走一遍。
	// 真实游戏里没有调试层，barrier 的 before 状态写错不会报错、只会偶尔闪一下 ——
	// 所以这条路的正确性必须在这里验。
	if (g_state.fakeMotionAt && count >= g_state.fakeMotionAt &&
		g_state.nrInitialized) {
		if (!g_state.fakeMotion) g_state.fakeMotion = CreateFakeMotion();
		if (g_state.fakeMotion) {
			// scale 故意设成等于纹理尺寸 —— 那是 CanResampleMotion 认的 UV 约定
			g_state.nrFilter.SetExternalMotion(
				g_state.fakeMotion, 1286, 724, 1286.0f, 724.0f);
		}
	}

	if (g_state.selfTestAtEvaluate) {
		const bool lie = g_state.selfTestAtEvaluate < 0;
		const uint64_t at = uint64_t(lie ? -g_state.selfTestAtEvaluate
			: g_state.selfTestAtEvaluate);
		if (count == at) {
			NgxEavesdrop::Get().SelfTestPreEvaluate(
				g_state.device12, g_state.capturedQueue, lie);
			// 自检里等过 fence 了，回读缓冲一定有数据 —— 直接倒出来，
			// 不然它要等 60 个 at-evaluate 帧，而夹具一辈子只跑一帧
			g_state.nrFilter.FlushPixelDump();
		}
	}
	// 这里只是**省掉一次 Snapshot**：数量和选中指针都没动，表肯定没变。
	// 真正的去重在 LogDepthCandidates 里（按尺寸/格式签名）—— 这一层挡不住
	// RE Engine 那种"选中指针每帧乱跳但尺寸一直没变"的情况，别只依赖它。
	{
		static uint32_t lastCandidateCount = 0;
		static ID3D12Resource* lastSelected = nullptr;
		DepthTracker& depth = GetDepthTracker();
		if (depth.CandidateCount() != lastCandidateCount ||
			depth.Selected() != lastSelected) {
			lastCandidateCount = depth.CandidateCount();
			lastSelected = depth.Selected();
			LogDepthCandidates();
		}
	}
}

/* ============================ hook 实现 ============================ */

// PresentEnter/Mark(PresentOriginal)/PresentExit 这三个标记的位置很关键：看门狗
// 就靠它们区分"卡在我们的代码里"和"卡在游戏原本的 Present 里"。别挪。
void ObservePresentChain(IDXGISwapChain* chain) {
    if (FrameGenSwapChains::Get().Contains(chain)) return;
    DXGI_SWAP_CHAIN_DESC desc{};
    if (SUCCEEDED(SwapOriginal(chain, VT_SWAPCHAIN_GET_DESC, g_originalGetDesc)(chain, &desc)))
        FrameGenSwapChains::Get().Observe(chain, desc.BufferCount);
}

// Present1 can forward to Present, and wrappers can call a native chain.
// Only the outer presentation callback performs NR/UI work.
static thread_local unsigned g_presentDepth = 0;
struct PresentScope { PresentScope() { ++g_presentDepth; } ~PresentScope() { --g_presentDepth; } };
HRESULT STDMETHODCALLTYPE HookedPresent(
    PresentFn original, IDXGISwapChain* swapChain, UINT syncInterval, UINT flags) {
    PresentScope scope;
    if (g_presentDepth > 1 || (flags & DXGI_PRESENT_TEST) || IsTeardownRequested())
        return original(swapChain, syncInterval, flags);
    ApplyPendingSettings();
    if (!DetectApiFromSwapChain(swapChain)) return original(swapChain, syncInterval, flags);
    ObservePresentChain(swapChain);
    const bool fg = NgxEavesdrop::Get().FrameGenerationActive(0);
    auto& watchdog = FreezeWatchdog::Get();
    if (!fg) { watchdog.PresentEnter(); OnPresent(swapChain); }
    if (!g_state.noUiPresent) UiFramePresent(swapChain);
    if (!fg) watchdog.Mark(Stage::PresentOriginal);
    const HRESULT hr = original(swapChain, syncInterval, flags);
    if (!fg) watchdog.PresentExit();
    return hr;
}

HRESULT STDMETHODCALLTYPE HookedPresent1(
    Present1Fn original, IDXGISwapChain1* swapChain, UINT syncInterval, UINT flags,
    const DXGI_PRESENT_PARAMETERS* params) {
    PresentScope scope;
    if (g_presentDepth > 1 || (flags & DXGI_PRESENT_TEST) || IsTeardownRequested())
        return original(swapChain, syncInterval, flags, params);
    ApplyPendingSettings();
    if (!DetectApiFromSwapChain(swapChain)) return original(swapChain, syncInterval, flags, params);
    ObservePresentChain(swapChain);
    const bool fg = NgxEavesdrop::Get().FrameGenerationActive(0);
    auto& watchdog = FreezeWatchdog::Get();
    if (!fg) { watchdog.PresentEnter(); OnPresent(swapChain); }
    if (!g_state.noUiPresent) UiFramePresent(swapChain);
    if (!fg) watchdog.Mark(Stage::PresentOriginal);
    const HRESULT hr = original(swapChain, syncInterval, flags, params);
    if (!fg) watchdog.PresentExit();
    return hr;
}

/* ==================== 真超分：代理 backbuffer 的几个 hook ==================== */

// 给游戏的是代理纹理而不是真 backbuffer。索引对齐真 swapchain 的索引，
// 所以 GetCurrentBackBufferIndex 不用改。
HRESULT STDMETHODCALLTYPE HookedGetBuffer(
	IDXGISwapChain* swapChain, UINT index, REFIID riid, void** surface) {
	// FG 下纯转发：真超分代理已退场（SR 本来就该关），不碰 GetBuffer
	if (NgxEavesdrop::Get().FrameGenerationActive(0)) {
		return SwapOriginal(swapChain, VT_SWAPCHAIN_GET_BUFFER, g_originalGetBuffer)(swapChain, index, riid, surface);
	}
	if (swapChain == g_state.scaledSwapChain && g_state.scaler.IsActive()) {
		ID3D12Resource* proxy = g_state.scaler.ProxyBuffer(index);
		if (proxy && surface) {
			// 走 QueryInterface 而不是直接给指针：调用方可能要的是
			// ID3D12Resource1 之类，而且这样引用计数也对
			return proxy->QueryInterface(riid, surface);
		}
	}
	return SwapOriginal(swapChain, VT_SWAPCHAIN_GET_BUFFER, g_originalGetBuffer)(swapChain, index, riid, surface);
}

// 游戏读 desc 是为了知道"我该按多大渲染"，所以这里必须报代理尺寸。
// 这是真超分唯一"撒谎"的地方，也是它比 DLAA 风险高的原因。
HRESULT STDMETHODCALLTYPE HookedGetDesc(
	IDXGISwapChain* swapChain, DXGI_SWAP_CHAIN_DESC* desc) {
	if (NgxEavesdrop::Get().FrameGenerationActive(0)) {
		return SwapOriginal(swapChain, VT_SWAPCHAIN_GET_DESC, g_originalGetDesc)(swapChain, desc);
	}
	static std::atomic<bool> logged{ false };
	if (!logged.exchange(true)) {
		D5_LOG_INFO(L"HookedGetDesc 首次被调用 (this=%p vtable=%p)",
			swapChain, *reinterpret_cast<void**>(swapChain));
	}
	const HRESULT hr = SwapOriginal(swapChain, VT_SWAPCHAIN_GET_DESC, g_originalGetDesc)(swapChain, desc);
	if (SUCCEEDED(hr) && desc && swapChain == g_state.scaledSwapChain &&
		g_state.scaler.IsActive()) {
		desc->BufferDesc.Width = g_state.scaler.ProxyWidth();
		desc->BufferDesc.Height = g_state.scaler.ProxyHeight();
	}
	return hr;
}

HRESULT STDMETHODCALLTYPE HookedGetDesc1(
	IDXGISwapChain1* swapChain, DXGI_SWAP_CHAIN_DESC1* desc) {
	if (NgxEavesdrop::Get().FrameGenerationActive(0)) {
		return SwapOriginal(swapChain, VT_SWAPCHAIN_GET_DESC1, g_originalGetDesc1)(swapChain, desc);
	}
	const HRESULT hr = SwapOriginal(swapChain, VT_SWAPCHAIN_GET_DESC1, g_originalGetDesc1)(swapChain, desc);
	const bool matches = swapChain == g_state.scaledSwapChain;
	const bool active = g_state.scaler.IsActive();
	if (SUCCEEDED(hr) && desc && matches && active) {
		desc->Width = g_state.scaler.ProxyWidth();
		desc->Height = g_state.scaler.ProxyHeight();
	}
	// 只记前几次：判断错了会让游戏拿着大视口往小 RT 上画，是最容易花屏的地方
	static std::atomic<int> logCount{ 0 };
	if (logCount.fetch_add(1) < 4) {
		D5_LOG_INFO(L"HookedGetDesc1: this=%p scaled=%p 匹配=%d 激活=%d "
			L"-> 报告 %ux%u", swapChain, g_state.scaledSwapChain,
			matches ? 1 : 0, active ? 1 : 0,
			desc ? desc->Width : 0, desc ? desc->Height : 0);
	}
	return hr;
}

// 建立/重建代理。targetWidth/Height 为 0 表示"按窗口客户区尺寸"。
void SetupScaler(
	IDXGISwapChain* swapChain, uint32_t targetWidth, uint32_t targetHeight) noexcept {
	g_state.scaler.Teardown();
	g_state.scaledSwapChain = nullptr;
	g_state.scalerActive.store(false, std::memory_order_release);

	const SrSettings& settings = g_state.srSettings;
	if (!settings.enabled || settings.mode != SrMode::Upscale) return;
	if (!g_state.device12) return;

	// 这个游戏已经被证明不看 swapchain 的 desc（见 BanProxyAndRecover）——
	// 再建代理就是再给它一张裁切画面。
	if (g_state.proxyBanned.load(std::memory_order_acquire)) {
		D5_LOG_INFO(L"代理对本游戏已禁用（它按自己的分辨率渲染），继续走 DLAA");
		return;
	}

	// **总开关关着就不建代理。**
	//
	// 代理一交给游戏就拆不掉（游戏持着那些 RTV），而它活着的时候我们就必须每帧
	// 帮它缩放 —— 也就是说"总开关关掉"在代理存在的前提下没法真正做到"什么都不做"。
	// 默认关总开关之后这条更要紧：玩家还没按 Alt+D，画面就已经被我们接管了。
	// 按下 Alt+D 之后 SetEnabled 那边会顺手 nudge 一次窗口，代理在那时才建。
	if (!g_state.enabled.load(std::memory_order_acquire)) {
		D5_LOG_INFO(L"总开关关着，不建真超分代理（按 Alt+D 打开后会自动重建）");
		return;
	}

	// **绝不建一组自己驱动不了的 backbuffer。**
	//
	// 代理是在这里（CreateSwapChainForHwnd 里）建的，比我们 init NGX 早好几秒。
	// 如果那之后 NGX 因为"游戏自己在跑 DLSS"被守卫拒掉，SR 就永远起不来 —— 而 SR 是
	// 这条链上**唯一**把代理放大回真 backbuffer 的环节。结果：游戏一直往 640x360 的
	// 代理上渲染，真 backbuffer 从来没人写，**画面全黑**。
	// 鬼武者实测：1595 帧"代理生效但 SR 没运行"，玩家看到的就是黑屏，而且反复改分辨率
	// 必然复现（每次 ResizeBuffers 都重建代理，撞上这个状态）。
	// 所以建代理之前必须先问一句 —— 拒绝真超分只是少个功能，黑屏是彻底不能用。
	std::string owner;
	if (GameAlreadyOwnsNgx(&owner) && !g_state.allowNgxCoexist) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			D5_LOG_ERROR(L"游戏自己在跑 DLSS（进程里有 %hs）—— **不建代理 backbuffer**。"
				L"我们的 NGX 会话会被守卫拒掉，那样 SR 就没人把代理放大回真 backbuffer，"
				L"画面会全黑。请在游戏里关掉 DLSS 再用真超分，或者只开 DLSS5。",
				owner.c_str());
			g_publisher.Publish(
				"游戏自己在跑 DLSS，真超分已停用（否则画面会全黑）。"
				"请在游戏里关掉 DLSS，或只开 DLSS5。");
		}
		return;
	}

	// 真 swapchain 的实际尺寸和格式要从真的 desc 拿（绕过我们自己的谎）
	DXGI_SWAP_CHAIN_DESC actual{};
	if (!g_originalGetDesc || FAILED(SwapOriginal(swapChain, VT_SWAPCHAIN_GET_DESC, g_originalGetDesc)(swapChain, &actual))) return;

	// 目标分辨率优先用窗口客户区，而不是调用方传进来的尺寸。
	// 为什么：代理生效后我们对游戏谎报的是代理尺寸，如果游戏 ResizeBuffers 时
	// 传的是它从 GetDesc 读回来的值，拿它当目标就会**一次次缩小**（每次再乘一遍
	// 倍率）。窗口客户区是外部事实，不受我们的谎影响。
	RECT client{};
	if (actual.OutputWindow && GetClientRect(actual.OutputWindow, &client) &&
		client.right > client.left && client.bottom > client.top) {
		targetWidth = uint32_t(client.right - client.left);
		targetHeight = uint32_t(client.bottom - client.top);
	} else if (!targetWidth || !targetHeight) {
		// 拿不到窗口（独占全屏等）时退回真 swapchain 的尺寸
		targetWidth = actual.BufferDesc.Width;
		targetHeight = actual.BufferDesc.Height;
	}

	if (!g_state.scaler.Setup(g_state.device12, actual.BufferCount,
		actual.BufferDesc.Format, targetWidth, targetHeight,
		settings.inputMultiplier)) {
		return;
	}
	g_state.scaledSwapChain = swapChain;
	g_state.scalerActive.store(true, std::memory_order_release);
	// **代理建起来了就把之前那句"降级 DLAA"撤掉。**
	// 那条提示是一次性的，不撤的话界面上会一直挂着一句已经不成立的话 ——
	// 玩家刚按 Alt+D 打开真超分，读到的却是"真超分未生效"。
	if (g_srDegradeWarned) {
		g_srDegradeWarned = false;
		g_publisher.Publish("真超分已生效");
	}
}

// 游戏改分辨率时会走这里。它给的尺寸是它心里的"输出分辨率"，所以就是我们的
// 目标尺寸；真 swapchain 按这个尺寸重建，代理再按倍率缩下去。
template<class Resize>
HRESULT ResizeTracked(
	IDXGISwapChain* swapChain, UINT bufferCount, UINT width, UINT height,
	DXGI_FORMAT format, UINT flags, Resize&& resize) {
    { std::lock_guard<std::recursive_mutex> lock(g_nrStateMutex);
      if (!ReUi::WaitIdle()) return DXGI_ERROR_WAS_STILL_DRAWING; }
    ObservePresentChain(swapChain);
    FrameGenSwapChains::Creation pending(bufferCount);
    // FG retains control of resizing; observe only after it succeeds.
    if (NgxEavesdrop::Get().FrameGenerationActive(0)) {
        const HRESULT hr = resize();
        if (SUCCEEDED(hr)) {
            NgxEavesdrop::Get().PauseCaptureFor(500);
            NgxEavesdrop::Get().NoteSwapChainRebuilt();
            FrameGenSwapChains::Get().Observe(swapChain, bufferCount, true);
        }
        return hr;
    }
	const bool wasScaled =
		swapChain == g_state.scaledSwapChain && g_state.scaler.IsActive();
	if (wasScaled) {
		// 代理纹理不属于 swapchain，但要先释放：尺寸变了它们全都得重建
		g_state.scaler.Teardown();
		g_state.scaledSwapChain = nullptr;
		g_state.scalerActive.store(false, std::memory_order_release);
	}

	D5_EVENT_D(ResizeBuffers, (uint64_t(width) << 32) | height);
	const HRESULT hr = resize();
	if (FAILED(hr)) {
		// 失败几乎总是"还有人持有 backbuffer 引用"。如果是我们持有的，游戏就再也
		// 换不了尺寸，画面会停在旧内容上 —— 必须记下来而不是静默转发。
		D5_LOG_ERROR(L"ResizeBuffers(%u, %ux%u, fmt=%u, flags=0x%X) 失败: 0x%08X"
			L"（常见原因：还有 backbuffer 引用没释放）",
			bufferCount, width, height, (unsigned)format, flags, hr);
	}

	if (SUCCEEDED(hr)) {
        FrameGenSwapChains::Get().Observe(swapChain, bufferCount, true);
		// 尺寸变了，游戏的深度缓冲全都会重建 —— 旧候选留着只会误导启发式
		GetDepthTracker().Invalidate();
		// 同理，换链期间让 UI 暂停一小段，避开 DXGI 的同步窗口。
		g_state.uiPauseUntil = GetTickCount64() + 500;
		// UI 的换链保护按 present 帧数再补一道（见 uiPauseFrames 字段说明）。
		g_state.uiPauseFrames = 120;
		// 旁听拷贝同样要停：它往游戏的命令列表录 barrier+copy，一样撞换链窗口。
		NgxEavesdrop::Get().PauseCaptureFor(500);
		// swapchain 重建 = 换链信号：让 evaluate 点 NR 暂停 + 强制 reset。
		NgxEavesdrop::Get().NoteSwapChainRebuilt();
	}

	// 同理，这里也要连 enabled 一起判，否则 SR 关着也会打一行"重建代理"
	if (SUCCEEDED(hr) && (wasScaled || (g_state.srSettings.enabled &&
		g_state.srSettings.mode == SrMode::Upscale))) {
		D5_LOG_INFO(L"ResizeBuffers %ux%u -> 重建代理", width, height);
		SetupScaler(swapChain, width, height);
	}
	return hr;
}

HRESULT STDMETHODCALLTYPE HookedResizeBuffers(IDXGISwapChain* swapChain, UINT bufferCount,
    UINT width, UINT height, DXGI_FORMAT format, UINT flags) {
    return ResizeTracked(swapChain, bufferCount, width, height, format, flags, [&] {
        return SwapOriginal(swapChain, VT_SWAPCHAIN_RESIZE_BUFFERS, g_originalResizeBuffers)(
            swapChain, bufferCount, width, height, format, flags);
    });
}

HRESULT STDMETHODCALLTYPE HookedResizeBuffers1(IDXGISwapChain3* swapChain, UINT bufferCount,
    UINT width, UINT height, DXGI_FORMAT format, UINT flags,
    const UINT* nodeMasks, IUnknown* const* presentQueues) {
    return ResizeTracked(swapChain, bufferCount, width, height, format, flags, [&] {
        return SwapOriginal(swapChain, VT_SWAPCHAIN_RESIZE_BUFFERS1, g_originalResizeBuffers1)(
            swapChain, bufferCount, width, height, format, flags, nodeMasks, presentQueues);
    });
}

HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForHwnd(
	IDXGIFactory2* factory, IUnknown* device, HWND hwnd,
	const DXGI_SWAP_CHAIN_DESC1* desc,
	const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
	IDXGIOutput* restrictToOutput, IDXGISwapChain1** swapChain) {
	// 这是我们自己的探测链，纯转发 —— 别当成游戏建链记进日志和面包屑
	if (g_creatingProbe.load(std::memory_order_relaxed)) {
		return g_originalCreateSwapChainForHwnd(factory, device, hwnd, desc,
			fullscreenDesc, restrictToOutput, swapChain);
	}
    if (!AcceptDxgiCreation(device, hwnd, desc ? desc->Width : 0, desc ? desc->Height : 0))
        return g_originalCreateSwapChainForHwnd(factory, device, hwnd, desc,
            fullscreenDesc, restrictToOutput, swapChain);

	// D3D12 下这个 pDevice 就是 command queue —— 这是拿到它的唯一正规途径。
	// **必须在 FG 检测之前抓**：开着 FG 启动游戏时第一次 swapchain 就是
	// BufferCount>=阈值，FG 检测分支会纯转发 return；如果 queue 抓取放在那个
	// return 之后，device/queue 就永远拿不到，evaluate 点 NR 因 nrInitialized
	// 一直 false 而一帧都不跑。抓 queue 只读 device，不碰 FG 资源，安全。
	if (device && !g_state.capturedQueue) {
		ID3D12CommandQueue* queue = nullptr;
		if (SUCCEEDED(device->QueryInterface(
			__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue)))) {
			g_state.capturedQueue = queue;   // 保持引用
			g_state.injectedEarly = true;
			queue->GetDevice(IID_PPV_ARGS(&g_state.device12));
			D5_LOG_INFO(L"抓到 D3D12 command queue: %p (hwnd=%p %ux%u)",
				queue, hwnd, desc ? desc->Width : 0, desc ? desc->Height : 0);
		}
	}

	// Initialize the SR-side plumbing even when the FG swapchain is not hooked.
	if (g_state.device12) {
		NgxEavesdrop::Get().SetDevice(g_state.device12);
		D3D12Validation::Attach(g_state.device12);
		EvaluateGpuGate::Get().Initialize(g_state.device12);
		InstallQueueHook(g_state.device12);
		TryInstallDepthTracker();
	}
	NgxEavesdrop::Get().PauseCaptureFor(500);
	NgxEavesdrop::Get().NoteSwapChainRebuilt();
    // Keep a provisional candidate only while the factory call is in flight.
    // Failure removes it automatically and cannot erase an existing FG chain.
    FrameGenSwapChains::Creation pending(desc ? desc->BufferCount : 0);
    if (desc && desc->BufferCount >= g_state.fgBufferCountThreshold &&
        NgxEavesdrop::Get().FrameGenerationModuleLoaded()) {
		// NR 也要在这里初始化：FG 下 present 链纯转发不跑 RunFilters，而 evaluate
		// 点 NR 需要 nrInitialized=true。queue/device 已经在上面的前置抓取里拿到了。
		TryInitNr();
        const HRESULT hr = g_originalCreateSwapChainForHwnd(factory, device, hwnd,
            desc, fullscreenDesc, restrictToOutput, swapChain);
        if (SUCCEEDED(hr) && swapChain && *swapChain) {
            FrameGenSwapChains::Get().Observe(*swapChain, desc ? desc->BufferCount : 0);
            RememberPresentQueue(*swapChain, device);
            PatchSwapChainVTable(*swapChain);
            D5_LOG_INFO(L"ImGui FG chain registered: chain=%p queue=%p buffers=%u",
                *swapChain, device, desc->BufferCount);
        }
        return hr;
	}

    // A newly created small chain cannot clear another chain's FG guard.
    // DXGI releases the old chain's private lifetime cookie when it dies.
	D5_EVENT_D(SwapChainCreated,
		desc ? (uint64_t(desc->Width) << 32) | desc->Height : 0);
	// 第 N 次创建 swapchain 要单独记：游戏在设置里切 DLSS / 开关补帧 / 进出独占全屏
	// 都可能重建交换链，而我们缓存的 scaledSwapChain / capturedQueue 是按第一次的
	// 情况来的。排查"改设置就卡"时，这一行能立刻说明它到底重建过没有。
	// 第 N 次创建要单独记：游戏在设置里切 DLSS / 开关补帧 / 进出独占全屏都可能重建
	// 交换链。**限流**：实测这里能被调用 3333 次（我们持有引用导致它一直失败重试），
	// 不限流的话 1.1MB 日志里全是这一行，真正有用的信息全被冲掉。
	static std::atomic<uint32_t> creations{ 0 };
	const uint32_t nth = creations.fetch_add(1) + 1;
	const bool logThis = nth <= 4 || nth % 100 == 0;
	if (nth > 1 && logThis) {
		D5_LOG_WARN(L"游戏第 %u 次创建 swapchain（%ux%u, BufferCount=%u）。"
			L"queue=%p scaledSwapChain=%p",
			nth, desc ? desc->Width : 0, desc ? desc->Height : 0,
			desc ? desc->BufferCount : 0,
			g_state.capturedQueue, g_state.scaledSwapChain);
	}

	const HRESULT hr = g_originalCreateSwapChainForHwnd(
		factory, device, hwnd, desc, fullscreenDesc, restrictToOutput, swapChain);

	// **失败必须记下来。** 上一版不记 HRESULT，结果游戏卡死时我们只知道"它在反复建
	// swapchain"，却不知道到底成功没有 —— 少了这一个数字，就分不清"游戏自己在抽"和
	// "它建不出来"。DXGI_ERROR_INVALID_CALL(0x887A0001) 基本就等于"这个 HWND 上还有
	// 一个没被销毁的 swapchain"，而那通常是因为有人（很可能是我们）还持着引用。
	if (FAILED(hr)) {
		if (logThis) {
			// 实测：这个 HWND 上还有没销毁的 swapchain 时，DXGI 返回的是
			// **E_ACCESSDENIED(0x80070005)**，不是想当然的 DXGI_ERROR_INVALID_CALL。
			// 两个都判一下。
			const bool hwndBusy =
				hr == E_ACCESSDENIED || hr == DXGI_ERROR_INVALID_CALL;
			D5_LOG_ERROR(L"CreateSwapChainForHwnd(%ux%u) 失败: 0x%08X%s",
				desc ? desc->Width : 0, desc ? desc->Height : 0, hr,
				hwndBusy
					? L"（这个 HWND 上还有没被销毁的 swapchain —— 说明有人还持着旧"
					  L"对象的引用。**优先怀疑我们自己**：任何对 swapchain 的 AddRef "
					  L"都会让游戏永远建不出新链，反复重试，画面冻住。）"
					: L"");
		}
		return hr;
	}

	// 只有真的建出来了才动我们的状态。失败时什么都不能改 —— 游戏还在用旧的那个。
	if (nth > 1) {
		// 旧 swapchain 大概率正在/已经被释放，而我们只按指针值认它。新对象很可能
		// 落在同一个地址上 —— 那样我们就会把新 swapchain 当成"已经装好代理的那个"，
		// 拿旧尺寸的代理纹理交给它。所以这里必须把代理彻底拆掉重来。
		if (g_state.scaledSwapChain) {
			D5_LOG_WARN(L"拆掉按上一个 swapchain 建的代理，避免指针复用认错对象");
			g_state.scaler.Teardown();
			g_state.scaledSwapChain = nullptr;
			g_state.scalerActive.store(false, std::memory_order_release);
		}
		// 重建 swapchain 时深度缓冲基本也一起重建了
		GetDepthTracker().Invalidate();
		// UI 每帧往游戏队列提交 imgui 命令，会撞上 DXGI 换链的同步窗口（MHW 实测
		// 在重建后 ~100ms ACCESS_LOST）。重建后让 UI 暂停一小段，等新链稳定。
		g_state.uiPauseUntil = GetTickCount64() + 500;
		// UI 的换链保护按 present 帧数再补一道（见 uiPauseFrames 字段说明）。
		g_state.uiPauseFrames = 120;
		// 旁听拷贝同样要停：它往游戏的命令列表录 barrier+copy，一样撞换链窗口
		// （生化9 开 FG 实测在重建 + 首次拷贝后 ~468ms ACCESS_LOST）。
		NgxEavesdrop::Get().PauseCaptureFor(500);
		// swapchain 重建 = 换链信号：让 evaluate 点 NR 暂停 + 强制 reset。
		NgxEavesdrop::Get().NoteSwapChainRebuilt();
	}

	// 早注入时这里就是我们第一次拿到真 swapchain 的地方，vtable 补丁必须在
	// SetupScaler **之前**打完 —— SetupScaler 要用 g_originalGetDesc 读真实尺寸，
	// 那个指针是打补丁时才拿到的。
	if (SUCCEEDED(hr) && swapChain && *swapChain) {
		FrameGenSwapChains::Get().Observe(*swapChain, desc ? desc->BufferCount : 0);
		RememberPresentQueue(*swapChain, device);
		PatchSwapChainVTable(*swapChain);
	}

	// 真超分要在游戏拿到第一个 backbuffer 之前把代理准备好
	if (SUCCEEDED(hr) && swapChain && *swapChain && !g_state.scaledSwapChain) {
		SetupScaler(*swapChain, desc ? desc->Width : 0, desc ? desc->Height : 0);
	}
	return hr;
}

// 老接口 IDXGIFactory::CreateSwapChain。走这条路的游戏拿不到真超分（代理需要
// DXGI_SWAP_CHAIN_DESC1 那套翻转模型的信息），但 DLAA 和 DLSS5 完全正常 ——
// 所以至少要保证 Present 能被 hook 上。
HRESULT STDMETHODCALLTYPE HookedCreateSwapChain(
	IDXGIFactory* factory, IUnknown* device, DXGI_SWAP_CHAIN_DESC* desc,
	IDXGISwapChain** swapChain) {
	// 探测设备走的正是这条老接口，必须挡住 —— 否则日志里会出现一条 8x8 的假线索
	if (g_creatingProbe.load(std::memory_order_relaxed)) {
		return g_originalCreateSwapChain(factory, device, desc, swapChain);
	}
    if (!AcceptDxgiCreation(device, desc ? desc->OutputWindow : nullptr,
            desc ? desc->BufferDesc.Width : 0, desc ? desc->BufferDesc.Height : 0))
        return g_originalCreateSwapChain(factory, device, desc, swapChain);

	if (device && !g_state.capturedQueue) {
		ID3D12CommandQueue* queue = nullptr;
		if (SUCCEEDED(device->QueryInterface(
			__uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue)))) {
			g_state.capturedQueue = queue;   // 保持引用
			g_state.injectedEarly = true;
			queue->GetDevice(IID_PPV_ARGS(&g_state.device12));
			D5_LOG_INFO(L"抓到 D3D12 command queue: %p（来自老接口 CreateSwapChain）",
				queue);
		}
	}

	D5_EVENT_D(SwapChainCreated, desc
		? (uint64_t(desc->BufferDesc.Width) << 32) | desc->BufferDesc.Height : 0);

    FrameGenSwapChains::Creation pending(desc ? desc->BufferCount : 0);
	const HRESULT hr = g_originalCreateSwapChain(factory, device, desc, swapChain);
	if (FAILED(hr)) {
		static std::atomic<uint32_t> failures{ 0 };
		const uint32_t nth = failures.fetch_add(1) + 1;
		if (nth <= 4 || nth % 100 == 0) {
			D5_LOG_ERROR(L"CreateSwapChain 第 %u 次失败: 0x%08X%s", nth, hr,
				(hr == E_ACCESSDENIED || hr == DXGI_ERROR_INVALID_CALL)
					? L"（HWND 上还有没销毁的 swapchain —— 有人还持着引用）" : L"");
		}
		return hr;
	}
	if (swapChain && *swapChain) {
		FrameGenSwapChains::Get().Observe(*swapChain, desc ? desc->BufferCount : 0);
		RememberPresentQueue(*swapChain, device);
		D5_LOG_INFO(L"游戏走的是老接口 CreateSwapChain（%ux%u）—— 真超分不可用，"
			L"DLAA/DLSS5 不受影响",
			desc ? desc->BufferDesc.Width : 0, desc ? desc->BufferDesc.Height : 0);
		PatchSwapChainVTable(*swapChain);
	}
	return hr;
}

/* ============================ 安装 hook ============================ */

// Present entries stay unchanged so overlay discovery sees the same address
// across replacement chains. Other interface methods retain per-table patches.
bool PatchSwapChainVTable(IDXGISwapChain* swapChain) noexcept {
	// 诊断：完全跳过 swapchain vtable 补丁。present 回调链（OnPresent 里的深度
	// 追踪、滤镜）全都因此停用 —— 用这一路把"游戏重建 swapchain 后设备丢失"的
	// 嫌疑范围二分掉。return false 表示"这次没装"，g_swapChainHooked 不会被置位，
	// 后续再建 swapchain 也只会再打一次日志、仍然不打补丁。
	if (g_state.noPresentHook) {
		static bool warned = false;
		if (!warned) {
			warned = true;
			D5_LOG_WARN(L"诊断：present hook 被 diagNoPresentHook 关掉了 —— "
				L"present 链（含深度追踪/滤镜）全部停用，这是刻意的");
		}
		return false;
	}
    if (!swapChain) return false;
    static std::mutex installMutex;
    std::lock_guard<std::mutex> lock(installMutex);
    auto patch = [](auto* object, size_t index, auto hook, auto& firstOriginal) {
        auto** table = *reinterpret_cast<void***>(object);
        // Another overlay may now be above us. Do not re-hook that slot and
        // break its forwarding chain when DXGI returns another shared instance.
        {
            std::lock_guard<std::mutex> registryLock(VtablePatchMutex());
            for (const auto& entry : VtablePatchRegistry())
                if (entry.slot == table + index) return;
        }
        if (table[index] == reinterpret_cast<void*>(hook)) return;
        void* original = PatchVTable(object, index, hook);
        if (!firstOriginal) firstOriginal = reinterpret_cast<std::remove_reference_t<decltype(firstOriginal)>>(original);
    };
    const auto reportFailure = [](const wchar_t* method, void* target, const char* reason) {
        static std::atomic<unsigned> failures{0};
        const unsigned count = ++failures;
        if (count <= 4 || count % 100 == 0)
            D5_LOG_WARN(L"%ls inline hook failed: target=%p reason=%hs count=%u; this implementation remains unhooked",
                method, target, reason ? reason : "unknown", count);
    };
    const char* failure = nullptr;
    void* presentTarget = (*reinterpret_cast<void***>(swapChain))[VT_SWAPCHAIN_PRESENT];
    auto present = PresentInlineHooks<PresentFn>::Install(presentTarget, &HookedPresent, &failure);
    bool ok = present != nullptr;
    if (!present) reportFailure(L"Present", presentTarget, failure);
    if (present && !g_originalPresent) g_originalPresent = present;
    patch(swapChain, VT_SWAPCHAIN_GET_BUFFER, &HookedGetBuffer, g_originalGetBuffer);
    patch(swapChain, VT_SWAPCHAIN_GET_DESC, &HookedGetDesc, g_originalGetDesc);
    patch(swapChain, VT_SWAPCHAIN_RESIZE_BUFFERS, &HookedResizeBuffers, g_originalResizeBuffers);
    IDXGISwapChain1* sc1 = nullptr;
    if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&sc1)))) {
        void* present1Target = (*reinterpret_cast<void***>(sc1))[VT_SWAPCHAIN_PRESENT1];
        auto present1 = PresentInlineHooks<Present1Fn>::Install(present1Target, &HookedPresent1, &failure);
        if (!present1) { ok = false; reportFailure(L"Present1", present1Target, failure); }
        if (present1 && !g_originalPresent1) g_originalPresent1 = present1;
        patch(sc1, VT_SWAPCHAIN_GET_DESC1, &HookedGetDesc1, g_originalGetDesc1);
        sc1->Release();
    }

    IDXGISwapChain3* sc3 = nullptr;
    if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&sc3)))) {
        patch(sc3, VT_SWAPCHAIN_RESIZE_BUFFERS1, &HookedResizeBuffers1, g_originalResizeBuffers1);
        sc3->Release();
    }

	if (ok) g_swapChainHooked.store(true, std::memory_order_release);
	return ok;
}

// 兜底：造一个临时的 D3D11 设备 + swapchain，只为了读出 DXGI 的 vtable。
//
// **只在晚注入时用。** 早注入时游戏还没初始化图形栈，我们在它之前建一个真的 D3D11
// 设备（驱动初始化、GPU 上下文）风险很高 —— OptiScaler 就为此专门改过：
// "Changed to hook dxgi/d3d12/d3d11/vulkan-1 when game loads them. This should
//  increase compatibility with Death Stranding and Capcom games"（v0.7.0-pre66），
// 而鬼武者正是 Capcom 的 RE Engine。早注入现在改走"等游戏自己创建 swapchain，
// 从它的对象上取 vtable"，一个设备都不建。
//
// 晚注入时这条路是必须的：游戏的 swapchain 早就建好了，工厂 hook 永远不会触发，
// 没有别的办法拿到 vtable。而那时游戏的图形栈已经就绪，这也是我们一直在用、
// 已经验证过的路径。
bool InstallProbeHooks() noexcept {
	g_creatingProbe.store(true, std::memory_order_relaxed);
	struct ProbeScope {
		~ProbeScope() { g_creatingProbe.store(false, std::memory_order_relaxed); }
	} probeScope;

	WNDCLASSEXW wc{};
	wc.cbSize = sizeof(wc);
	wc.lpfnWndProc = DefWindowProcW;
	wc.hInstance = GetModuleHandleW(nullptr);
	wc.lpszClassName = L"DXLProbe";
	RegisterClassExW(&wc);
	HWND probeWindow = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW,
		0, 0, 8, 8, nullptr, nullptr, wc.hInstance, nullptr);
	if (!probeWindow) {
		D5_LOG_ERROR(L"probe window failed: %lu", GetLastError());
		return false;
	}

	DXGI_SWAP_CHAIN_DESC scd{};
	scd.BufferCount = 1;
	scd.BufferDesc.Width = 8;
	scd.BufferDesc.Height = 8;
	scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	scd.OutputWindow = probeWindow;
	scd.SampleDesc.Count = 1;
	scd.Windowed = TRUE;
	scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

	ID3D11Device* probeDevice = nullptr;
	IDXGISwapChain* probeSwapChain = nullptr;
	const D3D_FEATURE_LEVEL levels[]{ D3D_FEATURE_LEVEL_11_0 };
	HRESULT hr = D3D11CreateDeviceAndSwapChain(
		nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 1,
		D3D11_SDK_VERSION, &scd, &probeSwapChain, &probeDevice, nullptr, nullptr);
	if (FAILED(hr) || !probeSwapChain) {
		D5_LOG_ERROR(L"probe swapchain failed: 0x%08X", hr);
		DestroyWindow(probeWindow);
		return false;
	}

	const bool ok = PatchSwapChainVTable(probeSwapChain);

	probeSwapChain->Release();
	probeDevice->Release();
	DestroyWindow(probeWindow);
	return ok;
}

// 只给 DXGI 工厂打补丁 —— **不创建任何设备、窗口或 swapchain**。
//
// `CreateDXGIFactory1` 不碰驱动、不建 GPU 上下文，代价和"加载 dxgi.dll"差不多，
// 而 dxgi.dll 在 core 被加载时就因为静态导入进来了。
//
// 工厂的 vtable 同样是全进程共享的，所以在这里打完补丁，游戏**之前和之后**创建的
// 工厂都会走我们的 CreateSwapChain / CreateSwapChainForHwnd。只要它还没建 swapchain，
// 我们就一定能接到。
bool InstallFactoryHooks() noexcept {
	IDXGIFactory2* factory = nullptr;
	if (FAILED(CreateDXGIFactory1(
		__uuidof(IDXGIFactory2), reinterpret_cast<void**>(&factory)))) {
		D5_LOG_ERROR(L"CreateDXGIFactory1 失败，工厂 hook 装不上");
		return false;
	}

	g_originalCreateSwapChainForHwnd =
		reinterpret_cast<CreateSwapChainForHwndFn>(PatchVTable(
			factory, VT_FACTORY_CREATE_SWAPCHAIN_FOR_HWND,
			&HookedCreateSwapChainForHwnd));
	// 老接口也要 hook：不少游戏（尤其是从 D3D11 时代改过来的）走的是
	// IDXGIFactory::CreateSwapChain 而不是 CreateSwapChainForHwnd
	g_originalCreateSwapChain = reinterpret_cast<CreateSwapChainFn>(PatchVTable(
		factory, VT_FACTORY_CREATE_SWAPCHAIN, &HookedCreateSwapChain));
	factory->Release();

	D5_LOG_INFO(L"工厂 hook 已装: CreateSwapChainForHwnd=%p CreateSwapChain=%p",
		g_originalCreateSwapChainForHwnd, g_originalCreateSwapChain);
	return g_originalCreateSwapChainForHwnd != nullptr ||
		g_originalCreateSwapChain != nullptr;
}

// 注入方是否声明了"这次是早注入"。见 IpcProtocol.h 里对这个事件的说明。
bool EarlyInjectRequested() noexcept {
	wchar_t name[128]{};
	_snwprintf_s(name, _TRUNCATE, L"%s.%lu",
		Ipc::EARLY_INJECT_EVENT_BASE, GetCurrentProcessId());
	HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, name);
	if (!event) return false;
	CloseHandle(event);
	return true;
}

bool InstallHooks() noexcept {
	QueryPerformanceFrequency(&g_state.qpcFreq);
	g_state.earlyInjectRequested = EarlyInjectRequested();

	if (g_state.earlyInjectRequested) {
		D5_LOG_INFO(L"早注入：不创建探测设备，等游戏自己创建 swapchain 再从它身上"
			L"取 vtable（避免在游戏初始化图形栈之前抢先建 D3D11 设备）");
		return InstallFactoryHooks();
	}

	D5_LOG_INFO(L"晚注入：游戏的 swapchain 大概已经建好了，用探测设备读 vtable");
	// **顺序很重要**：先造探测设备，再装工厂 hook。
	// 反过来的话，探测设备内部走的 IDXGIFactory::CreateSwapChain 会先撞进我们自己
	// 刚装好的工厂 hook 里，于是日志上留下一条"游戏走的是老接口 CreateSwapChain
	// （8x8）"——那是我们自己的 8x8 探测链，不是游戏。这些日志正是拿来查问题的，
	// 不能让它自己给自己造假线索。实测踩过。
	const bool probeOk = InstallProbeHooks();
	// 工厂 hook 晚注入也要装：游戏之后可能重建 swapchain（切全屏、改设置）
	const bool factoryOk = InstallFactoryHooks();
	return probeOk || factoryOk;
}

/* ============================ 命令管道 ============================ */

DWORD WINAPI CommandThread(LPVOID) {
	wchar_t name[128]{};
	_snwprintf_s(name, _TRUNCATE, L"%s.%lu",
		Ipc::COMMAND_PIPE_BASE, GetCurrentProcessId());
	D5_LOG_INFO(L"command pipe: %s", name);

	for (;;) {
		HANDLE pipe = CreateNamedPipeW(name, PIPE_ACCESS_DUPLEX,
			PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT,
			PIPE_UNLIMITED_INSTANCES,
			sizeof(Ipc::CommandReply), sizeof(Ipc::Command), 0, nullptr);
		if (pipe == INVALID_HANDLE_VALUE) {
			Sleep(1000);
			continue;
		}
		if (!ConnectNamedPipe(pipe, nullptr) &&
			GetLastError() != ERROR_PIPE_CONNECTED) {
			CloseHandle(pipe);
			continue;
		}

		Ipc::Command command{};
		DWORD transferred = 0;
		Ipc::CommandReply reply{ Ipc::MAGIC, Ipc::VERSION, 0, 0 };
		bool needsScalerNudge = false;
		if (ReadFile(pipe, &command, sizeof(command), &transferred, nullptr) &&
			transferred == sizeof(command) && command.magic == Ipc::MAGIC) {
			switch (static_cast<Ipc::CommandId>(command.id)) {
			case Ipc::CommandId::TogglePerfWindow: {
                std::lock_guard<std::recursive_mutex> lock(g_nrStateMutex);
                UiTogglePanel(); reply.accepted = 1; break;
            }
            case Ipc::CommandId::ToggleDebugView: {
				std::lock_guard<std::recursive_mutex> nrLock(g_nrStateMutex);
				// arg0: 0=关 1=深度 2=矢量；0xFFFFFFFF = 循环下一个。
				//
				// 为什么要有快捷键：debug 视图的用处全在"边动边切"上，
				// 而走界面要切窗口 + 点应用，画面早就不是那一刻了。
				// 而且它绕开设置文件 —— 界面那条路出问题时还有这一条能用。
				const uint32_t current = uint32_t(g_state.nrSettings.debugView);
				// **循环只到"矢量"为止。**
				//
				// 枚举里还有 Encoded / Diff 两档，但它们只在"游戏 DLSS 的 evaluate
				// 点"那条路上有实现，而那条路已经证明会挂设备（往游戏自己的命令列表上
				// 录 dispatch -> DXGI_ERROR_DEVICE_HUNG），永久不用了。留在循环里的
				// 后果是按两下 Alt+V 又看到深度 —— 看起来像"切换坏了"。
				// 枚举值保留是为了那条路的代码还能编过，不放进循环。
				constexpr uint32_t VIEW_COUNT = 5;
				const uint32_t next = command.arg0 == 0xFFFFFFFFu
					? (current + 1) % VIEW_COUNT : (command.arg0 % VIEW_COUNT);
				g_state.nrSettings.debugView = NrSettings::DebugView(next);
				// 同样照 NVIDIA 的写法：名字 + 状态，"关"用 OFF。
				const wchar_t* const viewName =
					next == 1 ? L"DEPTH" : next == 2 ? L"MOTION VECTORS" : L"OFF";
				D5_LOG_INFO(L"command ToggleDebugView -> %s", viewName);
				break;
			}
			case Ipc::CommandId::SetEnabled: {
				const bool on = command.arg0 != 0;
				g_state.enabled.store(on);
				// 提示文字学 NVIDIA 覆盖层的写法：功能名 + ON/OFF，没有解释性的话。
				// 唯一的例外是"关了但代理还在缩放"—— 那时画面确实还被我们动着，
				// 不说清楚玩家会以为开关坏了。
				const bool proxyLive =
					g_state.scalerActive.load(std::memory_order_acquire);
				D5_LOG_INFO(L"command SetEnabled=%u", command.arg0);
				reply.accepted = 1;
				// **不在这里 nudge。**
				//
				// 一度在这里加了"开总开关时顺手把代理补上"，代价是**每按一次
				// Alt+D 都让游戏重建一遍 backbuffer**（街霸 6 实测 16 秒连续卡顿，
				// 而且代理被禁用之后每次都注定失败）。真超分要不要生效由
				// 「应用」那条路负责，快捷键只管开关效果 —— 一个每帧都可能被按的
				// 键，绝不能挂上"让游戏重建渲染资源"这种动作。
				//
				// 于是真超分变成一个**启动期决定**：总开关关着时不建代理，
				// 所以这一局用不上；但 Alt+D 的状态会被 UI 记进配置，下一局启动时
				// 总开关就是开的，代理照常在 swapchain 创建时建立。说清楚就行。
				if (on && g_state.wantsUpscale.load(std::memory_order_acquire) &&
					!g_state.scalerActive.load(std::memory_order_acquire) &&
					!g_state.proxyBanned.load(std::memory_order_acquire)) {
					D5_LOG_INFO(L"真超分这一局用不上：代理只在游戏创建 swapchain 时能建，"
						L"而那会儿总开关是关的。这次的选择已被记住，下次启动就生效。");
					g_publisher.Publish(
						"真超分下次启动生效（代理只能在游戏启动时建立）。"
						"这一局按 DLAA 运行。");
				}
				break;
			}
			case Ipc::CommandId::ReloadSettings:
				g_state.settingsDirty.store(true);
				D5_LOG_INFO(L"command ReloadSettings");
				reply.accepted = 1;
				needsScalerNudge = true;
				break;
			case Ipc::CommandId::NudgeSwapChain:
				D5_LOG_INFO(L"command NudgeSwapChain");
				reply.accepted = TryNudgeWindowForResize() ? 1 : 0;
				break;
			default:
				D5_LOG_WARN(L"unknown command id=%u", command.id);
				break;
			}
			g_publisher.Publish();
		}
		WriteFile(pipe, &reply, sizeof(reply), &transferred, nullptr);
		FlushFileBuffers(pipe);
		DisconnectNamedPipe(pipe);
		CloseHandle(pipe);

		// 应答发完之后再做：nudge 要花半秒，放在应答前 UI 会以为管道卡住了。
		//
		// **这条路会实打实地让游戏变卡，条件必须掐死。** 一次 nudge = 把游戏窗口
		// 改小 2px 再改回，游戏收到 WM_SIZE 后重建整套 backbuffer 和 render target；
		// 我们这边跟着重建 NGX feature（实测 70~123 ms 卡在 present 线程上）、
		// 深度候选表整表作废。街霸 6 实测：一次点击触发 3 轮重试 = 6 次窗口 resize
		// = 6 次全套重建 = 16 秒的连续卡顿，而且**代理早就被判定不可用了，全是白干**。
		// 所以：
		//   · proxyBanned 之后一次都不要再试（永远不可能成功）
		//   · 重试从 3 次减到 1 次（成不成取决于游戏怎么处理 WM_SIZE，
		//     多试两次的边际收益远小于让玩家卡 16 秒的代价）
		if (needsScalerNudge) {
			// 等 present 线程把新设置读进去
			for (int i = 0; i < 100 && g_state.settingsDirty.load(); ++i) Sleep(10);
			if (g_state.wantsUpscale.load(std::memory_order_acquire) &&
				!g_state.scalerActive.load(std::memory_order_acquire) &&
				!g_state.proxyBanned.load(std::memory_order_acquire)) {
				D5_LOG_INFO(L"用户要真超分但代理未建立，尝试自动触发 swapchain 重建"
					L"（只试一次 —— 这一下会让游戏重建整套 backbuffer）");
				TryNudgeWindowForResize();
				g_publisher.Publish();
			} else if (g_state.proxyBanned.load(std::memory_order_acquire)) {
				D5_LOG_INFO(L"代理对本游戏已禁用，不再尝试触发重建（免得白卡一次）");
			}
		}
	}
}

DWORD WINAPI InitThread(LPVOID) {
    g_startupDiagnostics.Entered();
    SetTeardownExtraCleanup(&CleanupDxl);
    CommandListTracker::Get().SetResetObserver([](void* ctx, ID3D12GraphicsCommandList* list) noexcept {
        static_cast<SegMaskFilter*>(ctx)->NotifyReset(list);
    }, &g_state.segMask);
	g_startupDiagnostics.Mark(StartupDiagnostics::LogOpenEntered);
	Log::Get().Open(L"core");
	g_startupDiagnostics.Mark(StartupDiagnostics::LogOpenReturned);
	wchar_t exePath[MAX_PATH]{};
	GetModuleFileNameW(nullptr, exePath, MAX_PATH);
	D5_LOG_INFO(L"core injected into pid=%lu (%s)", GetCurrentProcessId(), exePath);
	D5_LOG_INFO(L"Core version %hs: automatic Evaluate/Present handoff; confirmed SR activity + fenced color sampling; preserve FG Evaluate path", kUiVersion);

	g_startupDiagnostics.Mark(StartupDiagnostics::StatusOpenEntered);
	if (!g_publisher.Open()) return 1;
	g_startupDiagnostics.StatusResult(true, ERROR_SUCCESS);
	g_publisher.Publish("injected, installing hooks");

	// **链式注入要最先装。**
	//
	// 我们可能就在启动壳里（Steam 游戏第一个进程往往只活几百毫秒）。壳一旦把真身
	// 拉起来，那个真身就只能靠"发现进程再注入"补上 —— 而那是竞态，实测赢一次输一次
	// （98 个模块时旁听接上 5 个钩子，125 个模块时一个都没接上，表现为"工具没效果"）。
	// 挂在这里，壳拉真身的那一刻我们就能把它挂起、注入、等它装好钩子再放行。
	InstallChainInject(g_state.selfModule);
	const auto validationMarker = SettingsReader::ConfigRoot(g_state.selfModule) / L"NRFG_D3D12_validation.enable";
	D3D12Validation::EnableBeforeDevice(GetFileAttributesW(validationMarker.c_str()) != INVALID_FILE_ATTRIBUTES);

	// **在游戏建设备之前把 DRED 打开。**
	//
	// 顺序是硬要求：DRED 的开关是进程级的，只对**之后**创建的设备生效。
	// 早注入时这里一定在游戏建设备之前；晚注入时就晚了（那种情况下 DumpDred 会
	// 明说"拿不到接口"，而不是静默无输出）。
	//
	// 为什么值得：设备挂掉时 GetDeviceRemovedReason 只回一个 0x887A0006
	// （"命令有问题"），谁的命令一个字都不说。这个项目已经为此猜过三次。
	// 面包屑的代价是每条命令一次写入，实测感知不到；换来的是能直接点名。
	{
		ID3D12DeviceRemovedExtendedDataSettings* dredSettings = nullptr;
		const HRESULT hr = D3D12GetDebugInterface(
			__uuidof(ID3D12DeviceRemovedExtendedDataSettings),
			reinterpret_cast<void**>(&dredSettings));
		if (SUCCEEDED(hr) && dredSettings) {
			dredSettings->SetAutoBreadcrumbsEnablement(
				D3D12_DRED_ENABLEMENT_FORCED_ON);
			dredSettings->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
			ID3D12DeviceRemovedExtendedDataSettings1* contexts = nullptr;
			if (SUCCEEDED(dredSettings->QueryInterface(IID_PPV_ARGS(&contexts)))) {
				contexts->SetBreadcrumbContextEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
				contexts->Release();
			}
			dredSettings->Release();
			FreezeWatchdog::Get().SetDredEnabled(true);
			D5_LOG_INFO(L"DRED 已打开（面包屑 + 页错误）—— 设备再挂就能点名了");
		} else {
			D5_LOG_WARN(L"DRED 打不开（0x%08X）—— 设备挂了只能拿到一个错误码。", hr);
		}
	}

	// 我们到底有多早？
	//
	// 有意义的判据**不是模块总数**，而是"游戏的 Streamline / NGX 有没有已经加载"——
	// 因为要旁听游戏的 DLSS 调用，就必须赶在它加载那一套之前。模块总数只是个粗略
	// 参考，而且下限不低：我们自己就静态依赖 d3d11/d3d12/dxgi + CRT，实测挂起启动
	// 注入时已经有 30 个（对照：等加载器安静之后再注入，鬼武者那次是 155 个）。
	{
		static const wchar_t* const WATCHED[]{
			L"_nvngx.dll", L"nvngx.dll", L"nvngx_dlss.dll", L"nvngx_dlssnr.dll",
			L"nvngx_dlssg.dll", L"nvngx_dlssd.dll", L"sl.interposer.dll",
			L"sl.dlss.dll", L"sl.dlss_d.dll", L"sl.common.dll", L"nvapi64.dll",
		};
		uint32_t alreadyThere = 0;
		for (const wchar_t* name : WATCHED) {
			const HMODULE module = GetModuleHandleW(name);
			if (!module) continue;
			++alreadyThere;
			wchar_t path[MAX_PATH]{};
			GetModuleFileNameW(module, path, MAX_PATH);
			D5_LOG_INFO(L"注入时进程里已有: %s -> %s", name, path);
		}

		HMODULE modules[1024];
		DWORD needed = 0;
		DWORD count = 0;
		if (EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules),
			&needed)) {
			count = needed / sizeof(HMODULE);
		}
		if (alreadyThere) {
			// 这一条**只是提示，不是判死刑**。原话写的是"旁听插不进去了"，
			// 实测被推翻了：鬼武者从 Steam 启动壳转手（没能用上挂起注入），注入时
			// 这 6 个模块已经在了，旁听照样接上并抄到了一万多帧。原因是模块"加载了"
			// 不等于它"已经解析完 NGX 函数地址"—— Streamline 是按需 GetProcAddress
			// 的，我们的 IAT 补丁装在那之前就还赶得上。所以别看到这条就下结论，
			// 看后面有没有"旁听已接上"。
			D5_LOG_WARN(L"注入时游戏已经加载了 %u 个 NVIDIA/Streamline 模块"
				L"（进程共 %lu 个模块）—— 我们**晚于**它的 DLSS 栈。"
				L"旁听可能接不上（看后面有没有「旁听已接上」；接不上时改用挂起启动注入）。",
				alreadyThere, count);
		} else {
			D5_LOG_INFO(L"注入时游戏还没加载任何 NVIDIA/Streamline 模块"
				L"（进程共 %lu 个模块）—— 我们**早于**它的 DLSS 栈，"
				L"可以旁听它的 DLSS 调用。", count);
		}
	}

	// 必须在装 hook 之前读一次：真超分要在 CreateSwapChainForHwnd 里就知道倍率，
	// 那一刻可能只比现在晚几毫秒。
	ReloadSettings();
	g_state.settingsDirty.store(false);

	CreateThread(nullptr, 0, CommandThread, nullptr, 0, nullptr);
	// 在装 hook 之前启动：这样连"第一帧就卡住"也能抓到
	FreezeWatchdog::Get().Start(g_state.watchdogMs);

	// 旁听游戏自己的 DLSS 调用。**必须在 DXGI hook 之前装** —— 游戏加载 Streamline
	// 比创建 swapchain 早得多，晚一步整条加载链就插不进去了。
	NgxEavesdrop::Get().Install(g_state.selfModule, g_state.eavesdrop);
	// 回调**无条件装上**，由它自己每帧看 g_state.nrAtEvaluate 决定要不要动手。
	// 这样运行中改设置也能生效 —— 按开关决定装不装的话，玩家在 UI 上打开这个开关
	// 之后必须重启游戏才有效，而他没有任何理由知道这一点。
	NgxEavesdrop::Get().SetPreEvaluateHook(&RunNrAtEvaluate);

	InstallLegacyGraphicsHooks({&BeginLegacyFrame, &ProcessLegacyFrame});
	if (!InstallHooks()) {
		D5_LOG_ERROR(L"hook installation failed");
		g_publisher.Publish("hook installation failed");
		return 1;
	}
	g_publisher.Publish("hooks installed, waiting for first present");

	// F8 切 NR 的独立监听线程。必须在 present 链之外启动（FG 换链后 present 链没挂
	// 新 swapchain 的 hook，等第一次 present 再懒启动会漏掉"开 FG 启动"的场景）。
	StartNrHotkeyMonitor();

	// 告诉注入方"可以放游戏跑了"。挂起启动注入靠这个事件保证顺序：在它被 set 之前
	// 游戏的主线程还挂着，所以我们的 IAT 补丁一定早于它加载 Streamline。
	{
		wchar_t name[128]{};
		_snwprintf_s(name, _TRUNCATE, L"%s.%lu",
			Ipc::READY_EVENT_BASE, GetCurrentProcessId());
		// 注入方先创建了它；这里 Open 而不是 Create，拿不到就说明不是挂起注入，
		// 那也没关系（普通注入不需要这个握手）。
		if (HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, name)) {
			SetEvent(ready);
			CloseHandle(ready);
			D5_LOG_INFO(L"已通知注入方：hook 装好了，可以 resume 游戏");
		}
	}

	// 早注入的最后兜底。
	//
	// 正常情况下游戏几秒内就会创建 swapchain，我们在工厂 hook 里接到并打上补丁。
	// 但万一它走的是我们没 hook 到的路径（CreateSwapChainForCoreWindow、自己带的
	// DXGI 封装等），那就一个功能都没有了。等足够久之后退回探测设备 —— 那时游戏
	// 早就渲染起来了，和晚注入的情形一样，是已经验证过的安全路径。
	//
	// 45 秒是刻意给足的：游戏启动、着色器编译、过场都可能很久，宁可等，也不要在
	// 它初始化图形栈的当口去建设备 —— 那正是我们要避免的事。
	if (g_state.earlyInjectRequested) {
		constexpr int DEADLINE_MS = 45000;
		for (int waited = 0; waited < DEADLINE_MS; waited += 250) {
			if (g_swapChainHooked.load(std::memory_order_acquire) || g_state.presentCount.load() > 0 || IsTeardownRequested()) return 0;
			Sleep(250);
		}
		if (!g_swapChainHooked.load(std::memory_order_acquire)) {
			D5_LOG_WARN(L"早注入等了 %d 秒也没等到游戏创建 swapchain —— 它可能用了"
				L"我们没 hook 到的建链路径。退回探测设备兜底。", DEADLINE_MS / 1000);
			if (InstallProbeHooks()) {
				g_publisher.Publish("已用探测设备兜底装上 hook（游戏的建链路径没接到）");
			} else {
				g_publisher.Publish("hook 没装上：既没接到游戏建 swapchain，探测设备也失败");
			}
		}
	}
	return 0;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
	if (reason == DLL_PROCESS_ATTACH) {
		g_startupDiagnostics.Open();
		DisableThreadLibraryCalls(module);
		g_state.selfModule = module;
		g_injectAtMs.store(GetTickCount64(), std::memory_order_relaxed);
		// DllMain 里不能做重活（loader lock），所以扔到线程里
		g_startupDiagnostics.Mark(StartupDiagnostics::WorkerCreateRequested);
		DWORD workerId = 0;
		HANDLE worker = CreateThread(nullptr, 0, InitThread, nullptr, 0, &workerId);
		const DWORD workerError = worker ? ERROR_SUCCESS : GetLastError();
		g_startupDiagnostics.Created(workerId, workerError);
		if (worker) CloseHandle(worker);
	} else if (reason == DLL_PROCESS_DETACH) {
		g_startupDiagnostics.Mark(StartupDiagnostics::ProcessDetach);
		// 退出清理的最终兜底：main 直接 return 的进程（ExitProcess 销毁窗口
		// 在 DLL_PROCESS_DETACH 之后，WM_NCDESTROY 路走不到）只有这里能接。
		// RequestTeardown 幂等——前面三段（NGX Shutdown 包装/WM_NCDESTROY/
		// 看门狗）谁先到谁清理，都到不了才轮到这里。
		Log::WriteExitMarker(L"DETACH-marker: DllMain DLL_PROCESS_DETACH");
		D3D12Validation::Detach();
		RequestTeardown();
		Log::WriteExitMarker(L"DETACH-marker: RequestTeardown done");
	}
	return TRUE;
}
