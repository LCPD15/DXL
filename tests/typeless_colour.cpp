// Actual production compute shaders: typed and typeless color storage must
// produce identical pixels, including the second input used by colour/layers.
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <cstring>
#include <cmath>
#include <cstdio>
#include <stdexcept>
#include "ComputePasses.h"
using Microsoft::WRL::ComPtr;
using namespace DXL;
static void Check(bool ok,const char* reason) { if(!ok) throw std::runtime_error(reason); }
static void HR(HRESULT hr) { Check(SUCCEEDED(hr),"D3D12 call"); }
int main() try {
    ComPtr<ID3D12Debug> debug; HR(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
    ComPtr<IDXGIFactory4> factory; HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
    ComPtr<IDXGIAdapter> warp; HR(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
    ComPtr<ID3D12Device> device; HR(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));
    ComPtr<ID3D12InfoQueue> info; HR(device.As(&info));
    ComPtr<ID3D12CommandQueue> queue; D3D12_COMMAND_QUEUE_DESC qd{}; HR(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)));
    ComPtr<ID3D12CommandAllocator> allocator; HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
    ComPtr<ID3D12GraphicsCommandList> list; HR(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))); HR(list->Close());
    ComPtr<ID3D12Fence> fence; HR(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))); UINT64 serial=0;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr); Check(event!=nullptr,"fence event");
    auto barrier=[&](ID3D12Resource* r,D3D12_RESOURCE_STATES from,D3D12_RESOURCE_STATES to) {
        D3D12_RESOURCE_BARRIER b{}; b.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,from,to}; list->ResourceBarrier(1,&b);
    };
    auto submit=[&] {
        HR(list->Close()); ID3D12CommandList* lists[]{list.Get()}; queue->ExecuteCommandLists(1,lists);
        HR(queue->Signal(fence.Get(),++serial)); HR(fence->SetEventOnCompletion(serial,event));
        Check(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"GPU timeout");
        Check(fence->GetCompletedValue()!=UINT64_MAX,"device removed");
        for(UINT64 i=0;i<info->GetNumStoredMessages();++i) {
            SIZE_T size=0; info->GetMessage(i,nullptr,&size); std::vector<char> bytes(size);
            auto* msg=reinterpret_cast<D3D12_MESSAGE*>(bytes.data()); HR(info->GetMessage(i,msg,&size));
            if(msg->Severity<=D3D12_MESSAGE_SEVERITY_ERROR) { puts(msg->pDescription); Check(false,"API validation"); }
        }
        info->ClearStoredMessages();
    };
    constexpr UINT W=8,H=8;
    auto texture=[&](DXGI_FORMAT format,D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES state) {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td{}; td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width=W; td.Height=H;
        td.DepthOrArraySize=td.MipLevels=1; td.Format=format; td.SampleDesc.Count=1; td.Flags=flags;
        ComPtr<ID3D12Resource> r; HR(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&td,state,nullptr,IID_PPV_ARGS(&r))); return r;
    };
    auto output=texture(DXGI_FORMAT_R32G32B32A32_FLOAT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width=256*H; bd.Height=1;
    bd.DepthOrArraySize=bd.MipLevels=1; bd.SampleDesc.Count=1; bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback; HR(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)));
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV; hd.NumDescriptors=4;
    ComPtr<ID3D12DescriptorHeap> views; HR(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&views)));
    const UINT stride=device->GetDescriptorHandleIncrementSize(hd.Type);
    ComputePasses passes; Check(passes.Initialize(device.Get()),"compute init");
    unsigned cases=0;
    for(bool bgra:{false,true}) {
        const auto typed=bgra?DXGI_FORMAT_B8G8R8A8_UNORM:DXGI_FORMAT_R8G8B8A8_UNORM;
        const auto typeless=bgra?DXGI_FORMAT_B8G8R8A8_TYPELESS:DXGI_FORMAT_R8G8B8A8_TYPELESS;
        std::array<ComPtr<ID3D12Resource>,4> inputs;
        HR(allocator->Reset()); HR(list->Reset(allocator.Get(),nullptr));
        for(unsigned i=0;i<inputs.size();++i) {
            inputs[i]=texture(i%2?typeless:typed,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET);
            auto rtv=views->GetCPUDescriptorHandleForHeapStart(); rtv.ptr+=i*stride;
            D3D12_RENDER_TARGET_VIEW_DESC desc{}; desc.Format=typed; desc.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
            device->CreateRenderTargetView(inputs[i].Get(),&desc,rtv);
            for(UINT y=0;y<H;++y) for(UINT x=0;x<W;++x) {
                const float shift=i>=2?23.0f:0.0f;
                const float rgba[]{(13+x*19+shift)/255.0f,(7+y*23+shift)/255.0f,(3+x*7+y*11+shift)/255.0f,(64+x*9)/255.0f};
                D3D12_RECT rect{LONG(x),LONG(y),LONG(x+1),LONG(y+1)}; list->ClearRenderTargetView(rtv,rgba,1,&rect);
            }
            barrier(inputs[i].Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        }
        submit();
        auto run=[&](unsigned pass,unsigned selection) {
            HR(allocator->Reset()); HR(list->Reset(allocator.Get(),nullptr));
            auto* a=inputs[selection&1].Get(); auto* b=inputs[2+((selection>>1)&1)].Get();
            bool ok=false;
            switch(pass) {
            case 0: ok=passes.RecordEncode(list.Get(),a,output.Get(),W,H,4,2.2f); break;
            case 1: ok=passes.RecordDecodeRatio(list.Get(),a,b,output.Get(),W,H,4,2.2f,0,.35f,2.25f); break;
            case 2: ok=passes.RecordColourBlend(list.Get(),a,b,output.Get(),W,H,1); break;
            case 3: ok=passes.RecordColourBlend(list.Get(),a,b,output.Get(),W,H,0,1.5f); break;
            case 4: ok=passes.RecordBoxDownsample(list.Get(),a,output.Get(),W/2,H/2,2); break;
            case 5: ok=passes.RecordRatioMake(list.Get(),a,b,output.Get(),W,H); break;
            case 6: ok=passes.RecordRatioApply(list.Get(),a,b,output.Get(),W,H,.5f,2); break;
            case 7: ok=passes.RecordVisualizeRaw(list.Get(),a,output.Get(),W,H,1); break;
            }
            Check(ok,"production shader rejected color SRV");
            barrier(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION src{},dst{}; src.pResource=output.Get(); dst.pResource=readback.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Footprint={DXGI_FORMAT_R32G32B32A32_FLOAT,W,H,1,256}; list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
            barrier(output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS); submit();
            void* data=nullptr; HR(readback->Map(0,nullptr,&data));
            std::vector<float> result;
            const UINT size=pass==4?W/2:W;
            for(UINT y=0;y<size;++y) {
                const auto* row=reinterpret_cast<const float*>(static_cast<const char*>(data)+y*256);
                result.insert(result.end(),row,row+size*4);
            }
            readback->Unmap(0,nullptr); return result;
        };
        for(unsigned pass=0;pass<8;++pass) {
            const auto control=run(pass,0);
            for(unsigned selection=1;selection<4;++selection) {
                const auto result=run(pass,selection);
                Check(result==control,"typed/typeless output pixels differ");
                for(float value:result) Check(std::isfinite(value),"nonfinite color output");
                ++cases;
            }
            if(pass==2 || pass==7) for(UINT y=0;y<H;++y) for(UINT x=0;x<W;++x) {
                const float shift=pass==2?23.0f:0.0f;
                const float expected[]{(13+x*19+shift)/255.0f,(7+y*23+shift)/255.0f,(3+x*7+y*11+shift)/255.0f};
                for(UINT c=0;c<3;++c) Check(std::abs(control[(y*W+x)*4+c]-expected[c])<1e-6f,"UNORM channels/transfer function changed");
            }
        }
        Check(inputs[1]->GetDesc().Format==typeless && inputs[3]->GetDesc().Format==typeless,"borrowed resource format changed");
        printf("PASS format family=%u: typed/typeless primary and second SRV, eight production shaders, exact output\n",unsigned(typeless));
    }
    // Unity supplies signed normalized vectors. Verify sign and zero against
    // independently computed display values, not just successful dispatch.
    auto motion=texture(DXGI_FORMAT_R16G16_SNORM,D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET,D3D12_RESOURCE_STATE_RENDER_TARGET);
    auto motionView=views->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(motion.Get(),nullptr,motionView);
    for (float x : {-.02f,0.0f,.02f}) {
        HR(allocator->Reset()); HR(list->Reset(allocator.Get(),nullptr));
        const float value[]{x,-x,0,1};
        list->ClearRenderTargetView(motionView,value,0,nullptr);
        barrier(motion.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Check(passes.RecordVisualize(list.Get(),motion.Get(),output.Get(),W,H,10,true),"SNORM motion visualization rejected");
        barrier(output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION src{},dst{}; src.pResource=output.Get(); dst.pResource=readback.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Footprint={DXGI_FORMAT_R32G32B32A32_FLOAT,W,H,1,256}; list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        barrier(output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barrier(motion.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
        submit();
        void* data=nullptr; HR(readback->Map(0,nullptr,&data));
        for(UINT y=0;y<H;++y) {
            const auto* row=reinterpret_cast<const float*>(static_cast<const char*>(data)+y*256);
            for(UINT i=0;i<W;++i) {
                Check(std::abs(row[i*4]-(.5f+x*10))<.0004f,"SNORM X direction/value lost");
                Check(std::abs(row[i*4+1]-(.5f-x*10))<.0004f,"SNORM Y direction/value lost");
                Check(std::abs(row[i*4+2]-.5f)<.0001f,"stationary motion reference lost");
            }
        }
        readback->Unmap(0,nullptr);
    }
    puts("PASS SNORM motion: negative/zero/positive vectors, signed directions, neutral grey, API errors=0");
    CloseHandle(event);
    printf("PASS typeless color: %u comparisons, RGBA8/BGRA8, raw UNORM channels, no implicit gamma, API errors=0\n",cases);
    return 0;
} catch(const std::exception& e) { printf("FAIL typeless color: %s\n",e.what()); return 1; }
