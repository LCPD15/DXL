#pragma once

// 「谁在调 NGX」探针。
//
// 为什么需要它：下一步要旁听游戏自己的 DLSS 调用（拿到真深度、真运动矢量、jitter、
// UI 合成前的颜色），但拦截点取决于一个必须实测的事实 —— 游戏那一侧到底是**谁**在调
// NGX，以及用的是**静态导入**还是 `GetProcAddress`。
//
// 这决定了两条互斥的实现路线：
//   A. 冒充 nvngx：hook LoadLibrary，游戏要 nvngx.dll 时把我们的模块句柄给它，
//      我们实现整套导出面并转发。对静态导入和 GetProcAddress 都有效，但导出面必须
//      实现完整。
//   B. 改调用方的 IAT（我们已经有 FindImportSlot）。便宜得多，但只对**静态导入**
//      有效；调用方走 GetProcAddress 就完全抓不到。
//
// 本项目没有 inline hook 引擎（刻意不引入 MinHook/Detours，只做 vtable 打补丁），
// 而 NGX 的入口是普通导出函数不是 vtable 方法，所以没有第三条路。
//
// 探针只读不写：枚举模块、扫导入表、打日志。不 hook、不改任何东西。

#include <windows.h>

namespace DXL {

// 把进程里所有 NVIDIA / NGX / Streamline 相关模块列出来，并对每一个扫它的导入表，
// 报告它是否静态导入了 nvngx 的函数（以及具体哪些）。
//
// 调用时机很关键：必须在**游戏的 DLSS 已经跑起来之后**。太早的话 snippet 还没加载，
// 什么都看不到。core 会在第一次 present 之后延迟若干秒再跑一次。
void LogNgxCallers(const wchar_t* whenLabel) noexcept;

}  // namespace DXL
