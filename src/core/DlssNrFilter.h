#pragma once

// DLSS5 / DLSSNR —— NGX feature 18，同分辨率的神经渲染滤镜。
//
// 和 DLSS SR 最大的不同：**它没有公开 API**。nvsdk_ngx.h 里没有 feature 18，
// 也没有对应的 helper。所以这里绕过 NGX core，直接加载 nvngx_dlssnr.dll 调它
// 自己导出的 CreateFeature / EvaluateFeature，参数用字符串键设置。
//
// 还有一道门：这个 DLL 会检查调用方身份 —— 它拿自己的模块句柄调
// GetModuleFileNameW，要求返回的文件名是 "nvngx.dll"（正常情况下它是被 NGX core
// 加载的）。我们不是 nvngx.dll，所以要把它导入表里的 GetModuleFileNameW 换掉。
// 改 IAT 而不是 inline hook：只影响这一个模块，不碰进程里其他人的调用。
//
// 关于这个 feature：
//   · 它**确实消费运动矢量**，且依赖时域累积。
//   · 它**也确实消费深度**。所以深度和矢量都必须给真的。
//
// 曾经有一条"DLSSNR 不使用深度通道"的结论写在这里（依据是逐像素噪声投毒时画面没变），
// **那个结论是错的，已经删除**。教训：一次"改了输入但画面没变"的实验只能说明那次
// 没观察到差别，不足以推出"这一路输入被忽略"。据此喂零深度是真的在压画质上限。

#include <windows.h>
#include <d3d12.h>
#include <cstdint>

#include "GpuImage.h"
#include "ComputePasses.h"
#include "OpticalFlow.h"
#include "SemanticMask.h"
#include "../common/IpcProtocol.h"

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_Parameter;

namespace DXL {

// 对应 UI 上 DLSS5 那一组控件。名字沿用 DLL 的参数键，别自己起别名，
// 否则对照 renodx / Magpie 的行为时会多一层翻译。
struct NrSettings {
	bool enabled = false;

	int preset = 0;              // DLSSNR.Hint.Render.Preset
	// 默认 2 = Cinematic（用户拍板的新游戏默认：电影风格）
	int style = 2;               // DLSSNR.Style
	float intensity = 1.0f;      // DLSSNR.Intensity（0..1）
	float colourStrength = 1.0f; // Post-composition: 0 original chroma, 1 current NR RGB.
	float selfLayers = 1.0f;     // 1..3, fractional: final NR residual gain, no extra inference.
	int trueLayers = 1;          // 1..5: serial NR stages, each with its own temporal feature.
	float localTone = 1.0f;      // DLSSNR.LocalToneStrength（0..2）
	float localStructure = 1.0f; // DLSSNR.LocalStructureStrength（0..2）
	// 默认 0.6（用户拍板的新游戏默认）；旧默认 -1 = 不干预，已弃
	float skinStructure = 0.6f;  // DLSSNR.SkinStructureStrength（0..2）
	bool autoMask = true;        // DLSSNR.UseAutoMask（默认开）
	bool uiCorrection = true;    // DLSSNR.UICorrection（默认开）

	// Scene-linear normalization used by the Evaluate encoder. <=0 selects
	// game exposure when supported, otherwise the sampled automatic fallback.
	// Automatic sampling targets encoded neutral grey 0.5 for the active curve.
	float toneScale = 0.0f;
	// Configurable exponent applies only to the legacy pure-gamma encoder.
	float toneGamma = 2.2f;
	// Default: hue-preserving Hybrid shoulder + standard sRGB transfer.
	// Legacy alternative: per-channel clamp + power curve.
	bool pureGamma = false;
	// **诊断用：跳过 DLSSNR 那一次 EvaluateFeature，别的一步不改。**
	//
	// 这是把"崩溃是我们自己的 GPU 活干的"和"是那次**重入 NGX** 的调用干的"分开的
	// 唯一干净办法 —— 两半的修法完全不同。跳过之后滤镜输出等于输入，按比例还原
	// 得到 ratio=1，画面**逐位不变**，但编码、barrier、写回、状态还原全都照跑。
	bool skipNgx = false;
	// **诊断用：不把结果写回游戏的颜色缓冲。**
	//
	// 把"读游戏的资源 + 跑我们自己的 dispatch"和"往游戏的资源里写"分开。
	// 后者是这条路上唯一**修改游戏状态**的动作，也是唯一有替代方案的动作 ——
	// 真是它的话，可以改成把 NGX 的 Color 参数指向我们自己的纹理，一次都不碰它的。
	bool skipWriteBack = false;
	// **诊断用：进来、记账、还原、走。一条 GPU 命令都不下。**
	//
	// 这是最后一刀：如果连这一档都崩，那"往游戏那条命令列表上录任何东西"本身就有问题
	// （或者问题在状态存/还上），evaluate 点这条路在这个引擎上就得放弃。
	// 如果这一档不崩，问题就在我们的两次 dispatch 或 NGX 的 dispatch 上。
	bool dryRun = false;

