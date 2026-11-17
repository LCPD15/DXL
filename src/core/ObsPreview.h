#pragma once
#include <windows.h>
#include <algorithm>
#include "MinHook.h"

namespace DXL::ObsPreview {
struct Viewport { int x, y, width, height; };
inline thread_local Viewport drawn{}, current{};
using Draw = void(*)();
inline Draw original[2]{};
inline void* targets[2]{};
inline void (*getViewport)(Viewport*);
inline void* (*getTarget)();
template<int I> void Observe() {
    if (!getTarget()) getViewport(&drawn);
    original[I]();
}
inline bool Installed() {
    static const bool active = [] {
        const auto obs = GetModuleHandleW(L"obs.dll");
        if (!obs || !GetModuleHandleW(L"obs64.exe")) return false;
        getViewport = reinterpret_cast<decltype(getViewport)>(GetProcAddress(obs,"gs_get_viewport"));
        getTarget = reinterpret_cast<decltype(getTarget)>(GetProcAddress(obs,"gs_get_render_target"));
        if (!getViewport || !getTarget) return false;
        const auto init = MH_Initialize();
        if (init != MH_OK && init != MH_ERROR_ALREADY_INITIALIZED) return false;
        const char* names[]{"obs_render_main_texture","obs_render_main_texture_src_color_only"};
        for (int i=0;i<2;++i) {
            auto address = reinterpret_cast<void*>(GetProcAddress(obs,names[i]));
            if (address && MH_CreateHook(address,i ? Observe<1> : Observe<0>,reinterpret_cast<void**>(&original[i])) == MH_OK &&
                MH_EnableHook(address) == MH_OK) targets[i] = address;
        }
        return targets[0] || targets[1];
    }();
    return active;
}
inline void Stop() { for (auto t : targets) if (t) MH_DisableHook(t); }
// Called under the presentation lock. A fullscreen projector has priority even
// when the operator clicks in the main OBS window on another monitor.
inline bool Select(HWND window, UINT width, UINT height) {
    current = drawn;
    drawn = {};
    const HWND root = GetAncestor(window,GA_ROOT);
    RECT r{};
    MONITORINFO m{sizeof(m)};
    const bool fullscreen = root && IsWindowVisible(root) && GetWindowRect(root,&r) &&
        GetMonitorInfoW(MonitorFromWindow(root,MONITOR_DEFAULTTONEAREST),&m) && EqualRect(&r,&m.rcMonitor);
    static HWND selected{};
    static ULONGLONG last{};
    const auto now = GetTickCount64();
    if (!fullscreen && selected != window && IsWindowVisible(selected) && now-last < 500) return false;
    if (fullscreen) { selected=window; last=now; }
    if (current.width<=0 || current.height<=0) return fullscreen;
    int x=std::clamp(current.x,0,int(width)), y=std::clamp(current.y,0,int(height));
    int right=std::clamp(current.x+current.width,0,int(width)), bottom=std::clamp(current.y+current.height,0,int(height));
    current={x,y,right-x,bottom-y};
    return current.width>0 && current.height>0;
}
}
