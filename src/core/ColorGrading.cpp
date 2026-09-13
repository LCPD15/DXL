#include "ColorGrading.h"
#include "NrColorFormat.h"
#include "shaders/precompiled/ColorGrading.h"
#include "shaders/precompiled/Bloom.h"
#include <wincodec.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <limits>
#include <d3dcompiler.h>
#include <d3d11shader.h>

namespace DXL {
using Microsoft::WRL::ComPtr;
namespace {
struct Params {
    uint32_t width, height, flags, frame;
    float exposure, contrast, saturation, temperature;
    float tint, shadows, midtones, highlights;
    float sharpen, bloom, vignette, grain;
    float monochrome, lutIntensity, lutSize, padding;
    float bloomScatter, bloomSaturation, bloomUnused0, bloomUnused1;
};
static_assert(sizeof(Params)==96);
float Bounded(float v,float neutral,float lo,float hi) noexcept {
    return std::isfinite(v)?std::clamp(v,lo,hi):neutral;
}
struct ComScope {
    HRESULT result=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    ~ComScope(){if(SUCCEEDED(result))CoUninitialize();}
};
bool ColorFormat(DXGI_FORMAT f) noexcept {
    switch(NrColorViewFormat(f)) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_R10G10B10A2_UNORM:case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:case DXGI_FORMAT_R32G32B32A32_FLOAT:return true;
    default:return false;
    }
}
}

bool ColorGrading::ValidateLutFilename(const std::string& name) noexcept {
    if(name.size()<5||name.size()>240||name.front()=='.'||name.back()=='.'||name.back()==' ')return false;
    for(unsigned char c:name)if(c<32||c==127||c=='/'||c=='\\'||c==':'||c=='*'||c=='?'||c=='"'||c=='<'||c=='>'||c=='|')return false;
    const char* ext=name.data()+name.size()-4;
    if(ext[0]!='.'||(ext[1]!='p'&&ext[1]!='P')||(ext[2]!='n'&&ext[2]!='N')||(ext[3]!='g'&&ext[3]!='G'))return false;
    return MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,name.c_str(),static_cast<int>(name.size()),nullptr,0)>0;
}

bool ColorGrading::InferLutLayout(uint32_t width,uint32_t height,uint32_t& cubeSize,uint32_t& tilesX) noexcept {
    cubeSize=tilesX=0;
    if(!width||!height||width>4096||height>4096)return false;
    for(uint32_t n=2;n<=64;n++)if(width%n==0&&height%n==0&&uint64_t(width)*height==uint64_t(n)*n*n){
        cubeSize=n;tilesX=width/n;return true;
    }
    return false;
}

void ColorGrading::SetModule(HMODULE module) noexcept {
    try {
        std::wstring path(32768,L'\0');
        const DWORD n=GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size()));
        if(!n||n>=path.size()){_lastError="Cannot locate the LUT folder";return;}
        path.resize(n);
        const auto fxRoot=std::filesystem::path(path).parent_path()/L"post-processing";
        if(fxRoot!=_fxRoot){_fxRoot=fxRoot;_fxPrograms.clear();}
        auto root=(std::filesystem::path(path).parent_path()/L"lut").wstring();
        if(root!=_lutRoot){_lutRoot=std::move(root);_lutAttempted=false;_lutPixels.clear();_cubeSize=0;}
    }catch(...){_lastError="Cannot locate the LUT folder";}
}

bool ColorGrading::Initialize(ID3D12Device* device) noexcept {
    if(_pso)return _device.Get()==device;
    if(!device)return false;
    _device=device;
    D3D12_DESCRIPTOR_RANGE ranges[2]{};
    ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,7,0,0,0};
    ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,7};
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[0].Constants={0,0,sizeof(Params)/4};
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[1].DescriptorTable={2,ranges};
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD=D3D12_FLOAT32_MAX;
    sampler.ShaderVisibility=D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC desc{2,parameters,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob,error;
    if(FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error))||
        FAILED(device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&_root)))){
        _lastError="Color grading root signature creation failed";return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};
    pso.pRootSignature=_root.Get();pso.CS={g_colorGradingCs,sizeof(g_colorGradingCs)};
    if(FAILED(device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&_pso)))){
        _lastError="Color grading shader creation failed";return false;
    }
    _lastError.clear();return true;
}

