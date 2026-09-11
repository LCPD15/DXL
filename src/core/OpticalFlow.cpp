#include "OpticalFlow.h"
#include <FidelityFX/host/ffx_opticalflow.h>
#include <FidelityFX/host/backends/dx12/ffx_dx12.h>
#include <wrl/client.h>
#include <vector>
#include <mutex>
#include <algorithm>
#include <cmath>
#include "shaders/precompiled/OpticalDensify.h"
#include "../common/Log.h"
using Microsoft::WRL::ComPtr;
namespace DXL {
namespace {
void Transition(ID3D12GraphicsCommandList* l,ID3D12Resource* r,D3D12_RESOURCE_STATES a,D3D12_RESOURCE_STATES b) {
    if(a==b) return;
    D3D12_RESOURCE_BARRIER v{}; v.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b}; l->ResourceBarrier(1,&v);
}
constexpr auto Read=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto Write=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
FfxResource Wrap(ID3D12Resource* r,FfxResourceStates state) {
    return ffxGetResourceDX12(r,ffxGetResourceDescriptionDX12(r),L"NRFG optical",state);
}
}
struct OpticalFlow::Impl {
    std::unique_ptr<FfxOpticalflowContext> context;
    std::vector<uint8_t> scratch;
    ComPtr<ID3D12Resource> input, sparse, scene, dense, probe;
    ComPtr<ID3D12DescriptorHeap> heap;
    ComPtr<ID3D12RootSignature> root;
    ComPtr<ID3D12PipelineState> pso;
    ComPtr<ID3D12QueryHeap> queries;
    bool history=false,failed=false,pending=false;
    uint64_t lastTick=0,frequency=0;
    int quality=-1;
    uint32_t sourceWidth=0,sourceHeight=0;
    mutable std::mutex mutex;
    OpticalFlowStatus status;
    void SetError(const char* error) {
        failed=true; history=false;
        std::lock_guard lock(mutex); status.active=false; status.ready=false; status.error=error;
        D5_LOG_WARN(L"Optical flow unavailable: %hs; keeping zero-motion fallback",error);
    }
    bool Create(ID3D12Device* d,uint32_t w,uint32_t h,int q) {
        sourceWidth=w; sourceHeight=h; quality=q;
        const uint32_t limit=q==0?640u:q==1?960u:1280u;
        float factor=std::min(1.0f,float(limit)/std::max(w,h));
        // Seven-level luminance pyramid: pad to whole 64-pixel tiles and keep
        // both dimensions >=128. Vector scale is derived from the actual extent.
        uint32_t fw=std::max(128u,((uint32_t(w*factor)+63)/64)*64);
        uint32_t fh=std::max(128u,((uint32_t(h*factor)+63)/64)*64);
        D3D12_FEATURE_DATA_SHADER_MODEL sm{D3D_SHADER_MODEL_6_2};
        D3D12_FEATURE_DATA_D3D12_OPTIONS1 caps{};
        if(FAILED(d->CheckFeatureSupport(D3D12_FEATURE_SHADER_MODEL,&sm,sizeof(sm))) || sm.HighestShaderModel<D3D_SHADER_MODEL_6_2 ||
           FAILED(d->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS1,&caps,sizeof(caps))) || !caps.WaveOps) return false;
        auto texture=[&](ComPtr<ID3D12Resource>& r,uint32_t width,uint32_t height,DXGI_FORMAT format,D3D12_RESOURCE_STATES state) {
            D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
            D3D12_RESOURCE_DESC rd{}; rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; rd.Width=width; rd.Height=height;
            rd.DepthOrArraySize=rd.MipLevels=1; rd.SampleDesc.Count=1; rd.Format=format; rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            return SUCCEEDED(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,state,nullptr,IID_PPV_ARGS(&r)));
        };
        if(!texture(input,fw,fh,DXGI_FORMAT_R16G16B16A16_FLOAT,Write) || !texture(dense,w,h,DXGI_FORMAT_R16G16_FLOAT,Read) ||
           !texture(sparse,(fw+7)/8,(fh+7)/8,DXGI_FORMAT_R16G16_SINT,Write) || !texture(scene,3,1,DXGI_FORMAT_R32_UINT,Write)) return false;
        scratch.resize(ffxGetScratchMemorySizeDX12(FFX_OPTICALFLOW_CONTEXT_COUNT));
        FfxOpticalflowContextDescription desc{}; desc.resolution={fw,fh};
        if(ffxGetInterfaceDX12(&desc.backendInterface,ffxGetDeviceDX12(d),scratch.data(),scratch.size(),FFX_OPTICALFLOW_CONTEXT_COUNT)!=FFX_OK) return false;
        auto candidate=std::make_unique<FfxOpticalflowContext>();
        if(ffxOpticalflowContextCreate(candidate.get(),&desc)!=FFX_OK) return false;
        context=std::move(candidate);
        D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors=3; hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if(FAILED(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)))) return false;
        auto handle=heap->GetCPUDescriptorHandleForHeapStart(); const auto step=d->GetDescriptorHandleIncrementSize(hd.Type);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{}; srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D; srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; srv.Texture2D.MipLevels=1;
        srv.Format=DXGI_FORMAT_R16G16_SINT; d->CreateShaderResourceView(sparse.Get(),&srv,handle); handle.ptr+=step;
        srv.Format=DXGI_FORMAT_R32_UINT; d->CreateShaderResourceView(scene.Get(),&srv,handle); handle.ptr+=step;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format=DXGI_FORMAT_R16G16_FLOAT; uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        d->CreateUnorderedAccessView(dense.Get(),nullptr,&uav,handle);
        D3D12_DESCRIPTOR_RANGE ranges[2]{}; ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0}; ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,2};
        D3D12_ROOT_PARAMETER rp[2]{}; rp[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; rp[0].DescriptorTable={2,ranges};
        rp[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; rp[1].Constants={0,0,8};
        D3D12_ROOT_SIGNATURE_DESC rs{}; rs.NumParameters=2; rs.pParameters=rp;
        ComPtr<ID3DBlob> blob,error;
        if(FAILED(D3D12SerializeRootSignature(&rs,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error)) ||
           FAILED(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)))) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC ps{}; ps.pRootSignature=root.Get(); ps.CS={kOpticalDensify,sizeof(kOpticalDensify)};
        if(FAILED(d->CreateComputePipelineState(&ps,IID_PPV_ARGS(&pso)))) return false;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{}; buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; buffer.Width=512; buffer.Height=1;
        buffer.DepthOrArraySize=buffer.MipLevels=1; buffer.SampleDesc.Count=1; buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        if(FAILED(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&probe)))) return false;
        D3D12_QUERY_HEAP_DESC qh{}; qh.Type=D3D12_QUERY_HEAP_TYPE_TIMESTAMP; qh.Count=2;
        if(FAILED(d->CreateQueryHeap(&qh,IID_PPV_ARGS(&queries)))) return false;
        {std::lock_guard lock(mutex); status.width=fw; status.height=fh; status.ready=true; status.error=nullptr;}
        D5_LOG_INFO(L"Optical flow created: FidelityFX FP32 quality=%d input=%ux%u vectors=%ux%u; same command list",q,w,h,fw,fh);
        return true;
    }
};
OpticalFlow::OpticalFlow():_impl(std::make_unique<Impl>()) {}
OpticalFlow::~OpticalFlow() { Destroy(); }
void OpticalFlow::Destroy() noexcept {
    auto& p=*_impl;
    if(p.context) { ffxOpticalflowContextDestroy(p.context.get()); p.context.reset(); }
    p.scratch.clear(); p.input.Reset(); p.sparse.Reset(); p.scene.Reset(); p.dense.Reset(); p.probe.Reset(); p.queries.Reset(); p.pso.Reset(); p.root.Reset(); p.heap.Reset();
    p.history=p.failed=p.pending=false; p.quality=-1; p.lastTick=0;
    std::lock_guard lock(p.mutex); p.status.active=p.status.ready=false; p.status.reset=true; p.status.error=nullptr; p.status.gpuMs=0;
}
void OpticalFlow::Suspend() noexcept {
    auto& p=*_impl; p.history=false; p.lastTick=0;
    std::lock_guard lock(p.mutex); p.status.active=false; p.status.reset=true;
}
OpticalFlowStatus OpticalFlow::Status() const noexcept { std::lock_guard lock(_impl->mutex); return _impl->status; }
ID3D12Resource* OpticalFlow::Record(ID3D12Device* device,ID3D12GraphicsCommandList* list,ComputePasses& passes,
    ID3D12Resource* input,int quality,bool reset,bool previousGpuComplete,uint64_t timestampFrequency) noexcept {
    auto& p=*_impl;
    if(!device || !list || !input || p.failed) return nullptr;
    try {
        const auto desc=input->GetDesc(); quality=std::clamp(quality,0,2);
        if(!p.context && !p.Create(device,uint32_t(desc.Width),desc.Height,quality)) { p.SetError("initialization/SM6.2 failed"); return nullptr; }
        // Resize/reconfigure must be done by the owner after draining the GPU.
        if(p.quality!=quality || p.sourceWidth!=desc.Width || p.sourceHeight!=desc.Height) { p.SetError("configuration changed without GPU drain"); return nullptr; }
        bool cut=false;
        if(p.pending && previousGpuComplete) {
            void* mapped=nullptr; D3D12_RANGE range{0,272};
            if(SUCCEEDED(p.probe->Map(0,&range,&mapped))) {
                // Match the GPU's four-frame cut window, including when a
                // pending Present probe could only be read several frames later.
                cut=((*reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(mapped)+4)) & 15u)!=0;
                const auto* ticks=reinterpret_cast<const uint64_t*>(static_cast<uint8_t*>(mapped)+256);
                if(p.frequency && ticks[1]>=ticks[0]) { std::lock_guard lock(p.mutex); p.status.gpuMs=double(ticks[1]-ticks[0])*1000.0/double(p.frequency); }
                D3D12_RANGE written{0,0}; p.probe->Unmap(0,&written);
            }
            p.pending=false;
        }
        const auto now=GetTickCount64(); const bool first=!p.history || reset || (p.lastTick && now-p.lastTick>250);
        const bool measure=!p.pending;
        if(measure) list->EndQuery(p.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
        const auto extent=p.input->GetDesc();
        if(!passes.RecordBoxDownsample(list,input,p.input.Get(),uint32_t(extent.Width),extent.Height,float(desc.Width)/float(extent.Width))) { p.SetError("input resample unavailable"); return nullptr; }
        Transition(list,p.input.Get(),Write,Read);
        FfxOpticalflowDispatchDescription job{}; job.commandList=ffxGetCommandListDX12(list); job.color=Wrap(p.input.Get(),FFX_RESOURCE_STATE_COMPUTE_READ);
        job.opticalFlowVector=Wrap(p.sparse.Get(),FFX_RESOURCE_STATE_UNORDERED_ACCESS); job.opticalFlowSCD=Wrap(p.scene.Get(),FFX_RESOURCE_STATE_UNORDERED_ACCESS);
        job.reset=first; job.backbufferTransferFunction=0; job.minMaxLuminance={0,1};
        const auto result=ffxOpticalflowContextDispatch(p.context.get(),&job);
        Transition(list,p.input.Get(),Read,Write);
        if(result!=FFX_OK) { p.SetError("dispatch failed"); return nullptr; }
        Transition(list,p.sparse.Get(),Write,Read); Transition(list,p.scene.Get(),Write,Read); Transition(list,p.dense.Get(),Read,Write);
        ID3D12DescriptorHeap* heaps[]={p.heap.Get()}; list->SetDescriptorHeaps(1,heaps); list->SetComputeRootSignature(p.root.Get()); list->SetPipelineState(p.pso.Get());
        list->SetComputeRootDescriptorTable(0,p.heap->GetGPUDescriptorHandleForHeapStart());
        const uint32_t constants[]={p.sourceWidth,p.sourceHeight,uint32_t(extent.Width),extent.Height,first?1u:0u,0u,0u,0u}; list->SetComputeRoot32BitConstants(1,8,constants,0);
        list->Dispatch((constants[0]+7)/8,(constants[1]+7)/8,1);
        Transition(list,p.dense.Get(),Write,Read); Transition(list,p.sparse.Get(),Read,Write); Transition(list,p.scene.Get(),Read,Write);
        if(measure) {
            Transition(list,p.scene.Get(),Write,D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION src{},dst{}; src.pResource=p.scene.Get(); dst.pResource=p.probe.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Footprint={DXGI_FORMAT_R32_UINT,3,1,1,256}; list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
            Transition(list,p.scene.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,Write);
            list->EndQuery(p.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1); list->ResolveQueryData(p.queries.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,p.probe.Get(),256);
            p.pending=true; p.frequency=timestampFrequency;
        }
        p.history=true; p.lastTick=now;
        {std::lock_guard lock(p.mutex); p.status.active=true; p.status.reset=first||cut; ++p.status.dispatches;}
        return p.dense.Get();
    } catch(...) { p.SetError("allocation failure"); return nullptr; }
}
}
