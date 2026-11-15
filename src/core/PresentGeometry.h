#pragma once
#include <windows.h>
#include <dxgi1_3.h>
#include <algorithm>

namespace DXL {
struct PresentGeometry {
    UINT x = 0, y = 0, width = 0, height = 0;
    FLOAT sx = 1, sy = 1, offsetX = 0, offsetY = 0;
    bool operator==(const PresentGeometry&) const = default;
};
inline bool ReadPresentGeometry(IDXGISwapChain2* source, HWND hwnd, UINT bw, UINT bh, PresentGeometry& g) {
    if (!source || !hwnd || !bw || !bh) return false;
    g = {};
    DXGI_SWAP_CHAIN_DESC1 desc{};
    if (FAILED(source->GetDesc1(&desc))) return false;
    UINT sw = bw, sh = bh;
    UINT activeW = 0, activeH = 0;
    if (SUCCEEDED(source->GetSourceSize(&activeW, &activeH)) && activeW && activeH) {
        sw = std::min(bw, activeW); sh = std::min(bh, activeH);
    }
    // DirectComposition targets the HWND's backing coordinate system. Query
    // its client extent under the window's own DPI awareness, also on mixed-DPI desktops.
    const auto old = SetThreadDpiAwarenessContext(GetWindowDpiAwarenessContext(hwnd));
    RECT r{};
    const BOOL valid = GetClientRect(hwnd, &r);
    if (old) SetThreadDpiAwarenessContext(old);
    if (!valid || r.right <= 0 || r.bottom <= 0 || IsIconic(GetAncestor(hwnd, GA_ROOT))) return false;
    const UINT cw = UINT(r.right), ch = UINT(r.bottom);
    g.width = sw; g.height = sh;
    // DXGI documents ASPECT_RATIO_STRETCH as ordinary STRETCH on Win32
    // window swap chains. Retain centered aspect fitting only for other targets.
    HWND boundWindow{};
    const bool fitAspect = desc.Scaling == DXGI_SCALING_ASPECT_RATIO_STRETCH &&
        (FAILED(source->GetHwnd(&boundWindow)) || !boundWindow);
    if (desc.Scaling == DXGI_SCALING_NONE) {
        g.width = std::min(cw, sw); g.height = std::min(ch, sh);
        if (GetWindowLongPtrW(hwnd, GWL_EXSTYLE) & WS_EX_LAYOUTRTL) {
            g.x = sw - g.width;
            g.offsetX = float(cw - g.width);
        }
    } else if (fitAspect) {
        g.sx = g.sy = std::min(float(cw) / sw, float(ch) / sh);
        g.offsetX = (float(cw) - sw*g.sx) * .5f; g.offsetY = (float(ch) - sh*g.sy) * .5f;
    } else {
        g.sx = float(cw) / sw; g.sy = float(ch) / sh;
    }
    return true;
}
} // namespace DXL
