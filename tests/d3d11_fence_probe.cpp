#include <windows.h>
#include <d3d10_1.h>
#include <d3d11_4.h>
#include <d3d11sdklayers.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <string>
using Microsoft::WRL::ComPtr;
static unsigned successes=0, failures=0, expectedRejections=0, optionalUnsupported=0;
static HRESULT lastFailure=S_OK;
static bool HR(HRESULT hr,const char* step) {
    if(FAILED(hr)){lastFailure=hr;printf("  %s hr=0x%08X\n",step,unsigned(hr));}
    return SUCCEEDED(hr);
}
static bool FenceDone(ID3D12Fence* fence,UINT64 value) {
    if(fence->GetCompletedValue()>=value) return true;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
    if(!event) return false;
    const auto hr=fence->SetEventOnCompletion(value,event);
    const DWORD wait=SUCCEEDED(hr)?WaitForSingleObject(event,3000):WAIT_FAILED;
    CloseHandle(event);
    if(wait!=WAIT_OBJECT_0) printf("  GPU completion failed hr=0x%08X wait=%lu\n",unsigned(hr),wait);
    return wait==WAIT_OBJECT_0;
}
static unsigned Debug11(ID3D11Device* device) {
    ComPtr<ID3D11InfoQueue> info;if(FAILED(device->QueryInterface(IID_PPV_ARGS(&info)))) return 0;
    unsigned errors=0,reported=0;
    for(UINT64 i=0;i<info->GetNumStoredMessagesAllowedByRetrievalFilter();++i) {
        SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<unsigned char> data(size);
        auto* m=reinterpret_cast<D3D11_MESSAGE*>(data.data());info->GetMessage(i,m,&size);
        if(m->Severity<=D3D11_MESSAGE_SEVERITY_WARNING) {
            if(m->Severity<=D3D11_MESSAGE_SEVERITY_ERROR)++errors;
            if(reported++<8) printf("  D3D11 debug severity=%u id=%u %s\n",unsigned(m->Severity),unsigned(m->ID),m->pDescription);
        }
    }
    printf("  D3D11 debug errors=%u\n",errors);
    info->ClearStoredMessages();
    return errors;
}
static unsigned Debug12(ID3D12Device* device) {
    ComPtr<ID3D12InfoQueue> info;if(FAILED(device->QueryInterface(IID_PPV_ARGS(&info))))return 0;
    unsigned errors=0;
    for(UINT64 i=0;i<info->GetNumStoredMessages();++i){
        SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<unsigned char> data(size);
        auto* m=reinterpret_cast<D3D12_MESSAGE*>(data.data());info->GetMessage(i,m,&size);
        if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){++errors;printf("  D3D12 debug id=%u %s\n",unsigned(m->ID),m->pDescription);}
    }
    printf("  D3D12 debug errors=%u\n",errors);return errors;
}
struct StateScope {
    ComPtr<ID3D11DeviceContext1> context;ComPtr<ID3DDeviceContextState> previous;
    bool active=false;
    bool Enter(ID3D11Device* device,REFIID iid) {
        ComPtr<ID3D11Device1> device1;device->QueryInterface(IID_PPV_ARGS(&device1));
        ComPtr<ID3D11DeviceContext> immediate;device->GetImmediateContext(&immediate);
        if(!device1||FAILED(immediate.As(&context))) return false;
        const auto level=device->GetFeatureLevel();D3D_FEATURE_LEVEL chosen{};
        ComPtr<ID3DDeviceContextState> state;
        if(!HR(device1->CreateDeviceContextState(device->GetCreationFlags()&D3D11_CREATE_DEVICE_SINGLETHREADED,
            &level,1,D3D11_SDK_VERSION,iid,&chosen,&state),"CreateDeviceContextState"))return false;
        context->SwapDeviceContextState(state.Get(),&previous);active=true;
        printf("  Scoped D3D11 state chosenFL=0x%X previous=%p\n",unsigned(chosen),previous.Get());
        return true;
    }
    ~StateScope(){if(active)context->SwapDeviceContextState(previous.Get(),nullptr);}
};
struct FencePair {ComPtr<ID3D12Fence> f12;ComPtr<ID3D11Fence> f11;};
static bool SharedFence(ID3D11Device5* d11,ID3D12Device* d12,bool originate11,FencePair& fence) {
    HANDLE handle=nullptr;HRESULT hr;
    if(originate11) {
        hr=d11->CreateFence(0,D3D11_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence.f11));
        if(!HR(hr,"D3D11 CreateFence"))return false;
        hr=fence.f11->CreateSharedHandle(nullptr,GENERIC_ALL,nullptr,&handle);
        if(!HR(hr,"D3D11 fence CreateSharedHandle"))return false;
        hr=d12->OpenSharedHandle(handle,IID_PPV_ARGS(&fence.f12));
    } else {
        hr=d12->CreateFence(0,D3D12_FENCE_FLAG_SHARED,IID_PPV_ARGS(&fence.f12));
        if(!HR(hr,"D3D12 CreateFence"))return false;
        hr=d12->CreateSharedHandle(fence.f12.Get(),nullptr,GENERIC_ALL,nullptr,&handle);
        if(!HR(hr,"D3D12 fence CreateSharedHandle"))return false;
        hr=d11->OpenSharedFence(handle,IID_PPV_ARGS(&fence.f11));
    }
    CloseHandle(handle);return HR(hr,"Open shared fence");
}
static bool SharedTexture(ID3D11Device* d11,ID3D12Device* d12,ComPtr<ID3D11Texture2D>& t11,ComPtr<ID3D12Resource>& t12) {
    D3D11_TEXTURE2D_DESC desc{};desc.Width=desc.Height=16;desc.MipLevels=desc.ArraySize=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;
    desc.BindFlags=D3D11_BIND_SHADER_RESOURCE;desc.MiscFlags=D3D11_RESOURCE_MISC_SHARED|D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    if(!HR(d11->CreateTexture2D(&desc,nullptr,&t11),"Create shared texture"))return false;
    ComPtr<IDXGIResource1> resource;if(!HR(t11.As(&resource),"shared resource interface"))return false;
    HANDLE handle=nullptr;
    if(!HR(resource->CreateSharedHandle(nullptr,DXGI_SHARED_RESOURCE_READ|DXGI_SHARED_RESOURCE_WRITE,nullptr,&handle),"texture CreateSharedHandle"))return false;
    const auto hr=d12->OpenSharedHandle(handle,IID_PPV_ARGS(&t12));CloseHandle(handle);
    return HR(hr,"Open shared texture12");
}
static bool Run(ID3D11Device* device,bool originate11,int scoped) {
    lastFailure=S_OK;
    StateScope state;
    if(scoped&&!state.Enter(device,scoped==2?__uuidof(ID3D11Device1):__uuidof(ID3D11Device)))return false;
    ComPtr<ID3D11DeviceContext> context;device->GetImmediateContext(&context);
    ComPtr<ID3D11DeviceContext4> context4;ComPtr<ID3D11Device5> device5;
    if(!HR(context.As(&context4),"QI context4")||!HR(device->QueryInterface(IID_PPV_ARGS(&device5)),"QI device5"))return false;
    printf("  flags=0x%X feature=0x%X contextType=%u origin=%s scope=%d\n",device->GetCreationFlags(),unsigned(device->GetFeatureLevel()),unsigned(context->GetType()),originate11?"D3D11":"D3D12",scoped);
    ComPtr<IDXGIDevice> dxgi;ComPtr<IDXGIAdapter> adapter;
    if(!HR(device->QueryInterface(IID_PPV_ARGS(&dxgi)),"QI DXGI device")||!HR(dxgi->GetAdapter(&adapter),"adapter"))return false;
    ComPtr<ID3D12Device> d12;if(!HR(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d12)),"Create private12"))return false;
    ComPtr<ID3D12CommandQueue> queue;D3D12_COMMAND_QUEUE_DESC qd{};
    if(!HR(d12->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)),"CreateQueue"))return false;
    FencePair inFence,outFence;
    if(!SharedFence(device5.Get(),d12.Get(),originate11,inFence)||!SharedFence(device5.Get(),d12.Get(),originate11,outFence))return false;
    // Probe before any resource copy, so invalid copy parameters cannot explain
    // a failure of this first immediate-context Signal.
    const auto empty=context4->Signal(inFence.f11.Get(),1);
    printf("  empty Signal=0x%08X removed11=0x%08X removed12=0x%08X\n",unsigned(empty),unsigned(device->GetDeviceRemovedReason()),unsigned(d12->GetDeviceRemovedReason()));
    if(FAILED(empty)){lastFailure=empty;return false;}
    context->Flush();if(!FenceDone(inFence.f12.Get(),1))return false;
    ComPtr<ID3D11Texture2D> color11,out11,input,readback;ComPtr<ID3D12Resource> color12,out12;
    if(!SharedTexture(device,d12.Get(),color11,color12)||!SharedTexture(device,d12.Get(),out11,out12))return false;
    std::vector<unsigned> pixels(256);for(unsigned i=0;i<pixels.size();++i)pixels[i]=0xFF000000u|(i<<16)|(0x5Au<<8)|(255-i);
    D3D11_TEXTURE2D_DESC td{};td.Width=td.Height=16;td.MipLevels=td.ArraySize=1;td.Format=DXGI_FORMAT_R8G8B8A8_UNORM;td.SampleDesc.Count=1;
    D3D11_SUBRESOURCE_DATA init{pixels.data(),16*4,0};
    if(!HR(device->CreateTexture2D(&td,&init,&input),"Create input"))return false;
    td.Usage=D3D11_USAGE_STAGING;td.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    if(!HR(device->CreateTexture2D(&td,nullptr,&readback),"Create readback"))return false;
    context->CopyResource(color11.Get(),input.Get());
    if(!HR(context4->Signal(inFence.f11.Get(),2),"copy-in Signal"))return false;
    context->Flush();
    if(!HR(queue->Wait(inFence.f12.Get(),2),"queue Wait"))return false;
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    if(!HR(d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"allocator")||
        !HR(d12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"list"))return false;
    D3D12_RESOURCE_BARRIER barriers[2]{};
    for(auto& b:barriers){b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;b.Transition.StateBefore=D3D12_RESOURCE_STATE_COMMON;}
    barriers[0].Transition.pResource=color12.Get();barriers[0].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
    barriers[1].Transition.pResource=out12.Get();barriers[1].Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_DEST;
    list->ResourceBarrier(2,barriers);list->CopyResource(out12.Get(),color12.Get());
    for(auto& b:barriers)std::swap(b.Transition.StateBefore,b.Transition.StateAfter);
    list->ResourceBarrier(2,barriers);list->Close();ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
    if(!HR(queue->Signal(outFence.f12.Get(),1),"queue Signal"))return false;
    if(!HR(context4->Wait(outFence.f11.Get(),1),"copy-out Wait"))return false;
    context->CopyResource(readback.Get(),out11.Get());
    if(!HR(context4->Signal(inFence.f11.Get(),3),"terminal Signal"))return false;
    context->Flush();
    if(!FenceDone(inFence.f12.Get(),3)) { puts("FATAL GPU dependency timed out; exiting only this isolated fixture");ExitProcess(3); }
    D3D11_MAPPED_SUBRESOURCE mapped{};if(!HR(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped),"Map completed copy"))return false;
    unsigned different=0;for(unsigned y=0;y<16;++y)for(unsigned x=0;x<16;++x)
        if(reinterpret_cast<const unsigned*>(static_cast<const unsigned char*>(mapped.pData)+y*mapped.RowPitch)[x]!=pixels[y*16+x])++different;
    context->Unmap(readback.Get(),0);
    printf("  pixel mismatches=%u (256 checked)\n",different);
    return !different&&Debug12(d12.Get())==0;
}
static void Report(ID3D11Device* device,bool origin,int scope,bool rejectExpected=false,bool required=true) {
    const bool ok=Run(device,origin,scope);
    const auto error=lastFailure;const auto debugErrors=Debug11(device);
    if(rejectExpected) {
        if(!ok&&(error==DXGI_ERROR_INVALID_CALL||error==E_INVALIDARG)) {
            ++expectedRejections;puts("  RESULT EXPECTED_OLD_MODE_REJECTION");
        } else {++failures;puts("  RESULT FAIL: old D3D10 mode did not produce the required negative control");}
    } else if(ok&&!debugErrors) {++successes;puts("  RESULT PASS");}
    else if(required) {++failures;puts("  RESULT FAIL: required positive path");}
    else {++optionalUnsupported;puts("  RESULT OPTIONAL_CAPABILITY_UNSUPPORTED");}
}
int main(int argc,char** argv) {
    const bool matrix=argc>1&&std::string(argv[1])=="--matrix";
    ComPtr<ID3D12Debug> debug12;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug12))))debug12->EnableDebugLayer();
    // Priority reproduction: a real D3D10 runtime device exposed through QI11.
    for(const UINT flags:{0u,UINT(D3D10_CREATE_DEVICE_DEBUG),UINT(D3D10_CREATE_DEVICE_SINGLETHREADED|D3D10_CREATE_DEVICE_DEBUG)}) {
        ComPtr<ID3D10Device> d10;const auto hr=D3D10CreateDevice(nullptr,D3D10_DRIVER_TYPE_HARDWARE,nullptr,flags,D3D10_SDK_VERSION,&d10);
        printf("NATIVE10 create flags=0x%X hr=0x%08X\n",flags,unsigned(hr));if(FAILED(hr)){++failures;continue;}
        ComPtr<ID3D11Device> d11;if(!HR(d10.As(&d11),"native10 QI11")){++failures;continue;}
        const D3D10_VIEWPORT sentinel{2,3,11,9,.125f,.875f};
        for(const bool origin:{false,true})for(const int scope:{0,1,2}) {
            d10->RSSetViewports(1,&sentinel);Report(d11.Get(),origin,scope,scope==0);
            UINT count=1;D3D10_VIEWPORT actual{};d10->RSGetViewports(&count,&actual);
            const bool restored=count==1&&memcmp(&actual,&sentinel,sizeof(actual))==0;
            printf("  native10 viewport restored=%u\n",restored?1:0);
            if(!restored)++failures;
        }
    }
    const std::vector<D3D_FEATURE_LEVEL> levels=matrix
        ?std::vector<D3D_FEATURE_LEVEL>{D3D_FEATURE_LEVEL_11_1,D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_1,D3D_FEATURE_LEVEL_10_0,D3D_FEATURE_LEVEL_9_3,D3D_FEATURE_LEVEL_9_1}
        :std::vector<D3D_FEATURE_LEVEL>{D3D_FEATURE_LEVEL_11_0,D3D_FEATURE_LEVEL_10_0};
    for(const auto level:levels)for(const UINT flags:{0u,UINT(D3D11_CREATE_DEVICE_SINGLETHREADED),UINT(D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS)}) {
        ComPtr<ID3D11Device> device;const auto hr=D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,flags|D3D11_CREATE_DEVICE_DEBUG,&level,1,D3D11_SDK_VERSION,&device,nullptr,nullptr);
        printf("NATIVE11 create FL=0x%X flags=0x%X hr=0x%08X\n",unsigned(level),flags,unsigned(hr));
        if(FAILED(hr)){if(level>=D3D_FEATURE_LEVEL_10_0)++failures;else++optionalUnsupported;continue;}
        for(const bool origin:{false,true})Report(device.Get(),origin,0,false,level>=D3D_FEATURE_LEVEL_10_0);
    }
    printf("SUMMARY passes=%u expected_old_mode_rejections=%u failures=%u optional_unsupported=%u\n",successes,expectedRejections,failures,optionalUnsupported);
    return successes>=24&&expectedRejections==6&&failures==0?0:1;
}
