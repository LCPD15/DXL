#pragma once
#include "FrameGenSwapChains.h"

// 旁听游戏自己的 DLSS 调用。
//
// 动机：游戏调 `NVSDK_NGX_D3D12_EvaluateFeature` 时，参数包里已经有我们一直在用启发式
// 猜、或者干脆拿不到的全部输入 —— 真深度、真运动矢量（含缩放）、jitter、UI 合成之前的
// 颜色、曝光、reactive mask、reset 标志。旁听一次就全拿到了，里程碑 4/5 的启发式可以
// 整块删掉。
//
// **只旁听，不替换。** 我们把参数抄一份，然后原样调真的 NGX。游戏自己的 DLSS 该怎么跑
// 还怎么跑 —— 这和 OptiScaler 那种"冒充 nvngx 替换掉游戏的上采样器"是完全不同的定位，
// 也小得多、危险得多小：真的 nvngx 继续负责一切，我们只在一个函数上搭个便车。
//
// 怎么插进去（实测决定的，不是设计选的）：
//   · 整条链上没有一处静态导入 nvngx 或 sl.*，全是 LoadLibrary + GetProcAddress，
//     所以直接 IAT 补丁 NGX 无从下手；
//   · 但游戏 exe / sl.interposer / sl.common / sl.dlss **每一个**都静态导入了
//     KERNEL32 的 `LoadLibrary*` 和 `GetProcAddress`；Unity 游戏则由 UnityPlayer.dll 发起加载；
//   → 于是：补丁游戏 exe / UnityPlayer.dll 的加载器函数 → 加载 sl.interposer 时补丁那一个 →
//     链式往下 → 谁问 `NVSDK_NGX_D3D12_EvaluateFeature`，就把**这一个**函数换成
//     我们的包装，其余原样返回。
//
// 早注入通常能接上加载器链。普通工作线程还会定期枚举已加载的已知 SR/RR 模块，
// 仅为其 D3D12 Evaluate 导出安装补充钩子，覆盖已经缓存的函数指针；不碰 FG 导出。

#include <windows.h>
#include <atomic>
#include <cstdint>

struct ID3D12Resource;
struct ID3D12Device;
struct ID3D12CommandQueue;
struct ID3D12GraphicsCommandList;

namespace DXL {

// 从游戏的 DLSS 调用里抄下来的一帧输入。
//
// 这些指针是**游戏的**资源，我们不持有引用（持有会妨碍它回收，swapchain 那次已经
// 教过一遍了）。只在同一帧内有效 —— frameSeq 用来判断新鲜度。
struct NgxEavesdropFrame {
	ID3D12Resource* color = nullptr;         // 渲染分辨率、UI 合成之前
	ID3D12Resource* depth = nullptr;         // 真深度
	ID3D12Resource* motionVectors = nullptr; // 真运动矢量
	ID3D12Resource* output = nullptr;        // 游戏 DLSS 的输出（输出分辨率）
	ID3D12Resource* exposure = nullptr;
	ID3D12Resource* biasColorMask = nullptr; // reactive mask

	float jitterX = 0.0f;
	float jitterY = 0.0f;
	float mvScaleX = 0.0f;
	float mvScaleY = 0.0f;
	float preExposure = 0.0f;
	float frameTimeDeltaMs = 0.0f;
	int reset = 0;
	// 深度是否反转（reversed-Z）。这一位不在每帧的参数里，而在 DLSS 的
	// **create flags**（bit 3）里 —— 我们只 hook 了 EvaluateFeature，看不到创建那一刻。
	// 但游戏往往复用同一个参数对象，所以 evaluate 时那个键常常还在，能读出来。
	// 读不到时 hasDepthInverted 为 false，调用方要自己决定默认值，别当成"不反转"。
	bool depthInverted = false;
	bool hasDepthInverted = false;
    bool colorHdrKnown = false;
    bool colorIsHdr = true; // Missing metadata preserves the established HDR path.

	uint32_t renderWidth = 0;    // 子矩形尺寸（游戏真正让 DLSS 处理的范围）
	uint32_t renderHeight = 0;

