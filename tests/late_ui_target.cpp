// Load the complete core AFTER DXGI creation; exercise the production barrier
// and queue hooks. Toggle via IPC and inspect only this fixture's framebuffer.
#include "../src/common/IpcClient.h"
#include "../src/common/NrParameterEdit.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <filesystem>
#include <stdexcept>
#pragma comment(lib,"d3d12.lib")
#pragma comment(lib,"dxgi.lib")
#pragma comment(lib,"user32.lib")
using Microsoft::WRL::ComPtr;
static void HR(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D12 call failed"); }
static constexpr UINT W=960,H=640;
int wmain(int argc,wchar_t** argv) try {
    if(argc!=3)return 2;
    setvbuf(stdout,nullptr,_IONBF,0);
    SetErrorMode(SEM_FAILCRITICALERRORS|SEM_NOGPFAULTERRORBOX);
    WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"DXLLateUiFixture";
    RegisterClassW(&wc);
    HWND window=CreateWindowW(wc.lpszClassName,L"DXL late-injection regression",WS_OVERLAPPEDWINDOW,
        20,20,W,H,nullptr,nullptr,wc.hInstance,nullptr);
    if(!window)return 3;
    ComPtr<ID3D12Device> device;HR(D3D12CreateDevice(nullptr,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC qd{};
    HR(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
    ComPtr<IDXGIFactory4> factory;HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    DXGI_SWAP_CHAIN_DESC1 sd{};sd.Width=W;sd.Height=H;sd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count=1;sd.BufferCount=4;sd.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;sd.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
    ComPtr<IDXGISwapChain1> chain1;HR(factory->CreateSwapChainForHwnd(queue.Get(),window,&sd,nullptr,nullptr,&chain1));
    ComPtr<IDXGISwapChain3> chain;HR(chain1.As(&chain));chain1.Reset();
    ComPtr<ID3D12CommandAllocator> allocator;HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list;HR(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));HR(list->Close());
    ComPtr<ID3D12DescriptorHeap> heap;D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;
    HR(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));
    ComPtr<ID3D12Resource> readback;D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rd.Width=UINT64(W)*H*4;rd.Height=1;
    rd.DepthOrArraySize=rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HR(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
    ComPtr<ID3D12Fence> fence;HR(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);UINT64 serial=0;
    const auto drain=[&]{HR(queue->Signal(fence.Get(),++serial));HR(fence->SetEventOnCompletion(serial,event));if(WaitForSingleObject(event,5000)!=WAIT_OBJECT_0)throw std::runtime_error("GPU timeout");};
    const auto reset=[&]{HR(allocator->Reset());HR(list->Reset(allocator.Get(),nullptr));};
    const auto submit=[&]{HR(list->Close());ID3D12CommandList* batch[]={list.Get()};queue->ExecuteCommandLists(1,batch);drain();};
    const auto barrier=[&](ID3D12Resource* resource,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to){
        D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to};list->ResourceBarrier(1,&b);
    };
    wchar_t name[128]{};swprintf_s(name,L"Local\\DXL.Ready.%lu",GetCurrentProcessId());
    HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,name);
    if(!LoadLibraryW((std::filesystem::path(argv[1]).parent_path()/L"nvngx_dlssg.dll").c_str())) return 14;
    if(!LoadLibraryW(argv[1])||WaitForSingleObject(ready,15000)!=WAIT_OBJECT_0)return 4;
    CloseHandle(ready);ShowWindow(window,SW_SHOWNORMAL);ShowWindow(window,SW_SHOWNA);
    printf("Late-loaded complete core: chain=%p queue=%p\n",chain.Get(),queue.Get());
    unsigned base=~0u,opened=0,closed=~0u,frames=0;bool show=false,hide=false;
    const auto start=GetTickCount64();
    while(GetTickCount64()-start<18500){
        MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){TranslateMessage(&msg);DispatchMessageW(&msg);}
        const auto ms=GetTickCount64()-start;
        if(ms>=12500&&!show){
            uint32_t packed=0;
            if(!DXL::EncodeNrEdit("nrSkinStructure",0.37,packed) ||
                !DXL::SendCommand(GetCurrentProcessId(),DXL::Ipc::CommandId::EditNrParameter,packed)) return 11;
            if(DXL::SendCommand(GetCurrentProcessId(),DXL::Ipc::CommandId::EditNrParameter,0xffffffff)) return 12;
            show=DXL::SendCommand(GetCurrentProcessId(),DXL::Ipc::CommandId::TogglePerfWindow);if(!show)return 5;}
        if(ms>=15500&&!hide){hide=DXL::SendCommand(GetCurrentProcessId(),DXL::Ipc::CommandId::TogglePerfWindow);if(!hide)return 6;}
        ComPtr<ID3D12Resource> buffer;HR(chain->GetBuffer(chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&buffer)));
        reset();barrier(buffer.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET);
        const auto rtv=heap->GetCPUDescriptorHandleForHeapStart();device->CreateRenderTargetView(buffer.Get(),nullptr,rtv);
        const float color[]={12.f/255,24.f/255,40.f/255,1};list->ClearRenderTargetView(rtv,color,0,nullptr);
        barrier(buffer.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_PRESENT);submit();
        HR(chain->Present(0,0));drain();++frames;
        if((ms>=12000&&ms<12250&&base==~0u)||(ms>=14500&&ms<15000&&opened==0)||(ms>=17500&&closed==~0u)){
            reset();barrier(buffer.Get(),D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=buffer.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Footprint={sd.Format,W,H,1,W*4};list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
            barrier(buffer.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_PRESENT);submit();
            void* data=nullptr;HR(readback->Map(0,nullptr,&data));auto* p=static_cast<unsigned char*>(data);unsigned changed=0;
            for(UINT i=0;i<W*H;++i)if(abs(int(p[i*4])-12)>2||abs(int(p[i*4+1])-24)>2||abs(int(p[i*4+2])-40)>2)++changed;
            readback->Unmap(0,nullptr);
            if(ms<12500)base=changed;else if(ms<15500)opened=changed;else closed=changed;
            printf("sample ms=%llu changed=%u\n",ms,changed);
        }
        Sleep(16);
    }
    DXL::StatusView status;DXL::Ipc::Status result{};status.Open(GetCurrentProcessId());status.Read(result);
    if (abs(result.nrParamSkinStructure - 0.37f) > 0.001f || result.nrParamVersion == 0) return 13;
    printf("PASS launcher parameter IPC: skin=%.2f revision=%u; invalid edit rejected\n", result.nrParamSkinStructure,result.nrParamVersion);
    const bool pass=base==0&&opened>10000&&closed==0&&result.nrEvaluateCount==0&&show&&hide;
    printf("LATE_UI_RESULT passed=%u frames=%u base=%u open=%u closed=%u NR=%llu\n",pass,frames,base,opened,closed,result.nrEvaluateCount);
    drain();DestroyWindow(window);CloseHandle(event);return pass?0:1;
}catch(const std::exception& e){printf("FAIL %s\n",e.what());return 1;}
