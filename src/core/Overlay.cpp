#include "Overlay.h"

#include <windows.h>
#include <windowsx.h>
#include <algorithm>
#include <cstdio>
#include "IatPatch.h"
#include "HookTeardown.h"
#include "../common/Log.h"

#pragma comment(lib, "gdi32.lib")

namespace DXL {
namespace {

HHOOK g_keyboardHook = nullptr;
HHOOK g_mouseHook = nullptr;

/* ---- 指针锁定拦截 ----
 *
 * 3D 游戏在"玩家操控状态"下每帧做两件事：`SetCursorPos` 把光标拽回画面中心
 * （视角转动的实现），`ClipCursor` 把光标锁死在窗口里。浮层开着的时候这两条
 * 都是毒：前者让 `GetCursorPos` 读到"中心 <-> 真位置"高频横跳（拖滑块时值
 * 疯狂往一个方向跑、拖面板拖不动 —— 用户实测），后者让光标物理上出不了
 * 游戏窗口（点击任何别的东西都没反应）。ReShade 不受害，因为它根本不读
 * 系统光标位置（用 raw input 的相对位移驱动自己的光标）。
 *
 * 修法：**浮层开着的时候把游戏 exe 的这两个调用拦成空操作**（IAT 补丁，
 * 和旁听 DLSS 用的同一个机制）。关浮层立刻还原 —— 游戏自己的视角转动
 * 一帧都不耽误。
 *
 * ---- #84 虚拟光标（IAT 补丁拦不住的钉法）----
 *
 * 原神实测：补丁明明装上了（日志 SetCursorPos=1 ClipCursor=1 ShowCursor=1，
 * 装在主 exe 上），光标照样被每帧钉死在画面中心 —— 拽光标的调用不走主 exe
 * 的 IAT（从别的模块发出 / 动态解析 / 反作弊驱动层，用户态补丁够不着）。
 *
 * 兜底：**raw 鼠标增量驱动的虚拟光标**。WM_INPUT 的 RAWMOUSE.lLastX/Y
 * 是硬件增量，投递不受任何 SetCursorPos 影响。
 *
 * **增量从哪来：piggyback 游戏自己的 raw 注册，绝不自己注册**（ReShade 的
 * 原则，鬼武者那局踩的坑）。RegisterRawInputDevices 是进程级单例、同
 * usagePage/usage 后调者赢 —— 我们注册 (1,2) 会把游戏的顶掉：鬼武者
 * （UE5）自己的输入栈从此收不到 raw 事件，present 线程跟着冻死
 * （实测：注册后 1 秒卡顿转储）。而会钉光标的游戏（要转视角）自己必定
 * 注册过 raw 鼠标 —— WM_INPUT 照投给游戏窗口，我们挂的 WndProc 顺手
 * 消费就行，一个字节都不用碰注册表。没注册 raw 的钉法（罕见）：虚拟
 * 光标不激活，退回真实光标路径，浮层照常可用。
 *
 * 每帧对比：**虚拟在动而真实不动连续 5 帧** = 被钉，切虚拟模式 ——
 * 命中/点击/画笔全走虚拟位置，系统光标藏掉。一致（一起动）只清零：
 * 正常游戏两位置永远同步，到不了阈值，零行为变化。 */
BOOL (WINAPI* g_realSetCursorPos)(int, int) = nullptr;
BOOL (WINAPI* g_realClipCursor)(const RECT*) = nullptr;
int (WINAPI* g_realShowCursor)(BOOL) = nullptr;

// 假装成功。返回值必须像真的：有的游戏会检查返回值决定要不要重试。
BOOL WINAPI HookedSetCursorPos(int, int) noexcept { return TRUE; }
BOOL WINAPI HookedClipCursor(const RECT*) noexcept { return TRUE; }
// ShowCursor 返回的是显示计数（>= 0 = 可见）。恒返回 0 = "一直可见"——
// 游戏循环调 ShowCursor(FALSE) 直到返回负值的写法（很常见）会立刻停住，
// 我们在 ForceSystemCursor 里推的计数也不会再被它压回去。
int WINAPI HookedShowCursor(BOOL) noexcept { return 0; }

// 重画上限。GDI 光栅化 480x700 大约一两毫秒，跑在 present 线程上 ——
// 拖滑条时每帧都重画会实打实吃掉帧时间。30Hz 手感上已经跟手了。
constexpr uint64_t MIN_RASTER_INTERVAL_MS = 33;

bool ModsHeld(uint32_t mods) noexcept {
	const bool alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
	const bool ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
	const bool shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
	return alt == ((mods & 1) != 0) && ctrl == ((mods & 2) != 0) &&
		shift == ((mods & 4) != 0);
}

// 同上，但用 `GetKeyState` 而不是 `GetAsyncKeyState`。
//
// **在 WndProc 里必须用这个。** GetKeyState 返回的是"处理当前这条消息的那一刻"
// 的键盘状态（和消息队列同步），GetAsyncKeyState 返回的是此刻的物理状态。
// 消息可能已经在队列里排了几毫秒，用异步状态判修饰键会在快速连按时判错。
bool ModsHeldSync(uint32_t mods, bool altFromMessage) noexcept {
	// Alt 优先用消息自带的 context 位：WM_SYSKEYDOWN 的 lParam 第 29 位就是
	// "按这个键的时候 Alt 是不是按下的"，比任何一次额外查询都准。
	const bool alt = altFromMessage || (GetKeyState(VK_MENU) & 0x8000) != 0;
	const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
	const bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
	return alt == ((mods & 1) != 0) && ctrl == ((mods & 2) != 0) &&
		shift == ((mods & 4) != 0);
}

// 一条键盘消息是不是"按下"
bool IsKeyDownMessage(UINT msg) noexcept {
	return msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
}

// 开着的时候要吞掉的键盘类消息
bool IsKeyboardMessage(UINT msg) noexcept {
	return msg == WM_KEYDOWN || msg == WM_KEYUP ||
		msg == WM_SYSKEYDOWN || msg == WM_SYSKEYUP ||
		msg == WM_CHAR || msg == WM_SYSCHAR || msg == WM_DEADCHAR ||
		msg == WM_UNICHAR || msg == WM_IME_CHAR;
}

// 开着的时候要吞掉的鼠标类消息。**只管客户区那一段。**
// 非客户区（WM_NC*，标题栏/边框/关闭按钮）故意不吞：浮层画在客户区里，
// 而吞掉 WM_NCLBUTTONDOWN 会让玩家连游戏窗口的关闭按钮都点不了。
bool IsMouseMessage(UINT msg) noexcept {
	return msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST;
}

// #84：POINT 没有比较运算符（C2737 那次的坑），发散判定里自己比。
bool SamePoint(POINT a, POINT b) noexcept {
	return a.x == b.x && a.y == b.y;
}

// "玩家现在正在玩这个游戏吗"。
//
// **按进程判，不按 HWND 判。** 原来是 `GetForegroundWindow() == 游戏窗口`，
// 那个游戏窗口是我们从 swapchain 的 desc 抄来的 HWND —— 而它和真正拿到焦点的
// 那个窗口经常不是同一个：无边框全屏的游戏常有一层外壳窗口、有的引擎另开一个
// 输入窗口、IME/启动器窗口也会插一脚，改分辨率时窗口还可能整个重建。
// 一旦不相等，快捷键就永远不响应（画面上完全看不出原因，只有日志里一行）。
//
// 我们真正要的条件是"前台窗口属于这个进程"—— 那正是"玩家在玩这个游戏"的意思，
// 而且同样安全：别的程序在前台时我们绝不吞输入。
bool ForegroundIsThisProcess() noexcept {
	const HWND foreground = GetForegroundWindow();
	if (!foreground) return false;
	DWORD pid = 0;
	GetWindowThreadProcessId(foreground, &pid);
	return pid == GetCurrentProcessId();
}

}  // namespace

Overlay& GetOverlay() noexcept {
	static Overlay instance;
	return instance;
}

/* ---------------- 输入：共用的一小块 ---------------- */

bool Overlay::ShouldSwallow() const noexcept {
	// **两个条件缺一不可。** 只看"浮层开着"的话，玩家 Alt+Tab 出去之后
	// 整个系统的鼠标键盘就废了 —— 而他多半正在全屏游戏里，切不回来。
	return _open.load(std::memory_order_acquire) &&
		_foreground.load(std::memory_order_acquire);
}

// 按下我们的组合键了。返回 true = 这条输入要吞掉。
//
// **在输入线程里直接翻转，不要只排队等 present 线程。**
// 关闭这件事必须在这里落地：游戏卡死时 present 不再跑，排队的关闭请求永远不会
// 被消费，而输入还被我们吞着 —— 那是这个功能能造成的最坏后果。既然关要在这里做，
// 开也一起做，状态才只有一个主人。（中间版本只在"已开"时置 false，结果关着时
// 没人置 true，浮层再也打不开 —— 状态所有权分裂就会这样。）
bool Overlay::OnHotkeyDown() noexcept {
	// **一次物理按键可能走多条输入路**（窗口过程的键盘消息 + raw input 的
	// WM_INPUT + 低级钩子兜底），每条路都会调到这里 —— 各翻转一次就是
	// "按一下开了瞬间又关上"（DS2 实测：日志里 快捷键->开 后 1ms 紧跟 ->关，
	// 按七次、七对开/关，浮层永远唤不出）。鼠标的 leftPressPulse 当年就是
	// 同一个坑（LESSONS 7.5.12），热键翻转当时没做幂等，这里补上。
	//
	// 时间防抖：人不可能 250ms 内真按两次这个组合键 —— 两条输入路对同一次
	// 按键的间隔是毫秒级，防抖窗内只认第一次。简单 compare_exchange 会有
	// ABA 的缝隙，这里循环到"要么确认在窗内（忽略），要么成功占位"为止。
	const ULONGLONG now = GetTickCount64();
	ULONGLONG last = _lastHotkeyToggle.load(std::memory_order_acquire);
	while (true) {
		if (now - last < 250) {
			D5_LOG_INFO(L"游戏内浮层：同一次按键走了第二条输入路"
				L"（距上次翻转 %llu ms）—— 忽略，不翻转", now - last);
			return true;
		}
		if (_lastHotkeyToggle.compare_exchange_strong(last, now,
				std::memory_order_acq_rel)) {
			break;
		}   // CAS 失败：last 被刷新成别人刚写的值，拿它重判一次窗
	}

	const bool wasOpen = _open.load(std::memory_order_acquire);
	_open.store(!wasOpen, std::memory_order_release);
	D5_LOG_INFO(L"游戏内浮层：快捷键 -> %s", wasOpen ? L"关" : L"开");
	return true;
}

void Overlay::RequestClose() noexcept {
	_open.store(false, std::memory_order_release);
}

/* ---------------- 位置 ---------------- */

// 面板左上角在客户区里的位置。**布局和贴图必须用同一个来源** ——
// 以前两处各算一遍居中，加上拖动之后立刻就会对不上（点到的和看到的错位）。
POINT Overlay::PanelOrigin(uint32_t targetWidth, uint32_t targetHeight) noexcept {
	const int width = int(_width ? _width : OVERLAY_WIDTH);
	const int height = int(_height ? _height : OVERLAY_MAX_HEIGHT);
	if (!_panelPlaced) {
		// **默认靠右**，不是居中。居中正好挡住画面主体（用户原话："很挡画面"）。
		// 竖直方向也偏上一点，给底部的血条/技能栏让路。
		_panelX = int(targetWidth) - width - PANEL_MARGIN;
		_panelY = PANEL_MARGIN;
		_panelPlaced = true;
	}
	// 每帧都夹一次：面板高度会随内容变，窗口也会改大小 —— 不夹的话拖出去就找不回来了
	const int maxX = (std::max)(0, int(targetWidth) - width);
	const int maxY = (std::max)(0, int(targetHeight) - height);
	_panelX = (std::min)((std::max)(_panelX, 0), maxX);
	_panelY = (std::min)((std::max)(_panelY, 0), maxY);
	return POINT{ _panelX, _panelY };
}

// 某个客户区坐标在不在面板矩形里。
//
// PanelOrigin 会在这里被消息线程调用 —— 它对 _panelX/_panelY 的写入是
// 幂等的同值写（present 线程算出同一个值），x64 上对齐 int 写不会撕裂，
// 竞态是良性的；不另做一份只读版是为了保住"布局只有一个来源"。
bool Overlay::ClientInsidePanel(HWND hwnd, LONG clientX, LONG clientY) noexcept {
	const uint32_t width = _width ? _width : OVERLAY_WIDTH;
	const uint32_t height = _height ? _height : OVERLAY_MAX_HEIGHT;
	RECT client{};
	if (!GetClientRect(hwnd, &client)) return false;
	const POINT origin = PanelOrigin(
		uint32_t(client.right - client.left), uint32_t(client.bottom - client.top));
	const LONG x = clientX - origin.x;
	const LONG y = clientY - origin.y;
	return x >= 0 && y >= 0 && x < (LONG)width && y < (LONG)height;
}

// #84：当前在被钉（虚拟光标）模式吗（见 Shared.virtualActive 的说明）。
bool Overlay::UsingVirtualCursor() const noexcept {
	std::lock_guard<std::mutex> guard(_sharedMutex);
	return _shared.virtualActive;
}

// raw input 版：没有 lParam 坐标。#84 起两路：被钉模式按**虚拟位置**判
//（raw 增量累计的，客户区坐标，无需换算）；否则问系统现在的光标在哪
//（传统路 —— 此时系统光标就是真实的那只，可信）。
bool Overlay::CursorInsidePanel(HWND hwnd) noexcept {
	bool virtualActive = false;
	POINT pt{ 0, 0 };
	{
		std::lock_guard<std::mutex> guard(_sharedMutex);
		virtualActive = _shared.virtualActive;
		if (virtualActive) pt = _shared.virtualClient;
	}
	if (!virtualActive) {
		if (!GetCursorPos(&pt)) return false;
		if (hwnd) ScreenToClient(hwnd, &pt);
	}
	return ClientInsidePanel(hwnd, pt.x, pt.y);
}

// 指针锁定补丁的装/卸。**只装在主 exe 上** —— recenter/clip 是游戏主循环
// 干的；别的模块（Steam overlay 之类）也调这两个函数的话被拦了反而是干扰。
void Overlay::InstallPointerLockPatch() noexcept {
	if (_pointerLockPatched) return;
	HMODULE exe = GetModuleHandleW(nullptr);
	// previous 拿回真函数存进 g_real*：拦成空操作不是转发，但卸载时
	// 要把 IAT 槽位写回这个原值。
	void* original = nullptr;
	const bool ok1 = Iat::Patch(exe, "SetCursorPos", &HookedSetCursorPos, &original);
	g_realSetCursorPos = reinterpret_cast<BOOL(WINAPI*)(int, int)>(original);
	original = nullptr;
	const bool ok2 = Iat::Patch(exe, "ClipCursor", &HookedClipCursor, &original);
	g_realClipCursor = reinterpret_cast<BOOL(WINAPI*)(const RECT*)>(original);
	original = nullptr;
	const bool ok3 = Iat::Patch(exe, "ShowCursor", &HookedShowCursor, &original);
	g_realShowCursor = reinterpret_cast<int(WINAPI*)(BOOL)>(original);
	// 有一个装上就算成功：有的游戏只调其中几个。一个都没有也别把
	// _pointerLockPatched 置上 —— Remove 会白跑一趟。
	_pointerLockPatched = ok1 || ok2 || ok3;
	D5_LOG_INFO(L"游戏内浮层：指针锁定补丁（SetCursorPos=%d ClipCursor=%d "
		L"ShowCursor=%d）—— 游戏不再把光标拽回中心/锁在窗口里/藏起来",
		int(ok1), int(ok2), int(ok3));
}

void Overlay::RemovePointerLockPatch() noexcept {
	if (!_pointerLockPatched) return;
	// 把真函数写回 IAT 槽位。Patch 遇到"槽位已经是我们"会拒绝（防递归），
	// 所以卸载走 WriteSlot 直接对原值。
	HMODULE exe = GetModuleHandleW(nullptr);
	void* dummy = nullptr;
	if (g_realSetCursorPos) {
		Iat::WriteSlot(Iat::FindSlot(exe, "SetCursorPos"), g_realSetCursorPos, &dummy);
	}
	if (g_realClipCursor) {
		Iat::WriteSlot(Iat::FindSlot(exe, "ClipCursor"), g_realClipCursor, &dummy);
	}
	if (g_realShowCursor) {
		Iat::WriteSlot(Iat::FindSlot(exe, "ShowCursor"), g_realShowCursor, &dummy);
	}
	g_realSetCursorPos = nullptr;
	g_realClipCursor = nullptr;
	g_realShowCursor = nullptr;
	_pointerLockPatched = false;
}

// #84：虚拟模式期间藏系统光标（钉死在中心的那只），退出时弹回。
// **只能在游戏的消息线程上调**（ShowCursor 改的是线程输入队列的计数）——
// WndProc 每条消息同步一次；激活/退出那一下由 ProcessInput PostMessage(WM_NULL)
// 唤醒。压到看不见为止（游戏别的模块可能也压过计数），次数记下来原样弹回。
void Overlay::SyncVirtualCursorVisibility() noexcept {
	bool active = false;
	{
		std::lock_guard<std::mutex> guard(_sharedMutex);
		active = _shared.virtualActive;
	}
	if (active && _virtualHideCalls == 0) {
		int guard = 0;
		while (guard++ < 32) {
			CURSORINFO info{ sizeof(CURSORINFO) };
			if (!GetCursorInfo(&info) || (info.flags & CURSOR_SHOWING) == 0) break;
			ShowCursor(FALSE);
			++_virtualHideCalls;
		}
	} else if (!active && _virtualHideCalls > 0) {
		for (int i = 0; i < _virtualHideCalls; ++i) ShowCursor(TRUE);
		_virtualHideCalls = 0;
	}
}

// 让系统光标在浮层打开期间露出来。
//
// **必须在游戏的消息线程上做**（也就是 WndProc 里）：`ShowCursor` 改的是
// 线程输入队列的显示计数，从 present 线程调不一定作数。
// 计数是累加的 —— 游戏可能调过很多次 `ShowCursor(FALSE)`，所以要一直加到 >= 0，
// 并记下加了多少次，关闭时原样减回去（不还的话游戏自己的光标逻辑就乱了）。
void Overlay::ForceSystemCursor(bool on) noexcept {
	if (on) {
		if (_cursorForced) return;
		int count = ShowCursor(TRUE);
		int guard = 0;
		while (count < 0 && guard++ < 32) count = ShowCursor(TRUE);
		_cursorShowCalls = guard + 1;
		_cursorForced = true;
		// 游戏常把光标夹在窗口里。夹着其实没坏处（浮层就在窗口里），
		// 所以**不动 ClipCursor** —— 少碰一个游戏的全局状态就少一类怪问题。
		D5_LOG_INFO(L"游戏内浮层：已让系统光标显示（ShowCursor 调用 %d 次）",
			_cursorShowCalls);
	} else {
		if (!_cursorForced) return;
		for (int i = 0; i < _cursorShowCalls; ++i) ShowCursor(FALSE);
		_cursorShowCalls = 0;
		_cursorForced = false;
	}
}

/* ---------------- 主路：挂游戏窗口的 WndProc ---------------- */

// 外壳：重入守卫。真正的处理在 WndProcInner 里。
//
// **为什么必须有这个外壳：子类化会成环。** dinput8 这类库会在我们挂上之后
// 子类化同一个窗口 —— 它把"上一个过程"存成**我们**（它子类化时我们恰好在
// 最上面），窗口从此变 ANSI；我们每帧自检发现被顶掉、重挂在它上面，于是：
//     OURS -> CallWindowProc(dinput8 的过程) -> dinput8 处理完把没处理的
//     转发给它存的"上一个" = OURS -> CallWindowProc(dinput8) -> …… 无限递归
// 实测在鬼武者上就是这个环把主线程的栈写穿（0xC0000005，写的目标正好在
// rsp 附近 —— 转储里看得清清楚楚），游戏黑屏十来秒后崩溃。街霸 6 用
// raw input、从不子类化窗口，所以同一版代码在它上面完全正常 —— 这也是
// 这个 bug 藏了好几轮才炸的原因。
//
// 守卫规则：同一条消息流里再次进到我们 = 环的第二跳。这时**什么都不做**，
// 直接桥到第一次挂这个窗口时存下的游戏真过程（_firstOriginal）—— 消息
// 到游戏手里恰好一次，环在这里断开。顺带把"同一条消息被我们处理两次"
// 也挡了：没有守卫的话就算不成环，快捷键也会被翻转两次（开了又关），
// 看起来就是"快捷键没反应"。
LRESULT CALLBACK Overlay::WndProc(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
	Overlay& self = GetOverlay();
	thread_local int depth = 0;
	if (depth > 0) {
		const WNDPROC first = self._firstOriginal.load(std::memory_order_acquire);
		if (first) {
			return self._firstOriginalUnicode
				? CallWindowProcW(first, hwnd, msg, w, l)
				: CallWindowProcA(first, hwnd, msg, w, l);
		}
		// 第一次挂的过程还没存上（不会发生，防呆）：宁可走 DefWindowProc
		// 也不再往 _originalProc 转发 —— 那正是成环的那条边。
		return IsWindowUnicode(hwnd) ? DefWindowProcW(hwnd, msg, w, l)
			: DefWindowProcA(hwnd, msg, w, l);
	}
	++depth;
	const LRESULT result = WndProcInner(hwnd, msg, w, l);
	--depth;
	return result;
}

LRESULT CALLBACK Overlay::WndProcInner(HWND hwnd, UINT msg, WPARAM w, LPARAM l) {
	Overlay& self = GetOverlay();
	const WNDPROC original = self._originalProc.load(std::memory_order_acquire);
	const bool unicode = self._hookedUnicode;

	// 转发给游戏原来的窗口过程。**变体要和挂的时候一致**（见 _hookedUnicode）。
	auto forward = [&]() -> LRESULT {
		if (original) {
			return unicode ? CallWindowProcW(original, hwnd, msg, w, l)
				: CallWindowProcA(original, hwnd, msg, w, l);
		}
		return unicode ? DefWindowProcW(hwnd, msg, w, l)
			: DefWindowProcA(hwnd, msg, w, l);
	};
	// 吞掉：不给游戏，但**系统该做的清理还得做**（raw input 尤其要求这一点）。

	// 窗口要没了：先把过程还回去再放行，否则我们的函数指针会挂在一个死窗口上
	if (msg == WM_NCDESTROY) {
		self.RemoveWndProcHook();
		// **退出清理的最早触发点。** 窗口销毁是"游戏在退出"的第一个信号，
		// 这里把所有 vtable 补丁（present / 深度探测 / 队列 / 工厂）都还原掉，
		// 游戏接下来的 teardown 不会再撞进我们的钩子 —— 街霸6退出崩溃的根治
		// （见 HookTeardown.h 顶部的完整说明）。注意这是单程的：中途换窗口
		// （全屏切换重建窗口）不在此列 —— 那走的是上面的换窗口分支，不是销毁。
		RequestTeardown();
		if (original) {
			return unicode ? CallWindowProcW(original, hwnd, msg, w, l)
				: CallWindowProcA(original, hwnd, msg, w, l);
		}
		return unicode ? DefWindowProcW(hwnd, msg, w, l)
			: DefWindowProcA(hwnd, msg, w, l);
	}

	// 消息能到这里就说明玩家在这个窗口上操作 —— 但 Alt+Tab 之后消息会停，
	// 所以前台标志仍然按进程判一次（便宜，且 ProcessInput 每帧也会再确认）。
	self._foreground.store(ForegroundIsThisProcess(), std::memory_order_release);

	// 失去激活就关：玩家已经走了，再吞下去就是把他的输入锁在一个空窗口上
	if (msg == WM_ACTIVATEAPP && w == FALSE) self.RequestClose();
	if (msg == WM_KILLFOCUS) self.RequestClose();

	const bool open = self._open.load(std::memory_order_acquire);

	// **系统光标的开关只能在这个线程上拨**（ShowCursor 改的是线程输入队列的
	// 计数）。所以放在这里跟着 open 同步 —— 消息线程上有任何一条消息进来就会对齐。
	if (open != self._cursorForced) self.ForceSystemCursor(open);
	// #84：虚拟光标模式藏系统光标的计数也在这个线程上对齐（同一个理由）。
	// present 线程只改 _shared.virtualActive，这里每条消息把它落实成 ShowCursor。
	self.SyncVirtualCursorVisibility();

	/* ---- 键盘 ---- */
	if (IsKeyboardMessage(msg)) {
		self._keyMessagesSeen.fetch_add(1, std::memory_order_relaxed);
		const UINT vk = (UINT)w;
		if (IsKeyDownMessage(msg)) {
			// WM_SYSKEYDOWN 的 lParam 第 29 位 = 按键时 Alt 是否按下
			const bool altFromMessage = (l & (1 << 29)) != 0;
			if (vk == self._hotkeyVk) {
				self._hotkeyVkSeen.fetch_add(1, std::memory_order_relaxed);
				if (ModsHeldSync(self._hotkeyMods, altFromMessage)) {
					self.OnHotkeyDown();
					return 0;
				}
				// **"按到了键但修饰键不对"必须留下线索。**
				// 上一版把这条日志写在"vk 和修饰键都对"的分支里，于是"没按"、
				// "按了但组合键不对"、"消息压根没来"三种情况在日志里完全一样，
				// 街霸 6 那一局就是这么查不下去的。限流打，别刷屏。
				static uint32_t mismatched = 0;
				if (mismatched++ % 20 == 0) {
					D5_LOG_WARN(L"游戏内浮层：收到了 vk=0x%02X（第 %u 次），但修饰键不对 —— "
						L"期望 mods=%u，实际 alt=%d ctrl=%d shift=%d。快捷键不会响应。",
						vk, mismatched, self._hotkeyMods,
						(altFromMessage || (GetKeyState(VK_MENU) & 0x8000)) ? 1 : 0,
						(GetKeyState(VK_CONTROL) & 0x8000) ? 1 : 0,
						(GetKeyState(VK_SHIFT) & 0x8000) ? 1 : 0);
				}
			}
			if (open) {
				// ESC 关掉；Alt+Tab 和 Win 键永远放行 —— 那是玩家的逃生出口
				if (vk == VK_ESCAPE) {
					self.RequestClose();
					return 0;
				}
				if (vk == VK_TAB && (altFromMessage ||
						(GetKeyState(VK_MENU) & 0x8000))) {
					self.RequestClose();
					return forward();
				}
				if (vk == VK_LWIN || vk == VK_RWIN) return forward();
			}
		}
		if (open && self.ShouldSwallow()) return 0;
		return forward();
	}

	/* ---- 鼠标 ---- */
	if (IsMouseMessage(msg)) {
		// **只吞面板矩形内的鼠标消息。** 整窗吞的话（第一版）面板外也是一片死区：
		// 玩家把光标移出游戏窗口想点别的程序 —— 消息全被我们吃掉，焦点永远
		// 切不出去（用户实测："只要唤起了，就什么都点不了"）。面板外放行，
		// 点到别的窗口是系统自己的路由，不用我们操心。
		//
		// 判位用**消息自带的 lParam 坐标**：这一次点击就发生在这里，没有竞态。
		// 用 GetCursorPos 判的话（第一版）指针锁定补丁没装上时游戏还在每帧
		// 拽光标，面板内的点击会被判成面板外 —— fixture 上实测踩过。
		// #84：被钉模式下 lParam 是被钉死的那只（钉点），不可信 ——
		// 按虚拟位置判：点击/滚轮都落在用户以为的位置上。
		const bool inside = open && self.ShouldSwallow() &&
			(self.UsingVirtualCursor()
				? self.CursorInsidePanel(hwnd)
				: self.ClientInsidePanel(hwnd, GET_X_LPARAM(l), GET_Y_LPARAM(l)));
		// 抬起**无条件**记（按下才看面板内）：按住拖出面板、在外面松手的话，
		// 只记面板内的 UP 会让 leftDown 永远卡在 true —— 滑条粘着光标甩不掉，
		// 要再点一下面板内才解得开。UP 照旧转发（游戏收到一个没有 DOWN 的 UP，
		// 和以前一样，无害）。
		if (open && self.ShouldSwallow() && msg == WM_LBUTTONUP) {
			std::lock_guard<std::mutex> guard(self._sharedMutex);
			self._shared.leftDown = false;
		}
		if (inside) {
			{
				std::lock_guard<std::mutex> guard(self._sharedMutex);
				switch (msg) {
				case WM_LBUTTONDOWN:
				case WM_LBUTTONDBLCLK:
					self._shared.leftDown = true;
					self._shared.leftPressPulse = true;
					break;
				case WM_LBUTTONUP:
					self._shared.leftDown = false;
					break;
				case WM_MOUSEWHEEL:
					self._shared.wheelDelta += GET_WHEEL_DELTA_WPARAM(w);
					break;
				default:
					break;
				}
			}
			return 0;
		}
		if (open && msg == WM_LBUTTONDOWN) {
			// 开着但点在面板外：放行前先解掉鼠标捕获。游戏在输入期间
			// SetCapture 的话，点击会被路由回游戏窗口（谁也点不了）；
			// ReleaseCapture 让这条点击真正落到光标所在的窗口上。
			// 玩家点别处丢焦点时 WM_ACTIVATEAPP(FALSE) 自己会关浮层，
			// 这里不抢着关 —— 万一他只是点了一下游戏自己的画面呢。
			ReleaseCapture();
		}
		return forward();
	}

	// 开着的时候光标归我们管：**设成标准箭头并且不转给游戏**。
	// 游戏正是在它自己的 WM_SETCURSOR 里把光标藏起来的，不转就等于把它挡住了。
	if (msg == WM_SETCURSOR && open && self.ShouldSwallow()) {
		SetCursor(LoadCursorW(nullptr, IDC_ARROW));
		return TRUE;
	}

	/* ---- raw input ---- */
	//
	// **RIDEV_NOLEGACY 的游戏压根没有 WM_KEYDOWN**，键盘只从这里来。所以快捷键
	// 也得在这条路上认一次，否则那类游戏永远打不开浮层。
	// 吞的时候仍然要交给 DefWindowProc —— raw input 要求应用把没处理的
	// WM_INPUT 交给它做清理，直接 return 0 会泄漏系统侧的缓冲。
	if (msg == WM_INPUT) {
		UINT size = 0;
		if (GetRawInputData((HRAWINPUT)l, RID_INPUT, nullptr, &size,
				sizeof(RAWINPUTHEADER)) == 0 && size > 0 &&
			size <= sizeof(RAWINPUT) * 2) {
			alignas(8) uint8_t buffer[sizeof(RAWINPUT) * 2]{};
			if (GetRawInputData((HRAWINPUT)l, RID_INPUT, buffer, &size,
					sizeof(RAWINPUTHEADER)) == size) {
				const RAWINPUT* raw = reinterpret_cast<const RAWINPUT*>(buffer);
				if (raw->header.dwType == RIM_TYPEKEYBOARD) {
					const USHORT vk = raw->data.keyboard.VKey;
					const bool down =
						(raw->data.keyboard.Flags & RI_KEY_BREAK) == 0;
					self._keyMessagesSeen.fetch_add(1, std::memory_order_relaxed);
					if (down && vk == self._hotkeyVk) {
						self._hotkeyVkSeen.fetch_add(1, std::memory_order_relaxed);
						// raw input 里没有 context 位，只能查同步键盘状态
						if (ModsHeldSync(self._hotkeyMods, false)) {
							self.OnHotkeyDown();
							return unicode ? DefWindowProcW(hwnd, msg, w, l)
								: DefWindowProcA(hwnd, msg, w, l);
						}
					}
					if (down && vk == VK_ESCAPE && open) self.RequestClose();
				} else if (raw->header.dwType == RIM_TYPEMOUSE && open &&
					self.ShouldSwallow()) {
					// 面板内判定先做（拿不拿锁都行，这里必须在锁外 ——
					// CursorInsidePanel 自己也要拿 _sharedMutex）。#84 起虚拟感知：
					// 被钉模式下按虚拟位置判（真实的那只钉在中心不可信）。
					const bool inside = self.CursorInsidePanel(hwnd);
					const USHORT mouseFlags = raw->data.mouse.usFlags;
					const USHORT buttonFlags = raw->data.mouse.usButtonFlags;
					std::lock_guard<std::mutex> guard(self._sharedMutex);
					// ---- #84 虚拟光标：增量累计（不受面板内门控 —— 出了面板也要跟：
					// 发散判定要看它动没动，回面板时位置也得是连续的）----
					// MOUSE_MOVE_ABSOLUTE（RDP/数位板）：lLastX/Y 不是位移而是绝对值，
					// 累计会乱跳 —— 标记停用，本次打开不再启用虚拟光标。
					if (mouseFlags & MOUSE_MOVE_ABSOLUTE) {
						self._shared.virtualAbsolute = true;
					} else {
						self._shared.virtualClient.x += raw->data.mouse.lLastX;
						self._shared.virtualClient.y += raw->data.mouse.lLastY;
						// 夹进客户区：系统光标出不了窗口，虚拟的也不该能 ——
						// 不夹的话推着鼠标抵住窗口边时它会飘出画面外。
						RECT client{};
						if (hwnd && GetClientRect(hwnd, &client)) {
							POINT& v = self._shared.virtualClient;
							v.x = (std::max)(client.left,
								(std::min)(v.x, client.right - 1));
							v.y = (std::max)(client.top,
								(std::min)(v.y, client.bottom - 1));
						}
					}
					// ---- 按键/滚轮：按下/滚轮只在面板内记；抬起无条件记
					//（拖出面板在外面松手也要解除 leftDown —— 理由同传统消息那条路）。
					if (buttonFlags & RI_MOUSE_LEFT_BUTTON_UP) {
						self._shared.leftDown = false;
					}
					if (inside) {
						if (buttonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) {
							self._shared.leftDown = true;
							self._shared.leftPressPulse = true;
						}
						if (buttonFlags & RI_MOUSE_WHEEL) {
							self._shared.wheelDelta +=
								(SHORT)raw->data.mouse.usButtonData;
						}
					}
				}
			}
		}
		if (open && self.ShouldSwallow()) {
			// 吞掉但让系统清理。**这里不按光标位置门控**：RIDEV_NOLEGACY 的游戏
			// 键盘只走这条路，光标在面板外时键盘不吞的话快捷键/ESC 就失灵了。
			// 面板外的误滚轮代价小（面板滚一下），键盘失灵代价大，取舍如此。
			return unicode ? DefWindowProcW(hwnd, msg, w, l)
				: DefWindowProcA(hwnd, msg, w, l);
		}
		return forward();
	}

	return forward();
}

// 每帧在 present 线程上调。三件事：该挂就挂、换窗口就搬、被顶掉就重挂。
void Overlay::EnsureInputHook() noexcept {
	// 低级钩子兜底一旦启用就一直用它 —— 两条路同时生效会把快捷键翻转两次
	if (_llFallback) return;

	const HWND want = _gameWindow.load(std::memory_order_acquire);
	if (!want || !IsWindow(want)) return;

	const HWND hooked = _hookedWindow.load(std::memory_order_acquire);
	if (hooked == want) {
		// **完整性自检。** 游戏会自己重设窗口过程（换分辨率、进出全屏、
		// 自己的 UI 框架初始化都可能），被顶掉之后我们就再也收不到消息，
		// 而这在画面上完全看不出来。REFramework 也做同一件事。
		//
		// **查询必须用挂的时候同一个变体。** 拿 W 去查一个用 A 装进去的过程，
		// 拿回来的是 user32 的翻译 thunk（0xFFFFxxxx），永远不等于我们的裸
		// 指针 —— 于是每帧都误报"被顶掉"、每帧重挂一次（实测鬼武者 + REFramework
		// 那一局 41 秒重挂 2315 次）。更糟的是重挂拿回的 `previous` 是指向
		// 我们自己的 thunk，`_originalProc` 被覆盖成"我们自己"：转发链变成
		// 我们 -> 守卫桥 -> 游戏真过程，**夹在链里的 REFramework 被整个跳过** ——
		// 它挂的是 Present 所以菜单还渲染，但一条消息都收不到
		// （点不了、拖不动、快捷键无效）。A 查 A 装拿回的才是裸指针。
		const LONG_PTR current = _hookedUnicode
			? GetWindowLongPtrW(want, GWLP_WNDPROC)
			: GetWindowLongPtrA(want, GWLP_WNDPROC);
		if (current == reinterpret_cast<LONG_PTR>(&Overlay::WndProc)) return;
		D5_LOG_WARN(L"游戏内浮层：窗口过程被游戏顶掉了（现在是 %p），重新挂上",
			reinterpret_cast<void*>(current));
		_hookedWindow.store(nullptr, std::memory_order_release);
		_originalProc.store(nullptr, std::memory_order_release);
	} else if (hooked) {
		// 换窗口了：先把旧的还回去。**游戏真过程一并作废** —— 它属于
		// 旧窗口，桥到新窗口上就是把两个窗口的过程接错（见 _firstOriginal）。
		RemoveWndProcHook();
		_firstOriginal.store(nullptr, std::memory_order_release);
	}

	_hookedUnicode = IsWindowUnicode(want) != FALSE;
	SetLastError(0);
	const LONG_PTR previous = _hookedUnicode
		? SetWindowLongPtrW(want, GWLP_WNDPROC,
			reinterpret_cast<LONG_PTR>(&Overlay::WndProc))
		: SetWindowLongPtrA(want, GWLP_WNDPROC,
			reinterpret_cast<LONG_PTR>(&Overlay::WndProc));
	if (!previous && GetLastError() != 0) {
		static bool told = false;
		if (!told) {
			told = true;
			D5_LOG_ERROR(L"游戏内浮层：挂窗口过程失败（窗口 %p，错误 %lu）—— "
				L"快捷键不会响应。", want, GetLastError());
		}
		return;
	}
	_originalProc.store(reinterpret_cast<WNDPROC>(previous),
		std::memory_order_release);
	// **第一次挂这个窗口时，把游戏自己的真过程另存一份。** 之后被谁顶掉、
	// 重挂多少次都只更新 _originalProc；而 WndProc 的重入守卫在"环的第二跳"
	// 上桥的是这一份 —— 桥到 _originalProc 的话，第二跳又是顶掉我们的那个
	// （比如 dinput8），环就还在。看现在挂着的还是不是"不是我们也不是
	// 游戏真过程"的第三方（= 有库在我们之上子类化），把这件事写进日志。
	if (!_firstOriginal.load(std::memory_order_acquire)) {
		_firstOriginal.store(reinterpret_cast<WNDPROC>(previous),
			std::memory_order_release);
		_firstOriginalUnicode = _hookedUnicode;
	} else if (reinterpret_cast<WNDPROC>(previous) !=
			_firstOriginal.load(std::memory_order_acquire)) {
		D5_LOG_INFO(L"游戏内浮层：窗口过程上有人子类化（这次的原过程 %p，"
			L"游戏真过程 %p）—— 重入守卫会在这条边上断环。",
			reinterpret_cast<void*>(previous),
			(void*)_firstOriginal.load(std::memory_order_acquire));
	}
	_hookedWindow.store(want, std::memory_order_release);
	D5_LOG_INFO(L"游戏内浮层：已挂上窗口过程（窗口 %p，%s，原过程 %p）",
		want, _hookedUnicode ? L"Unicode" : L"ANSI",
		reinterpret_cast<void*>(previous));
}

void Overlay::RemoveWndProcHook() noexcept {
	const HWND hooked = _hookedWindow.exchange(nullptr, std::memory_order_acq_rel);
	const WNDPROC original = _originalProc.exchange(nullptr,
		std::memory_order_acq_rel);
	if (!hooked || !original) return;
	// 只在"现在挂着的确实是我们"时才还 —— 中间被别人插了一层的话，
	// 硬还回去会把那一层直接抹掉。**查询用挂的时候同一个变体**（理由同
	// EnsureInputHook 里的自检：混着查拿到的是翻译 thunk，永远比不上，
	// 后果是永远不还 —— 窗口一换，我们的过程就留在一个死窗口上）。
	if (IsWindow(hooked) &&
		(_hookedUnicode
			? GetWindowLongPtrW(hooked, GWLP_WNDPROC)
			: GetWindowLongPtrA(hooked, GWLP_WNDPROC)) ==
				reinterpret_cast<LONG_PTR>(&Overlay::WndProc)) {
		if (_hookedUnicode) {
			SetWindowLongPtrW(hooked, GWLP_WNDPROC,
				reinterpret_cast<LONG_PTR>(original));
		} else {
			SetWindowLongPtrA(hooked, GWLP_WNDPROC,
				reinterpret_cast<LONG_PTR>(original));
		}
	}
}

/* ---------------- 兜底：低级钩子（只在 WndProc 挂不上时启用） ---------------- */

LRESULT CALLBACK Overlay::KeyboardProc(int code, WPARAM wParam, LPARAM lParam) {
	Overlay& self = GetOverlay();
	if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wParam, lParam);