	// **降分辨率跑 DLSSNR。** 1.0 = 全分辨率（默认），0.5 = 长宽各一半。
	//
	// 为什么需要它：DLSSNR 是固定的神经网络图，**开销只跟像素数走，和强度无关**
	// （强度归零画面不变，但帧数照样掉 —— 用户实测）。所以在 DLSS 之后这条路上，
	// 唯一能省开销的办法就是少给它像素。实测拟合：固定约 1.7ms + 每兆像素约 2.5ms，
	// 于是 4K（8.3 Mpx）大约 22ms —— 60fps 的预算全吃掉，必须能降。
	//
	// **不是把低分辨率结果插值放大。** 那样细节全变成插值来的，画面会明显变软，
	// 等于用 DLSSNR 换来一张更糊的图。做法是只放大**改动量**：
	//   低分输出 / 低分输入 = 比值（低频、平滑） -> 双线性放大 -> 乘到全分辨率原图上
	// 细节全部来自原生分辨率，只有修正量是低分辨率的。
	// 也没有再跑一次 DLSS/FSR：那需要抖动和自己的时域历史，和游戏的 DLSS 叠加会
	// 双重鬼影，而且在 4K 要花 2~4ms，把省下的又吃回去。
	float renderScale = 1.0f;

    // Legacy checker controls remain available to old profiles. Semantic mode
    // takes precedence and follows the DXL RGBA contract (no inverted values).
    bool controlMask = false;
    float controlMaskR = 1, controlMaskG = 1, controlMaskB = 1, controlMaskA = 1;
    bool semanticMask = false;
    float semanticIntensity[SEM_GROUP_COUNT] = {};
    bool semanticFlipY = false; // Only correct the semantic input on the Evaluate route.
    float semanticFeather = 8.0f; // Radius on a mask grid with at most 640 pixels on the long edge.
    uint32_t semanticEnabled = 1;
    float semanticBgIntensity = 1;
    bool semanticDebugView = false;

	// **A/B 开关**：允许单独关掉真矢量 / 真深度，用来判断画面变化到底来自哪一路。
	// 默认开 —— 抄到了就用。关掉时退回零纹理（不是退回"不跑滤镜"）。
	bool useRealMotion = true;
	bool useRealDepth = true;
	bool opticalFlow = true;     // Only when the selected native motion is unavailable.
	int opticalQuality = 1;      // 0 performance (640), 1 balanced (960), 2 quality (1280).

	// debug 视图：把喂给 DLSSNR 的深度/矢量直接画到画面上。
	// 开着时**不跑 DLSSNR** —— 画面就是那张假彩色图，这样看到的一定是原始输入本身，
	// 不会被滤镜的效果混进来。
	// Encoded / Diff 只有 evaluate 点那条路有意义（backbuffer 那条不做色调压缩）。
	//   Encoded = 压缩之后、真正喂给 DLSSNR 的那张图。**先看清它长什么样再猜**——
	//            "滤镜期待什么输入"这件事上我的直觉已经错过两次。
	//   Diff    = |滤镜输出 - 输入| 放大。全黑 = 滤镜什么都没做，这是能证伪的判据。
	enum class DebugView : int {
		Off = 0, Depth = 1, Motion = 2, Encoded = 3, Diff = 4
	};
	DebugView debugView = DebugView::Off;
	// 深度是幂曲线的指数（0.05 左右能把 reversed-Z 的层次拉开），
	// 矢量是偏移增益（20 左右能让常见的 UV 位移看得见）。
	float debugGain = 0.0f;   // 0 = 按视图类型取默认值

