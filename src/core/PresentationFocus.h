#pragma once
#include <windows.h>

namespace DXL {
// Embedded render surfaces share focus with their top-level host. Separate
// windows (including owned dialogs) must not activate the game's overlay.
inline bool SamePresentationWindowTree(HWND renderWindow, HWND inputWindow) noexcept {
    if (!renderWindow || !inputWindow) return false;
    DWORD renderPid = 0, inputPid = 0;
    if (!GetWindowThreadProcessId(renderWindow, &renderPid) ||
        !GetWindowThreadProcessId(inputWindow, &inputPid) ||
        renderPid != GetCurrentProcessId() || inputPid != renderPid) return false;
    const HWND root = GetAncestor(renderWindow, GA_ROOT);
    return root && root == GetAncestor(inputWindow, GA_ROOT);
}

inline bool HasPresentationFocus(HWND renderWindow) noexcept {
    return SamePresentationWindowTree(renderWindow, GetForegroundWindow());
}
} // namespace DXL
