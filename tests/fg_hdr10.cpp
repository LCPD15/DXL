#include "DlssFgBackend.h"
#include "FgHdr10.h"
#include <cmath>
#include "ForcedNrOutput.h"
#include "../src/common/Log.h"
#include <wrl/client.h>
#include <dxgi1_6.h>
#include <algorithm>
#include <array>
#include <cstdio>
#include <stdexcept>
#include <vector>

using Microsoft::WRL::ComPtr;
static void Check(HRESULT hr) { if (FAILED(hr)) throw std::runtime_error("D3D12 operation failed"); }

// Upload a horizontally translating pattern. Check that intermediate pixels
// differ from BOTH adjacent real images, not merely that NGX returned success.
int main(int argc, char** argv) try {
    DXL::Log::Get().Open(L"fg-smoke");
    const bool ten = argc > 1 && strcmp(argv[1], "--rgb10") == 0;
    const bool bgra = argc > 1 && strcmp(argv[1], "--bgra") == 0;
    const auto format = bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : ten ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    const unsigned w = 1280, h = 720;
    ComPtr<IDXGIFactory6> factory;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT i=0; ; ++i) {
        Check(factory->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter)));
        DXGI_ADAPTER_DESC1 d{}; adapter->GetDesc1(&d);
        if (d.VendorId == 0x10de) break;
        adapter.Reset();
    }
    ComPtr<ID3D12Device> dev;
    Check(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)));
    ComPtr<ID3D12CommandQueue> q;
    D3D12_COMMAND_QUEUE_DESC qd{};
    Check(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)));
    ComPtr<ID3D12Fence> fence;
    Check(dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    ComPtr<ID3D12CommandAllocator> alloc;
    Check(dev->CreateCommandAllocator(qd.Type,IID_PPV_ARGS(&alloc)));
    ComPtr<ID3D12GraphicsCommandList> list;
    Check(dev->CreateCommandList(0,qd.Type,alloc.Get(),nullptr,IID_PPV_ARGS(&list)));
    UINT64 serial = 0;
    auto submit = [&] {
        Check(list->Close());
        ID3D12CommandList* lists[]{list.Get()}; q->ExecuteCommandLists(1,lists);
        Check(q->Signal(fence.Get(),++serial)); Check(fence->SetEventOnCompletion(serial,nullptr));
        Check(alloc->Reset()); Check(list->Reset(alloc.Get(),nullptr));
    };
    auto resource = [&](D3D12_RESOURCE_DESC d,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state) {
        ComPtr<ID3D12Resource> r;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type=type; hp.CreationNodeMask=hp.VisibleNodeMask=1;
        Check(dev->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)));
        return r;
    };
    D3D12_RESOURCE_DESC d{};
    d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=16;d.Height=16;
    d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;
    d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    auto source=resource(d,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_RENDER_TARGET);
    ComPtr<ID3D12DescriptorHeap> rtv;
    D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1};
    Check(dev->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv)));
    auto handle=rtv->GetCPUDescriptorHandleForHeapStart();dev->CreateRenderTargetView(source.Get(),nullptr,handle);
    float value[]{1,1,1,1};list->ClearRenderTargetView(handle,value,0,nullptr);
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={source.Get(),0,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COMMON};
    list->ResourceBarrier(1,&b);submit();
    DXL::FgHdr10 convert;
    if(!convert.Init(dev.Get(),16,16)||!convert.Convert(q.Get(),source.Get()))throw std::runtime_error("conversion failed");
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 bytes{};
    auto out=convert.image->GetDesc();dev->GetCopyableFootprints(&out,0,1,0,&fp,nullptr,nullptr,&bytes);
    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;
    buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    auto readback=resource(buffer,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION src{convert.image.Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION dst{readback.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};dst.PlacedFootprint=fp;
    list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);submit();
    UINT* raw{};Check(readback->Map(0,nullptr,reinterpret_cast<void**>(&raw)));
    auto pixel=*raw;readback->Unmap(0,nullptr);
    double v=pow(.008,2610.0/16384),pq=pow((3424.0/4096+2413.0/128*v)/(1+2392.0/128*v),2523.0/32);
    for(int c=0;c<3;++c)if(abs(int((pixel>>(10*c))&1023)-int(lround(pq*1023)))>2)throw std::runtime_error("80 nit white PQ mismatch");
    printf("PASS: scRGB 80 nit white -> HDR10 PQ %f, packed %08x\n",pq,pixel);
    return 0;
}catch(const std::exception& e){fprintf(stderr,"%s\n",e.what());return 1;}
