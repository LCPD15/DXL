#pragma once
#include <windows.h>
#include <d3d11.h>
#include <d3d12.h>
#include <dxgi.h>
#include "../../third_party/reshade/include/reshade_api.hpp"

namespace DXL {
// One owner, called under the NR/UI state lock. The private runtime is bound
// only to a verified display swapchain, or our legacy capture bridge.
class ReShadeBridge {
public:
    bool Needed(HMODULE module) noexcept;
    bool Render(IDXGISwapChain* chain, IUnknown* device, ID3D12CommandQueue* queue,
                HMODULE module, bool enabled) noexcept;
    bool RenderTexture11(ID3D11Device* device, ID3D11Texture2D* image,
                         HMODULE module, bool enabled) noexcept;
    void BeforeResize(IDXGISwapChain* chain) noexcept;
    void BeforeCreate(HWND window) noexcept;
    void Shutdown() noexcept;
    void Reload() noexcept;
    void Save() noexcept;
    reshade::api::effect_runtime* Runtime() const noexcept;
    const char* Error() const noexcept;
    static bool InternalCall() noexcept;
private:
    struct State;
    State* _state = nullptr;
};
}