	// 诊断用，平时别动
	float mvScale = 1.0f;
	bool depthInverted = true;
	bool forceReset = false;
};

// 在游戏的 DLSS evaluate 点处理一帧所需要的全部输入。
//
// 状态字段是**从游戏的 barrier 观察来的**，不是猜的。处理完我们必须原样还回去 ——
// 游戏后面的命令还按它自己记的状态走，还错了就是未定义行为。
struct NrEvaluateInput {
	ID3D12Resource* color = nullptr;
	D3D12_RESOURCE_STATES colorState{};
	ID3D12Resource* motion = nullptr;
	D3D12_RESOURCE_STATES motionState{};
	// 深度也是真吃的（见文件头）。给不了就留 nullptr，那时才退回零深度。
	ID3D12Resource* depth = nullptr;
	D3D12_RESOURCE_STATES depthState{};
	// 深度是否反转（reversed-Z）。**优先用游戏自己告诉 NGX 的那个值** ——
	// 它在 DLSS 的 create flags 里（bit 3），我们在 evaluate 点能把它读出来。
	// 读不到时 hasDepthInverted 为 false，退回设置里的值。
	bool depthInverted = false;
	bool hasDepthInverted = false;
    bool colorHdrKnown = false;
    bool colorIsHdr = true; // Missing metadata preserves the established HDR path.
	// 游戏真正让 DLSS 处理的范围。可能比资源本身小，两者不一定相等。
	// 这是**颜色/输出**的子矩形（输出分辨率）。
	uint32_t subrectWidth = 0;
	uint32_t subrectHeight = 0;
	// 矢量/深度的子矩形（渲染分辨率）。SR→NR 时颜色是 SR 输出（输出分辨率），
	// 而矢量/深度是 DLSS 的输入（渲染分辨率），两者尺寸不同 —— 必须分开传，否则
	// 模型按输出分辨率采样渲染分辨率的矢量，时域累积错位（画面变糊、效果弱）。
	// present 路径两者相等，调用方传同一个值即可。
	uint32_t guideSubrectWidth = 0;
	uint32_t guideSubrectHeight = 0;
	// 游戏自己给 NGX 的 MV_Scale，直接透传，别用我们设置里的调试值
	float mvScaleX = 0.0f;
	float mvScaleY = 0.0f;
	float jitterX = 0.0f;
	float jitterY = 0.0f;
	// 游戏给 NGX 的预曝光。evaluate 点的颜色是 tonemap **之前**的线性 HDR，
	// 值可以远大于 1 —— 曝光是唯一能把它折回滤镜预期范围的线索。
	float preExposure = 0.0f;
	bool reset = false;
	// 游戏曝光纹理（1x1，游戏填它当前用的曝光值）+ 它的格式。用它算白点。
	ID3D12Resource* exposure = nullptr;
	uint32_t exposureFormat = 0;
	D3D12_RESOURCE_STATES exposureState = D3D12_RESOURCE_STATE_COMMON;
	bool exposureStateKnown = false;
};

// Current resource/feature mode. Automatic switching requires a shared CPU
// state lock AND completion of external Evaluate GPU work. Prepare additionally
// drains this filter's own queue before rebuilding textures/feature/history.
enum class NrMode : uint32_t {
	None = 0,      // 还没用过，两条路都可以认领
	Present,       // 在 present 上处理 backbuffer
	AtEvaluate,    // 原生 SR 完成后处理其输出，位于 FG 之前
};

class DlssNrFilter {
public:
	~DlssNrFilter();

	DlssNrFilter(const DlssNrFilter&) = delete;
	DlssNrFilter& operator=(const DlssNrFilter&) = delete;
	DlssNrFilter() = default;

	bool Initialize(
		ID3D12Device* device,
		ID3D12CommandQueue* queue,
		HMODULE selfModule) noexcept;

	// 退出清理（core 在 RequestTeardown 的回调里调，见 HookTeardown.h）：
	// 等 GPU 把我们自己的活跑完 → 释放 NGX feature。**必须 public 且在这里
	// 调** —— 只还原 vtable 不够：DS2 实测（Documents\*.mdmp）崩溃 PC 在
	// nvngx_dlssnr.dll+0x147f8，游戏调 NGX Shutdown 拆会话时 snippet 还握着
	// 我们没释放的 feature（读 null+0x48）。纹理不在这里放（进程要没了，
	// OS 收走）；feature 必须放（它是挂在**游戏的** NGX 会话上的活物）。
	// False retains the feature when GPU completion is not proven. Caller keeps
	// the process-lifetime owner alive; external Evaluate work is gated separately.
	bool TeardownForExit() noexcept;
	// Bounded completion proof for callers that own the images used by Execute.
	// Does not cover external Evaluate command lists; those use EvaluateGpuGate.
	bool WaitForOwnGpuIdle() noexcept;

	// DLSSNR 是同分辨率滤镜，只需要一个尺寸。
	//
	// allowModeSwitch requires the caller to hold the state lock and prove that
	// external Evaluate work has completed. Default false preserves legacy callers.
	bool Prepare(
		uint32_t width,
		uint32_t height,
		DXGI_FORMAT colorFormat,
		const NrSettings& settings,
		NrMode mode, bool allowModeSwitch = false) noexcept;
	// Caller holds the NR state lock and has drained external Evaluate work.
	// The filter drains its own submissions before adopting the presentation queue.
	bool SetPresentQueue(ID3D12CommandQueue* queue) noexcept;

	NrMode Mode() const noexcept { return _mode; }

	// colorSource 和 dest 可以是同一块资源（原地处理 backbuffer），
	// 也可以让 dest 指向 Handoff()，把结果留在交接纹理里给下一级滤镜。
	bool Execute(const GpuImage& colorSource, const GpuImage& dest) noexcept;