	const KBDLLHOOKSTRUCT* info = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lParam);
	const bool down = wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN;
	const DWORD vk = info ? info->vkCode : 0;

	// 前台判断放在这里做一次：钩子线程本来就在，省得再开定时器。
	// 窗口只用来做坐标换算（见 ProcessInput），"在不在前台"按进程判。
	const HWND gameWindow = self._gameWindow.load(std::memory_order_acquire);
	const bool gameForeground = ForegroundIsThisProcess();
	self._foreground.store(gameForeground, std::memory_order_release);

	// **快捷键也要看前台。**
	//
	// 第一版是无条件处理并 `return 1`。后果不是"多开一次浮层"，而是
	// **游戏在后台跑着的时候，用户在任何别的程序里按这个组合键都会被吃掉** ——
	// 而他完全不会想到是这个工具干的。夹具上第一次跑就撞见了。
	if (down && vk == self._hotkeyVk && ModsHeld(self._hotkeyMods)) {
		self._hotkeyVkSeen.fetch_add(1, std::memory_order_relaxed);
		if (gameForeground) {
			self.OnHotkeyDown();
			return 1;   // 只有游戏在前台时才吞，别的程序里放行
		}
		// **"按了键但没反应"必须留下线索。** 不打这条日志的话，用户遇到的现象是
		// 快捷键完全失灵，而原因（前台窗口不属于这个游戏）在画面上是看不出来的。
		// 每 60 次重复一条：只打一次的话，"焦点跑到别的窗口去了"这种从某一刻开始
		// 才出现的故障会正好落在已经打过之后，日志里反而是干净的。
		static uint32_t ignored = 0;
		if (ignored++ % 60 == 0) {
			DWORD pid = 0;
			GetWindowThreadProcessId(GetForegroundWindow(), &pid);
			D5_LOG_WARN(L"游戏内浮层：收到快捷键（第 %u 次），但前台窗口不属于本进程"
				L"（前台 %p pid=%lu，本进程 pid=%lu，swapchain 窗口 %p），忽略。",
				ignored, GetForegroundWindow(), pid, GetCurrentProcessId(),
				gameWindow);
		}
	}

	if (!self.ShouldSwallow()) {
		return CallNextHookEx(nullptr, code, wParam, lParam);
	}

	// **Alt+Tab 永远放行**，而且顺手关掉浮层 —— 玩家要切出去了，
	// 再吞下去就等于把他的输入锁在一个他已经离开的窗口上。
	if (vk == VK_TAB && (GetAsyncKeyState(VK_MENU) & 0x8000)) {
		self._open.store(false, std::memory_order_release);
		return CallNextHookEx(nullptr, code, wParam, lParam);
	}
	// Windows 键也放行：那是玩家的逃生出口
	if (vk == VK_LWIN || vk == VK_RWIN) {
		return CallNextHookEx(nullptr, code, wParam, lParam);
	}

	if (down && vk == VK_ESCAPE) {
		// **钩子线程自己先把开关关掉，不要只排队等 present 线程处理。**
		//
		// 关闭的正常路径在 ProcessInput（present 线程）里。但如果游戏卡死了、
		// present 不再跑，那条路就永远不会执行 —— 而输入还被我们吞着，
		// 玩家按 ESC 没反应、鼠标也动不了。这是这个功能能造成的最坏后果。
		// 所以这里直接落 _open=false（ShouldSwallow 立刻就为假了），
		// 同时照常排队，让 present 线程醒着的话把清理工作也做掉。
		self._open.store(false, std::memory_order_release);
		return 1;
	}
	// 其余按键在浮层开着时一律吞掉
	return 1;
}

