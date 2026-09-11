#pragma once

#include <cmath>

namespace DXL {

// The automatic fallback maps its sampled neutral grey to encoded 0.5.
// Hybrid is identity here (below its 0.75 knee), followed by standard sRGB.
// Keep this separate from the legacy configurable pure-gamma curve.
inline float NrSampleTargetLinear(bool pureGamma, float gamma) noexcept {
    if (pureGamma) return gamma > 1.001f ? std::pow(0.5f, gamma) : 0.5f;
    return 0.21404114f; // sRGB EOTF(0.5)
}

} // namespace DXL
