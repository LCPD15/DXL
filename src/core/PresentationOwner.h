#pragma once
#include <windows.h>
#include "../common/IpcProtocol.h"

namespace DXL {
// A secondary DXGI surface must not displace a selected GL/D3D9 window or
// claim a process merely because its tiny/hidden helper presents first.
// Reasonably sized hidden fixtures remain valid when no visible primary exists.
inline bool AcceptDxgiPresentation(Ipc::GraphicsApi activeApi, HWND candidate,
                                   UINT width, UINT height) noexcept {
    if (activeApi == Ipc::GraphicsApi::OpenGL || activeApi == Ipc::GraphicsApi::D3D9)
        return false;
    // DXGI accepts zero dimensions at creation to use the window client size.
    RECT candidateClient{};
    if (candidate && (!width || !height) && GetClientRect(candidate, &candidateClient)) {
        if (!width) width = static_cast<UINT>(candidateClient.right);
        if (!height) height = static_cast<UINT>(candidateClient.bottom);
    }
    // Startup helpers can present before the main window even exists. Never
    // let a tiny/degenerate surface commit the process to its graphics device;
    // that would prevent the later main surface from taking ownership.
    if (width <= 64 || height <= 64) return false;
    const bool helper = candidate && !IsWindowVisible(candidate);
    if (!helper) return true;
    struct Search { HWND excluded; DWORD pid; bool found = false; } search{candidate, GetCurrentProcessId()};
    EnumWindows([](HWND window, LPARAM value) -> BOOL {
        auto& s = *reinterpret_cast<Search*>(value);
        if (window == s.excluded || !IsWindowVisible(window) || IsIconic(window)) return TRUE;
        DWORD pid = 0; GetWindowThreadProcessId(window, &pid);
        if (pid != s.pid || (GetWindowLongPtrW(window, GWL_EXSTYLE) & WS_EX_TOOLWINDOW)) return TRUE;
        RECT client{};
        if (GetClientRect(window, &client) && client.right >= 128 && client.bottom >= 96) {
            s.found = true; return FALSE;
        }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&search));
    return !search.found;
}
} // namespace DXL