bool ColorGrading::LoadLut(const std::string& name) {
    if(_lutAttempted&&name==_loadedFilename){
        if(!_lutPixels.empty()){_lastError.clear();return true;}
        _lastError=_lutLoadError;
        return false;
    }
    _loadedFilename=name;_lutAttempted=true;_lutPixels.clear();_cubeSize=0;++_lutRevision;
    _lutLoadError="Cannot load the selected PNG LUT";
    if(!ValidateLutFilename(name)||_lutRoot.empty()){_lastError=_lutLoadError="Choose a PNG file directly inside the lut folder";return false;}
    const int count=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,name.data(),static_cast<int>(name.size()),nullptr,0);
    std::wstring wide(count,L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,name.data(),static_cast<int>(name.size()),wide.data(),count);
    const auto path=std::filesystem::path(_lutRoot)/wide;
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    if(!GetFileAttributesExW(path.c_str(),GetFileExInfoStandard,&attributes)||
        (attributes.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||
        attributes.nFileSizeHigh||attributes.nFileSizeLow>16*1024*1024){_lastError=_lutLoadError="LUT file is missing, too large, or is a link";return false;}
    ComScope com;
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> converter;
    GUID format{};
    if(FAILED(CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory)))||
        FAILED(factory->CreateDecoderFromFilename(path.c_str(),nullptr,GENERIC_READ,WICDecodeMetadataCacheOnDemand,&decoder))||
        FAILED(decoder->GetContainerFormat(&format))||!IsEqualGUID(format,GUID_ContainerFormatPng)||FAILED(decoder->GetFrame(0,&frame))){
        _lastError=_lutLoadError="Cannot decode this PNG LUT";return false;
    }
    UINT width=0,height=0;uint32_t n=0,tiles=0;
    if(FAILED(frame->GetSize(&width,&height))||!InferLutLayout(width,height,n,tiles)){
        _lastError=_lutLoadError="Unsupported PNG LUT layout (expected a tiled 2-64 cube)";return false;
    }
    if(FAILED(factory->CreateFormatConverter(&converter))||FAILED(converter->Initialize(frame.Get(),GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone,nullptr,0,WICBitmapPaletteTypeCustom))){_lastError=_lutLoadError="Cannot convert this PNG LUT";return false;}
    std::vector<uint8_t> image(static_cast<size_t>(width)*height*4);
    if(FAILED(converter->CopyPixels(nullptr,width*4,static_cast<UINT>(image.size()),image.data()))){_lastError=_lutLoadError="Cannot read PNG LUT pixels";return false;}
    _lutPixels.resize(static_cast<size_t>(n)*n*n*4);
    for(uint32_t b=0;b<n;b++)for(uint32_t g=0;g<n;g++)for(uint32_t r=0;r<n;r++){
        const size_t src=((b/tiles*n+g)*width+b%tiles*n+r)*4;
        const size_t dst=((b*n+g)*n+r)*4;
        std::memcpy(_lutPixels.data()+dst,image.data()+src,4);
    }
    _cubeSize=n;_lastError.clear();_lutLoadError.clear();return true;
}

