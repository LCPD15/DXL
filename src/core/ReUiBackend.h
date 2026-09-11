// Shared ImGui D3D12/D3D11 in-game overlay backend for DXL.
// Owns: imgui context + renderer backend, fonts, RTV/descriptor heaps, one
// command list, input polling, and a minimal WndProc swallow (while the panel
// is open the game stops seeing mouse/raw input so the camera does not fight
// the UI). All drawing happens on the present thread right before Present.
//
// Cursor policy (ReShade-style): while the panel is open the backend hooks a
// handful of user32 cursor APIs (SetCursorPos / ClipCursor / GetCursorPos /
// GetMessagePos / ShowCursor / SetCursor) so the game can not re-centre or
// confine the OS cursor, and per-frame it clips the cursor to the game window
// and draws a software pointer when needed. Existing WM_INPUT relative motion
// drives a fallback pointer if the game still pins the OS cursor; raw input
// registrations are never replaced. The game receives a frozen position, so
// GetCursorPos-based mouse look does not spin the camera while you use the UI.
#pragma once
#include <Windows.h>
#include <d3d12.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <atomic>
#include <functional>

struct ImFont;

namespace ReUi {

// Enable on the present thread with the actual D3D12 device / present queue.
// Queue/format changes rebuild only GPU resources, preserving window hooks.
bool InitOnce(ID3D12Device* device, ID3D12CommandQueue* queue,
              HWND hwnd, DXGI_FORMAT rtvFormat);
// D3D11 bridge games keep the same ImGui/input implementation. Rendering uses
// an isolated immediate-context state so the game's complete pipeline survives.
bool InitOnce11(ID3D11Device* device, ID3D11DeviceContext* context,
                HWND hwnd, DXGI_FORMAT rtvFormat);
void Shutdown() noexcept;
// Drain our overlay work before DXGI releases/replaces back buffers.
bool WaitIdle() noexcept;

// Fonts baked by the backend. The panel uses the default font (2x of the old
// 13px one); toast text should be drawn with FontToast() at ToastFontSize()
// (3x) explicitly — this imgui fork renders fonts at a chosen size, so pass
// the same size you want to AddText / CalcTextSizeA.
ImFont* FontUi() noexcept;
ImFont* FontToast() noexcept;
float ToastFontSize() noexcept;

// Tells the backend which atomic gates the UI panel (used for cursor +
// input swallowing). May be null. Call on the present thread every frame,
// including when drawing is skipped, so closing can release cursor ownership.
void SetUiOpen(std::atomic<bool>* uiOpen) noexcept;

// Call once per frame after the game finished rendering but before Present.
// Builds an imgui frame (polled input), runs buildUi() to add widgets, then
// records the draw data onto the current back buffer.
void FramePresent(IDXGISwapChain* swapChain,
                  const std::function<void()>& buildUi);
void FramePresent11(IDXGISwapChain* swapChain,
                    const std::function<void()>& buildUi);

// Private accelerator image used by OpenGL / D3D9 capture.
void FrameTexture11(ID3D11Texture2D* image, const std::function<void()>& buildUi);

}  // namespace ReUi
