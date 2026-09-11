#pragma once

#include <d3d12.h>
#include <mutex>

namespace DXL {

// The NR feature and its scratch textures are singletons. A CPU recording lock
// cannot protect them from work already recorded but not yet submitted/completed.
// Keep one external command list in flight, and signal on its ACTUAL queue.
// Once submitted, wait for that list instead of alternating filtered/unfiltered
// frames. Never wait for an unsubmitted list: its submission may depend on this
// recording worker returning. A bounded wait also breaks queue dependency cycles.
class EvaluateGpuGate {
public:
    static constexpr DWORD WAIT_BUDGET_MS = 100;
    struct Statistics {
        UINT64 admitted = 0;
        UINT64 waits = 0, waitMs = 0, maxWaitMs = 0;
        UINT64 unsubmitted = 0, timeouts = 0, unavailable = 0, errors = 0;
    };
    static EvaluateGpuGate& Get() { static auto* gate = new EvaluateGpuGate; return *gate; }
    bool Initialize(ID3D12Device* device) {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_event) _event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        return _event && (_fence || (device && SUCCEEDED(device->CreateFence(0,
            D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&_fence)))));
    }
    void SetSubmissionHookReady(bool ready) {
        std::lock_guard<std::mutex> lock(_mutex);
        _ready = ready;
    }
    bool Begin(ID3D12GraphicsCommandList* list) {
        // One event/one reservation; submission and Reset must remain able to
        // enter while this CPU thread sleeps on the GPU fence.
        std::lock_guard<std::mutex> beginLock(_beginMutex);
        std::unique_lock<std::mutex> lock(_mutex);
        if (_closing || !_ready || !_fence || !_event || !list || _failed) {
            ++_stats.unavailable; return false;
        }
        if (_pending) {
            if (!_submitted) { ++_stats.unsubmitted; return false; }
            auto completed = _fence->GetCompletedValue();
            if (completed == UINT64_MAX) { _failed = true; ++_stats.errors; return false; }
            if (completed < _value) {
                if (!_eventArmed) {
                    ResetEvent(_event);
                    if (FAILED(_fence->SetEventOnCompletion(_value, _event))) {
                        _failed = true; ++_stats.errors; return false;
                    }
                    _eventArmed = true;
                }
                ++_stats.waits;
                const auto start = GetTickCount64();
                DWORD result = WAIT_OBJECT_0;
                do {
                    const auto elapsed = GetTickCount64() - start;
                    if (elapsed >= WAIT_BUDGET_MS) break;
                    lock.unlock();
                    result = WaitForSingleObject(_event, DWORD(WAIT_BUDGET_MS - elapsed));
                    lock.lock();
                    completed = _fence->GetCompletedValue();
                    // An earlier completion notification can arrive late. Do
                    // not mistake its wake-up for completion of this value.
                } while (result == WAIT_OBJECT_0 && completed < _value);
                const auto elapsed = GetTickCount64() - start;
                _stats.waitMs += elapsed;
                if (elapsed > _stats.maxWaitMs) _stats.maxWaitMs = elapsed;
                completed = _fence->GetCompletedValue();
                if (completed == UINT64_MAX || result == WAIT_FAILED) {
                    _failed = true; ++_stats.errors; return false;
                }
                // Fence completion, not the event's return value, grants reuse.
                // On timeout retain the reservation and the event registration.
                if (completed < _value) { ++_stats.timeouts; return false; }
            }
            _pending = nullptr;
            _eventArmed = false;
        }
        _pending = list;
        _submitted = false;
        ++_stats.admitted;
        return true;
    }
    Statistics Snapshot() {
        std::lock_guard<std::mutex> lock(_mutex);
        return _stats;
    }
    // Present may switch/rebuild the single filter only when all external NR
    // work has completed. Do not reserve a game list or stall the Present hook.
    bool IsIdle() {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_failed) return false;
        if (!_pending) return true;
        if (!_submitted || !_fence) return false;
        const auto done = _fence->GetCompletedValue();
        return done != UINT64_MAX && done >= _value;
    }
    // Stop new admissions and retire only work already submitted to its actual
    // queue. An unsubmitted recording can depend on this thread returning; never
    // wait for it. False means owners must retain every referenced resource until
    // process exit, including against automatic/static-destructor cleanup.
    bool DrainForExit(DWORD budgetMs = WAIT_BUDGET_MS) {
        std::lock_guard<std::mutex> beginLock(_beginMutex);
        std::unique_lock<std::mutex> lock(_mutex);
        _closing = true;
        if (!_pending) return true;
        const auto removed = [&]() {
            if (!_fence) return false;
            if (_fence->GetCompletedValue() == UINT64_MAX) return true;
            ID3D12Device* device = nullptr;
            if (FAILED(_fence->GetDevice(IID_PPV_ARGS(&device))) || !device) return false;
            const bool gone = FAILED(device->GetDeviceRemovedReason());
            device->Release();
            return gone;
        };
        if (removed()) return true;
        if (!_submitted || !_fence || !_event || _failed) return false;
        const auto start = GetTickCount64();
        for (;;) {
            const UINT64 done = _fence->GetCompletedValue();
            if (done == UINT64_MAX || done >= _value) {
                _pending = nullptr;
                return true;
            }
            const auto elapsed = GetTickCount64() - start;
            if (elapsed >= budgetMs) return removed();
            if (!_eventArmed) {
                ResetEvent(_event);
                if (FAILED(_fence->SetEventOnCompletion(_value, _event))) return removed();
                _eventArmed = true;
            }
            lock.unlock();
            const DWORD result = WaitForSingleObject(_event, DWORD(budgetMs - elapsed));
            lock.lock();
            if (result == WAIT_FAILED) return removed();
            // As in Begin(), a stale notification cannot authorize destruction.
        }
    }
    void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* lists) {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_pending || _submitted || !_fence) return;
        for (UINT i = 0; i < count; ++i) {
            if (lists[i] != _pending) continue;
            _submitted = true;
            _failed = FAILED(queue->Signal(_fence, ++_value));
            if (_failed) ++_stats.errors;
            break;
        }
    }
    void Reset(ID3D12GraphicsCommandList* list) {
        std::lock_guard<std::mutex> lock(_mutex);
        // Reset can discard a recorded list. Submitted work remains protected
        // until its fence completes even if the CPU resets the list early.
        if (_pending == list && !_submitted) _pending = nullptr;
    }
private:
    std::mutex _beginMutex;
    std::mutex _mutex;
    ID3D12Fence* _fence = nullptr; // process lifetime, including device-lost teardown
    HANDLE _event = nullptr; // outlives any outstanding SetEventOnCompletion
    ID3D12CommandList* _pending = nullptr;
    UINT64 _value = 0;
    Statistics _stats{};
    bool _eventArmed = false;
    bool _ready = false, _submitted = false, _failed = false, _closing = false;
};
} // namespace DXL