LRESULT CALLBACK Overlay::MouseProc(int code, WPARAM wParam, LPARAM lParam) {
	Overlay& self = GetOverlay();
	if (code != HC_ACTION) return CallNextHookEx(nullptr, code, wParam, lParam);
	if (!self.ShouldSwallow()) {
		return CallNextHookEx(nullptr, code, wParam, lParam);
	}

	const MSLLHOOKSTRUCT* info = reinterpret_cast<const MSLLHOOKSTRUCT*>(lParam);
	{
		std::lock_guard<std::mutex> guard(self._sharedMutex);
		if (info) self._shared.cursorScreen = info->pt;
		switch (wParam) {
		case WM_LBUTTONDOWN:
			self._shared.leftDown = true;
			self._shared.leftPressPulse = true;
			break;
		case WM_LBUTTONUP:
			self._shared.leftDown = false;
			break;
		case WM_MOUSEWHEEL:
			if (info) {
				self._shared.wheelDelta +=
					GET_WHEEL_DELTA_WPARAM(info->mouseData);
			}
			break;
		default:
			break;
		}
	}
	return 1;   // 开着的时候鼠标一律不给游戏
}

DWORD WINAPI Overlay::HookThread(LPVOID param) {
	Overlay* self = static_cast<Overlay*>(param);
	// **低级钩子必须装在一个有消息循环的线程上** —— 回调是通过这个线程的
	// 消息队列派发的。装在没有泵的线程上钩子会安静地不工作。
	g_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc,
		GetModuleHandleW(nullptr), 0);
	g_mouseHook = SetWindowsHookExW(WH_MOUSE_LL, MouseProc,
		GetModuleHandleW(nullptr), 0);
	if (!g_keyboardHook) {
		D5_LOG_WARN(L"游戏内浮层：键盘钩子装不上（错误 %lu）—— "
			L"快捷键不会响应。常见原因是游戏以管理员身份运行而工具不是。",
			GetLastError());
	} else {
		D5_LOG_INFO(L"游戏内浮层：键鼠钩子已装上（键盘 %p 鼠标 %p）",
			g_keyboardHook, g_mouseHook);
	}

	MSG message{};
	while (self->_hookRunning.load(std::memory_order_acquire)) {
		// 带超时的等待：既能收钩子回调，也能定期检查退出标志
		if (MsgWaitForMultipleObjects(0, nullptr, FALSE, 100, QS_ALLINPUT) ==
			WAIT_OBJECT_0) {
			while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
				if (message.message == WM_QUIT) {
					self->_hookRunning.store(false, std::memory_order_release);
					break;
				}
				TranslateMessage(&message);
				DispatchMessageW(&message);
			}
		}
	}

	if (g_keyboardHook) { UnhookWindowsHookEx(g_keyboardHook); g_keyboardHook = nullptr; }
	if (g_mouseHook) { UnhookWindowsHookEx(g_mouseHook); g_mouseHook = nullptr; }
	return 0;
}

