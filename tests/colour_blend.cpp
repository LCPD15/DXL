#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <algorithm>
#include <vector>
#include <cmath>
#include <limits>
#include <cstdio>
#include <stdexcept>
#include <source_location>
#include "ComputePasses.h"
#include "NrToneMapping.h"
using Microsoft::WRL::ComPtr;
using namespace DXL;
static void Check(bool b, const std::source_location at=std::source_location::current()) { if(!b) { printf("assertion line %u\n",at.line()); throw std::runtime_error("colour assertion"); } }
static void HR(HRESULT h) { Check(SUCCEEDED(h)); }
int main() try {
    ComPtr<ID3D12Debug> dbg; HR(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))); dbg->EnableDebugLayer();
    ComPtr<IDXGIFactory4> f; HR(CreateDXGIFactory1(IID_PPV_ARGS(&f)));
    ComPtr<IDXGIAdapter> a; HR(f->EnumWarpAdapter(IID_PPV_ARGS(&a)));
    ComPtr<ID3D12Device> d; HR(D3D12CreateDevice(a.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d)));
    ComPtr<ID3D12InfoQueue> info; HR(d.As(&info));
    ComPtr<ID3D12CommandQueue> q; D3D12_COMMAND_QUEUE_DESC qd{}; HR(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)));
    ComPtr<ID3D12CommandAllocator> al; HR(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&al)));
    ComPtr<ID3D12GraphicsCommandList> l; HR(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,al.Get(),nullptr,IID_PPV_ARGS(&l))); HR(l->Close());
    ComPtr<ID3D12Fence> fence; HR(d->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))); UINT64 serial=0;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC td{}; td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width=td.Height=1;
    td.DepthOrArraySize=td.MipLevels=1; td.Format=DXGI_FORMAT_R16G16B16A16_FLOAT; td.SampleDesc.Count=1;
    td.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET|D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ComPtr<ID3D12Resource> original,model,out;
    HR(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&original)));
    HR(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&model)));
    td.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
    HR(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&out)));
    hp.Type=D3D12_HEAP_TYPE_READBACK; td={}; td.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; td.Width=256; td.Height=1;
    td.DepthOrArraySize=td.MipLevels=1; td.SampleDesc.Count=1; td.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> read; HR(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&read)));
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors=2;
    ComPtr<ID3D12DescriptorHeap> heap; HR(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)));
    auto ov=heap->GetCPUDescriptorHandleForHeapStart(),mv=ov; mv.ptr+=d->GetDescriptorHandleIncrementSize(hd.Type);
    d->CreateRenderTargetView(original.Get(),nullptr,ov); d->CreateRenderTargetView(model.Get(),nullptr,mv);
    ComputePasses passes; Check(passes.Initialize(d.Get()));
    auto barrier=[&](ID3D12Resource* r,D3D12_RESOURCE_STATES b,D3D12_RESOURCE_STATES e) {
        D3D12_RESOURCE_BARRIER v{}; v.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,b,e}; l->ResourceBarrier(1,&v);
    };
    auto run=[&](int mode,float strength,const std::array<float,4>& o,const std::array<float,4>& m,
                 float whitePoint=4.0f,uint32_t curveFlags=0u,float selfLayers=1.0f) {
        HR(al->Reset()); HR(l->Reset(al.Get(),nullptr)); l->ClearRenderTargetView(ov,o.data(),0,nullptr); l->ClearRenderTargetView(mv,m.data(),0,nullptr);
        barrier(original.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        if(mode==3 || mode==4) {
            barrier(model.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            Check(passes.RecordEncode(l.Get(),original.Get(),model.Get(),1,1,whitePoint,2.2f,curveFlags));
            barrier(model.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        } else {
            barrier(model.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        if(mode==0) Check(passes.RecordColourBlend(l.Get(),original.Get(),model.Get(),out.Get(),1,1,strength,selfLayers));
        if(mode==1) Check(passes.RecordRatioApply(l.Get(),original.Get(),model.Get(),out.Get(),1,1,strength,selfLayers));
        if(mode==2 || mode==3) Check(passes.RecordDecodeRatio(l.Get(),original.Get(),model.Get(),out.Get(),1,1,whitePoint,2.2f,curveFlags,strength,selfLayers));
        if(mode==4) Check(passes.RecordVisualizeRaw(l.Get(),model.Get(),out.Get(),1,1,1));
        barrier(out.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{}; src.pResource=out.Get(); dst.pResource=read.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint={DXGI_FORMAT_R32G32B32A32_FLOAT,1,1,1,256}; l->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        barrier(out.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barrier(original.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
        barrier(model.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
        HR(l->Close()); ID3D12CommandList* lists[]{l.Get()}; q->ExecuteCommandLists(1,lists);
        HR(q->Signal(fence.Get(),++serial)); HR(fence->SetEventOnCompletion(serial,event)); Check(WaitForSingleObject(event,5000)==WAIT_OBJECT_0);
        void* data=nullptr; HR(read->Map(0,nullptr,&data)); auto result=*static_cast<std::array<float,4>*>(data); read->Unmap(0,nullptr); return result;
    };
    unsigned checks=0;
    const std::array<float,4> originals[]={{.75f,.125f,.0625f,.25f},{12,3,1,.5f},{0,0,0,1},{0x1p-20f,0x1p-20f,0x1p-20f,1},{0,1,0,1},{.25f,.25f,.25f,1}};
    const std::array<float,4> m={.125f,.75f,.5f,1};
    auto lum=[](const auto& c) { return c[0]*.2126f+c[1]*.7152f+c[2]*.0722f; };
    for(int mode=0;mode<3;++mode) for(const auto& o:originals) {
        const auto full=run(mode,1,o,m);
        for(float s:{0.0f,.5f,1.0f}) {
            const auto got=run(mode,s,o,m);
            printf("mode=%d original=%.8g,%.8g,%.8g strength=%.1f got=%.8g,%.8g,%.8g alpha=%.8g\n",mode,o[0],o[1],o[2],s,got[0],got[1],got[2],got[3]);
            for(int c=0;c<3;++c) {
                const float native=lum(o)>1e-6f?o[c]*lum(full)/lum(o):o[c];
                const float expected=native*(1-s)+full[c]*s;
                Check(std::isfinite(got[c]) && fabs(got[c]-expected)<1e-4f*(1+fabs(expected)));
                if(s==1) Check(got[c]==full[c]);
                if(mode<2 && s==1) Check(got[c]==(mode==0?m[c]:o[c]*m[c]));
            }
            Check(got[3]==o[3]); ++checks;
        }
    }
    unsigned layerChecks=0;
    for(int mode=0;mode<3;++mode) for(const auto& o:originals) for(float s:{0.0f,.5f,1.0f}) {
        const auto single=run(mode,s,o,m);
        for(float n:{-3.0f,0.0f,1.0f,1.01f,1.25f,1.5f,1.99f,2.0f,2.75f,3.0f,5.0f,
                     std::numeric_limits<float>::quiet_NaN(),std::numeric_limits<float>::infinity()}) {
            const auto got=run(mode,s,o,m,4,0,n);
            const float clamped=std::isfinite(n)?std::clamp(n,1.0f,3.0f):1.0f;
            for(int c=0;c<3;++c) {
                const float expected=clamped==1?single[c]:std::max(o[c]+clamped*(single[c]-o[c]),0.0f);
                Check(std::isfinite(got[c]) && fabs(got[c]-expected)<2e-4f*(1+fabs(expected)));
                if(clamped==1) Check(got[c]==single[c]);
            }
            Check(got[3]==o[3]); ++layerChecks;
        }
    }
    printf("PASS self layers: %u GPU cases; fractional 1..3 and finite/clamp, residual after colour restoration, default exact, HDR/black/alpha\n",layerChecks);
    // Independent contract: copying the actual encoded texture instead of
    // running NR must preserve scene-linear RGB, even above the encoding knee.
    // Use exactly representable half-float input values and allow only the
    // quantization error of the production RGBA16F encoded texture.
    unsigned roundTrips=0;
    float maxRelativeError=0;
    const std::array<float,4> hdrCases[]={
        {.5f,.5f,.5f,.5f},{1,1,1,.25f},{16,16,16,1},{256,16,2,.75f},
        {12,3,1,.5f},{.25f,.125f,.0625f,1},{0,16,1,1},{0,0,0,1},
        {0x1p-20f,0x1p-20f,0x1p-20f,1},{.03125f,.0625f,.125f,.25f}};
    for(uint32_t curve:{0u,ComputePasses::FLAG_PURE_GAMMA})
        for(float wp:{.25f,1.0f,4.0f,64.0f}) for(float strength:{0.0f,.5f,1.0f})
            for(const auto& original:hdrCases) {
                const auto got=run(3,strength,original,{},wp,curve);
                for(int c=0;c<3;++c) {
                    const float error=fabs(got[c]-original[c]);
                    maxRelativeError=std::max(maxRelativeError,error/std::max(original[c],1e-4f));
                    if(!std::isfinite(got[c]) || error>0.003f*original[c]+2e-5f) {
                        printf("FAIL neutral round-trip: curve=%u wp=%g colour=%g channel=%d original=%g got=%g\n",
                            curve,wp,strength,c,original[c],got[c]);
                        Check(false);
                    }
                }
                Check(got[3]==original[3]); ++roundTrips;
            }
    printf("PASS neutral HDR: %u GPU encode/resolve cases, max relative error=%g; highlight/chroma/black/alpha\n",
        roundTrips,maxRelativeError);
    // Check that automatic exposure targets the actual encoder's mid-grey,
    // rather than a parameter of a curve no longer used by the shader.
    for(uint32_t curve:{0u,ComputePasses::FLAG_PURE_GAMMA}) for(float grey:{.0625f,.5f,8.0f}) {
        const float wp=grey/NrSampleTargetLinear(curve!=0,2.2f);
        const auto encoded=run(4,1,{grey,grey,grey,1},{},wp,curve);
        for(int c=0;c<3;++c) Check(fabs(encoded[c]-.5f)<.001f);
    }
    // A real model residual must still change the picture; fixing neutrality
    // must not accidentally turn the resolve into an unconditional bypass.
    const auto darker=run(2,1,{.5f,.5f,.5f,1},{.75f,.75f,.75f,1},.25f);
    const auto black=run(2,1,{.5f,.5f,.5f,1},{0,0,0,1},.25f);
    for(int c=0;c<3;++c) { Check(darker[c]>.1f && darker[c]<.4f); Check(fabs(black[c])<1e-5f); }
    puts("PASS exposure target: six GPU mid-grey cases; model residual remains effective");
    for(UINT64 i=0;i<info->GetNumStoredMessages();++i) {
        SIZE_T bytes=0; info->GetMessage(i,nullptr,&bytes); std::vector<char> data(bytes); auto* msg=(D3D12_MESSAGE*)data.data(); HR(info->GetMessage(i,msg,&bytes));
        if(msg->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) { puts(msg->pDescription); Check(false); }
    }
    CloseHandle(event); printf("PASS colour: %u GPU cases; direct/ratio/decode, 0/0.5/1, HDR/black/near-black, alpha, exact full-strength endpoint; API errors=0\n",checks);
    return 0;
} catch(const std::exception& e) { printf("FAIL colour: %s\n",e.what()); return 1; }
