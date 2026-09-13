#pragma once
#include "../src/core/ReShadeUiMetadata.h"
#include <stdexcept>
#include <cmath>

inline void VerifyReShadeUiMetadata(reshade::api::effect_runtime* runtime) {
    using namespace DXL::ReShadeUiDetail;
    const auto require = [](bool okay, const char* message) { if (!okay) throw std::runtime_error(message); };
    const auto effects = Enumerate(runtime, true);
    for (size_t index = 1; index < effects.size(); ++index)
        require(!DXL::PostFxFilenameLess(effects[index].file, effects[index - 1].file), "ReShade UI files sort alphabetically");
    const auto effect = std::find_if(effects.begin(), effects.end(), [](const auto& item) { return item.file == "90-DXL-UI-Validation.fx"; });
    require(effect != effects.end(), "Real compiler exposes UI metadata test effect");
    require(effect->techniques.size() == 2, "ReShade UI enumerates both techniques from one file");
    require(effect->uniforms.size() == 7, "ReShade UI excludes source and hidden uniforms");
    const auto uniform = [&](const char* name) -> const Uniform& {
        const auto found = std::find_if(effect->uniforms.begin(), effect->uniforms.end(), [&](const auto& item) { return item.name == name; });
        require(found != effect->uniforms.end(), "ReShade UI visible uniform exists"); return *found;
    };
    require(uniform("Strength").label == "Strength" && uniform("Strength").tooltip == "Runtime metadata", "ReShade UI reads actual labels and tooltips");
    require(uniform("Tint").uiType == "color" && uniform("Tint").rows == 3, "ReShade UI exposes float3 color");
    require(uniform("Mode").type == reshade::api::format::r32_sint && uniform("Mode").items == std::vector<std::string>{"One", "Two", "Three"}, "ReShade UI preserves embedded NUL enum annotation");
    require(uniform("Enabled").type == reshade::api::format::r32_typeless, "ReShade UI maps boolean native type");
    require(uniform("Counter").type == reshade::api::format::r32_uint, "ReShade UI maps unsigned native type");
    require(uniform("Weights").arrayLength == 2 && uniform("Weights").rows == 2, "ReShade UI exposes vector arrays");
    require(uniform("Locked").readOnly, "ReShade UI honors noedit annotation");
    const auto chinese = Enumerate(runtime, false);
    const auto chineseEffect = std::find_if(chinese.begin(), chinese.end(), [](const auto& item) { return item.file == "90-DXL-UI-Validation.fx"; });
    require(chineseEffect != chinese.end() && chineseEffect->uniforms.front().label == "强度", "ReShade UI reads localized author annotation");
    auto primary = runtime->find_technique(effect->file.c_str(), "UiPrimary");
    auto secondary = runtime->find_technique(effect->file.c_str(), "UiSecondary");
    runtime->set_technique_state(primary, true); runtime->set_technique_state(secondary, false);
    runtime->set_uniform_value_float(uniform("Strength").handle, 1.5f);
    runtime->set_uniform_value_int(uniform("Mode").handle, 2);
    runtime->set_uniform_value_float(uniform("Locked").handle, 0.9f);
    ResetEffectValues(runtime, *effect);
    float strength = 0, locked = 0; int32_t mode = 0;
    runtime->get_uniform_value_float(uniform("Strength").handle, &strength, 1);
    runtime->get_uniform_value_int(uniform("Mode").handle, &mode, 1);
    runtime->get_uniform_value_float(uniform("Locked").handle, &locked, 1);
    require(std::abs(strength - 0.25f) < 0.0001f && mode == 1, "ReShade UI resets runtime defaults");
    require(runtime->get_technique_state(primary) && !runtime->get_technique_state(secondary), "ReShade UI reset retains mixed technique switches");
    require(std::abs(locked - 0.9f) < 0.0001f, "ReShade UI reset leaves noedit value intact");
    runtime->set_technique_state(primary, false);
    runtime->reset_uniform_value(uniform("Locked").handle);
    std::puts("PASS: native ReShade UI metadata, typed annotations, enum items, arrays, bilingual labels and value-only reset");
}