/* ---------------- 生命周期 ---------------- */

bool Overlay::Initialize(ID3D12Device* device, DXGI_FORMAT backbufferFormat,
	HWND gameWindow, OverlayApplyFn apply) noexcept {
	Release();
	_gameWindow.store(gameWindow, std::memory_order_release);
	_apply = apply;
	_use11 = false;
	if (!_panel.Initialize(device, backbufferFormat, OVERLAY_WIDTH,
			OVERLAY_MAX_HEIGHT, L"D5Q.Overlay.Upload")) {
		return false;
	}
	return FinishInitialize();
}

bool Overlay::Initialize11(ID3D11Device* device, ID3D11DeviceContext* context,
	DXGI_FORMAT backbufferFormat, HWND gameWindow, OverlayApplyFn apply) noexcept {
	Release();
	_gameWindow.store(gameWindow, std::memory_order_release);
	_apply = apply;
	_use11 = true;
	if (!_panel11.Initialize(device, context, backbufferFormat, OVERLAY_WIDTH,
			OVERLAY_MAX_HEIGHT, L"D5Q.Overlay.Upload11")) {
		return false;
	}
	return FinishInitialize();
}

// Initialize / Initialize11 的公共尾巴（两个后端只有贴图层不同，其余必须
// 一字不差 —— 抽出来就是防走样）。
bool Overlay::FinishInitialize() noexcept {
	_pixels = new uint8_t[size_t(OVERLAY_WIDTH) * OVERLAY_MAX_HEIGHT * 4]();

	// **主路：挂游戏窗口的 WndProc。** 成功就到此为止 —— 低级钩子不装。
	EnsureInputHook();
	if (_hookedWindow.load(std::memory_order_acquire)) {
		D5_LOG_INFO(L"游戏内浮层已就绪（%ux 最高 %u，快捷键 mods=%u vk=0x%02X，"
			L"输入走窗口过程，%s）",
			OVERLAY_WIDTH, OVERLAY_MAX_HEIGHT, _hotkeyMods, _hotkeyVk,
			_use11 ? L"D3D11" : L"D3D12");
		return true;
	}

	// 挂不上才退回低级钩子。**两条路只能有一条生效**：都开着的话按一下快捷键
	// 会被翻转两次（开了又关），表现出来就是"完全没反应"。
	// 这里置上 _llFallback 之后 EnsureInputHook 就不再尝试窗口过程了 ——
	// 中途切换主人会让"谁拥有 _open"这件事说不清楚，那是踩过的坑。
	_llFallback = true;
	D5_LOG_WARN(L"游戏内浮层：窗口过程挂不上（窗口 %p），退回低级键鼠钩子。"
		L"这条路在部分游戏上收不到快捷键（前台窗口和 swapchain 窗口不是同一个）。",
		_gameWindow.load(std::memory_order_acquire));
	_hookRunning.store(true, std::memory_order_release);
	_hookThread = CreateThread(nullptr, 0, HookThread, this, 0, &_hookThreadId);
	if (!_hookThread) {
		D5_LOG_WARN(L"游戏内浮层：钩子线程创建失败，浮层只能显示不能交互。");
		_hookRunning.store(false, std::memory_order_release);
	}
	D5_LOG_INFO(L"游戏内浮层已就绪（%ux 最高 %u，快捷键 mods=%u vk=0x%02X，"
		L"输入走低级钩子兜底，%s）",
		OVERLAY_WIDTH, OVERLAY_MAX_HEIGHT, _hotkeyMods, _hotkeyVk,
		_use11 ? L"D3D11" : L"D3D12");
	return true;
}

