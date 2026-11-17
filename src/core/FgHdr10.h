#pragma once
#include <d3dcompiler.h>
#include <wrl/client.h>

namespace DXL {
// Convert linear scRGB (1 = 80 nits, Rec.709) to the HDR10 contract used by DLSSG.
struct FgHdr10 {
    template<class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D12Resource> image;
    Ptr<ID3D12RootSignature> root;
    Ptr<ID3D12PipelineState> pso;
    Ptr<ID3D12DescriptorHeap> heap;
    Ptr<ID3D12CommandAllocator> allocator;
    Ptr<ID3D12GraphicsCommandList> list;
    Ptr<ID3D12Fence> fence;
    UINT64 serial = 0;
    bool Init(ID3D12Device* device, UINT w, UINT h) {
        const char* shader = R"(
Texture2D<float4> src:register(t0);
RWTexture2D<float4> dst:register(u0);
float3 pq(float3 v) {
 v=pow(saturate(v*0.008),2610.0/16384.0);
 return pow((3424.0/4096.0+(2413.0/128.0)*v)/(1+(2392.0/128.0)*v),2523.0/32.0);
}
[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID) {
 uint w,h;dst.GetDimensions(w,h);if(id.x>=w||id.y>=h)return;
 float3 c=src[id.xy].rgb;
 float3 r=float3(dot(c,float3(.627404,.329283,.043313)),
                 dot(c,float3(.069097,.91954,.011362)),
                 dot(c,float3(.016391,.088013,.895595)));
 dst[id.xy]=float4(pq(max(r,0)),1);
})";
        Ptr<ID3DBlob> code, blob, errors;
        if (FAILED(D3DCompile(shader,strlen(shader),nullptr,nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors))) return false;
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};
        ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,1};
        D3D12_ROOT_PARAMETER param{};
        param.ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        param.DescriptorTable={2,ranges};
        D3D12_ROOT_SIGNATURE_DESC signature{1,&param};
        if (FAILED(D3D12SerializeRootSignature(&signature,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&errors)) ||
            FAILED(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)))) return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
        pipeline.pRootSignature=root.Get();pipeline.CS={code->GetBufferPointer(),code->GetBufferSize()};
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,2,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;hp.CreationNodeMask=hp.VisibleNodeMask=1;
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;rd.Width=w;rd.Height=h;
        rd.DepthOrArraySize=rd.MipLevels=rd.SampleDesc.Count=1;rd.Format=DXGI_FORMAT_R10G10B10A2_UNORM;
        rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        return SUCCEEDED(device->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&pso))) &&
            SUCCEEDED(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap))) &&
            SUCCEEDED(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&image))) &&
            SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))) &&
            SUCCEEDED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))) &&
            SUCCEEDED(list->Close()) && SUCCEEDED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));
    }
    bool Convert(ID3D12CommandQueue* queue, ID3D12Resource* src) {
        if (serial && FAILED(fence->SetEventOnCompletion(serial,nullptr))) return false;
        Ptr<ID3D12Device> device;queue->GetDevice(IID_PPV_ARGS(&device));
        auto handle=heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
        srv.Format=src->GetDesc().Format;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
        device->CreateShaderResourceView(src,&srv,handle);
        handle.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        device->CreateUnorderedAccessView(image.Get(),nullptr,nullptr,handle);
        if (FAILED(allocator->Reset())||FAILED(list->Reset(allocator.Get(),pso.Get()))) return false;
        D3D12_RESOURCE_BARRIER barriers[2]{};
        for(auto& b:barriers){b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;}
        barriers[0].Transition.pResource=src;barriers[0].Transition.StateAfter=D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
        barriers[1].Transition.pResource=image.Get();barriers[1].Transition.StateAfter=D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        list->ResourceBarrier(2,barriers);
        ID3D12DescriptorHeap* heaps[]{heap.Get()};list->SetDescriptorHeaps(1,heaps);
        list->SetComputeRootSignature(root.Get());list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
        const auto d=image->GetDesc();list->Dispatch((UINT(d.Width)+7)/8,(d.Height+7)/8,1);
        for(auto& b:barriers)std::swap(b.Transition.StateBefore,b.Transition.StateAfter);
        list->ResourceBarrier(2,barriers);
        if (FAILED(list->Close()))return false;
        ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
        return SUCCEEDED(queue->Signal(fence.Get(),++serial));
    }
};
}
