#pragma once

#include <dxgiformat.h>

namespace DXL {

// Keep working textures in the game's copy-compatible format family. Unity
// exposes RGBA8 SR output as TYPELESS; use its copy-compatible UNORM color view.
// A typeless resource is valid; only its SRV/UAV needs a concrete typed format.
// Do not guess float/UNORM for other typeless families (some are depth guides).
constexpr DXGI_FORMAT NrColorViewFormat(DXGI_FORMAT format) noexcept {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    default:
        return format;
    }
}

} // namespace DXL
