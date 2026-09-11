#pragma once
#include <dxgi1_4.h>
#include <atomic>
#include <mutex>
#include <new>
#include "../common/Log.h"
#include "FgDetectionSettings.h"

namespace DXL {
// Buffer count + an FG module remains a conservative heuristic. Keep the
// count for EACH live chain; creation of an unrelated 3-buffer chain is not
// evidence that a still-live 4-buffer FG chain was destroyed.
class FrameGenSwapChains {
    struct Entry {
        Entry* next = nullptr;
        UINT buffers = 0;
        const void* chain = nullptr; // diagnostic value only; NEVER AddRef
    };
    std::mutex mutex_;
    Entry* entries_ = nullptr;
    UINT threshold_ = 4;
    std::atomic<UINT> candidates_{0};
    std::atomic<bool> trackingFailed_{false};
    std::atomic<bool> loggingAlive_{true};
    void Publish() noexcept {
        UINT count = 0;
        for (auto* e = entries_; e; e = e->next) if (e->buffers >= threshold_) ++count;
        candidates_.store(count, std::memory_order_release);
    }
    void Add(Entry& e, UINT buffers, const void* chain) noexcept {
        std::lock_guard lock(mutex_);
        e.buffers = buffers; e.chain = chain; e.next = entries_; entries_ = &e;
        Publish();
        if (chain && loggingAlive_.load()) D5_LOG_INFO(L"FG chain track: chain=%p buffers=%u candidates=%u", chain, buffers, candidates_.load());
    }
    void Update(Entry& e, UINT buffers, bool resized) noexcept {
        std::lock_guard lock(mutex_);
        // ResizeBuffers(0,...) retains the existing count.
        if (!buffers || buffers == e.buffers) return;
        // Some wrappers forward their private-data store to a native inner
        // chain. Observing that inner chain's smaller factory descriptor must
        // not lower an already observed FG count. Only a successful resize can.
        if (!resized && buffers < e.buffers) return;
        const UINT before = e.buffers;
        e.buffers = buffers; Publish();
        D5_LOG_INFO(L"FG chain resize: chain=%p buffers=%u->%u candidates=%u", e.chain, before, buffers, candidates_.load());
    }
    void Remove(Entry& e) noexcept {
        // Process detach can kill a thread that owned a lock. Once CRT teardown
        // begins, no render work is allowed and the registry need not be edited.
        if (!loggingAlive_.load(std::memory_order_acquire)) return;
        std::lock_guard lock(mutex_);
        auto** link = &entries_;
        while (*link && *link != &e) link = &(*link)->next;
        if (*link) *link = e.next;
        Publish();
        if (e.chain && loggingAlive_.load()) D5_LOG_INFO(L"FG chain release: chain=%p buffers=%u candidates=%u", e.chain, e.buffers, candidates_.load());
    }
    // DXGI owns this cookie through SetPrivateDataInterface and releases it on
    // chain destruction. It owns no chain/queue/backbuffer, so cannot keep an
    // old chain alive or introduce a Release-vtable hook into an FG wrapper.
    class Cookie final : public IUnknown {
        std::atomic<ULONG> refs_{1};
        Entry entry_;
    public:
        Cookie(IDXGIObject* chain, UINT buffers) { Get().Add(entry_, buffers, chain); }
        ~Cookie() { Get().Remove(entry_); }
        void Update(UINT buffers, bool resized) { Get().Update(entry_, buffers, resized); }
        UINT Buffers() const {
            std::lock_guard lock(Get().mutex_);
            return entry_.buffers;
        }
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
            if (!out) return E_POINTER;
            *out = nullptr;
            if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
            *out = static_cast<IUnknown*>(this); AddRef(); return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
        ULONG STDMETHODCALLTYPE Release() override {
            const auto left = --refs_;
            if (!left) delete this;
            return left;
        }
    };
    inline static constexpr GUID cookieId_{0xd1f97c70,0x36d5,0x4f62,{0xa8,0x5b,0x2e,0x78,0x5d,0xc0,0x59,0x01}};
    std::mutex attachMutex_;
public:
    static FrameGenSwapChains& Get() noexcept {
        // Cookies may be released during DXGI's process shutdown, after normal
        // C++ statics. The tiny registry therefore has process lifetime.
        static auto* state = new FrameGenSwapChains;
        // Stop using the CRT logger before its destructor runs. DXGI may
        // release cookies later in another DLL's process-detach callback.
        struct LogLifetime {
            FrameGenSwapChains* state;
            ~LogLifetime() { state->loggingAlive_.store(false); }
        };
        static LogLifetime lifetime{[] { Log::Get(); return state; }()};
        return *state;
    }
    bool HasCandidate() const noexcept { return candidates_.load(std::memory_order_acquire) != 0 || trackingFailed_.load(); }
    UINT CandidateCount() const noexcept { return candidates_.load(std::memory_order_acquire); }
    // Automatic SR->Present handoff needs a stronger condition than an advanced
    // threshold override. Six buffers below threshold eight do not prove FG is
    // off. Query the CURRENT chain's cookie so a replacement low-buffer chain
    // can recover without waiting for unrelated/old chains to be destroyed.
    // The caller also requires a known FG module and an observed native SR run;
    // this is uncertainty about ownership, not detection of enabled FG.
    bool AutomaticHandoffUncertain(IDXGIObject* chain, UINT* observedBuffers = nullptr) noexcept {
        UINT buffers = 0;
        Cookie* cookie = nullptr; UINT bytes = sizeof(cookie);
        if (chain && SUCCEEDED(chain->GetPrivateData(cookieId_, &bytes, &cookie)) && cookie) {
            buffers = cookie->Buffers(); cookie->Release();
        }
        if (observedBuffers) *observedBuffers = buffers;
        return buffers == 0 || buffers >= 4;
    }
    void SetThreshold(UINT threshold) noexcept {
        std::lock_guard lock(mutex_); threshold_ = threshold; Publish();
    }
    class Creation {
        Entry entry_;
    public:
        explicit Creation(UINT buffers) { Get().Add(entry_, buffers, nullptr); }
        ~Creation() { Get().Remove(entry_); }
        Creation(const Creation&) = delete;
        Creation& operator=(const Creation&) = delete;
    };
    bool Observe(IDXGIObject* chain, UINT buffers, bool resized = false) noexcept {
        if (!chain) return false;
        // Serialize duplicate observations of wrapper interfaces that share
        // the underlying private-data store. No registry lock spans COM calls.
        std::lock_guard lock(attachMutex_);
        Cookie* cookie = nullptr;
        UINT bytes = sizeof(cookie);
        if (SUCCEEDED(chain->GetPrivateData(cookieId_, &bytes, &cookie)) && cookie) {
            cookie->Update(buffers, resized); cookie->Release(); return true;
        }
        // No valid count yet: leave registration to the factory/first Present.
        if (!buffers) return false;
        auto* created = new (std::nothrow) Cookie(chain, buffers);
        if (!created) { trackingFailed_.store(true); return false; }
        // The cookie's COM vtable must outlive all DXGI objects, including when
        // hook teardown precedes their destruction. Pin code, not game objects.
        HMODULE pinned = nullptr;
        const bool codeAlive = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&Get), &pinned) != FALSE;
        const HRESULT hr = codeAlive ? chain->SetPrivateDataInterface(cookieId_, created) : E_FAIL;
        created->Release();
        if (FAILED(hr)) {
            // An opaque wrapper without DXGI private-data support cannot be
            // tracked safely. Keep Present guarded when an FG module exists,
            // until restart; never silently resume NR on an untracked FG chain.
            trackingFailed_.store(true);
            D5_LOG_WARN(L"FG chain tracking unavailable: chain=%p buffers=%u hr=0x%08X; conservative FG Present guard until restart", chain, buffers, hr);
        }
        return SUCCEEDED(hr);
    }
    bool Contains(IDXGIObject* chain) noexcept {
        IUnknown* cookie = nullptr; UINT bytes = sizeof(cookie);
        if (FAILED(chain->GetPrivateData(cookieId_, &bytes, &cookie)) || !cookie) return false;
        cookie->Release(); return true;
    }
};
} // namespace DXL
