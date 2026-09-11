#pragma once

// 退出时先拆钩子 —— 街霸6退出崩溃的根治（2026-09 实测）。
//
// 背景：游戏退出序列里释放设备/swapchain 的那段窗口，我们的
// present / GetBuffer / GetDesc / ResizeBuffers / 队列 / 深度探测钩子
// 全都还挂在 vtable 上 —— 游戏的 teardown 撞进这些钩子就是 0xC0000005
// （core-89640：街霸6日志在正常 GPU 统计中戛然而止，连退出检测都没跑到；
// 对照 re9.exe.70284.dmp：崩溃 PC 在游戏代码里，栈底是 present 链）。
// 装了 REFramework 就不崩，是因为它检测到设备重置时先把自己拆干净
// （re2_framework_log.txt 结尾的 "Removing SetCursorPos patch"）。
// 这里抄的是同一件事：**窗口开始销毁时，先把我们所有的 vtable 补丁还原**，
// 再放行游戏的 teardown。
//
// 触发点（谁先看到"游戏在退出"谁调，只做一次）：
//   1. 浮层窗口过程的 WM_NCDESTROY（最早 —— 窗口销毁的最先信号）
//   2. 看门狗的"主窗口已经销毁"判定（兜底 —— 浮层没挂上/别的窗口时）
//
// 全部 inline + C++17 inline 变量：core.cpp / DepthTracker.cpp / Overlay.cpp /
// FreezeWatchdog.cpp 四个编译单元共用同一份，不需要谁给谁提供定义。

#include <windows.h>
#include <atomic>
#include <mutex>
#include <vector>

#include "../common/Log.h"

namespace DXL {

/* ---------------- vtable 补丁登记簿 ---------------- */

// 打补丁时把（槽位地址, 原函数）记进来，退出时**反序**还原。
// 同一块 vtable 是 dxgi/d3d12 全进程共享的，还原就是把原函数写回槽位 ——
// 之后游戏的调用直达驱动，不再经过我们。
struct VtablePatchEntry {
	void** slot;
	void* original;
};

inline std::vector<VtablePatchEntry>& VtablePatchRegistry() {
	// Stacked hooks can retain our forwarding wrappers through CRT teardown.
	// Keep the original-function registry alive for those last callers.
	static auto& registry = *new std::vector<VtablePatchEntry>;
	return registry;
}

inline std::mutex& VtablePatchMutex() {
	static auto& mutex = *new std::mutex;
	return mutex;
}

// 打补丁成功后调用。original 为空（VirtualProtect 失败等）不记 ——
// 没拿到原函数的补丁还原不了，也本来就不该留着自己用。
inline void RecordVtablePatch(void** slot, void* original) noexcept {
	if (!slot || !original) return;
	std::lock_guard<std::mutex> lock(VtablePatchMutex());
	VtablePatchRegistry().push_back({ slot, original });
}

// 反序还原所有补丁。写回的是原函数指针，重复调用幂等；
// 指针大小的写在 x64 上天然对齐，和游戏线程的并发调用不冲突。
inline void RestoreAllVtablePatches() noexcept {
	std::lock_guard<std::mutex> lock(VtablePatchMutex());
	auto& registry = VtablePatchRegistry();
	for (size_t i = registry.size(); i-- > 0;) {
		void** slot = registry[i].slot;
		void* original = registry[i].original;
		DWORD oldProtect = 0;
		if (VirtualProtect(slot, sizeof(void*), PAGE_READWRITE, &oldProtect)) {
			*slot = original;
			VirtualProtect(slot, sizeof(void*), oldProtect, &oldProtect);
		}
	}
}

/* ---------------- 退出请求 ---------------- */

inline std::atomic<bool> g_teardownRequested{ false };

inline bool IsTeardownRequested() noexcept {
	return g_teardownRequested.load(std::memory_order_acquire);
}

/* ---------------- 额外清理回调 ---------------- */

// vtable 之外还有"活物"要收的，往这里登记。这个头是独立文件、够不到
// core.cpp 的 g_state（NrFilter 在里面），所以用回调把两者接起来。
//
// 目前唯一的登记项：**释放我们自己的 NGX feature + 纹理**。DS2 实测
// （Documents\...\*.mdmp，v1.10.89.0 04-25-00）：只还原 vtable 不够 ——
// 崩溃 PC 在 nvngx_dlssnr.dll+0x147f8（我们自己的 snippet），读 null+0x48。
// 时序：退出清理"完成"之后，游戏调 NGX Shutdown 拆会话，snippet 内部
// 还握着我们没释放的 feature —— 空指针。上一版"不释放自己的 D3D 资源"
// 的理由（进程要没了，OS 会收走）对**纹理**成立，对 **NGX feature** 不成立：
// NGX 会话是游戏主动拆的，拆的时候会碰我们登记在里面的 feature。
inline std::mutex& ExtraCleanupMutex() {
	static auto& mutex = *new std::mutex;
	return mutex;
}

inline void (*g_extraCleanup)() noexcept = nullptr;

inline void SetTeardownExtraCleanup(void (*fn)() noexcept) noexcept {
	std::lock_guard<std::mutex> lock(ExtraCleanupMutex());
	g_extraCleanup = fn;
}

// 幂等：第一个看到的做全部清理，后来者直接返回。
// 纹理不释放（进程马上要没了，OS 会收走；在非 present 线程跨线程释放
// 反而引入新竞态）—— NGX feature 必须释放（见 g_extraCleanup 的说明）。
inline void RequestTeardown() noexcept {
	if (g_teardownRequested.exchange(true, std::memory_order_acq_rel)) return;
	D5_LOG_INFO(L"退出清理：游戏在退出 —— 先把所有 vtable 补丁还原"
		L"（present / GetBuffer / GetDesc / ResizeBuffers / 队列 / 工厂 / 深度探测），"
		L"游戏的 teardown 不再经过我们");
	RestoreAllVtablePatches();
	void (*extra)() noexcept;
	{
		std::lock_guard<std::mutex> lock(ExtraCleanupMutex());
		extra = g_extraCleanup;
	}
	if (extra) extra();
	D5_LOG_INFO(L"退出清理：完成（含 NGX feature 释放）");
}

}  // namespace DXL
