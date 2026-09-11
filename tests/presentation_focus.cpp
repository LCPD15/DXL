#include "../src/core/PresentationFocus.h"
#include <cstdio>

int main() {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const auto make = [&](DWORD style, HWND parent) {
        return CreateWindowExW(0, L"STATIC", L"DXL focus regression", style,
            0, 0, 80, 80, parent, nullptr, instance, nullptr);
    };
    HWND host = make(WS_OVERLAPPEDWINDOW, nullptr);
    HWND child = make(WS_CHILD, host);
    HWND nested = make(WS_CHILD, child);
    HWND sibling = make(WS_CHILD, host);
    HWND other = make(WS_OVERLAPPEDWINDOW, nullptr);
    HWND dialog = make(WS_POPUP, host);
    if (!host || !child || !nested || !sibling || !other || !dialog) return 2;
    unsigned failed = 0;
    const auto check = [&](const char* label, HWND a, HWND b, bool expected) {
        if (DXL::SamePresentationWindowTree(a, b) != expected) {
            std::printf("FAIL %s\n", label); ++failed;
        }
    };
    check("top-level game", host, host, true);
    check("embedded render surface with foreground host", child, host, true);
    check("nested render surface", nested, host, true);
    check("focus moves to sibling control", child, sibling, true);
    check("separate simulator window", child, other, false);
    check("owned dialog", child, dialog, false);
    check("desktop belongs to another process", child, GetDesktopWindow(), false);
    check("no foreground", child, nullptr, false);
    check("missing render surface", nullptr, host, false);
    DestroyWindow(nested);
    check("destroyed render surface", nested, host, false);
    DestroyWindow(dialog); DestroyWindow(other); DestroyWindow(host);
    std::printf("Presentation focus: %s (10 cases; hidden native windows)\n", failed ? "FAIL" : "PASS");
    return failed ? 1 : 0;
}
