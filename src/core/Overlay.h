// 游戏内可交互浮层。
//
// 快捷键打开，再按一次 / ESC / 右上角的 X 关闭。打开期间**吞掉游戏的键鼠输入** ——
// 不吞的话拖滑条时角色会跟着动，根本没法用。
//
// 三块：
//   1. 布局 + 绘制 —— OverlayRaster.h（纯 CPU，能独立测，见 tests/test-overlay-raster.cpp）
//   2. 贴到画面上 —— GpuPanel（CPU 画好 -> 转 backbuffer 格式 -> CopyTextureRegion，
//      不读背景所以不需要 UAV）
//   3. 输入 —— 这个文件。**挂游戏窗口的 WndProc**，低级钩子只做兜底。
//
// **为什么主路是 WndProc 而不是低级钩子（换过一次，原因写在这）：**
//
// 第一版用 `WH_KEYBOARD_LL` / `WH_MOUSE_LL`，理由是"游戏用 raw input，WndProc 收不全
// 也吞不掉"。实测在街霸 6（RE Engine）上**快捷键根本没反应**，而钩子明明装上了。
// 原因是低级钩子拿不到"这条输入是给谁的"—— 我们只能拿 `GetForegroundWindow()` 去和
// swapchain 报的 HWND 比，而这两个在很多游戏里就不是同一个窗口（无边框全屏的外壳
// 窗口、引擎另开的输入窗口、改分辨率时窗口重建）。日志里的实证：
//     前台 0x1FFF543A，游戏 0x091678FC —— 忽略
// 两个参考实现都不走低级钩子：
//   · REFramework（praydog，RE Engine 专用，也就是街霸 6 同引擎）——
//     `GetWindowLongPtr(m_wnd, GWLP_WNDPROC)` 挂窗口过程，消息喂给
//     `ImGui_ImplWin32_WndProcHandler`，要吞就**不往下转**（`return false`），
//     并且定期 `CallWindowProc(proc, m_wnd, WM_NULL, 0, 0)` 自检钩子还在不在。
//   · ReShade —— hook 消息泵（`input.cpp` 里的 `HookGetMessage`）和 raw input API。
//
// 挂 WndProc 之后"谁有焦点"这个问题自动消失：**消息能到这个窗口，就说明玩家在
// 这个窗口里**。也没有 `LowLevelHooksTimeout`（回调超 300ms 被系统静默摘钩子）
// 和"游戏在后台时全系统吃掉这个组合键"的风险。
//
// 代价是两条，都处理了 / 都记着：
//   · **游戏会自己重设 WndProc** —— 每帧自检，被顶掉就重挂（REFramework 同样如此）。
//   · **挡住消息 ≠ 挡住输入**：RIDEV_NOLEGACY 的游戏压根没有 WM_KEYDOWN，只有
//     WM_INPUT，所以 WM_INPUT 里也要认快捷键、开着时也要吞掉它（吞的时候仍要
//     交给 DefWindowProc，那是 raw input 要求的清理）。**用 DirectInput/XInput
//     轮询或者直接读 GetAsyncKeyState 的游戏我们仍然挡不住** —— ReShade 是靠额外
//     hook 那些 API 解决的，我们还没做。
//
// ⚠️ **吞输入是这个项目里最危险的一个功能** —— 写错了用户的鼠标键盘会失灵，
// 而他正在全屏游戏里，未必能切出去。所以下面几条是硬约束，改这个文件前先读：
//   · 只有"浮层开着 **且** 前台窗口属于这个进程"才吞，两个条件缺一不可
//   · 前台一丢就自动关闭浮层并停止吞
//   · Alt+Tab / Win 键永远放行
//   · WndProc 里只记状态，不做耗时的事 —— 那是游戏的消息线程，卡住它等于卡住游戏
//   · core 卸载时必须把 WndProc 还回去、摘掉钩子

#pragma once

#include <d3d12.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

#include "GpuPanel.h"
#include "GpuPanel11.h"
#include "OverlayRaster.h"

namespace DXL {

// 浮层改了某个设置时回调给 core。core 负责写进 g_state.nrSettings。
using OverlayApplyFn = void (*)(OverlaySetting setting, float value);

class Overlay {
public:
	bool Initialize(ID3D12Device* device, DXGI_FORMAT backbufferFormat,
		HWND gameWindow, OverlayApplyFn apply) noexcept;
	// D3D11 后端（古墓丽影 DX11 那类）。输入/布局/光栅化全套共用，
	// 只有贴图层换 GpuPanel11。哪个后端先 Initialize 哪个生效。
	bool Initialize11(ID3D11Device* device, ID3D11DeviceContext* context,
		DXGI_FORMAT backbufferFormat, HWND gameWindow, OverlayApplyFn apply) noexcept;
	void Release() noexcept;

