// Real DXGI presentation with an independent overlay's repeated hook discovery.
// The overlay surrogate deliberately models a retained native detour plus a
// refreshed forwarding method, the topology found in RDR2's exhausted stack.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include "MinHook.h"
using Microsoft::WRL::ComPtr;
using Present1 = HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain1*,UINT,UINT,const DXGI_PRESENT_PARAMETERS*);
Present1 nativePresent1 = nullptr, overlayNext = nullptr;
void* discoveredNative = nullptr;
unsigned nativeReturns = 0, ordinaryReturns = 0;
thread_local unsigned overlayDepth = 0;
void Check(bool value, const char* what) { if (!value) { printf("FAIL %s\n",what); ExitProcess(2); } }
void Hr(HRESULT result,const char* what) { if (FAILED(result)) { printf("FAIL %s hr=%08x\n",what,unsigned(result));ExitProcess(3); } }
HRESULT STDMETHODCALLTYPE OverlayPresent1(IDXGISwapChain1* chain,UINT interval,UINT flags,const DXGI_PRESENT_PARAMETERS* params) {
    if (++overlayDepth > 32) {
        printf("FAIL bounded overlay forwarding recursion depth=%u nativeReturns=%u\n",overlayDepth,nativeReturns);
        ExitProcess(33);
    }
    const bool native = overlayNext == nativePresent1;
    const HRESULT result = overlayNext(chain,interval,flags,params);
    if (native && SUCCEEDED(result)) ++nativeReturns;
    --overlayDepth;
    return result;
}
void DiscoverOverlay(IDXGISwapChain1* chain) {
    void* current=(*reinterpret_cast<void***>(chain))[22];
    printf("OVERLAY discover native=%p current=%p\n",discoveredNative,current);
    // The native entry remains intercepted. A new table entry may be another
    // overlay and is adopted as the next layer, exactly where a table rewrite
    // can close a forwarding cycle back through the native entry.
    if(current!=discoveredNative)overlayNext=reinterpret_cast<Present1>(current);
}
ComPtr<ID3D12Device> device;
ComPtr<ID3D12CommandQueue> queue;
ComPtr<IDXGIFactory4> factory;
ComPtr<IDXGISwapChain3> chain;
ComPtr<ID3D12CommandAllocator> allocator;
ComPtr<ID3D12GraphicsCommandList> commands;
ComPtr<ID3D12DescriptorHeap> rtv;
ComPtr<ID3D12Fence> fence;
HANDLE fenceEvent=nullptr;UINT64 fenceValue=0;HWND window=nullptr;
void Drain(){Hr(queue->Signal(fence.Get(),++fenceValue),"Signal");Hr(fence->SetEventOnCompletion(fenceValue,fenceEvent),"Fence event");Check(WaitForSingleObject(fenceEvent,5000)==WAIT_OBJECT_0,"bounded GPU fence");}
void CreateWindowAndChain(){
    window=CreateWindowW(L"STATIC",L"DXL presentation hook chain fixture",WS_OVERLAPPEDWINDOW,100,100,400,260,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    Check(window!=nullptr,"window");ShowWindow(window,SW_SHOWNOACTIVATE);
    DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=320;desc.Height=180;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> sc;Hr(factory->CreateSwapChainForHwnd(queue.Get(),window,&desc,nullptr,nullptr,&sc),"CreateSwapChainForHwnd");Hr(sc.As(&chain),"Swapchain3");
}
void Draw(unsigned frame,bool usePresent1=true){
    MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}
    ComPtr<ID3D12Resource> image;Hr(chain->GetBuffer(chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&image)),"GetBuffer");
    Hr(allocator->Reset(),"Reset allocator");Hr(commands->Reset(allocator.Get(),nullptr),"Reset list");
    auto handle=rtv->GetCPUDescriptorHandleForHeapStart();device->CreateRenderTargetView(image.Get(),nullptr,handle);
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=image.Get();b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore=D3D12_RESOURCE_STATE_PRESENT;b.Transition.StateAfter=D3D12_RESOURCE_STATE_RENDER_TARGET;commands->ResourceBarrier(1,&b);
    const float color[4]={0.2f,float(frame%64)/64.0f,0.5f,1};commands->ClearRenderTargetView(handle,color,0,nullptr);
    b.Transition.StateBefore=D3D12_RESOURCE_STATE_RENDER_TARGET;b.Transition.StateAfter=D3D12_RESOURCE_STATE_PRESENT;commands->ResourceBarrier(1,&b);Hr(commands->Close(),"Close list");
    ID3D12CommandList* lists[]={commands.Get()};queue->ExecuteCommandLists(1,lists);Drain();
    if(usePresent1){DXGI_PRESENT_PARAMETERS params{};Hr(chain->Present1(0,0,&params),"Present1");}
    else {Hr(chain->Present(0,0),"Present");++ordinaryReturns;}
}
int wmain(int argc,wchar_t**argv){
    setvbuf(stdout,nullptr,_IONBF,0);printf("HOOK_CHAIN pid=%lu\n",GetCurrentProcessId());Check(argc==2 || argc==3,"core path argument");
    const bool coreFirst=argc==3 && wcscmp(argv[2],L"--core-first")==0;
    Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"Factory");Hr(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"Device");
    D3D12_COMMAND_QUEUE_DESC q{};Hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"Queue");
    Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"Allocator");Hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&commands)),"List");Hr(commands->Close(),"Initial close");
    D3D12_DESCRIPTOR_HEAP_DESC h{};h.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;h.NumDescriptors=1;Hr(device->CreateDescriptorHeap(&h,IID_PPV_ARGS(&rtv)),"RTV heap");Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Fence");fenceEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);Check(fenceEvent!=nullptr,"fence event");
    CreateWindowAndChain();
    if(coreFirst){Check(LoadLibraryW(argv[1])!=nullptr,"core load before overlay");Sleep(3000);}
    discoveredNative=(*reinterpret_cast<void***>(chain.Get()))[22];
    Check(MH_Initialize()==MH_OK,"overlay MinHook initialize");Check(MH_CreateHook(discoveredNative,reinterpret_cast<void*>(&OverlayPresent1),reinterpret_cast<void**>(&nativePresent1))==MH_OK,"overlay native hook");overlayNext=nativePresent1;Check(MH_EnableHook(discoveredNative)==MH_OK,"overlay hook enabled");
    for(unsigned i=0;i<12;++i)Draw(i);
    if(!coreFirst){Check(LoadLibraryW(argv[1])!=nullptr,"core load after overlay");Sleep(3000);}
    for(unsigned i=0;i<12;++i)Draw(i,false);
    Drain();chain.Reset();DestroyWindow(window);CreateWindowAndChain();DiscoverOverlay(chain.Get());
    for(unsigned i=0;i<120;++i)Draw(i);
    Drain();Hr(chain->ResizeBuffers(2,400,240,DXGI_FORMAT_UNKNOWN,0),"ResizeBuffers");DiscoverOverlay(chain.Get());
    for(unsigned i=0;i<60;++i)Draw(i);
    Drain();chain.Reset();DestroyWindow(window);Sleep(4500);CreateWindowAndChain();DiscoverOverlay(chain.Get());
    for(unsigned i=0;i<120;++i)Draw(i);
    Drain();chain.Reset();DestroyWindow(window);
    // Count only calls which really reached the captured native trampoline,
    // not successful HRESULTs invented by a recursive wrapper bailout.
    printf("HOOK_CHAIN nativePresent1=%u ordinaryPresent=%u GPUfences=%llu\n",nativeReturns,ordinaryReturns,fenceValue);
    Check(nativeReturns==312 && ordinaryReturns==12,"all real presentations reached native implementation");
    puts("PASS stable Present/Present1 overlay chain, resize, destroyed-window replacement and GPU completion");return 0;
}
