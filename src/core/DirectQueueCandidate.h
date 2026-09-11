#pragma once
#include <d3d12.h>
#include <mutex>
#include <utility>

namespace DXL {
// A retained candidate for initializing work on the selected D3D12 device.
// This is not proof that the queue presents any particular swapchain.
class DirectQueueCandidate {
public:
    DirectQueueCandidate() = default;
    DirectQueueCandidate(const DirectQueueCandidate&) = delete;
    DirectQueueCandidate& operator=(const DirectQueueCandidate&) = delete;
    ~DirectQueueCandidate() { Clear(); }

    bool SetDevice(ID3D12Device* device) noexcept {
        if (!device) return false;
        std::unique_lock lock(_mutex);
        if (_closed) return false;
        if (_device == device) return true;
        IUnknown* identity = nullptr;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&identity))) || !identity) return false;
        if (!_identity) {
            _device = device;
            _identity = identity;
            return true;
        }
        const bool same = _identity == identity;
        lock.unlock();
        identity->Release();
        return same;
    }

    bool Observe(ID3D12CommandQueue* queue) noexcept {
        if (!queue) return false;
        std::unique_lock lock(_mutex);
        if (_closed || !_identity) return false;
        // Most submissions reuse the same queue. Its retained reference makes
        // this fast path safe without repeated GetDevice/AddRef calls.
        if (_queue == queue) return true;
        if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
        IUnknown* owner = nullptr;
        const HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&owner));
        const bool accepted = SUCCEEDED(hr) && owner && owner == _identity;
        ID3D12CommandQueue* previous = nullptr;
        if (accepted) {
            queue->AddRef();
            previous = std::exchange(_queue, queue);
        }
        lock.unlock();
        if (owner) owner->Release();
        if (previous) previous->Release();
        return accepted;
    }

    // Caller owns the returned reference, including across a concurrent Clear.
    ID3D12CommandQueue* Acquire() noexcept {
        std::lock_guard lock(_mutex);
        if (_queue) _queue->AddRef();
        return _queue;
    }

    // Teardown is final. Racing submissions cannot repopulate the candidate.
    void Clear() noexcept {
        ID3D12CommandQueue* queue = nullptr;
        IUnknown* identity = nullptr;
        {
            std::lock_guard lock(_mutex);
            _closed = true;
            queue = std::exchange(_queue, nullptr);
            identity = std::exchange(_identity, nullptr);
            _device = nullptr;
        }
        if (queue) queue->Release();
        if (identity) identity->Release();
    }

private:
    std::mutex _mutex;
    ID3D12Device* _device = nullptr; // Lifetime held by _identity.
    IUnknown* _identity = nullptr;
    ID3D12CommandQueue* _queue = nullptr;
    bool _closed = false;
};
} // namespace DXL