bool ColorGrading::UploadLut(ID3D12GraphicsCommandList* list,ColorGradingFrame& slot) {
    if(slot.lut&&slot.lutRevision==_lutRevision)return true;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    desc.Width=desc.Height=_cubeSize;desc.DepthOrArraySize=static_cast<UINT16>(_cubeSize);desc.MipLevels=1;
    desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> lut,upload;
    if(FAILED(_device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&lut))))return false;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT rows=0;UINT64 rowSize=0,total=0;
    _device->GetCopyableFootprints(&desc,0,1,0,&footprint,&rows,&rowSize,&total);
    D3D12_RESOURCE_DESC buffer{};buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=total;
    buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=1;buffer.SampleDesc.Count=1;buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    heap.Type=D3D12_HEAP_TYPE_UPLOAD;
    if(FAILED(_device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload))))return false;
    void* mapped=nullptr;D3D12_RANGE noRead{};
    if(FAILED(upload->Map(0,&noRead,&mapped)))return false;
    for(uint32_t z=0;z<_cubeSize;z++)for(uint32_t y=0;y<_cubeSize;y++)
        std::memcpy(static_cast<uint8_t*>(mapped)+footprint.Offset+(z*rows+y)*footprint.Footprint.RowPitch,
            _lutPixels.data()+(z*_cubeSize+y)*_cubeSize*4,_cubeSize*4);
    upload->Unmap(0,nullptr);
    // All allocation/map failure paths occur before recording any references.
    slot.lut=std::move(lut);slot.upload=std::move(upload);slot.lutRevision=_lutRevision;
    D3D12_TEXTURE_COPY_LOCATION destination{},source{};
    destination.pResource=slot.lut.Get();destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    source.pResource=slot.upload.Get();source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;source.PlacedFootprint=footprint;
    list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={slot.lut.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE};
    list->ResourceBarrier(1,&barrier);return true;
}

bool ColorGrading::PrepareBloom(ID3D12GraphicsCommandList* list,ID3D12Resource* source,
    uint32_t width,uint32_t height,const ColorGradingSettings& settings,bool inputLinear,ColorGradingFrame& slot) {
    struct BloomParams {
        uint32_t sourceWidth,sourceHeight,targetWidth,targetHeight;
        uint32_t firstLevel,inputLinear;
        float threshold,softKnee,radius,padding[3];
    };
    static_assert(sizeof(BloomParams)==48);
    if(!_bloomPso) {
        D3D12_DESCRIPTOR_RANGE ranges[2]{};
        ranges[0]={D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};
        ranges[1]={D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,1};
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        parameters[0].Constants={0,0,sizeof(BloomParams)/4};
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[1].DescriptorTable={2,ranges};
        D3D12_STATIC_SAMPLER_DESC sampler{};
        sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.MaxLOD=D3D12_FLOAT32_MAX;
        D3D12_ROOT_SIGNATURE_DESC desc{2,parameters,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
        ComPtr<ID3DBlob> blob,error;
        if(FAILED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error)))return false;
        ComPtr<ID3D12RootSignature> root;
        if(FAILED(_device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root))))return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=root.Get();
        pso.CS={g_bloomDownsampleCs,sizeof(g_bloomDownsampleCs)};
        if(FAILED(_device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&_bloomPso))))return false;
        _bloomRoot=std::move(root);
    }
    if(!slot.bloomHeap) {
        D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap.NumDescriptors=ColorGradingFrame::BloomLevels*2;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if(FAILED(_device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&slot.bloomHeap))))return false;
    }
    // This slot has retired. Allocate every level before recording references;
    // a failed resize leaves no partial GPU work. Each level starts/ends as SRV,
    // so discarding an unsubmitted command list does not desynchronize state.
    uint32_t levelWidth=width,levelHeight=height;
    for(uint32_t i=0;i<ColorGradingFrame::BloomLevels;i++) {
        levelWidth=(levelWidth+1)/2;levelHeight=(levelHeight+1)/2;
        if(slot.bloomPyramid[i]) {
            const auto existing=slot.bloomPyramid[i]->GetDesc();
            if(existing.Width==levelWidth&&existing.Height==levelHeight)continue;
        }
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width=levelWidth;desc.Height=levelHeight;desc.DepthOrArraySize=desc.MipLevels=1;
        desc.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;desc.SampleDesc.Count=1;
        desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> resource;
        if(FAILED(_device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&resource))))return false;
        slot.bloomPyramid[i]=std::move(resource);
    }
    const auto stride=_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto cpu=slot.bloomHeap->GetCPUDescriptorHandleForHeapStart();
    for(uint32_t i=0;i<ColorGradingFrame::BloomLevels;i++) {
        auto* input=i?slot.bloomPyramid[i-1].Get():source;
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=NrColorViewFormat(input->GetDesc().Format);
        srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
        _device->CreateShaderResourceView(input,&srv,cpu);cpu.ptr+=stride;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        _device->CreateUnorderedAccessView(slot.bloomPyramid[i].Get(),nullptr,&uav,cpu);cpu.ptr+=stride;
    }
    BloomParams params{};params.threshold=Bounded(settings.bloomThreshold,.65f,0,2);
    params.softKnee=Bounded(settings.bloomSoftKnee,.5f,0,1);params.radius=Bounded(settings.bloomRadius,1,.25f,3);
    params.sourceWidth=width;params.sourceHeight=height;
    auto gpu=slot.bloomHeap->GetGPUDescriptorHandleForHeapStart();
    ID3D12DescriptorHeap* heaps[]={slot.bloomHeap.Get()};
    list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(_bloomRoot.Get());list->SetPipelineState(_bloomPso.Get());
    for(uint32_t i=0;i<ColorGradingFrame::BloomLevels;i++) {
        const auto desc=slot.bloomPyramid[i]->GetDesc();params.targetWidth=static_cast<uint32_t>(desc.Width);params.targetHeight=desc.Height;
        params.firstLevel=i==0;params.inputLinear=inputLinear;
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={slot.bloomPyramid[i].Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS};
        list->ResourceBarrier(1,&barrier);
        list->SetComputeRoot32BitConstants(0,sizeof(BloomParams)/4,&params,0);list->SetComputeRootDescriptorTable(1,gpu);
        list->Dispatch((params.targetWidth+7)/8,(params.targetHeight+7)/8,1);
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(1,&barrier);
        gpu.ptr+=stride*2;params.sourceWidth=params.targetWidth;params.sourceHeight=params.targetHeight;
    }
    return true;
}

