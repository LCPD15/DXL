#pragma once
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <atomic>
#include <chrono>
#include <mutex>
#include <cstdint>

namespace DXL {
class FgPresentation {
public:
    using Time = std::chrono::steady_clock::time_point;
    void ObserveInput(IUnknown* identity, Time received) noexcept;
    float InputFps() const noexcept;
    float OutputFps() const noexcept;
    // -1 until a recent window includes at least one submitted real frame.
    float OutputLatencyMs() const noexcept;
    // Capture is serialized by the same lock as final NR and UI. The worker
    // owns a different presentation queue and never advances the host chain.
    void Capture(Time received, IDXGISwapChain3* source, HWND hwnd, DXGI_FORMAT fmt, DXGI_COLOR_SPACE_TYPE space, ID3D12Resource* frame,
        ID3D12CommandQueue* writer, bool creationQueue, ID3D11Texture2D* frame11, HMODULE self,
        UINT multiplier, std::atomic<uint32_t>& maxMultiplier, std::atomic<uint32_t>& status, std::atomic<uint64_t>& presented) noexcept;
    void Stop() noexcept;
    static bool Internal() noexcept;
private:
    class RateWindow {
        mutable std::mutex mutex_;
        Time start_{}, last_{};
        uint64_t intervals_ = 0;
        double latencySum_ = 0;
        unsigned latencySamples_ = 0;
        float fps_ = 0, latencyMs_ = -1;
        void Clear() noexcept;
    public:
        void Reset() noexcept;
        void Record(Time now, double latencyMs = -1) noexcept;
        float Read(bool latency) const noexcept;
    };
    RateWindow input_, output_;
    IUnknown* inputIdentity_ = nullptr; // Opaque key; producer serialized by NR state lock.
    struct Session;
    Session* session_ = nullptr;
    UINT pendingWidth_ = 0, pendingHeight_ = 0;
    ULONGLONG stableSince_ = 0;
};
} // namespace DXL