	// 从资源自己的 desc 读出来的真实尺寸/格式。子矩形可能比资源本身小。
	uint32_t colorWidth = 0;
	uint32_t colorHeight = 0;
	uint32_t colorFormat = 0;
	uint32_t depthWidth = 0;
	uint32_t depthHeight = 0;
	uint32_t depthFormat = 0;
	uint32_t motionWidth = 0;
	uint32_t motionHeight = 0;
	uint32_t motionFormat = 0;
	// SR 输出（输出分辨率）的真实尺寸/格式。SR→NR 时用它做 NR 的输入颜色。
	uint32_t outputWidth = 0;
	uint32_t outputHeight = 0;
	uint32_t outputFormat = 0;
	// 游戏的曝光纹理（1x1，游戏填它当前用的曝光值）。用它算白点才对 ——
	// 我们之前用"画面正中亮度/1.5"只代表局部，高光过曝。
	uint32_t exposureWidth = 0;
	uint32_t exposureHeight = 0;
	uint32_t exposureFormat = 0;

	uint64_t frameSeq = 0;       // 第几次旁听到，0 = 还没有
};

// 我们自己保存的那一份拷贝。
//
// **为什么必须拷**：旁听拿到的指针只在游戏的 DLSS evaluate 那一刻有效，到我们
// present 时游戏很可能已经把那些深度/矢量缓冲复用了。所以在 evaluate 那一刻就用
// 游戏的命令列表把它们拷进我们自己的纹理，present 时用副本。
//
// 拷贝只用 ResourceBarrier + CopyResource，**不碰 root signature / PSO** ——
// 那是 OptiScaler 在 RE Engine 上要专门 workaround 的东西（`RestoreComputeSig*`），
// 我们绕开。
struct NgxCapturedTextures {
	ID3D12Resource* depth = nullptr;
	ID3D12Resource* motionVectors = nullptr;
	uint32_t width = 0;
	uint32_t height = 0;
	float mvScaleX = 0.0f;
	float mvScaleY = 0.0f;
	uint64_t frameSeq = 0;       // 这份副本是第几帧拷的
};

class NgxEavesdrop {
public:
	static NgxEavesdrop& Get() noexcept;

	// 把加载器函数的 IAT 补丁装到"根"模块上（挂起注入时就是游戏 exe）。
	// 之后新加载的模块由 LoadLibrary 的 hook 自动接上。
	// selfModule 用来跳过我们自己 —— 绝不能 hook 自己的调用。
	// enabled = false 时什么都不装。**默认就是关的** —— 这套东西第一版有个
	// "多个 snippet 共用一个真地址"的 bug，把游戏画面撕成了条带错位。修好之后仍然
	// 保持 opt-in（settings.json 里的 diagEavesdrop），等真实游戏验证过再考虑默认开。
	void Install(HMODULE selfModule, bool enabled) noexcept;

	// 拷贝需要设备。core 探测到 D3D12 设备之后调一次。
	void SetDevice(ID3D12Device* device) noexcept;
	void SetEvaluateMode(bool enabled) noexcept { _evaluateMode.store(enabled); }
	bool EvaluateMode() const noexcept { return _evaluateMode.load(); }

	// 换链暂停：从现在起 ms 毫秒内不往游戏的命令列表拷深度/矢量。
	//
	// 为什么需要：游戏重建 swapchain / ResizeBuffers 之后有一小段 DXGI 换链同步
	// 窗口，往游戏的命令列表里录 barrier + CopyResource 会撞上它（生化9 开 FG 实测
	// 在"第 2 次创建 swapchain + 第一次拷贝成功"后 ~468ms ACCESS_LOST）。core 在
	// HookedCreateSwapChain / HookedResizeBuffers 里检测到换链时，除了暂停 UI/NR，
	// 也要调这个让旁听拷贝一起停一小段，等新链稳定。
	void PauseCaptureFor(uint64_t ms) noexcept {
		_capturePauseUntil.store(GetTickCount64() + ms, std::memory_order_release);
	}
	// 现在是否处于换链暂停期（供内部 ForwardEvaluate 判断要不要拷）。
	bool CapturePaused() const noexcept {
		const uint64_t until = _capturePauseUntil.load(std::memory_order_acquire);
		return until != 0 && GetTickCount64() < until;
	}

