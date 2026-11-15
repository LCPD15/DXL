#include "DlssFgBackend.h"
#include "ComputePasses.h"
#include "NgxSession.h"
#include "OpticalFlow.h"
#include "../common/Log.h"
#include <wrl/client.h>
#include <DirectXMath.h>
#include <nvsdk_ngx_helpers_dlssg.h>
#include <cfloat>
#include <cstdio>

namespace DXL {
namespace {
using Microsoft::WRL::ComPtr;
constexpr auto Common = D3D12_RESOURCE_STATE_COMMON;
constexpr auto Read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr auto Write = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
                D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &b);
}

D3D12_RESOURCE_DESC BufferDesc(D3D12_RESOURCE_FLAGS flags, UINT bytes = sizeof(UINT)) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = bytes;
    d.Height = d.DepthOrArraySize = d.MipLevels = d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    d.Flags = flags;
    return d;
}

NVSDK_NGX_DLSSG_Opt_Eval_Params FixedCamera(UINT width, UINT height, bool reset, bool hdr) {
    using namespace DirectX;
    NVSDK_NGX_DLSSG_Opt_Eval_Params p{};
    p.cameraNear = 0.1f;
    p.cameraFar = 1000.f;
    p.cameraFOV = XM_PIDIV2;
    p.cameraAspectRatio = float(width) / float(height);
    p.cameraRight[0] = p.cameraUp[1] = p.cameraFwd[2] = 1;
    auto projection = XMMatrixPerspectiveFovLH(p.cameraFOV, p.cameraAspectRatio, p.cameraNear, p.cameraFar);
    auto store = [](float (&matrix)[4][4], FXMMATRIX values) {
        XMFLOAT4X4 f;
        XMStoreFloat4x4(&f, values);
        memcpy(matrix, &f, sizeof(f));
    };
    store(p.cameraViewToClip, projection);
    store(p.clipToCameraView, XMMatrixInverse(nullptr, projection));
    store(p.clipToPrevClip, XMMatrixIdentity());
    store(p.prevClipToClip, XMMatrixIdentity());
    store(p.clipToLensClip, XMMatrixIdentity());
    // Our dense FidelityFX vectors are current-to-previous in full-size pixels.
    p.mvecScale[0] = -1.f / width;
    p.mvecScale[1] = -1.f / height;
    p.motionVectorsInvalidValue = FLT_MAX;
    p.cameraMotionIncluded = true;
    p.colorBuffersHDR = hdr;
    p.reset = reset;
    p.depthSubrectSize = p.mvecsSubrectSize = p.backbufferSubrectSize =
        p.outputInterpSubrectSize = {width, height};
    return p;
}
}

struct DlssFgBackend::Impl {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> alloc;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> results[3];
    ComPtr<ID3D12Resource> depth, disable, disableSeed, readback, pendingInput;
    ComPtr<ID3D12DescriptorHeap> depthRtv;
    ComputePasses passes;
    OpticalFlow flow;
    NVSDK_NGX_Handle* feature = nullptr;
    NVSDK_NGX_Parameter* params = nullptr;
    UINT width = 0, height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT64 completed = 0, timestampFrequency = 0, frame = 0;
    unsigned limit = 0, previousMultiplier = 0;
    bool acquired = false, resetHistory = true;
    char failure[160]{};