	// 在**游戏自己的命令列表**上跑一次，就地把游戏即将喂给 DLSS 的颜色处理掉。
	//
	// 和 Execute() 的区别：
	//   · 不取命令槽、不 Reset、不 Close、不提交、不 Signal fence —— 这条列表是游戏
	//     的，生命周期归它管。所以这条路也不存在"跳帧"（没有槽位要等）。
	//   · 颜色是**原地**的：游戏颜色 → _colorIn → NGX → _output → 写回游戏颜色。
	//     NGX 不允许输入输出是同一块资源，所以两次拷贝都省不掉。
	//   · 跑在游戏的**渲染线程**上，不是 present 线程。所以开了这条路时，present
	//     路径上绝对不能再碰这个滤镜（core 侧靠 dlss5AtEvaluate 二选一保证）。
	//
	// 为什么插在"游戏自己的 evaluate 之前"这个精确位置，而不是别处：NGX 的
	// EvaluateFeature 内部要 dispatch 计算着色器，必然覆盖这条列表上的 root
	// signature / 描述符堆 / PSO（OptiScaler 专门为此做了 RestoreComputeSig* 选项）。
	// 而游戏紧接着就要自己调一次 EvaluateFeature —— 也就是说它本来就预期这些状态在
	// 这一点上会被打乱、本来就会在之后重新绑定。插在这里，我们蹭的是它已经付过的代价。
	bool ExecuteOnList(
		ID3D12GraphicsCommandList* list, const NrEvaluateInput& input) noexcept;

	// 串联给 DLSS SR 时用这个当它的输入。
	//
	// **降分辨率时必须交出 _fullOut，不是 _output。** _output 是滤镜实际跑的那个
	// 低分辨率纹理；把它交给 SR，SR 会收到一张尺寸不符的输入然后直接失败
	// （实测 NGX_D3D12_EVALUATE_DLSS_EXT failed: 0xBAD00005）。
	// _fullOut 是比值放大之后的全分辨率结果，尺寸和不降分辨率时完全一样，
	// 所以上层什么都不用改。
	GpuImage Handoff() const noexcept {
		return { _scaled && _fullOut ? _fullOut : _output,
			D3D12_RESOURCE_STATE_UNORDERED_ACCESS };
	}

	// 用真运动矢量 / 真深度代替零纹理。
	//
	// **尺寸必须和 NR 的工作分辨率一致**，否则 DLSSNR 会按自己的子矩形去采一张更小/
	// 更大的图，出来的是错位的鬼影 —— 那比零纹理更糟。所以尺寸不符时调用方要传
	// nullptr，我们老老实实用零纹理。
	// scaleX/Y 直接用游戏给 NGX 的 MV_Scale：实测鬼武者是 (1286, 724) = 渲染分辨率，
	// 说明它的矢量在归一化 UV 空间。
	// motionWidth/Height 是那张矢量纹理**自己**的尺寸。和 NR 的工作分辨率不一致时
	// 我们用计算着色器把它重采样上来 —— 这就是 backbuffer 那条路能用上真矢量的办法。
	void SetExternalMotion(
		ID3D12Resource* motion, uint32_t motionWidth, uint32_t motionHeight,
		float scaleX, float scaleY) noexcept {
		_externalMotion = motion;
		_externalMotionWidth = motionWidth;
		_externalMotionHeight = motionHeight;
		_externalMvScaleX = scaleX;
		_externalMvScaleY = scaleY;
	}
	// depthWidth/Height 同理：不一致时我们用最近邻重采样上来。
	void SetExternalDepth(ID3D12Resource* depth,
		uint32_t depthWidth, uint32_t depthHeight) noexcept {
		_externalDepth = depth;
		_externalDepthWidth = depthWidth;
		_externalDepthHeight = depthHeight;
	}
	bool UsingExternalMotion() const noexcept { return _externalMotion != nullptr; }
	bool UsingExternalDepth() const noexcept { return _externalDepth != nullptr; }
    uint32_t ExternalMotionWidth() const noexcept { return _externalMotionWidth; }
    uint32_t ExternalMotionHeight() const noexcept { return _externalMotionHeight; }
    uint32_t ExternalDepthWidth() const noexcept { return _externalDepthWidth; }
    uint32_t ExternalDepthHeight() const noexcept { return _externalDepthHeight; }

	OpticalFlowStatus OpticalStatus() const noexcept { return _optical.Status(); }
	void InvalidateOpticalHistory() noexcept { _optical.Suspend(); _needsReset = true; }

	// **诊断：把处理前后的真实像素值抄回 CPU 打进日志。**
	//
	// 为什么必须有这个：evaluate 点上画面变成一片灰，而"输入范围不对"和"滤镜输出了
	// 一个常数"这两种原因在画面上长得一样，靠看是分不出来的。数值能。
	//
	// 实现上刻意**不阻塞**：在游戏的命令列表上录一次 CopyTextureRegion 到 readback
	// 堆（正好蹭已经搬到 COPY_SOURCE 的那两个时机，不多下一条 barrier），
	// 然后等几十帧之后再 Map —— 那时 GPU 一定早就过去了，不需要等 fence。
	void ArmPixelDump(uint64_t atEvaluateFrame, bool quiet = false) noexcept {
		_dumpAtFrame = atEvaluateFrame;
		_dumpReported = false;
		_dumpRecordedAt = 0;
		_dumpAttempts = 0;
		_dumpQuiet = quiet;
	}

	// 夹具专用：GPU 已经确定做完了（自检自己等过 fence），跳过"等 60 帧"那道门
	// 直接把回读结果打出来。真实游戏里不要用这个 —— 那边没人保证 GPU 到位。
	void FlushPixelDump() noexcept {
		_dumpForce = true;
		ReportPixelDump();
		_dumpForce = false;
	}