	// **从游戏的 ResourceBarrier 里观察资源状态。**
	//
	// 为什么不能猜：CopyResource 要求源处于 COPY_SOURCE，所以必须先 transition，
	// 而 transition 的"before"状态写错就是未定义行为（调试层会报，但真实游戏里没有
	// 调试层）。NGX 文档说输入应该在 NON_PIXEL_SHADER_RESOURCE，但 OptiScaler 专门为
	// "引擎把 DLSS 资源交在错误状态上"做了可配置的 workaround（它的 Issues.md 点名
	// Unreal 的 DLSS 插件），说明这事儿因游戏而异，猜不得。
	// 所以改成只用**观察到的**状态：没观察到就不拷，宁可少一路数据也不要未定义行为。
	// list = 录下这条 barrier 的命令列表。**这个参数是为一个具体问题加的**：
	// 游戏把颜色写完、转成只读的那条 barrier，是录在"装着 DLSS 调用的那条列表"上，
	// 还是**更早的另一条列表**上？
	//
	// 这决定了"不碰游戏的命令列表，也能在 DLSS 之前处理低分辨率颜色"到底可不可行：
	//   · 同一条列表 -> 颜色是在这条还没提交的列表里写的。我们自己提交一条列表会跑在
	//     它**前面**，读到的是上一帧的颜色 -> 不可行。
	//   · 更早的列表 -> 那次写已经排进队列了。我们在 evaluate 这一刻提交自己的列表，
	//     它就正好夹在"写颜色"和"读颜色"之间 -> **可行，而且一次都不碰游戏的列表**。
	// 这个前提我之前一直是假设的（假设同一条），从没验证过。
	void NoteBarrier(ID3D12Resource* resource, uint32_t stateAfter,
		void* list = nullptr) noexcept;
	// 某个资源最后一次 barrier 是录在哪条列表上的。拿不到就返回 nullptr。
	void* LastBarrierList(ID3D12Resource* resource) const noexcept;
	bool ResourceState(ID3D12Resource* resource, uint32_t& state) const noexcept;

	// 最近拷好的一份。frameSeq 为 0 表示还没有。
	NgxCapturedTextures Captured() const noexcept;

	// 在游戏的 DLSS evaluate **之前**回调，让上层有机会在**游戏自己的命令列表**上
	// 处理它即将喂给 DLSS 的颜色。
	//
	// 这是"把 DLSSNR 挪到 evaluate 点"的接入口。为什么非得在这儿：
	//   · 颜色和运动矢量在这里是**同一个分辨率**（游戏的渲染分辨率）。在 present 上
	//     做不到 —— 那里颜色已经是 backbuffer 尺寸，而矢量还是渲染尺寸，喂进去是
	//     错位鬼影。
	//   · 这里的颜色是 UI 合成**之前**的，顺带解决 DLSSNR 会"增强"UI 文字的问题。
	//   · 处理完写回游戏的颜色缓冲，效果自然流进它自己的 DLSS。
	//
	// colorState / motionState 是**观察到的**资源状态；这两个观察不到就不回调
	// （拿错状态下 barrier 是未定义行为）。depthState 允许为 0 —— 深度是"有更好"，
	// 观察不到时实现方退回零深度就行，不必因此放弃整帧。
	// 回调跑在游戏的渲染线程上，不是 present 线程 —— 实现方必须自己保证不和 present
	// 路径并发用同一个滤镜。
	using PreEvaluateHook = bool (*)(
		ID3D12GraphicsCommandList* list,
		const NgxEavesdropFrame& frame,
		uint32_t colorState,
		uint32_t motionState,
		uint32_t depthState);
	void SetPreEvaluateHook(PreEvaluateHook hook) noexcept;

	// 回调真的跑成功了多少帧。0 = 没接上（没开、没观察到状态、或者滤镜没就绪）。
	uint64_t PreEvaluateFrames() const noexcept;
	// 因为"还没观察到颜色/矢量的状态"而放弃的帧数。用来把这个原因和别的原因分开。
	uint64_t PreEvaluateStateMisses() const noexcept;

