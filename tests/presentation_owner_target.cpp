// Actual mixed-API startup: a tiny D3D11 helper presents before the D3D12 main window.
// Reuse the existing D3D12 rendering/debug fixture, but own the startup sequence.
#define wmain DxlUnusedFixtureMain
#include "../src/testapp/testapp.cpp"
#undef wmain
#include "../src/common/IpcClient.h"
#include "../src/common/StartupDiagnostics.h"
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdint>

using Microsoft::WRL::ComPtr;

namespace {
LRESULT CALLBACK HelperWindowProc(HWND window, UINT message, WPARAM w, LPARAM l) {
    return DefWindowProcW(window, message, w, l);
}

bool PumpMessages() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) return false;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    return true;
}

bool WindowProcBelongsTo(HWND window, HMODULE expected) {
    const auto proc = GetWindowLongPtrW(window, GWLP_WNDPROC);
    HMODULE owner = nullptr;
    return proc && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
        GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
        reinterpret_cast<LPCWSTR>(proc), &owner) && owner == expected;
}

struct Helper {
    HWND window = nullptr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<IDXGISwapChain> chain;
    ComPtr<ID3D11RenderTargetView> target;
    bool CreateAndPresent() {
        WNDCLASSW wc{};
        wc.lpfnWndProc = HelperWindowProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"DXLTinyPresentationHelper";
        if (!RegisterClassW(&wc)) return false;
        window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName,
            L"DXL synthetic helper", WS_POPUP, -32000, -32000, 186, 17,
            nullptr, nullptr, wc.hInstance, nullptr);
        if (!window) return false;
        ShowWindow(window, SW_SHOWNOACTIVATE);
        DXGI_SWAP_CHAIN_DESC desc{};
        desc.BufferDesc.Width = 186;
        desc.BufferDesc.Height = 17;
        desc.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        desc.BufferCount = 2;
        desc.OutputWindow = window;
        desc.Windowed = TRUE;
        desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE,
            nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
            &desc, &chain, &device, nullptr, &context);
        if (FAILED(hr)) return Fail("Helper D3D11CreateDeviceAndSwapChain", hr);
        ComPtr<ID3D11Texture2D> buffer;
        if (FAILED(hr = chain->GetBuffer(0, IID_PPV_ARGS(&buffer))) ||
            FAILED(hr = device->CreateRenderTargetView(buffer.Get(), nullptr, &target)))
            return Fail("Helper render target", hr);
        for (unsigned frame = 0; frame < 12; ++frame) {
            if (!PumpMessages()) return false;
            const float color[]{0.1f, 0.2f, 0.3f, 1.0f};
            context->ClearRenderTargetView(target.Get(), color);
            hr = chain->Present(0, 0);
            if (FAILED(hr)) return Fail("Helper Present", hr);
            Sleep(16);
        }
        context->Flush();
        printf("OWNER_HELPER hwnd=%p visible=%u size=186x17 presents=12\n", window, IsWindowVisible(window));
        return true;
    }
};
}

int wmain(int argc, wchar_t** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    if (argc != 2) return 3;
    // Enable before loading the core, whose discovery may create a temporary device.
    ComPtr<ID3D12Debug> debug;
    if (FAILED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)))) return 4;
    debug->EnableDebugLayer();
    g_useDebugLayer = true;
    g_syncInterval = 0;
    wchar_t readyName[128]{};
    swprintf_s(readyName, L"%s.%lu", DXL::Ipc::READY_EVENT_BASE, GetCurrentProcessId());
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, readyName);
    if (!ready) return 5;
    HMODULE core = LoadLibraryW(argv[1]);
    const DWORD wait = core ? WaitForSingleObject(ready, 15000) : WAIT_FAILED;
    CloseHandle(ready);
    printf("OWNER_CORE loaded=%u ready=%u pid=%lu\n", core != nullptr, wait == WAIT_OBJECT_0, GetCurrentProcessId());
    DXL::StartupDiagnostics::Snapshot startup{};
    DWORD startupError = 0;
    const bool haveStartup = DXL::StartupDiagnostics::Read(GetCurrentProcessId(), startup, startupError);
    printf("OWNER_STARTUP available=%u flags=0x%08X worker=%u error=%u\n", haveStartup,
        startup.milestones, startup.workerThread, startupError);
    if (!core || wait != WAIT_OBJECT_0) {
        // Preserve only this synthetic target briefly for a startup thread dump.
        printf("OWNER_STARTUP_FAILED holdMs=30000 pid=%lu\n", GetCurrentProcessId());
        Sleep(30000);
        return 6;
    }

    Helper helper;
    if (!helper.CreateAndPresent()) return 7;
    DXL::StatusView view;
    DXL::Ipc::Status initial{};
    if (view.Open(GetCurrentProcessId())) view.Read(initial);
    printf("OWNER_AFTER_HELPER api=%u hooked=%u nr=%llu helperUi=%u\n", initial.api, initial.hooked,
        static_cast<unsigned long long>(initial.nrEvaluateCount), WindowProcBelongsTo(helper.window, core));

    WNDCLASSW wc{};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DXLPresentationOwnerMain";
    if (!RegisterClassW(&wc)) return 8;
    // Off-screen and non-activating: real visible HWNDs without stealing focus.
    g_window = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName,
        L"DXL D3D12 owner regression", WS_POPUP, -32000, -32000, 960, 540,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_window) return 9;
    ShowWindow(g_window, SW_SHOWNOACTIVATE);
    if (!InitDevice() || !g_infoQueue || !InitSwapChain()) return 10;
    g_pendingResize = false;
    printf("OWNER_MAIN hwnd=%p visible=%u size=%ux%u\n", g_window, IsWindowVisible(g_window), g_width, g_height);

    DXL::Ipc::Status last{};
    const auto started = GetTickCount64();
    unsigned frames = 0, stable = 0;
    bool passed = false;
    while (GetTickCount64() - started < 14000) {
        if (!PumpMessages()) return 11;
        RenderFrame(frames++);
        if (FAILED(g_device->GetDeviceRemovedReason())) return 12;
        if (!view.IsOpen()) view.Open(GetCurrentProcessId());
        if (view.Read(last)) {
            const bool correct = last.api == static_cast<uint32_t>(DXL::Ipc::GraphicsApi::D3D12) &&
                last.hooked && last.nrEvaluateCount >= 60 && last.nrEvaluateFailures == 0 &&
                frames >= 120 && WindowProcBelongsTo(g_window, core) &&
                !WindowProcBelongsTo(helper.window, core);
            stable = correct ? stable + 1 : 0;
            if (stable >= 8) { passed = true; break; }
        }
        Sleep(8);
    }
    WaitForGpu();
    DrainDebugMessages();
    printf("OWNER_STATUS passed=%u api=%u hooked=%u route=%u frames=%u presents=%llu nr=%llu failures=%llu mainUi=%u helperUi=%u\n",
        passed, last.api, last.hooked, last.nrRoute, frames,
        static_cast<unsigned long long>(last.presentCount), static_cast<unsigned long long>(last.nrEvaluateCount),
        static_cast<unsigned long long>(last.nrEvaluateFailures), WindowProcBelongsTo(g_window, core),
        WindowProcBelongsTo(helper.window, core));
    printf("OWNER_DEBUG enabled=%u\n", g_useDebugLayer && g_infoQueue != nullptr);
    // Let the existing core WM_NCDESTROY/process-detach cleanup run. Keep all
    // device resources alive until then, just like the existing E2E target.
    DestroyWindow(g_window);
    g_window = nullptr;
    DestroyWindow(helper.window);
    helper.window = nullptr;
    return passed ? 0 : 2;
}
