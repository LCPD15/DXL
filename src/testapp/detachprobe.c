// detachprobe.c —— 最小探针：DLL_PROCESS_ATTACH / DETACH 各用纯 Win32
// （CreateFileW+WriteFile，不碰 CRT）写一行标记。用来隔离判别：
// 注入式 DLL 在宿主 ExitProcess 时到底收不收得到 DLL_PROCESS_DETACH。
// core 的 DETACH 标记缺失有两种解释：DETACH 没执行 / 执行了但写失败。
// 这个探针把两个变量拆开——它自己的日志和 core 用同一条直写路径。
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static void Mark(const wchar_t* text)
{
    wchar_t path[MAX_PATH];
    if (!GetModuleFileNameW((HMODULE)0, path, MAX_PATH)) return;
    // 放到 %LOCALAPPDATA%\DXL\logs\detachprobe.txt（目录已存在）
    wchar_t dir[MAX_PATH];
    if (!GetEnvironmentVariableW(L"LOCALAPPDATA", dir, MAX_PATH)) return;
    wcscat_s(dir, MAX_PATH, L"\\DXL\\logs\\detachprobe.txt");
    (void)path;
    char utf8[256];
    int n = WideCharToMultiByte(CP_UTF8, 0, text, -1, utf8, sizeof(utf8), 0, 0);
    if (n <= 0) return;
    HANDLE h = CreateFileW(dir, FILE_APPEND_DATA, FILE_SHARE_READ, 0,
                           OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, 0);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(h, utf8, (DWORD)(n - 1), &w, 0);
    WriteFile(h, "\r\n", 2, &w, 0);
    CloseHandle(h);
}

BOOL APIENTRY DllMain(HMODULE m, DWORD reason, LPVOID param)
{
    (void)m;
    (void)param;
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(m);
        Mark(L"probe: ATTACH");
    } else if (reason == DLL_PROCESS_DETACH) {
        Mark(L"probe: DETACH");
    }
    return TRUE;
}
