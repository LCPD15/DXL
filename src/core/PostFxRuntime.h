#pragma once
// Included by ColorGrading.cpp only. Compiled programs and all GPU references
// are retained by the caller's retired frame slot, including across refresh.
namespace DXL {
namespace {
const char* FxPrefix=R"DXL(
Texture2D<float4> DXL_Source : register(t0);
RWTexture2D<float4> DXL_Target : register(u0);
SamplerState DXL_LinearClamp : register(s0);
cbuffer DXL_Constants : register(b0) {
    uint2 DXL_Size;
    uint DXL_Frame;
    uint DXL_LinearInput;
    float4 DXL_Values[4];
};
float2 DXL_TexelSize() { return 1.0 / float2(DXL_Size); }
float DXL_Parameter(uint index) { return DXL_Values[min(index,15u)/4][min(index,15u)%4]; }
float3 DXL_Sample(float2 uv) { return DXL_Source.SampleLevel(DXL_LinearClamp,uv,0).rgb; }
float3 DXL_Read(int2 pixel) { return DXL_Source.Load(int3(clamp(pixel,int2(0,0),int2(DXL_Size)-1),0)).rgb; }
)DXL";
const char* FxSuffix=R"DXL(
[numthreads(8,8,1)]
void DXL_InternalMain(uint3 id:SV_DispatchThreadID) {
    if(any(id.xy>=DXL_Size))return;
    float4 original=DXL_Source.Load(int3(id.xy,0));
    float3 result=DXL_Effect((float2(id.xy)+0.5)/float2(DXL_Size),original.rgb);
    DXL_Target[id.xy]=float4(all(isfinite(result))?result:original.rgb,original.a);
}
)DXL";
void FxTransition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};
    list->ResourceBarrier(1,&barrier);
}
}

