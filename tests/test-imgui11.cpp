// Production backend regression: real D3D11 WARP swapchains, complete context
// restoration, conditional-render immunity, buffer rotation, resize and rebind.
#include <d3d11_1.h>
#include <d3d11sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include <vector>
#include "../src/core/ReUiBackend.cpp"
using Microsoft::WRL::ComPtr;
static void Check(bool ok, const char* text) { if (!ok) throw std::runtime_error(text); }
static void HR(HRESULT hr) { if (FAILED(hr)) { printf("HRESULT %08X\n", unsigned(hr)); throw std::runtime_error("D3D11 call"); } }
constexpr UINT ProbeMessage = WM_APP + 17;
static WNDPROC previousProc = nullptr;
static unsigned messages = 0;
static LRESULT CALLBACK GameProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == ProbeMessage) { ++messages; return 0x1234; }
    return DefWindowProcW(h, m, w, l);
}
static LRESULT CALLBACK ForeignProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return CallWindowProcW(previousProc, h, m, w, l);
}
#include "ui-cursor-regression.h"
int main() try {
    TestCursorFallback();
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_error_mode(_OUT_TO_STDERR);
    setvbuf(stdout, nullptr, _IONBF, 0);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL level{};
    HR(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr,
        D3D11_CREATE_DEVICE_DEBUG, nullptr, 0, D3D11_SDK_VERSION,
        &device, &level, &context));
    ComPtr<ID3D11InfoQueue> info; HR(device.As(&info));
    ComPtr<IDXGIDevice> dxgi; HR(device.As(&dxgi));
    ComPtr<IDXGIAdapter> adapter; HR(dxgi->GetAdapter(&adapter));
    ComPtr<IDXGIFactory2> factory; HR(adapter->GetParent(IID_PPV_ARGS(&factory)));
    WNDCLASSW wc{}; wc.lpfnWndProc = GameProc;
    wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = L"DXL.Imgui11Regression";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"DXL ImGui11 regression",
        WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr, nullptr, wc.hInstance, nullptr);
    Check(hwnd != nullptr, "window creation");

    ComPtr<ID3DBlob> code;
    constexpr char shader[] = "[numthreads(1,1,1)] void main(uint3 id:SV_DispatchThreadID) {}";
    HR(D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr,
        "main", "cs_5_0", 0, 0, &code, nullptr));
    ComPtr<ID3D11ComputeShader> compute;
    HR(device->CreateComputeShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &compute));
    D3D11_TEXTURE2D_DESC td{};
    td.Width = td.Height = 64; td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1;
    td.BindFlags = D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> gameTarget; HR(device->CreateTexture2D(&td, nullptr, &gameTarget));
    ComPtr<ID3D11RenderTargetView> gameRtv; HR(device->CreateRenderTargetView(gameTarget.Get(), nullptr, &gameRtv));
    D3D11_QUERY_DESC qd{ D3D11_QUERY_OCCLUSION_PREDICATE, 0 };
    ComPtr<ID3D11Predicate> predicate; HR(device->CreatePredicate(&qd, &predicate));
    context->Begin(predicate.Get()); context->End(predicate.Get());
    context->Flush();
    BOOL visible = TRUE;
    const ULONGLONG started = GetTickCount64();
    while (context->GetData(predicate.Get(), &visible, sizeof(visible), 0) == S_FALSE)
        Check(GetTickCount64() - started < 5000, "predicate completion");
    Check(!visible, "empty query must reject ordinary game draws");

    ImGuiContext* stableContext = nullptr;
    ImFont* stableFont = nullptr;
    unsigned frames = 0;
    constexpr DXGI_FORMAT formats[] = { DXGI_FORMAT_R8G8B8A8_UNORM,
        DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM };
    for (unsigned segment = 0; segment < 3; ++segment) {
        DXGI_SWAP_CHAIN_DESC1 sd{};
        sd.Width = 256 + segment * 8; sd.Height = 192;
        sd.Format = formats[segment]; sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = segment + 2; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> chain1;
        HR(factory->CreateSwapChainForHwnd(device.Get(), hwnd, &sd, nullptr, nullptr, &chain1));
        ComPtr<IDXGISwapChain3> chain; HR(chain1.As(&chain)); chain1.Reset();
        Check(ReUi::InitOnce11(device.Get(), context.Get(), hwnd, sd.Format), "init D3D11 renderer");
        if (!stableContext) { stableContext = g.imgui; stableFont = g.uiFont; }
        Check(g.imgui == stableContext && g.uiFont == stableFont, "format change rebuilt input/context");
        if (segment == 0) {
            previousProc = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd, GWLP_WNDPROC,
                reinterpret_cast<LONG_PTR>(&ForeignProc)));
            Check(previousProc != nullptr, "foreign subclass installation");
            std::atomic<bool> open{true};
            ReUi::SetUiOpen(&open);
            SendMessageW(hwnd, WM_MOUSEWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
            SendMessageW(hwnd, WM_MOUSEHWHEEL, MAKEWPARAM(0, WHEEL_DELTA), 0);
            Check(g.wheelY.load() == WHEEL_DELTA && g.wheelX.load() == -WHEEL_DELTA,
                "scroll deltas swallowed before ImGui");
            ReUi::SetUiOpen(nullptr);
        }
        for (unsigned frame = 0; frame < 10; ++frame) {
            if (frame == 5) {
                context->ClearState(); context->Flush();
                Check(ReUi::WaitIdle(), "pre-resize idle");
                HR(chain->ResizeBuffers(sd.BufferCount, sd.Width + 8, sd.Height, sd.Format, 0));
            }
            Check(ReUi::InitOnce11(device.Get(), context.Get(), hwnd, sd.Format), "repeat init");
            Check(GetWindowLongPtrW(hwnd, GWLP_WNDPROC) == reinterpret_cast<LONG_PTR>(&ForeignProc),
                "renderer replaced foreign subclass");
            const unsigned before = messages;
            Check(SendMessageW(hwnd, ProbeMessage, 0, 0) == 0x1234 && messages == before + 1,
                "window chain broke or repeated");
            ComPtr<ID3D11Texture2D> bb;
            HR(chain->GetBuffer(chain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb)));
            ComPtr<ID3D11RenderTargetView> clearView;
            HR(device->CreateRenderTargetView(bb.Get(), nullptr, &clearView));
            const float black[] = { 0, 0, 0, 1 };
            context->ClearRenderTargetView(clearView.Get(), black);
            clearView.Reset();
            auto* target = gameRtv.Get();
            context->OMSetRenderTargets(1, &target, nullptr);
            context->CSSetShader(compute.Get(), nullptr, 0);
            context->SetPredication(predicate.Get(), TRUE);
            const D3D11_VIEWPORT viewport{ 2, 3, 40, 50, .2f, .8f };
            context->RSSetViewports(1, &viewport);
            context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_POINTLIST);

            ReUi::FramePresent(chain.Get(), [] {
                ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), ImVec2(64, 64), IM_COL32(255, 0, 0, 255));
                ImGui::GetBackgroundDrawList()->AddText(g.uiFont, kUiFontPx,
                    ImVec2(80, 0), IM_COL32_WHITE, "DXL NR");
            });
            ComPtr<ID3D11ComputeShader> actualCompute;
            context->CSGetShader(&actualCompute, nullptr, nullptr);
            Check(actualCompute.Get() == compute.Get(), "game compute shader lost");
            ComPtr<ID3D11RenderTargetView> actualTarget;
            context->OMGetRenderTargets(1, &actualTarget, nullptr);
            Check(actualTarget.Get() == gameRtv.Get(), "game render target lost");
            ComPtr<ID3D11Predicate> actualPredicate; BOOL actualCondition = FALSE;
            context->GetPredication(&actualPredicate, &actualCondition);
            Check(actualPredicate.Get() == predicate.Get() && actualCondition, "game predication lost");
            D3D11_VIEWPORT actualViewport{}; UINT count = 1;
            context->RSGetViewports(&count, &actualViewport);
            Check(count == 1 && memcmp(&viewport, &actualViewport, sizeof(viewport)) == 0, "game viewport lost");
            D3D11_PRIMITIVE_TOPOLOGY topology{};
            context->IAGetPrimitiveTopology(&topology);
            Check(topology == D3D11_PRIMITIVE_TOPOLOGY_POINTLIST, "game topology lost");
            context->ClearState();
            D3D11_TEXTURE2D_DESC readDesc{};
            readDesc.Width = 128; readDesc.Height = 40;
            readDesc.MipLevels = readDesc.ArraySize = 1; readDesc.SampleDesc.Count = 1;
            readDesc.Format = sd.Format; readDesc.Usage = D3D11_USAGE_STAGING;
            readDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            ComPtr<ID3D11Texture2D> staging; HR(device->CreateTexture2D(&readDesc, nullptr, &staging));
            D3D11_BOX rect{ 0, 0, 0, 128, 40, 1 };
            context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, bb.Get(), 0, &rect);
            D3D11_MAPPED_SUBRESOURCE mapped{};
            HR(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            const auto* pixels = static_cast<const unsigned char*>(mapped.pData);
            const UINT pixel = *reinterpret_cast<const UINT*>(pixels + 16 * mapped.RowPitch + 16 * 4);
            const UINT expected = sd.Format == DXGI_FORMAT_B8G8R8A8_UNORM ? 0x00ff0000
                : sd.Format == DXGI_FORMAT_R10G10B10A2_UNORM ? 1023 : 255;
            Check((pixel & expected) == expected, "ImGui pixel missing or conditional-rendered away");
            unsigned textPixels = 0;
            for (unsigned y = 0; y < 40; ++y) for (unsigned x = 80; x < 128; ++x)
                if (*reinterpret_cast<const UINT*>(pixels + y * mapped.RowPitch + x * 4) & 0x00ffffff) ++textPixels;
            context->Unmap(staging.Get(), 0);
            Check(textPixels > 10, "font texture missing after rebind");
            bb.Reset(); HR(chain->Present(0, 0)); ++frames;
        }
        context->ClearState(); context->Flush(); chain.Reset();
        printf("D3D11 segment %u passed: format %u buffers %u resize/state/pixels\n", segment, unsigned(sd.Format), sd.BufferCount);
    }
    ReUi::Shutdown();
    Check(SendMessageW(hwnd, ProbeMessage, 0, 0) == 0x1234, "shutdown broke foreign subclass");
    DestroyWindow(hwnd);
    Check(inputHooks.empty(), "destroyed HWND retained");
    unsigned errors = 0;
    for (UINT64 i = 0; i < info->GetNumStoredMessages(); ++i) {
        SIZE_T bytes = 0; info->GetMessage(i, nullptr, &bytes);
        std::vector<unsigned char> storage(bytes);
        auto* m = reinterpret_cast<D3D11_MESSAGE*>(storage.data());
        HR(info->GetMessage(i, m, &bytes));
        if (m->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) {
            printf("D3D11 ERROR: %s\n", m->pDescription); ++errors;
        }
    }
    Check(errors == 0, "D3D11 validation errors");
    printf("PASS: ImGui D3D11 %u frames, pipeline restore, rotation, resize, format rebind, input chain; 0 D3D11 errors\n", frames);
    return 0;
} catch (const std::exception& e) { printf("FAIL: %s\n", e.what()); return 1; }