	// **诊断夹具**：不用真游戏，拿一组假资源把"evaluate 点处理"这条路整走一遍。
	//
	// 为什么需要它：那条路全是 ResourceBarrier + CopyResource，写错 before 状态、
	// 或者两侧格式不同族，都是**未定义行为而不是报错** —— 真实游戏里没有调试层会
	// 告诉你，画面可能只是偶尔闪一下。而我们的测试目标是开着 D3D12 调试层跑的，
	// 让调试层去挑错，比在真游戏上盯画面可靠得多。
	// 返回 false = 夹具本身没跑起来（不代表被测代码有问题）。
	//
	// lieAboutState = true 时**故意报错一个 before 状态**。这是给夹具自己做的
	// 反向验证：一个在 bug 面前也不会失败的测试是没有意义的。开着它跑，调试层必须
	// 报错；不报错就说明这个夹具根本没在验证什么，得先修夹具。
	bool SelfTestPreEvaluate(
		ID3D12Device* device, ID3D12CommandQueue* queue,
		bool lieAboutState = false) noexcept;

	bool IsInstalled() const noexcept { return _installed; }

	// Call from an ordinary worker after Install, never Present/LoadLibrary or
	// DllMain. Internally throttled to once a second; teardown stops discovery.
	// Retained module references/trampolines remain valid until process exit.
	void PollKnownUpscalerExports() noexcept;

	// 最近旁听到的一帧。返回的 frameSeq 为 0 表示还没抓到过。
	NgxEavesdropFrame LatestFrame() const noexcept;

	// 旁听到过多少帧。0 = 一次都没有（链条没接上，或者游戏没在跑 DLSS）。
	uint64_t EvaluateSeen() const noexcept;

	// 距离旁听抓到第一帧过了多少毫秒。0 = 还没抓到第一帧。
	// 用于"开着 NR 启动游戏"时把 NR 初始化推迟到游戏 DLSS 稳定之后 —— 鬼武者从启动
	// 到能加载游戏的主菜单阶段约 20~30 秒，这期间 SR 输出是黑的（错误画面），过早
	// 初始化 NR 会拿黑画面积累时域历史，进加载场景时重投影越界 → DEVICE_HUNG。
	uint64_t MsSinceFirstEvaluate() const noexcept;

    // Conservative guard: at least one live/in-creation chain has enough
    // buffers AND an FG module is present. Module preload alone is insufficient.
    bool FrameGenerationActive(uint64_t windowMs) const noexcept;
    bool FrameGenerationModuleLoaded() const noexcept;
	uint32_t ModulesPatched() const noexcept {
		return _modulesPatched.load(std::memory_order_relaxed);
	}
	// 游戏解析过多少个 NVSDK_NGX_* 函数。这个数比上面那个更早变非零，
	// 所以能区分"链条没接上"和"接上了但游戏还没开始 evaluate"。
	// 真正挂上 EvaluateFeature 的钩子数。**0 = 这一局什么都不会发生。**
	//
	// 必须能被状态块读到：注入太晚时（游戏已经把 NGX 的函数指针取走了）我们既不报错
	// 也不工作，UI 上一个原因都不显示，用户看到的就是"工具没效果"。
	// 实测踩过一次：同一个游戏，注入时进程 98 个模块 -> 接上 5 个钩子；
	// 125 个模块 -> 一个都没接上，而两次的日志除了这一行之外几乎一样。
	uint32_t AttachedEvaluateHooks() const noexcept {
		return _attachedHooks.load(std::memory_order_relaxed);
	}
	// 由自由函数（GetProcAddress 的包装）调用 —— 那里在 Get() 声明之前，
	// 所以走这个 setter，而不是直接摸私有成员。
	void NoteHookAttached() noexcept {
		_attachedHooks.fetch_add(1, std::memory_order_relaxed);
	}
	uint32_t NgxLookups() const noexcept {
		return _ngxLookups.load(std::memory_order_relaxed);
	}

