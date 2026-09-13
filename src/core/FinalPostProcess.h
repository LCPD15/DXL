#pragma once
#include "DlssNrFilter11.h"
#include <wrl/client.h>

namespace DXL {
// Scene adjustments stay with the NR/native SR route. Output effects never
// enter that image, so game tone mapping, upscaling and FG cannot filter them.
inline NrSettings SceneColorSettings(const NrSettings& source) {
    auto result = source;
    result.grading = source.grading.BasicOnly();
    return result;
}

inline NrSettings FinalColorSettings(const ColorGradingSettings& source) {
    NrSettings result;
    result.enabled = false;
    result.grading = source.FinalOnly();
    return result;
}

// The caller supplies an independently verified native display buffer and its
// presentation/writer queue. This object deliberately owns no NR model, route
// state or Evaluate command list, and never changes the NR FG admission guard.
class FinalPostProcess12 {
public:
    bool Execute(ID3D12Resource* image, D3D12_RESOURCE_STATES state,
                 ID3D12CommandQueue* queue, HMODULE module,
                 const ColorGradingSettings& grading) noexcept {
        if (!WaitIdle() || !image || !queue || !grading.AnyFinalActive()) return false;
        if (_syncFailed || queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
        Microsoft::WRL::ComPtr<ID3D12Device> imageDevice, queueDevice;
        if (FAILED(image->GetDevice(IID_PPV_ARGS(&imageDevice))) ||
            FAILED(queue->GetDevice(IID_PPV_ARGS(&queueDevice))) || imageDevice.Get() != queueDevice.Get()) return false;
        if (!_device) {
            _device = imageDevice;
            _initialized = _filter.Initialize(_device.Get(), queue, module);
        }
        if (!_initialized || imageDevice.Get() != _device.Get() || !_filter.SetPresentQueue(queue)) return false;
        const auto desc = image->GetDesc();
        if (!_filter.Prepare(UINT(desc.Width), desc.Height, desc.Format,
                FinalColorSettings(grading), NrMode::Present)) return false;
        // Keep the borrowed DXGI buffer alive on the exceptional timeout path.
        // Successful frames drop this reference before returning to Present.
        _pendingImage = image;
        const bool drew = _filter.Execute({image, state}, {image, state});
        if (!_filter.WaitForOwnGpuIdle()) {
            _syncFailed = true;
            return false;
        }
        _pendingImage.Reset();
        return drew;
    }
    const DlssNrFilter& Filter() const noexcept { return _filter; }
    bool SyncFailed() const noexcept { return _syncFailed; }
    bool WaitIdle() noexcept {
        if (!_filter.WaitForOwnGpuIdle()) return false;
        _pendingImage.Reset();
        return true;
    }
    bool TeardownForExit() noexcept {
        if (!_filter.TeardownForExit()) return false;
        _pendingImage.Reset();
        return true;
    }
private:
    Microsoft::WRL::ComPtr<ID3D12Device> _device;
    Microsoft::WRL::ComPtr<ID3D12Resource> _pendingImage;
    DlssNrFilter _filter;
    bool _initialized = false, _syncFailed = false;
};

class FinalPostProcess11 {
public:
    bool Execute(ID3D11Device* device, ID3D11Texture2D* image, HMODULE module,
                 const ColorGradingSettings& grading) noexcept {
        if (!device || !image || !grading.AnyFinalActive()) return false;
        Microsoft::WRL::ComPtr<ID3D11Device> imageDevice;
        image->GetDevice(&imageDevice);
        if (imageDevice.Get() != device) return false;
        if (!_device) {
            _device = device;
            _initialized = _filter.Initialize(device, module);
        }
        if (!_initialized || _device.Get() != device) return false;
        return _filter.Execute(image, FinalColorSettings(grading));
    }
    const DlssNrFilter& Filter() const noexcept { return _filter.Filter12ForStatus(); }
    bool TeardownForExit() noexcept { return _filter.TeardownForExit(); }
private:
    Microsoft::WRL::ComPtr<ID3D11Device> _device;
    DlssNrFilter11 _filter;
    bool _initialized = false;
};
}
