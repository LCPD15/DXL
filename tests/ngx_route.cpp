// Exercise the production NGX routing with misleading generic parameters.
#include "../src/core/NgxEavesdrop.cpp"
#include "NgxParameterBag.h"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include <future>
using namespace DXL;
using Microsoft::WRL::ComPtr;
static int behavior=0, nrCalls=0;
static uint32_t nativeSlot=0;
static NgxParameterBag nativeParams;
static ID3D12Resource* expectedOutput=nullptr;
static void Check(bool yes, const char* msg) { if(!yes) throw std::runtime_error(msg); }
#include "NgxIdentityCases.h"
static NVSDK_NGX_Result NVSDK_CONV Leaf(ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter*,PFN_NVSDK_NGX_ProgressCallback) { return NVSDK_NGX_Result_Success; }
static NVSDK_NGX_Result NVSDK_CONV Core(ID3D12GraphicsCommandList* l,const NVSDK_NGX_Handle* h,const NVSDK_NGX_Parameter*,PFN_NVSDK_NGX_ProgressCallback c) {
    if(behavior) ForwardConfirmedUpscaler(nativeSlot,l,h,&nativeParams,c);
    if(behavior==2) ForwardConfirmedUpscaler(nativeSlot,l,h,&nativeParams,c);
    return NVSDK_NGX_Result_Success;
}
static bool Nr(ID3D12GraphicsCommandList* list, const NgxEavesdropFrame& f, uint32_t, uint32_t, uint32_t) noexcept {
    if(f.output!=expectedOutput) return false;
    if(!EvaluateGpuGate::Get().Begin(list)) return false;
    ++nrCalls; return true;
}
int main() try {
    TestNgxIdentity();
    ComPtr<ID3D12Debug> debug; Check(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))),"debug"); debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory; Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))),"factory");
    ComPtr<IDXGIAdapter> adapter; factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter));
    ComPtr<ID3D12Device> device; Check(SUCCEEDED(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))),"device");
    ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC qd{}; device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue));
    ComPtr<ID3D12CommandAllocator> allocator; device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator));
    ComPtr<ID3D12GraphicsCommandList> list; device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)); list->Close();
    auto texture=[&] { D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{}; td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width=64; td.Height=64;
        td.DepthOrArraySize=td.MipLevels=1; td.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; td.SampleDesc.Count=1;
        ComPtr<ID3D12Resource> r; Check(SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&r))),"texture"); return r; };
    auto native=texture(), decoy=texture(); expectedOutput=native.Get();
    NgxParameterBag generic;
    for(auto* p : {&nativeParams,&generic}) {
        auto* r=p==&generic?decoy.Get():native.Get();
        for(auto* key : {NVSDK_NGX_Parameter_Color,NVSDK_NGX_Parameter_Depth,NVSDK_NGX_Parameter_MotionVectors,NVSDK_NGX_Parameter_Output}) p->Set(key,(void*)r);
        p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,64);
        p->Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height,64);
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors=1; hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> heap; device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap));
    D3D12_ROOT_SIGNATURE_DESC rd{}; ComPtr<ID3DBlob> blob,error; D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error);
    ComPtr<ID3D12RootSignature> root; device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root));
    auto& gate=EvaluateGpuGate::Get(); Check(gate.Initialize(device.Get()),"gate"); gate.SetSubmissionHookReady(true);
    ComPtr<ID3D12Fence> fence; device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence));
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr); UINT64 serial=0;
    // Slot 0 is the real cached SR fixture classified by HookedGetProcAddress.
    Check(g_realEvaluate[0].load() && g_nativeUpscalerTarget[0].load(), "cached SR route not initialized");
    // Preserve the second real cached leaf (RR) before slot 1 becomes the core.
    g_realEvaluate[2].store(g_realEvaluate[1].load()); g_nativeUpscalerTarget[2].store(true);
    g_realEvaluate[1].store((void*)&Core); g_nativeUpscalerTarget[1].store(false);
    CommandListTracker::rootsReady.store(true); NgxEavesdrop::Get().SetPreEvaluateHook(&Nr);
    auto run=[&](int kind,int count,int bindingMode=0) { behavior=kind;
        for(int i=0;i<count;++i) {
            allocator->Reset(); list->Reset(allocator.Get(),nullptr);
            auto& t=CommandListTracker::Get(); t.NoteReset(list.Get());
            if (bindingMode != 2 && bindingMode != 3) {
                ID3D12DescriptorHeap* heaps[]{heap.Get()}; list->SetDescriptorHeaps(1,heaps);
                t.NoteDescriptorHeaps(list.Get(),1,heaps);
                if (bindingMode == 4) { list->SetDescriptorHeaps(0,heaps); t.NoteDescriptorHeaps(list.Get(),0,nullptr); }
            }
            if (bindingMode != 1 && bindingMode != 3) { list->SetComputeRootSignature(root.Get()); t.NoteComputeRootSignature(list.Get(),root.Get()); }
            Check(NVSDK_NGX_SUCCEED(ForwardConfirmedUpscaler(1,list.Get(),nullptr,&generic,nullptr)),"forward result");
            Check(SUCCEEDED(list->Close()),"list validation"); ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1,lists); gate.Submitted(queue.Get(),1,lists);
            queue->Signal(fence.Get(),++serial); fence->SetEventOnCompletion(serial,event); Check(WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"GPU completion");
        }
    };
    run(0,90); Check(nrCalls==0 && NgxEavesdrop::Get().EvaluateSeen()==0,"generic keys misclassified as SR");
    run(1,90); Check(nrCalls>0,"nested real SR not processed with actual output"); const int before=nrCalls;
    run(0,10); run(2,10); Check(nrCalls==before,"other/ambiguous calls processed");
    // Reproduce a CPU frame reaching the NR hook while the previous GPU frame
    // is still queued. There must be an NR call on BOTH frames of every pair.
    ComPtr<ID3D12CommandAllocator> allocator2;
    Check(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator2))),"second allocator");
    ComPtr<ID3D12GraphicsCommandList> list2;
    Check(SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator2.Get(),nullptr,IID_PPV_ARGS(&list2))),"second list");
    list2->Close();
    ComPtr<ID3D12Fence> delay; device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&delay));
    auto record=[&](ID3D12CommandAllocator* a, ID3D12GraphicsCommandList* l) {
        Check(SUCCEEDED(a->Reset()) && SUCCEEDED(l->Reset(a,nullptr)),"delayed reset");
        ID3D12DescriptorHeap* heaps[]{heap.Get()}; l->SetDescriptorHeaps(1,heaps); l->SetComputeRootSignature(root.Get());
        auto& t=CommandListTracker::Get(); t.NoteReset(l); t.NoteDescriptorHeaps(l,1,heaps); t.NoteComputeRootSignature(l,root.Get());
        Check(NVSDK_NGX_SUCCEED(ForwardConfirmedUpscaler(1,l,nullptr,&generic,nullptr)),"delayed forward");
        Check(SUCCEEDED(l->Close()),"delayed close");
    };
    behavior=1;
    for(UINT64 i=1;i<=32;++i) {
        record(allocator.Get(),list.Get());
        Check(SUCCEEDED(queue->Wait(delay.Get(),i)),"queue delay");
        ID3D12CommandList* first[]{list.Get()}; queue->ExecuteCommandLists(1,first); gate.Submitted(queue.Get(),1,first);
        auto unblock=std::async(std::launch::async,[&,i] { Sleep(10); return delay->Signal(i); });
        record(allocator2.Get(),list2.Get()); // previous implementation skips NR here
        Check(SUCCEEDED(unblock.get()),"unblock GPU");
        ID3D12CommandList* second[]{list2.Get()}; queue->ExecuteCommandLists(1,second); gate.Submitted(queue.Get(),1,second);
        queue->Signal(fence.Get(),++serial); fence->SetEventOnCompletion(serial,event);
        Check(WaitForSingleObject(event,5000)==WAIT_OBJECT_0,"pair completion");
    }
    const auto stats=gate.Snapshot();
    Check(nrCalls==before+64,"alternating NR frame drops under GPU delay");
    Check(stats.waits>=32 && stats.timeouts==0 && stats.unsubmitted==0,"unexpected admission skips");
    // Wilds emits ordinary temporal resets during gameplay. A successful native
    // SR evaluation must still reach NR on that frame and all following frames.
    const int beforeReset=nrCalls;
    nativeParams.Set(NVSDK_NGX_Parameter_Reset,1);
    run(1,1); Check(nrCalls==beforeReset+1,"native SR reset incorrectly pauses NR");
    nativeParams.Set(NVSDK_NGX_Parameter_Reset,0);
    run(1,12); Check(nrCalls==beforeReset+13,"post-reset 60-frame NR blackout");
    nativeParams.Set(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width,32);
    run(1,12); Check(nrCalls==beforeReset+25,"dynamic render width incorrectly pauses NR");
    NgxEavesdrop::Get().SetEvaluateMode(true);
    while(NgxEavesdrop::Get().TakeResetFrame()) {}
    NgxEavesdrop::Get().NoteDepthChanged();
    Check(!NgxEavesdrop::Get().PauseActive() && !NgxEavesdrop::Get().TakeResetFrame(),"unrelated depth candidates reset/pause native guides");
    run(1,12); Check(nrCalls==beforeReset+37,"depth discovery interrupts confirmed native SR");
    NgxEavesdrop::Get().PauseCaptureFor(20);
    run(1,1); Check(nrCalls==beforeReset+37,"actual swapchain capture pause bypassed");
    // End the test pause explicitly; avoid depending on the coarse Windows
    // GetTickCount64 clock advancing by a particular amount during Sleep.
    NgxEavesdrop::Get().PauseCaptureFor(0);
    run(1,1); Check(nrCalls==beforeReset+38,"NR does not immediately resume after actual swapchain pause");
    puts("PASS native continuity: reset frame + 12 following frames, dynamic guides, unrelated depth burst; actual capture pause retained");
    const int beforeEmpty=nrCalls;
    run(1,12,true);
    Check(nrCalls==beforeEmpty+12,"known-empty compute state blocked native NR");
    Check(CommandListTracker::Get().Snapshot(list.Get()).computeRootSignature==nullptr,"empty state polluted");
    puts("PASS known-empty route: observed Reset + heaps only, 12/12 native NR calls");
    const int beforeHeapless = nrCalls;
    for (int i=0;i<24;++i) { run(1,1,0); run(1,1,2); }
    Check(nrCalls == beforeHeapless + 48, "alternating heap/root-only frames dropped NR");
    run(1,12,3); run(1,12,4);
    Check(nrCalls == beforeHeapless + 72, "known empty or explicitly unbound heap dropped NR");
    puts("PASS heapless continuity: 48/48 alternating bound/root-only frames plus 24/24 Reset-empty/explicit heap-unbind frames reach NR");
    // ZZZ retains its six-buffer chain after its FG option changes. The
    // conservative Present guard must not suppress confirmed SR -> NR work.
    // This uses the real cached .bin SR leaf and the production routing code;
    // Nr above is a callback fixture, not an actual frame-generation renderer.
    const int beforeFg = nrCalls;
    {
        FrameGenSwapChains::Creation retainedSixBuffers(6);
        Check(NgxEavesdrop::Get().FrameGenerationActive(0), "six-buffer FG guard missing");
        run(1,12);
        Check(nrCalls == beforeFg + 12, "Present FG guard blocked confirmed SR NR");
        NgxEavesdrop::Get().PauseCaptureFor(2000);
        run(1,1);
        Check(nrCalls == beforeFg + 12, "swapchain safety pause bypassed under FG");
        NgxEavesdrop::Get().PauseCaptureFor(0);
        run(1,12);
        Check(nrCalls == beforeFg + 24, "NR did not resume while six-buffer chain remained");
    }
    Check(!NgxEavesdrop::Get().FrameGenerationActive(0), "retired chain left Present guarded");
    run(1,12);
    Check(nrCalls == beforeFg + 36, "NR stopped after FG chain retirement");
    puts("PASS retained six-buffer FG guard: SR NR continues, real pause retained, 12/12 frames resume without chain recreation, chain retirement does not interrupt NR");
    // Switching SR/RR can select another confirmed native export without any
    // swapchain rebuild. An active primary still excludes competing upscalers;
    // an abandoned export must not lock the process out of NR forever.
    const int beforeSwitch = nrCalls;
    while (NgxEavesdrop::Get().TakeResetFrame()) {}
    nativeSlot = 2;
    run(1,1);
    Check(nrCalls == beforeSwitch, "active SR allowed a second upscaler to run NR");
    Sleep(450);
    run(1,12);
    Check(nrCalls == beforeSwitch + 12, "inactive SR export permanently blocked replacement RR");
    Check(NgxEavesdrop::Get().TakeResetFrame(), "SR/RR slot switch did not reset NR history");
    nativeSlot = 0;
    run(1,1);
    Check(nrCalls == beforeSwitch + 12, "old SR immediately stole an active RR slot");
    while (NgxEavesdrop::Get().TakeResetFrame()) {}
    Sleep(450);
    run(1,12);
    Check(nrCalls == beforeSwitch + 24, "RR to SR switch did not recover NR");
    Check(NgxEavesdrop::Get().TakeResetFrame(), "RR/SR slot switch did not reset NR history");
    puts("PASS native export handoff: active SR/RR exclusion, stopped SR -> RR -> SR recovery, history reset on both switches");
    ComPtr<ID3D12InfoQueue> info; device.As(&info);
    for(UINT64 i=0;i<info->GetNumStoredMessages();++i) { SIZE_T n=0; info->GetMessage(i,nullptr,&n); std::vector<unsigned char> data(n); auto* m=(D3D12_MESSAGE*)data.data(); info->GetMessage(i,m,&n);
        if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) { puts(m->pDescription); throw std::runtime_error("API error"); } }
    CloseHandle(event); printf("PASS production route: generic/FG-like keys rejected; nested real SR uses native output; multiple leaves rejected; 64/64 delayed frames processed, waits=%llu, skips=0; NR=%d; PIX markers validation clean\n",stats.waits,nrCalls);
    return 0;
} catch(const std::exception& e) { printf("FAIL %s\n",e.what()); return 1; }