    ~Impl() {
        if (feature) NVSDK_NGX_D3D12_ReleaseFeature(feature);
        if (params) NVSDK_NGX_D3D12_DestroyParameters(params);
        if (acquired) NgxSession::Get().ReleaseFrameGeneration();
    }
    bool Fail(const char* what, UINT code = 0) {
        if (!failure[0]) {
            if (code) sprintf_s(failure, "%s (0x%08X)", what, code);
            else sprintf_s(failure, "%s", what);
            D5_LOG_WARN(L"DLSS FG: %hs", failure);
        }
        return false;
    }
    bool Resource(const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE type,
                  D3D12_RESOURCE_STATES state, ComPtr<ID3D12Resource>& resource) {
        D3D12_HEAP_PROPERTIES hp{};
        hp.Type = type;
        hp.CreationNodeMask = hp.VisibleNodeMask = 1;
        auto hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, state,
                                                  nullptr, IID_PPV_ARGS(&resource));
        return SUCCEEDED(hr) || Fail("resource allocation", hr);
    }
    bool Submit() {
        HRESULT hr = list->Close();
        if (FAILED(hr)) return Fail("close FG command list", hr);
        ID3D12CommandList* lists[]{list.Get()};
        queue->ExecuteCommandLists(1, lists);
        hr = queue->Signal(fence.Get(), ++completed);
        if (FAILED(hr)) return Fail("signal FG fence", hr);
        // A null event makes SetEventOnCompletion wait. The queue must finish
        // before inspecting the provider veto or recycling its resource bindings.
        hr = fence->SetEventOnCompletion(completed, nullptr);
        if (FAILED(hr) || fence->GetCompletedValue() == UINT64_MAX)
            return Fail("FG device removed or fence wait failed", device->GetDeviceRemovedReason());
        pendingInput.Reset();
        return true;
    }
    bool Begin() {
        HRESULT hr = alloc->Reset();
        if (SUCCEEDED(hr)) hr = list->Reset(alloc.Get(), nullptr);
        return SUCCEEDED(hr) || Fail("reset FG command list", hr);
    }
    bool Initialize(ID3D12CommandQueue* q, ID3D12Resource* color, HMODULE self) {
        queue = q;
        if (FAILED(q->GetDevice(IID_PPV_ARGS(&device)))) return Fail("queue has no device");
        auto desc = color->GetDesc();
        width = UINT(desc.Width); height = desc.Height; format = desc.Format;
        if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || !width || !height ||
            desc.SampleDesc.Count != 1 || desc.MipLevels != 1 || desc.DepthOrArraySize != 1 ||
            (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE))
            return Fail("unsupported FG input");
        D3D12_FEATURE_DATA_FORMAT_SUPPORT support{format};
        if (FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
            !(support.Support2 & D3D12_FORMAT_SUPPORT2_UAV_TYPED_STORE))
            return Fail("output format lacks typed UAV stores");
        if (!NgxSession::Get().AcquireFrameGeneration(device.Get(), self))
            return Fail("cannot own NGX session on this device");
        acquired = true;
        NVSDK_NGX_Parameter* caps = nullptr;
        auto ngx = NVSDK_NGX_D3D12_GetCapabilityParameters(&caps);
        if (NVSDK_NGX_FAILED(ngx) || !caps) return Fail("NGX capabilities", ngx);
        int available = 0, why = 0;
        unsigned long long wide = 0;
        caps->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &available);
        caps->Get(NVSDK_NGX_Parameter_FrameGeneration_Available, &wide);
        caps->Get(NVSDK_NGX_Parameter_FrameGeneration_FeatureInitResult, &why);
        unsigned long long count64 = 0;
        int count32 = 0;
        caps->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &count64);
        caps->Get(NVSDK_NGX_DLSSG_Parameter_MultiFrameCountMax, &count32);
        NVSDK_NGX_D3D12_DestroyParameters(caps);
        if (!available && !wide) return Fail("DLSS frame generation unavailable for this driver/device", why);
        // Older providers expose only the availability of standard 2x FG.
        const auto supported = count64 ? count64 : (count32 > 0 ? unsigned(count32) : 1u);
        limit = supported >= 3 ? 4 : unsigned(supported + 1);
        ngx = NVSDK_NGX_D3D12_AllocateParameters(&params);
        if (!params || NVSDK_NGX_FAILED(ngx)) return Fail("allocate FG parameters", ngx);
        if (!passes.Initialize(device.Get())) return Fail("optical flow compute initialization");
        if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc.Get(), nullptr, IID_PPV_ARGS(&list))) ||
            FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return Fail("create FG command context");
        q->GetTimestampFrequency(&timestampFrequency);
        desc.Format = DXGI_FORMAT_R32_FLOAT;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (!Resource(desc, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_RENDER_TARGET, depth) ||
            !Resource(BufferDesc(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS), D3D12_HEAP_TYPE_DEFAULT, Common, disable) ||
            !Resource(BufferDesc(D3D12_RESOURCE_FLAG_NONE), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, disableSeed) ||
            !Resource(BufferDesc(D3D12_RESOURCE_FLAG_NONE, 3 * sizeof(UINT)), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, readback))
            return false;
        void* raw = nullptr;
        D3D12_RANGE noRead{};
        if (FAILED(disableSeed->Map(0, &noRead, &raw))) return Fail("upload FG veto default");
        *static_cast<UINT*>(raw) = 1;
        disableSeed->Unmap(0, nullptr);
        D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1};
        if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&depthRtv)))) return Fail("fixed depth RTV");
        auto rtv = depthRtv->GetCPUDescriptorHandleForHeapStart();
        device->CreateRenderTargetView(depth.Get(), nullptr, rtv);
        const float flat[4]{.9f, .9f, .9f, .9f};
        list->ClearRenderTargetView(rtv, flat, 0, nullptr);
        Transition(list.Get(), depth.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, Read);
        params->Set(NVSDK_NGX_DLSSG_Parameter_ResourceNeverProvided_Flags, unsigned(
            NVSDK_NGX_DLSSG_ResourceFlags_UI | NVSDK_NGX_DLSSG_ResourceFlags_UIAlpha |
            NVSDK_NGX_DLSSG_ResourceFlags_HUDLess | NVSDK_NGX_DLSSG_ResourceFlags_BidirectionalDistortionField));
        params->Set(NVSDK_NGX_DLSSG_Parameter_ResourceAlwaysProvided_Flags, unsigned(
            NVSDK_NGX_DLSSG_ResourceFlags_Backbuffer | NVSDK_NGX_DLSSG_ResourceFlags_MVecs |
            NVSDK_NGX_DLSSG_ResourceFlags_Depth));
        NVSDK_NGX_DLSSG_Create_Params create{width, height, unsigned(format), width, height};
        ngx = NGX_D3D12_CREATE_DLSSG(list.Get(), 1, 1, &feature, params, &create);
        if (!Submit()) return false;
        if (NVSDK_NGX_FAILED(ngx) || !feature) return Fail("create DLSSG", ngx);
        D5_LOG_INFO(L"DLSS FG: feature ready (up to %ux) %ux%u format=%u", limit, width, height, unsigned(format));
        return true;
    }

    bool Evaluate(ID3D12Resource* color, ID3D12Resource** generated, bool hdr, unsigned multiplier) {
        if (multiplier > limit) {
            char reason[128];
            sprintf_s(reason, "DLSS FG requested %ux; current provider supports up to %ux", multiplier, limit);
            return Fail(reason);
        }
        const unsigned count = multiplier - 1;
        for (unsigned k = 0; k < count; ++k) {
            if (results[k]) continue;
            auto desc = color->GetDesc();
            desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (!Resource(desc, D3D12_HEAP_TYPE_DEFAULT, Common, results[k])) return false;
        }
        resetHistory |= previousMultiplier != multiplier;
        previousMultiplier = multiplier;
        if (!Begin()) return false;
        pendingInput = color;
        Transition(list.Get(), color, Common, Read);
        auto motion = flow.Record(device.Get(), list.Get(), passes, color, 1, resetHistory, true, timestampFrequency);
        auto camera = FixedCamera(width, height, resetHistory || flow.Status().reset, hdr);
        NVSDK_NGX_Result ngx = NVSDK_NGX_Result_Fail;
        if (motion) {
            // The provider may only write its veto on the first evaluation of a
            // source pair. Preserve the value across subframes and inspect each.
            Transition(list.Get(), disable.Get(), Common, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyBufferRegion(disable.Get(), 0, disableSeed.Get(), 0, sizeof(UINT));
            Transition(list.Get(), disable.Get(), D3D12_RESOURCE_STATE_COPY_DEST, Write);
            NVSDK_NGX_D3D12_DLSSG_Eval_Params inputs{};
            inputs.pBackbuffer = color;
            inputs.pDepth = depth.Get();
            inputs.pMVecs = motion;
            inputs.pOutputDisableInterpolation = disable.Get();
            params->Set(NVSDK_NGX_DLSSG_Parameter_BackbufferFrameID, ++frame);
            camera.multiFrameCount = count;
            for (unsigned k = 0; k < count; ++k) {
                auto* output = results[k].Get();
                Transition(list.Get(), output, Common, Write);
                inputs.pOutputInterpFrame = output;
                camera.multiFrameIndex = k + 1;
                ngx = NGX_D3D12_EVALUATE_DLSSG(list.Get(), feature, params, &inputs, &camera);
                Transition(list.Get(), output, Write, Common);
                Transition(list.Get(), disable.Get(), Write, D3D12_RESOURCE_STATE_COPY_SOURCE);
                list->CopyBufferRegion(readback.Get(), k * sizeof(UINT), disable.Get(), 0, sizeof(UINT));
                if (NVSDK_NGX_FAILED(ngx) || k + 1 == count) break;
                Transition(list.Get(), disable.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, Write);
            }
        }
        Transition(list.Get(), color, Read, Common);
        if (!Submit()) return false;
        if (!motion) return Fail("FG optical flow unavailable");
        if (NVSDK_NGX_FAILED(ngx)) return Fail("evaluate DLSSG", ngx);
        void* raw = nullptr;
        D3D12_RANGE range{0, count * sizeof(UINT)}, noWrite{};
        if (FAILED(readback->Map(0, &range, &raw))) return Fail("read FG veto");
        bool allowed = true;
        for (unsigned k = 0; k < count; ++k)
            allowed &= static_cast<const BYTE*>(raw)[k * sizeof(UINT)] == 0;
        readback->Unmap(0, &noWrite);
        resetHistory = false;
        if (camera.reset || !allowed) return false;
        for (unsigned k = 0; k < count; ++k) generated[k] = results[k].Get();
        return true;
    }
};

