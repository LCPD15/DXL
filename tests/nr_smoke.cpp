// Runs the actual signed NR snippet on an isolated NVIDIA device. No game injection.
// --typeless reproduces v0.41's guide format; validation errors abort before submission.
// --typeless-color reproduces Unity's RGBA8 typeless SR output; --rgba8-color is its typed control.
// --heapless-bindings tests real NR with observed Reset, no descriptor heaps,
// and alternating empty/bound compute signatures on the Evaluate path.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <vector>
#include "DlssNrFilter.h"
#include "CommandListTracker.h"
#include "NgxGuideFormat.h"
#include "EvaluateGpuGate.h"
#include <nvsdk_ngx_helpers.h>
using Microsoft::WRL::ComPtr;
using namespace DXL;
void Check(bool b, const char* message) { if (!b) throw std::runtime_error(message); }
void HR(HRESULT h) { if (FAILED(h)) { printf("HRESULT=%08X\n", unsigned(h)); throw std::runtime_error("D3D12 failed"); } }
namespace DXL {
// Observe the production chain at the SDK boundary, then call the actual model.
// Also inject one creation failure without exhausting the user's GPU memory.
struct NrLayerTestAccess {
    using CreateFn=NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*,NVSDK_NGX_Feature,NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**);
    using EvalFn=NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter*,PFN_NVSDK_NGX_ProgressCallback);
    static inline CreateFn realCreate=nullptr;
    static inline EvalFn realEval=nullptr;
    static inline DlssNrFilter* filter=nullptr;
    static inline int createCalls=0, failAt=0, stage=0;
    static inline uint64_t checkedCalls=0;
    static inline ID3D12Resource* lastOutput=nullptr;
    static inline const NVSDK_NGX_Handle* handles[5]{};
    static inline const NVSDK_NGX_Parameter* bags[5]{};
    static NVSDK_NGX_Result NVSDK_CONV Create(ID3D12GraphicsCommandList* l,NVSDK_NGX_Feature f,NVSDK_NGX_Parameter* p,NVSDK_NGX_Handle** h) {
        if(++createCalls==failAt) return NVSDK_NGX_Result_Fail;
        return realCreate(l,f,p,h);
    }
    static NVSDK_NGX_Result NVSDK_CONV Eval(ID3D12GraphicsCommandList* l,const NVSDK_NGX_Handle* h,const NVSDK_NGX_Parameter* p,PFN_NVSDK_NGX_ProgressCallback cb) {
        ID3D12Resource *input=nullptr,*output=nullptr;
        Check(NVSDK_NGX_SUCCEED(p->Get("DLSSNR.Color",&input)),"missing layer input");
        Check(NVSDK_NGX_SUCCEED(p->Get("DLSSNR.Output",&output)),"missing layer output");
        Check(input && output && input!=output,"NR stage aliases its input/output");
        if(stage) Check(input==lastOutput,"next layer must consume previous model output");
        for(int i=0;i<stage;++i) Check(h!=handles[i] && p!=bags[i],"NR layers share temporal feature or parameter bag");
        if(stage) {
            for(const char* key:{"DLSSNR.MVec","DLSSNR.Depth"}) {
                ID3D12Resource *a=nullptr,*b=nullptr;
                Check(NVSDK_NGX_SUCCEED(bags[0]->Get(key,&a)) && NVSDK_NGX_SUCCEED(p->Get(key,&b)) && a==b,"guides differ between layers");
            }
            for(const char* key:{"DLSSNR.Reset","DLSSNR.MVecSubrectWidth","DLSSNR.MVecSubrectHeight","DLSSNR.DepthSubrectWidth","DLSSNR.DepthSubrectHeight"}) {
                unsigned a=0,b=0;
                Check(NVSDK_NGX_SUCCEED(bags[0]->Get(key,&a)) && NVSDK_NGX_SUCCEED(p->Get(key,&b)) && a==b,"reset or guide subrect differs between layers");
            }
        }
        handles[stage]=h; bags[stage]=p; lastOutput=output;
        ++checkedCalls;
        const auto result=realEval(l,h,p,cb);
        if(++stage==filter->LiveLayerCount()) {
            Check(output==filter->_output,"chain must end in final output");
            stage=0;
        }
        return result;
    }
    static void Install(DlssNrFilter& nr,int fail) {
        // Production graphics initialization is now independent of NR. Load
        // the optional runtime before wrapping its actual SDK entry points.
        Check(nr.LoadSnippet(nr._selfModule),"test NR runtime load");
        filter=&nr; failAt=fail;
        realCreate=reinterpret_cast<CreateFn>(nr._createFeature);
        realEval=reinterpret_cast<EvalFn>(nr._evaluateFeature);
        nr._createFeature=reinterpret_cast<void*>(&Create);
        nr._evaluateFeature=reinterpret_cast<void*>(&Eval);
    }
    static DXGI_FORMAT WorkingColorFormat(const DlssNrFilter& nr) { return nr._colorFormat; }
    static void InjectFailure(DlssNrFilter& nr) { nr.Fail("isolated regression: transient NR failure"); }
    static void CheckRecovered(const DlssNrFilter& nr) {
        Check(nr._failureCount==0 && !nr._disabled,"successful NR frame did not reset consecutive failure count");
    }
    static void CheckTerminalStreak(DlssNrFilter& nr) {
        for(int i=1;i<=8;++i) {
            InjectFailure(nr);
            Check(nr._failureCount==uint64_t(i),"failure streak count mismatch");
            Check(nr._disabled==(i==8),"NR failure cutoff must remain eight consecutive failures");
        }
        const auto calls=nr.ModelCallCount();
        Check(!nr.Execute({},{}),"disabled NR accepted another frame");
        Check(nr.ModelCallCount()==calls,"disabled NR invoked the model");
    }
};
}
void* Patch(void* object, size_t i, void* hook) noexcept {
    auto** v = *reinterpret_cast<void***>(object); DWORD old;
    if (!VirtualProtect(v+i, sizeof(void*), PAGE_READWRITE, &old)) return nullptr;
    void* real=v[i]; v[i]=hook; VirtualProtect(v+i, sizeof(void*), old, &old); return real;
}
void Barrier(ID3D12GraphicsCommandList* l, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER t{}; t.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b}; l->ResourceBarrier(1,&t);
}
unsigned Errors(ID3D12InfoQueue* info) {
    unsigned errors=0;
    for (UINT64 i=0; i<info->GetNumStoredMessages(); ++i) {
        SIZE_T n=0; info->GetMessage(i,nullptr,&n); std::vector<unsigned char> bytes(n);
        auto* m=reinterpret_cast<D3D12_MESSAGE*>(bytes.data()); info->GetMessage(i,m,&n);
        if (m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) { ++errors; puts(m->pDescription); }
    }
    info->ClearStoredMessages(); return errors;
}
int main(int argc, char** argv) try {
    setvbuf(stdout,nullptr,_IONBF,0);
    bool typeless=false, large=false, resetEveryFrame=false,present=false,directGuides=false,withSr=false,fenceWait=false,switchRoutes=false;
    bool emptyCompute=false, switchLayers=false, typelessColor=false, rgba8Color=false, heaplessBindings=false, failureStreak=false, grading=false;
    float selfLayers=1.0f;
    int trueLayers=1, failLayer=0;
    bool noGuides=false,switchMotion=false,noOptical=false,switchQuality=false,debugMotion=false,selectedZero=false;
    for(int i=1;i<argc;++i) {
        typelessColor |= !strcmp(argv[i],"--typeless-color");
        rgba8Color |= !strcmp(argv[i],"--rgba8-color");
        heaplessBindings |= !strcmp(argv[i],"--heapless-bindings");
        failureStreak |= !strcmp(argv[i],"--failure-streak");
        grading |= !strcmp(argv[i],"--grading");
        if(!strncmp(argv[i],"--self-layers=",14)) selfLayers=float(atof(argv[i]+14));
        if(!strncmp(argv[i],"--true-layers=",14)) trueLayers=atoi(argv[i]+14);
        switchLayers |= !strcmp(argv[i],"--switch-layers");
        if(!strncmp(argv[i],"--fail-layer=",13)) failLayer=atoi(argv[i]+13);
        noGuides |= !strcmp(argv[i],"--no-guides");
        switchMotion |= !strcmp(argv[i],"--switch-motion");
        noOptical |= !strcmp(argv[i],"--no-optical");
        switchQuality |= !strcmp(argv[i],"--switch-quality");
        debugMotion |= !strcmp(argv[i],"--debug-motion");
        selectedZero |= !strcmp(argv[i],"--selected-zero");
    }
    for(int i=1;i<argc;++i) emptyCompute |= !strcmp(argv[i],"--empty-compute");
    for(int i=1;i<argc;++i) { typeless |= !strcmp(argv[i],"--typeless"); large |= !strcmp(argv[i],"--4k"); resetEveryFrame |= !strcmp(argv[i],"--reset-every-frame"); present |= !strcmp(argv[i],"--present"); directGuides |= !strcmp(argv[i],"--direct-guides"); }
    Check(!(present && directGuides),"direct guides test is for SR->NR only");
    for(int i=1;i<argc;++i) withSr |= !strcmp(argv[i],"--with-sr");
    for(int i=1;i<argc;++i) fenceWait |= !strcmp(argv[i],"--fence-wait");
    for(int i=1;i<argc;++i) switchRoutes |= !strcmp(argv[i],"--switch-routes");
    Check(!switchRoutes || (!present && !directGuides && !withSr && !fenceWait),"switch test needs no other route flags");
    float testScale=1.0f; bool switchScale=false;
    for(int i=1;i<argc;++i) {
        if(!strncmp(argv[i],"--scale=",8)) testScale=float(atof(argv[i]+8));
        switchScale |= !strcmp(argv[i],"--switch-scale");
    }
    Check(testScale>=0.5f && testScale<=1.0f,"scale must be 0.5..1");
    Check(!switchScale || (!switchRoutes && !present && !fenceWait),"scale sweep uses standalone Evaluate");
    if(switchRoutes) present=true;
    Check(!withSr || (directGuides && !present),"SR test needs --direct-guides");
    Check(!fenceWait || !present,"fence-wait is for external command lists");
    Check(!heaplessBindings || !present || switchRoutes,"heapless bindings need an Evaluate route");
    ComPtr<ID3D12Debug> debug; HR(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory; HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> gpu;
    for (UINT i=0;;++i) { ComPtr<IDXGIAdapter1> a; if (FAILED(factory->EnumAdapters1(i,&a))) break;
        DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d); if (d.VendorId==0x10DE && !(d.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)) {gpu=a; break;} }
    Check(gpu!=nullptr,"NVIDIA hardware not found");
    DXGI_ADAPTER_DESC1 adapter{}; gpu->GetDesc1(&adapter); printf("NVIDIA device %04X, depth=%s\n",adapter.DeviceId,typeless?"19 (old)":"21 (fixed)");
    ComPtr<ID3D12Device> d; HR(D3D12CreateDevice(gpu.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&d)));
    ComPtr<ID3D12InfoQueue> info; HR(d.As(&info));
    ComPtr<ID3D12CommandQueue> q; D3D12_COMMAND_QUEUE_DESC qd{}; HR(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)));
    ComPtr<ID3D12CommandQueue> presentQueue;
    if(switchRoutes) HR(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&presentQueue)));
    ComPtr<ID3D12CommandAllocator> alloc; HR(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc)));
    ComPtr<ID3D12CommandAllocator> alloc2; HR(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc2)));
    ComPtr<ID3D12GraphicsCommandList> l; HR(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc.Get(),nullptr,IID_PPV_ARGS(&l)));
    Check(CommandListTracker::InstallRootHooks(l.Get(),&Patch),"root hooks"); HR(l->Close());
    ComPtr<ID3D12Fence> fence; HR(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))); UINT64 serial=0;
    auto flush=[&] {
        for(auto* queue : {q.Get(),presentQueue.Get()}) if(queue) {
            HR(queue->Signal(fence.Get(),++serial)); HANDLE e=CreateEventW(nullptr,FALSE,FALSE,nullptr);
            HR(fence->SetEventOnCompletion(serial,e)); DWORD result=WaitForSingleObject(e,15000); CloseHandle(e);
            Check(result==WAIT_OBJECT_0 && fence->GetCompletedValue()!=UINT64_MAX,"GPU timeout/device removed");
        }
    };
    auto texture=[&](UINT w,UINT h,DXGI_FORMAT f,D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{}; td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width=w; td.Height=h;
        td.DepthOrArraySize=1; td.MipLevels=1; td.Format=f; td.SampleDesc.Count=1; td.Flags=flags;
        ComPtr<ID3D12Resource> r; HR(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,state,nullptr,IID_PPV_ARGS(&r))); return r; };
    auto heap=[&](D3D12_DESCRIPTOR_HEAP_TYPE type,UINT count,bool visible=false) {
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=type; hd.NumDescriptors=count;
        hd.Flags=visible?D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE:D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        ComPtr<ID3D12DescriptorHeap> h; HR(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&h))); return h; };
    const UINT W=large?3840:512,H=large?2160:288,GW=large?2560:256,GH=large?1440:144;
    const UINT sceneFrame=(switchRoutes||switchScale||switchLayers)?0:large?1300:30,totalFrames=(switchRoutes||switchScale||switchLayers)?420:sceneFrame+100,cell=large?120:16;
    const UINT routeSpan=60;
    printf("Color %ux%u, guides %ux%u, scene at %u, resetEveryFrame=%d\n",W,H,GW,GH,sceneFrame,resetEveryFrame);
    const DXGI_FORMAT evaluateColorFormat=typelessColor?DXGI_FORMAT_R8G8B8A8_TYPELESS:
        rgba8Color?DXGI_FORMAT_R8G8B8A8_UNORM:DXGI_FORMAT_R11G11B10_FLOAT;
    DXGI_FORMAT colorFormat=present?DXGI_FORMAT_R10G10B10A2_UNORM:evaluateColorFormat;
    printf("Evaluate resource format=%u, typeless color=%d\n",unsigned(evaluateColorFormat),typelessColor);
    auto color=texture(W,H,colorFormat,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_RENDER_TARGET);
    ComPtr<ID3D12Resource> alternateColor;
    if(switchRoutes) alternateColor=texture(W,H,evaluateColorFormat,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    auto depth=texture(GW,GH,DXGI_FORMAT_R32G8X24_TYPELESS,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL,D3D12_RESOURCE_STATE_DEPTH_WRITE);
    auto copy=texture(GW,GH,typeless?DXGI_FORMAT_R32G8X24_TYPELESS:NgxGuideFormat(DXGI_FORMAT_R32G8X24_TYPELESS),D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
    auto motion=texture(GW,GH,DXGI_FORMAT_R16G16_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET);
    const auto motionRead = D3D12_RESOURCE_STATES(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    const auto depthRead = D3D12_RESOURCE_STATES(motionRead | D3D12_RESOURCE_STATE_DEPTH_READ);
    if (directGuides) puts("Borrow original depth+motion, actual states 0xE0/0xC0; pass compute-read access 0x40; no copies");
    auto rtv=heap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV,2),dsv=heap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV,1),bind=heap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,true);
    auto colorView=rtv->GetCPUDescriptorHandleForHeapStart(),motionView=colorView;
    motionView.ptr+=d->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto setColorView=[&] {
        D3D12_RENDER_TARGET_VIEW_DESC view{}; view.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
        view.Format=colorFormat==DXGI_FORMAT_R8G8B8A8_TYPELESS?DXGI_FORMAT_R8G8B8A8_UNORM:colorFormat;
        d->CreateRenderTargetView(color.Get(),&view,colorView);
    };
    setColorView(); d->CreateRenderTargetView(motion.Get(),nullptr,motionView);
    D3D12_DEPTH_STENCIL_VIEW_DESC dd{}; dd.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT; dd.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2D;
    d->CreateDepthStencilView(depth.Get(),&dd,dsv->GetCPUDescriptorHandleForHeapStart());
    NVSDK_NGX_Handle* srFeature=nullptr; NVSDK_NGX_Parameter* srParameters=nullptr;
    auto srInput=texture(GW,GH,colorFormat,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST);
    if(withSr) {
        wchar_t exeDir[MAX_PATH]{}; GetModuleFileNameW(nullptr,exeDir,MAX_PATH);
        *wcsrchr(exeDir,L'\\')=0;
        NVSDK_NGX_FeatureCommonInfo common{}; const wchar_t* search[]{exeDir};
        common.PathListInfo.Path=search; common.PathListInfo.Length=1;
        auto result=NVSDK_NGX_D3D12_Init_with_ProjectID("b7e4f2a1-6c39-4d58-9a2e-1f0c7d38b45a",
            NVSDK_NGX_ENGINE_TYPE_CUSTOM,"NRFG-regression",exeDir,d.Get(),&common,NVSDK_NGX_Version_API);
        printf("Native NGX SR init = %08X\n",unsigned(result)); Check(NVSDK_NGX_SUCCEED(result),"native SR init");
        Check(NVSDK_NGX_SUCCEED(NVSDK_NGX_D3D12_AllocateParameters(&srParameters)),"SR parameter allocation");
        HR(alloc->Reset()); HR(l->Reset(alloc.Get(),nullptr));
        NVSDK_NGX_DLSS_Create_Params cp{}; cp.Feature.InWidth=GW; cp.Feature.InHeight=GH;
        cp.Feature.InTargetWidth=W; cp.Feature.InTargetHeight=H; cp.Feature.InPerfQualityValue=NVSDK_NGX_PerfQuality_Value_MaxQuality;
        cp.InFeatureCreateFlags=NVSDK_NGX_DLSS_Feature_Flags_IsHDR|NVSDK_NGX_DLSS_Feature_Flags_MVLowRes|
            NVSDK_NGX_DLSS_Feature_Flags_DepthInverted|NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
        result=NGX_D3D12_CREATE_DLSS_EXT(l.Get(),1,1,&srFeature,srParameters,&cp);
        printf("Native SR create = %08X\n",unsigned(result)); Check(NVSDK_NGX_SUCCEED(result),"native SR create");
        Check(!Errors(info.Get()),"SR create validation"); HR(l->Close());
        ID3D12CommandList* initLists[]{l.Get()}; q->ExecuteCommandLists(1,initLists); flush();
    }
    D3D12_ROOT_SIGNATURE_DESC rd{}; ComPtr<ID3DBlob> blob,error; HR(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));
    ComPtr<ID3D12RootSignature> root; HR(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
    DlssNrFilter nr; Check(nr.Initialize(d.Get(),q.Get(),GetModuleHandleW(nullptr)),nr.LastError());
    NrLayerTestAccess::Install(nr,failLayer);
    NrSettings settings{}; settings.enabled=true; settings.toneScale=4; settings.localTone=1.02f; settings.localStructure=1.02f;
    if(grading) settings.grading.monochrome={true,1.0f};
    settings.opticalFlow=!noOptical;
    if(debugMotion) settings.debugView=NrSettings::DebugView::Motion;
    if(selectedZero) settings.useRealMotion=settings.useRealDepth=false;
    settings.renderScale=testScale;
    settings.selfLayers=selfLayers; settings.trueLayers=trueLayers;
    for(int i=1;i<argc;++i) if(!strncmp(argv[i],"--colour=",9)) settings.colourStrength=float(atof(argv[i]+9));
    Check(settings.colourStrength>=0 && settings.colourStrength<=1,"colour range");
    printf("Colour strength=%.2f\n",settings.colourStrength);
    if(failLayer) {
        Check(failLayer>1 && failLayer<=trueLayers,"failure test needs true layers >= failing layer > 1");
        Check(!nr.Prepare(W,H,colorFormat,settings,present?NrMode::Present:NrMode::AtEvaluate),"expected injected layer creation failure");
        Check(!nr.LiveLayerCount(),"failed chain was left partially active");
        for(int retry=0;retry<8;++retry) Check(!nr.Prepare(W,H,colorFormat,settings,present?NrMode::Present:NrMode::AtEvaluate),"failed configuration must wait for user change");
        Check(NrLayerTestAccess::createCalls==failLayer,"failed chain retried every frame");
        trueLayers=settings.trueLayers=1;
        puts("PASS injected layer creation failure: partial chain released, no repeated allocation; retry at 1 layer");
    }
    const bool prepared=nr.Prepare(W,H,colorFormat,settings,present?NrMode::Present:NrMode::AtEvaluate);
    if(!prepared) printf("Prepare blocked: resource format=%u, block=%u, reason=%s\n",unsigned(colorFormat),nr.LastBlock(),nr.LastError());
    Check(prepared,nr.LastError()); flush();
    if(typelessColor && !present) {
        const int created=NrLayerTestAccess::createCalls;
        Check(NrLayerTestAccess::WorkingColorFormat(nr)==DXGI_FORMAT_R8G8B8A8_UNORM,"working texture must use typed RGBA8 UNORM");
        Check(nr.Prepare(W,H,DXGI_FORMAT_R8G8B8A8_UNORM,settings,NrMode::AtEvaluate),"typed alias Prepare");
        Check(nr.Prepare(W,H,colorFormat,settings,NrMode::AtEvaluate),"typeless alias Prepare");
        Check(NrLayerTestAccess::createCalls==created,"typed/typeless aliases must not recreate NR temporal features");
        puts("PASS typed/typeless Prepare reuses the same NR features and typed working texture format");
    }
    Check(nr.LiveLayerCount()==(trueLayers<1?1:trueLayers>5?5:trueLayers),"initial layer clamp/count");
    Check(!Errors(info.Get()),"initialization validation errors");
    auto& gate=EvaluateGpuGate::Get();
    if(fenceWait || switchRoutes) { Check(gate.Initialize(d.Get()),"NR gate init"); gate.SetSubmissionHookReady(true); }
    D3D12_HEAP_PROPERTIES readHeap{}; readHeap.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC readDesc{}; readDesc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    readDesc.Width=32768; readDesc.Height=1; readDesc.DepthOrArraySize=1; readDesc.MipLevels=1;
    readDesc.SampleDesc.Count=1; readDesc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> pixels; HR(d->CreateCommittedResource(&readHeap,D3D12_HEAP_FLAG_NONE,
        &readDesc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&pixels)));
    auto probe=[&](UINT slot) {
        Barrier(l.Get(),color.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource=color.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource=pixels.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        const auto readFormat=colorFormat==DXGI_FORMAT_R8G8B8A8_TYPELESS?DXGI_FORMAT_R8G8B8A8_UNORM:colorFormat;
        dst.PlacedFootprint.Offset=slot*16384; dst.PlacedFootprint.Footprint={readFormat,64,64,1,256};
        D3D12_BOX box{W/2,H/2,0,W/2+64,H/2+64,1}; l->CopyTextureRegion(&dst,0,0,0,&src,&box);
        Barrier(l.Get(),color.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    };
    UINT heaplessAttempts=0, heaplessRecorded=0, heaplessStable=0, heaplessStableRecorded=0;
    for (UINT frame=0;frame<totalFrames;++frame) {
        if(grading) {
            // Exercise independent grading with the model suspended, then
            // resume the same history owner. Repeat in both routes/scales.
            const bool modelOn=frame%routeSpan<20 || frame%routeSpan>=30;
            if(settings.enabled!=modelOn) {
                flush();settings.enabled=modelOn;
                Check(nr.Prepare(W,H,colorFormat,settings,present?NrMode::Present:NrMode::AtEvaluate,true),"NR plus grading on/off transition");
                flush();printf("NR/GRADING SWITCH frame=%u NR=%d grading=1\n",frame,modelOn);
            }
        }
        if(switchMotion || switchQuality) {
            const bool optical=!noOptical && (!switchMotion || frame<60 || frame>=80);
            const int quality=switchQuality?int((frame/40)%3):settings.opticalQuality;
            // Drain for an actual reconfiguration, not every frame: otherwise
            // combining quality changes with --fence-wait never tests the gate.
            if(settings.opticalFlow!=optical || settings.opticalQuality!=quality) flush();
            settings.opticalFlow=optical;
            settings.opticalQuality=quality;
            Check(nr.Prepare(W,H,colorFormat,settings,present?NrMode::Present:NrMode::AtEvaluate),"optical settings reconfigure");
        }
        const bool routeChanged=switchRoutes && frame>0 && frame%routeSpan==0;
        if(routeChanged) {
            Check(gate.IsIdle(),"mode change while external NR in flight");
            present=!present; color.Swap(alternateColor);
            colorFormat=present?DXGI_FORMAT_R10G10B10A2_UNORM:evaluateColorFormat;
            setColorView();
        }
        if(switchScale && frame%routeSpan==0) {
            constexpr float scales[]{1.0f,0.75f,0.5f,0.75f,1.0f,0.5f,1.0f};
            settings.renderScale=scales[frame/routeSpan];
            Check(nr.Prepare(W,H,colorFormat,settings,NrMode::AtEvaluate),"live scale Prepare");
            flush();
            printf("SCALE SWITCH frame=%u scale=%.2f model=%ux%u output=%ux%u\n",frame,settings.renderScale,nr.Width(),nr.Height(),W,H);
            Check(nr.Width()==(settings.renderScale==1?W:(UINT(W*settings.renderScale)&~7u)),"actual NR scale dimensions");
        }
        if(switchRoutes && (routeChanged || frame==0)) {
            if(present) Check(nr.SetPresentQueue(presentQueue.Get()),"adopt actual Present queue");
            NrSettings routeSettings=settings; routeSettings.forceReset=present && !settings.opticalFlow;
            Check(nr.Prepare(W,H,colorFormat,routeSettings,present?NrMode::Present:NrMode::AtEvaluate,true),"live mode Prepare");
            printf("HOT SWITCH frame=%u mode=%s format=%u\n",frame,present?"Present":"Evaluate",unsigned(colorFormat));
            flush();
        }
        const bool layerChanged=switchLayers && frame%routeSpan==0;
        if(layerChanged) {
            constexpr int layers[]{1,2,5,3,4,1,1};
            flush();
            settings.trueLayers=layers[frame/routeSpan];
            // Last two spans change only self layers: model call count must not change.
            settings.selfLayers=1.0f+float(frame/routeSpan)*0.25f;
            Check(nr.Prepare(W,H,colorFormat,settings,present?NrMode::Present:NrMode::AtEvaluate),"live layer Prepare");
            flush();
            printf("LAYER SWITCH frame=%u true=%d self=%.2f\n",frame,settings.trueLayers,settings.selfLayers);
            Check(nr.LiveLayerCount()==settings.trueLayers,"live feature count");
        }
        auto* frameAllocator=(fenceWait && (frame&1))?alloc2.Get():alloc.Get();
        // The gate completed frame N-2 before frame N-1 was recorded. Reuse
        // that allocator; Reset of the list itself may overlap GPU execution.
        HR(frameAllocator->Reset()); HR(l->Reset(frameAllocator,nullptr));
        if (frame==0) {
            float zero[4]{}; l->ClearRenderTargetView(motionView,zero,0,nullptr);
            l->ClearDepthStencilView(dsv->GetCPUDescriptorHandleForHeapStart(),D3D12_CLEAR_FLAG_DEPTH|D3D12_CLEAR_FLAG_STENCIL,.5f,0,0,nullptr);
            if (directGuides) {
                Barrier(l.Get(),depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,depthRead);
                Barrier(l.Get(),motion.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,motionRead);
            } else {
                Barrier(l.Get(),depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,D3D12_RESOURCE_STATE_COPY_SOURCE);
                l->CopyResource(copy.Get(),depth.Get());
                Barrier(l.Get(),copy.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                Barrier(l.Get(),motion.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            }
        } else Barrier(l.Get(),color.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_RENDER_TARGET);
        float rgba[4]{}; l->ClearRenderTargetView(colorView,rgba,0,nullptr);
        if (frame>=sceneFrame) for (UINT y=0;y<H;y+=cell) for (UINT x=0;x<W;x+=cell) {
            float c[4]={float(1+(x/cell)%7),float(1+(y/cell)%5),float(1+((x+y)/cell)%9),1};
            if (present || typelessColor || rgba8Color) { c[0]*=.1f; c[1]*=.1f; c[2]*=.1f; }
            D3D12_RECT rect{LONG(x),LONG(y),LONG(x+cell),LONG(y+cell)}; l->ClearRenderTargetView(colorView,c,1,&rect);
        }
        Barrier(l.Get(),color.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const bool heaplessFrame=heaplessBindings && !present;
        const bool emptyComputeFrame=emptyCompute || (heaplessFrame && (frame&1));
        ID3D12DescriptorHeap* heaps[]{bind.Get()};
        auto& tracker=CommandListTracker::Get();
        if(!heaplessFrame) { l->SetDescriptorHeaps(1,heaps); tracker.NoteDescriptorHeaps(l.Get(),1,heaps); }
        if(!emptyComputeFrame) { l->SetComputeRootSignature(root.Get()); tracker.NoteComputeRootSignature(l.Get(),root.Get()); }
        else Check(tracker.Snapshot(l.Get()).resetObserved && !tracker.Snapshot(l.Get()).computeRootSignature,
            "empty-compute fixture must enter with observed Reset and no compute signature");
        if(heaplessFrame) Check(tracker.Snapshot(l.Get()).resetObserved &&
            tracker.Snapshot(l.Get()).KnownEmptyDescriptorHeaps(),"heapless fixture must enter NR without a bound heap");
        NrEvaluateInput input{}; input.color=color.Get(); input.colorState=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        input.motion=motion.Get(); input.depth=directGuides?depth.Get():copy.Get(); input.motionState=input.depthState=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        const bool omitMotion=noGuides || (switchMotion && (frame/20)%2==1);
        if(omitMotion) input.motion=nullptr;
        input.subrectWidth=W; input.subrectHeight=H; input.guideSubrectWidth=GW; input.guideSubrectHeight=GH;
        input.mvScaleX=float(GW); input.mvScaleY=float(GH); input.preExposure=1; input.hasDepthInverted=true; input.depthInverted=true;
        input.reset=resetEveryFrame||frame==0||frame==sceneFrame;
        if(withSr) {
            // Produce a real SR result on the same device/list immediately before NR.
            Barrier(l.Get(),color.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION src{},dst{}; src.pResource=color.Get(); dst.pResource=srInput.Get();
            D3D12_BOX crop{0,0,0,GW,GH,1}; l->CopyTextureRegion(&dst,0,0,0,&src,&crop);
            Barrier(l.Get(),color.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Barrier(l.Get(),srInput.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            CommandListStateScope srEnvelope(l.Get());
            NVSDK_NGX_D3D12_DLSS_Eval_Params ep{}; ep.Feature.pInColor=srInput.Get(); ep.Feature.pInOutput=color.Get();
            ep.pInDepth=depth.Get(); ep.pInMotionVectors=motion.Get(); ep.InMVScaleX=float(GW); ep.InMVScaleY=float(GH);
            ep.InRenderSubrectDimensions={GW,GH}; ep.InReset=frame==0||frame==sceneFrame;
            ep.InJitterOffsetX=input.jitterX; ep.InJitterOffsetY=input.jitterY;
            Check(NVSDK_NGX_SUCCEED(NGX_D3D12_EVALUATE_DLSS_EXT(l.Get(),srFeature,srParameters,&ep)),"native SR evaluate");
            Barrier(l.Get(),srInput.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        }
        const bool inspect=frame==totalFrames-1 || ((switchRoutes||switchScale||switchLayers) && (frame+1)%routeSpan==0);
        if (inspect) probe(0);
        bool ran=false;
        const bool injectTransient=failureStreak && frame>=20 && frame<=60 && frame%5==0;
        if(injectTransient) NrLayerTestAccess::InjectFailure(nr);
        const auto opticalBefore=nr.OpticalStatus().dispatches;
        const auto callsBefore=nr.ModelCallCount();
        const auto gradingBefore=nr.GradingCount();
        if (present) {
            // Present NR must operate without any native SR initialization/evaluate.
            Check(!Errors(info.Get()),"display input validation"); HR(l->Close());
            ID3D12CommandList* prep[]{l.Get()}; q->ExecuteCommandLists(1,prep); flush();
            nr.SetExternalDepth(switchRoutes?nullptr:copy.Get(),GW,GH);
            nr.SetExternalMotion((switchRoutes||omitMotion)?nullptr:motion.Get(),GW,GH,float(GW),float(GH));
            ran=nr.Execute({color.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS},{color.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS});
            flush(); HR(alloc->Reset()); HR(l->Reset(alloc.Get(),nullptr));
        } else {
            CommandListStateScope gameEnvelope(l.Get());
            Check(gameEnvelope.Ready(),"game state envelope");
            if(fenceWait || switchRoutes) Check(gate.Begin(l.Get()),"NR frame dropped by GPU gate");
            ran=nr.ExecuteOnList(l.Get(),input);
            if(emptyComputeFrame) Check(!tracker.Snapshot(l.Get()).computeRootSignature,
                "real NR polluted known-empty game state");
        }
        if(heaplessFrame) {
            ++heaplessAttempts; if(ran) ++heaplessRecorded;
            Check(tracker.Snapshot(l.Get()).KnownEmptyDescriptorHeaps(),"real NR polluted heapless game bindings");
            if(frame>1 && !routeChanged && !layerChanged && !(switchScale && frame%routeSpan==0)) {
                ++heaplessStable; if(ran) ++heaplessStableRecorded;
                Check(ran,"actual NR dropped a stable heapless frame");
            }
        }
        if (inspect) probe(1);
        if(ran) {
            Check(nr.ModelCallCount()==callsBefore+(debugMotion?0:nr.LiveLayerCount()),"model calls must equal true layers, independent of self layers");
            if(grading)Check(nr.GradingCount()==gradingBefore+1,"one grading pass must follow every processed frame");
            const auto opticalAfter=nr.OpticalStatus();
            const bool expectOptical=settings.enabled && settings.opticalFlow && (selectedZero || (present?(switchRoutes||omitMotion):omitMotion));
            Check(opticalAfter.dispatches==opticalBefore+(expectOptical?1u:0u),"optical workload was not gated by native motion/checkbox");
            Check(opticalAfter.active==expectOptical,"optical status disagrees with selected motion");
        }
        if (directGuides) {
            // The next engine pass must still see the original full state masks.
            Barrier(l.Get(),depth.Get(),depthRead,D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Barrier(l.Get(),motion.Get(),motionRead,D3D12_RESOURCE_STATE_RENDER_TARGET);
            float zero[4]{}; l->ClearRenderTargetView(motionView,zero,0,nullptr);
            l->ClearDepthStencilView(dsv->GetCPUDescriptorHandleForHeapStart(),D3D12_CLEAR_FLAG_DEPTH,.5f,0,0,nullptr);
            Barrier(l.Get(),depth.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE,depthRead);
            Barrier(l.Get(),motion.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,motionRead);
        }
        if (!ran && frame>1 && !routeChanged && !layerChanged && !(switchScale && frame%routeSpan==0)) { printf("block=%u %s\n",nr.LastBlock(),nr.LastError()); Check(false,"NR skipped"); }
        Check(!Errors(info.Get()),"recording validation errors (not submitted)");
        HR(l->Close()); ID3D12CommandList* lists[]{l.Get()}; q->ExecuteCommandLists(1,lists);
        nr.NotifyGradingSubmitted(1,lists);
        if(fenceWait || (switchRoutes && !present)) gate.Submitted(q.Get(),1,lists);
        if(!fenceWait || frame==totalFrames-1) flush();
        Check(!Errors(info.Get()),"GPU validation errors");
        if(injectTransient) {
            Check(ran,"real NR frame did not recover after an isolated failure");
            NrLayerTestAccess::CheckRecovered(nr);
        }
        if((switchRoutes||switchScale||switchLayers) && inspect) {
            void* sample=nullptr; HR(pixels->Map(0,nullptr,&sample));
            const auto* v=static_cast<const UINT*>(sample); unsigned changed=0;
            for(UINT i=0;i<4096;++i) changed+=v[i]!=v[4096+i]; pixels->Unmap(0,nullptr);
            printf("HOT SWITCH output %s: changed=%u/4096\n",present?"Present":"Evaluate",changed);
            Check(changed>0,"mode switched but NR has no visible output");
        }
        if (frame%(large?100:10)==0) printf("frame=%u NR=%llu %s\n",frame,nr.EvaluateCount(),fenceWait?"submitted (no per-frame flush)":"GPU completed");
    }
    if(fenceWait) {
        const auto s=gate.Snapshot();
        printf("NR admission: admitted=%llu waits=%llu totalWaitMs=%llu unsubmitted=%llu timeout=%llu errors=%llu\n",
            s.admitted,s.waits,s.waitMs,s.unsubmitted,s.timeouts,s.errors);
        Check(s.admitted==totalFrames && s.waits>0 && !s.unsubmitted && !s.timeouts && !s.errors,"continuous GPU fence admission");
    }
    if(heaplessBindings) {
        printf("Heapless actual NR: recorded=%u/%u, stable consecutive successes=%u/%u (empty/root-only compute alternation)\n",
            heaplessRecorded,heaplessAttempts,heaplessStableRecorded,heaplessStable);
        Check(heaplessStable>0 && heaplessStableRecorded==heaplessStable,"heapless model continuity");
    }
    void* mapped=nullptr; HR(pixels->Map(0,nullptr,&mapped)); const auto* values=static_cast<const UINT*>(mapped);
    unsigned changed=0,nonfinite=0;
    for(UINT i=0;i<4096;++i) { const UINT v=values[4096+i]; changed+=v!=values[i];
        if (colorFormat==DXGI_FORMAT_R11G11B10_FLOAT) nonfinite+=((v>>6)&31)==31 || ((v>>17)&31)==31 || ((v>>27)&31)==31; }
    pixels->Unmap(0,nullptr);
    printf("HDR output probe: changed=%u/4096, nonfinite=%u\n",changed,nonfinite);
    Check(changed>0 && nonfinite==0,"NR output has no effect or contains nonfinite pixels");
    printf("Optical workload: %llu dispatches; native-priority / checkbox checks passed\n",nr.OpticalStatus().dispatches);
    printf("Layer workload: %llu model calls, %llu processed frames, final true=%d self=%.2f\n",nr.ModelCallCount(),nr.EvaluateCount(),nr.LiveLayerCount(),settings.selfLayers);
    if(grading) {Check(nr.GradingCount()>nr.EvaluateCount(),"standalone grading frames were not recorded");printf("Grading workload: %llu frames including NR-off spans\n",nr.GradingCount());}
    Check(NrLayerTestAccess::stage==0 && NrLayerTestAccess::checkedCalls==nr.ModelCallCount(),"incomplete NR chain");
    puts("PASS layer SDK contract: distinct temporal handles/bags, serial output-to-input, shared guides/reset, final output");
    if(failureStreak) {
        NrLayerTestAccess::CheckTerminalStreak(nr);
        puts("PASS real NR failure recovery: nine isolated failures followed by successful GPU frames; eight consecutive failures still disable NR");
    }
    nr.TeardownForExit(); flush();
    if(withSr) {
        NVSDK_NGX_D3D12_ReleaseFeature(srFeature); NVSDK_NGX_D3D12_DestroyParameters(srParameters);
        NVSDK_NGX_D3D12_Shutdown1(d.Get()); puts("Native SR + direct NR both completed on the same device and list");
    }
    printf("PASS actual NR: %s, %llu evaluations, debug errors=0\n",present?"standalone display path, no native SR":"black -> HDR",nr.EvaluateCount());
    return 0;
} catch (const std::exception& e) { printf("FAIL: %s\n",e.what()); return 1; }
