#pragma once

// UI 宿主 <-> 注入到游戏进程里的 core DLL 之间的契约。
//
// 设计取向和 Magpie 深度桥那次一样，但吸取了教训：
//   · 命名共享内存放**状态**（core -> UI 单向，每帧可写，UI 轮询读）
//   · 命名管道放**命令**（UI -> core，低频，请求/应答）
//   · 设置本身走 settings.json，core 自己读文件，不塞进 IPC
//     —— 这样 UI 侧加设置项不需要动这个头文件
//
// 共享内存以 core 所在的**游戏进程 PID** 命名，所以一个 UI 可以同时看多个目标。

#include <cstdint>

namespace DXL::Ipc {

inline constexpr uint32_t MAGIC = 0x35534C44;   // 'DLS5'
// 2: 加了 DLSSNR 的计数器。UI 和 core 是一起构建的，所以只要两边同时更新，
// 版本号在这里主要是给外部工具（scripts/probe-status.py）看的。
inline constexpr uint32_t VERSION = 13;

inline constexpr wchar_t STATUS_MEMORY_BASE[] = L"Local\\DXL.Status";
inline constexpr wchar_t COMMAND_PIPE_BASE[] = L"\\\\.\\pipe\\DXL.Command";

// 注入方在注入**之前**创建这个命名事件，表示"这次是早注入"（进程刚起，还没建
// swapchain）。core 在 InstallHooks 里查它，决定要不要走"自己造探测设备"那条路。
//
// 为什么需要这个而不是在进程里自己判断：core 静态链接了 d3d11/d3d12/dxgi，
// 这三个模块在 core 被加载的瞬间就都在了，所以"GetModuleHandle(d3d12.dll) 是否
// 为空"这类判断永远为真，分不出早晚。而注入方本来就知道答案 —— 它是启动游戏的
// 那一方还是挂到运行中的游戏上，一清二楚。
inline constexpr wchar_t EARLY_INJECT_EVENT_BASE[] = L"Local\\DXL.EarlyInject";

// core 装完 hook 之后 set 这个事件。
//
// 挂起启动注入必须等它：注入方在 LoadLibrary 返回后就 ResumeThread，而那时 core 的
// InitThread 还在装 hook —— 游戏一旦先跑起来就可能抢先加载 Streamline，我们的 IAT
// 补丁就再也插不进它的加载链了。DllMain 里不适合干这些活（loader lock），所以改成
// 让注入方**等 core 说准备好了**再放游戏跑。
inline constexpr wchar_t READY_EVENT_BASE[] = L"Local\\DXL.Ready";

// 为什么"在游戏的 DLSS 之前处理"那条路没跑起来。
//
// 这个枚举存在的理由：上一版界面上只有一句"已请求，但还没处理到任何一帧"，
// 而真实原因是"游戏的颜色格式是 R11G11B10_FLOAT，不在我们的白名单里"——
// 那句话把一个有明确原因的失败说成了一个谜。**能说出原因的失败才修得动。**
enum class NrAtEvaluateBlock : uint32_t {
	None = 0,
	NrNotReady,        // DLSSNR 自己还没初始化好（通常再等几帧就行）
	StateUnobserved,   // 还没从游戏的 barrier 里观察到颜色/矢量的状态
	ColorFormat,       // 游戏的颜色格式我们做不了 UAV 输出，日志里有具体哪一项不行
	ColorSize,         // 游戏颜色的尺寸和我们准备的纹理不一致
	EvaluateFailed,    // NGX 报错了，详情看日志
	// 这一局游戏已经锁定在另一条处理路径上了。切换必须重启游戏 ——
	// 见 DlssNrFilter 里 NrMode 的说明（跨线程热切换会用已释放的资源，实测崩过）。
	ModeLocked,
	// HDR->0..1 的计算着色器没就绪。没有它绝不能把没压过的 HDR 喂进去（一片灰）。
	ToneUnavailable,
	// 命令列表状态记账没装上。没有它就没法把游戏的描述符堆还回去 —— 会闪退，
	// 所以主动拒绝而不是硬上。
	StateUntracked,
	// 旁听一个钩子都没挂上 —— 注入晚于游戏解析 NGX 函数指针。
	// 这一条**必须存在**：否则这种情况下 UI 上一个原因都不显示，
	// 表现是"工具毫无效果"，而那和十几种别的原因长得一模一样。
	EavesdropNotAttached,
};

// core 探测到的渲染 API
enum class GraphicsApi : uint32_t {
	Unknown = 0,
	D3D9 = 9,
	D3D11 = 11,
	D3D12 = 12,
	Vulkan = 20,
	OpenGL = 30,
};

// 各功能的运行状态。UI 直接把它显示在 Home 页。
enum class FeatureState : uint32_t {
	Unavailable = 0,   // 环境不支持（缺 DLL、显卡不支持）
	Disabled,          // 支持但用户关了
	Standby,           // 已开启，等条件（还没拿到合适的资源）
	Active,            // 正在生效
	Failed,            // 试过但失败了，详情看日志
};

// core -> UI 的状态块。只有 core 写，UI 只读。
// 布局刻意全部 4/8 字节对齐，避免两侧编译器打包差异。
struct Status {
	uint32_t magic;
	uint32_t version;
	// core 每次写完自增。UI 读到奇数说明正在写，重读即可（seqlock）。
	volatile uint32_t sequence;
	uint32_t gamePid;