void Overlay::Release() noexcept {
	// **窗口过程要还回去。** 不还的话 core 卸载后游戏窗口上还挂着一个已经不存在的
	// 函数指针，下一条消息就是崩溃。
	RemoveWndProcHook();
	// 指针锁定补丁同理：不还原的话游戏 exe 的 IAT 里永远指向一个不存在的函数。
	RemovePointerLockPatch();
	if (_hookThread) {
		_hookRunning.store(false, std::memory_order_release);
		// 叫醒消息循环，让它自己摘钩子后退出
		if (_hookThreadId) PostThreadMessageW(_hookThreadId, WM_QUIT, 0, 0);
		WaitForSingleObject(_hookThread, 2000);
		CloseHandle(_hookThread);
		_hookThread = nullptr;
		_hookThreadId = 0;
	}
	_llFallback = false;
	_panel.Release();
	_panel11.Release();
	_use11 = false;
	delete[] _pixels;
	_pixels = nullptr;
	_items.clear();
	_open.store(false, std::memory_order_release);
	_dirty = true;
}

void Overlay::SetHotkey(uint32_t vk, uint32_t mods) noexcept {
	if (vk) _hotkeyVk = vk;
	_hotkeyMods = mods;
}

void Overlay::SetLang(int lang) noexcept {
	if (_state.lang == lang) return;
	_state.lang = lang;   // 0 中 / 1 日 / 2 英，见 OverlayRaster.h 的 OT
	_dirty = true;
}

