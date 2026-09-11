#pragma once

// 卡画面的事后取证工具。
//
// 起因：早注入 + 游戏原生 DLSS，玩家在游戏里改 DLSS 设置时画面会卡住。这类问题
// 最难的地方是**卡住的时候我们什么都不知道** —— 日志停在最后一条正常输出上，没有
// 崩溃、没有异常、没有堆栈。于是先做能说话的仪器，再谈修。
//
// 三样东西：
//   1. 面包屑环 —— present 路径每一步落一个标记（时间/阶段/线程/一个附加数值）。
//      热路径成本是一次 relaxed 的 fetch_add 加几个 store，可以常开。
//   2. 独立看门狗线程 —— 每 200ms 看 present 计数有没有前进。停住超过阈值就转储。
//   3. 转储 —— 倒出最近的面包屑 + 进程内所有线程的 RIP（解析成 模块+偏移）
//      + GetDeviceRemovedReason。
//
// 它要回答的**唯一一个问题**是：卡住的时候，控制权在谁手里？
//   · 最后一步是 PresentOriginal  → 卡在游戏原本的 Present 里，不是我们
//   · 最后一步是 PresentExit      → 我们已经返回了，游戏自己不再 present
//   · 最后一步是我们的某个阶段     → 就是我们，而且直接指出是哪一步
// 在拿到这个答案之前，任何"修复"都只是猜。

#include <windows.h>
#include <atomic>
#include <cstdint>

struct ID3D12Device;

namespace DXL {

// present 路径上的阶段。顺序按实际执行顺序排，方便读日志。
enum class Stage : uint16_t {
	None = 0,
	PresentEnter,
	ReloadSettings,
	DepthInstall,
	FiltersEnter,
	NgxCoreInit,
	UpscalerInit,
	NrInit,
	GetBackBuffer,
	NrPrepare,
	NrClaimSlot,
	NrRecord,
	NrEvaluate,
	NrSubmit,
	SrPrepare,
	SrClaimSlot,
	SrRecord,
	SrEvaluate,
	SrSubmit,
	FiltersExit,
	DepthEndFrame,
	PresentOriginal,     // 控制权已经交给游戏原本的 Present
	PresentExit,
	// evaluate 点那条路的阶段。
	//
	// **这几个是补上一个真实的盲点。** 上一次真崩溃时，帧环里从头到尾只有 present
	// 路径的标记，而问题就出在 evaluate 这条路上 —— 转储告诉我"我们已经从 Present
	// 返回了"，可那句话对这条路毫无意义：它跑在游戏的**渲染线程**上，和 present
	// 完全是两回事。于是"崩的时候我们在不在里面"这个最基本的问题都答不上来。
	NrEvalEnter,
	NrEvalEncode,
	NrEvalNgx,
	NrEvalDecode,
	NrEvalWriteBack,
	NrEvalExit,
	// 非 present 线程也会落的标记，用来还原"改设置"这一刻发生了什么
	SwapChainCreated,
	ResizeBuffers,
	SettingsReload,
	CommandThread,
};

const wchar_t* StageName(Stage stage) noexcept;

class FreezeWatchdog {
public:
	static FreezeWatchdog& Get() noexcept;

	// 阈值单位毫秒。0 = 不启动（配置里可以关掉）。
	void Start(uint32_t stallMs) noexcept;
	void Stop() noexcept;
	bool IsRunning() const noexcept { return _thread != nullptr; }

	// GetDeviceRemovedReason 要用。设备丢了是"画面定住但进程还活着"最常见的成因，
	// 必须在转储里给出来。
	void SetDevice(ID3D12Device* device) noexcept { _device = device; }

	// 游戏的主窗口。用来把"游戏在退出"和"游戏卡住了"分开 —— 窗口销毁之后不再
	// present 是完全正常的，那种情况下转储没有信息量，也不该计进卡顿次数。
	void SetWatchedWindow(HWND window) noexcept {
		_window.store(window, std::memory_order_relaxed);
	}

	// DRED 的开关有没有真的打开（只能在游戏建设备之前打开，晚注入就来不及）。
	// 转储时要用它区分两种"面包屑是空的"：**没开**，还是**开了但这次不是命令出错**。
	// 两者的下一步完全不同，混成一句话说等于没说。
	void SetDredEnabled(bool enabled) noexcept { _dredEnabled = enabled; }