	bool IsReady() const noexcept { return _feature != nullptr; }
	bool IsDisabled() const noexcept { return _disabled; }
	uint32_t Width() const noexcept { return _width; }
	uint32_t Height() const noexcept { return _height; }
	uint64_t EvaluateCount() const noexcept { return _evaluateCount; }
	uint64_t ModelCallCount() const noexcept { return _modelCallCount; }
	int LiveLayerCount() const noexcept { return _feature ? _settings.trueLayers : 0; }
	uint64_t FailureCount() const noexcept { return _failureCount; }
	// 界面上常驻的 DLSSNR 耗时读数。样本数为 0 时返回 0，UI 据此显示"—"。
	float RecentGpuMs() const noexcept { return float(_timingEmaMs); }
	float WorstGpuMs() const noexcept { return float(_timingWorstMs); }
	uint32_t TimingSamples() const noexcept { return _timingSamples; }
	uint64_t SkippedFrames() const noexcept { return _skippedFrames; }
	const char* LastError() const noexcept { return _lastError; }
	// evaluate 点那条路最近一次被什么挡住了（Ipc::NrAtEvaluateBlock）。
	// 成功跑一帧就清回 None。
	uint32_t LastBlock() const noexcept { return _lastBlock; }
	// 实际生效的白点（自动或手填）。给日志和诊断用。
	float EffectiveWhitePoint() const noexcept;
	static float ClampWhitePoint(float wp) noexcept;
	uint32_t CurveFlags() const noexcept;
	float DebugOutScale() const noexcept;

    struct SemanticMaskProvider {
        bool (*snapshot)(void*, SemanticMaskSnapshot&) noexcept = nullptr;
        bool (*capture)(void*, ID3D12GraphicsCommandList*, ID3D12Resource*, D3D12_RESOURCE_STATES, bool) noexcept = nullptr;
        void (*submitted)(void*, ID3D12CommandQueue*, UINT, ID3D12CommandList* const*) noexcept = nullptr;
        void* ctx = nullptr;
        bool Valid() const noexcept { return ctx && snapshot; }
    };
    void SetSemanticMaskProvider(const SemanticMaskProvider& provider) noexcept {
        if (_semantic.ctx != provider.ctx || _semantic.snapshot != provider.snapshot) {
            _semanticSnapshot = {}; _maskVersion = UINT64_MAX;
        }
        _semantic = provider;
    }
    // Test/read-only diagnostics: RGBA8 UNORM in NON_PIXEL_SHADER_RESOURCE after evaluation.
    ID3D12Resource* ControlMaskResource() const noexcept { return _controlMask; }

private:
    friend struct NrLayerTestAccess;
	// 见 DlssSrUpscaler.h 里同名常量的说明
	static constexpr uint32_t SLOT_COUNT = 4;
	static constexpr DWORD SLOT_WAIT_BUDGET_MS = 4;

	struct Slot {
		ID3D12CommandAllocator* allocator = nullptr;
		ID3D12GraphicsCommandList* commandList = nullptr;
		uint64_t fenceValue = 0;
	};

	// 游戏的矢量到底能不能重采样。
	//
	// 判据：**游戏给 NGX 的 MVecScale 等于矢量纹理自己的尺寸** —— 那是"矢量存在
	// 归一化 UV 空间"的标志（鬼武者实测 (1286, 724) 正好等于 1286x724）。
	// UV 与分辨率无关，所以重采样只是空间缩放、值原样搬。
	// 如果不等，说明矢量是别的约定（比如已经是像素单位），那样缩放会算错方向和幅度，
	// 出来是错位鬼影 —— 比零矢量更糟，所以宁可不做。
	bool CanResampleMotion() noexcept;
	bool LoadSnippet(HMODULE selfModule) noexcept;
	void UnloadSnippet() noexcept;
	bool CreateCommandObjects() noexcept;
    bool EnsureControlMaskFilled(ID3D12GraphicsCommandList* list, uint32_t uploadSlot) noexcept;
    void CaptureSemanticInput(ID3D12GraphicsCommandList* list, ID3D12Resource* color) noexcept;

