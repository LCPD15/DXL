// Exercise the production ImGui renderer against actual DXGI chains on WARP.
// Changes queue, buffer count, size and RTV format without restarting the UI.
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <vector>
#include "../src/core/ReUiBackend.cpp"
using Microsoft::WRL::ComPtr;
static void Check(bool b, const char* s) { if (!b) throw std::runtime_error(s); }
static void HR(HRESULT hr) { if (FAILED(hr)) { printf("HRESULT %08X\n", unsigned(hr)); throw std::runtime_error("D3D call"); } }
// REFramework-style subclass: retain the previous callback and forward to it.
// The ceiling makes the old production bug fail this test instead of exhausting
// the test process's stack. Production code must never reach that ceiling.
constexpr UINT kProbeMessage = WM_APP + 17;
constexpr UINT kNestedMessage = WM_APP + 18;
constexpr LRESULT kProbeResult = 0x1234;
static WNDPROC overlayPrevious = nullptr;
static unsigned gameMessages = 0, destroyedWindows = 0;
static bool overlayCycle = false;
static unsigned ansiDestroyed = 0;
static LRESULT CALLBACK AnsiGameProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == kProbeMessage) return 0x5678;
    if (m == WM_NCDESTROY) ++ansiDestroyed;
    return DefWindowProcA(h,m,w,l);
}
static LRESULT CALLBACK GameProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == kProbeMessage) { ++gameMessages; return kProbeResult; }
    if (m == kNestedMessage) return SendMessageW(h,kProbeMessage,w,l);
    if (m == WM_NCDESTROY) ++destroyedWindows;
    return DefWindowProcW(h,m,w,l);
}
static LRESULT CALLBACK OverlayProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    static thread_local unsigned depth = 0;
    if (depth >= 16) { overlayCycle = true; return 0; }
    ++depth;
    const LRESULT result = CallWindowProcW(overlayPrevious,h,m,w,l);
    --depth;
    return result;
}
static void CheckMessages(HWND hwnd) {
    const unsigned before = gameMessages;
    Check(SendMessageW(hwnd,kProbeMessage,0,0)==kProbeResult,"window message lost or subclass cycle");
    Check(SendMessageW(hwnd,kNestedMessage,1,2)==kProbeResult,"legitimate nested window message lost");
    Check(!overlayCycle && gameMessages==before+2,"window message forwarded more than once");
}
#include "ui-cursor-regression.h"
int main() try {
    TestCursorFallback();
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    SetUnhandledExceptionFilter([](EXCEPTION_POINTERS* e)->LONG { printf("UI EXCEPTION %08X at %p\n", e->ExceptionRecord->ExceptionCode,e->ExceptionRecord->ExceptionAddress); fflush(stdout); return EXCEPTION_EXECUTE_HANDLER; });
    _set_error_mode(_OUT_TO_STDERR); setvbuf(stdout,nullptr,_IONBF,0);
    puts("Starting ImGui DXGI regression");
    ComPtr<ID3D12Debug> debug; HR(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory; HR(CreateDXGIFactory2(DXGI_CREATE_FACTORY_DEBUG,IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> adapter; HR(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)));
    ComPtr<ID3D12Device> device; HR(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> info; HR(device.As(&info));
    ComPtr<ID3D12InfoQueue1> liveInfo;
    if (SUCCEEDED(device.As(&liveInfo))) {
        DWORD cookie=0;
        HR(liveInfo->RegisterMessageCallback([](D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
            D3D12_MESSAGE_ID id, LPCSTR text, void*) {
            if (severity<=D3D12_MESSAGE_SEVERITY_ERROR) printf("D3D12 error %u: %s\n",unsigned(id),text);
        },D3D12_MESSAGE_CALLBACK_FLAG_NONE,nullptr,&cookie));
    }
    ComPtr<ID3D12CommandQueue> queues[2]; D3D12_COMMAND_QUEUE_DESC qd{};
    for (auto& q : queues) HR(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)));
    WNDCLASSW wc{}; wc.lpfnWndProc=GameProc; wc.hInstance=GetModuleHandleW(nullptr); wc.lpszClassName=L"NRFG.UiRegression";
    RegisterClassW(&wc);
    HWND hwnd=CreateWindowW(wc.lpszClassName,L"NRFG UI regression",WS_OVERLAPPEDWINDOW,0,0,320,240,nullptr,nullptr,wc.hInstance,nullptr);
    Check(hwnd!=nullptr,"window creation");
    ComPtr<ID3D12CommandAllocator> alloc; HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)));
    ComPtr<ID3D12GraphicsCommandList> list; HR(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&list))); HR(list->Close());
    ComPtr<ID3D12DescriptorHeap> rtv; D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors=1;
    HR(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv)));
    ComPtr<ID3D12Fence> fence; HR(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))); UINT64 serial=0;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    auto submit=[&](ID3D12CommandQueue* q) {
        HR(list->Close()); ID3D12CommandList* lists[]{list.Get()}; q->ExecuteCommandLists(1,lists);
        HR(q->Signal(fence.Get(),++serial)); HR(fence->SetEventOnCompletion(serial,event));
        Check(WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"test queue timeout");
    };
    auto barrier=[&](ID3D12Resource* r,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
        D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after}; list->ResourceBarrier(1,&b);
    };
    D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{}; rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; rd.Width=256; rd.Height=1;
    rd.Width=512+512*40;
    rd.DepthOrArraySize=rd.MipLevels=1; rd.SampleDesc.Count=1; rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> pixels; HR(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&pixels)));
    unsigned verified=0, textVerified=0;
    ImGuiContext* context=nullptr;
    ImFont* font=nullptr;
    void* cursorTrampoline=nullptr;
    for (unsigned segment=0;segment<6;++segment) {
        auto* q=queues[segment%2].Get();
        DXGI_SWAP_CHAIN_DESC1 sd{}; sd.Width=256+segment*8; sd.Height=192;
        sd.Format=segment%2?DXGI_FORMAT_R10G10B10A2_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.SampleDesc.Count=1; sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount=segment%2?4:3; sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> sc1; HR(factory->CreateSwapChainForHwnd(q,hwnd,&sd,nullptr,nullptr,&sc1));
        ComPtr<IDXGISwapChain3> sc; HR(sc1.As(&sc)); sc1.Reset();
        for (unsigned frame=0;frame<10;++frame) {
            if (frame==5) {
                HR(alloc->Reset()); HR(list->Reset(alloc.Get(),nullptr)); HR(list->Close());
                Check(ReUi::WaitIdle(),"pre-resize UI drain"); HR(sc->ResizeBuffers(sd.BufferCount,sd.Width+8,sd.Height,sd.Format,0));
            }
            ComPtr<ID3D12Resource> bb; HR(sc->GetBuffer(sc->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&bb)));
            HR(alloc->Reset()); HR(list->Reset(alloc.Get(),nullptr));
            const float black[]{0,0,0,1}; auto view=rtv->GetCPUDescriptorHandleForHeapStart();
            device->CreateRenderTargetView(bb.Get(),nullptr,view);
            barrier(bb.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
            list->ClearRenderTargetView(view,black,0,nullptr);
            barrier(bb.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT); submit(q);
            if (frame==0) printf("Initializing segment %u\n",segment);
            // The game owns the HWND on a different thread from Present.
            bool initialized=false;
            std::thread renderThread([&]{ initialized=ReUi::InitOnce(device.Get(),q,hwnd,sd.Format); });
            renderThread.join();
            Check(initialized,"ImGui queue/format initialization");
            if (!context) { context=g.imgui; font=g.uiFont; cursorTrampoline=reinterpret_cast<void*>(o_GetCursorPos); }
            Check(g.imgui==context && g.uiFont==font && reinterpret_cast<void*>(o_GetCursorPos)==cursorTrampoline,"renderer rebind replaced context, font or cursor hooks");
            CheckMessages(hwnd);
            if (segment) Check(GetWindowLongPtrW(hwnd,GWLP_WNDPROC)==reinterpret_cast<LONG_PTR>(&OverlayProc),"UI replaced a foreign subclass on queue change");
            ReUi::FramePresent(sc.Get(),[]{
                ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0,0),ImVec2(64,64),IM_COL32(255,0,0,255));
                ImGui::GetBackgroundDrawList()->AddText(g.uiFont,kUiFontPx,ImVec2(80,0),IM_COL32_WHITE,"NR + FG");
            });
            Check(ReUi::WaitIdle() && g.pendingBuffer==nullptr && g.queue==q,"old UI GPU work or stale queue");
            HR(alloc->Reset()); HR(list->Reset(alloc.Get(),nullptr));
            barrier(bb.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=bb.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=pixels.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Footprint={sd.Format,1,1,1,256}; D3D12_BOX box{16,16,0,17,17,1};
            list->CopyTextureRegion(&dst,0,0,0,&src,&box);
            dst.PlacedFootprint.Offset=512;
            dst.PlacedFootprint.Footprint={sd.Format,128,40,1,512}; box={80,0,0,208,40,1};
            list->CopyTextureRegion(&dst,0,0,0,&src,&box);
            barrier(bb.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PRESENT); submit(q);
            void* data=nullptr; HR(pixels->Map(0,nullptr,&data)); const UINT value=*static_cast<UINT*>(data);
            unsigned textPixels=0;
            const auto* text=reinterpret_cast<const UINT*>(static_cast<const char*>(data)+512);
            for (unsigned i=0;i<128*40;++i) if (text[i] & (segment%2?0x3fffffff:0x00ffffff)) ++textPixels;
            pixels->Unmap(0,nullptr);
            Check((value & (segment%2?1023:255))==(segment%2?1023:255),"UI pixel missing"); ++verified;
            Check(textPixels>20,"font texture missing after queue change"); ++textVerified;
            bb.Reset(); HR(sc->Present(0,0));
            // Present itself enqueues work. The fixture must drain that work
            // before its immediate resize/destroy, just as a game is required to.
            HR(q->Signal(fence.Get(),++serial)); HR(fence->SetEventOnCompletion(serial,event));
            Check(WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"DXGI present drain");
        }
        HR(alloc->Reset()); HR(list->Reset(alloc.Get(),nullptr)); HR(list->Close());
        Check(ReUi::WaitIdle(),"chain destruction drain"); sc.Reset();
        printf("UI segment %u: queue %u, buffers %u, format %u, resize + pixel checks passed\n",segment,segment%2,sd.BufferCount,unsigned(sd.Format));
        if (segment==0) {
            overlayPrevious=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(hwnd,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(&OverlayProc)));
            Check(overlayPrevious!=nullptr,"foreign subclass installation");
            CheckMessages(hwnd);
        }
    }
    ReUi::Shutdown(); CheckMessages(hwnd);
    // A different HWND must have its own forwarding edge and A/W variant.
    WNDCLASSA ansiClass{}; ansiClass.lpfnWndProc=AnsiGameProc; ansiClass.hInstance=wc.hInstance; ansiClass.lpszClassName="NRFG.UiAnsi";
    RegisterClassA(&ansiClass);
    HWND ansi=CreateWindowA(ansiClass.lpszClassName,"ANSI",WS_OVERLAPPEDWINDOW,0,0,100,100,nullptr,nullptr,wc.hInstance,nullptr);
    Check(ansi && !IsWindowUnicode(ansi),"ANSI window creation");
    EnsureInputHook(ansi);
    Check(SendMessageA(ansi,kProbeMessage,0,0)==0x5678,"ANSI forwarding");
    EnsureInputHook(hwnd); CheckMessages(hwnd);
    Check(GetWindowLongPtrA(ansi,GWLP_WNDPROC)==reinterpret_cast<LONG_PTR>(&AnsiGameProc),"old window subclass not detached");
    Check(SendMessageA(ansi,kProbeMessage,0,0)==0x5678,"wrong original procedure after window change");
    EnsureInputHook(ansi); DestroyWindow(ansi);
    Check(ansiDestroyed==1,"ANSI WM_NCDESTROY lost");
    CheckMessages(hwnd); DestroyWindow(hwnd);
    Check(destroyedWindows==1,"WM_NCDESTROY not forwarded to game");
    Check(inputHooks.empty(),"destroyed HWND forwarding records retained");
    // Even a malformed foreign callback below us must not defeat the guard.
    HWND cycle=CreateWindowW(wc.lpszClassName,L"Cycle",WS_OVERLAPPEDWINDOW,0,0,100,100,nullptr,nullptr,wc.hInstance,nullptr);
    Check(cycle!=nullptr,"cycle window creation");
    overlayPrevious=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(cycle,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(&OverlayProc)));
    const auto originalGame=overlayPrevious;
    EnsureInputHook(cycle);
    overlayPrevious=&UiWndProc; // deliberately introduce a foreign -> UI loop
    Check(SendMessageW(cycle,kProbeMessage,0,0)==0 && !overlayCycle,"cycle guard recursed through a foreign callback");
    overlayPrevious=originalGame;
    CheckMessages(cycle); DestroyWindow(cycle);
    Check(destroyedWindows==2 && inputHooks.empty(),"window teardown lost forwarding or retained records");
    CloseHandle(event);
    unsigned errors=0;
    for (UINT64 i=0;i<info->GetNumStoredMessages();++i) {
        SIZE_T bytes=0; info->GetMessage(i,nullptr,&bytes); std::vector<char> buffer(bytes);
        auto* m=reinterpret_cast<D3D12_MESSAGE*>(buffer.data()); HR(info->GetMessage(i,m,&bytes));
        if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) { puts(m->pDescription); ++errors; }
    }
    Check(!errors,"D3D validation errors");
    printf("PASS ImGui: %u backbuffer and %u font checks, 6 queue/format/chain changes, foreign subclass + nested messages preserved, API errors=0\n",verified,textVerified);
    return 0;
} catch (const std::exception& e) { printf("FAIL UI: %s\n",e.what()); return 1; }