bool ColorGrading::PrepareFxRoot() {
    if(_fxSignature)return true;
    D3D12_DESCRIPTOR_RANGE ranges[2]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,1,0,0,1}};
    D3D12_ROOT_PARAMETER params[2]{};
    params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[0].Constants={0,0,20};
    params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[1].DescriptorTable={2,ranges};
    D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.MaxLOD=D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC desc{2,params,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ComPtr<ID3DBlob> blob,error;
    return SUCCEEDED(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error))&&
        SUCCEEDED(_device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&_fxSignature)));
}
ColorGrading::PostFxProgram& ColorGrading::PrepareFx(const std::string& filename) {
    auto [it,inserted]=_fxPrograms.try_emplace(filename);
    auto& result=it->second;
    if(!inserted)return result;
    result.definition=ReadPostFxDefinition(_fxRoot,filename);
    if(!result.definition.error.empty()){result.error=result.definition.error;return result;}
    if(result.definition.reshade){result.error="This effect is managed by the Custom FX panel";return result;}
    if(!PrepareFxRoot()){result.error="Cannot create effect root signature";return result;}
    // Only our wrapper can bind textures. No filesystem include handler is
    // supplied, so #include cannot fetch arbitrary files from the game machine.
    const std::string source=std::string(FxPrefix)+"\n#line 1\n"+result.definition.source+"\n"+FxSuffix;
    ComPtr<ID3DBlob> shader,error;
    const HRESULT hr=D3DCompile(source.data(),source.size(),filename.c_str(),nullptr,nullptr,"DXL_InternalMain","cs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS|D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&shader,&error);
    if(FAILED(hr)) {
        result.error=error?std::string(static_cast<const char*>(error->GetBufferPointer()),std::min<size_t>(error->GetBufferSize(),2048)):"Shader compilation failed";
        while(!result.error.empty()&&result.error.back()=='\0')result.error.pop_back();
        return result;
    }
    ComPtr<ID3D11ShaderReflection> reflection;
    if(FAILED(D3DReflect(shader->GetBufferPointer(),shader->GetBufferSize(),IID_PPV_ARGS(&reflection)))){result.error="Cannot validate effect shader";return result;}
    D3D11_SHADER_DESC reflected{};reflection->GetDesc(&reflected);
    for(UINT i=0;i<reflected.BoundResources;i++) {
        D3D11_SHADER_INPUT_BIND_DESC bind{};
        if(FAILED(reflection->GetResourceBindingDesc(i,&bind))){result.error="Cannot inspect effect bindings";return result;}
        const bool allowed=bind.BindCount==1&&bind.BindPoint==0&&
            ((bind.Type==D3D_SIT_TEXTURE&&std::strcmp(bind.Name,"DXL_Source")==0)||
             (bind.Type==D3D_SIT_UAV_RWTYPED&&std::strcmp(bind.Name,"DXL_Target")==0)||
             (bind.Type==D3D_SIT_CBUFFER&&std::strcmp(bind.Name,"DXL_Constants")==0)||
             (bind.Type==D3D_SIT_SAMPLER&&std::strcmp(bind.Name,"DXL_LinearClamp")==0));
        if(!allowed){result.error="Only the DXL source, target, constants and sampler bindings are supported";return result;}
    }
    UINT x=0,y=0,z=0;reflection->GetThreadGroupSize(&x,&y,&z);
    if(x!=8||y!=8||z!=1){result.error="Effect changed the DXL dispatch contract";return result;}
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso{};pso.pRootSignature=_fxSignature.Get();pso.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
    if(FAILED(_device->CreateComputePipelineState(&pso,IID_PPV_ARGS(&result.pso))))result.error="Cannot create effect pipeline";
    return result;
}

bool ColorGrading::Record(ID3D12GraphicsCommandList* list,ID3D12Resource* source,ID3D12Resource* target,
    uint32_t width,uint32_t height,const ColorGradingSettings& settings,bool inputLinear,uint32_t frameIndex,ColorGradingFrame& slot) noexcept {
    if(!settings.AnyActive())return false;
    try {
        if(!list||!source||!target||source==target||!width||!height||!_device)return false;
        if(settings.fxReloadRevision!=_fxRequestedRevision){_fxRequestedRevision=settings.fxReloadRevision;_fxPrograms.clear();}
        struct Active { const PostFxSetting* setting;PostFxProgram* program; };
        std::vector<const PostFxSetting*> ordered;
        for(const auto& effect:settings.fx)if(effect.enabled&&ValidatePostFxFilename(effect.file))ordered.push_back(&effect);
        std::sort(ordered.begin(),ordered.end(),[](const auto* a,const auto* b){return PostFxFilenameLess(a->file,b->file);});
        if(ordered.size()>64)ordered.resize(64);
        size_t uncached=0;for(const auto* effect:ordered)uncached+=!_fxPrograms.contains(effect->file);
        if(_fxPrograms.size()+uncached>128)_fxPrograms.clear();
        std::vector<Active> active;std::string errors;
        std::wstring previous;
        for(const auto* effect:ordered) {
            const auto wide=PostFxWide(effect->file);
            if(!previous.empty()&&CompareStringOrdinal(previous.c_str(),-1,wide.c_str(),-1,TRUE)==CSTR_EQUAL)continue;
            previous=wide;
            if(active.size()>=32){errors="Only 32 simultaneous .fx effects are supported";break;}
            auto& program=PrepareFx(effect->file);
            if(program.pso)active.push_back({effect,&program});
            else if(errors.empty())errors=effect->file+": "+program.error;
        }
        if(active.empty()) {
            const bool recorded=RecordBuiltins(list,source,target,width,height,settings,inputLinear,frameIndex,slot);
            if(!errors.empty())_lastError=errors;
            return recorded;
        }
        const auto a=source->GetDesc(),b=target->GetDesc();
        if(a.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||b.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D||
            a.SampleDesc.Count!=1||b.SampleDesc.Count!=1||a.DepthOrArraySize!=1||b.DepthOrArraySize!=1||
            a.Width<width||b.Width<width||a.Height<height||b.Height<height||!ColorFormat(a.Format)||!ColorFormat(b.Format)||
            (a.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)||!(b.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)){
            _lastError="Unsupported post-processing resources";return false;
        }
        auto builtin=settings;builtin.fx.clear();
        const bool wantsBuiltin=builtin.AnyActive();
        const size_t scratchCount=(active.size()>1||wantsBuiltin)?2:0;
        for(size_t i=0;i<scratchCount;i++) {
            auto desc=b;desc.Width=width;desc.Height=height;desc.MipLevels=1;desc.Format=NrColorViewFormat(b.Format);
            desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            const auto old=slot.fxScratch[i]?slot.fxScratch[i]->GetDesc():D3D12_RESOURCE_DESC{};
            if(old.Width==width&&old.Height==height&&old.Format==desc.Format)continue;
            D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
            ComPtr<ID3D12Resource> resource;
            if(FAILED(_device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&resource)))){
                _lastError="Cannot allocate post-processing texture";return false;
            }
            slot.fxScratch[i]=std::move(resource);
        }
        if(!slot.fxHeap) {
            D3D12_DESCRIPTOR_HEAP_DESC desc{};desc.Type=D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;desc.NumDescriptors=64;
            desc.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if(FAILED(_device->CreateDescriptorHeap(&desc,IID_PPV_ARGS(&slot.fxHeap)))){_lastError="Cannot allocate effect descriptors";return false;}
        }
        // Reserve before recording: allocation exceptions must not discard a
        // partially recorded chain or release resources already referenced by it.
        slot.fxPrograms.clear();slot.fxPrograms.reserve(active.size());
        for(const auto& pass:active)slot.fxPrograms.push_back(pass.program->pso);
        ID3D12Resource* current=source;size_t scratchIndex=0;
        if(wantsBuiltin) {
            auto* scratch=slot.fxScratch[0].Get();
            FxTransition(list,scratch,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            const bool built=RecordBuiltins(list,source,scratch,width,height,builtin,inputLinear,frameIndex,slot);
            FxTransition(list,scratch,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            if(built){current=scratch;scratchIndex=1;}
        }
        const auto stride=_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        auto cpu=slot.fxHeap->GetCPUDescriptorHandleForHeapStart();auto gpu=slot.fxHeap->GetGPUDescriptorHandleForHeapStart();
        ID3D12DescriptorHeap* heaps[]={slot.fxHeap.Get()};
        list->SetDescriptorHeaps(1,heaps);list->SetComputeRootSignature(_fxSignature.Get());
        for(size_t i=0;i<active.size();i++) {
            const bool last=i+1==active.size();auto* output=last?target:slot.fxScratch[scratchIndex].Get();
            if(!last)FxTransition(list,output,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            D3D12_SHADER_RESOURCE_VIEW_DESC srv{};srv.Format=NrColorViewFormat(current->GetDesc().Format);
            srv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;srv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;srv.Texture2D.MipLevels=1;
            _device->CreateShaderResourceView(current,&srv,cpu);cpu.ptr+=stride;
            D3D12_UNORDERED_ACCESS_VIEW_DESC uav{};uav.Format=NrColorViewFormat(output->GetDesc().Format);uav.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;
            _device->CreateUnorderedAccessView(output,nullptr,&uav,cpu);cpu.ptr+=stride;
            struct Constants {uint32_t width,height,frame,linear;float values[16];} constants{width,height,frameIndex,inputLinear?1u:0u,{}};
            const auto& metadata=active[i].program->definition.parameters;const auto& values=active[i].setting->values;
            for(size_t j=0;j<metadata.size();j++)constants.values[j]=(j<values.size()&&std::isfinite(values[j]))?
                std::clamp(values[j],metadata[j].minimum,metadata[j].maximum):metadata[j].defaultValue;
            list->SetPipelineState(active[i].program->pso.Get());
            list->SetComputeRoot32BitConstants(0,20,&constants,0);list->SetComputeRootDescriptorTable(1,gpu);gpu.ptr+=stride*2;
            list->Dispatch((width+7)/8,(height+7)/8,1);
            if(!last)FxTransition(list,output,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            current=output;scratchIndex^=1;
        }
        if(!errors.empty())_lastError=errors;
        else if(!wantsBuiltin)_lastError.clear();
        return true;
    }catch(...){_lastError="Could not prepare post-processing effects";return false;}
}
}
