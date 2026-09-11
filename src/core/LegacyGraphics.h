#pragma once

#include <windows.h>
#include <d3d11.h>
#include "../common/IpcProtocol.h"

namespace DXL {

// Native x64 OpenGL 3.2+ (8-bit RGB window) / D3D9 (A8/X8R8G8B8)
// presentation capture. D3D9 MSAA uses resolve + texture draw. The private D3D11
// accelerator is deliberately CPU-connected, so hybrid GPU configurations do
// not require the game and NR devices to share an adapter or resource handle.
// Callbacks run synchronously on the game's Present thread and are serialized.
struct LegacyGraphicsCallbacks {
    // Called before allocations/capture, including while the effect is off.
    // Return false to preserve the native frame and discard buffered results.
    bool (*begin)(Ipc::GraphicsApi api, HWND window) noexcept = nullptr;
    // Return true only when image contains a processed frame. Device/image are
    // borrowed. A retained device must be AddRef'd. D3D11 commands may remain
    // queued: this module flushes and polls completion without a CPU GPU wait.
    bool (*process)(ID3D11Device* device, ID3D11Texture2D* image,
        Ipc::GraphicsApi api, HWND window) noexcept = nullptr;
};

// Install on the initialization worker, never from DllMain. Poll periodically
// from that worker to discover a d3d9.dll loaded after injection. GL is hooked
// through GDI SwapBuffers, so opengl32.dll does not need to be loaded early.
bool InstallLegacyGraphicsHooks(const LegacyGraphicsCallbacks& callbacks) noexcept;
// Pass false once another graphics API owns the process; export discovery is
// cheap, but a late D3D9 dummy device is then neither needed nor constructed.
void PollLegacyGraphicsHooks(bool allowDeviceProbe = true) noexcept;
// Call before releasing the core's NR bridge. Stops callbacks and releases
// D3D9 reset-sensitive surfaces. Trampolines stay allocated for in-flight calls.
void StopLegacyGraphicsHooks() noexcept;

struct LegacyGraphicsStats {
    uint64_t captures = 0, processed = 0, displayed = 0, skipped = 0;
    uint64_t generation = 0;
    double lastCaptureMs = 0;
    HRESULT d3d9Probe = E_PENDING, d3d9ExProbe = E_PENDING;
    const char* error = "";
};
LegacyGraphicsStats GetLegacyGraphicsStats() noexcept;

} // namespace DXL
