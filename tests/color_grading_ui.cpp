// Render the production grading controls and backend on WARP; save bilingual
// screenshots without opening a game or changing player settings.
#include <d3d11_1.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include <limits>
#include "imgui_internal.h"
#include "../src/core/ReUiBackend.cpp"
#include "../src/core/ColorGradingUi.h"
using Microsoft::WRL::ComPtr;
static void Check(bool b,const char* message){if(!b)throw std::runtime_error(message);}
static void HR(HRESULT hr){if(FAILED(hr)){printf("HRESULT %08X\n",unsigned(hr));throw std::runtime_error("graphics call");}}
static void TestResetInteraction() {
    DXL::ColorGradingUi ui; DXL::ColorGradingSettings values;
    values.highlights={true,.73f}; values.monochrome={true,.64f};
    values.bloom={true,3.0f}; values.bloomSaturation=.4f;
    values.lutEnabled=true; values.lutIntensity=.42f; values.lutFile="Neutral-16.png";
    auto& io=ImGui::GetIO(); io.DisplaySize=ImVec2(1200,3200); io.DeltaTime=1.0f/60;
    const auto frame=[&](ImVec2 position, bool pressed) {
        io.MousePos=position; io.MouseDown[0]=pressed;
        ImGui_ImplDX11_NewFrame(); ImGui::NewFrame();
        ImGui::SetNextWindowPos(ImVec2(0,0),ImGuiCond_Always);
        ImGui::SetNextWindowSize(ImVec2(1180,3100),ImGuiCond_Always);
        ImGui::Begin("GradingInteraction",nullptr,ImGuiWindowFlags_NoSavedSettings);
        ImGui::GetStateStorage()->SetInt(ImGui::GetID("Bloom details"),1);
        ui.Draw(values,GetModuleHandleW(nullptr),true); ImGui::End(); ImGui::Render();
    };
    const auto position=[&](const char* tableName, int column) {
        auto* window=ImGui::FindWindowByName("GradingInteraction"); Check(window!=nullptr,"interaction window");
        auto* table=ImGui::GetCurrentContext()->Tables.GetByKey(window->GetID(tableName)); Check(table!=nullptr,"interaction table");
        const auto& col=table->Columns[column];
        return ImVec2((col.WorkMinX+col.WorkMaxX)*.5f,(table->RowPosY1+table->RowPosY2)*.5f);
    };
    frame(ImVec2(-100,-100),false); frame(ImVec2(-100,-100),false);
    auto reset=position("BasicColor",3); frame(reset,false); frame(reset,true); frame(reset,false);
    Check(values.highlights.enabled && values.highlights.value==0,"basic reset restores number and retains enabled checkbox");
    reset=position("ColorFilters",3); frame(reset,false); frame(reset,true); frame(reset,false);
    Check(values.monochrome.enabled && values.monochrome.value==0,"filter reset retains enabled checkbox");
    reset=position("LutControl",3); frame(reset,false); frame(reset,true); frame(reset,false);
    Check(values.lutEnabled && values.lutIntensity==1 && values.lutFile=="Neutral-16.png","LUT reset preserves enable and selected file");
    const auto toggle=position("ColorFilters",1);
    Check(toggle.x>position("ColorFilters",0).x && toggle.x<position("ColorFilters",2).x,"checkbox lies next to value after label");
    frame(toggle,false); frame(toggle,true); frame(toggle,false);
    Check(!values.monochrome.enabled,"checkbox at new position is clickable");
    auto* bloomWindow=ImGui::FindWindowByName("GradingInteraction");
    auto* bloomTable=ImGui::GetCurrentContext()->Tables.GetByKey(ImHashStr("BloomDetails",0,bloomWindow->GetID("Bloom details")));
    Check(bloomTable!=nullptr,"expanded production Bloom details table");
    const auto& bloomColumn=bloomTable->Columns[2];
    reset=ImVec2((bloomColumn.WorkMinX+bloomColumn.WorkMaxX)*.5f,(bloomTable->RowPosY1+bloomTable->RowPosY2)*.5f);
    frame(reset,false); frame(reset,true); frame(reset,false);
    Check(values.bloomSaturation==1 && values.bloom.enabled && values.bloom.value==3,
        "Bloom detail reset restores only its default, retaining Bloom enable and strength3");
    const auto sample=std::find_if(values.fx.begin(),values.fx.end(),[](const auto& item){return item.file=="02-Letterbox.fx";});
    Check(sample!=values.fx.end() && sample->values.size()==2,"bundled FX controls discovered automatically");
    sample->enabled=true; sample->values[1]=.25f; frame(ImVec2(-100,-100),false);
    auto* fxWindow=ImGui::FindWindowByName("GradingInteraction");
    auto* fxTable=ImGui::GetCurrentContext()->Tables.GetByKey(ImHashStr("FxValues",0,fxWindow->GetID("02-Letterbox.fx")));
    Check(fxTable!=nullptr,"generated FX parameter table");
    const auto& fxColumn=fxTable->Columns[2];
    reset=ImVec2((fxColumn.WorkMinX+fxColumn.WorkMaxX)*.5f,(fxTable->RowPosY1+fxTable->RowPosY2)*.5f);
    frame(reset,false); frame(reset,true); frame(reset,false);
    Check(sample->enabled && sample->values[1]==1,"generated FX reset restores default and retains switch");
    puts("PASS real UI click sequence: reset preserves switches; relocated checkbox toggles; expanded Bloom detail reset retains enable and strength3");
}
static void SavePng(ID3D11Device* device,ID3D11DeviceContext* context,ID3D11Texture2D* image,const wchar_t* path){
    D3D11_TEXTURE2D_DESC d{};image->GetDesc(&d);d.Usage=D3D11_USAGE_STAGING;d.BindFlags=0;d.CPUAccessFlags=D3D11_CPU_ACCESS_READ;
    ComPtr<ID3D11Texture2D> staging;HR(device->CreateTexture2D(&d,nullptr,&staging));context->CopyResource(staging.Get(),image);
    D3D11_MAPPED_SUBRESOURCE map{};HR(context->Map(staging.Get(),0,D3D11_MAP_READ,0,&map));
    ComPtr<IWICImagingFactory> factory;HR(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)));
    ComPtr<IWICStream> stream;HR(factory->CreateStream(&stream));HR(stream->InitializeFromFilename(path,GENERIC_WRITE));
    ComPtr<IWICBitmapEncoder> encoder;HR(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder));HR(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache));
    ComPtr<IWICBitmapFrameEncode> frame;HR(encoder->CreateNewFrame(&frame,nullptr));HR(frame->Initialize(nullptr));HR(frame->SetSize(d.Width,d.Height));
    WICPixelFormatGUID format=GUID_WICPixelFormat32bppBGRA;HR(frame->SetPixelFormat(&format));
    HR(frame->WritePixels(d.Height,map.RowPitch,map.RowPitch*d.Height,static_cast<BYTE*>(map.pData)));HR(frame->Commit());HR(encoder->Commit());
    context->Unmap(staging.Get(),0);
}
int main() try {
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> context;D3D_FEATURE_LEVEL level{};
    HR(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,D3D11_CREATE_DEVICE_DEBUG,nullptr,0,D3D11_SDK_VERSION,&device,&level,&context));
    WNDCLASSW wc{};wc.lpfnWndProc=DefWindowProcW;wc.hInstance=GetModuleHandleW(nullptr);wc.lpszClassName=L"DXL.GradingUiTest";RegisterClassW(&wc);
    HWND window=CreateWindowW(wc.lpszClassName,L"DXL Grading test",WS_OVERLAPPEDWINDOW,0,0,1200,1080,nullptr,nullptr,wc.hInstance,nullptr);
    Check(window!=nullptr,"window create");Check(ReUi::InitOnce11(device.Get(),context.Get(),window,DXGI_FORMAT_B8G8R8A8_UNORM),"production UI init");
    D3D11_TEXTURE2D_DESC d{};d.Width=1200;d.Height=1080;d.Format=DXGI_FORMAT_B8G8R8A8_UNORM;d.MipLevels=d.ArraySize=d.SampleDesc.Count=1;d.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> image;HR(device->CreateTexture2D(&d,nullptr,&image));ComPtr<ID3D11RenderTargetView> rtv;HR(device->CreateRenderTargetView(image.Get(),nullptr,&rtv));
    DXL::ColorGradingUi controls;DXL::ColorGradingSettings settings;
    settings.exposure={true,.25f};settings.saturation={true,1.15f};settings.lutFile="Neutral-16.png";
    settings.sharpen={true,3.0f};settings.bloom={true,3.0f};
    for(const auto member:{&DXL::ColorGradingSettings::sharpen,&DXL::ColorGradingSettings::bloom}){
        const auto parameter=std::find_if(DXL::ColorGradingParameters.begin(),DXL::ColorGradingParameters.end(),[&](const auto& entry){return entry.member==member;});
        Check(parameter!=DXL::ColorGradingParameters.end() && parameter->minimum==0 && parameter->maximum==3,"production sharpening and Bloom sliders expose0..3");
    }
    settings.highlights.value=std::numeric_limits<float>::quiet_NaN();
    for(unsigned language=0;language<2;++language){
        const bool english=language!=0;
        for(unsigned frame=0;frame<12;++frame){
            const float background[]{.03f,.04f,.05f,1};context->ClearRenderTargetView(rtv.Get(),background);
            ReUi::FrameTexture11(image.Get(),[&]{
                ImGui::SetNextWindowPos(ImVec2(20,20),ImGuiCond_Always);
                ImGui::SetNextWindowSize(ImVec2(1140,1020),ImGuiCond_Always);
                ImGui::SetNextWindowScroll(ImVec2(0,frame<4?0.0f:frame<8?900.0f:650.0f));
                ImGui::Begin("DXL - DLSS eXtended Loader",nullptr,ImGuiWindowFlags_NoSavedSettings);
                ImGui::TextColored(ImVec4(.46f,.72f,0,1),"DXL - DLSS eXtended Loader  v0.6");ImGui::SameLine();ImGui::TextDisabled("by LCPD15");
                if(ImGui::BeginTabBar("Features")){
                    if(ImGui::BeginTabItem("DLSSNR")){ImGui::TextUnformatted("NR controls");ImGui::EndTabItem();}
                    if(ImGui::BeginTabItem(english?"Color Grading":"调色 / Color Grading",nullptr,ImGuiTabItemFlags_SetSelected)){
                        ImGui::GetStateStorage()->SetInt(ImGui::GetID(english?"Bloom details":"柔光细节 / Bloom details"),1);
                        controls.Draw(settings,GetModuleHandleW(nullptr),english);ImGui::EndTabItem();
                    }
                    ImGui::EndTabBar();
                }
                ImGui::End();
            });
            if(frame==3) SavePng(device.Get(),context.Get(),image.Get(),english?L"grading-en.png":L"grading-zh.png");
            if(frame==7) SavePng(device.Get(),context.Get(),image.Get(),english?L"grading-fx-en.png":L"grading-fx-zh.png");
            if(frame==11) SavePng(device.Get(),context.Get(),image.Get(),english?L"grading-bloom-en.png":L"grading-bloom-zh.png");
        }
    }
    Check(settings.exposure.value==.25f && settings.saturation.value==1.15f,"rendering must not mutate controls");
    Check(settings.highlights.value==0 && std::isfinite(settings.highlights.value),"non-finite typed input must normalize before persistence");
    Check(settings.sharpen.value==3 && settings.bloom.value==3,"production controls preserve sharpening and Bloom strength3");
    TestResetInteraction();
    ReUi::Shutdown();DestroyWindow(window);context->ClearState();context->Flush();CoUninitialize();
    puts("PASS production grading UI rendered in Chinese and English; controls preserve values");return 0;
}catch(const std::exception& e){fprintf(stderr,"FAIL %s\n",e.what());return 1;}
