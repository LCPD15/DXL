// Production grading through D3D12 Present/Evaluate and the D3D11 bridge.
// No game injection or NGX runtime: color grading must stand alone.
#include <windows.h>
#include <d3d11_4.h>
#include <d3d11sdklayers.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <dxgi1_4.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <filesystem>
#include <stdexcept>
#include <cstdio>
#include <cstring>
#include <cmath>
#include "DlssNrFilter11.h"
#include "CommandListTracker.h"
#include "NrColorFormat.h"
#include "FinalPostProcess.h"
#include "NrRoutePolicy.h"
using Microsoft::WRL::ComPtr;
using namespace DXL;
static void Check(bool ok, const char* what) { if (!ok) throw std::runtime_error(what); }
static void HR(HRESULT hr) { if(FAILED(hr)) { std::printf("HRESULT=%08X\n",unsigned(hr)); throw std::runtime_error("Graphics API failed"); } }
static constexpr UINT W=32,H=16;
static constexpr float Color[]{.2f,.5f,.8f,.7f};
namespace DXL {
struct NrLayerTestAccess {
    static void ExpectLatchedMissingRuntime(DlssNrFilter& filter) {
        Check(filter._snippetLoadAttempted&&!filter._snippetInitialized&&!filter._feature,"Missing runtime was not latched");
        // If Prepare retries LoadSnippet, this deliberately invalid module
        // changes LastError. A latched attempt must never inspect it again.
        filter._selfModule=reinterpret_cast<HMODULE>(uintptr_t(1));
    }
};
}
static void Barrier(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    if(before==after)return;
    D3D12_RESOURCE_BARRIER b{}; b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after}; list->ResourceBarrier(1,&b);
}
static void Gray(const std::array<unsigned char,4>& p) {
    Check(std::abs(int(p[0])-int(p[1]))<=1&&std::abs(int(p[1])-int(p[2]))<=1,"Monochrome is not grayscale");
    Check(std::abs(int(p[3])-179)<=1,"Grading changed alpha");
}
static void WriteLut() {
    wchar_t module[32768]{};GetModuleFileNameW(nullptr,module,32768);
    const auto folder=std::filesystem::path(module).parent_path()/L"lut";
    std::filesystem::create_directories(folder);
    const auto path=folder/L"pipeline-test.png";
    ComPtr<IWICImagingFactory> factory;HR(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream;HR(factory->CreateStream(&stream));HR(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder;HR(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder));HR(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> properties;HR(encoder->CreateNewFrame(&frame,&properties));HR(frame->Initialize(properties.Get()));
    HR(frame->SetSize(4,2));WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;HR(frame->SetPixelFormat(&format));
    Check(IsEqualGUID(format,GUID_WICPixelFormat32bppBGRA),"PNG encoder format");
    unsigned char pixels[4*2*4]{};
    for(unsigned b=0;b<2;b++)for(unsigned g=0;g<2;g++)for(unsigned r=0;r<2;r++) {
        auto* p=pixels+(g*4+b*2+r)*4; p[0]=r*255;p[1]=g*255;p[2]=b*255;p[3]=255;
    }
    HR(frame->WritePixels(2,16,sizeof(pixels),pixels));HR(frame->Commit());HR(encoder->Commit());
}
struct Device12 {
    ComPtr<ID3D12Device> device;ComPtr<ID3D12InfoQueue> info;ComPtr<ID3D12InfoQueue1> messages;DWORD messageCookie=0;
    ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12DescriptorHeap> rtv;UINT64 serial=0;HANDLE event=nullptr;
    Device12() {
        ComPtr<IDXGIFactory4> factory;HR(CreateDXGIFactory1(IID_PPV_ARGS(&factory)));
        ComPtr<IDXGIAdapter> warp;HR(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)));
        HR(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)));HR(device.As(&info));
        if(SUCCEEDED(device.As(&messages)))HR(messages->RegisterMessageCallback([](D3D12_MESSAGE_CATEGORY,D3D12_MESSAGE_SEVERITY severity,D3D12_MESSAGE_ID id,LPCSTR description,void*){
            if(severity<=D3D12_MESSAGE_SEVERITY_WARNING)std::printf("D3D12[%u]: %s\n",unsigned(id),description);
        },D3D12_MESSAGE_CALLBACK_FLAG_NONE,nullptr,&messageCookie));
        D3D12_COMMAND_QUEUE_DESC q{};HR(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)));
        HR(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)));
        HR(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)));HR(list->Close());
        HR(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)));event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        D3D12_DESCRIPTOR_HEAP_DESC hd{};hd.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;hd.NumDescriptors=1;HR(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv)));
    }
    ~Device12(){if(messages)messages->UnregisterMessageCallback(messageCookie);if(event)CloseHandle(event);}
    void Validate() {
        for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T n=0;info->GetMessage(i,nullptr,&n);std::vector<char> bytes(n);
            auto* m=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());HR(info->GetMessage(i,m,&n));
            if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){puts(m->pDescription);Check(false,"D3D12 validation error");}}
        info->ClearStoredMessages();
    }
    void Begin(){HR(allocator->Reset());HR(list->Reset(allocator.Get(),nullptr));CommandListTracker::Get().NoteReset(list.Get());}
    void End(DlssNrFilter* filter=nullptr){HR(list->Close());ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
        if(filter)filter->NotifyGradingSubmitted(1,lists);HR(queue->Signal(fence.Get(),++serial));HR(fence->SetEventOnCompletion(serial,event));
        Check(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"D3D12 fence timeout");Validate();}
    ComPtr<ID3D12Resource> Texture(DXGI_FORMAT format=DXGI_FORMAT_R8G8B8A8_UNORM,UINT width=W,UINT height=H) {
        D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=width;d.Height=height;
        d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Format=format;d.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_CLEAR_VALUE clear{};clear.Format=NrColorViewFormat(format);std::memcpy(clear.Color,Color,sizeof(Color));
        ComPtr<ID3D12Resource> r;HR(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_RENDER_TARGET,&clear,IID_PPV_ARGS(&r)));return r;
    }
    ComPtr<ID3D12Resource> Buffer(UINT64 bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_FLAGS flags=D3D12_RESOURCE_FLAG_NONE) {
        D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=1;
        d.DepthOrArraySize=d.MipLevels=d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=flags;
        ComPtr<ID3D12Resource> r;HR(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&d,type==D3D12_HEAP_TYPE_READBACK?D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&r)));return r;
    }
    void Clear(ID3D12Resource* r,DXGI_FORMAT view=DXGI_FORMAT_R8G8B8A8_UNORM){Begin();D3D12_RENDER_TARGET_VIEW_DESC desc{};desc.Format=view;desc.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(r,&desc,rtv->GetCPUDescriptorHandleForHeapStart());list->ClearRenderTargetView(rtv->GetCPUDescriptorHandleForHeapStart(),Color,0,nullptr);End();}
    std::array<unsigned char,4> Read(ID3D12Resource* r,D3D12_RESOURCE_STATES state,DXGI_FORMAT view=DXGI_FORMAT_R8G8B8A8_UNORM) {
        auto readback=Buffer(256*H,D3D12_HEAP_TYPE_READBACK);Begin();Barrier(list.Get(),r,state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=r;b.pResource=readback.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        b.PlacedFootprint.Footprint={view,W,H,1,256};D3D12_BOX box{0,0,0,W,H,1};list->CopyTextureRegion(&b,0,0,0,&a,&box);
        Barrier(list.Get(),r,D3D12_RESOURCE_STATE_COPY_SOURCE,state);End();void* p=nullptr;HR(readback->Map(0,nullptr,&p));
        std::array<unsigned char,4> result;std::memcpy(result.data(),p,4);readback->Unmap(0,nullptr);return result;
    }
};
static void RunSplit12() {
    Device12 d; DlssNrFilter scene; FinalPostProcess12 output;
    Check(scene.Initialize(d.device.Get(),d.queue.Get(),GetModuleHandleW(nullptr)),"Scene initialize");
    NrSettings all;all.enabled=false;all.grading.exposure={true,1};all.grading.monochrome={true,.5f};
    const auto basic=SceneColorSettings(all);const auto final=FinalColorSettings(all.grading);
    Check(basic.grading.AnyBasicActive()&&!basic.grading.AnyFinalActive(),"Final effects leaked into SR image");
    Check(!final.enabled&&!final.grading.AnyBasicActive()&&final.grading.AnyFinalActive(),"Scene/NR leaked into output pass");
    auto image=d.Texture();auto reference=d.Texture();
    // Build the independent final-only reference; final must not apply exposure
    // a second time or use NR's scene/menu gating.
    d.Clear(reference.Get());
    Check(output.Execute(reference.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,d.queue.Get(),GetModuleHandleW(nullptr),all.grading),"Final-only reference");
    const auto expected=d.Read(reference.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);
    const auto countBefore=output.Filter().GradingCount();
    NrRoutePolicy route;route.Native(1000,1);route.Applied(1000);
    Check(!route.UsePresent(2000,false,true),"Fixture should model blocked automatic menu handoff");
    std::array<unsigned char,4> scenePixel{};
    for(unsigned frame=0;frame<6;++frame) {
        d.Clear(image.Get());
        if(frame%2==0) {
            Check(scene.Prepare(W,H,DXGI_FORMAT_R8G8B8A8_UNORM,basic,NrMode::Present),"Scene basic prepare");
            Check(scene.Execute({image.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET},{image.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET}),"Scene basic draw");
            Check(scene.WaitForOwnGpuIdle(),"Scene retire");scenePixel=d.Read(image.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);
            Check(scenePixel[0]>65&&scenePixel[1]>160,"Basic exposure failed");
            // Emulate game UI composition/tone mapping occurring after SR:
            // the first pixel is replaced with its original (ungraded) color.
            d.Begin();D3D12_RENDER_TARGET_VIEW_DESC view{};view.Format=DXGI_FORMAT_R8G8B8A8_UNORM;view.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
            d.device->CreateRenderTargetView(image.Get(),&view,d.rtv->GetCPUDescriptorHandleForHeapStart());
            D3D12_RECT rect{0,0,1,1};d.list->ClearRenderTargetView(d.rtv->GetCPUDescriptorHandleForHeapStart(),Color,1,&rect);d.End();
        }
        // Odd frames have no scene evaluation at all (menu or a physical FG
        // output). Every actual output still receives exactly one final pass.
        Check(output.Execute(image.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,d.queue.Get(),GetModuleHandleW(nullptr),all.grading),"Final failed during scene pause");
        const auto actual=d.Read(image.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);
        for(unsigned c=0;c<4;c++)Check(std::abs(int(actual[c])-int(expected[c]))<=1,"Output effect absent/doubled or scene exposure reached UI");
    }
    Check(scene.GradingCount()==3&&output.Filter().GradingCount()==countBefore+6,"Scene/final dispatch count mismatch");
    Check(scene.ModelCallCount()==0&&output.Filter().ModelCallCount()==0,"Standalone final initialized NR");
    // A final-only configuration leaves all upstream work disabled.
    NrSettings onlyFinal;onlyFinal.grading.monochrome={true,1};
    Check(!SceneColorSettings(onlyFinal).grading.AnyActive(),"Final-only profile activates scene processing");
    ComPtr<ID3D12CommandQueue> nextQueue;D3D12_COMMAND_QUEUE_DESC q{};HR(d.device->CreateCommandQueue(&q,IID_PPV_ARGS(&nextQueue)));
    d.Clear(image.Get());Check(output.Execute(image.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,nextQueue.Get(),GetModuleHandleW(nullptr),onlyFinal.grading),"Verified output queue switch");
    Gray(d.Read(image.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET));
    Check(!GetModuleHandleW(L"nvngx_dlssnr.dll"),"Final pass loaded NGX runtime");
    Check(output.TeardownForExit(),"Final teardown");Check(scene.TeardownForExit(),"Scene teardown");d.Validate();
    puts("PASS: scene/final split; paused scene and physical output frames; UI composed before output; no double grading; queue switch; NR calls=0.");
}
static void Run12() {
    puts("D3D12: creating WARP device");Device12 d;DlssNrFilter f;Check(f.Initialize(d.device.Get(),d.queue.Get(),GetModuleHandleW(nullptr)),"Graphics-only Initialize");
    puts("D3D12: graphics initialized without NR");
    Check(!GetModuleHandleW(L"nvngx_dlssnr.dll"),"Color-only initialization loaded NR runtime");
    NrSettings s;Check(!f.Prepare(W,H,DXGI_FORMAT_R8G8B8A8_UNORM,s,NrMode::Present),"Neutral profile allocated active processing");
    s.grading.monochrome={true,1};auto image=d.Texture();d.Clear(image.Get());
    Check(f.Prepare(W,H,DXGI_FORMAT_R8G8B8A8_UNORM,s,NrMode::Present),"Present grade Prepare");
    for(unsigned i=0;i<9;i++) {
        const bool drawn=f.Execute({image.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET},{image.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET});
        // WARP can exceed the real-time 4ms slot budget. Retire before each
        // assertion so a failed assertion cannot release a borrowed GPU image.
        Check(f.WaitForOwnGpuIdle(),"WARP frame retire");Check(drawn,"Present grading failed");
    }
    Check(f.WaitForOwnGpuIdle(),"Present retire");Gray(d.Read(image.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET));
    Check(f.EvaluateCount()==0&&f.ModelCallCount()==0&&f.GradingCount()==9,"Color-only frame counters include NR");
    puts("D3D12: Present frames passed");
    auto typeless=d.Texture(DXGI_FORMAT_R8G8B8A8_TYPELESS);d.Clear(typeless.Get());
    Check(f.Prepare(W,H,DXGI_FORMAT_R8G8B8A8_TYPELESS,s,NrMode::Present),"Typeless grading Prepare");
    Check(f.Execute({typeless.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET},f.Handoff()),"Grading Handoff");Check(f.WaitForOwnGpuIdle(),"Handoff retire");
    Gray(d.Read(f.Handoff().resource,f.Handoff().state));
    puts("D3D12: typed Handoff passed");
    auto resized=d.Texture(DXGI_FORMAT_R8G8B8A8_UNORM,W*2,H*2);d.Clear(resized.Get());
    Check(f.Prepare(W*2,H*2,DXGI_FORMAT_R8G8B8A8_UNORM,s,NrMode::Present),"Grading resize");
    Check(f.Execute({resized.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET},{resized.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET}),"Grading resized frame");
    Check(f.WaitForOwnGpuIdle(),"Resize retire");Gray(d.Read(resized.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET));
    puts("D3D12: resize passed");
    // A complete root signature/PSO/constants/UAV binding must survive grading.
    ComPtr<ID3DBlob> shader,errors,signature;const char* code="RWStructuredBuffer<uint> o:register(u0);cbuffer C:register(b0){uint v;}[numthreads(1,1,1)]void main(){o[0]=v;}";
    HR(D3DCompile(code,std::strlen(code),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&shader,&errors));
    D3D12_ROOT_PARAMETER args[2]{};args[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;args[0].Constants={0,0,1};
    args[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;args[1].Descriptor={0,0};D3D12_ROOT_SIGNATURE_DESC rd{2,args,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    HR(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&errors));ComPtr<ID3D12RootSignature> root;
    HR(d.device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&root)));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};pd.pRootSignature=root.Get();pd.CS={shader->GetBufferPointer(),shader->GetBufferSize()};ComPtr<ID3D12PipelineState> pso;
    HR(d.device->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso)));auto marker=d.Buffer(4,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    auto markerRead=d.Buffer(4,D3D12_HEAP_TYPE_READBACK);constexpr UINT Magic=0x16C0FFEE;
    auto bind=[&]{auto& t=CommandListTracker::Get();d.list->SetComputeRootSignature(root.Get());t.NoteComputeRootSignature(d.list.Get(),root.Get());
        d.list->SetPipelineState(pso.Get());t.NotePipelineState(d.list.Get(),pso.Get());d.list->SetComputeRoot32BitConstant(0,Magic,0);t.NoteRootConstants(d.list.Get(),false,0,1,&Magic,0);
        d.list->SetComputeRootUnorderedAccessView(1,marker->GetGPUVirtualAddress());t.NoteRootAddress(d.list.Get(),false,1,marker->GetGPUVirtualAddress(),CommandListState::RootArgument::UAV);};
    CommandListTracker::rootsReady=true;NrEvaluateInput input;input.color=image.Get();input.colorState=D3D12_RESOURCE_STATE_RENDER_TARGET;
    input.colorHdrKnown=true;input.colorIsHdr=false;input.subrectWidth=W;input.subrectHeight=H;
    d.Clear(image.Get());Check(f.Prepare(W,H,DXGI_FORMAT_R8G8B8A8_UNORM,s,NrMode::AtEvaluate,true),"Present-to-Evaluate grading Prepare");d.Begin();bind();
    Check(f.ExecuteOnList(d.list.Get(),input),"Grade-only Evaluate requires no motion/depth");d.list->Dispatch(1,1,1);
    Barrier(d.list.Get(),marker.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);d.list->CopyBufferRegion(markerRead.Get(),0,marker.Get(),0,4);
    Barrier(d.list.Get(),marker.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);d.End(&f);
    void* data=nullptr;HR(markerRead->Map(0,nullptr,&data));const UINT result=*static_cast<UINT*>(data);markerRead->Unmap(0,nullptr);Check(result==Magic,"Evaluate did not restore game compute bindings");
    Gray(d.Read(image.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET));
    puts("D3D12: Evaluate and compute state restore passed");
    // Discard the first LUT-upload list, then retry. No cached uploaded state
    // may survive a Reset that abandoned its commands before queue submission.
    s.grading={};s.grading.lutEnabled=true;s.grading.lutFile="pipeline-test.png";d.Clear(image.Get());
    Check(f.Prepare(W,H,DXGI_FORMAT_R8G8B8A8_UNORM,s,NrMode::AtEvaluate,true),"LUT Prepare");d.Begin();bind();
    Check(f.ExecuteOnList(d.list.Get(),input),"First LUT recording");HR(d.list->Close());f.NotifyGradingReset(d.list.Get());d.Begin();bind();
    Check(f.ExecuteOnList(d.list.Get(),input),"LUT retry after abandoned upload");d.End(&f);
    const auto lutPixel=d.Read(image.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);
    Check(std::abs(int(lutPixel[0])-204)<=1&&std::abs(int(lutPixel[1])-128)<=1&&std::abs(int(lutPixel[2])-51)<=1,"PNG LUT mapping after discard");
    s.grading.lutFile="missing-pipeline.png";d.Clear(image.Get());
    Check(f.Prepare(W,H,DXGI_FORMAT_R8G8B8A8_UNORM,s,NrMode::Present,true),"Evaluate-to-Present grading Prepare");
    Check(!f.Execute({image.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET},{image.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET}),"Missing-only LUT should not claim rendered frame");
    Check(f.WaitForOwnGpuIdle(),"Missing LUT retire");const auto unchanged=d.Read(image.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET);
    Check(std::abs(int(unchanged[0])-51)<=1&&std::abs(int(unchanged[2])-204)<=1,"Invalid LUT changed game pixels");
    Check(!f.IsDisabled()&&!GetModuleHandleW(L"nvngx_dlssnr.dll"),"Grading disabled itself or loaded NGX");
    s.enabled=true;s.grading={};s.grading.monochrome={true,1};
    Check(f.Prepare(W,H,DXGI_FORMAT_R8G8B8A8_UNORM,s,NrMode::Present),"Missing NR runtime must allow grading");
    const std::string missingError=f.LastError();NrLayerTestAccess::ExpectLatchedMissingRuntime(f);
    for(unsigned i=0;i<8;i++)Check(f.Prepare(W,H,DXGI_FORMAT_R8G8B8A8_UNORM,s,NrMode::Present),"Repeated missing-runtime grading fallback");
    Check(missingError==f.LastError()&&!f.IsReady(),"Failed NR initialization was retried");
    Check(f.Execute({image.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET},{image.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET}),"Grading failed after missing NR runtime");
    Check(f.WaitForOwnGpuIdle(),"Missing-runtime fallback retire");Gray(d.Read(image.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET));
    Check(f.TeardownForExit(),"Grading teardown");d.Validate();puts("PASS: D3D12 Present, Evaluate, typeless, resize, handoff, bindings, invalid LUT and discarded-upload recovery; NR calls=0.");
}
static void Run11() {
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level{};
    HR(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context));
    ComPtr<ID3D11InfoQueue> info;HR(device.As(&info));FinalPostProcess11 finalOutput;DlssNrFilter11 f;Check(f.Initialize(device.Get(),GetModuleHandleW(nullptr)),"D3D11 grading Initialize");
    ComPtr<ID3D11Query> complete;D3D11_QUERY_DESC query{D3D11_QUERY_EVENT,0};HR(device->CreateQuery(&query,&complete));
    auto wait=[&]{context->End(complete.Get());context->Flush();const auto start=GetTickCount64();
        for(;;){const HRESULT result=context->GetData(complete.Get(),nullptr,0,0);if(result==S_OK)break;HR(result);
            Check(GetTickCount64()-start<10000,"D3D11 grading completion timeout");Sleep(1);}};
    NrSettings s;s.grading.monochrome={true,1};
    for(auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM}) {
        D3D11_TEXTURE2D_DESC desc{};desc.Width=W;desc.Height=H;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;desc.Format=format;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> image;HR(device->CreateTexture2D(&desc,nullptr,&image));ComPtr<ID3D11RenderTargetView> rtv;HR(device->CreateRenderTargetView(image.Get(),nullptr,&rtv));context->ClearRenderTargetView(rtv.Get(),Color);
        D3D11_VIEWPORT vp{1,2,12,9,0,1};context->RSSetViewports(1,&vp);ID3D11RenderTargetView* view=rtv.Get();context->OMSetRenderTargets(1,&view,nullptr);
        for(unsigned i=0;i<7;i++){const bool drawn=f.Execute(image.Get(),s);wait();Check(drawn,"D3D11 grade frame");}
        UINT count=1;D3D11_VIEWPORT actual{};context->RSGetViewports(&count,&actual);Check(!std::memcmp(&actual,&vp,sizeof(vp)),"D3D11 viewport not restored");
        ComPtr<ID3D11RenderTargetView> current;context->OMGetRenderTargets(1,&current,nullptr);Check(current.Get()==rtv.Get(),"D3D11 output binding not restored");
        desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;ComPtr<ID3D11Texture2D> readback;HR(device->CreateTexture2D(&desc,nullptr,&readback));
        context->CopyResource(readback.Get(),image.Get());D3D11_MAPPED_SUBRESOURCE mapped{};HR(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));
        std::array<unsigned char,4> p{};std::memcpy(p.data(),mapped.pData,4);context->Unmap(readback.Get(),0);Gray(p);
        context->ClearRenderTargetView(rtv.Get(),Color);
        auto finalSettings=s.grading;finalSettings.exposure={true,1};
        const bool finalDrawn=finalOutput.Execute(device.Get(),image.Get(),GetModuleHandleW(nullptr),finalSettings);wait();
        Check(finalDrawn,"D3D11 standalone output failed");context->CopyResource(readback.Get(),image.Get());
        HR(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));std::memcpy(p.data(),mapped.pData,4);context->Unmap(readback.Get(),0);
        Gray(p);Check(p[0]>110&&p[0]<125,"D3D11 final output applied upstream exposure again");
        NrSettings bad;bad.grading.lutEnabled=true;bad.grading.lutFile="missing-pipeline.png";
        for(unsigned i=0;i<9;i++){const bool drawn=f.Execute(image.Get(),bad);wait();Check(!drawn,"Missing-only LUT claimed a D3D11 frame");}
        Check(!f.IsDisabled(),"Repeated invalid LUT latched the D3D11 bridge off");
        const bool recovered=f.Execute(image.Get(),s);wait();Check(recovered,"D3D11 bridge could not recover after invalid LUT");
        context->CopyResource(readback.Get(),image.Get());HR(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped));context->Unmap(readback.Get(),0);
        UINT quality=0;HR(device->CheckMultisampleQualityLevels(format,4,&quality));if(quality){desc.Usage=D3D11_USAGE_DEFAULT;desc.CPUAccessFlags=0;desc.SampleDesc.Count=4;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
            ComPtr<ID3D11Texture2D> msaa;HR(device->CreateTexture2D(&desc,nullptr,&msaa));Check(!f.Execute(msaa.Get(),s),"MSAA texture must be rejected before CopyResource");}
    }
    Check(finalOutput.Filter().GradingCount()==2&&finalOutput.Filter().ModelCallCount()==0,"D3D11 final counts or NR leak");
    Check(finalOutput.TeardownForExit(),"D3D11 final teardown");
    Check(f.EvaluateCount()==0&&f.GradingCount()==16,"D3D11 color-only counted NR frames");Check(f.TeardownForExit(),"D3D11 grading teardown");
    for(UINT64 i=0;i<info->GetNumStoredMessages();i++){SIZE_T n=0;info->GetMessage(i,nullptr,&n);std::vector<char>b(n);auto* m=reinterpret_cast<D3D11_MESSAGE*>(b.data());HR(info->GetMessage(i,m,&n));
        if(m->Severity<=D3D11_MESSAGE_SEVERITY_ERROR){puts(m->pDescription);Check(false,"D3D11 validation error");}}
    puts("PASS: D3D11 RGBA/BGRA shared-fence grading, state restoration and MSAA rejection; NR calls=0.");
}
int main() try {
    setvbuf(stdout,nullptr,_IONBF,0);
    HR(CoInitializeEx(nullptr,COINIT_MULTITHREADED));ComPtr<ID3D12Debug> debug;HR(D3D12GetDebugInterface(IID_PPV_ARGS(&debug)));debug->EnableDebugLayer();
    WriteLut();RunSplit12();Run12();Run11();CoUninitialize();puts("PASS: standalone grading integration regression.");return 0;
}catch(const std::exception& e){std::printf("FAIL: %s\n",e.what());return 1;}
