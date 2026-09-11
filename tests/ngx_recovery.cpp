// Exercise real production export recovery with pointers cached before hooks,
// and an EXE loader IAT deliberately restored after early installation.
#include "../src/core/NgxEavesdrop.cpp"
#include <filesystem>
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <thread>
using namespace DXL;
static void Check(bool yes, const char* message) { if (!yes) throw std::runtime_error(message); }
struct Fixture {
    HMODULE module{};
    EvaluateFeatureFn cached{};
    LONG (*calls)(unsigned){};
    const void* (*lastHandle)(){};
    unsigned (*vulkan)(unsigned){};
    FARPROC create{};
    unsigned char vkCode[16]{}, createCode[16]{}, evalCode[16]{};
};
static Fixture LoadFixture(const std::filesystem::path& path, GetProcAddressFn lookup) {
    Fixture f;
    f.module = LoadLibraryW(path.c_str()); Check(f.module != nullptr, "load fixture");
    f.cached = reinterpret_cast<EvaluateFeatureFn>(lookup(f.module, FN_EVALUATE));
    f.calls = reinterpret_cast<LONG(*)(unsigned)>(lookup(f.module, "FixtureCalls"));
    f.lastHandle = reinterpret_cast<const void*(*)()>(lookup(f.module, "FixtureLastHandle"));
    f.vulkan = reinterpret_cast<unsigned(*)(unsigned)>(lookup(f.module, "NVSDK_NGX_VULKAN_EvaluateFeature"));
    f.create = lookup(f.module, FN_CREATE);
    Check(f.cached && f.calls && f.lastHandle && f.vulkan && f.create, "fixture exports");
    memcpy(f.evalCode, reinterpret_cast<void*>(f.cached), 16);
    memcpy(f.vkCode, reinterpret_cast<void*>(f.vulkan), 16);
    memcpy(f.createCode, reinterpret_cast<void*>(f.create), 16);
    return f;
}
static uint64_t Entered() {
    uint64_t total=0; for (auto& count : g_enteredEvaluate) total += count.load(); return total;
}
static void Call(const Fixture& f, EvaluateFeatureFn target, bool intercepted) {
    const auto before = Entered(); const auto nativeBefore = f.calls(0);
    const auto handle = reinterpret_cast<const NVSDK_NGX_Handle*>(uintptr_t(0x724621));
    Check(NVSDK_NGX_SUCCEED(target(nullptr, handle, nullptr, nullptr)), "original NGX result changed");
    Check(f.calls(0) == nativeBefore + 1 && f.lastHandle() == handle, "original call count/ABI changed");
    Check(Entered() == before + (intercepted ? 1 : 0), "missing or duplicate wrapper invocation");
}
int main(int argc, char** argv) try {
    const bool disableRecovery=argc > 1 && strcmp(argv[1],"--negative-no-recovery")==0;
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr, executable, _countof(executable));
    const auto folder=std::filesystem::path(executable).parent_path();
    const auto rawLookup = &GetProcAddress;
    auto sr = LoadFixture(folder / L"nvngx_dlss.dll", rawLookup);
    auto unknown = LoadFixture(folder / L"unrelated.dll", rawLookup);
    auto fg = LoadFixture(folder / L"nvngx_dlssg.dll", rawLookup);
    const auto forwarded = LoadLibraryW((folder / L"forwarded/nvngx_dlssd.dll").c_str());
    Check(forwarded != nullptr && rawLookup(forwarded,FN_EVALUATE)==reinterpret_cast<FARPROC>(fg.cached), "forwarded FG fixture");
    // Simulate both an existing cached address and a later IAT wrapper.
    Call(sr, sr.cached, false);
    NgxEavesdrop::Get().Install(GetModuleHandleW(nullptr), true);
    const auto wrapped = reinterpret_cast<EvaluateFeatureFn>(HookedGetProcAddress(sr.module, FN_EVALUATE));
    Call(sr, wrapped, true); Call(sr, sr.cached, false);
    const auto initialSlots=g_targetCount.load();
    if (!disableRecovery) NgxEavesdrop::Get().PollKnownUpscalerExports();
    Check(g_targetCount.load() == initialSlots, "IAT/export recovery allocated duplicate slots");
    Call(sr, sr.cached, true); Call(sr, wrapped, true);
    const auto repeated = reinterpret_cast<EvaluateFeatureFn>(HookedGetProcAddress(sr.module, FN_EVALUATE));
    Check(repeated == wrapped, "new GPA result differs from existing slot"); Call(sr, repeated, true);
    const auto trampoline = g_realEvaluate[0].load();
    uint32_t trampolineSlot=MAX_TARGETS;
    Check(WrapperForTarget(trampoline, true, &trampolineSlot)==reinterpret_cast<void*>(wrapped) && trampolineSlot==0,
        "trampoline identity allocated duplicate wrapper");
    // Restore just the synthetic EXE loader slots; its next SR/RR .bin loads
    // bypass the original chain exactly as cached/manual loaders do.
    const auto* owner=FindPatched(GetModuleHandleW(nullptr)); Check(owner, "EXE loader hooks");
    if (owner->loadLibraryW) Iat::WriteSlot(Iat::FindSlot(GetModuleHandleW(nullptr),"LoadLibraryW"),reinterpret_cast<void*>(owner->loadLibraryW),nullptr);
    if (owner->getProcAddress) Iat::WriteSlot(Iat::FindSlot(GetModuleHandleW(nullptr),"GetProcAddress"),reinterpret_cast<void*>(owner->getProcAddress),nullptr);
    auto rr = LoadFixture(folder / L"NVIDIA/NGX/models/dlssd/versions/1/files/model.bin", rawLookup);
    auto cachedSr = LoadFixture(folder / L"NVIDIA/NGX/models/dlss/versions/1/files/model.bin", rawLookup);
    auto cachedFg = LoadFixture(folder / L"NVIDIA/NGX/models/dlssg/versions/1/files/model.bin", rawLookup);
    Call(rr, rr.cached, false); Call(cachedSr, cachedSr.cached, false);
    const auto priorCalls=rr.calls(0);
    std::atomic<bool> stopCalls{false}, resultChanged{false};
    std::atomic<unsigned> concurrentCalls{0};
    std::thread caller([&] {
        while (!stopCalls.load()) {
            for (unsigned i=0;i<50;++i) {
                if (!NVSDK_NGX_SUCCEED(rr.cached(nullptr,nullptr,nullptr,nullptr))) resultChanged.store(true);
                ++concurrentCalls;
            }
            Sleep(1);
        }
    });
    Sleep(1010); NgxEavesdrop::Get().PollKnownUpscalerExports();
    Sleep(20); stopCalls.store(true); caller.join();
    Check(concurrentCalls.load()>100 && !resultChanged.load() && rr.calls(0)==priorCalls+LONG(concurrentCalls.load()),
        "concurrent cached calls changed result or invoked original more than once");
    Call(rr, rr.cached, true); Call(cachedSr, cachedSr.cached, true);
    Check(g_targetCount.load()==3, "unknown or FG consumed an Evaluate slot");
    for (const auto* f : {&fg,&cachedFg,&unknown}) {
        Check(memcmp(f->evalCode,reinterpret_cast<void*>(f->cached),16)==0,"excluded export modified");
        Call(*f,f->cached,false);
    }
    Call(fg,reinterpret_cast<EvaluateFeatureFn>(rawLookup(forwarded,FN_EVALUATE)),false);
    for (const auto* f : {&sr,&rr,&cachedSr,&fg,&cachedFg,&unknown}) {
        Check(memcmp(f->vkCode,reinterpret_cast<void*>(f->vulkan),16)==0,"Vulkan export modified");
        Check(memcmp(f->createCode,reinterpret_cast<void*>(f->create),16)==0,"CreateFeature export modified");
        const auto before=Entered();
        Check(f->vulkan(0x5149)==(0x5149 ^ 0x7351a9b2u) && f->calls(2)==1,"Vulkan ABI changed");
        using Create = NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*,NVSDK_NGX_Feature,const NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**);
        Check(NVSDK_NGX_SUCCEED(reinterpret_cast<Create>(f->create)(nullptr,NVSDK_NGX_Feature_SuperSampling,nullptr,nullptr)) && f->calls(1)==1,"CreateFeature ABI changed");
        Check(Entered()==before,"non-D3D12-Evaluate invoked wrapper");
    }
    // A game releasing its own reference must not invalidate our trampoline.
    Check(FreeLibrary(sr.module)!=0,"release game module reference"); Call(sr,sr.cached,true);
    const auto hooks=NgxEavesdrop::Get().AttachedEvaluateHooks();
    RequestTeardown(); Sleep(1010); NgxEavesdrop::Get().PollKnownUpscalerExports();
    Check(NgxEavesdrop::Get().AttachedEvaluateHooks()==hooks,"recovery continued during teardown");
    Call(sr,sr.cached,true); Call(rr,rr.cached,true);
    puts("PASS late cached SR, shared IAT/export/trampoline identity, restored loader IAT, concurrent cached SR/RR .bin calls, FG/forwarded-FG/unknown/Vulkan/Create exclusion, retained module, teardown forwarding");
    return 0;
} catch(const std::exception& e) { printf("FAIL %s\n",e.what()); return 1; }
