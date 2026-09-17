#pragma once
#include "DlssNrFilter.h"
#include <wrl/client.h>

namespace DXL {
enum class ForcedNrBlock { Off, WaitingOutput, Busy, Paused, NotReady, Failed, Running };

inline ForcedNrBlock ForcedNrAdmission(bool enabled, bool idle, bool paused,
                                       bool nativeOutput, bool queueReady) noexcept {
    if (!enabled) return ForcedNrBlock::Off;
    if (!idle) return ForcedNrBlock::Busy;
    if (paused) return ForcedNrBlock::Paused;
    if (!nativeOutput || !queueReady) return ForcedNrBlock::WaitingOutput;
    return ForcedNrBlock::Running;
}

inline NrSettings ForcedNrSettings(const NrSettings& source, bool reset) {
    auto result = source;
    result.useRealDepth = false;
    result.useRealMotion = false;
    result.forceReset = reset || !result.opticalFlow;
    result.grading = source.grading.BasicOnly();
    return result;
}

// Borrows the existing NR feature only after external Evaluate work retires.
// Keep a buffer reference on a timeout; a failed wait never permits its reuse.
class ForcedNrOutput12 {
public:
    bool Execute(DlssNrFilter& filter, ID3D12Resource* image, ID3D12CommandQueue* queue,
                 const NrSettings& settings, bool reset) noexcept {
        if (!Retire(filter) || !image || !queue || _syncFailed) return false;
        if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
        Microsoft::WRL::ComPtr<ID3D12Device> imageDevice, queueDevice;
        if (FAILED(image->GetDevice(IID_PPV_ARGS(&imageDevice))) ||
            FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) || imageDevice.Get() != queueDevice.Get()) return false;
        if (!filter.SetPresentQueue(queue)) return false;
        filter.SetExternalMotion(nullptr, 0, 0, 0, 0);
        filter.SetExternalDepth(nullptr, 0, 0);
        const auto desc = image->GetDesc();
        if (!filter.Prepare(UINT(desc.Width), desc.Height, desc.Format,
                            ForcedNrSettings(settings, reset), NrMode::Present, true)) return false;
        _pendingImage = image;
        const bool drew = filter.Execute({image, D3D12_RESOURCE_STATE_PRESENT},
                                         {image, D3D12_RESOURCE_STATE_PRESENT});
        if (!Retire(filter)) return false;
        return drew;
    }
    bool Retire(DlssNrFilter& filter) noexcept {
        if (!filter.WaitForOwnGpuIdle()) { _syncFailed = true; return false; }
        _pendingImage.Reset();
        return true;
    }
    bool SyncFailed() const noexcept { return _syncFailed; }
private:
    Microsoft::WRL::ComPtr<ID3D12Resource> _pendingImage;
    bool _syncFailed = false;
};
}