bool ColorGrading::RecordBuiltins(ID3D12GraphicsCommandList* list,ID3D12Resource* source,ID3D12Resource* target,
    uint32_t width,uint32_t height,const ColorGradingSettings& s,bool inputLinear,uint32_t frameIndex,ColorGradingFrame& slot) {
    if(!s.AnyActive())return false;
    try {
        if(!_pso||!list||!source||!target||source==target||!width||!height)return false;
        const auto a=source->GetDesc(),b=target->GetDesc();
        if(a.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||b.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||
            a.SampleDesc.Count!=1||b.SampleDesc.Count!=1||a.DepthOrArraySize!=1||b.DepthOrArraySize!=1||
            a.Width<width||b.Width<width||a.Height<height||b.Height<height||
            !ColorFormat(a.Format)||!ColorFormat(b.Format)||(a.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)||
            !(b.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)){_lastError="Unsupported color grading resources";return false;}
        Params p{};p.width=width;p.height=height;p.frame=frameIndex;
        const ColorGradingControl* controls[]={&s.exposure,&s.contrast,&s.saturation,&s.temperature,&s.tint,&s.shadows,&s.midtones,&s.highlights,&s.sharpen,&s.bloom,&s.vignette,&s.grain,&s.monochrome};
        float* values[]={&p.exposure,&p.contrast,&p.saturation,&p.temperature,&p.tint,&p.shadows,&p.midtones,&p.highlights,&p.sharpen,&p.bloom,&p.vignette,&p.grain,&p.monochrome};
        for(uint32_t i=0;i<13;i++){
            const float neutral=(i==1||i==2)?1.0f:0.0f;
            const float lo=i==0?-4.0f:(i>=3&&i<=7?-1.0f:0.0f);
            const float hi=i==0?4.0f:((i==8||i==9)?3.0f:((i==1||i==2)?2.0f:1.0f));
            *values[i]=Bounded(controls[i]->value,neutral,lo,hi);
            if(controls[i]->enabled&&std::abs(*values[i]-neutral)>0.00001f)p.flags|=1u<<i;
        }
        if(s.lutReloadRevision!=_requestedRevision){_requestedRevision=s.lutReloadRevision;_lutAttempted=false;}
        const bool wantsLut=s.lutEnabled&&Bounded(s.lutIntensity,0,0,1)>0.00001f;
        if(!wantsLut)_lastError.clear();
        bool lut=wantsLut&&LoadLut(s.lutFile);
        if(lut){p.flags|=1u<<13;p.lutIntensity=Bounded(s.lutIntensity,0,0,1);p.lutSize=static_cast<float>(_cubeSize);}
        if(!p.flags)return false;
        if(inputLinear)p.flags|=0x10000;
        if(!slot.heap){
            D3D12_DESCRIPTOR_HEAP_DESC heap{};heap.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;heap.NumDescriptors=8;heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if(FAILED(_device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&slot.heap)))){_lastError="Color grading descriptor allocation failed";return false;}
        }
        if(lut&&!UploadLut(list,slot)){
            p.flags&=~(1u<<13);lut=false;_lastError="Color grading LUT GPU upload failed";
            if(!(p.flags&0xffff))return false;
        }
        bool bloom=(p.flags&(1u<<9))!=0;
        if(bloom&&!PrepareBloom(list,source,width,height,s,inputLinear,slot)) {
            p.flags&=~(1u<<9);bloom=false;_lastError="Bloom pyramid preparation failed";
            if(!(p.flags&0xffff))return false;
        }
        p.bloomScatter=Bounded(s.bloomScatter,.7f,0,1);
        p.bloomSaturation=Bounded(s.bloomSaturation,1,0,2);
        if(bloom) {
            const auto outputFormat=NrColorViewFormat(b.Format);
            if(outputFormat==DXGI_FORMAT_R16G16B16A16_FLOAT)p.padding=65504;
            if(outputFormat==DXGI_FORMAT_R11G11B10_FLOAT)p.padding=64512;
        }
        auto cpu=slot.heap->GetCPUDescriptorHandleForHeapStart();
        const auto stride=_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=NrColorViewFormat(a.Format);srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
        _device->CreateShaderResourceView(source,&srv,cpu);cpu.ptr+=stride;
        srv={};srv.Format=DXGI_FORMAT_R8G8B8A8_UNORM;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE3D;
        srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture3D.MipLevels=1;
        _device->CreateShaderResourceView(lut?slot.lut.Get():nullptr,&srv,cpu);cpu.ptr+=stride;
        for(uint32_t i=0;i<ColorGradingFrame::BloomLevels;i++) {
            srv={};srv.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
            srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
            _device->CreateShaderResourceView(bloom?slot.bloomPyramid[i].Get():nullptr,&srv,cpu);cpu.ptr+=stride;
        }
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=NrColorViewFormat(b.Format);uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
        _device->CreateUnorderedAccessView(target,nullptr,&uav,cpu);
        ID3D12DescriptorHeap* heaps[]={slot.heap.Get()};
        list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(_root.Get());list->SetPipelineState(_pso.Get());
        list->SetComputeRoot32BitConstants(0,sizeof(Params)/4,&p,0);
        list->SetComputeRootDescriptorTable(1,slot.heap->GetGPUDescriptorHandleForHeapStart());
        list->Dispatch((width+7)/8,(height+7)/8,1);return true;
    }catch(...){_lastError="Color grading could not prepare this frame";return false;}
}

} // namespace DXL
#include "PostFxRuntime.h"
