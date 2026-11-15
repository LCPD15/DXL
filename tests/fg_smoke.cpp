#include "DlssFgBackend.h"
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
    d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; d.Width=w; d.Height=h; d.MipLevels=d.DepthOrArraySize=d.SampleDesc.Count=1; d.Format=format;
    auto color=resource(d,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_COMMON);
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{}; UINT64 bytes{};
    dev->GetCopyableFootprints(&d,0,1,0,&fp,nullptr,nullptr,&bytes);
    d.Format=DXGI_FORMAT_UNKNOWN; d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; d.Width=bytes; d.Height=1; d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    auto upload=resource(d,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
    auto readback=resource(d,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION input{upload.Get(),D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    input.PlacedFootprint=fp;
    D3D12_TEXTURE_COPY_LOCATION output=input; output.pResource=readback.Get();
    D3D12_TEXTURE_COPY_LOCATION img{color.Get(),D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    DXL::DlssFgBackend fg;
    unsigned valid=0, novel=0;
    unsigned expected=0, ordered=0;
    unsigned validMode[5]{};
    auto image = [ten](unsigned x,unsigned y,int offset) {
        x = (x + 2048 - offset) % 1024;
        unsigned n= ((x/13 + y/21)%4)*60 + (x^y)%60;
        return ten ? 0xc0000000u | ((4*n)<<20) | ((4*n)<<10) | (4*n) : 0xff000000u | n*0x010101;
    };
    for (int frame=0; frame<24; ++frame) {
        uint8_t* data=nullptr;
        Check(upload->Map(0,nullptr,reinterpret_cast<void**>(&data)));
        for (unsigned y=0;y<h;++y) for(unsigned x=0;x<w;++x)
            reinterpret_cast<UINT*>(data+y*fp.Footprint.RowPitch)[x]=image(x,y,frame*12);
        upload->Unmap(0,nullptr);
        // Implicit COMMON -> COPY_DEST promotion of a texture write needs explicit restoration.
        list->CopyTextureRegion(&img,0,0,0,&input,nullptr);
        D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition={color.Get(),0,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON};
        list->ResourceBarrier(1,&b); submit();
        const unsigned multiplier = frame < 6 ? 2 : frame < 12 ? 3 : frame < 18 ? 4 : 2;
        ID3D12Resource* mid[3]{};
        if (frame == 19) fg.ResetHistory();
        if (frame == 21) fg.Reset();
        if (!fg.Generate(q.Get(),color.Get(),GetModuleHandleW(nullptr),mid,ten,multiplier)) {
            if (fg.Error()) { printf("FG failure: %s\n",fg.Error()); return 1; }
            continue;
        }
        ++validMode[multiplier];
        ++valid;
        int lastOffset = -1;
        bool inOrder = true;
        for (unsigned k = 0; k < multiplier-1; ++k) {
            ++expected;
            if (!mid[k] || (k && mid[k] == mid[k-1])) return 6;
            auto src=img; src.pResource=mid[k];
            list->CopyTextureRegion(&output,0,0,0,&src,nullptr); submit();
            Check(readback->Map(0,nullptr,reinterpret_cast<void**>(&data)));
            unsigned count=0, bright=0;
            std::array<unsigned,13> error{};
            for(unsigned y=0;y<h;y+=11) for(unsigned x=15;x<w;x+=17) {
                auto generated=reinterpret_cast<UINT*>(data+y*fp.Footprint.RowPitch)[x];
                auto sample=[ten](UINT v){return int(ten ? (v&1023u)/4 : v&255u);};
                int v=sample(generated);
                int a=sample(image(x,y,12*(frame-1))), c=sample(image(x,y,12*frame));
                if (std::abs(v-a)>4 && std::abs(v-c)>4) ++count;
                if (v>10 && v<240) ++bright;
                for (int pos=0;pos<=12;++pos)
                    error[pos] += std::abs(v-sample(image(x,y,12*(frame-1)+pos)));
            }
            readback->Unmap(0,nullptr);
            int fit=int(std::min_element(error.begin(),error.end())-error.begin());
            if (count>100 && bright>1000) ++novel;
            int target=12*(k+1)/multiplier;
            inOrder &= fit > lastOffset && std::abs(fit-target)<=1;
            lastOffset=fit;
            if(frame==3 || frame==9 || frame==15)
                printf("Position %ux sub %u expected %d actual %d pixels\n",multiplier,k+1,target,fit);
        }
        ordered+=inOrder;
    }
    printf("DLSSG format=%u: valid pairs=%u/%u/%u (2x/3x/4x), %u/%u novel images, %u/%u in phase\n",
        unsigned(format),validMode[2],validMode[3],validMode[4],novel,expected,ordered,valid);
    if (!validMode[2] || !validMode[3] || !validMode[4] || novel != expected || ordered*4 < valid*3) return 2;
    fg.Reset();
    DXL::DlssNrFilter nr;
    if (!nr.Initialize(dev.Get(), q.Get(), GetModuleHandleW(nullptr))) return 4;
    DXL::NrSettings settings{};
    settings.enabled = true;
    DXL::ForcedNrOutput12 finalOutput;
    if (!finalOutput.Execute(nr, color.Get(), q.Get(), settings, true) || nr.EvaluateCount() != 1) {
        printf("NR after FG shutdown failed: %s\n", nr.LastError());
        return 5;
    }
    puts("NR after FG restart and shutdown: 1 evaluation");
    return 0;
} catch (const std::exception& e) { printf("%s\n",e.what()); return 3; }
