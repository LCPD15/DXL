#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include "DlssNrFilter.h"
#include "NgxParameterBag.h"
#include "CommandListTracker.h"
using Microsoft::WRL::ComPtr;
using namespace DXL;
void Check(bool b,const char* message) { if(!b) throw std::runtime_error(message); }
void HR(HRESULT h) { if(FAILED(h)) { printf("HRESULT=%08X\n",unsigned(h)); Check(false,"D3D12 API failed"); } }
void Barrier(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER v{}; v.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b}; l->ResourceBarrier(1,&v);
}
void* Patch(void* obj,size_t index,void* hook) noexcept {
    auto** vt=*reinterpret_cast<void***>(obj); DWORD old=0;
    if(!VirtualProtect(vt+index,sizeof(void*),PAGE_READWRITE,&old)) return nullptr;
    void* original=vt[index]; vt[index]=hook; VirtualProtect(vt+index,sizeof(void*),old,&old); return original;
}
namespace DXL {
struct NrLayerTestAccess {
    using EvalFn=NVSDK_NGX_Result(NVSDK_CONV*)(ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter*,PFN_NVSDK_NGX_ProgressCallback);
    static inline EvalFn original=nullptr;
    static inline DlssNrFilter* active=nullptr;
    static inline uint64_t checked=0;
    static NVSDK_NGX_Result NVSDK_CONV Eval(ID3D12GraphicsCommandList* l,const NVSDK_NGX_Handle* h,const NVSDK_NGX_Parameter* p,PFN_NVSDK_NGX_ProgressCallback cb) {
        ID3D12Resource* mask=nullptr; unsigned w=0,hgt=0;
        Check(NVSDK_NGX_SUCCEED(p->Get("DLSSNR.ControlMask",&mask)) && mask==active->_controlMask,"missing mask at actual NGX evaluation");
        Check(NVSDK_NGX_SUCCEED(p->Get("DLSSNR.ControlMaskSubrectWidth",&w)) && w==active->_width,"mask width mismatch");
        Check(NVSDK_NGX_SUCCEED(p->Get("DLSSNR.ControlMaskSubrectHeight",&hgt)) && hgt==active->_height,"mask height mismatch");
        ++checked; return original(l,h,p,cb);
    }
    static void Instrument(DlssNrFilter& nr) { active=&nr; original=reinterpret_cast<EvalFn>(nr._evaluateFeature); nr._evaluateFeature=reinterpret_cast<void*>(&Eval); }
    static void UnitInit(DlssNrFilter& nr,ID3D12Device* d,ID3D12CommandQueue* q) {
        nr._device=d; d->AddRef(); nr._queue=q; q->AddRef(); nr._width=8; nr._height=2;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{}; rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width=8; rd.Height=2;
        rd.DepthOrArraySize=1; rd.MipLevels=1; rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM; rd.SampleDesc.Count=1;
        HR(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&nr._controlMask)));
    }
    static ID3D12Resource* ModelInput(DlssNrFilter& nr) { return nr._colorIn; }
    static void LargeMask(DlssNrFilter& nr) {
        nr._width=1280;nr._height=720;nr._settings.semanticMask=true;
        Check(nr._passes.Initialize(nr._device),"compute init");
        Check(nr.CreateTextures(DXGI_FORMAT_R8G8B8A8_UNORM,false),"large mask allocation");
        Check(nr._controlMaskLow && nr._controlMaskLow->GetDesc().Width==640,"missing GPU upsample source");
    }
    static void Fill(DlssNrFilter& nr,ID3D12GraphicsCommandList* l,const NrSettings& settings,unsigned slot) {
        nr._settings=settings; Check(nr.EnsureControlMaskFilled(l,slot),"production mask upload failed");
    }
};
}
int main() try {
    setvbuf(stdout,nullptr,_IONBF,0);
    ComPtr<ID3D12Debug> debug; HR(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory; HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    for(UINT i=0;;++i) { ComPtr<IDXGIAdapter1> a; if(FAILED(factory->EnumAdapters1(i,&a))) break; DXGI_ADAPTER_DESC1 d{}; a->GetDesc1(&d); if(d.VendorId==0x10DE) {adapter=a;break;} }
    Check(adapter.Get()!=nullptr,"NVIDIA adapter required");
    ComPtr<ID3D12Device> device; HR(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> info; HR(device.As(&info));
    ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC qd{}; HR(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12Fence> fence; HR(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))); UINT64 serial=0;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    auto flush=[&] {HR(queue->Signal(fence.Get(),++serial)); HR(fence->SetEventOnCompletion(serial,event)); Check(WaitForSingleObject(event,15000)==WAIT_OBJECT_0,"GPU timeout");};
    auto makeList=[&](ComPtr<ID3D12CommandAllocator>& a,ComPtr<ID3D12GraphicsCommandList>& l) {
        HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)));
        HR(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,a.Get(),nullptr,IID_PPV_ARGS(&l)));
    };
    auto readback=[&](ID3D12GraphicsCommandList* l,ID3D12Resource* mask,D3D12_RESOURCE_STATES state=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) {
        auto desc=mask->GetDesc(); const uint32_t bpp=desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT?8:4;
        const uint32_t pitch=(uint32_t(desc.Width)*bpp+255)&~255u;
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC rb{};rb.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;rb.Width=uint64_t(pitch)*desc.Height;
        rb.Height=1;rb.DepthOrArraySize=1;rb.MipLevels=1;rb.SampleDesc.Count=1;rb.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> out;HR(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rb,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&out)));
        Barrier(l,mask,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{};src.pResource=mask;src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION dst{};dst.pResource=out.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint={desc.Format,uint32_t(desc.Width),desc.Height,1,pitch};
        l->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        Barrier(l,mask,D3D12_RESOURCE_STATE_COPY_SOURCE,state);
        return out;
    };
    SemanticMaskSnapshot source; source.width=8;source.height=2;source.version=1;source.publishedMs=GetTickCount64();
    source.coverage={0,0,128,255,0,0,0,0,0,0,128,255,0,0,0,0};
    source.groupIds={0,1,0,0,255,17,18,0,0,1,0,0,255,17,18,0};
    DlssNrFilter::SemanticMaskProvider provider;
    provider.ctx=&source;provider.snapshot=[](void* ctx,SemanticMaskSnapshot& out) noexcept {out=*static_cast<SemanticMaskSnapshot*>(ctx);return true;};
    NrSettings settings;settings.semanticFeather=0;settings.enabled=true;settings.semanticMask=true;settings.semanticEnabled=(1u<<0)|(1u<<17);
    settings.semanticIntensity[0]=0;settings.semanticIntensity[1]=.4f;settings.semanticIntensity[17]=1;settings.semanticBgIntensity=.5f;
    {
        SemanticMaskSnapshot split;split.width=32;split.height=18;split.version=1;split.publishedMs=GetTickCount64();
        split.coverage.assign(32*18,0);split.groupIds.assign(32*18,0);
        for(unsigned y=0;y<18;++y)for(unsigned x=16;x<32;++x)split.groupIds[y*32+x]=255;
        auto splitProvider=provider;splitProvider.ctx=&split;
        DlssNrFilter large;NrLayerTestAccess::UnitInit(large,device.Get(),queue.Get());
        NrLayerTestAccess::LargeMask(large);large.SetSemanticMaskProvider(splitProvider);
        NrSettings smooth=settings;smooth.semanticFeather=2;smooth.semanticBgIntensity=1;
        // Two updates exercise both first-use and shader-readable resource states.
        for(unsigned frame=0;frame<2;++frame) {
            ComPtr<ID3D12CommandAllocator> a;ComPtr<ID3D12GraphicsCommandList> l;makeList(a,l);
            split.version++;split.publishedMs=GetTickCount64();
            NrLayerTestAccess::Fill(large,l.Get(),smooth,frame);
            auto rb=readback(l.Get(),large.ControlMaskResource());HR(l->Close());
            ID3D12CommandList* ls[]{l.Get()};queue->ExecuteCommandLists(1,ls);flush();
            void* mapped=nullptr;D3D12_RANGE range{0,1280*720*4};HR(rb->Map(0,&range,&mapped));
            auto* row=static_cast<uint8_t*>(mapped)+360*1280*4;
            Check(row[0]==0 && row[1279*4]==255,"large mask interior changed");
            Check(row[639*4]>0 && row[639*4]<255 && row[640*4]>0 && row[640*4]<255,"GPU feather edge missing");
            for(unsigned x=0;x<1280;++x)for(unsigned c=1;c<4;++c)Check(row[x*4+c]==255,"GPU changed GBA background");
            rb->Unmap(0,nullptr);
        }
        puts("PASS production GPU mask upsample: soft R edge, exact GBA, first and repeated resource states");
    }
    {
        DlssNrFilter unit;NrLayerTestAccess::UnitInit(unit,device.Get(),queue.Get());unit.SetSemanticMaskProvider(provider);
        ComPtr<ID3D12CommandAllocator> allocs[5];ComPtr<ID3D12GraphicsCommandList> lists[5];ComPtr<ID3D12Resource> reads[5];
        unsigned char expected[5][8]{};
        // Queue all updates only after CPU filled every slot: catches upload-memory reuse.
        for(unsigned frame=0;frame<5;++frame) {
            makeList(allocs[frame],lists[frame]);
            const float bg=float(frame)/4;settings.semanticBgIntensity=bg;
            source.version++;source.publishedMs=GetTickCount64();
            if(frame==3) source.publishedMs=GetTickCount64()-501; // stale must be all background
            NrLayerTestAccess::Fill(unit,lists[frame].Get(),settings,frame);
            reads[frame]=readback(lists[frame].Get(),unit.ControlMaskResource());HR(lists[frame]->Close());
            const auto b=uint8_t(bg*255+.5f);
            expected[frame][0]=frame==3?b:0;expected[frame][1]=b;
            expected[frame][2]=frame==3?b:uint8_t(bg*(128.f/255)*255+.5f);
            expected[frame][3]=b;expected[frame][4]=b;expected[frame][5]=frame==3?b:255;expected[frame][6]=b;expected[frame][7]=frame==3?b:0;
        }
        ID3D12CommandList* submitted[]{lists[0].Get(),lists[1].Get(),lists[2].Get(),lists[3].Get(),lists[4].Get()};queue->ExecuteCommandLists(5,submitted);flush();
        for(unsigned frame=0;frame<5;++frame) {
            void* data=nullptr;D3D12_RANGE r{0,512};HR(reads[frame]->Map(0,&r,&data));const auto* px=static_cast<const uint8_t*>(data);
            const auto bg=uint8_t(float(frame)*255/4+.5f);
            for(unsigned y=0;y<2;++y)for(unsigned x=0;x<8;++x) {
                Check(px[y*256+x*4]==expected[frame][x],"GPU mask R contract");
                for(unsigned c=1;c<4;++c)Check(px[y*256+x*4+c]==bg,"GPU mask GBA must equal background");
            }
            D3D12_RANGE noWrite{0,0};reads[frame]->Unmap(0,&noWrite);
        }
        puts("PASS production GPU mask RGBA: endpoints/soft-edge/disabled/unknown/stale/five in-flight uploads");
    }
    // Run the actual model on both routes and three independent NR features.
    ComPtr<ID3D12CommandAllocator> alloc;ComPtr<ID3D12GraphicsCommandList> list;makeList(alloc,list);
    Check(CommandListTracker::InstallRootHooks(list.Get(),Patch),"tracker hooks");HR(list->Close());
    ComPtr<ID3D12DescriptorHeap> rtv,bind;D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;HR(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv)));
    hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;HR(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&bind)));
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC td{};
    td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;td.Width=256;td.Height=128;td.DepthOrArraySize=1;td.MipLevels=1;
    td.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;td.SampleDesc.Count=1;td.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> color;HR(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&color)));
    device->CreateRenderTargetView(color.Get(),nullptr,rtv->GetCPUDescriptorHandleForHeapStart());
    D3D12_ROOT_SIGNATURE_DESC rd{};ComPtr<ID3DBlob> blob,error;HR(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error));
    ComPtr<ID3D12RootSignature> root;HR(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)));
    DlssNrFilter nr;Check(nr.Initialize(device.Get(),queue.Get(),GetModuleHandleW(nullptr)),"real NR init");
    nr.SetSemanticMaskProvider(provider);NrLayerTestAccess::Instrument(nr);
    settings.semanticBgIntensity=.65f;settings.semanticIntensity[0]=.1f;settings.opticalFlow=false;settings.trueLayers=3;
    uint64_t successful=0;
    for(unsigned scenario=0;scenario<4;++scenario) {
        const bool present=(scenario&1)!=0;settings.renderScale=scenario>=2?.5f:1;
        for(unsigned frame=0;frame<10;++frame) {
            source.version++;source.publishedMs=GetTickCount64();
            Check(nr.Prepare(256,128,td.Format,settings,present?NrMode::Present:NrMode::AtEvaluate,true),"NR prepare");flush();
            HR(alloc->Reset());HR(list->Reset(alloc.Get(),nullptr));
            const float a[]{.25f,.4f,.6f,1};list->ClearRenderTargetView(rtv->GetCPUDescriptorHandleForHeapStart(),a,0,nullptr);
            ID3D12DescriptorHeap* heaps[]{bind.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(root.Get());
            CommandListTracker::Get().NoteDescriptorHeaps(list.Get(),1,heaps); CommandListTracker::Get().NoteComputeRootSignature(list.Get(),root.Get());
            bool ran=false;
            if(!present) {NrEvaluateInput input{};input.color=color.Get();input.colorState=D3D12_RESOURCE_STATE_RENDER_TARGET;input.subrectWidth=256;input.subrectHeight=128;
                input.guideSubrectWidth=256;input.guideSubrectHeight=128;ran=nr.ExecuteOnList(list.Get(),input);}
            HR(list->Close());ID3D12CommandList* submitted[]{list.Get()};queue->ExecuteCommandLists(1,submitted);flush();
            if(present) {GpuImage image{color.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET};ran=nr.Execute(image,image);flush();}
            if(ran) ++successful;
        }
        Check(nr.ControlMaskResource()!=nullptr,"runtime mask missing");
        HR(alloc->Reset());HR(list->Reset(alloc.Get(),nullptr));auto rb=readback(list.Get(),nr.ControlMaskResource());HR(list->Close());
        ID3D12CommandList* submitted[]{list.Get()};queue->ExecuteCommandLists(1,submitted);flush();
        void* data=nullptr;D3D12_RANGE range{0,4};HR(rb->Map(0,&range,&data));auto* first=static_cast<uint8_t*>(data);
        printf("mask observed=(%u,%u,%u,%u) scenario=%u frames=%llu\n", first[0],first[1],first[2],first[3],scenario,(unsigned long long)successful);
        Check(first[0]==26 && first[1]==166 && first[2]==166 && first[3]==166,"actual NR route mask bytes");D3D12_RANGE noWrite{0,0};rb->Unmap(0,&noWrite);
        printf("PASS actual NR %s scale=%.1f trueLayers=3 mask RGBA=(26,166,166,166)\n",present?"Present":"Evaluate",settings.renderScale);
    }
    Check(successful>=36 && NrLayerTestAccess::checked==successful*3,"mask not bound to every neural layer");
    settings.renderScale=1;settings.trueLayers=1;settings.semanticBgIntensity=1;
    source.coverage.assign(16,0);source.groupIds.assign(16,0);
    auto half=[](uint16_t h) {const uint32_t e=(h>>10)&31,m=h&1023;
        if(e==0)return std::ldexp(float(m),-24);
        if(e==31)return m?0.f:65504.f;
        const float f=std::ldexp(float(1024+m),int(e)-25);return h&32768?-f:f;};
    for(unsigned contract=0;contract<3;++contract) {
    double deltas[2]{};
    for(unsigned endpoint=0;endpoint<2;++endpoint) {
        settings.semanticIntensity[0]=float(endpoint);
        ComPtr<ID3D12Resource> reference, result, modelInput;
        for(unsigned frame=0;frame<48;++frame) {
            source.version++;source.publishedMs=GetTickCount64();
            Check(nr.Prepare(256,128,td.Format,settings,NrMode::AtEvaluate,true),"mask endpoint NR prepare");flush();
            HR(alloc->Reset());HR(list->Reset(alloc.Get(),nullptr));
            for(int y=0;y<128;y+=8)for(int x=0;x<256;x+=8) {
                const float v=.12f+float(((x/8)*7+(y/8)*11)%19)*.035f;
                const float pixel[]{v,v*.72f+.08f,v*.45f+.1f,1};
                const D3D12_RECT r{x,y,x+8,y+8};list->ClearRenderTargetView(rtv->GetCPUDescriptorHandleForHeapStart(),pixel,1,&r);
            }
            if(frame==47)reference=readback(list.Get(),color.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);
            ID3D12DescriptorHeap* heaps[]{bind.Get()};list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(root.Get());
            CommandListTracker::Get().NoteDescriptorHeaps(list.Get(),1,heaps);CommandListTracker::Get().NoteComputeRootSignature(list.Get(),root.Get());
            NrEvaluateInput input{};input.color=color.Get();input.colorState=D3D12_RESOURCE_STATE_RENDER_TARGET;
            input.subrectWidth=256;input.subrectHeight=128;input.guideSubrectWidth=256;input.guideSubrectHeight=128;
            input.colorHdrKnown=contract!=0; input.colorIsHdr=contract!=1;
            const bool ran=nr.ExecuteOnList(list.Get(),input);
            if(frame==47) {Check(ran,"endpoint final frame not processed");result=readback(list.Get(),color.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET); modelInput=readback(list.Get(),NrLayerTestAccess::ModelInput(nr),D3D12_RESOURCE_STATE_COPY_DEST);}
            HR(list->Close());ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);flush();
        }
        void *a=nullptr,*b=nullptr;D3D12_RANGE range{0,256*128*8};HR(reference->Map(0,&range,&a));HR(result->Map(0,&range,&b));
        void* proxy=nullptr; HR(modelInput->Map(0,&range,&proxy));
        const bool same=memcmp(a,proxy,256*128*8)==0;
        Check(same==(contract==1),"explicit LDR must preserve model input bytes; HDR/unknown must keep encoding");
        modelInput->Unmap(0,nullptr);
        auto* pa=static_cast<uint16_t*>(a);auto* pb=static_cast<uint16_t*>(b);
        for(unsigned i=0;i<256*128;++i)for(unsigned c=0;c<3;++c)deltas[endpoint]+=std::abs(half(pa[i*4+c])-half(pb[i*4+c]));
        deltas[endpoint]/=256*128*3;
        D3D12_RANGE none{0,0};reference->Unmap(0,&none);result->Unmap(0,&none);
        printf("Actual textured scene mask R=%u GBA=1 mean RGB change=%.8f\n",endpoint,deltas[endpoint]);
    }
    Check(deltas[1]>deltas[0]+.0001,"R endpoints did not control actual NR output");
    puts("PASS real NR output responds to R strength independently of GBA background");
    printf("PASS color contract %u: real model input readback and R endpoints\n",contract);
    }
    nr.TeardownForExit();flush();
    for(UINT64 i=0;i<info->GetNumStoredMessages();++i) {SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<uint8_t> bytes(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());info->GetMessage(i,m,&size);
        if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){puts(m->pDescription);Check(false,"D3D12 debug error");}}
    CloseHandle(event);printf("PASS actual NR mask calls=%llu, no D3D12 errors\n",(unsigned long long)NrLayerTestAccess::checked);return 0;
}catch(const std::exception& e){printf("FAIL %s\n",e.what());return 1;}