	// 快捷键。mods 的位：1=Alt 2=Ctrl 4=Shift。0 的话用默认 Alt+0。
	// （字母键容易撞游戏的物品栏/地图，默认用数字键。）
	void SetHotkey(uint32_t vk, uint32_t mods) noexcept;
	// 界面语言（0 中 / 1 日 / 2 英），跟着工具界面的选择走。见 OverlayRaster.h 的 OT。
	void SetLang(int lang) noexcept;

	// **每帧都要喂。** 游戏的窗口不是一成不变的：改分辨率/改显示模式时它可能
	// 重建 swapchain 甚至换 HWND。挂 WndProc 的那一路靠它知道该挂到哪个窗口上；
	// 只在 Initialize 时抄一次的后果是窗口一换，快捷键就永远不响应了。
	void SetGameWindow(HWND window) noexcept {
		if (window) _gameWindow.store(window, std::memory_order_release);
	}

	bool IsOpen() const noexcept { return _open.load(std::memory_order_acquire); }

	// 每帧从 core 灌进来的只读状态（耗时、提示文字、当前参数值）
	void SetTiming(float recentMs, float worstMs, bool hasSamples) noexcept;
	void SetValues(const float (&values)[OVERLAY_SETTING_COUNT]) noexcept;
	void SetInfo(std::vector<OverlayInfoLine> info) noexcept;
	// 状态区（分辨率/矢量·深度来源/帧数）—— 耗时下面，永远显示（见 OverlayState::status）
	void SetStatus(std::vector<OverlayInfoLine> status) noexcept;

	// 在 present 线程上跑：处理累积的输入、必要时重画、录一次拷贝。
	// 调用方负责先把 target 转成 COPY_DEST。
	void Record(ID3D12GraphicsCommandList* list, ID3D12Resource* target,
		uint32_t targetWidth, uint32_t targetHeight) noexcept;

	// D3D11 版：immediate context 串行、无需 barrier，拷贝即时执行。
	void Record11(ID3D11DeviceContext* context, ID3D11Resource* target,
		uint32_t targetWidth, uint32_t targetHeight) noexcept;

private:
	// 主路：挂在游戏窗口上的窗口过程。WndProc 是外壳（重入守卫，见 .cpp），
	// 真正的处理在 WndProcInner 里。
	static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
	static LRESULT CALLBACK WndProcInner(HWND hwnd, UINT msg, WPARAM w, LPARAM l);
	// 兜底：WndProc 挂不上时才用的低级钩子
	static LRESULT CALLBACK KeyboardProc(int code, WPARAM wParam, LPARAM lParam);
	static LRESULT CALLBACK MouseProc(int code, WPARAM wParam, LPARAM lParam);
	static DWORD WINAPI HookThread(LPVOID param);

	// 每帧在 present 线程上调：该挂就挂、被顶掉就重挂、换窗口就搬过去。
	void EnsureInputHook() noexcept;
	void RemoveWndProcHook() noexcept;
	// 面板左上角在客户区里的位置（会夹进画面内）。**布局和贴图共用这一个来源。**
	POINT PanelOrigin(uint32_t targetWidth, uint32_t targetHeight) noexcept;
	// 某个客户区坐标在不在面板矩形里。**消息线程用 lParam 版**（ClientInsidePanel）：
	// 消息里的坐标就是这一次点击发生的位置，没有竞态；raw input 没有 lParam
	// 坐标，只能用 GetCursorPos 版（CursorInsidePanel）—— 指针锁定补丁没装上时
	// 游戏还在每帧拽光标，那一版可能判错，所以只用于 raw input 的兜底。
	bool ClientInsidePanel(HWND hwnd, LONG clientX, LONG clientY) noexcept;
	bool CursorInsidePanel(HWND hwnd) noexcept;
	// #84：当前在被钉（虚拟光标）模式吗。传统鼠标消息的路由判位用 ——
	// 被钉模式下 lParam 坐标（真实光标 = 钉点）不可信，按虚拟位置判。
	bool UsingVirtualCursor() const noexcept;
	// 浮层开着时把游戏的 SetCursorPos/ClipCursor 拦成空操作（见 .cpp 顶部的
	// 说明）。present 线程装/卸，开就装、关就卸。
	void InstallPointerLockPatch() noexcept;
	void RemovePointerLockPatch() noexcept;
	// 让系统光标在浮层打开期间露出来 / 还回去。**只能在游戏的消息线程上调。**
	void ForceSystemCursor(bool on) noexcept;
	// #84：虚拟光标模式期间把（钉死在中心的）系统光标多压几层藏起来，
	// 退出时原样弹回。**同样只能在消息线程上调**（ShowCursor 线程绑定）。
	// WndProc 每条消息都会同步一次，present 线程只改 _shared.virtualActive。
	void SyncVirtualCursorVisibility() noexcept;
	// WndProc / 低级钩子共用的一段：按下我们的组合键了，翻转开关。
	// 返回 true = 这条输入要吞掉。
	bool OnHotkeyDown() noexcept;
	// 关闭请求（ESC / X / 失去前台）。可以从任意线程调。
	void RequestClose() noexcept;

