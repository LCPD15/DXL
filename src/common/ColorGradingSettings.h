#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <string>
#include "PostFxSettings.h"

namespace DXL {

struct ColorGradingControl {
    bool enabled = false;
    float value = 0.0f;
    bool Active(float neutral = 0.0f) const noexcept {
        return enabled && std::isfinite(value) && std::abs(value - neutral) > 0.00001f;
    }
};

// Values are deliberately independent from NR. The owner persists these in
// the game's profile and snapshots them before recording GPU work.
struct ColorGradingSettings {
    ColorGradingControl exposure;
    ColorGradingControl contrast{false, 1.0f};
    ColorGradingControl saturation{false, 1.0f};
    ColorGradingControl temperature;
    ColorGradingControl tint;
    ColorGradingControl shadows;
    ColorGradingControl midtones;
    ColorGradingControl highlights;
    ColorGradingControl sharpen;
    ColorGradingControl bloom;
    float bloomThreshold = 0.65f;
    float bloomSoftKnee = 0.5f;
    float bloomRadius = 1.0f;
    float bloomScatter = 0.7f;
    float bloomSaturation = 1.0f;
    ColorGradingControl vignette;
    ColorGradingControl grain;
    ColorGradingControl monochrome;
    bool lutEnabled = false;
    float lutIntensity = 1.0f;
    std::string lutFile;
    uint32_t lutReloadRevision = 0; // Runtime refresh request; not persisted.
    std::vector<PostFxSetting> fx;
    uint32_t fxReloadRevision = 0; // Explicit refresh, not persisted.

    bool AnyBasicActive() const noexcept {
        return exposure.Active() || contrast.Active(1.0f) || saturation.Active(1.0f) ||
            temperature.Active() || tint.Active() || shadows.Active() || midtones.Active() ||
            highlights.Active();
    }
    bool AnyFinalActive() const noexcept {
        if (std::any_of(fx.begin(), fx.end(), [](const auto& effect) { return effect.enabled; })) return true;
        return sharpen.Active() || bloom.Active() || vignette.Active() ||
            grain.Active() || monochrome.Active() ||
            (lutEnabled && std::isfinite(lutIntensity) && lutIntensity > 0.00001f && !lutFile.empty());
    }
    bool AnyActive() const noexcept { return AnyBasicActive() || AnyFinalActive(); }
    ColorGradingSettings BasicOnly() const {
        auto result=*this;
        result.sharpen.enabled=result.bloom.enabled=result.vignette.enabled=false;
        result.grain.enabled=result.monochrome.enabled=result.lutEnabled=false;
        result.fx.clear();
        return result;
    }
    ColorGradingSettings FinalOnly() const {
        auto result=*this;
        result.exposure.enabled=result.contrast.enabled=result.saturation.enabled=false;
        result.temperature.enabled=result.tint.enabled=result.shadows.enabled=false;
        result.midtones.enabled=result.highlights.enabled=false;
        return result;
    }
};

} // namespace DXL
