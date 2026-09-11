#pragma once
#include <d3d12.h>
#include <mutex>
#include <cmath>
#include <cstring>
#include "NrRoutePolicy.h"

namespace DXL {
// Nine 8x8 tiles, copied before NR modifies the native SR output. Each readback
// has its own actual-submission fence. Sampling never waits or borrows guides.
class NrRouteProbe {
public:
    struct Status { bool nativeRecent, dormant, present; uint64_t samples, dropped; bool handoffBlocked; };
    static NrRouteProbe& Get() { static auto* p = new NrRouteProbe; return *p; }
    void Enable(bool enabled) { std::lock_guard<std::mutex> lock(_mutex); _enabled = enabled; }
    void SetSubmissionHookReady(bool ready) { std::lock_guard<std::mutex> lock(_mutex); _ready = ready; }
    void Observe(ID3D12GraphicsCommandList* list, ID3D12Resource* output,
        const void* feature, uint64_t now, bool canRecord) {
        std::lock_guard<std::mutex> lock(_mutex);
        _policy.Native(now, reinterpret_cast<uintptr_t>(feature));
        Poll();
        if (!_enabled || !_ready || !canRecord || !list || !output || now - _lastRecord < 50) return;
        const auto desc = output->GetDesc();
        const UINT bpp = Bytes(desc.Format);
        if (!bpp || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            desc.SampleDesc.Count != 1 || desc.DepthOrArraySize != 1 || desc.Width < 8 || desc.Height < 8) return;
        Slot* s = nullptr;
        for (auto& candidate : _slots) if (!candidate.list && !candidate.failed) { s = &candidate; break; }
        if (!s) { ++_dropped; return; }
        if (!s->readback) {
            ID3D12Device* device = nullptr;
            if (FAILED(list->GetDevice(IID_PPV_ARGS(&device)))) return;
            D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;
            D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            bd.Width = BUFFER_SIZE; bd.Height = 1; bd.DepthOrArraySize = bd.MipLevels = 1;
            bd.SampleDesc.Count = 1; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            const auto hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&s->readback));
            if (SUCCEEDED(hr)) device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&s->fence));
            device->Release();
            if (!s->readback || !s->fence) { s->failed = true; return; }
            s->readback->SetName(L"NRFG.SRActivity.Readback");
        }
        s->list = list; s->submitted = false; s->at = now; s->epoch = _policy.Epoch();
        s->format = desc.Format; s->serial = ++_serial; _lastRecord = now;
        D3D12_RESOURCE_BARRIER b{}; b.Transition.pResource = output;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        list->ResourceBarrier(1, &b);
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = output;
        for (UINT i = 0; i < 9; ++i) {
            D3D12_TEXTURE_COPY_LOCATION dst{}; dst.pResource = s->readback;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Offset = i * TILE_BYTES;
            dst.PlacedFootprint.Footprint = {desc.Format, 8, 8, 1, 256};
            const UINT x = UINT((desc.Width - 8) * (1 + (i % 3) * 2) / 6);
            const UINT y = (desc.Height - 8) * (1 + (i / 3) * 2) / 6;
            const D3D12_BOX box{x, y, 0, x + 8, y + 8, 1};
            list->CopyTextureRegion(&dst, 0, 0, 0, &src, &box);
        }
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        list->ResourceBarrier(1, &b);
    }
    void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
        std::lock_guard<std::mutex> lock(_mutex);
        for (auto& s : _slots) if (s.list && !s.submitted && !s.failed) {
            for (UINT i = 0; i < count; ++i) if (lists[i] == s.list) {
                s.submitted = true; s.failed = FAILED(queue->Signal(s.fence, ++s.value)); break;
            }
        }
    }
    void Reset(ID3D12GraphicsCommandList* list) {
        std::lock_guard<std::mutex> lock(_mutex);
        for (auto& s : _slots) if (s.list == list && !s.submitted) s.list = nullptr;
    }
    bool UseEvaluate(uint64_t now, bool fg) {
        std::lock_guard<std::mutex> lock(_mutex); Poll();
        return !_enabled || _policy.UseEvaluate(now, fg);
    }
    Status Snapshot(uint64_t now, bool fg, bool uncertainFgChain = false) {
        std::lock_guard<std::mutex> lock(_mutex); Poll();
        return {_policy.NativeRecent(now), _policy.Dormant(now),
            _enabled && _policy.UsePresent(now, fg, uncertainFgChain), _samples, _dropped,
            _enabled && _policy.UsePresent(now, fg) && _policy.AutomaticHandoffBlocked(uncertainFgChain)};
    }
    void Applied(uint64_t now) { std::lock_guard<std::mutex> lock(_mutex); _policy.Applied(now); }
