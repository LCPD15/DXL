// Reuse the real D3D12 fixture; add only deterministic lifetime and IPC checks.
#define wmain DxlOriginalFixtureMain
#include "../src/testapp/testapp.cpp"
#undef wmain
#include "../src/common/IpcClient.h"
#include <atomic>
#include <thread>

int wmain(int argc, wchar_t** argv) {
    std::atomic<bool> finished{false};
    std::atomic<bool> passed{false};
    const DWORD fixtureThread = GetCurrentThreadId();
    const auto ownWindow = [fixtureThread]() {
        HWND result = nullptr;
        EnumThreadWindows(fixtureThread, [](HWND window, LPARAM resultPtr) -> BOOL {
            *reinterpret_cast<HWND*>(resultPtr) = window;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&result));
        return result;
    };
    std::thread monitor([&] {
        const ULONGLONG started = GetTickCount64();
        DXL::StatusView view;
        DXL::Ipc::Status last{};
        unsigned validSnapshots = 0;
        bool hidden = false;
        while (!finished.load() && GetTickCount64() - started < 30000) {
            if (!hidden) {
                if (const HWND window = ownWindow()) { ShowWindow(window, SW_HIDE); hidden = true; }
            }
            if (!view.IsOpen()) view.Open(GetCurrentProcessId());
            if (view.Read(last)) {
                if (last.hooked && last.nrRoute == 1 && last.nrMotionSource == 2 &&
                    last.nrEvaluateCount >= 60 && last.nrEvaluateFailures == 0 && last.nrAtEvaluateFrames == 0 &&
                    last.nrParamTrueLayers == 2 && last.nrParamSelfLayers > 1.36f && last.nrParamSelfLayers < 1.38f)
                    ++validSnapshots;
                else validSnapshots = 0;
                if (validSnapshots >= 5) { passed = true; break; }
            }
            Sleep(200);
        }
        printf("E2E_STATUS passed=%u pid=%lu hooked=%u route=%u motion=%u presents=%llu nr=%llu failures=%llu evaluate=%llu self=%.3f true=%d opticalMs=%.3f early=%u\n",
            passed.load() ? 1u : 0u, GetCurrentProcessId(), last.hooked, last.nrRoute, last.nrMotionSource,
            (unsigned long long)last.presentCount, (unsigned long long)last.nrEvaluateCount,
            (unsigned long long)last.nrEvaluateFailures, (unsigned long long)last.nrAtEvaluateFrames,
            last.nrParamSelfLayers, last.nrParamTrueLayers, last.nrOpticalFlowMs, last.injectedEarly);
        printf("E2E_SR_RUNTIME loaded=%u\n", GetModuleHandleW(L"nvngx_dlss.dll") ? 1u : 0u);
        if (const HWND window = ownWindow()) PostMessageW(window, WM_CLOSE, 0, 0);
    });
    const int result = DxlOriginalFixtureMain(argc, argv);
    finished = true;
    monitor.join();
    printf("E2E_DEBUG enabled=%u\n", g_useDebugLayer && g_infoQueue ? 1u : 0u);
    if (result != 0) return result;
    return passed.load() ? 0 : 2;
}
