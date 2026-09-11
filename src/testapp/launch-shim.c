// 冒充 Steam 行为的测试壳，用来验证「从工具启动」能跳过启动壳。
//
// 为什么需要它：Steam 游戏直接跑 exe 时，它会通过 Steam 重新拉起自己，第一个进程
// 只是个活几百毫秒的壳。实测在鬼武者上踩到过 —— 注入成功 0.25 秒后进程就退出了，
// 于是「从工具启动」看起来完全没生效。
//
// 这个 bug 只能用真实进程复现，所以做成一个固定的测试夹具：
//   build\DXL-inject.exe --launch build\DXL-launch-shim.exe
// 期望输出：「已从工具启动并注入 pid <真进程>（跳过了 1 个启动壳进程）」

#include <windows.h>
#include <stdio.h>
int wmain(int argc, wchar_t** argv) {
    int isReal = 0;
    for (int i = 1; i < argc; ++i) {
        if (!_wcsicmp(argv[i], L"--real")) isReal = 1;
    }
    if (!isReal) {
        wchar_t self[MAX_PATH];
        GetModuleFileNameW(NULL, self, MAX_PATH);
        wchar_t cmd[1024];
        _snwprintf_s(cmd, 1024, _TRUNCATE, L"\"%s\" --real", self);
        STARTUPINFOW si = { sizeof(si) };
        PROCESS_INFORMATION pi = { 0 };
        printf("[shim] pid=%lu 拉起真正的进程后退出\n", GetCurrentProcessId());
        fflush(stdout);
        CreateProcessW(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
        Sleep(300);
        return 0;
    }
    printf("[real] pid=%lu 这才是真正的游戏进程，活 40 秒\n", GetCurrentProcessId());
    fflush(stdout);
    Sleep(40000);
    return 0;
}