	// **`_open` 的所有权在输入线程**（见 Overlay.cpp 里对"游戏卡死时输入解不开"
	// 的说明）。下面这两个只是 present 线程上的善后，不改状态。
	void OnOpened() noexcept;
	void OnClosed() noexcept;
	// present 线程主动关闭（前台丢了 / 光栅化失败）：既落状态也善后。
	void Close() noexcept;
	// 输入线程调用，只判断"要不要吞"，不做别的
	bool ShouldSwallow() const noexcept;
	// present 线程调用：把输入线程记下的输入变成交互
	void ProcessInput(uint32_t targetWidth, uint32_t targetHeight) noexcept;

	GpuPanel _panel;
	GpuPanel11 _panel11;   // D3D11 后端的贴图层（Initialize11 时启用）
	bool _use11 = false;   // Record/Record11 各自判定，不共用状态
	uint8_t* _pixels = nullptr;
	// Initialize / Initialize11 的公共尾巴：_pixels 分配 + 输入挂钩 + 就绪日志。
	// 两个后端只有贴图层不同，其余必须一字不差 —— 抽出来就是防走样。
	bool FinishInitialize() noexcept;
	std::vector<OverlayItem> _items;
	OverlayState _state;
	uint32_t _width = 0;
	uint32_t _height = 0;
	bool _dirty = true;

	// 钩子线程每次回调都读它，present 线程每帧写它 —— 必须是原子的
	std::atomic<HWND> _gameWindow{ nullptr };
	OverlayApplyFn _apply = nullptr;

	// ---- WndProc 主路 ----
	// 我们挂上去的那个窗口，以及它原来的窗口过程。
	// WndProc 在游戏的消息线程上跑，Ensure/Remove 在 present 线程上跑 —— 都要原子。
	std::atomic<HWND> _hookedWindow{ nullptr };
	std::atomic<WNDPROC> _originalProc{ nullptr };
	// **第一次挂这个窗口时存下的游戏真过程。** dinput8 这类库会子类化窗口，
	// 把我们存在 _originalProc 里的换成它的；重入守卫（WndProc 外壳）桥的
	// 是这一份，桥到 _originalProc 的话环就还在（见 .cpp 外壳上的注释）。
	std::atomic<WNDPROC> _firstOriginal{ nullptr };
	bool _firstOriginalUnicode = true;
	// 窗口是 Unicode 还是 ANSI 的。挂的时候用哪个变体，转发时就得用同一个变体，
	// 否则 WM_CHAR 之类带文本的消息会被系统在两套编码之间来回转，游戏收到乱码。
	bool _hookedUnicode = true;
	// WndProc 一次都没挂上时才启用的低级钩子兜底。**两条路只能有一条生效** ——
	// 都开着的话按一下快捷键会被翻转两次（开了又关），看起来就是"没反应"。
	bool _llFallback = false;
	// 诊断用：WndProc 到底有没有收到过键盘消息 / 收到过多少次没匹配上的按键。
	// 这两个数字是区分"没按"、"按了但组合键不对"、"消息压根没来"的唯一办法 ——
	// 上一版只在"vk 和修饰键都对"时才打日志，于是这三种情况在日志里一模一样。
	std::atomic<uint32_t> _keyMessagesSeen{ 0 };
	std::atomic<uint32_t> _hotkeyVkSeen{ 0 };
	// 挂上之后过了多少帧，用来在 30 秒时报一次"输入到底通没通"
	uint32_t _framesSinceHook = 0;

	std::atomic<bool> _open{ false };
	// 上一次热键翻转的时刻。多条输入路（窗口过程 / raw input / 低级钩子）对同一次
	// 物理按键都会调 OnHotkeyDown —— 250ms 防抖窗内只认第一次（见那边的说明）。
	std::atomic<ULONGLONG> _lastHotkeyToggle{ 0 };
	// 钩子线程和 present 线程都读；钩子线程要用它决定吞不吞，所以必须是原子的
	std::atomic<bool> _foreground{ false };

