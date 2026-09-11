#pragma once
#include <windows.h>
#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>

namespace DXL::WindowState {
struct State { int left, top, width, height; bool maximized; };
inline std::optional<int> Number(std::string_view json, std::string_view name) {
    const auto key = std::string("\"") + std::string(name) + "\"";
    auto pos = json.find(key);
    if (pos == json.npos) return {};
    pos = json.find(':', pos + key.size());
    if (pos == json.npos) return {};
    ++pos;
    while (pos < json.size() && (json[pos]==' ' || json[pos]=='\n' || json[pos]=='\r' || json[pos]=='\t')) ++pos;
    int value = 0;
    const auto result = std::from_chars(json.data()+pos, json.data()+json.size(), value);
    if (result.ec != std::errc{}) return {};
    const char* end = result.ptr;
    while (end < json.data()+json.size() && (*end==' ' || *end=='\n' || *end=='\r' || *end=='\t')) ++end;
    if (end == json.data()+json.size() || (*end != ',' && *end != '}')) return {};
    return value;
}
inline std::optional<State> Parse(std::string_view json) {
    auto version=Number(json,"version"), x=Number(json,"left"), y=Number(json,"top"),
         w=Number(json,"width"), h=Number(json,"height"), max=Number(json,"maximized");
    if (!version || *version != 1 || !x || !y || !w || !h || !max ||
        *x < -1000000 || *x > 1000000 || *y < -1000000 || *y > 1000000 ||
        *w < 64 || *w > 32768 || *h < 64 || *h > 32768 || (*max != 0 && *max != 1)) return {};
    return State{*x,*y,*w,*h,*max==1};
}
inline std::string Serialize(const State& s) {
    return "{\"version\":1,\"left\":"+std::to_string(s.left)+",\"top\":"+std::to_string(s.top)+
        ",\"width\":"+std::to_string(s.width)+",\"height\":"+std::to_string(s.height)+
        ",\"maximized\":"+std::to_string(s.maximized ? 1 : 0)+"}\n";
}
inline State Fit(State s, const RECT& work) {
    const int w=std::max(1L,work.right-work.left), h=std::max(1L,work.bottom-work.top);
    s.width=std::clamp(s.width,std::min(860,w),w);
    s.height=std::clamp(s.height,std::min(560,h),h);
    s.left=std::clamp(s.left,int(work.left),int(work.right)-s.width);
    s.top=std::clamp(s.top,int(work.top),int(work.bottom)-s.height);
    return s;
}
inline std::optional<State> Capture(HWND hwnd) {
    WINDOWPLACEMENT placement{sizeof(WINDOWPLACEMENT)};
    if (!GetWindowPlacement(hwnd,&placement)) return {};
    RECT r=placement.rcNormalPosition;
    // WINDOWPLACEMENT uses workspace coordinates; store screen coordinates.
    MONITORINFO monitor{sizeof(MONITORINFO)};
    if (GetMonitorInfoW(MonitorFromWindow(hwnd,MONITOR_DEFAULTTONEAREST),&monitor))
        OffsetRect(&r,monitor.rcWork.left-monitor.rcMonitor.left,monitor.rcWork.top-monitor.rcMonitor.top);
    if (r.right<=r.left || r.bottom<=r.top) return {};
    return State{r.left,r.top,r.right-r.left,r.bottom-r.top,
        IsZoomed(hwnd)!=FALSE || (IsIconic(hwnd) && (placement.flags&WPF_RESTORETOMAXIMIZED)!=0)};
}
inline bool Save(const std::filesystem::path& path, const State& state) {
    const auto tmp=std::filesystem::path(path.wstring()+L".tmp."+std::to_wstring(GetCurrentProcessId()));
    const auto text=Serialize(state);
    { std::ofstream out(tmp,std::ios::binary|std::ios::trunc);
      out.write(text.data(),std::streamsize(text.size())); out.close(); if (!out) return false; }
    const bool ok=MoveFileExW(tmp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
    if (!ok) { std::error_code ec; std::filesystem::remove(tmp,ec); }
    return ok;
}
inline std::optional<State> Load(const std::filesystem::path& path) {
    std::ifstream in(path,std::ios::binary);
    if (!in) return {};
    char data[4097]{}; in.read(data,sizeof(data));
    if (in.gcount()>4096) return {};
    return Parse(std::string_view(data,size_t(in.gcount())));
}
inline bool Restore(HWND hwnd, const std::filesystem::path& path) {
    const auto saved=Load(path);
    if (!saved) return false;
    RECT r{saved->left,saved->top,saved->left+saved->width,saved->top+saved->height};
    MONITORINFO monitor{sizeof(MONITORINFO)};
    if (!GetMonitorInfoW(MonitorFromRect(&r,MONITOR_DEFAULTTONEAREST),&monitor)) return false;
    const auto state=Fit(*saved,monitor.rcWork);
    SetWindowPos(hwnd,nullptr,state.left,state.top,state.width,state.height,SWP_NOZORDER|SWP_NOACTIVATE);
    ShowWindow(hwnd,state.maximized ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL);
    return true;
}
}