	bool CreateTextures(DXGI_FORMAT colorFormat, bool atEvaluate) noexcept;
	void ReleaseTextures() noexcept;
	bool CreateFeature() noexcept;
	void DestroyFeature() noexcept;
	void SetCreateParameters(NVSDK_NGX_Parameter* parameters) noexcept;
	NVSDK_NGX_Parameter* LayerParameters(int layer) const noexcept;
	ID3D12Resource* LayerOutput(int layer) const noexcept;
	// Returns an NVSDK_NGX_Result as unsigned to keep the private SDK out of this header.
	uint32_t EvaluateLayers(ID3D12GraphicsCommandList* list) noexcept;
	// color/motion 之外的都是"这一帧的"参数。两条路（present 路径和 evaluate 点）
	// 共用这一份，避免两套参数各写一遍慢慢漂移。
	void SetEvaluateParameters(
		ID3D12Resource* color,
		ID3D12Resource* motion,
		ID3D12Resource* depth,
		bool depthInverted,
		uint32_t subrectWidth,
		uint32_t subrectHeight,
		uint32_t guideSubrectWidth,
		uint32_t guideSubrectHeight,
		float mvScaleX,
		float mvScaleY,
		float jitterX,
		float jitterY,
		float preExposure,
		bool reset) noexcept;
	// 把"snippet 问了但我们没给"的键名倒进日志。只倒一次。
	//
	// 这是唯一一条能问出 DLSSNR 真实接口的路：它没有公开头文件，我们那张参数表是
	// 逐个试出来的。第一次在真实游戏的 evaluate 点上跑，画面变成一片灰，猜不如让它自己说。
	void LogParameterMisses() noexcept;
	// 把清零纹理的清除命令录进某条列表。
	void RecordZeroClears(ID3D12GraphicsCommandList* list) noexcept;
	// 同上，但用**我们自己的**命令列表并立刻提交。
	//
	// evaluate 点那条路必须走这个版本：ClearUnorderedAccessViewFloat 要先
	// SetDescriptorHeaps，而那会覆盖游戏在它自己列表上绑好的描述符堆 ——
	// 绝不能在游戏的列表上干这件事。
	bool EnsureZeroTexturesCleared() noexcept;
	// 返回 false = 这一帧跳过（不是错误，也不停用自己）。调用方直接放弃这一帧。
	bool TryClaimSlot(Slot& slot) noexcept;

	// **等我们提交的所有活跑完。**
	//
	// 重建纹理 / feature 之前必须调。命令槽轮转 4 个，也就是我们手上随时可能有
	// 3 帧还没执行完的命令列表，它们全都在引用旧的 _colorIn / _output / _fullOut /
	// _ratio 和旧的 NGX feature。不等就直接释放 = GPU 用已释放的资源 —— 游戏当场死。
	//
	// 这个坑在 NrMode 那段注释里被我写过（"运行中切换会崩"），但只在**换路径**那件事上
	// 防住了；分辨率是"同一条路上换尺寸"，走的是同一段释放代码，却一直没人等 fence。
	// 以前它是潜伏的（尺寸只在改游戏分辨率时变，那时 swapchain 重建自己带同步），
	// 加了「处理分辨率」滑块之后变成必然触发：点一次应用就崩。
	void WaitForOwnWorkIdle() noexcept;
	// 上一次等 fence 超时了 —— 那种情况下不许释放旧资源，也不许当成重建成功。
	bool _pendingRebuildBlocked = false;

	// **量一下我们到底花了多少 GPU 时间。**
	//
	// 这条路（DLSS 之后、输出分辨率）存在的代价就是性能，而"贵多少"一直只是估算。
	// 时间戳查询是唯一能给出真实数字的办法，而且这里几乎免费：present 路径本来就有
	// 命令槽和 fence，槽位轮转到下一圈时上一次的结果一定已经写完了，不用额外同步。
	bool EnsureTimingQueries() noexcept;
	void RecordTimingBegin(
		ID3D12GraphicsCommandList* list, uint32_t slotIndex) noexcept;
	void RecordTimingEnd(
		ID3D12GraphicsCommandList* list, uint32_t slotIndex) noexcept;
	void ReportTiming(uint32_t slotIndex) noexcept;

	ID3D12QueryHeap* _timingHeap = nullptr;
	ID3D12Resource* _timingReadback = nullptr;
	uint64_t _timingFrequency = 0;
	// 这个槽位有没有录过时间戳（第一圈不能读，里面是垃圾）
	bool _timingArmed[8]{};
	double _timingSumMs = 0.0;
	uint32_t _timingSamples = 0;
	double _timingWorstMs = 0.0;
	// 界面常驻显示用的近期值。全程均值不适合当实时读数（见 IpcProtocol 里的说明）。
	double _timingEmaMs = 0.0;
	void Fail(const char* what) noexcept;

	ID3D12Device* _device = nullptr;
	ID3D12CommandQueue* _queue = nullptr;

	// snippet DLL 及其导出
	HMODULE _snippet = nullptr;
	void* _initExt = nullptr;
	void* _createFeature = nullptr;
	void* _evaluateFeature = nullptr;
	void* _releaseFeature = nullptr;
	void* _shutdown = nullptr;
	void** _getModuleFileNameSlot = nullptr;   // 被我们改掉的 IAT 槽位
	bool _snippetInitialized = false;

	NVSDK_NGX_Handle* _feature = nullptr;
	NVSDK_NGX_Parameter* _parameters = nullptr;
	// 上面那个指针指向的实体。自己实现的容器，不经过 NGX core —— 这样 DLSSNR
	// 可以和游戏原生的 DLSS 共存。
	class NgxParameterBag* _parameterBag = nullptr;
	NVSDK_NGX_Handle* _extraFeatures[4]{};
	class NgxParameterBag* _extraParameters[4]{};
	uint64_t _modelCallCount = 0;
	bool _layerSetupFailed = false;
	NrMode _failedLayerMode = NrMode::None;
	DXGI_FORMAT _failedLayerFormat = DXGI_FORMAT_UNKNOWN;