	// 给内部实现回调用的，不是给外面调的
	void NoteModulePatched() noexcept;
	void NoteNgxLookup(const char* name) noexcept;
	// FG 检测的 BufferCount 阈值（core.cpp 读配置后设置，默认 4）。
	void SetFgBufferCountThreshold(uint32_t t) noexcept {
		_fgBufferCountThreshold.store(t, std::memory_order_relaxed);
        FrameGenSwapChains::Get().SetThreshold(t);
	}
	uint32_t FgBufferCountThreshold() const noexcept {
		return _fgBufferCountThreshold.load(std::memory_order_relaxed);
	}
	// 换链（进场景 / 切分辨率 / 开关独占全屏）时触发连续 N 帧 reset。output 指针变
	// 和 swapchain 重建都调这个。连续多帧 reset 是为了彻底清空 DLSSNR 内部的时域历史
	// （单次 reset 不够，DLSSNR 内部可能缓存了多个时域缓冲）。
	void NoteResetFrames() noexcept {
		_resetFrames.store(RESET_FRAMES_ON_REBUILD, std::memory_order_release);
	}
	// swapchain 重建（CreateSwapChain / ResizeBuffers）那一刻调用。进场景不重建
	// swapchain（复用），所以进场景的换链由 output 指针变检测；这里只覆盖"真的重建
	// swapchain"的情况（切分辨率 / 开关独占全屏）。
	void NoteSwapChainRebuilt() noexcept {
		NoteResetFrames();
	}
	// 每帧调用：若还有剩余 reset 帧，消耗一帧并返回 true（这一帧强制 reset DLSSNR）。
	bool TakeResetFrame() noexcept {
		int v = _resetFrames.load(std::memory_order_acquire);
		while (v > 0) {
			if (_resetFrames.compare_exchange_weak(v, v - 1,
				std::memory_order_acq_rel)) {
				return true;
			}
		}
		return false;
	}

	// 深度缓冲爆发（进场景/换场景，短时间内大量新深度缓冲）→ 暂停 NR 一段时间 + reset。
	// 用时间基准（不是帧数）：进场景的深度缓冲重建爆发实测持续 ~0.75 秒（30 个候选），
	// 固定 30 帧（~0.18 秒）覆盖不住爆发期，NR 在爆发还没结束时就恢复 → 读到过渡态深度
	// → DLSSNR 时域重投影越界 → DEVICE_HUNG。时间基准能覆盖整个爆发期。
	void NoteDepthChanged() noexcept {
		// Evaluate borrows the confirmed SR call's own guides. Churn in the
		// heuristic depth candidate table does not invalidate those resources.
		if (EvaluateMode()) return;
		NoteResetFrames();
		_pauseUntil.store(GetTickCount64() + DEPTH_CHANGE_PAUSE_MS,
			std::memory_order_release);
	}
	// 暂停期还没过（进场景深度重建中）→ NR 不处理。
	bool PauseActive() const noexcept {
		const uint64_t until = _pauseUntil.load(std::memory_order_acquire);
		return until && GetTickCount64() < until;
	}

	static constexpr int RESET_FRAMES_ON_REBUILD = 1;
	static constexpr uint64_t DEPTH_CHANGE_PAUSE_MS = 1000;   // 进场景后暂停 1 秒

private:
	std::atomic<bool> _evaluateMode{ false };
	NgxEavesdrop() = default;

	bool _installed = false;
	std::atomic<uint32_t> _modulesPatched{ 0 };
	std::atomic<uint32_t> _ngxLookups{ 0 };
	std::atomic<uint32_t> _attachedHooks{ 0 };
	// 换链暂停到这一刻（GetTickCount64 毫秒）。此前旁听不拷深度/矢量。
	std::atomic<uint64_t> _capturePauseUntil{ 0 };
	// FG 检测阈值（默认 4，core.cpp 读配置后覆盖）。
	std::atomic<uint32_t> _fgBufferCountThreshold{ 4 };
	// 换链后剩余要连续 reset 的帧数（见 NoteResetFrames / TakeResetFrame）。
	std::atomic<int> _resetFrames{ 0 };
	// 深度重建后暂停 NR 到这一刻（GetTickCount64 毫秒，见 NoteDepthChanged / PauseActive）。
	std::atomic<uint64_t> _pauseUntil{ 0 };
	// 旁听抓到第一帧的时刻（GetTickCount64 毫秒，0 = 还没抓到）。见 MsSinceFirstEvaluate。
	std::atomic<uint64_t> _firstEvaluateAtMs{ 0 };
};

}  // namespace DXL
