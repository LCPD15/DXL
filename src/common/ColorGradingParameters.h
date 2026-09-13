#pragma once
#include "ColorGradingSettings.h"
#include "PostFxState.h"
#include <array>
#include <string_view>

namespace DXL {
struct ColorGradingParameter {
    const char* key;
    const char* chinese;
    const char* english;
    ColorGradingControl ColorGradingSettings::*member;
    float neutral, minimum, maximum;
};
inline constexpr std::array<ColorGradingParameter, 13> ColorGradingParameters{{
    {"colorExposure", "曝光 / Exposure", "Exposure", &ColorGradingSettings::exposure, 0, -4, 4},
    {"colorContrast", "对比度 / Contrast", "Contrast", &ColorGradingSettings::contrast, 1, 0, 2},
    {"colorSaturation", "饱和度 / Saturation", "Saturation", &ColorGradingSettings::saturation, 1, 0, 2},
    {"colorTemperature", "色温 / Temperature", "Temperature", &ColorGradingSettings::temperature, 0, -1, 1},
    {"colorTint", "色偏 / Tint", "Tint", &ColorGradingSettings::tint, 0, -1, 1},
    {"colorShadows", "阴影 / Shadows", "Shadows", &ColorGradingSettings::shadows, 0, -1, 1},
    {"colorMidtones", "中间调 / Midtones", "Midtones", &ColorGradingSettings::midtones, 0, -1, 1},
    {"colorHighlights", "高光 / Highlights", "Highlights", &ColorGradingSettings::highlights, 0, -1, 1},
    {"colorSharpen", "锐化 / Sharpen", "Sharpen", &ColorGradingSettings::sharpen, 0, 0, 3},
    {"colorBloom", "柔光 / Bloom", "Soft glow / Bloom", &ColorGradingSettings::bloom, 0, 0, 3},
    {"colorVignette", "暗角 / Vignette", "Vignette", &ColorGradingSettings::vignette, 0, 0, 1},
    {"colorGrain", "胶片颗粒 / Film grain", "Film grain", &ColorGradingSettings::grain, 0, 0, 1},
    {"colorMonochrome", "黑白 / Monochrome", "Monochrome", &ColorGradingSettings::monochrome, 0, 0, 1},
}};
struct BloomParameter {
    const char* key;
    const char* chinese;
    const char* english;
    float ColorGradingSettings::*member;
    float defaultValue, minimum, maximum;
    const char* hintChinese;
    const char* hintEnglish;
};
inline constexpr std::array<BloomParameter, 5> BloomParameters{{
    {"colorBloomThreshold", "高光阈值 / Highlight threshold", "Highlight threshold", &ColorGradingSettings::bloomThreshold, .65f, 0, 2,
        "越高，越只让较亮的区域产生光晕。", "Higher values restrict glow to brighter areas."},
    {"colorBloomSoftKnee", "阈值柔和度 / Threshold softness", "Threshold softness", &ColorGradingSettings::bloomSoftKnee, .5f, 0, 1,
        "让阈值附近的光晕过渡更柔和。", "Softens the transition around the highlight threshold."},
    {"colorBloomRadius", "光晕半径 / Glow radius", "Glow radius", &ColorGradingSettings::bloomRadius, 1, .25f, 3,
        "调整光晕模糊的范围。", "Adjusts the blur radius of the glow."},
    {"colorBloomScatter", "扩散 / Scatter", "Scatter", &ColorGradingSettings::bloomScatter, .7f, 0, 1,
        "越高，宽范围的柔光越明显。", "Higher values emphasize the wider glow."},
    {"colorBloomSaturation", "光晕饱和度 / Glow saturation", "Glow saturation", &ColorGradingSettings::bloomSaturation, 1, 0, 2,
        "只调整光晕的色彩强度，0 为无彩色光晕。", "Adjusts only the glow color; zero produces a neutral glow."},
}};
template<class Reader> ColorGradingSettings ReadColorGradingSettings(const Reader& reader) {
    ColorGradingSettings result;
    for (const auto& entry : ColorGradingParameters) {
        auto& control = result.*entry.member;
        control.enabled = reader.GetBool(std::string(entry.key) + "Enabled", false);
        const auto v = reader.GetFloat(entry.key, entry.neutral);
        control.value = std::isfinite(v) ? std::clamp(v, entry.minimum, entry.maximum) : entry.neutral;
    }
    for (const auto& entry : BloomParameters) {
        const auto value = reader.GetFloat(entry.key, entry.defaultValue);
        result.*entry.member = std::isfinite(value) ? std::clamp(value, entry.minimum, entry.maximum) : entry.defaultValue;
    }
    result.lutEnabled = reader.GetBool("colorLutEnabled", false);
    const auto v = reader.GetFloat("colorLutIntensity", 1);
    result.lutIntensity = std::isfinite(v) ? std::clamp(v, 0.0f, 1.0f) : 1.0f;
    result.lutFile = reader.GetString("colorLutFile", "");
    result.fx = DeserializePostFxState(reader.GetString("colorFxState", ""));
    return result;
}
} // namespace DXL