	// HDR <-> 0..1 的可逆往返。只有 evaluate 点那条路用得到 ——
	// backbuffer 上的颜色已经是 display-referred 的，不需要压。
	ComputePasses _passes;
	// 解码后的结果。不能直接解码进游戏的颜色缓冲：那块资源不一定带
	// ALLOW_UNORDERED_ACCESS，建不出 UAV。所以多一张，再 CopyResource 过去。
	ID3D12Resource* _decoded = nullptr;
    bool _evaluateLdr = false;
    SemanticMaskProvider _semantic;
    SemanticMaskSnapshot _semanticSnapshot;
    ID3D12Resource* _controlMask = nullptr;
    ID3D12Resource* _controlMaskLow = nullptr;
    bool _controlMaskLowReady = false;
    // One upload per own command slot plus one externally fenced Evaluate slot.
    // Reusing a mapped upload while the GPU reads another frame is forbidden.
    ID3D12Resource* _controlUploads[5]{};
    bool _controlMaskFilled = false, _maskWasFresh = false;
    uint64_t _maskVersion = UINT64_MAX;
    uint64_t _maskSettingsHash = 0;

	ID3D12Resource* _colorIn = nullptr;    // 原地处理时的中转
	ID3D12Resource* _output = nullptr;     // NGX 的输出（UAV）
	ID3D12Resource* _layerPing = nullptr; // Only for trueLayers > 1; final stage always uses _output.
	ID3D12Resource* _zeroDepth = nullptr;
	ID3D12Resource* _zeroMotion = nullptr;
	OpticalFlow _optical;
	bool _usedOptical = false;
	ID3D12DescriptorHeap* _uavHeapGpu = nullptr;
	ID3D12DescriptorHeap* _uavHeapCpu = nullptr;
	uint32_t _uavStride = 0;
	bool _zeroTexturesCleared = false;

	Slot _slots[SLOT_COUNT]{};
	uint32_t _nextSlot = 0;
	ID3D12Fence* _fence = nullptr;
	HANDLE _fenceEvent = nullptr;
	uint64_t _fenceValue = 0;

	uint32_t _width = 0;
	uint32_t _height = 0;
	DXGI_FORMAT _colorFormat = DXGI_FORMAT_UNKNOWN;
	uint32_t _loggedTypelessColorViews = 0;
	NrSettings _settings;

	bool _needsReset = true;
	bool _disabled = false;
	uint64_t _evaluateCount = 0;
	uint64_t _failureCount = 0;
	uint64_t _skippedFrames = 0;
	uint64_t _setupWaitSkips = 0;
	// 外部（旁听游戏 DLSS 得到的）运动矢量。不持有引用 —— 它是旁听那边的副本，
	// 生命周期由那边管。
	// 重采样到 NR 工作分辨率之后的矢量。只有尺寸不一致时才用得到。
	ID3D12Resource* _motionUpscaled = nullptr;
	// 深度重采样到工作分辨率之后放这里。单通道 float —— 深度值本身与分辨率无关。
	ID3D12Resource* _depthUpscaled = nullptr;
	ID3D12Resource* _externalMotion = nullptr;
	uint32_t _externalMotionWidth = 0;
	uint32_t _externalMotionHeight = 0;
	ID3D12Resource* _externalDepth = nullptr;
	uint32_t _externalDepthWidth = 0;
	uint32_t _externalDepthHeight = 0;
	float _externalMvScaleX = 0.0f;
	float _externalMvScaleY = 0.0f;
	const char* _lastError = "";
	uint32_t _lastBlock = uint32_t(Ipc::NrAtEvaluateBlock::None);
	bool _missesLogged = false;
	NrMode _mode = NrMode::None;

	/* ---- 像素回读诊断 ---- */
	// 抄一个小方块就够判断量级。8x8 的 R11G11B10 一行 32 字节，
	// readback 的行距要对齐到 256，所以缓冲是 256*8。
	static constexpr uint32_t DUMP_SIZE = 8;
	// 录完之后等这么多帧再 Map。不等 fence —— 这是诊断，不值得为它在游戏的渲染线程
	// 上加一个同步点。
	static constexpr uint64_t DUMP_SETTLE_FRAMES = 60;
	// 抄到全黑就往后挪这么多帧再试。游戏开头几秒可能一直在淡入 / 过场，
	// 靠人掐时间的探针不算探针。
	static constexpr uint64_t DUMP_RETRY_FRAMES = 600;
	static constexpr uint32_t MAX_DUMP_ATTEMPTS = 12;
	// 自动白点多久重测一次。场景亮度会随镜头/场地变化一个数量级，定死就会错。
	// 300 帧 ≈ 5 秒，够跟上场景切换，又不会让白点抖动。
	static constexpr uint64_t WHITE_POINT_REFRESH_FRAMES = 300;
	// 自动白点第一次测量放在第几帧（相对开始处理的那一帧）。
	// 不能是 0：游戏刚进画面往往还在淡入，测到的是黑屏；也不该太晚，
	// 在它测出来之前用的都是 4.0 保底，那段时间滤镜基本无效。30 帧 ≈ 0.5 秒，
	// 加上 DUMP_SETTLE_FRAMES 的 60 帧，第一个白点大约 1.5 秒后到位。
	static constexpr uint64_t AUTO_WHITE_POINT_FIRST_FRAME = 30;
	// 自动白点的安全区间。淡入淡出/黑屏时测出来的亮度接近 0，推出来的白点会
	// 塌到零点几 —— 那等于给整张图乘上百倍，出黑屏那一刻直接过曝。
	static constexpr float WHITE_POINT_MIN = 0.25f;
	static constexpr float WHITE_POINT_MAX = 200.0f;

