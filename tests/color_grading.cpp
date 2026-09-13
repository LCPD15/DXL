#include "../src/core/ColorGrading.h"
#include <dxgi1_4.h>
#include <d3d12sdklayers.h>
#include <wincodec.h>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <vector>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <fstream>

using Microsoft::WRL::ComPtr;
using namespace DXL;
namespace {
void Check(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void Hr(HRESULT hr,const char* message){Check(SUCCEEDED(hr),message);}
struct Pixel{float r,g,b,a;};
constexpr UINT W=17,H=13;
bool useHardware=false;
float Decode(float c){float a=std::abs(c);return std::copysign(a<=.04045f?a/12.92f:std::pow((a+.055f)/1.055f,2.4f),c);}
float Encode(float c){float a=std::abs(c);return std::copysign(a<=.0031308f?a*12.92f:1.055f*std::pow(a,1/2.4f)-.055f,c);}
float Clamp(float x){return std::clamp(x,0.f,1.f);}
float Half(uint16_t value){
    const float magnitude=(value&0x7c00)?std::ldexp(float(1024+(value&1023)),int((value>>10)&31)-25):std::ldexp(float(value&1023),-24);
    return value&0x8000?-magnitude:magnitude;
}
void Near(float a,float b,float tolerance,const char* context){if(!std::isfinite(a)||std::abs(a-b)>tolerance){std::printf("%s: actual=%g expected=%g\n",context,a,b);throw std::runtime_error(context);}}
void WriteLut(const wchar_t* filename,UINT n,UINT tilesX,bool invert){
    const UINT width=n*tilesX,height=n*(n/tilesX);
    std::vector<BYTE> pixels(size_t(width)*height*4);
    for(UINT b=0;b<n;b++)for(UINT g=0;g<n;g++)for(UINT r=0;r<n;r++){
        size_t at=((b/tilesX*n+g)*width+b%tilesX*n+r)*4;
        pixels[at]=BYTE(std::lround(255.f*r/(n-1)));pixels[at+1]=BYTE(std::lround(255.f*g/(n-1)));pixels[at+2]=BYTE(std::lround(255.f*b/(n-1)));pixels[at+3]=255;
        if(invert)for(size_t c=0;c<3;c++)pixels[at+c]=255-pixels[at+c];
    }
    ComPtr<IWICImagingFactory> factory;ComPtr<IWICStream> stream;ComPtr<IWICBitmapEncoder> encoder;ComPtr<IWICBitmapFrameEncode> frame;ComPtr<IPropertyBag2> bag;
    Hr(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)),"WIC factory");
    Hr(factory->CreateStream(&stream),"WIC stream");Hr(stream->InitializeFromFilename(filename,GENERIC_WRITE),"PNG output");
    Hr(factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder),"PNG encoder");Hr(encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache),"PNG init");
    Hr(encoder->CreateNewFrame(&frame,&bag),"PNG frame");Hr(frame->Initialize(bag.Get()),"PNG frame init");Hr(frame->SetSize(width,height),"PNG size");
    auto format=GUID_WICPixelFormat32bppBGRA;Hr(frame->SetPixelFormat(&format),"PNG format");Check(IsEqualGUID(format,GUID_WICPixelFormat32bppBGRA),"PNG BGRA supported");
    for(size_t i=0;i<pixels.size();i+=4)std::swap(pixels[i],pixels[i+2]);
    Hr(frame->WritePixels(height,width*4,static_cast<UINT>(pixels.size()),pixels.data()),"PNG pixels");Hr(frame->Commit(),"PNG frame commit");Hr(encoder->Commit(),"PNG commit");
}
struct Fixture{
    UINT width=W,height=H;
    ComPtr<ID3D12Device> device;ComPtr<ID3D12CommandQueue> queue;ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12InfoQueue> info;ColorGrading grading;ColorGradingFrame slot;
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);UINT64 fenceValue=0;
    Fixture(UINT w=W,UINT h=H):width(w),height(h){
        ComPtr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
        ComPtr<IDXGIFactory4> factory;ComPtr<IDXGIAdapter> warp;
        Hr(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"DXGI factory");
        if(useHardware) {
            for(UINT i=0;;i++) {
                ComPtr<IDXGIAdapter1> adapter;
                if(factory->EnumAdapters1(i,&adapter)==DXGI_ERROR_NOT_FOUND)break;
                DXGI_ADAPTER_DESC1 desc{};Hr(adapter->GetDesc1(&desc),"Adapter description");
                if(desc.VendorId==0x10de&&!(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)){adapter.As(&warp);break;}
            }
            Check(warp!=nullptr,"NVIDIA hardware adapter available");
        } else Hr(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)),"WARP adapter");
        Hr(D3D12CreateDevice(warp.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"Test device");
        DXGI_ADAPTER_DESC adapterDescription{};Hr(warp->GetDesc(&adapterDescription),"Test adapter description");
        std::printf("GPU fixture mode: %s; vendor %04x, device %04x\n",useHardware?"NVIDIA hardware":"WARP",adapterDescription.VendorId,adapterDescription.DeviceId);
        device.As(&info);D3D12_COMMAND_QUEUE_DESC q{};Hr(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"Command queue");
        Hr(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Fence");Check(event!=nullptr,"Fence event");
        Check(grading.Initialize(device.Get()),grading.LastError());grading.SetModule(nullptr);
    }
    ~Fixture(){if(event)CloseHandle(event);}
    ComPtr<ID3D12Resource> Texture(D3D12_RESOURCE_STATES state,D3D12_RESOURCE_FLAGS flags,DXGI_FORMAT format=DXGI_FORMAT_R32G32B32A32_FLOAT){
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;d.Width=width;d.Height=height;d.DepthOrArraySize=d.MipLevels=1;d.Format=format;d.SampleDesc.Count=1;d.Flags=flags;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;ComPtr<ID3D12Resource> r;
        Hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&r)),"Texture");return r;
    }
    ComPtr<ID3D12Resource> Buffer(UINT64 bytes,D3D12_HEAP_TYPE type){
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;d.Height=d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=type;ComPtr<ID3D12Resource> r;
        Hr(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&r)),"Buffer");return r;
    }
    std::vector<Pixel> Run(const std::vector<Pixel>& pixels,const ColorGradingSettings& settings,bool linear=false,UINT frame=1,
        bool discard=false,DXGI_FORMAT targetFormat=DXGI_FORMAT_R32G32B32A32_FLOAT,UINT regionWidth=0,UINT regionHeight=0){
        Check(pixels.size()==width*height,"Source size");
        if(!regionWidth)regionWidth=width;if(!regionHeight)regionHeight=height;
        auto source=Texture(D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_NONE),target=Texture(D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,targetFormat);
        auto desc=source->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT rows;UINT64 rowSize,total;
        device->GetCopyableFootprints(&desc,0,1,0,&fp,&rows,&rowSize,&total);
        auto upload=Buffer(total,D3D12_HEAP_TYPE_UPLOAD);
        const auto targetDesc=target->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT targetFp{};
        device->GetCopyableFootprints(&targetDesc,0,1,0,&targetFp,&rows,&rowSize,&total);
        auto readback=Buffer(total,D3D12_HEAP_TYPE_READBACK);
        BYTE* mapped=nullptr;D3D12_RANGE noRead{};Hr(upload->Map(0,&noRead,reinterpret_cast<void**>(&mapped)),"Upload map");
        for(UINT y=0;y<height;y++)std::memcpy(mapped+fp.Offset+y*fp.Footprint.RowPitch,pixels.data()+y*width,width*sizeof(Pixel));upload->Unmap(0,nullptr);
        ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
        Hr(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"Allocator");Hr(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"Command list");
        D3D12_TEXTURE_COPY_LOCATION a{},b{};a.pResource=source.Get();a.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;b.pResource=upload.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;b.PlacedFootprint=fp;
        list->CopyTextureRegion(&a,0,0,0,&b,nullptr);
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;barrier.Transition={source.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};list->ResourceBarrier(1,&barrier);
        const bool recorded=grading.Record(list.Get(),source.Get(),target.Get(),regionWidth,regionHeight,settings,linear,frame,slot);
        if(!recorded||discard){Hr(list->Close(),"Close unused list");return {};}
        barrier.Transition={target.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};list->ResourceBarrier(1,&barrier);
        a.pResource=readback.Get();a.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;a.PlacedFootprint=targetFp;
        b.pResource=target.Get();b.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;b.SubresourceIndex=0;list->CopyTextureRegion(&a,0,0,0,&b,nullptr);
        Hr(list->Close(),"Close list");ID3D12CommandList* submit[]={list.Get()};queue->ExecuteCommandLists(1,submit);Hr(queue->Signal(fence.Get(),++fenceValue),"Signal");
        Hr(fence->SetEventOnCompletion(fenceValue,event),"Fence completion");Check(WaitForSingleObject(event,20000)==WAIT_OBJECT_0,"GPU timeout");
        Hr(readback->Map(0,nullptr,reinterpret_cast<void**>(&mapped)),"Readback map");std::vector<Pixel> result(regionWidth*regionHeight);
        for(UINT y=0;y<regionHeight;y++) {
            const auto* row=mapped+targetFp.Offset+y*targetFp.Footprint.RowPitch;
            if(targetFormat==DXGI_FORMAT_R16G16B16A16_FLOAT) {
                for(UINT x=0;x<regionWidth;x++) {
                    const auto* values=reinterpret_cast<const uint16_t*>(row)+x*4;
                    for(int c=0;c<3;c++)Check((values[c]&0x7c00)!=0x7c00,"Half-float output has no infinity or NaN");
                    result[y*regionWidth+x]={Half(values[0]),Half(values[1]),Half(values[2]),Half(values[3])};
                }
            } else std::memcpy(result.data()+y*regionWidth,row,regionWidth*sizeof(Pixel));
        }
        readback->Unmap(0,&noRead);
        for(UINT y=0;y<regionHeight;y++)for(UINT x=0;x<regionWidth;x++)Near(result[y*regionWidth+x].a,pixels[y*width+x].a,targetFormat==DXGI_FORMAT_R16G16B16A16_FLOAT?.0005f:0,"Alpha preserved");
        return result;
    }
    void CheckDebug(){
        if(!info){std::puts("D3D12 debug layer unavailable; WARP readback checks still performed");return;}
        for(UINT64 i=0;i<info->GetNumStoredMessages();i++){
            SIZE_T size=0;info->GetMessage(i,nullptr,&size);std::vector<BYTE> bytes(size);auto message=reinterpret_cast<D3D12_MESSAGE*>(bytes.data());Hr(info->GetMessage(i,message,&size),"Debug message");
            if(message->Severity<=D3D12_MESSAGE_SEVERITY_WARNING){std::printf("D3D12: %s\n",message->pDescription);Check(false,"D3D12 validation warning/error");}
        }
    }
};
void VerifyBloom(Fixture& f) {
    f.width=129;f.height=97;
    const UINT width=f.width,height=f.height,cx=width/2,cy=height/2;
    std::vector<Pixel> image(width*height,Pixel{.08f,.08f,.08f,.75f});
    for(UINT y=cy-3;y<=cy+3;y++)for(UINT x=cx-3;x<=cx+3;x++)image[y*width+x]={2,1.4f,.8f,.75f};
    ColorGradingSettings s;s.bloom={true,1};s.bloomSoftKnee=0;s.bloomScatter=0;s.bloomRadius=.25f;
    const auto narrow=f.Run(image,s);
    auto moment=[&](const std::vector<Pixel>& pixels){
        double energy=0,sum=0;
        for(UINT y=0;y<height;y++)for(UINT x=0;x<width;x++) {
            const double dx=double(x)-cx,dy=double(y)-cy;
            const double contribution=std::max(pixels[y*width+x].r-image[y*width+x].r,0.f);
            energy+=contribution;sum+=contribution*(dx*dx+dy*dy);
        }
        Check(energy>0,"Bloom moment has positive energy");return sum/energy;
    };
    s.bloomRadius=3;const auto radiusOnly=f.Run(image,s);
    Check(moment(radiusOnly)>moment(narrow)*1.5,"Radius independently broadens the fine-scale halo");
    s.bloomRadius=.25f;s.bloomScatter=1;const auto scatterOnly=f.Run(image,s);
    Check(moment(scatterOnly)>moment(narrow)*2,"Scatter independently introduces coarse-scale halo");
    s.bloomScatter=1;s.bloomRadius=3;
    const auto wide=f.Run(image,s);
    double narrowMoment=0,wideMoment=0,narrowEnergy=0,wideEnergy=0;
    for(UINT y=0;y<height;y++)for(UINT x=0;x<width;x++) {
        const size_t i=y*width+x;const double distance=double(int(x)-int(cx))*(int(x)-int(cx))+double(int(y)-int(cy))*(int(y)-int(cy));
        const double a=std::max(narrow[i].r-image[i].r,0.f),b=std::max(wide[i].r-image[i].r,0.f);
        narrowEnergy+=a;wideEnergy+=b;narrowMoment+=a*distance;wideMoment+=b*distance;
        Check(std::isfinite(wide[i].r)&&std::isfinite(wide[i].g)&&std::isfinite(wide[i].b),"Wide Bloom finite");
    }
    Check(narrowEnergy>0&&wideEnergy>0,"Highlight produces glow energy");
    Check(wideMoment/wideEnergy>narrowMoment/narrowEnergy*2,"Pyramid radius and scatter broaden glow beyond a local kernel");
    Check(wide[cy*width+cx+20].r>image[cy*width+cx+20].r+.0001f,"Coarse pyramid adds a distant smooth halo");
    s.bloomRadius=1;s.bloomScatter=.7f;const auto strength1=f.Run(image,s);
    s.bloom.value=3;const auto strength3=f.Run(image,s);
    for(size_t i=0;i<image.size();i++)Near(strength3[i].r-image[i].r,3*(strength1[i].r-image[i].r),.000002f,"Bloom strength 3 exceeds strength 1 without old clamp");
    s.bloom.value=99;const auto bounded=f.Run(image,s);
    for(size_t i=0;i<image.size();i++)Near(bounded[i].r,strength3[i].r,0,"GPU Bloom strength clamps to 3");
    s.bloom.value=1;s.bloomThreshold=2;s.bloomSoftKnee=0;auto out=f.Run(image,s);
    for(size_t i=0;i<image.size();i++)Near(out[i].r,image[i].r,0,"High threshold excludes lower luminance content");
    s.bloomThreshold=0;s.bloomSaturation=0;out=f.Run(image,s);
    for(size_t i=0;i<image.size();i++) {
        Near(out[i].r-image[i].r,out[i].g-image[i].g,.000001f,"Saturation 0 produces neutral glow R/G");
        Near(out[i].g-image[i].g,out[i].b-image[i].b,.000001f,"Saturation 0 produces neutral glow G/B");
    }
    s.bloomSaturation=2;out=f.Run(image,s);
    Check((out[cy*width+cx].r-image[cy*width+cx].r)>(out[cy*width+cx].b-image[cy*width+cx].b)*1.5f,"Bloom saturation increases halo color");
    std::vector<Pixel> nearThreshold(image.size(),Pixel{.8f,.8f,.8f,.75f});
    s={};s.bloom={true,1};s.bloomThreshold=1;s.bloomSoftKnee=0;
    out=f.Run(nearThreshold,s);Near(out[0].r,.8f,0,"Hard knee has no sub-threshold glow");
    s.bloomSoftKnee=1;out=f.Run(nearThreshold,s);Check(out[0].r>.82f,"Soft knee fades in below threshold");
    s={};s.bloom={true,1};const auto defaults=f.Run(image,s);
    s.bloomThreshold=std::numeric_limits<float>::quiet_NaN();s.bloomSoftKnee=std::numeric_limits<float>::infinity();
    s.bloomRadius=std::numeric_limits<float>::quiet_NaN();s.bloomScatter=-std::numeric_limits<float>::infinity();s.bloomSaturation=std::numeric_limits<float>::quiet_NaN();
    out=f.Run(image,s);for(size_t i=0;i<image.size();i++)Near(out[i].r,defaults[i].r,0,"Invalid raw Bloom parameters use finite defaults");
    s={};s.bloom={true,3};s.bloomThreshold=0;s.bloomSaturation=2;s.bloomRadius=3;
    std::vector<Pixel> hdr(image.size(),Pixel{60000,100,2,.75f});out=f.Run(hdr,s,true,1,false,DXGI_FORMAT_R16G16B16A16_FLOAT);
    Check(out[0].r>60000&&out[0].r<=65504&&out[0].g>1,"Bloom retains extended HDR and bounds half-float storage");
    // A subrect of a larger source must never gather bright data outside it.
    std::vector<Pixel> padded(image.size(),Pixel{8,8,8,.75f});
    for(UINT y=0;y<37;y++)for(UINT x=0;x<51;x++)padded[y*width+x]={.08f,.08f,.08f,.75f};
    s={};s.bloom={true,3};s.bloomRadius=3;
    out=f.Run(padded,s,false,1,false,DXGI_FORMAT_R32G32B32A32_FLOAT,51,37);
    for(const auto& pixel:out)Near(pixel.r,.08f,0,"Subrect bloom cannot sample bright padding");
    f.width=31;f.height=23;std::vector<Pixel> resized(f.width*f.height,Pixel{1,1,1,.75f});
    Check(f.Run(resized,s,false,1,true).empty(),"Abandoned Bloom command list is not submitted");
    out=f.Run(resized,s);Check(out.size()==resized.size()&&out[0].r>1,"Retired Bloom slot survives resize and abandoned recording");
    f.width=f.height=1;out=f.Run({Pixel{1,1,1,.75f}},s);Check(out.size()==1&&out[0].r>1,"Bloom 1x1 downsample levels are valid");
    s.bloom.value=0;Check(f.Run({Pixel{1,1,1,.75f}},s).empty(),"Zero strength skips all Bloom work");
    s.bloom={false,3};Check(f.Run({Pixel{1,1,1,.75f}},s).empty(),"Disabled Bloom skips all work");
    f.width=W;f.height=H;
    std::printf("PASS: Bloom 5-level bright-spot halo (mean radius squared %.2f -> %.2f), strength3, threshold/knee/radius/scatter/saturation, finite raw settings, half-float HDR, odd/subrect/1x1, resize and abandoned-list reuse\n",narrowMoment/narrowEnergy,wideMoment/wideEnergy);
}
}
int wmain(int argc,wchar_t** argv){
    try{
        useHardware=argc>1&&std::wcscmp(argv[1],L"--hardware")==0;
        Hr(CoInitializeEx(nullptr,COINIT_MULTITHREADED),"COM");
        const auto root=std::filesystem::current_path()/L"lut";std::filesystem::create_directories(root);
        WriteLut((root/L"identity.png").c_str(),2,2,false);WriteLut((root/L"invert.png").c_str(),2,2,true);WriteLut((root/L"tiled.png").c_str(),64,8,false);
        Check(ColorGrading::ValidateLutFilename("grade.PNG"),"PNG uppercase");
        for(const auto* name:{"../bad.png","a/b.png","C:\\evil.png","bad.png:stream","bad.png ","bad.fx",".hidden.png"})Check(!ColorGrading::ValidateLutFilename(name),"Unsafe filename rejected");
        UINT cube=0,tiles=0;Check(ColorGrading::InferLutLayout(512,512,cube,tiles)&&cube==64&&tiles==8,"Tiled layout");Check(ColorGrading::InferLutLayout(16,256,cube,tiles)&&cube==16&&tiles==1,"Vertical layout");Check(!ColorGrading::InferLutLayout(512,513,cube,tiles),"Invalid layout rejected");
        Fixture f;std::vector<Pixel> source(W*H);
        for(UINT y=0;y<H;y++)for(UINT x=0;x<W;x++)source[y*W+x]={x/float(W-1)*1.5f-.1f,y/float(H-1)*.8f,.17f+((x+y)%4)*.13f,(x+y)/32.f};
        ColorGradingSettings s;Check(f.Run(source,s).empty(),"Disabled effects skip dispatch");
        Check(!f.slot.bloomHeap&&!f.slot.bloomPyramid[0],"Disabled Bloom allocates no pyramid resources");
        s.contrast={true,1};Check(f.Run(source,s).empty(),"Neutral enabled effect skips dispatch");
        s={};s.lutEnabled=true;s.lutFile="identity.png";
        auto out=f.Run(source,s);for(size_t i=0;i<out.size();i++){Near(out[i].r,source[i].r,.00003f,"Identity LUT R");Near(out[i].g,source[i].g,.00003f,"Identity LUT G");Near(out[i].b,source[i].b,.00003f,"Identity LUT B");}
        auto hdr=source;for(auto& p:hdr){p.r*=6;p.g*=3;p.b*=2;}
        out=f.Run(hdr,s,true);for(size_t i=0;i<out.size();i++){Near(out[i].r,hdr[i].r,.0001f,"HDR identity R");Near(out[i].g,hdr[i].g,.0001f,"HDR identity G");Near(out[i].b,hdr[i].b,.0001f,"HDR identity B");}
        s.lutFile="invert.png";s.lutIntensity=.5f;out=f.Run(source,s);for(size_t i=0;i<out.size();i++){Near(out[i].r,source[i].r+.5f*(1-2*Clamp(source[i].r)),.00003f,"LUT intensity R");Near(out[i].g,source[i].g+.5f*(1-2*Clamp(source[i].g)),.00003f,"LUT intensity G");}
        s.lutFile="tiled.png";s.lutIntensity=1;out=f.Run(source,s);for(size_t i=0;i<out.size();i++)Near(out[i].r,source[i].r,.0021f,"64 cube tiled identity");
        s.lutFile="identity.png";out=f.Run(source,s);WriteLut((root/L"identity.png").c_str(),2,2,true);++s.lutReloadRevision;out=f.Run(source,s);Near(out[3].r,source[3].r+1-2*Clamp(source[3].r),.00003f,"Same-name refresh");
        s={};s.exposure={true,1};out=f.Run(hdr,s,true);for(size_t i=0;i<out.size();i++){Near(out[i].r,hdr[i].r*2,.0002f,"Linear exposure stop");Near(out[i].g,hdr[i].g*2,.0002f,"Linear exposure G");}
        out=f.Run(source,s,false);Near(out[4].r,Encode(Decode(source[4].r)*2),.0001f,"SDR exposure stop");
        s={};s.monochrome={true,1};out=f.Run(source,s);for(const auto& p:out){Near(p.r,p.g,.00001f,"Monochrome RG");Near(p.g,p.b,.00001f,"Monochrome GB");}
        s={};s.saturation={true,0};out=f.Run(source,s);for(const auto& p:out)Near(p.r,p.g,.00001f,"Zero saturation");
        for(int effect=0;effect<13;effect++){
            s={};ColorGradingControl* controls[]={&s.exposure,&s.contrast,&s.saturation,&s.temperature,&s.tint,&s.shadows,&s.midtones,&s.highlights,&s.sharpen,&s.bloom,&s.vignette,&s.grain,&s.monochrome};
            *controls[effect]={true,.6f};out=f.Run(source,s);Check(out.size()==source.size(),"Enabled effect dispatch");float diff=0;
            for(size_t i=0;i<out.size();i++){Check(std::isfinite(out[i].r)&&std::isfinite(out[i].g)&&std::isfinite(out[i].b),"Finite effect output");diff+=std::abs(out[i].r-source[i].r)+std::abs(out[i].g-source[i].g)+std::abs(out[i].b-source[i].b);}
            Check(diff>1e-4f,"Enabled effect changes image");
        }
        std::vector<Pixel> edge(W*H);for(UINT y=0;y<H;y++)for(UINT x=0;x<W;x++)edge[y*W+x]={x<W/2?.25f:.75f,.5f,.5f,.8f};
        s={};s.sharpen={true,1};out=f.Run(edge,s);
        Check(out[W/2-1].r<.20f&&out[W/2].r>.80f,"Sharpen visibly strengthens existing edge extrema");
        const auto sharpen1=out;s.sharpen.value=3;out=f.Run(edge,s);
        Check(out[W/2-1].r<sharpen1[W/2-1].r-.03f&&out[W/2].r>sharpen1[W/2].r+.03f,"Sharpen strength3 exceeds strength1");
        std::vector<Pixel> flat(W*H,Pixel{.5f,.5f,.5f,.3f});s={};s.grain={true,1};out=f.Run(flat,s);
        float grainMean=0;for(const auto& p:out)grainMean+=std::abs(p.r-.5f);grainMean/=out.size();
        Check(grainMean>.045f&&grainMean<.075f,"Film grain full range is visible and bounded");
        VerifyBloom(f);
        const auto fxRoot=PostFxRoot(nullptr);std::filesystem::create_directories(fxRoot);
        auto writeFx=[&](const std::string& name,const std::string& code){std::ofstream file(fxRoot/PostFxWide(name),std::ios::binary|std::ios::trunc);file<<code;Check(bool(file),"Write isolated fixture effect");};
        writeFx("a-bias.fx","// @dxl name_en Bias\n// @dxl name_zh 偏移\n// @dxl param amount -1 1 0.1 | Amount | 强度\nfloat3 DXL_Effect(float2 uv,float3 color){return color+DXL_Parameter(0);}\n");
        writeFx("B-gain.fx","// @dxl param gain 0 2 1.5 | Gain | 增益\nfloat3 DXL_Effect(float2 uv,float3 color){return color*DXL_Parameter(0);}\n");
        auto catalog=EnumeratePostFx(nullptr);Check(catalog.size()>=2&&catalog[0].file=="a-bias.fx"&&catalog[1].file=="B-gain.fx","Catalog uses case-insensitive deterministic alphabetic order");
        Check(catalog[0].parameters.size()==1&&catalog[0].parameters[0].titleZh=="强度"&&catalog[0].titleEn=="Bias"&&!catalog[0].error.size(),"Bilingual parameter metadata");
        for(const auto* name:{"../bad.fx","C:\\bad.fx","bad.fx:stream",".hidden.fx","bad.png"})Check(!ValidatePostFxFilename(name),"Effect traversal rejected");
        s={};s.fx={{"B-gain.fx",true,{}},{"a-bias.fx",true,{}}};out=f.Run(source,s);
        Check(!out.empty(),f.grading.LastError());for(size_t i=0;i<out.size();i++)Near(out[i].r,(source[i].r+.1f)*1.5f,.00001f,"FX defaults and alphabetic chain");
        s.fx[0].values={9};s.fx[1].values={-.3f};out=f.Run(source,s);Near(out[8].r,(source[8].r-.3f)*2,.00001f,"FX values bounded by metadata");
        s={};s.fx={{"a-bias.fx",true,{.2f}}};s.monochrome={true,1};out=f.Run(source,s);for(const auto& p:out)Near(p.r,p.g,.00001f,"Builtins precede extension passes");
        writeFx("a-bias.fx","// @dxl param amount -1 1 0.1 | Amount | 强度\nfloat3 DXL_Effect(float2 uv,float3 color){return color+DXL_Parameter(0)*2;}\n");
        s={};s.fx={{"a-bias.fx",true,{.2f}}};out=f.Run(source,s);Near(out[8].r,source[8].r+.2f,.00001f,"Compiled effect cached until refresh");
        ++s.fxReloadRevision;out=f.Run(source,s);Near(out[8].r,source[8].r+.4f,.00001f,"Effect refresh replaces cached program");
        s.fx[0].values={std::numeric_limits<float>::quiet_NaN()};out=f.Run(source,s);Near(out[8].r,source[8].r+.2f,.00001f,"FX nonfinite parameter uses metadata default");
        writeFx("broken.fx","float3 DXL_Effect(float2 uv,float3 color) { this is invalid HLSL; }\n");
        s={};s.fx={{"broken.fx",false,{}}};Check(f.Run(source,s).empty(),"Disabled invalid FX does not compile or dispatch");
        s.fx[0].enabled=true;Check(f.Run(source,s).empty(),"Invalid FX skips dispatch");Check(std::strstr(f.grading.LastError(),"broken.fx"),"Per-file compiler error names file");
        s.monochrome={true,1};out=f.Run(source,s);Check(!out.empty(),"Invalid FX preserves working builtins");
        writeFx("include.fx","#include \"outside.h\"\nfloat3 DXL_Effect(float2 uv,float3 color) { return color; }\n");
        s={};s.fx={{"include.fx",true,{}}};Check(f.Run(source,s).empty()&&*f.grading.LastError(),"Effect includes cannot read arbitrary paths");
        writeFx("binding.fx","Texture2D<float4> Other:register(t7);\nfloat3 DXL_Effect(float2 uv,float3 color) { return Other.Load(int3(0,0,0)).rgb; }\n");
        s.fx={{"binding.fx",true,{}}};Check(f.Run(source,s).empty()&&*f.grading.LastError(),"Additional shader resources rejected");
        writeFx("nonfinite.fx","float3 DXL_Effect(float2 uv,float3 color) { return color/(uv.x-uv.x); }\n");
        s.fx={{"nonfinite.fx",true,{}}};out=f.Run(source,s);Check(!out.empty(),f.grading.LastError());for(size_t i=0;i<out.size();i++)Near(out[i].r,source[i].r,0,"Nonfinite FX output preserves input");
        writeFx("bad-metadata.fx","// @dxl param strength 1 0 0 | Bad | Bad\nfloat3 DXL_Effect(float2 uv,float3 color){return color;}");
        Check(!ReadPostFxDefinition(fxRoot,"bad-metadata.fx").error.empty(),"Invalid parameter metadata rejected before compilation");
        s={};s.exposure={true,1};s.grain={true,.5f};s.fx={{"B-gain.fx",true,{}}};s.lutEnabled=true;s.lutFile="identity.png";
        Check(s.BasicOnly().AnyBasicActive()&&!s.BasicOnly().AnyFinalActive(),"Basic route excludes all final effects");
        Check(s.FinalOnly().AnyFinalActive()&&!s.FinalOnly().AnyBasicActive(),"Final route excludes basic adjustments");
        s={};s.fx={{"sample-Sepia.fx",true,{1}}};out=f.Run(source,s);Check(!out.empty(),f.grading.LastError());
        Near(out[8].r,source[8].r*.393f+source[8].g*.769f+source[8].b*.189f,.00001f,"Bundled sepia compiles and matches transform");
        s.fx={{"sample-Letterbox.fx",true,{.1f,1}}};out=f.Run(source,s);Check(!out.empty(),f.grading.LastError());
        Near(out[0].r,0,0,"Bundled letterbox shades top");Near(out[(H/2)*W+8].r,source[(H/2)*W+8].r,0,"Bundled letterbox preserves center");
        s={};
        for(int i=0;i<33;i++) {
            char name[32]{};std::snprintf(name,sizeof(name),"chain-%02d.fx",i);
            writeFx(name,"float3 DXL_Effect(float2 uv,float3 color){return color+0.001;}\n");
            s.fx.push_back({name,true,{}});
        }
        out=f.Run(source,s);Check(!out.empty(),f.grading.LastError());
        Near(out[8].r,source[8].r+.032f,.00001f,"Full32-pass descriptor/scratch chain and active cap");
        Check(std::strstr(f.grading.LastError(),"32"),"Active effect cap diagnostic");
        s.fx.pop_back();out=f.Run(source,s);Check(!out.empty(),f.grading.LastError());
        Near(out[8].r,source[8].r+.032f,.00001f,"Retired chain scratch reuse");
        std::puts("PASS: DXL .fx metadata/order, defaults/bounds, pixel chain, builtins order, refresh/cache, disabled/invalid/include/resource rejection, nonfinite fallback and route split");
        s={};s.lutEnabled=true;s.lutFile="missing.png";Check(f.Run(source,s).empty(),"Missing LUT skips safely");Check(std::strlen(f.grading.LastError())>0,"Missing LUT diagnostic");
        s.exposure={true,1};out=f.Run(source,s);Check(!out.empty(),"Invalid LUT retains other effects");
        s.lutEnabled=false;out=f.Run(source,s);Check(!out.empty()&&!*f.grading.LastError(),"Disabled LUT clears stale error");
        s.lutEnabled=true;out=f.Run(source,s);Check(!out.empty()&&*f.grading.LastError(),"Reenabled invalid LUT restores diagnostic");
        s.lutIntensity=0;out=f.Run(source,s);Check(!out.empty()&&!*f.grading.LastError(),"Zero-strength LUT clears stale error");
        s={};s.exposure={true,std::numeric_limits<float>::quiet_NaN()};Check(f.Run(source,s).empty(),"NaN does not dispatch");
        f.CheckDebug();std::puts("PASS: GPU color grading: inactive/neutral skip, 13 effects, alpha, SDR/HDR exposure, LUT identity/invert/strength/layout/reload/rejection, D3D12 validation");
        CoUninitialize();return 0;
    }catch(const std::exception& e){std::printf("FAIL: %s\n",e.what());return 1;}
}
