// Real hardware NR + D3D11/D3D12 shared-fence regression. No injected game,
// no window, no deployment. Models are loaded from the test executable's ngx/.
#include <d3d10_1.h>
#include <d3d10sdklayers.h>
#include <d3d11_4.h>
#include <d3d11sdklayers.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <wrl/client.h>
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <memory>
#include <vector>
#include "DlssNrFilter11.h"
using Microsoft::WRL::ComPtr;
using namespace DXL;
static void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
static void HR(HRESULT hr) { if (FAILED(hr)) { printf("HRESULT %08X\n", unsigned(hr)); throw std::runtime_error("D3D bridge operation"); } }
// Native D3D10 state and rendering remain usable after every bridge call.
// A false occlusion predicate is intentionally bound during NR; the bridge's
// private state must not inherit it or its copies would silently be skipped.
struct Game10State {
    ID3D10Device* device;
    ComPtr<ID3D10Texture2D> target, readback;
    ComPtr<ID3D10RenderTargetView> rtv;
    ComPtr<ID3D10VertexShader> vs;ComPtr<ID3D10PixelShader> ps;
    ComPtr<ID3D10RasterizerState> raster;ComPtr<ID3D10BlendState> blend;
    ComPtr<ID3D10DepthStencilState> depth;ComPtr<ID3D10Predicate> predicate;
    D3D10_VIEWPORT viewport{2,3,10,9,.125f,.875f};
    RECT scissor{3,4,10,10};float factors[4]{.125f,.25f,.5f,.75f};
    explicit Game10State(ID3D10Device* d):device(d) {
        D3D10_TEXTURE2D_DESC desc{};desc.Width=desc.Height=16;desc.MipLevels=desc.ArraySize=desc.SampleDesc.Count=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.BindFlags=D3D10_BIND_RENDER_TARGET;
        HR(device->CreateTexture2D(&desc,nullptr,&target));HR(device->CreateRenderTargetView(target.Get(),nullptr,&rtv));
        desc.BindFlags=0;desc.Usage=D3D10_USAGE_STAGING;desc.CPUAccessFlags=D3D10_CPU_ACCESS_READ;
        HR(device->CreateTexture2D(&desc,nullptr,&readback));
        const char* vertex="float4 main(uint id:SV_VertexID):SV_POSITION {float2 p=id==0?float2(-1,-1):id==1?float2(-1,3):float2(3,-1);return float4(p,0,1);}";
        const char* pixel="float4 main():SV_Target {return float4(1,.25,0,1);}";
        ComPtr<ID3DBlob> code,error;
        HR(D3DCompile(vertex,strlen(vertex),nullptr,nullptr,nullptr,"main","vs_4_0",0,0,&code,&error));
        HR(device->CreateVertexShader(code->GetBufferPointer(),code->GetBufferSize(),&vs));code.Reset();
        HR(D3DCompile(pixel,strlen(pixel),nullptr,nullptr,nullptr,"main","ps_4_0",0,0,&code,&error));
        HR(device->CreatePixelShader(code->GetBufferPointer(),code->GetBufferSize(),&ps));
        D3D10_RASTERIZER_DESC rd{};rd.FillMode=D3D10_FILL_SOLID;rd.CullMode=D3D10_CULL_NONE;rd.DepthClipEnable=TRUE;rd.ScissorEnable=TRUE;
        HR(device->CreateRasterizerState(&rd,&raster));
        D3D10_BLEND_DESC bd{};bd.SrcBlend=bd.SrcBlendAlpha=D3D10_BLEND_ONE;bd.DestBlend=bd.DestBlendAlpha=D3D10_BLEND_ZERO;
        bd.BlendOp=bd.BlendOpAlpha=D3D10_BLEND_OP_ADD;bd.RenderTargetWriteMask[0]=D3D10_COLOR_WRITE_ENABLE_ALL;
        HR(device->CreateBlendState(&bd,&blend));
        D3D10_DEPTH_STENCIL_DESC dd{};dd.DepthFunc=D3D10_COMPARISON_ALWAYS;dd.StencilReadMask=dd.StencilWriteMask=0xff;
        dd.FrontFace={D3D10_STENCIL_OP_KEEP,D3D10_STENCIL_OP_KEEP,D3D10_STENCIL_OP_KEEP,D3D10_COMPARISON_ALWAYS};dd.BackFace=dd.FrontFace;
        HR(device->CreateDepthStencilState(&dd,&depth));
        D3D10_QUERY_DESC query{D3D10_QUERY_OCCLUSION_PREDICATE,0};HR(device->CreatePredicate(&query,&predicate));
        predicate->Begin();predicate->End();
    }
    void Bind() {
        auto* view=rtv.Get();device->OMSetRenderTargets(1,&view,nullptr);
        device->OMSetBlendState(blend.Get(),factors,0x13579bdf);device->OMSetDepthStencilState(depth.Get(),73);
        device->RSSetState(raster.Get());device->RSSetViewports(1,&viewport);device->RSSetScissorRects(1,&scissor);
        device->IASetInputLayout(nullptr);device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        device->VSSetShader(vs.Get());device->GSSetShader(nullptr);device->PSSetShader(ps.Get());
        device->SetPredication(predicate.Get(),TRUE);
    }
    void Verify() {
        ComPtr<ID3D10RenderTargetView> view;ComPtr<ID3D10DepthStencilView> dsv;device->OMGetRenderTargets(1,&view,&dsv);
        Check(view.Get()==rtv.Get()&&!dsv,"D3D10 render target not restored");
        ComPtr<ID3D10VertexShader> v;ComPtr<ID3D10PixelShader> p;device->VSGetShader(&v);device->PSGetShader(&p);
        Check(v.Get()==vs.Get()&&p.Get()==ps.Get(),"D3D10 shaders not restored");
        ComPtr<ID3D10RasterizerState> r;device->RSGetState(&r);Check(r.Get()==raster.Get(),"D3D10 rasterizer not restored");
        ComPtr<ID3D10BlendState> b;float f[4]{};UINT mask=0;device->OMGetBlendState(&b,f,&mask);
        Check(b.Get()==blend.Get()&&memcmp(f,factors,sizeof(f))==0&&mask==0x13579bdf,"D3D10 blend state not restored");
        ComPtr<ID3D10DepthStencilState> d;UINT ref=0;device->OMGetDepthStencilState(&d,&ref);
        Check(d.Get()==depth.Get()&&ref==73,"D3D10 depth/stencil state not restored");
        D3D10_VIEWPORT vp{};UINT count=1;device->RSGetViewports(&count,&vp);
        Check(count==1&&memcmp(&vp,&viewport,sizeof(vp))==0,"D3D10 viewport not restored");
        RECT rect{};count=1;device->RSGetScissorRects(&count,&rect);Check(count==1&&EqualRect(&rect,&scissor),"D3D10 scissor not restored");
        ComPtr<ID3D10Predicate> pred;BOOL value=FALSE;device->GetPredication(&pred,&value);
        Check(pred.Get()==predicate.Get()&&value,"D3D10 predication not restored");
    }
    void DrawAndRead() {
        device->SetPredication(nullptr,FALSE);
        const float blue[4]{0,0,1,1};device->ClearRenderTargetView(rtv.Get(),blue);
        device->Draw(3,0);device->CopyResource(readback.Get(),target.Get());
        D3D10_MAPPED_TEXTURE2D mapped{};HR(readback->Map(0,D3D10_MAP_READ,0,&mapped));
        const auto* row=reinterpret_cast<const UINT*>(static_cast<const char*>(mapped.pData)+5*mapped.RowPitch);
        const UINT inside=row[5],outside=*static_cast<const UINT*>(mapped.pData);
        readback->Unmap(0);
        Check((inside&0xff)==255&&((inside>>16)&255)==0&&((inside>>8)&255)>=63&&((inside>>8)&255)<=64,
            "native D3D10 Draw failed after bridge");
        Check((outside&0x00ffffff)==0x00ff0000,"native D3D10 viewport/scissor not honored");
    }
};
int main(int argc,char** argv) try {
    bool native10=false,singleThreaded=false;
    for(int i=1;i<argc;++i){native10|=std::string(argv[i])=="--d3d10";singleThreaded|=std::string(argv[i])=="--single-threaded";}
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOGPFAULTERRORBOX);
    _set_error_mode(_OUT_TO_STDERR); setvbuf(stdout, nullptr, _IONBF, 0);
    ComPtr<ID3D12Debug> debug; HR(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))); debug->EnableDebugLayer();
    ComPtr<ID3D11Device> device; ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D10Device> device10;
    const UINT flags=D3D11_CREATE_DEVICE_DEBUG|(singleThreaded?D3D11_CREATE_DEVICE_SINGLETHREADED:0);
    if(native10) {
        HR(D3D10CreateDevice(nullptr,D3D10_DRIVER_TYPE_HARDWARE,nullptr,flags,D3D10_SDK_VERSION,&device10));
        HR(device10.As(&device));device->GetImmediateContext(&context);
    } else HR(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        flags, nullptr, 0, D3D11_SDK_VERSION, &device, nullptr, &context));
    printf("Native API=%s featureLevel=0x%X flags=0x%X\n",native10?"D3D10":"D3D11",unsigned(device->GetFeatureLevel()),device->GetCreationFlags());
    ComPtr<ID3D11InfoQueue> info11; HR(device.As(&info11));
    std::unique_ptr<Game10State> game10;if(native10)game10=std::make_unique<Game10State>(device10.Get());
    auto* filter = new DlssNrFilter11; // explicit verified teardown, like core State
    puts("Initializing real NR D3D11 bridge");
    Check(filter->Initialize(device.Get(), GetModuleHandleW(nullptr)), filter->LastError());
    ComPtr<ID3D12InfoQueue> info12; HR(filter->BridgeDevice12()->QueryInterface(IID_PPV_ARGS(&info12)));
    NrSettings settings; settings.enabled = true; settings.opticalFlow = true;
    unsigned totalRan = 0, changedPixels = 0;
    for (unsigned segment = 0; segment < 4; ++segment) {
        const UINT width = segment % 2 ? 640 : 512, height = segment % 2 ? 360 : 288;
        settings.trueLayers = segment == 1 ? 3 : segment == 2 ? 2 : 1;
        settings.selfLayers = segment == 3 ? 1.5f : 1.0f;
        settings.renderScale = segment % 2 ? .5f : 1.0f;
        D3D11_TEXTURE2D_DESC td{}; td.Width = width; td.Height = height;
        td.MipLevels = td.ArraySize = td.SampleDesc.Count = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> image,staging;ComPtr<ID3D10Texture2D> image10,staging10;
        if(native10) {
            D3D10_TEXTURE2D_DESC ten{};ten.Width=width;ten.Height=height;ten.MipLevels=ten.ArraySize=ten.SampleDesc.Count=1;
            ten.Format=td.Format;ten.BindFlags=D3D10_BIND_RENDER_TARGET;
            HR(device10->CreateTexture2D(&ten,nullptr,&image10));HR(image10.As(&image));
            ten.BindFlags=0;ten.Usage=D3D10_USAGE_STAGING;ten.CPUAccessFlags=D3D10_CPU_ACCESS_READ;
            HR(device10->CreateTexture2D(&ten,nullptr,&staging10));
        } else HR(device->CreateTexture2D(&td, nullptr, &image));
        td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if(!native10)HR(device->CreateTexture2D(&td, nullptr, &staging));
        std::vector<UINT> source(width * height);
        unsigned ranSegment = 0;
        for (unsigned frame = 0; frame < 16; ++frame) {
            for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) {
                const UINT r = 40 + ((x + frame) / 8 % 2) * 100;
                const UINT gr = 30 + ((y / 8) % 2) * 110;
                const UINT b = 45 + ((x + y) % 120);
                source[y * width + x] = 0xff000000 | (b << 16) | (gr << 8) | r;
            }
            if(native10)device10->UpdateSubresource(image10.Get(),0,nullptr,source.data(),width*4,0);
            else context->UpdateSubresource(image.Get(), 0, nullptr, source.data(), width * 4, 0);
            if(game10)game10->Bind();
            const auto before = filter->Filter12ForStatus().ModelCallCount();
            const bool ran = filter->Execute(image.Get(), settings);
            if(game10)game10->Verify();
            if (ran) {
                ++ranSegment; ++totalRan;
                Check(filter->Filter12ForStatus().ModelCallCount() - before == settings.trueLayers,
                    "D3D11 bridge layer count mismatch");
            }
            // Leave each segment's tail buffered/in flight. The next resize,
            // or final explicit teardown, must itself flush and retire it.
            if (frame == 15) {if(native10)device10->SetPredication(nullptr,FALSE);continue;}
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if(native10) {
                device10->SetPredication(nullptr,FALSE);device10->CopyResource(staging10.Get(),image10.Get());
                D3D10_MAPPED_TEXTURE2D ten{};HR(staging10->Map(0,D3D10_MAP_READ,0,&ten));mapped.pData=ten.pData;mapped.RowPitch=ten.RowPitch;
            } else {
                context->Flush();context->CopyResource(staging.Get(), image.Get());
                HR(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped));
            }
            if (ran) for (UINT y = 0; y < height; y += 8) for (UINT x = 0; x < width; x += 8) {
                const auto pixel = *reinterpret_cast<const UINT*>(static_cast<const char*>(mapped.pData) + y * mapped.RowPitch + x * 4);
                if ((pixel & 0x00ffffff) != (source[y * width + x] & 0x00ffffff)) ++changedPixels;
            }
            if(native10){staging10->Unmap(0);game10->DrawAndRead();}
            else context->Unmap(staging.Get(), 0);
        }
        Check(ranSegment >= 12, filter->LastError());
        printf("Bridge segment %u passed: %ux%u trueLayers=%d self=%.2f scale=%.2f frames=%u\n",
            segment, width, height, settings.trueLayers, settings.selfLayers, settings.renderScale, ranSegment);
    }
    if(game10)game10->Bind();
    Check(filter->TeardownForExit(), "bridge exit did not retire GPU work");
    if(game10){game10->Verify();game10->DrawAndRead();}
    unsigned errors = 0;
    for (UINT64 i = 0; i < info11->GetNumStoredMessages(); ++i) {
        SIZE_T size = 0; info11->GetMessage(i, nullptr, &size);
        std::vector<char> bytes(size); auto* m = reinterpret_cast<D3D11_MESSAGE*>(bytes.data());
        HR(info11->GetMessage(i, m, &size));
        if (m->Severity <= D3D11_MESSAGE_SEVERITY_ERROR) { puts(m->pDescription); ++errors; }
    }
    for (UINT64 i = 0; i < info12->GetNumStoredMessages(); ++i) {
        SIZE_T size = 0; info12->GetMessage(i, nullptr, &size);
        std::vector<char> bytes(size); auto* m = reinterpret_cast<D3D12_MESSAGE*>(bytes.data());
        HR(info12->GetMessage(i, m, &size));
        if (m->Severity <= D3D12_MESSAGE_SEVERITY_ERROR) { puts(m->pDescription); ++errors; }
    }
    Check(errors == 0, "bridge GPU validation errors");
    Check(changedPixels > 1000, "NR did not change D3D11 image pixels");
    printf("PASS real NR %s bridge: %u frames, %u changed pixels, resize + layers + optical fallback, clean retirement, 0 D3D11/D3D12 errors%s\n",
        native10?"D3D10->D3D11":"D3D11",totalRan,changedPixels,native10?"; native10 state/RT/viewport/scissor/shaders/blend/depth/predicate restored, post-NR native Draw verified":"");
    return 0;
} catch (const std::exception& e) { fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