/* ---------------- 每帧灌进来的状态 ---------------- */

void Overlay::SetTiming(float recentMs, float worstMs, bool hasSamples) noexcept {
	wchar_t main[32]{};
	wchar_t sub[128]{};
	const int lang = _state.lang;   // 三语文案，见 OverlayRaster.h 的 OT
	int level = 0;
	if (!hasSamples || !(recentMs > 0.0f)) {
		wcscpy_s(main, L"—");
		wcscpy_s(sub, OT(lang, L"还没有样本（DLSSNR 没在跑，或刚开）",
			L"サンプルなし（DLSSNR が動いていない、または開始直後）",
			L"No samples yet (DLSSNR not running, or just opened)"));
	} else {
		_snwprintf_s(main, _TRUNCATE, L"%.2f ms", recentMs);
		// 60fps 的预算是 16.7ms。占三成以上就该提醒了 ——
		// 这个读数存在的意义就是让人当场判断"划不划算"。
		_snwprintf_s(sub, _TRUNCATE,
			OT(lang, L"最差 %.1f ms · 占 60fps 预算 %d%%",
				L"最悪 %.1f ms · 60fps 予算の %d%%",
				L"worst %.1f ms · %d%% of the 60fps budget"),
			worstMs, (int)(recentMs / 16.67f * 100.0f + 0.5f));
		level = recentMs > 8.0f ? 2 : recentMs > 5.0f ? 1 : 0;
	}
	if (_state.timingMain != main || _state.timingSub != sub ||
		_state.timingLevel != level) {
		_state.timingMain = main;
		_state.timingSub = sub;
		_state.timingLevel = level;
		_dirty = true;
	}
}

void Overlay::SetValues(const float (&values)[OVERLAY_SETTING_COUNT]) noexcept {
	for (int i = 0; i < OVERLAY_SETTING_COUNT; ++i) {
		if (_state.values[i] != values[i]) {
			_state.values[i] = values[i];
			_dirty = true;
		}
	}
}

void Overlay::SetInfo(std::vector<OverlayInfoLine> info) noexcept {
	if (info.size() != _state.info.size()) {
		_state.info = std::move(info);
		_dirty = true;
		return;
	}
	for (size_t i = 0; i < info.size(); ++i) {
		if (info[i].text != _state.info[i].text ||
			info[i].warn != _state.info[i].warn) {
			_state.info = std::move(info);
			_dirty = true;
			return;
		}
	}
}

// 状态区（分辨率/矢量·深度来源/帧数）。和 SetInfo 同一个判脏模式 ——
// 帧数每秒都变，靠这个判脏把"没变就不重画"保住（拖动参数时重画是另一路）。
void Overlay::SetStatus(std::vector<OverlayInfoLine> status) noexcept {
	if (status.size() != _state.status.size()) {
		_state.status = std::move(status);
		_dirty = true;
		return;
	}
	for (size_t i = 0; i < status.size(); ++i) {
		if (status[i].text != _state.status[i].text ||
			status[i].warn != _state.status[i].warn) {
			_state.status = std::move(status);
			_dirty = true;
			return;
		}
	}
}

/* ---------------- 交互 ---------------- */

// 状态由钩子线程拥有；这两个只在 present 线程上做善后。
void Overlay::OnOpened() noexcept {
	_draggingItem = -1;
	_dirty = true;
	// 打开就拦指针锁定：不拦的话游戏每帧把光标拽回画面中心（拖滑块/拖面板
	// 全废），ClipCursor 还把光标锁死在窗口里（点外面任何东西都没反应）。
	InstallPointerLockPatch();
	// #84 虚拟光标本次打开的初始状态：种子 = 此刻真实光标（就是用户眼里光标
	// 在的地方），发散计数器清零。注册 raw 鼠标拿增量来源（游戏自己的注册
	// 先存下，关闭时原样还原 —— 不还的话它的 raw 输入就静默死了）。
	{
		POINT seed{ 0, 0 };
		const HWND hwnd = _gameWindow.load(std::memory_order_acquire);
		if (GetCursorPos(&seed) && hwnd) ScreenToClient(hwnd, &seed);
		std::lock_guard<std::mutex> guard(_sharedMutex);
		_shared.virtualClient = seed;
		_shared.virtualActive = false;
		_shared.virtualAbsolute = false;
	}
	_pinStreak = 0;
	_pinPrevReal = { 0, 0 };
	_pinPrevVirtual = { 0, 0 };
	D5_LOG_INFO(L"游戏内浮层：打开（游戏输入已被接管）");
}

void Overlay::OnClosed() noexcept {
	_draggingItem = -1;
	_state.hotIndex = -1;
	_state.cursor = { -1, -1 };
	// 关闭就还原 —— 游戏自己的视角转动一帧都不耽误。
	RemovePointerLockPatch();
	// #84 虚拟光标收尾：虚拟标志复位（下次打开重新播种/判定）。
	// 藏系统光标的 ShowCursor 计数由消息线程在下一条消息上弹回
	//（SyncVirtualCursorVisibility —— ShowCursor 线程绑定，present 线程调不作数）。
	{
		std::lock_guard<std::mutex> guard(_sharedMutex);
		_shared.virtualActive = false;
		_shared.virtualAbsolute = false;
	}
	// 唤醒消息线程：藏系统光标压下去的 ShowCursor 计数要在那边弹回来
	//（SyncVirtualCursorVisibility —— ShowCursor 线程绑定）。游戏消息
	// 稀疏时这一下省掉一段"关了浮层光标还是不见"的窗口期。
	if (const HWND hwnd = _gameWindow.load(std::memory_order_acquire)) {
		PostMessageW(hwnd, WM_NULL, 0, 0);
	}
	D5_LOG_INFO(L"游戏内浮层：关闭（输入还给游戏）");
}

// present 线程主动关（前台丢了、光栅化失败）。要同时落状态和善后。
void Overlay::Close() noexcept {
	if (_open.exchange(false, std::memory_order_acq_rel)) {
		_wasOpen = false;
		OnClosed();
	}
}