DlssFgBackend::DlssFgBackend() = default;
DlssFgBackend::~DlssFgBackend() = default;
void DlssFgBackend::Reset() noexcept { session_.reset(); }
void DlssFgBackend::ResetHistory() noexcept { if (session_) session_->resetHistory = true; }
unsigned DlssFgBackend::MaxMultiplier() const noexcept { return session_ ? session_->limit : 0; }
const char* DlssFgBackend::Error() const noexcept { return session_ && session_->failure[0] ? session_->failure : nullptr; }

bool DlssFgBackend::Generate(ID3D12CommandQueue* q, ID3D12Resource* color, HMODULE self, ID3D12Resource** generated, bool hdr10, unsigned multiplier) {
    if (!generated || multiplier < 2 || multiplier > 4) return false;
    for (unsigned k = 0; k < multiplier - 1; ++k) generated[k] = nullptr;
    if (!q || !color || q->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
    if (!session_) {
        session_ = std::make_unique<Impl>();
        if (!session_->Initialize(q, color, self)) return false;
    }
    if (Error()) return false;
    const auto d = color->GetDesc();
    if (session_->queue.Get() != q || d.Width != session_->width || d.Height != session_->height ||
        d.Format != session_->format) return session_->Fail("FG target changed; reset required");
    return session_->Evaluate(color, generated, hdr10, multiplier);
}
}
