#pragma once
#include "../src/core/NgxModuleIdentity.h"
#include "../src/core/FrameGenSwapChains.h"

static void TestNgxIdentity() {
    using K = NgxModuleKind;
    const std::pair<const wchar_t*, K> cases[]{
        {L"C:\\game\\NVNGX_DLSS.DLL", K::Sr},
        {L"C:\\game\\nvngx_dlssd.dll", K::Rr},
        {L"C:\\game\\NVNGX_DLSSG.DLL", K::Fg},
        {L"C:/game/SL.DLSS_G.DLL", K::Fg},
        {L"C:/game/nvngx_dlssg_debug.dll", K::Fg},
        {L"C:/game/custom_sl.dlss_g.dll", K::Fg},
        {L"C:\\game\\_nvngx.dll", K::Other},
        {L"C:\\game\\nvngx_dlssnr.dll", K::Other},
        {L"C:\\game\\160_E658700.bin", K::Other},
        {L"C:\\NVIDIA\\NGX\\models\\dlss\\versions\\20318464\\files\\160_E658700.bin", K::CachedSr},
        {L"c:/nvidia/ngx/models/DLSSD/versions/20318464/files/160_E658700.BIN", K::CachedRr},
        {L"C:\\NVIDIA\\NGX\\models\\dlssg\\versions\\20318464\\files\\160_E658700.bin", K::CachedFg},
        {L"C:\\NVIDIA\\NGX\\models\\dlssnr\\versions\\20318464\\files\\160_E658700.bin", K::Other},
        {L"C:\\NVIDIA\\NGX\\models\\dlssg-extra\\versions\\20318464\\files\\160_E658700.bin", K::Other},
        {L"C:\\NVIDIA\\NGX\\models\\dlss\\versions\\\\files\\160_E658700.bin", K::Other},
        {L"C:\\NVIDIA\\NGX\\models\\dlss\\files\\160_E658700.bin", K::Other},
        {L"C:\\NVIDIA\\NGX\\models\\dlss\\versions\\20318464\\files\\extra\\160_E658700.bin", K::Other},
        {L"C:\\NVIDIA\\NGX\\models\\dlss\\versions\\20318464\\files\\..\\160_E658700.bin", K::Other},
    };
    for (const auto& c : cases) Check(ClassifyNgxModule(c.first) == c.second, "NGX path identity");
    auto load = [](const wchar_t* model) {
        auto p = std::filesystem::absolute(std::filesystem::path(L"ngx-fixtures/NVIDIA/NGX/models") /
            model / L"versions/20318464/files/160_E658700.bin");
        HMODULE m = LoadLibraryW(p.c_str()); Check(m != nullptr, "load synthetic cached NGX module"); return m;
    };
    // Keep the three module mappings for the duration of the production route
    // test below. The real cache SR export becomes its native SR leaf.
    const auto sr = load(L"dlss"), rr = load(L"dlssd"), fg = load(L"dlssg");
    Check(sr != rr && rr != fg && sr != fg, "same-basename fixture modules must be distinct");
    const auto srReal = GetProcAddress(sr, FN_EVALUATE);
    const auto srHook = HookedGetProcAddress(sr, FN_EVALUATE);
    Check(srHook != srReal && g_targetCount.load() == 1, "cache SR wrapper missing");
    Check(g_nativeUpscalerTarget[0].load(), "cached SR was treated as generic (player nativeLeaves=0 bug)");
    Check(HookedGetProcAddress(sr, FN_EVALUATE) == srHook && g_targetCount.load() == 1, "repeat lookup allocated another slot");
    HookedGetProcAddress(rr, FN_EVALUATE);
    Check(g_nativeUpscalerTarget[1].load(), "cached RR not recognized");
    const auto count = g_targetCount.load();
    Check(HookedGetProcAddress(fg, FN_EVALUATE) == GetProcAddress(fg, FN_EVALUATE), "cached FG evaluate was wrapped");
    Check(HookedGetProcAddress(fg, FN_CREATE) == GetProcAddress(fg, FN_CREATE), "cached FG create was wrapped");
    Check(g_targetCount.load() == count, "FG consumed an SR slot");
    Check(HookedGetProcAddress(sr, FN_CREATE) == GetProcAddress(sr, FN_CREATE), "SR CreateFeature changed");
    Check(!NgxEavesdrop::Get().FrameGenerationActive(0), "preloaded cached FG falsely disabled Present");
    { FrameGenSwapChains::Creation native(3);
      Check(!NgxEavesdrop::Get().FrameGenerationActive(0), "3 buffers plus preloaded FG disabled Present");
      { FrameGenSwapChains::Creation fgPending(4);
        Check(NgxEavesdrop::Get().FrameGenerationActive(0), "cached FG module not used by Present guard");
        { FrameGenSwapChains::Creation anotherNative(3);
          Check(NgxEavesdrop::Get().FrameGenerationActive(0), "nested 3-buffer chain cleared FG guard"); }
      }
      Check(!NgxEavesdrop::Get().FrameGenerationActive(0), "failed FG creation left Present guard stuck");
    }
    puts("PASS NGX identity: 18 path cases; same-name SR/RR/FG PE files; cached SR admitted; FG untouched; repeat slot stable");
}