void Overlay::ProcessInput(uint32_t targetWidth, uint32_t targetHeight) noexcept {
	Shared snapshot;
	{
		std::lock_guard<std::mutex> guard(_sharedMutex);
		snapshot = _shared;
		// #82 滚动：滚轮是消费型的 —— 快照带走的这份就地清零
		//（关着也清：积几圈、一打开猛跳一下更怪）。
		_shared.wheelDelta = 0;
	}

	// **`_open` 由钩子线程拥有，这里只做副作用。**
	//
	// 一开始是反过来的（钩子只排队、present 线程翻转），但那样游戏一卡死输入就
	// 永远解不开。改成钩子直接落 `_open` 之后，present 线程再按计数翻转就会
	// 立刻把它开回去 —— 所以这里不能再翻转，只能观察它变了没有。
	const bool nowOpen = _open.load(std::memory_order_acquire);
	if (nowOpen != _wasOpen) {
		_wasOpen = nowOpen;
		if (nowOpen) OnOpened(); else OnClosed();
	}
	if (!nowOpen) {
		// 关着的时候把脉冲清掉，别积到下次打开
		std::lock_guard<std::mutex> guard(_sharedMutex);
		_shared.leftPressPulse = false;
		return;
	}

	// **前台丢了就自动关。** 钩子线程也会更新这个标志，但它只在有输入时才跑；
	// 玩家 Alt+Tab 之后可能一个输入都没有，所以这里每帧再确认一次。
	// 判据同钩子那边：前台窗口属不属于这个进程（见 ForegroundIsThisProcess）。
	const HWND gameWindow = _gameWindow.load(std::memory_order_acquire);
	if (!ForegroundIsThisProcess()) {
		_foreground.store(false, std::memory_order_release);
		Close();
		return;
	}
	_foreground.store(true, std::memory_order_release);

	if (_items.empty() || !_width || !_height) return;

	// #82 滚动：滚轮改 scroll（布局时平移内容行）。一档（WHEEL_DELTA=120）
	// 滚 40px ≈ 一行半，参照常见滚轮列表的手感。maxScroll 用上一帧布局的值
	// —— 这帧末重排才回填新的，差一帧无感。改了置 _dirty：本帧立刻重排重画。
	if (snapshot.wheelDelta != 0) {
		const int next = _state.scroll - snapshot.wheelDelta * 40 / 120;
		const int clamped = next < 0 ? 0
			: (next > _state.maxScroll ? _state.maxScroll : next);
		if (clamped != _state.scroll) {
			_state.scroll = clamped;
			_dirty = true;
		}
	}

	// 屏幕坐标 -> 面板本地坐标
	const POINT origin = PanelOrigin(targetWidth, targetHeight);
	const int panelX = origin.x;
	const int panelY = origin.y;
	// **光标位置每帧自己去问，不依赖输入消息。**
	//
	// 低级鼠标钩子那一版是从 MSLLHOOKSTRUCT.pt 抄的，换成挂 WndProc 之后
	// 没人再写这个值了 —— 结果光标永远停在 (0,0)，点击一律 `命中 -1`
	// （夹具日志：`左键 屏幕(0,0) -> 客户区(-242,-265) ... 命中 -1`）。
	// 改成直接问系统：这条路和输入走哪条通道无关，而且鼠标被游戏
	// 隐藏/夹住时它仍然是对的。
	POINT screen{ 0, 0 };
	if (!GetCursorPos(&screen)) screen = snapshot.cursorScreen;
	POINT client = screen;
	if (gameWindow) ScreenToClient(gameWindow, &client);

	// ---- #84 虚拟光标：发散判定 + 位置路由 ----
	//
	// 真实位置（client）每帧来自 GetCursorPos —— 游戏钉住它时它死在钉点；
	// 虚拟位置（snapshot.virtualClient）来自 raw 增量累计 —— 投递不受
	// SetCursorPos 影响。两者各自跟上一帧比：
	//   · 虚拟在动而真实不动，连续 5 帧 = 被钉（真实那只已经不听鼠标的了）
	//     → 切虚拟模式：之后命中/点击/画笔全走虚拟位置，系统光标藏掉。
	//   · 一起动 / 都不动：清零。正常游戏两位置永远同步，到不了阈值，
	//     整个功能等于不存在。
	// 切过去就保持到关浮层 —— 不自动切回：游戏中途停止 recenter 的话，
	// 真实位置会从钉点起跳，切回会让光标瞬移。窗口中心到面板的种子
	// 位移已经累计在虚拟位置里，不丢。
	bool usingVirtual = snapshot.virtualActive;
	if (!usingVirtual && !snapshot.virtualAbsolute) {
		const bool realMoved = !SamePoint(client, _pinPrevReal);
		const bool virtualMoved = !SamePoint(snapshot.virtualClient, _pinPrevVirtual);
		_pinStreak = (virtualMoved && !realMoved) ? _pinStreak + 1 : 0;
		if (_pinStreak >= 5) {
			usingVirtual = true;
			{
				std::lock_guard<std::mutex> guard(_sharedMutex);
				_shared.virtualActive = true;
			}
			// 唤醒消息线程：SyncVirtualCursorVisibility 要在那边把系统光标
			// 藏掉（ShowCursor 线程绑定，present 线程调不作数）。WM_NULL 无副作用。
			if (gameWindow) PostMessageW(gameWindow, WM_NULL, 0, 0);
			D5_LOG_INFO(L"游戏内浮层：光标被游戏钉住（虚拟动/真实不动连续 %d 帧）"
				L"—— 已切虚拟光标（raw 增量驱动，命中/点击全走虚拟位置）", _pinStreak);
			_pinStreak = 0;
		}
	}
	_pinPrevReal = client;
	_pinPrevVirtual = snapshot.virtualClient;
	if (usingVirtual) client = snapshot.virtualClient;
	const POINT local{ client.x - panelX, client.y - panelY };

	// **光标画不画，看系统光标是不是已经露出来了。**
	//
	// 我们只能在面板矩形里画自己的光标（贴的是一块不透明位图），所以光标一移出
	// 面板玩家就找不到它了 —— 用户原话："鼠标位于叠加界面外时不显示，不知道
	// 鼠标在哪"。正解是让**系统光标**露出来（见 ForceSystemCursor），那样整个
	// 画面上都看得见。这里实时问一次系统：真露出来了就别再画自己的，
	// 否则面板上会有两个光标。
	CURSORINFO cursorInfo{ sizeof(CURSORINFO) };
	const bool systemCursorVisible =
		GetCursorInfo(&cursorInfo) && (cursorInfo.flags & CURSOR_SHOWING) != 0;
	// #84：虚拟模式下系统光标已被我们藏掉 —— 面板外也必须画箭头（吸到最近边），
	// 不然光标一出面板就整个消失（"找不到鼠标"——#7 在虚拟模式下的复活）。
	// 非虚拟模式维持原判：系统光标露着就交给它，只在面板内补画。
	POINT drawCursor{ -1, -1 };
	if (usingVirtual) {
		// local.x 是 LONG、宽度是 uint32_t —— 两边显式转成 LONG，
		// std::min/max 的模板推导才对得上（C2672 那次的坑）。
		drawCursor.x = (std::max)(0L, (std::min)(local.x, (LONG)_width - 1));
		drawCursor.y = (std::max)(0L, (std::min)(local.y, (LONG)_height - 1));
	} else if (!systemCursorVisible && local.x >= 0 && local.y >= 0 &&
			local.x < (int)_width && local.y < (int)_height) {
		drawCursor = local;
	}
	if (drawCursor.x != _state.cursor.x || drawCursor.y != _state.cursor.y) {
		_state.cursor = drawCursor;
		_dirty = true;
	}

	const int hit = OverlayHitTest(_items, local, _height);
	if (hit != _state.hotIndex) {
		_state.hotIndex = hit;
		_dirty = true;
	}

	// 新按下的左键：取走这一帧的脉冲。置 true 是幂等的，两条输入路
	// （WM_INPUT / 传统消息）撞在一起也只算一次。
	bool pressed = false;
	{
		std::lock_guard<std::mutex> guard(_sharedMutex);
		pressed = _shared.leftPressPulse;
		_shared.leftPressPulse = false;
	}
	if (pressed) {
		// **点了没反应时唯一的线索。** 屏幕坐标、面板本地坐标、命中了谁，
		// 三个都要打 —— 只打"命中 -1"的话分不清是坐标换算错了还是矩形不对。
		// #84：虚拟模式下屏幕坐标是被钉死的那只（没意义），打上标记防误读。
		static uint32_t logged = 0;
		if (logged++ < 5) {
			D5_LOG_INFO(L"游戏内浮层：左键%s 屏幕(%ld,%ld) -> 客户区(%ld,%ld) -> "
				L"面板本地(%ld,%ld)，面板原点(%d,%d) 尺寸 %ux%u，命中 %d",
				usingVirtual ? L"（虚拟）" : L"",
				screen.x, screen.y,
				client.x, client.y, local.x, local.y,
				panelX, panelY, _width, _height, hit);
		}
	}

	// ---- 拖动标题栏移动面板 ----
	//
	// 松手就结束（不靠"再点一次"），否则光标移出窗口再松手会让面板永远粘在鼠标上。
	if (!snapshot.leftDown) _dragging = false;
	if (pressed && hit < 0 && OverlayInTitleBar(local, _width)) {
		_dragging = true;
		_dragGrab = { client.x - panelX, client.y - panelY };
	}
	if (_dragging && snapshot.leftDown) {
		_panelX = int(client.x - _dragGrab.x);
		_panelY = int(client.y - _dragGrab.y);
		_panelPlaced = true;
		// 位置变了要重贴（面板内容没变，所以不用 _dirty；Record 每帧都会贴）
	}

	if (pressed && hit >= 0) {
		const OverlayItem& item = _items[hit];
		switch (item.kind) {
		case OverlayItemKind::Close:
			Close();
			return;
		case OverlayItemKind::Toggle: {
			const float next = _state.values[item.setting] > 0.5f ? 0.0f : 1.0f;
			_state.values[item.setting] = next;
			if (_apply) _apply(OverlaySetting(item.setting), next);
			_dirty = true;
			break;
		}
		case OverlayItemKind::Check: {
			// #79 复选格：点整格翻转。Apply 后 _dirty 重排布局 ——
			// 新勾的组出现强度行 / 取消的组行消失（LayoutOverlay 重算）。
			const float next = _state.values[item.setting] > 0.5f ? 0.0f : 1.0f;
			_state.values[item.setting] = next;
			if (_apply) _apply(OverlaySetting(item.setting), next);
			_dirty = true;
			break;
		}
		case OverlayItemKind::Select: {
			int count = 0;
			OverlayOptionsFor(OverlaySetting(item.setting), count);
			if (count > 0) {
				const int now = (int)(_state.values[item.setting] + 0.5f);
				const float next = float((now + 1) % count);
				_state.values[item.setting] = next;
				if (_apply) _apply(OverlaySetting(item.setting), next);
				_dirty = true;
			}
			break;
		}
		case OverlayItemKind::Slider: {
			// **按下先只记住抓取点，不要立刻改值。**
			//
			// 滑块的"自己跳回 0"就是这里来的：命中测试认**整行**（更好点），
			// 但取值只按**槽内**的 x 映射 —— 光标在行的左半边（标签上）按下时，
			// x 远小于槽的 left，比例被钳到 0，值直接拍到最小值。
			// （实测症状：Local Structure 显示 0.00、Skin Structure -1.00，
			//  都正好是各自的 minValue。）
			//
			// 两条取值路：按在槽上 -> 滑块跳到光标处（绝对）；按在标签上 ->
			// 按下时的值 + 相对位移（拖多远走多远）。后者是 DCC 软件的习惯，
			// 也让"点行选 中、再拖"永远不丢当前值。
			_draggingItem = hit;
			_dragGrabX = local.x;
			_dragStartValue = _state.values[item.setting];
			_dragInTrough = local.x >= item.control.left &&
				local.x < item.control.right;
			break;
		}
		default:
			break;
		}
	}

	// 拖动中
	if (_draggingItem >= 0) {
		if (!snapshot.leftDown) {
			_draggingItem = -1;
		} else if (_draggingItem < (int)_items.size()) {
			const OverlayItem& item = _items[_draggingItem];
			float value = _dragStartValue;
			if (_dragInTrough) {
				// 按在槽上：滑块跟着光标走（绝对位置）
				value = OverlaySliderValueAt(item, local.x);
			} else {
				// 按在标签上：相对位移。槽宽 -> 值域的换算和绝对路一致。
				const int span = item.control.right - item.control.left;
				if (span > 0) {
					const float perPixel =
						(item.maxValue - item.minValue) / float(span);
					value = _dragStartValue +
						float(local.x - _dragGrabX) * perPixel;
				}
				value = OverlaySliderQuantize(item, value);
			}
			if (value != _state.values[item.setting]) {
				_state.values[item.setting] = value;
				if (_apply) _apply(OverlaySetting(item.setting), value);
				_dirty = true;
			}
		}
	}
}