	// 每帧都会落的标记。写进"帧环"，只保留最近几帧。
	void Mark(Stage stage, uint64_t detail = 0) noexcept;
	// 稀有事件（重建 swapchain、ResizeBuffers、改设置…）。写进独立的"事件环"。
	//
	// 为什么必须分开两个环：帧环一帧就要写十来颗面包屑，几百格转眼就被冲干净，
	// 而"玩家改设置那一刻发生了什么"恰恰是**几秒前**的稀有事件 —— 混在一个环里
	// 它一定会被冲掉。实测过一次才发现：转储里 192 步全是同 2 秒内的重复帧。
	void MarkEvent(Stage stage, uint64_t detail = 0) noexcept;
	void PresentEnter() noexcept;
	void PresentExit() noexcept;

	// 卡住过几次。发布到状态块里，UI 上能看见。
	uint32_t StallCount() const noexcept {
		return _stallCount.load(std::memory_order_relaxed);
	}
	// 最后一次卡住时停在哪一步，给 UI 直接显示成一句话
	Stage LastStallStage() const noexcept {
		return _lastStallStage.load(std::memory_order_relaxed);
	}

private:
	FreezeWatchdog() noexcept;
	~FreezeWatchdog();

	static DWORD WINAPI ThreadMain(void* self) noexcept;
	void Run() noexcept;
	void Dump(uint32_t stalledMs, uint64_t presentSeq) noexcept;
	void DumpThreads() noexcept;

	struct Crumb {
		int64_t qpc = 0;
		uint64_t detail = 0;
		uint32_t threadId = 0;
		Stage stage = Stage::None;
	};

	// 环形缓冲。转储时可能读到正在被覆盖的一格 —— 诊断工具，能接受。
	template <uint32_t N>
	struct Ring {
		Crumb slots[N]{};
		std::atomic<uint64_t> next{ 0 };

		void Push(Stage stage, uint64_t detail) noexcept {
			const uint64_t index = next.fetch_add(1, std::memory_order_relaxed);
			Crumb& crumb = slots[index % N];
			crumb.qpc = 0;   // 先清零：万一转储正好读到半写状态，0 比旧时间戳好认
			crumb.detail = detail;
			crumb.threadId = GetCurrentThreadId();
			crumb.stage = stage;
			QueryPerformanceCounter(reinterpret_cast<LARGE_INTEGER*>(&crumb.qpc));
		}
	};

	void DumpRing(const Crumb* slots, uint32_t capacity, uint64_t next,
		const wchar_t* title) noexcept;
	void DumpDred() noexcept;

	// 帧环：够看最近 8 帧左右的完整形状
	Ring<96> _frames;
	// 事件环：整个会话的稀有事件都在里面，不会被帧噪声冲掉
	Ring<64> _events;

	std::atomic<uint64_t> _presentSeq{ 0 };
	std::atomic<bool> _inPresent{ false };
	// 只用 IsWindow 读，不做任何 GUI 调用 —— 看门狗线程不该碰游戏的窗口
	std::atomic<HWND> _window{ nullptr };
	std::atomic<int64_t> _presentEnterQpc{ 0 };
	std::atomic<uint32_t> _presentThreadId{ 0 };

	std::atomic<uint32_t> _stallCount{ 0 };
	std::atomic<Stage> _lastStallStage{ Stage::None };

	ID3D12Device* _device = nullptr;
	// 设备丢失只报一次。它一旦丢了就永远是丢的状态，不挡住会每 200ms 刷一份转储。
	bool _deviceLossReported = false;
	bool _dredEnabled = false;
	int64_t _qpcFreq = 1;
	uint32_t _stallMs = 3000;
	HANDLE _thread = nullptr;
	HANDLE _stopEvent = nullptr;
	DWORD _watchdogThreadId = 0;
};

}  // namespace DXL

// 热路径上用宏，改成不编译诊断代码时只要改这一处。
// 每帧都会走到的用 D5_STAGE，一次性/稀有的用 D5_EVENT —— 别混，混了事件环就白做了。
#define D5_STAGE(stage) \
	::DXL::FreezeWatchdog::Get().Mark(::DXL::Stage::stage)
#define D5_STAGE_D(stage, detail) \
	::DXL::FreezeWatchdog::Get().Mark(::DXL::Stage::stage, (detail))
#define D5_EVENT(stage) \
	::DXL::FreezeWatchdog::Get().MarkEvent(::DXL::Stage::stage)
#define D5_EVENT_D(stage, detail) \
	::DXL::FreezeWatchdog::Get().MarkEvent(::DXL::Stage::stage, (detail))