	bool EnsureDumpBuffers() noexcept;
	void RecordDumpCopy(
		ID3D12GraphicsCommandList* list,
		ID3D12Resource* source,
		ID3D12Resource* destination) noexcept;
	void ReportPixelDump() noexcept;
	// 游戏曝光纹理回读（用它算白点）。
	bool EnsureExposureReadback() noexcept;
	void RecordExposureRead(
		ID3D12GraphicsCommandList* list, ID3D12Resource* exposure) noexcept;
	void TryReadExposure() noexcept;

	uint64_t _dumpAtFrame = 0;        // 0 = 关
	uint64_t _dumpRecordedAt = 0;     // 录下拷贝时的 _evaluateCount，0 = 还没录
	uint32_t _dumpAttempts = 0;       // 因为抄到全黑而重试过几次
	bool _dumpReported = false;
	bool _dumpForce = false;
	// true = 这次回读只为测白点，日志只留一行。
	// 自动白点是常驻功能（每 5 秒一次），配着十几行排障信息一起打会把日志淹掉。
	bool _dumpQuiet = false;
	ID3D12Resource* _dumpBefore = nullptr;
	ID3D12Resource* _dumpAfter = nullptr;
	// 压缩域里的一对：喂给滤镜的 vs 滤镜吐出来的。
	// **这一对才能回答"滤镜到底动了多少"** —— before/after 那一对是线性 HDR 域的，
	// 中间隔着压缩和还原，滤镜的改动被曲线揉过一道，看不清。
	ID3D12Resource* _dumpFilterIn = nullptr;
	ID3D12Resource* _dumpFilterOut = nullptr;
	// 写回**之后**再读一次游戏的颜色缓冲。
	// 这是"我们写进去的东西到底有没有被游戏用"的唯一直接判据 ——
	// 前面所有采样都在我们自己的纹理上，证明不了这一点。
	ID3D12Resource* _dumpWritten = nullptr;
	// 自动白点：从像素回读测到的场景亮度推出来。0 = 还没测到，用保底值。
	float _autoWhitePoint = 0.0f;
	// **这一帧编码时实际用的白点。** 诊断必须用它，不能现算 ——
	// ReportPixelDump 自己会在中途更新 _autoWhitePoint，现算出来的是"下一帧的白点"，
	// 拿它去核对"上一帧编出来的数"必然对不上，而那个假警报会把人引到编码器上去查。
	float _dumpWhitePoint = 0.0f;
	// 降分辨率那条路：_width/_height 是**滤镜实际跑的**（低）分辨率，
	// _fullWidth/_fullHeight 是画面真正的分辨率。不加这一对的话下游全都会按低分辨率
	// 去拷贝目标，画面会变成左上角一小块（这个坑在代理 backbuffer 上踩过一次）。
	uint32_t _fullWidth = 0;
	uint32_t _fullHeight = 0;
	bool _scaled = false;
	ID3D12Resource* _fullOut = nullptr;
	ID3D12Resource* _downsampled = nullptr; // Linear HDR before scaled Evaluate encoding
	ID3D12Resource* _ratio = nullptr;     // 低分辨率的改动量（比值）
	// 上一次回读测到的场景亮度 / 编码值区间。debug 视图靠它们决定写出倍数和拉伸区间 ——
	// 不然一张窄带信号在 tonemap 之后必然是一团灰，而那看起来正好像"输入是灰的"。
	float _lastSceneLuma = 0.0f;
	float _lastEncodedLo = 0.0f;
	float _lastEncodedHi = 0.0f;
	// 游戏曝光纹理（1x1，游戏填它当前用的曝光值）的 CPU 回读。用它算白点才对 ——
	// "画面正中 8x8 亮度/1.5"只代表局部，高光过曝。白点 = preExposure / exposure。
	ID3D12Resource* _exposureReadback = nullptr;
	float _gameExposure = 0.0f;        // 曝光纹理回读值（0=还没读到/游戏没给）
	float _gamePreExposure = 1.0f;     // 同帧的 preExposure 缓存
	uint32_t _exposureFormat = 0;      // 曝光纹理格式（决定怎么解码回读字节）
	uint64_t _exposureReadFrame = 0;   // 上次 Map 读值的帧号
};

}  // namespace DXL
