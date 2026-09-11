// Reproduce a statically imported UnityPlayer.dll owning the Streamline loader.
#include "../src/core/NgxEavesdrop.cpp"
#include <filesystem>
#include <stdexcept>
#include <cstdio>
using namespace DXL;
extern "C" __declspec(dllimport) FARPROC UnityLookup(const wchar_t*, const wchar_t*, const char*);
static void Check(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
int main() try {
    wchar_t executable[32768]{};
    GetModuleFileNameW(nullptr, executable, _countof(executable));
    const auto folder = std::filesystem::path(executable).parent_path();
    const auto sr = folder / L"nvngx_dlss.dll", fg = folder / L"nvngx_dlssg.dll";
    const auto plugin = folder / L"sl.common.dll";
    Check(GetModuleHandleW(L"UnityPlayer.dll") != nullptr, "UnityPlayer must be a startup import");
    Check(GetModuleHandleW(L"sl.common.dll") == nullptr, "Streamline must load after Install");
    NgxEavesdrop::Get().Install(GetModuleHandleW(nullptr), true);
    Check(FindPatched(GetModuleHandleW(L"UnityPlayer.dll")) != nullptr, "startup UnityPlayer was omitted from loader hooks");
    const auto evaluated = UnityLookup(plugin.c_str(), sr.c_str(), FN_EVALUATE);
    const auto native = GetProcAddress(GetModuleHandleW(L"nvngx_dlss.dll"), FN_EVALUATE);
    // Test's own IAT is hooked; use its saved original to obtain the raw export.
    const auto* owner = FindPatched(GetModuleHandleW(nullptr));
    const auto rawGetProc = owner && owner->getProcAddress ? owner->getProcAddress : &GetProcAddress;
    const auto rawSr = rawGetProc(GetModuleHandleW(L"nvngx_dlss.dll"), FN_EVALUATE);
    Check(evaluated && evaluated != rawSr && evaluated == native, "Unity -> Streamline -> SR did not return the Evaluate wrapper");
    Check(g_targetCount.load() == 1 && g_nativeUpscalerTarget[0].load(), "SR identity not retained");
    Check(FindPatched(GetModuleHandleW(L"sl.common.dll")) != nullptr, "Streamline loader chain missing");
    const auto repeat = UnityLookup(plugin.c_str(), sr.c_str(), FN_EVALUATE);
    Check(repeat == evaluated && g_targetCount.load() == 1, "repeated lookup recursed or allocated another slot");
    const auto created = UnityLookup(plugin.c_str(), sr.c_str(), FN_CREATE);
    Check(created == rawGetProc(GetModuleHandleW(L"nvngx_dlss.dll"), FN_CREATE), "CreateFeature changed");
    const auto fgEval = UnityLookup(plugin.c_str(), fg.c_str(), FN_EVALUATE);
    Check(fgEval == rawGetProc(GetModuleHandleW(L"nvngx_dlssg.dll"), FN_EVALUATE), "FG Evaluate must pass through");
    Check(FindPatched(GetModuleHandleW(L"nvngx_dlssg.dll")) == nullptr && g_targetCount.load() == 1, "FG was patched or took an SR slot");
    // Call through the wrapper to verify ABI forwarding; empty synthetic inputs
    // deliberately do not assert valid native guide pixels or GPU rendering.
    Check(NVSDK_NGX_SUCCEED(reinterpret_cast<EvaluateFeatureFn>(evaluated)(nullptr,nullptr,nullptr,nullptr)), "wrapper forwarding failed");
    Check(!IsInterestingModule(L"UnityPlayerHelper.dll") && !IsInterestingModule(L"unrelated-engine.dll"), "engine allowlist became too broad");
    puts("PASS startup UnityPlayer -> dynamic Streamline -> SR Evaluate, original ABI, repeated lookup, unchanged CreateFeature, FG exclusion, narrow allowlist");
    return 0;
} catch(const std::exception& e) { printf("FAIL %s\n", e.what()); return 1; }