private:
    static constexpr UINT TILE_BYTES = 256 * 8, BUFFER_SIZE = TILE_BYTES * 9;
    struct Slot {
        ID3D12Resource* readback = nullptr;
        ID3D12Fence* fence = nullptr;
        ID3D12CommandList* list = nullptr;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        UINT64 value = 0, at = 0, epoch = 0, serial = 0;
        bool submitted = false, failed = false;
    };
    static UINT Bytes(DXGI_FORMAT f) {
        switch (f) {
        case DXGI_FORMAT_R11G11B10_FLOAT: case DXGI_FORMAT_R10G10B10A2_UNORM:
        // The raw RGBA8/BGRA8 bytes are identical across typeless and UNORM
        // views. Keep the original format in the copy footprint; alpha is
        // still ignored by Black(), and any nonzero RGB remains valid SR.
        case DXGI_FORMAT_R8G8B8A8_TYPELESS: case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_R8G8B8A8_UNORM: case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM: case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return 4;
        case DXGI_FORMAT_R16G16B16A16_FLOAT: return 8;
        case DXGI_FORMAT_R32G32B32A32_FLOAT: return 16;
        default: return 0;
        }
    }
    static bool Black(const unsigned char* p, DXGI_FORMAT f) {
        // Require exact zero RGB (ignore alpha). Near-black imagery is valid SR
        // content and must never be classified as an inactive menu pass.
        if (f == DXGI_FORMAT_R16G16B16A16_FLOAT) {
            uint16_t v[4]; std::memcpy(v, p, 8);
            return ((v[0] | v[1] | v[2]) & 0x7fff) == 0;
        }
        if (f == DXGI_FORMAT_R32G32B32A32_FLOAT) {
            float v[4]; std::memcpy(v, p, 16); return v[0] == 0 && v[1] == 0 && v[2] == 0;
        }
        uint32_t v; std::memcpy(&v, p, 4);
        if (f == DXGI_FORMAT_R11G11B10_FLOAT) return v == 0;
        if (f == DXGI_FORMAT_R10G10B10A2_UNORM) return (v & 0x3fffffff) == 0;
        return (v & 0xffffff) == 0;
    }
    void Poll() {
        // Process completions by recording serial, not descriptor-slot index.
        for (;;) {
            Slot* next = nullptr;
            for (auto& s : _slots) if (s.list && s.submitted && !s.failed) {
                const auto done = s.fence->GetCompletedValue();
                if (done == UINT64_MAX) { s.failed = true; continue; }
                if (done >= s.value && (!next || s.serial < next->serial)) next = &s;
            }
            if (!next) return;
            if (next->serial > _lastSampleSerial) {
                void* mapped = nullptr; D3D12_RANGE range{0, BUFFER_SIZE};
                if (SUCCEEDED(next->readback->Map(0, &range, &mapped))) {
                    const auto* data = static_cast<const unsigned char*>(mapped);
                    bool empty = true; const auto bpp = Bytes(next->format);
                    for (UINT t = 0; empty && t < 9; ++t)
                        for (UINT y = 0; empty && y < 8; ++y)
                            for (UINT x = 0; empty && x < 8; ++x)
                                empty = Black(data + t * TILE_BYTES + y * 256 + x * bpp, next->format);
                    D3D12_RANGE written{0, 0}; next->readback->Unmap(0, &written);
                    _policy.Sample(next->at, next->epoch, empty); ++_samples;
                    _lastSampleSerial = next->serial;
                }
            }
            next->list = nullptr;
        }
    }
    std::mutex _mutex;
    NrRoutePolicy _policy;
    Slot _slots[3]{}; // process lifetime, including unsubmitted/device-lost work
    bool _enabled = false, _ready = false;
    uint64_t _lastRecord = 0, _serial = 0, _lastSampleSerial = 0, _samples = 0, _dropped = 0;
};
}
