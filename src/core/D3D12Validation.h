#pragma once
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <atomic>
#include <mutex>
#include <vector>
#include "../common/Log.h"

namespace DXL {
// Opt-in API validation for the failing game. Must be enabled before its device
// exists. A marker file enables this without the launcher's JSON rewrite losing it.
class D3D12Validation {
public:
    class Scope {
    public:
        explicit Scope(const wchar_t* phase) : _old(_phase) { _phase=phase; }
        ~Scope() { _phase=_old; }
    private: const wchar_t* _old;
    };
    static void EnableBeforeDevice(bool enabled) noexcept {
        if (!enabled) return;
        ID3D12Debug* debug=nullptr;
        const HRESULT hr=D3D12GetDebugInterface(IID_PPV_ARGS(&debug));
        if (SUCCEEDED(hr)) {
            debug->EnableDebugLayer(); debug->Release(); _enabled=true;
            D5_LOG_WARN(L"D3D12 API validation ENABLED for this diagnostic run (GPU-based validation off)");
        } else D5_LOG_ERROR(L"D3D12 API validation unavailable: 0x%08X",hr);
    }
    static void Attach(ID3D12Device* device) noexcept {
        if (!_enabled || !device) return;
        std::lock_guard<std::mutex> lock(_mutex);
        if (_queue) return;
        ID3D12InfoQueue1* queue=nullptr;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&queue)))) return;
        DWORD cookie=0;
        const HRESULT hr=queue->RegisterMessageCallback(&Message,D3D12_MESSAGE_CALLBACK_FLAG_NONE,nullptr,&cookie);
        if (FAILED(hr)) { queue->Release(); D5_LOG_ERROR(L"D3D12 validation callback failed: 0x%08X",hr); return; }
        _queue=queue; _cookie=cookie;
        D5_LOG_INFO(L"D3D12 validation callback attached; errors/warnings include recording phase");
        for(UINT64 i=0;i<queue->GetNumStoredMessages();++i) {
            SIZE_T n=0; queue->GetMessage(i,nullptr,&n); std::vector<unsigned char> data(n);
            auto* message=reinterpret_cast<D3D12_MESSAGE*>(data.data());
            if (SUCCEEDED(queue->GetMessage(i,message,&n)))
                Message(message->Category,message->Severity,message->ID,message->pDescription,nullptr);
        }
    }
    static void Detach() noexcept {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_queue) { _queue->UnregisterMessageCallback(_cookie); _queue->Release(); _queue=nullptr; }
    }
private:
    static void CALLBACK Message(D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY severity,
        D3D12_MESSAGE_ID id, LPCSTR description, void*) noexcept {
        if (severity>D3D12_MESSAGE_SEVERITY_WARNING) return;
        // Bound repeated diagnostics without doing file I/O on every draw.
        unsigned seen=_counts[unsigned(id)%2048].fetch_add(1);
        if (seen >= (severity<=D3D12_MESSAGE_SEVERITY_ERROR?4u:1u)) return;
        D5_LOG_ERROR(L"D3D12 validation phase=%s severity=%u id=%u: %hs",
            _phase,unsigned(severity),unsigned(id),description);
    }
    inline static thread_local const wchar_t* _phase=L"game/queue";
    inline static std::atomic<bool> _enabled{false};
    inline static std::atomic<unsigned> _counts[2048]{};
    inline static std::mutex _mutex;
    inline static ID3D12InfoQueue1* _queue=nullptr;
    inline static DWORD _cookie=0;
};
}
