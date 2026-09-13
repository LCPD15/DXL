// Direct GPU contract tests for the private, unhooked ReShade effect runtime.
// All shaders, presets, cache files and windows belong to this fixture directory.
#include <windows.h>
#include <d3d11.h>
#include <d3d11sdklayers.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <wincodec.h>
#include "../third_party/reshade/include/reshade_api.hpp"
#include <array>
#include <vector>
#include <filesystem>
#include <fstream>
#include <cstdio>
#include <stdexcept>
#include <cmath>
#include <algorithm>
#include <functional>
#include <string_view>
#include <cstring>
#include "reshade_ui_validation.h"
#include "reshade_ui_draw_validation.h"
#include "../src/core/ReShadeBridge.h"

using Microsoft::WRL::ComPtr;
namespace api=reshade::api;
namespace {
void Require(bool okay,const char* what){if(!okay)throw std::runtime_error(what);}
void Hr(HRESULT hr,const char* what){if(FAILED(hr)){std::printf("HRESULT %08lx: %s\n",static_cast<unsigned long>(hr),what);throw std::runtime_error(what);}}
std::string Utf8(const std::filesystem::path& path){const auto value=path.u8string();return {reinterpret_cast<const char*>(value.data()),value.size()};}
void Write(const std::filesystem::path& path,const std::string& data){std::ofstream file(path,std::ios::binary|std::ios::trunc);file<<data;Require(bool(file),"fixture file write");}
std::string Read(const std::filesystem::path& path){std::ifstream file(path,std::ios::binary);return {std::istreambuf_iterator<char>(file),std::istreambuf_iterator<char>()};}
struct Pixel { uint8_t r,g,b,a; };
using Color=std::array<float,4>;
void Near(float actual,float expected,float tolerance,const char* reason){if(!std::isfinite(actual)||std::abs(actual-expected)>tolerance){std::printf("%s actual %.6f expected %.6f\n",reason,actual,expected);throw std::runtime_error(reason);}}
Color Quantize(Color input){for(auto& v:input)v=std::round(std::clamp(v,0.f,1.f)*255.f)/255.f;return input;}
Color ReadColor(const std::vector<Pixel>& pixels,UINT width,UINT height){const auto p=pixels[(height/2)*width+width/2];return {p.r/255.f,p.g/255.f,p.b/255.f,p.a/255.f};}
void Expect(Color actual,Color expected,const char* reason,float tolerance=.014f){for(int i=0;i<3;i++)Near(actual[i],expected[i],tolerance,reason);}
LRESULT CALLBACK WndProc(HWND window,UINT message,WPARAM w,LPARAM l){return DefWindowProcW(window,message,w,l);}
struct Runtime {
    using Create=bool(*)(api::device_api,void*,void*,void*,const char*,api::effect_runtime**);
    using Destroy=void(*)(api::effect_runtime*);
    using Present=void(*)(api::effect_runtime*);
    HMODULE module=nullptr;Create create=nullptr;Destroy destroy=nullptr;Present present=nullptr;api::effect_runtime* value=nullptr;
    explicit Runtime(const wchar_t* dll){module=LoadLibraryExW(dll,nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);Require(module!=nullptr,"Load private ReShade library");
        create=reinterpret_cast<Create>(GetProcAddress(module,"ReShadeCreateEffectRuntime"));destroy=reinterpret_cast<Destroy>(GetProcAddress(module,"ReShadeDestroyEffectRuntime"));present=reinterpret_cast<Present>(GetProcAddress(module,"ReShadeUpdateAndPresentEffectRuntime"));Require(create&&destroy&&present,"Official standalone exports");
        if(auto open=reinterpret_cast<void(*)(const wchar_t*)>(GetProcAddress(module,"DxlReShadeOpenLog")))open((std::filesystem::current_path()/L"runtime.log").c_str());}
    void Stop(){if(value){destroy(value);value=nullptr;}}
    void Flush(){if(auto flush=reinterpret_cast<void(*)()>(GetProcAddress(module,"DxlReShadeFlushIni")))flush();}
    bool SavePreset(){auto save=reinterpret_cast<bool(*)(api::effect_runtime*)>(GetProcAddress(module,"DxlReShadeSavePreset"));Require(save!=nullptr,"Private save status export");return save(value);}
    ~Runtime(){Stop();if(module)FreeLibrary(module);}
};
struct Surface {
    bool dx11;UINT width=192,height=128;HWND window=nullptr;
    ComPtr<IDXGISwapChain3> chain;ComPtr<IDXGIFactory4> factory;
    ComPtr<ID3D12Device> device12;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12InfoQueue> info12;
    ComPtr<ID3D11Device> device11;ComPtr<ID3D11DeviceContext> context;ComPtr<ID3D11InfoQueue> info11;
    ComPtr<ID3D11Buffer> sentinelConstants;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);UINT64 nextFence=0;
    explicit Surface(bool use11):dx11(use11) {
        WNDCLASSW cls{};cls.lpfnWndProc=WndProc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"DXL_ReShade_Fixture";RegisterClassW(&cls);
        window=CreateWindowExW(0,cls.lpszClassName,L"DXL ReShade GPU fixture",WS_OVERLAPPEDWINDOW,0,0,width,height,nullptr,nullptr,cls.hInstance,nullptr);Require(window!=nullptr,"Create hidden fixture window");
        if(!dx11){ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();}
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI factory");ComPtr<IDXGIAdapter> warp;Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)),"WARP adapter");
        if(dx11) {
            D3D_FEATURE_LEVEL level;UINT flags=D3D11_CREATE_DEVICE_BGRA_SUPPORT|D3D11_CREATE_DEVICE_DEBUG;
            auto hr=D3D11CreateDevice(warp.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,flags,nullptr,0,D3D11_SDK_VERSION,&device11,&level,&context);
            if(hr==DXGI_ERROR_SDK_COMPONENT_MISSING)hr=D3D11CreateDevice(warp.Get(),D3D_DRIVER_TYPE_UNKNOWN,nullptr,D3D11_CREATE_DEVICE_BGRA_SUPPORT,nullptr,0,D3D11_SDK_VERSION,&device11,&level,&context);
            Hr(hr,"D3D11 WARP device");device11.As(&info11);
            D3D11_BUFFER_DESC marker{};marker.ByteWidth=16;marker.Usage=D3D11_USAGE_DEFAULT;marker.BindFlags=D3D11_BIND_CONSTANT_BUFFER;
            Hr(device11->CreateBuffer(&marker,nullptr,&sentinelConstants),"State sentinel constants");
        } else {
            Hr(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device12)),"D3D12 WARP device");device12.As(&info12);
            D3D12_COMMAND_QUEUE_DESC desc{};Hr(device12->CreateCommandQueue(&desc,IID_PPV_ARGS(&queue)),"D3D12 queue");Hr(device12->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"D3D12 fence");
        }
        DXGI_SWAP_CHAIN_DESC1 desc{};desc.Width=width;desc.Height=height;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BufferUsage=DXGI_USAGE_RENDER_TARGET_OUTPUT;desc.BufferCount=2;desc.SwapEffect=DXGI_SWAP_EFFECT_FLIP_DISCARD;
        ComPtr<IDXGISwapChain1> created;Hr(factory->CreateSwapChainForHwnd(dx11?static_cast<IUnknown*>(device11.Get()):static_cast<IUnknown*>(queue.Get()),window,&desc,nullptr,nullptr,&created),"Real fixture swapchain");Hr(created.As(&chain),"Swapchain3");factory->MakeWindowAssociation(window,DXGI_MWA_NO_ALT_ENTER);
    }
    ~Surface(){Wait();chain.Reset();if(context)context->ClearState();if(event)CloseHandle(event);if(window)DestroyWindow(window);}
    void Wait(){if(dx11){if(context)context->Flush();return;}if(!queue||!fence)return;Hr(queue->Signal(fence.Get(),++nextFence),"Fence signal");Hr(fence->SetEventOnCompletion(nextFence,event),"Fence completion");Require(WaitForSingleObject(event,30000)==WAIT_OBJECT_0,"GPU timeout");}
    void Submit(const std::function<void(ID3D12GraphicsCommandList*)>& record){ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;Hr(device12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"Fixture allocator");Hr(device12->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"Fixture command list");record(list.Get());Hr(list->Close(),"Fixture close");ID3D12CommandList* lists[]={list.Get()};queue->ExecuteCommandLists(1,lists);Wait();}
    void CreateRuntime(Runtime& runtime,const std::filesystem::path& config){Require(runtime.create(dx11?api::device_api::d3d11:api::device_api::d3d12,dx11?static_cast<void*>(device11.Get()):static_cast<void*>(device12.Get()),dx11?static_cast<void*>(context.Get()):static_cast<void*>(queue.Get()),chain.Get(),Utf8(config).c_str(),&runtime.value),"Create independent ReShade runtime");}
    void Clear(Color color){
        if(dx11){ComPtr<ID3D11Texture2D> buffer;ComPtr<ID3D11RenderTargetView> view;Hr(chain->GetBuffer(0,IID_PPV_ARGS(&buffer)),"D3D11 backbuffer");Hr(device11->CreateRenderTargetView(buffer.Get(),nullptr,&view),"D3D11 RTV");context->ClearRenderTargetView(view.Get(),color.data());return;}
        ComPtr<ID3D12Resource> buffer;Hr(chain->GetBuffer(chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&buffer)),"D3D12 backbuffer");
        D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_RTV;heap.NumDescriptors=1;ComPtr<ID3D12DescriptorHeap> rtv;Hr(device12->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&rtv)),"RTV heap");device12->CreateRenderTargetView(buffer.Get(),nullptr,rtv->GetCPUDescriptorHandleForHeapStart());
        Submit([&](auto* list){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={buffer.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_RENDER_TARGET};list->ResourceBarrier(1,&b);list->ClearRenderTargetView(rtv->GetCPUDescriptorHandleForHeapStart(),color.data(),0,nullptr);std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(1,&b);});
    }
    std::vector<Pixel> Capture(){
        std::vector<Pixel> pixels(size_t(width)*height);
        if(dx11){ComPtr<ID3D11Texture2D> buffer,readback;Hr(chain->GetBuffer(0,IID_PPV_ARGS(&buffer)),"Capture11 buffer");D3D11_TEXTURE2D_DESC desc{};buffer->GetDesc(&desc);desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.MiscFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Hr(device11->CreateTexture2D(&desc,nullptr,&readback),"Capture11 staging");context->CopyResource(readback.Get(),buffer.Get());D3D11_MAPPED_SUBRESOURCE map{};Hr(context->Map(readback.Get(),0,D3D11_MAP_READ,0,&map),"Capture11 map");for(UINT y=0;y<height;y++)memcpy(pixels.data()+size_t(y)*width,static_cast<const BYTE*>(map.pData)+size_t(y)*map.RowPitch,width*4);context->Unmap(readback.Get(),0);return pixels;}
        ComPtr<ID3D12Resource> buffer,readback;Hr(chain->GetBuffer(chain->GetCurrentBackBufferIndex(),IID_PPV_ARGS(&buffer)),"Capture12 buffer");const auto desc=buffer->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT rows;UINT64 rowBytes,total;device12->GetCopyableFootprints(&desc,0,1,0,&footprint,&rows,&rowBytes,&total);
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=total;d.Height=d.DepthOrArraySize=d.MipLevels=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.SampleDesc.Count=1;D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;Hr(device12->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"Capture12 readback");
        Submit([&](auto* list){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={buffer.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_PRESENT,D3D12_RESOURCE_STATE_COPY_SOURCE};list->ResourceBarrier(1,&b);D3D12_TEXTURE_COPY_LOCATION dst{},src{};dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;src.pResource=buffer.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(1,&b);});
        BYTE* map=nullptr;Hr(readback->Map(0,nullptr,reinterpret_cast<void**>(&map)),"Capture12 map");for(UINT y=0;y<height;y++)memcpy(pixels.data()+size_t(y)*width,map+footprint.Offset+size_t(y)*footprint.Footprint.RowPitch,width*4);D3D12_RANGE noWrite{};readback->Unmap(0,&noWrite);return pixels;
    }
    Color Frame(Runtime& runtime,Color color){
        Clear(color);
        if(dx11){ID3D11Buffer* buffer=sentinelConstants.Get();context->PSSetConstantBuffers(0,1,&buffer);context->VSSetConstantBuffers(0,1,&buffer);D3D11_VIEWPORT view{3,5,31,29,0,1};context->RSSetViewports(1,&view);context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP);}
        runtime.present(runtime.value);
        if(dx11){ComPtr<ID3D11Buffer> pixel,vertex;context->PSGetConstantBuffers(0,1,&pixel);context->VSGetConstantBuffers(0,1,&vertex);Require(pixel.Get()==sentinelConstants.Get()&&vertex.Get()==sentinelConstants.Get(),"ReShade preserves host D3D11 constants");D3D11_VIEWPORT view{};UINT count=1;context->RSGetViewports(&count,&view);Require(count==1&&view.TopLeftX==3&&view.TopLeftY==5&&view.Width==31&&view.Height==29,"ReShade preserves host viewport");D3D11_PRIMITIVE_TOPOLOGY topology{};context->IAGetPrimitiveTopology(&topology);Require(topology==D3D11_PRIMITIVE_TOPOLOGY_LINESTRIP,"ReShade preserves host topology");}
        auto pixels=Capture();const auto output=ReadColor(pixels,width,height);Hr(chain->Present(0,0),"Fixture present");MSG message{};while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}return output;
    }
    void Resize(UINT w,UINT h){Wait();if(context){context->ClearState();context->Flush();}Hr(chain->ResizeBuffers(2,w,h,DXGI_FORMAT_R8G8B8A8_UNORM,0),"Resize swapchain after runtime release");width=w;height=h;}
    void DebugCheck(){
        if(info12)for(UINT64 i=0;i<info12->GetNumStoredMessages();i++){SIZE_T size=0;info12->GetMessage(i,nullptr,&size);std::vector<BYTE> data(size);auto* m=reinterpret_cast<D3D12_MESSAGE*>(data.data());info12->GetMessage(i,m,&size);if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR){std::printf("D3D12 error: %s\n",m->pDescription);throw std::runtime_error("D3D12 validation error");}}
        if(info11)for(UINT64 i=0;i<info11->GetNumStoredMessages();i++){SIZE_T size=0;info11->GetMessage(i,nullptr,&size);std::vector<BYTE> data(size);auto* m=reinterpret_cast<D3D11_MESSAGE*>(data.data());info11->GetMessage(i,m,&size);if(m->Severity<=D3D11_MESSAGE_SEVERITY_ERROR){std::printf("D3D11 error: %s\n",m->pDescription);throw std::runtime_error("D3D11 validation error");}}
    }
};
void WriteLut(const std::filesystem::path& path){
    constexpr UINT n=32,width=n*n,height=n;std::vector<BYTE> pixels(width*height*4);
    for(UINT b=0;b<n;b++)for(UINT g=0;g<n;g++)for(UINT r=0;r<n;r++){const size_t index=(size_t(g)*width+b*n+r)*4;pixels[index]=BYTE(255-std::lround(255.f*b/(n-1)));pixels[index+1]=BYTE(255-std::lround(255.f*g/(n-1)));pixels[index+2]=BYTE(255-std::lround(255.f*r/(n-1)));pixels[index+3]=255;}
    ComPtr<IWICImagingFactory> f;ComPtr<IWICStream> stream;ComPtr<IWICBitmapEncoder> encoder;ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> bag;
    Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&f)),"WIC factory");Hr(f->CreateStream(&stream),"PNG stream");Hr(stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE),"PNG filename");Hr(f->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"PNG encoder");Hr(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"PNG initialize");Hr(encoder->CreateNewFrame(&frame,&bag),"PNG frame");Hr(frame->Initialize(bag.Get()),"PNG frame init");Hr(frame->SetSize(width,height),"PNG size");GUID format=GUID_WICPixelFormat32bppBGRA;Hr(frame->SetPixelFormat(&format),"PNG format");Hr(frame->WritePixels(height,width*4,static_cast<UINT>(pixels.size()),pixels.data()),"PNG pixels");Hr(frame->Commit(),"PNG frame commit");Hr(encoder->Commit(),"PNG commit");
}
Color Daltonize(Color color,int type){float l=17.8824f*color[0]+43.5161f*color[1]+4.11935f*color[2],m=3.45565f*color[0]+27.1554f*color[1]+3.86714f*color[2],s=.0299566f*color[0]+.184309f*color[1]+1.46709f*color[2];if(type==0)l=2.02344f*m-2.52581f*s;else if(type==1)m=.494207f*l+1.24827f*s;else s=-.395913f*l+.801109f*m;float er=color[0]-(.0809444479f*l-.130504409f*m+.116721066f*s),eg=color[1]-(-.0102485335f*l+.0540193266f*m-.113614708f*s),eb=color[2]-(-.000365296938f*l-.00412161469f*m+.693511405f*s);return {color[0],std::clamp(color[1]+er*.7f+eg,0.f,1.f),std::clamp(color[2]+er*.7f+eb,0.f,1.f),color[3]};}
api::effect_technique WaitTechnique(Surface& surface,Runtime& runtime,const char* file,const char* name){const ULONGLONG deadline=GetTickCount64()+30000;do{surface.Frame(runtime,{.2f,.4f,.6f,1});auto handle=runtime.value->find_technique(file,name);if(handle.handle)return handle;Sleep(10);}while(GetTickCount64()<deadline);throw std::runtime_error(std::string("Technique did not load: ")+file);}
Color Settle(Surface& surface,Runtime& runtime,Color input,Color expected,const char* reason){Color output{};const auto deadline=GetTickCount64()+30000;do{output=surface.Frame(runtime,input);bool match=true;for(int i=0;i<3;i++)match&=std::abs(output[i]-expected[i])<.014f;if(match)return output;Sleep(10);}while(GetTickCount64()<deadline);Expect(output,expected,reason);return output;}
void VerifyProductionBridge(Surface& surface,Color input,const std::filesystem::path& shaders,const std::filesystem::path& textures){
    wchar_t exe[32768]{};Require(GetModuleFileNameW(nullptr,exe,32768)>0,"Fixture executable path");
    const auto folder=std::filesystem::path(exe).parent_path()/L"post-processing";
    std::filesystem::create_directories(folder/L"upstream");
    for(const auto& path:std::filesystem::directory_iterator(shaders))if(path.is_regular_file())std::filesystem::copy_file(path.path(),folder/L"upstream"/path.path().filename(),std::filesystem::copy_options::overwrite_existing);
    std::filesystem::copy_file(textures/L"lut.png",folder/L"upstream"/L"lut.png",std::filesystem::copy_options::overwrite_existing);
    Write(folder/L"legacy-dxl.fx","// @dxl name_en DXL-only test\nfloat3 DXL_Effect(float2 uv,float3 color){return 1-color;}\n");
    const auto staged=std::filesystem::current_path()/L"state"/L"ReShade"/L"fixture"/L"effects";
    std::filesystem::create_directories(staged);
    Write(staged/L"Removed-in-previous-process.fx","#include \"ReShade.fxh\"\nfloat4 White(float4 p:SV_Position,float2 uv:TEXCOORD):SV_Target{return 1;}\ntechnique Stale{pass{VertexShader=PostProcessVS;PixelShader=White;}}\n");
    DXL::ReShadeBridge bridge;
    struct Guard { DXL::ReShadeBridge& bridge;~Guard(){bridge.Shutdown();} } guard{bridge};
    Require(bridge.Needed(nullptr),bridge.Error());
    auto render=[&](bool enabled){surface.Clear(input);bridge.Render(surface.chain.Get(),surface.dx11?static_cast<IUnknown*>(surface.device11.Get()):static_cast<IUnknown*>(surface.device12.Get()),surface.queue.Get(),nullptr,enabled);auto pixels=surface.Capture();auto color=ReadColor(pixels,surface.width,surface.height);Hr(surface.chain->Present(0,0),"Production bridge present");return color;};
    const auto wait=[&](){const auto deadline=GetTickCount64()+30000;do{render(true);if(auto* runtime=bridge.Runtime()){auto t=runtime->find_technique("LUT.fx","LUT");if(t.handle)return t;}Sleep(10);}while(GetTickCount64()<deadline);throw std::runtime_error(std::string("Bridge effect unavailable: ")+bridge.Error());};
    auto lut=wait();auto* runtime=bridge.Runtime();
    Require(runtime->find_technique("legacy-dxl.fx",nullptr).handle==0,"DXL source excluded from native compiler");
    Require(!std::filesystem::exists(staged/L"legacy-dxl.fx"),"DXL-only source never staged to ReShade");
    Require(!std::filesystem::exists(staged/L"Removed-in-previous-process.fx"),"Stale staged effect from prior process removed");
    Require(std::filesystem::exists(staged/L"upstream"/L"LUT.fx")&&std::filesystem::exists(staged/L"upstream"/L"lut.png"),"Bridge retains relative include/texture layout");
    runtime->enumerate_techniques(nullptr,[](auto* r,auto t){r->set_technique_state(t,false);});runtime->set_technique_state(lut,true);
    const Color invert{1-input[0],1-input[1],1-input[2],1};
    const auto settle=[&](Color expected,const char* reason){Color result{};const auto deadline=GetTickCount64()+30000;do{result=render(true);bool match=true;for(int i=0;i<3;i++)match&=std::abs(result[i]-expected[i])<.014f;if(match)return;Sleep(10);}while(GetTickCount64()<deadline);Expect(result,expected,reason);};
    settle(invert,"Production bridge actually applies external LUT");Expect(render(false),input,"Production master off bypasses final effects");Require(runtime->get_technique_state(lut),"Master off preserves per-effect enabled state");settle(invert,"Production master on resumes effect");
    auto chroma=runtime->find_uniform_variable("LUT.fx","fLUT_AmountChroma"),luma=runtime->find_uniform_variable("LUT.fx","fLUT_AmountLuma");runtime->set_uniform_value_float(chroma,0.f);runtime->set_uniform_value_float(luma,0.f);bridge.Save();settle(input,"Production uniform values apply");
    bridge.Reload();lut=wait();runtime=bridge.Runtime();Require(runtime->get_technique_state(lut),"Reload restores saved technique toggle");float value=1;runtime->get_uniform_value_float(runtime->find_uniform_variable("LUT.fx","fLUT_AmountLuma"),&value,1);Near(value,0,0,"Reload restores saved uniform");settle(input,"Reload preserves output");
    runtime->reset_uniform_value(runtime->find_uniform_variable("LUT.fx","fLUT_AmountChroma"));runtime->reset_uniform_value(runtime->find_uniform_variable("LUT.fx","fLUT_AmountLuma"));bridge.Save();settle(invert,"Production reset restores shader default output");
    bridge.BeforeResize(surface.chain.Get());surface.Resize(256,176);lut=wait();settle(invert,"Production BeforeResize releases COM buffers and recreates saved effects");
    bridge.Shutdown();
    if(surface.dx11){
        DXL::ReShadeBridge legacy;Guard legacyGuard{legacy};
        ComPtr<ID3D11Texture2D> image,readback;D3D11_TEXTURE2D_DESC desc{};desc.Width=192;desc.Height=128;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Usage=D3D11_USAGE_DEFAULT;desc.BindFlags=D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE;
        Hr(surface.device11->CreateTexture2D(&desc,nullptr,&image),"Legacy input texture");ComPtr<ID3D11RenderTargetView> rtv;Hr(surface.device11->CreateRenderTargetView(image.Get(),nullptr,&rtv),"Legacy source RTV");desc.Usage=D3D11_USAGE_STAGING;desc.BindFlags=0;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;Hr(surface.device11->CreateTexture2D(&desc,nullptr,&readback),"Legacy readback texture");
        auto legacyFrame=[&](bool enabled){surface.context->ClearRenderTargetView(rtv.Get(),input.data());legacy.RenderTexture11(surface.device11.Get(),image.Get(),nullptr,enabled);surface.context->CopyResource(readback.Get(),image.Get());D3D11_MAPPED_SUBRESOURCE mapped{};Hr(surface.context->Map(readback.Get(),0,D3D11_MAP_READ,0,&mapped),"Legacy processed readback");const auto p=*reinterpret_cast<const Pixel*>(static_cast<const BYTE*>(mapped.pData)+64*mapped.RowPitch+96*4);surface.context->Unmap(readback.Get(),0);return Color{p.r/255.f,p.g/255.f,p.b/255.f,p.a/255.f};};
        Color last{};const auto deadline=GetTickCount64()+30000;do{last=legacyFrame(true);if(std::abs(last[0]-invert[0])<.014f&&std::abs(last[1]-invert[1])<.014f&&std::abs(last[2]-invert[2])<.014f)break;Sleep(10);}while(GetTickCount64()<deadline);Expect(last,invert,"Production legacy texture bridge actual external LUT output");Expect(legacyFrame(false),input,"Legacy bridge master off leaves image unchanged");legacy.Shutdown();
    }
    std::puts("PASS: production ReShadeBridge actual final output, source classification/staging, includes/textures, master toggle, reset/save/reload, resize and legacy texture bridge");
}
}
int wmain(int argc,wchar_t** argv){
    try {
        Require(argc>=3,"Usage: reshade_runtime.exe absoluteDllPath dx11|dx12");Hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM");
        const bool dx11=std::wstring_view(argv[2])==L"dx11";const auto root=std::filesystem::current_path();const auto shaders=root/L"Shaders",textures=root/L"Textures";
        std::filesystem::create_directories(shaders);std::filesystem::create_directories(textures);std::filesystem::create_directories(root/L"Cache");
        WriteLut(textures/L"lut.png");
        Write(shaders/L"MultiPass.fx",R"FX(
#include "ReShade.fxh"
uniform float Gain < ui_type="slider"; ui_min=0.0; ui_max=1.0; > = 0.5;
texture Scratch { Width=BUFFER_WIDTH; Height=BUFFER_HEIGHT; Format=RGBA8; };
sampler ScratchSampler { Texture=Scratch; };
float4 First(float4 p:SV_Position,float2 uv:TEXCOORD):SV_Target { return tex2D(ReShade::BackBuffer,uv)*float4(Gain,Gain,Gain,1); }
float4 Second(float4 p:SV_Position,float2 uv:TEXCOORD):SV_Target { return tex2D(ScratchSampler,uv)+float4(0.125,0.0625,0,0); }
technique TwoPass { pass { VertexShader=PostProcessVS; PixelShader=First; RenderTarget=Scratch; } pass { VertexShader=PostProcessVS; PixelShader=Second; } }
)FX");
        Write(shaders/L"AutoEnabled.fx",R"FX(
#include "ReShade.fxh"
float4 AutoWhite(float4 p:SV_Position,float2 uv:TEXCOORD):SV_Target { return 1; }
technique AnnotatedEnable < enabled=true; > { pass { VertexShader=PostProcessVS; PixelShader=AutoWhite; } }
)FX");
        const auto config=root/L"Runtime.ini",preset=root/L"GameA.ini";
        Write(preset,"Techniques=\nTechniqueSorting=\n");
        Write(config,"[GENERAL]\nEffectSearchPaths="+Utf8(shaders)+"\nTextureSearchPaths="+Utf8(textures)+"\nIntermediateCachePath="+Utf8(root/L"Cache")+"\nPresetPath="+Utf8(preset)+"\nSkipLoadingDisabledEffects=0\nPerformanceMode=0\n[INPUT]\nKeyOverlay=0,0,0,0\n[OVERLAY]\nShowScreenshotMessage=0\nTutorialProgress=4\n");
        Surface surface(dx11);Runtime runtime(argv[1]);surface.CreateRuntime(runtime,config);
        const auto dalton=WaitTechnique(surface,runtime,"Daltonize.fx","Daltonize");auto lut=WaitTechnique(surface,runtime,"LUT.fx","LUT");const auto multi=WaitTechnique(surface,runtime,"MultiPass.fx","TwoPass");
        WaitTechnique(surface,runtime,"90-DXL-UI-Validation.fx","UiPrimary");
        VerifyReShadeUiMetadata(runtime.value);
        WaitTechnique(surface,runtime,"90-DXL-UI-Validation.fx","UiPrimary");
        VerifyReShadeUiDraw(runtime.value);
        const auto annotated=WaitTechnique(surface,runtime,"AutoEnabled.fx","AnnotatedEnable");Require(!runtime.value->get_technique_state(annotated),"Source enabled=true cannot override default-off consent");
        runtime.value->enumerate_techniques(nullptr,[](auto* r,auto t){r->set_technique_state(t,false);});
        Color input=Quantize({.6f,.2f,.4f,1});Expect(surface.Frame(runtime,input),input,"All native effects disabled leave pixels unchanged");
        runtime.value->set_technique_state(dalton,true);Require(runtime.SavePreset(),"Immediate enable saves while GPU pipeline creation is queued");Require(Read(preset).find("Daltonize@Daltonize.fx")!=std::string::npos,"Immediate enabled technique reaches disk before next present");Settle(surface,runtime,input,Daltonize(input,0),"Unmodified upstream Daltonize output");
        const auto type=runtime.value->find_uniform_variable("Daltonize.fx","Type");Require(type.handle!=0,"Upstream int uniform exposed");runtime.value->set_uniform_value_int(type,1);Settle(surface,runtime,input,Daltonize(input,1),"Integer uniform changes actual rendered output");
        runtime.value->reset_uniform_value(type);int reset=-1;runtime.value->get_uniform_value_int(type,&reset,1);Require(reset==0&&runtime.value->get_technique_state(dalton),"Reset preserves technique enabled state");Settle(surface,runtime,input,Daltonize(input,0),"Reset restores rendered default");
        runtime.value->set_uniform_value_int(type,2);runtime.value->save_current_preset();runtime.Flush();Require(std::filesystem::file_size(preset)>0,"Preset saved");
        runtime.value->set_current_preset_path(Utf8(root/L"GameB.ini").c_str());runtime.value->set_technique_state(dalton,false);runtime.value->save_current_preset();runtime.Flush();
        runtime.value->set_current_preset_path(Utf8(preset).c_str());Settle(surface,runtime,input,Daltonize(input,2),"Per-game preset restored output");
        runtime.value->set_technique_state(dalton,false);runtime.value->set_technique_state(multi,true);
        Color expected=Quantize({input[0]*.5f,input[1]*.5f,input[2]*.5f,1});expected[0]+=.125f;expected[1]+=.0625f;
        Settle(surface,runtime,input,expected,"Native two-pass intermediate texture output");
        runtime.value->set_technique_state(multi,false);runtime.value->set_uniform_value_float(runtime.value->find_uniform_variable("MultiPass.fx","Gain"),.27f);runtime.value->save_current_preset();runtime.Flush();
        const auto dormantPreset=Read(preset);Require(dormantPreset.find("Gain=0.27")!=std::string::npos,"Disabled technique uniform is persisted");
        runtime.value->reload_effect_next_frame("MultiPass.fx");const auto reloadedMulti=WaitTechnique(surface,runtime,"MultiPass.fx","TwoPass");float dormant=0;runtime.value->get_uniform_value_float(runtime.value->find_uniform_variable("MultiPass.fx","Gain"),&dormant,1);Near(dormant,.27f,.00001f,"Dormant value restores after effect reload");Require(!runtime.value->get_technique_state(reloadedMulti),"Dormant technique stays off after reload");
        runtime.value->reset_uniform_value(runtime.value->find_uniform_variable("MultiPass.fx","Gain"));
        lut=runtime.value->find_technique("LUT.fx","LUT");Require(lut.handle!=0,"Other effect still available after reload");
        runtime.value->set_technique_state(reloadedMulti,false);runtime.value->set_technique_state(lut,true);
        Settle(surface,runtime,input,{1-input[0],1-input[1],1-input[2],1},"Unmodified upstream LUT reads external PNG through includes");
        runtime.value->set_uniform_value_float(runtime.value->find_uniform_variable("LUT.fx","fLUT_AmountChroma"),0.f);runtime.value->set_uniform_value_float(runtime.value->find_uniform_variable("LUT.fx","fLUT_AmountLuma"),0.f);
        Settle(surface,runtime,input,input,"Upstream LUT strength0 is neutral");
        runtime.value->set_technique_state(lut,false);runtime.value->set_technique_state(reloadedMulti,true);runtime.value->save_current_preset();runtime.Flush();
        surface.Wait();runtime.Stop();surface.Resize(224,160);surface.CreateRuntime(runtime,config);WaitTechnique(surface,runtime,"MultiPass.fx","TwoPass");Settle(surface,runtime,input,expected,"Resize + runtime rebuild preserves preset and output");
        surface.Wait();runtime.value->save_current_preset();runtime.Flush();const auto beforeColdStart=Read(preset);runtime.Stop();surface.CreateRuntime(runtime,config);Require(!runtime.SavePreset(),"Cold runtime defers save until CPU metadata exists");runtime.value->save_current_preset();runtime.Flush();runtime.Stop();Require(Read(preset)==beforeColdStart,"Save and destroy before first present preserve existing preset");
        VerifyProductionBridge(surface,input,shaders,textures);surface.DebugCheck();std::printf("PASS: %s official ReShade standalone runtime; actual upstream shader pixels, includes/external PNG, multipass texture, uniform/reset/toggle, preset isolation/reload, resize/lifetime; no API errors\n",dx11?"D3D11":"D3D12");
        CoUninitialize();return 0;
    }catch(const std::exception& error){std::printf("FAIL: %s\n",error.what());return 1;}
}
