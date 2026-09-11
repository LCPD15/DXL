#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <string>
#include "SegMaskFilter.h"
using Microsoft::WRL::ComPtr;
using namespace DXL;
void Check(bool b, const char* m) { if (!b) throw std::runtime_error(m); }
void HR(HRESULT h) { Check(SUCCEEDED(h), "D3D12 API failed"); }
namespace DXL {
struct SegMaskTestAccess {
    static void VerifyInput(SegMaskFilter& f, bool flipY) {
        Check(f._frameFlipY == flipY, "captured orientation was overwritten while busy");
        const uint32_t contentH = uint32_t(float(f._frameHeight)*640.0f/std::max(f._frameWidth,f._frameHeight)+.5f);
        const float top = f._hInput[0], bottom = f._hInput[size_t(contentH-1)*640];
        Check(std::abs(top-(flipY?.2f:.8f)) < .006f && std::abs(bottom-(flipY?.8f:.2f)) < .006f,
            "actual CHW model input has wrong vertical orientation");
    }
    static bool Busy(SegMaskFilter& f) { return f._copyInFlight[0].load(); }
    static bool Failed(SegMaskFilter& f) { return f._trtFailed.load(std::memory_order_acquire); }
    static bool Lean(SegMaskFilter& f) { return f._leanRuntime; } // read after IsReady acquire
    static bool AdeReady(SegMaskFilter& f) { return f._adeReady; } // read after published snapshot acquire
    static bool Released(SegMaskFilter& f) {
        return !f._nvinfer && !f._runtime && !f._engine && !f._context &&
            !f._engine2 && !f._context2 && !f._cuCtx && !f._cuStream && !f._dInput;
    }
    static void Unpack(SegMaskFilter& f) {
        uint8_t out[4]{};
        uint32_t bgra = 0xFF2468A0u;
        f.UnpackFrameToRgba(reinterpret_cast<const uint8_t*>(&bgra), 4, 1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, out);
        Check(out[0] == 0x24 && out[1] == 0x68 && out[2] == 0xA0, "BGRA channel order");
        uint16_t half[]{0x3C00, 0x3800, 0, 0x3C00};
        f.UnpackFrameToRgba(reinterpret_cast<const uint8_t*>(half), 8, 1, 1, DXGI_FORMAT_R16G16B16A16_FLOAT, out);
        Check(out[0] == 255 && out[1] == 128 && out[2] == 0, "float RGB decode");
        uint32_t packed = (15u<<6) | ((14u<<6)<<11);
        f.UnpackFrameToRgba(reinterpret_cast<const uint8_t*>(&packed),4,1,1,DXGI_FORMAT_R11G11B10_FLOAT,out);
        Check(out[0]==255 && out[1]==128 && out[2]==0,"R11G11B10 float RGB decode");
    }
};
}
int main(int argc, char** argv) try {
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string expected = argc > 1 ? argv[1] : "";
    const bool expectFailure = argc > 2 && std::string(argv[2]) == "fail";
    const bool verifyRuntime = expected == "lean" || expected == "full";
    ComPtr<ID3D12Debug> debug; HR(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory; HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i=0;;++i) { ComPtr<IDXGIAdapter1> a; if (FAILED(factory->EnumAdapters1(i,&a))) break;
        DXGI_ADAPTER_DESC1 desc{}; a->GetDesc1(&desc); if(desc.VendorId==0x10DE) { adapter=a; break; } }
    Check(adapter.Get()!=nullptr, "NVIDIA adapter required");
    ComPtr<ID3D12Device> device; HR(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> info; HR(device.As(&info));
    ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC qd{}; HR(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator; HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list; HR(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))); HR(list->Close());
    ComPtr<ID3D12Fence> fence; HR(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))); uint64_t serial=0;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    auto flush=[&] { HR(queue->Signal(fence.Get(),++serial)); HR(fence->SetEventOnCompletion(serial,event)); Check(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"GPU wait"); };
    auto texture=[&](uint32_t w,uint32_t h) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width=w; desc.Height=h;
        desc.DepthOrArraySize=1; desc.MipLevels=1; desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM; desc.SampleDesc.Count=1;
        desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> r; HR(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&r))); return r;
    };
    SegMaskFilter filter; SegMaskTestAccess::Unpack(filter);
    struct FilterCleanup { SegMaskFilter& filter; ~FilterCleanup() { filter.TeardownForExit(); } } cleanup{filter};
    filter.SetSemanticGroups(verifyRuntime ? 0x3FFFFu : 1u);
    if (verifyRuntime) filter.SetActive(false);
    Check(filter.Initialize(device.Get(),queue.Get(),GetModuleHandleW(nullptr)),"segmentation init");
    filter.SetDebugView(false);
    if (verifyRuntime) {
        Sleep(100);
        Check(!filter.IsReady() && !GetModuleHandleW(L"nvinfer_11.dll") &&
            !GetModuleHandleW(L"nvinfer_lean_11.dll"), "disabled semantic worker loaded a runtime");
        filter.SetActive(true);
        puts("PASS disabled semantic worker performs no runtime load");
    }
    const auto deadline=GetTickCount64()+30000;
    while(!filter.IsReady() && !SegMaskTestAccess::Failed(filter) && GetTickCount64()<deadline) Sleep(10);
    if (expectFailure) {
        Check(!filter.IsReady() && SegMaskTestAccess::Failed(filter), "invalid Lean package must fail deterministically");
        Check(!GetModuleHandleW(L"nvinfer_11.dll"), "Lean failure silently loaded Full runtime");
        filter.TeardownForExit();
        Check(SegMaskTestAccess::Released(filter), "failed setup retained runtime resources");
        puts("PASS invalid Lean runtime/plan fails without loading Full, teardown complete");
        CloseHandle(event);
        return 0;
    }
    if(!filter.IsReady()) wprintf(L"TRT failure: %s\n",filter.LastError());
    Check(filter.IsReady(),"actual TensorRT model did not load");
    if (verifyRuntime) {
        const bool lean = expected == "lean";
        Check(SegMaskTestAccess::Lean(filter) == lean, "selected wrong runtime backend");
        Check(GetModuleHandleW(lean ? L"nvinfer_lean_11.dll" : L"nvinfer_11.dll") != nullptr, "selected runtime module not loaded");
        Check(GetModuleHandleW(lean ? L"nvinfer_11.dll" : L"nvinfer_lean_11.dll") == nullptr, "other runtime unexpectedly loaded");
        printf("PASS explicit %s backend; other runtime module absent\n", expected.c_str());
    }
    printf("PASS actual TensorRT initialized on matching NVIDIA adapter\n");
    SemanticMaskSnapshot snapshot;
    auto capture=[&](uint32_t w,uint32_t h,bool discard,bool flipY=false) {
        while(SegMaskTestAccess::Busy(filter)) Sleep(2);
        auto color=texture(w,h);
        ComPtr<ID3D12DescriptorHeap> heap; D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors=1;
        HR(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));
        device->CreateRenderTargetView(color.Get(),nullptr,heap->GetCPUDescriptorHandleForHeapStart());
        HR(allocator->Reset()); HR(list->Reset(allocator.Get(),nullptr));
        const float rgb[]{.2f,.4f,.6f,1}; list->ClearRenderTargetView(heap->GetCPUDescriptorHandleForHeapStart(),rgb,0,nullptr);
        const float topRgb[]{.8f,.1f,.3f,1}; const D3D12_RECT topRect{0,0,LONG(w),LONG(h/2)};
        list->ClearRenderTargetView(heap->GetCPUDescriptorHandleForHeapStart(),topRgb,1,&topRect);
        bool recorded=false; for(int i=0;i<200 && !recorded;++i) { recorded=filter.RecordFrame(list.Get(),color.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,flipY); if(!recorded) Sleep(2); }
        Check(recorded,"capture did not become ready");
        Check(!filter.RecordFrame(list.Get(),color.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,!flipY),"second capture reused busy buffer");
        HR(list->Close());
        if(discard) { HR(allocator->Reset()); HR(list->Reset(allocator.Get(),nullptr)); filter.NotifyReset(list.Get()); HR(list->Close()); Check(!SegMaskTestAccess::Busy(filter),"discarded capture stayed busy"); return; }
        const auto previous=snapshot.version;
        ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1,lists); filter.NotifySubmitted(queue.Get(),1,lists); flush();
        const auto end=GetTickCount64()+5000;
        while(snapshot.version==previous && GetTickCount64()<end) { filter.CopyLatest(snapshot); Sleep(2); }
        Check(snapshot.version>previous && snapshot.width==w && snapshot.height==h,"inference snapshot mismatch");
        Check(snapshot.Valid(GetTickCount64()),"published snapshot invalid");
        Check(snapshot.sourceFlipY==flipY,"source orientation missing from snapshot");
        SegMaskTestAccess::VerifyInput(filter,flipY);
        for(auto group:snapshot.groupIds) Check(group==255 || group<SEM_GROUP_COUNT,"invalid model group ID");
        printf("PASS actual capture/inference %ux%u version=%llu %.2fms\n",w,h,(unsigned long long)snapshot.version,filter.LastInferenceMs());
    };
    capture(128,72,true); capture(128,72,false); capture(257,145,false); capture(64,64,false); capture(128,72,false,true); capture(128,72,false,false);
    if (verifyRuntime) Check(!SegMaskTestAccess::AdeReady(filter), "single-model mode must not load ADE even with legacy scene bits enabled");
    filter.SetActive(false); const uint64_t frozen=snapshot.version; Sleep(550); filter.CopyLatest(snapshot);
    Check(snapshot.version==frozen && !snapshot.Valid(GetTickCount64()),"off or stale snapshot contract");
    filter.SetActive(true); capture(128,72,false);
    filter.TeardownForExit(); flush();
    Check(SegMaskTestAccess::Released(filter), "TRT/CUDA resources not released at shutdown");
    for(UINT64 i=0;i<info->GetNumStoredMessages();++i) {
        SIZE_T size=0; info->GetMessage(i,nullptr,&size); std::vector<uint8_t> b(size);
        auto* m=reinterpret_cast<D3D12_MESSAGE*>(b.data()); info->GetMessage(i,m,&size);
        if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) { puts(m->pDescription); Check(false,"debug layer error"); }
    }
    CloseHandle(event); puts("PASS semantic capture reset/resize/off-on/stale + real TensorRT, no D3D12 errors"); return 0;
} catch(const std::exception& e) { printf("FAIL %s\n",e.what()); return 1; }