/* ---------------- 上屏 ---------------- */

void Overlay::Record(ID3D12GraphicsCommandList* list, ID3D12Resource* target,
	uint32_t targetWidth, uint32_t targetHeight) noexcept {
	if (!_panel.IsReady() || !_pixels) return;

	// 每帧确认输入这条路还通着：窗口换了要搬过去，窗口过程被游戏顶掉了要重挂。
	EnsureInputHook();

	// **"挂上了但一条消息都没来"必须能看出来。**
	//
	// 街霸 6 那一局的日志里"浮层已就绪 + 钩子已装上"都在，然后什么都没有 ——
	// 于是没法区分"用户没按"、"按了但组合键不对"、"消息压根没到我们这儿"。
	// 现在过 30 秒（约 1800 帧）报一次现状，三种情况在日志里长得不一样。
	if (++_framesSinceHook == 1800) {
		const uint32_t keys = _keyMessagesSeen.load(std::memory_order_relaxed);
		const uint32_t hits = _hotkeyVkSeen.load(std::memory_order_relaxed);
		if (keys == 0) {
			D5_LOG_WARN(L"游戏内浮层：挂了 30 秒，**一条键盘消息都没收到**"
				L"（窗口 %p，路径=%s）。要么这个窗口不是接键盘的那个，"
				L"要么游戏用的输入路径我们还没覆盖。",
				_hookedWindow.load(std::memory_order_acquire),
				_llFallback ? L"低级钩子" : L"窗口过程");
		} else {
			D5_LOG_INFO(L"游戏内浮层：30 秒内收到 %u 条键盘消息，其中 %u 次是 vk=0x%02X。"
				L"（路径=%s）", keys, hits, _hotkeyVk,
				_llFallback ? L"低级钩子" : L"窗口过程");
		}
	}

	ProcessInput(targetWidth, targetHeight);
	if (!_open.load(std::memory_order_acquire)) return;
	if (!list || !target) return;

	const uint64_t now = GetTickCount64();
	if (_dirty && now - _lastRasterTick >= MIN_RASTER_INTERVAL_MS) {
		_lastRasterTick = now;
		_dirty = false;
		// 上下各留 16px 边距，别贴着画面边缘
		const uint32_t available = targetHeight > 32 ? targetHeight - 32 : 0;
		LayoutOverlay(_state, _items, _width, _height, available, &_state.maxScroll);
		// #82：内容缩水（取消勾选组）后把超界的 scroll 收回来 ——
		// 不收的话永远停在底，滚轮回滚要先空转一段才有反应。
		if (_state.scroll > _state.maxScroll) _state.scroll = _state.maxScroll;
		if (!RasterizeOverlay(_state, _items, _width, _height, _pixels) ||
			!_panel.Upload(_pixels, _width, _height)) {
			// 画不出来就别反复试，直接关掉 —— 一个画不出来的浮层还吞着输入
			// 是最坏的组合。
			D5_LOG_WARN(L"游戏内浮层：光栅化或上传失败，关闭浮层。");
			Close();
			return;
		}
	}
	if (!_panel.Width() || !_panel.Height()) return;
	// 面板比画面还大就别贴了（小窗口 / 低分辨率）
	if (_panel.Width() > targetWidth || _panel.Height() > targetHeight) {
		// 只报一次：每帧一条会把日志淹掉（实测刷了几十行）
		static bool told = false;
		if (!told) {
			told = true;
			D5_LOG_WARN(L"游戏内浮层：面板 %ux%u 比画面 %ux%u 还大，贴不上去。"
				L"画面太小（窗口模式或低分辨率）时会这样。",
				_panel.Width(), _panel.Height(), targetWidth, targetHeight);
		}
		return;
	}

	// **和命中测试用同一个来源。** 两处各算一遍的话，拖动之后"看到的"和
	// "点到的"就会错位 —— 而那种错位在画面上完全看不出原因。
	const POINT origin = PanelOrigin(targetWidth, targetHeight);
	const uint32_t x = uint32_t(origin.x);
	const uint32_t y = uint32_t(origin.y);
	static bool loggedOnce = false;
	if (!loggedOnce) {
		loggedOnce = true;
		D5_LOG_INFO(L"游戏内浮层：第一次贴图 —— 面板 %ux%u 贴到 (%u,%u)，"
			L"画面 %ux%u", _panel.Width(), _panel.Height(), x, y,
			targetWidth, targetHeight);
	}
	_panel.Record(list, target, x, y);
}

// D3D11 版：immediate context 串行、无 barrier，逻辑与 Record 逐行对应。
// 没抽公共函数是因为两边的"贴"那一步签名差太多（命令列表录 vs 即时调用），
// 硬抽会把**顺序**这个最容易走样的东西藏进间接层 —— 复制粘贴 + 对照注释
// 反而能一眼看出两边是否同步改了。
void Overlay::Record11(ID3D11DeviceContext* context, ID3D11Resource* target,
	uint32_t targetWidth, uint32_t targetHeight) noexcept {
	if (!_panel11.IsReady() || !_pixels) return;

	EnsureInputHook();
	// 30 秒现状汇报（同 Record —— 三种情况在日志里长得不一样）
	if (++_framesSinceHook == 1800) {
		const uint32_t keys = _keyMessagesSeen.load(std::memory_order_relaxed);
		const uint32_t hits = _hotkeyVkSeen.load(std::memory_order_relaxed);
		if (keys == 0) {
			D5_LOG_WARN(L"游戏内浮层：挂了 30 秒，**一条键盘消息都没收到**"
				L"（窗口 %p，路径=%s）。要么这个窗口不是接键盘的那个，"
				L"要么游戏用的输入路径我们还没覆盖。",
				_hookedWindow.load(std::memory_order_acquire),
				_llFallback ? L"低级钩子" : L"窗口过程");
		} else {
			D5_LOG_INFO(L"游戏内浮层：30 秒内收到 %u 条键盘消息，其中 %u 次是 vk=0x%02X。"
				L"（路径=%s）", keys, hits, _hotkeyVk,
				_llFallback ? L"低级钩子" : L"窗口过程");
		}
	}

	ProcessInput(targetWidth, targetHeight);
	if (!_open.load(std::memory_order_acquire)) return;
	if (!context || !target) return;

	const uint64_t now = GetTickCount64();
	if (_dirty && now - _lastRasterTick >= MIN_RASTER_INTERVAL_MS) {
		_lastRasterTick = now;
		_dirty = false;
		const uint32_t available = targetHeight > 32 ? targetHeight - 32 : 0;
		LayoutOverlay(_state, _items, _width, _height, available, &_state.maxScroll);
		// #82：内容缩水（取消勾选组）后把超界的 scroll 收回来 ——
		// 不收的话永远停在底，滚轮回滚要先空转一段才有反应。
		if (_state.scroll > _state.maxScroll) _state.scroll = _state.maxScroll;
		if (!RasterizeOverlay(_state, _items, _width, _height, _pixels) ||
			!_panel11.Upload(_pixels, _width, _height)) {
			D5_LOG_WARN(L"游戏内浮层：光栅化或上传失败，关闭浮层。");
			Close();
			return;
		}
	}
	if (!_panel11.Width() || !_panel11.Height()) return;
	if (_panel11.Width() > targetWidth || _panel11.Height() > targetHeight) {
		static bool told = false;
		if (!told) {
			told = true;
			D5_LOG_WARN(L"游戏内浮层：面板 %ux%u 比画面 %ux%u 还大，贴不上去。"
				L"画面太小（窗口模式或低分辨率）时会这样。",
				_panel11.Width(), _panel11.Height(), targetWidth, targetHeight);
		}
		return;
	}

	// **和命中测试用同一个来源**（同 Record 里的说明）。
	const POINT origin = PanelOrigin(targetWidth, targetHeight);
	const uint32_t x = uint32_t(origin.x);
	const uint32_t y = uint32_t(origin.y);
	static bool loggedOnce = false;
	if (!loggedOnce) {
		loggedOnce = true;
		D5_LOG_INFO(L"游戏内浮层(D3D11)：第一次贴图 —— 面板 %ux%u 贴到 (%u,%u)，"
			L"画面 %ux%u", _panel11.Width(), _panel11.Height(), x, y,
			targetWidth, targetHeight);
	}
	_panel11.CopyTo(context, target, x, y);
}

}  // namespace DXL