	uint32_t api;              // GraphicsApi
	uint32_t hooked;           // 0/1：present 路径是否已接管
	uint32_t srState;          // FeatureState
	uint32_t nrState;          // FeatureState

	uint32_t fgState;          // FeatureState
	uint32_t nativeDepth;      // 0/1：是否拿到了游戏的深度缓冲
	uint32_t nativeMotion;     // 0/1：是否拿到了引擎矢量
	// 0/1：我们是否赶在游戏创建 swapchain 之前就装好了 hook。
	// 这一位决定 UI 该不该提示玩家"从工具启动游戏" —— 真超分和原生深度都要求
	// 我们足够早在场，迟到注入这两件事都拿不到。
	uint32_t injectedEarly;

	uint32_t proxyActive;      // 0/1：真超分的代理 backbuffer 是否已建立
	uint32_t depthCandidates;  // 发现的深度缓冲候选数量
	uint32_t depthSelected;    // 选中的候选下标，没有则 0xFFFFFFFF
	// 0/1：游戏无视了我们谎报的 swapchain 尺寸。
	// 真超分靠"给游戏一组更小的 backbuffer 并谎报尺寸"实现，但有些引擎（RE Engine
	// 实测如此）按自己配置里的分辨率渲染和布局，根本不看 swapchain 的 desc。那样
	// 它会把全分辨率的画面往我们的小代理上写，只有左上角装得进去 —— 画面看起来
	// 像被裁切并放大了。判据：游戏的主深度缓冲尺寸远大于代理尺寸。
	uint32_t proxySizeIgnored;

	uint32_t renderWidth;      // DLSS 输入分辨率
	uint32_t renderHeight;
	uint32_t outputWidth;      // DLSS 输出分辨率
	uint32_t outputHeight;

	uint64_t presentCount;     // 已接管的 present 次数
	uint64_t evaluateCount;    // DLSS SR 的 NGX evaluate 成功次数
	uint64_t evaluateFailures;

	uint64_t nrEvaluateCount;  // DLSSNR 的 evaluate 成功次数
	uint64_t nrEvaluateFailures;

	float frameMs;             // 游戏帧时间
	float upscaleGpuMs;        // 超分本身的 GPU 耗时

	/* ---- 卡顿诊断（v5 新增）---- */
	// present 停止前进超过阈值的次数。> 0 就说明这个进程里真的卡过，
	// 日志里会有一份完整的转储。
	uint32_t stallCount;
	// 最后一次卡住时停在哪一步（Stage 枚举值）。UI 只负责显示，不解释语义 ——
	// 名字表在 core 侧（FreezeWatchdog.cpp 的 StageName）。
	uint32_t lastStallStage;
	// 因为命令槽没就绪而主动跳过的帧数。**这不是错误**：跳帧是我们为了绝不阻塞
	// 游戏 present 线程而付的代价。数字持续上涨说明 GPU 一直落后，可以考虑降画质；
	// 偶尔涨几帧是正常的。
	uint64_t srSkippedFrames;
	uint64_t nrSkippedFrames;

	/* ---- 旁听游戏自己的 DLSS 调用（v6 新增）---- */
	// 打了加载器补丁的模块数。0 = 旁听没装上。
	uint32_t eavesdropModules;
	// 游戏解析过多少个 NVSDK_NGX_* 函数。比下面那个更早变非零，所以能区分
	// "链条没接上"和"接上了但游戏还没开始 evaluate"。
	uint32_t eavesdropLookups;
	// 旁听到的帧数。> 0 就说明我们真的看见了游戏的深度/矢量/jitter。
	uint64_t eavesdropFrames;
	// 最近一帧抄到的东西，够 UI 判断"拿到了没有"
	uint32_t eavesdropRenderWidth;
	uint32_t eavesdropRenderHeight;
	uint32_t eavesdropHasDepth;    // 0/1
	uint32_t eavesdropHasMotion;   // 0/1
	float eavesdropJitterX;
	float eavesdropJitterY;
	// 真正拷进我们自己纹理的帧数（v7）。和上面的 eavesdropFrames 不同：
	// 看见 ≠ 拷到 —— 拷贝要求先从 barrier 观察到资源状态，没观察到就跳过。
	uint64_t eavesdropCapturedFrames;
	uint32_t eavesdropCapturedWidth;
	uint32_t eavesdropCapturedHeight;
	float eavesdropMvScaleX;
	float eavesdropMvScaleY;
	// 0/1：DLSSNR 这一帧真的用上了真矢量（而不是零矢量）
	uint32_t nrUsingRealMotion;
	uint32_t reservedV7;

