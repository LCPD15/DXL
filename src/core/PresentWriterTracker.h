#pragma once
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <atomic>

namespace DXL {
// Late injection cannot recover the DXGI creation queue from GetDevice.
// Instead, observe the actual current backbuffer's submitted PRESENT barrier.
// This proves its last writer, not its DXGI/FG presentation queue. The caller
// must fence UI completion before returning to Present. Unknown FG proxies
// remain excluded; native DXGI resource access can be independently verified.
class PresentWriterTracker {
    using Queue = Microsoft::WRL::ComPtr<ID3D12CommandQueue>;
    struct Watch {
        Microsoft::WRL::ComPtr<IUnknown> device;
        Queue writer;
        uint64_t submitted = 0, consumed = 0;
    };
    struct Cookie final : IUnknown {
        std::atomic<ULONG> refs{1};
        std::shared_ptr<Watch> watch;
        explicit Cookie(std::shared_ptr<Watch> value) : watch(std::move(value)) {}
        HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
            if (!out) return E_POINTER;
            *out = nullptr;
            if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
            *out = static_cast<IUnknown*>(this); AddRef(); return S_OK;
        }
        ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
        ULONG STDMETHODCALLTYPE Release() override {
            const auto n = --refs; if (!n) delete this; return n;
        }
    };
    struct EndState { std::weak_ptr<Watch> watch; bool present = false; };
    std::mutex mutex;
    std::atomic<bool> enabled{false};
    std::unordered_map<ID3D12Resource*, std::weak_ptr<Watch>> resources;
    std::unordered_map<ID3D12CommandList*, std::vector<EndState>> lists;
    uint64_t serial = 0;
    inline static thread_local unsigned ignoreDepth = 0;
    inline static constexpr GUID key{0xa0acb509,0x8359,0x41d5,{0x98,0x61,0xae,0xc4,0xb2,0xb2,0xb9,0x62}};
public:
    class IgnoreScope {
    public:
        IgnoreScope() noexcept { ++ignoreDepth; }
        ~IgnoreScope() { --ignoreDepth; }
        IgnoreScope(const IgnoreScope&) = delete;
        IgnoreScope& operator=(const IgnoreScope&) = delete;
    };
    static PresentWriterTracker& Get() {
        static auto* instance = new PresentWriterTracker;
        return *instance;
    }
    // Resources own the lifetime cookie, never vice versa: ResizeBuffers can
    // release every backbuffer while recorded lists retain only weak evidence.
    void WatchBuffer(ID3D12Resource* resource) {
        if (!resource) return;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto found = resources.find(resource);
            if (found != resources.end() && !found->second.expired()) return;
        }
        auto watch = std::make_shared<Watch>();
        if (FAILED(resource->GetDevice(IID_PPV_ARGS(&watch->device)))) return;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto found = resources.find(resource);
            if (found != resources.end() && !found->second.expired()) return;
            for (auto it = resources.begin(); it != resources.end(); )
                if (it->second.expired()) it = resources.erase(it); else ++it;
            if (resources.size() >= 64) return;
            resources[resource] = watch;
            enabled.store(true, std::memory_order_release);
        }
        auto* cookie = new Cookie(watch);
        resource->SetPrivateDataInterface(key, cookie);
        cookie->Release();
    }
    void Reset(ID3D12CommandList* list) {
        if (!enabled.load(std::memory_order_acquire)) return;
        std::lock_guard<std::mutex> lock(mutex); lists.erase(list);
    }
    void Barriers(ID3D12CommandList* list, UINT count,
                  const D3D12_RESOURCE_BARRIER* barriers, const void* caller = nullptr) {
        if (ignoreDepth || !enabled.load(std::memory_order_acquire) || !list || !barriers) return;
        std::lock_guard<std::mutex> lock(mutex);
        for (UINT i = 0; i < count; ++i) {
            const auto& b = barriers[i];
            if (b.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION) continue;
            auto it = resources.find(b.Transition.pResource);
            if (it == resources.end()) continue;
            auto watch = it->second.lock(); if (!watch) continue;
            // NR/UI transitions recorded by this DLL are not game evidence.
            if (caller) {
                HMODULE origin = nullptr, self = nullptr;
                const DWORD flags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT;
                GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(caller), &origin);
                GetModuleHandleExW(flags, reinterpret_cast<LPCWSTR>(&Get), &self);
                if (!origin || origin == self) return;
            }
            if (lists.size() >= 4096 && !lists.count(list)) lists.clear();
            auto& states = lists[list];
            EndState* end = nullptr;
            for (auto& state : states) if (state.watch.lock() == watch) { end = &state; break; }
            if (!end) { states.push_back({watch, false}); end = &states.back(); }
            end->present = b.Flags == D3D12_RESOURCE_BARRIER_FLAG_NONE &&
                b.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES &&
                b.Transition.StateAfter == D3D12_RESOURCE_STATE_PRESENT &&
                b.Transition.StateBefore != D3D12_RESOURCE_STATE_PRESENT;
        }
    }
    void Submitted(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* batch) {
        if (ignoreDepth || !enabled.load(std::memory_order_acquire) || !queue || !batch ||
            queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) return;
        std::lock_guard<std::mutex> lock(mutex);
        Microsoft::WRL::ComPtr<IUnknown> device;
        for (UINT i = 0; i < count; ++i) {
            auto it = lists.find(batch[i]); if (it == lists.end()) continue;
            if (!device && FAILED(queue->GetDevice(IID_PPV_ARGS(&device)))) return;
            for (auto& state : it->second) if (auto watch = state.watch.lock()) {
                if (watch->device.Get() != device.Get()) continue;
                watch->submitted = ++serial;
                watch->writer = state.present ? queue : nullptr;
            }
        }
    }
    static bool HasNativeBufferAccess(IDXGISwapChain3* chain, const void* originalGetBuffer = nullptr) {
        if (!chain) return false;
        const auto dxgi = GetModuleHandleW(L"dxgi.dll");
        auto** table = *reinterpret_cast<void***>(chain);
        // GetBuffer and GetCurrentBackBufferIndex must both be implemented by
        // system DXGI. Do not trust a proxy returning private FG input buffers.
        for (unsigned slot : {9u, 36u}) {
            HMODULE owner = nullptr;
            if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                reinterpret_cast<LPCWSTR>(slot == 9 && originalGetBuffer ? originalGetBuffer : table[slot]), &owner) || owner != dxgi) return false;
        }
        return dxgi != nullptr;
    }
    Queue Acquire(ID3D12Resource* resource, bool frameGeneration, bool nativeBufferAccess = false) {
        if (frameGeneration && !nativeBufferAccess) return {};
        std::lock_guard<std::mutex> lock(mutex);
        auto it = resources.find(resource);
        if (it == resources.end()) return {};
        auto watch = it->second.lock();
        if (!watch || watch->consumed == watch->submitted) return {};
        watch->consumed = watch->submitted;
        return watch->writer;
    }
};
} // namespace DXL