	uint32_t _hotkeyVk = '0';
	uint32_t _hotkeyMods = 1;   // Alt

	// 指针锁定补丁当前装没装。present 线程独占（OnOpened/OnClosed 都是它调）。
	bool _pointerLockPatched = false;

	// ---- #84 虚拟光标 ----
	// 发散判定（present 线程独占，ProcessInput 每帧）。上一帧的真实/虚拟位置
	// 和连续"虚拟动而真实不动"的帧数（被钉的特征）。一致（两者一起动）只清零
	// 不另计 —— 正常游戏永远到不了阈值，被钉游戏到了就切虚拟模式并保持到关浮层
	//（不自动切回：游戏中途停止 recenter 时真实位置会从中心起跳，切回会跳位置）。
	// 增量不靠我们自己注册 raw 鼠标（见 .cpp 顶部：ReShade 原则，鬼武者踩的坑）
	// —— piggyback 游戏自己的注册，挂着的 WndProc 顺手消费 WM_INPUT。
	POINT _pinPrevReal{ 0, 0 };
	POINT _pinPrevVirtual{ 0, 0 };
	int _pinStreak = 0;
	// 虚拟模式期间为藏系统光标多压下去的 ShowCursor(FALSE) 次数
	//（消息线程在 SyncVirtualCursorVisibility 里压/弹）。
	int _virtualHideCalls = 0;

	HANDLE _hookThread = nullptr;
	DWORD _hookThreadId = 0;
	std::atomic<bool> _hookRunning{ false };

	// 钩子线程 -> present 线程。钩子里只写这几个值，别的什么都不做。
	struct Shared {
		POINT cursorScreen{ 0, 0 };
		bool leftDown = false;
		// 上一帧到现在之间按过左键。**脉冲，不是计数**：一次物理点击会同时走
		// WM_INPUT（raw input）和 WM_LBUTTONDOWN（传统消息）两条路 —— 计数的话
		// 两次 +1 跨帧落下，开关会被翻转两次（看起来"点了没反应"）；
		// 用状态边沿的话快速点击（按下和抬起都落在两帧之间）会整个丢掉。
		// 置 true 是幂等的，present 线程消费时清掉。
		bool leftPressPulse = false;
		int wheelDelta = 0;
		// ---- #84 虚拟光标（消息线程写，present 线程快照读）----
		// raw 鼠标增量累计出来的位置（**客户区坐标**）。OnOpened 时用当时的
		// 真实光标位置播种 —— 之后每条 WM_INPUT 增量都加在它上面。
		// 未激活（virtualActive=false）时它仍然跟着增量走，只是没人用它；
		// 一旦判定被钉（见 .cpp 里 ProcessInput 的发散判定）就切成唯一位置来源。
		POINT virtualClient{ 0, 0 };
		bool virtualActive = false;     // 被钉模式：命中/点击/画笔全走虚拟位置
		bool virtualAbsolute = false;   // 设备是绝对坐标（RDP/数位板）：增量无意义，本次打开永久停用
	};
	Shared _shared;
	mutable std::mutex _sharedMutex;
	// present 线程上一次看到的开关状态，用来检测跳变
	bool _wasOpen = false;

	// 拖动中的滑条。-1 = 没在拖。
	int _draggingItem = -1;
	int _dragGrabX = 0;           // 按下时的本地 x（相对位移那条路用）
	float _dragStartValue = 0.0f; // 按下时的值
	bool _dragInTrough = false;   // 按在槽上（绝对）还是标签上（相对）

	// ---- 面板位置（present 线程独占）----
	// 距画面边缘的留白。默认位置靠右上角，不再居中 —— 居中正好挡住画面主体。
	static constexpr int PANEL_MARGIN = 32;
	int _panelX = 0;
	int _panelY = 0;
	bool _panelPlaced = false;      // 还没定过位 = 用默认位置
	bool _dragging = false;         // 正在拖标题栏
	POINT _dragGrab{ 0, 0 };        // 按下时光标相对面板原点的偏移

	// ---- 系统光标（消息线程独占）----
	// 面板是一块不透明位图，我们只能在它里面画光标；光标一移出面板玩家就找不着了。
	// 所以打开时把**系统光标**顶出来，关闭时原样还回去。
	bool _cursorForced = false;
	int _cursorShowCalls = 0;       // ShowCursor(TRUE) 调了几次，关的时候要减回去

	// 上一次重画的时间，用来限制重画频率（GDI 光栅化是 present 线程上的 CPU 开销）
	uint64_t _lastRasterTick = 0;
};

// 全局单例。低级钩子的回调是自由函数，必须能拿到实例。
Overlay& GetOverlay() noexcept;

}  // namespace DXL