	/* ---- DLSSNR 跑在游戏的 evaluate 点上（v8 新增）---- */
	// 在游戏的 DLSS evaluate 之前成功处理了多少帧。> 0 = 那条路真的在跑，
	// 也就意味着 DLSSNR 用的是**同分辨率的真矢量**。
	uint64_t nrAtEvaluateFrames;
	// 0/1：设置里要求走 evaluate 点，且条件满足（旁听+DLSS5 都开着）
	uint32_t nrAtEvaluateWanted;
	// NrAtEvaluateBlock。nrAtEvaluateFrames 为 0 时它说明卡在哪一步。
	uint32_t nrAtEvaluateBlocked;

	/* ---- v10 ---- */
	// DLSSNR 自己的 GPU 耗时，常驻显示在界面上。
	// **用指数滑动平均，不是全程均值** —— 全程均值在长会话里会把几分钟前的数字
	// 一直拖进来，玩家看到的不是"现在多少"。改分辨率时会清零重新开始。
	float nrGpuMs;
	float nrGpuMsWorst;
	// 0/1：这个游戏**自己**在用 DLSS（旁听到它解析过 NVSDK_NGX_* 或真的 evaluate 过）。
	// 用来禁掉我们的 DLSS 超分 —— 两套 DLSS 同时跑会崩。
	uint32_t gameDlssSeen;
	// 0/1：总开关（Alt+D）当前的状态。**权威值在 core 这边** —— 它启动时从配置的
	// masterEnabled 读初值，之后只被 Alt+D 改。UI 侧原来自己记一份，结果和 core
	// 不一致时按一下 Alt+D 会"反着来"（UI 以为是开的，core 其实是关的）。
	// 占的是原来的 reservedV10，所以布局和 VERSION 都不用动。
	uint32_t masterEnabled;

	/* ---- v11 ---- */
	// 当前 DLSSNR 参数值。**浮层的改动只落在 core 内存里**（nrSettings），
	// 不带回这块的话工具界面上显示的还是旧值 —— 两边不同步，用户在浮层里
	// 调好的参数一回工具就"变回去了"。UI 拿到后回填输入框并落盘。
	float nrParamIntensity;
	float nrParamLocalTone;
	float nrParamLocalStructure;
	float nrParamSkinStructure;
	uint32_t nrParamPreset;
	uint32_t nrParamStyle;
	uint32_t nrParamAutoMask;       // 0/1
	uint32_t nrParamUiCorrection;   // 0/1
	// 处理分辨率。加浮层同步时漏了它（状态块只有上面 8 个）—— 于是"降分辨率
	// 不能双向同步"（用户实测：游戏里改了它，工具永远显示旧值）。
	float nrParamRenderScale;
	// **参数版本号：只在游戏内浮层改参数（ApplyOverlaySetting）时 +1。**
	// UI 靠它区分"core 真的被人改了"和"我自己推送的回声"：UI 推给 core 走
	// ReloadSettings（不 +1），回声的版本号不变，UI 忽略。不加这个的话，
	// UI 轮询会把"core 还没收到新值"当成"core 改了"，抢在推送防抖之前把
	// 本地编辑覆盖回旧值（用户实测：工具里改 skin，游戏里永远弹回 -1 ——
	// 那是旧配置文件里的旧默认值）。
	uint32_t nrParamVersion;

	// core 侧最近一条消息，UI 显示在日志里
	char message[256];

	// DXL v12: appended to preserve all earlier member offsets.
	float nrParamColourStrength;
	float nrParamSelfLayers;
	int32_t nrParamTrueLayers;
	uint32_t nrParamOpticalFlow;
	int32_t nrParamOpticalFlowQuality;
	uint32_t nrRoute;         // 0 waiting, 1 Present, 2 post-SR Evaluate, 3 automatic handoff paused
	uint32_t nrMotionSource;  // 0 zero, 1 native, 2 optical
	float nrOpticalFlowMs;
	uint32_t nrParamSemanticMask;
	uint32_t nrParamSemanticEnabled;
	float nrParamSemanticBg;
	float nrParamSemanticIntensity[18];
	uint32_t nrParamSemanticDebug;
    uint32_t nrParamSemanticFlipY;
    float nrParamSemanticFeather;
};

// UI -> core 的命令
enum class CommandId : uint32_t {
	None = 0,
	ReloadSettings,   // settings.json 变了，重新读
	SetEnabled,       // arg0: 0/1 总开关
	TogglePerfWindow,
	ToggleDebugView,  // arg0: 0=off 1=depth 2=motion
	Shutdown,         // 卸载 hook 并退出（尽力而为）
	// 迟到注入时真超分需要游戏重建 swapchain 才能生效。core 靠临时改一下游戏
	// 窗口的尺寸来触发（游戏收到 WM_SIZE 后自己 ResizeBuffers）。
	NudgeSwapChain,
    EditNrParameter, // arg0: validated parameter index + fixed-point value
};

struct Command {
	uint32_t magic;
	uint32_t version;
	uint32_t id;       // CommandId
	uint32_t arg0;
};

struct CommandReply {
	uint32_t magic;
	uint32_t version;
	uint32_t accepted;
	uint32_t reserved;
};

}  // namespace DXL::Ipc
