#pragma once
#include "ReShadeUiMetadata.h"
#include "imgui.h"
#include <type_traits>

namespace DXL {
namespace ReShadeUiDetail {
inline void Tooltip(const std::string& text) {
    if (!text.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::BeginTooltip(); ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30);
        ImGui::TextUnformatted(text.c_str()); ImGui::PopTextWrapPos(); ImGui::EndTooltip();
    }
}
template<class T> inline bool Numeric(Runtime* runtime, const Uniform& uniform, size_t components,
    T* values, ImGuiDataType dataType) {
    std::array<T, 16> minimum{}, maximum{}, step{};
    bool hasMinimum = false, hasMaximum = false;
    if constexpr (std::is_same_v<T, float>) {
        hasMinimum = runtime->get_annotation_float_from_uniform_variable(uniform.handle, "ui_min", minimum.data(), 1);
        hasMaximum = runtime->get_annotation_float_from_uniform_variable(uniform.handle, "ui_max", maximum.data(), 1);
        if (!hasMaximum) maximum[0] = 1;
        if (!runtime->get_annotation_float_from_uniform_variable(uniform.handle, "ui_step", step.data(), 1)) step[0] = 0.001f;
    } else if constexpr (std::is_same_v<T, int32_t>) {
        hasMinimum = runtime->get_annotation_int_from_uniform_variable(uniform.handle, "ui_min", minimum.data(), 1);
        hasMaximum = runtime->get_annotation_int_from_uniform_variable(uniform.handle, "ui_max", maximum.data(), 1);
        if (!hasMaximum) maximum[0] = 1;
        if (!runtime->get_annotation_int_from_uniform_variable(uniform.handle, "ui_step", step.data(), 1)) step[0] = 1;
    } else {
        hasMinimum = runtime->get_annotation_uint_from_uniform_variable(uniform.handle, "ui_min", minimum.data(), 1);
        hasMaximum = runtime->get_annotation_uint_from_uniform_variable(uniform.handle, "ui_max", maximum.data(), 1);
        if (!hasMaximum) maximum[0] = 1;
        if (!runtime->get_annotation_uint_from_uniform_variable(uniform.handle, "ui_step", step.data(), 1)) step[0] = 1;
    }
    // ReShade UI bounds/step are scalar annotations shared by vector components.
    minimum.fill(minimum[0]); maximum.fill(maximum[0]); step.fill(step[0]);
    const bool slider = uniform.uiType == "slider" || (uniform.uiType.empty() && hasMinimum && hasMaximum);
    bool changed = false;
    if constexpr (std::is_same_v<T, float>) {
        if (uniform.uiType == "color" && uniform.columns == 1 && (components == 3 || components == 4)) {
            changed = components == 3 ? ImGui::ColorEdit3("##value", values, ImGuiColorEditFlags_Float) :
                ImGui::ColorEdit4("##value", values, ImGuiColorEditFlags_Float);
            for (size_t i = 0; i < components; ++i) if (!std::isfinite(values[i])) values[i] = 0;
            return changed;
        }
    } else if (components == 1 && !uniform.items.empty()) {
        const int64_t selected = static_cast<int64_t>(values[0]);
        const char* preview = selected >= 0 && size_t(selected) < uniform.items.size() ? uniform.items[size_t(selected)].c_str() : "-";
        if (ImGui::BeginCombo("##value", preview)) {
            for (size_t index = 0; index < uniform.items.size(); ++index) {
                if (ImGui::Selectable(uniform.items[index].c_str(), selected == int64_t(index))) { values[0] = T(index); changed = true; }
            }
            ImGui::EndCombo();
        }
        return changed;
    }
    for (size_t component = 0; component < components; ++component) {
        ImGui::PushID(int(component)); ImGui::SetNextItemWidth(-1);
        const T before = values[component];
        const bool validRange = std::isfinite(double(minimum[component])) && std::isfinite(double(maximum[component])) && minimum[component] < maximum[component];
        if (slider && validRange) changed |= ImGui::SliderScalar("##value", dataType, &values[component], &minimum[component], &maximum[component], nullptr, ImGuiSliderFlags_AlwaysClamp);
        else if (uniform.uiType == "drag") {
            const float speed = std::isfinite(double(step[component])) && step[component] > 0 ? float(step[component]) : 0.01f;
            changed |= ImGui::DragScalar("##value", dataType, &values[component], speed,
                hasMinimum && validRange ? &minimum[component] : nullptr, hasMaximum && validRange ? &maximum[component] : nullptr);
        } else changed |= ImGui::InputScalar("##value", dataType, &values[component]);
        if constexpr (std::is_same_v<T, float>) if (!std::isfinite(values[component])) values[component] = std::isfinite(before) ? before : 0;
        if (hasMinimum && hasMaximum && validRange) values[component] = std::clamp(values[component], minimum[component], maximum[component]);
        ImGui::PopID();
    }
    return changed;
}
inline bool DrawUniform(Runtime* runtime, const Uniform& uniform, bool english) {
    bool changed = false;
    const size_t components = size_t(uniform.rows) * uniform.columns;
    if (!components || components > 16) return false;
    const size_t arrayLength = std::max(uniform.arrayLength, 1u);
    ImGui::PushID(uniform.name.c_str());
    ImGui::BeginDisabled(uniform.readOnly);
    for (size_t arrayIndex = 0; arrayIndex < arrayLength; ++arrayIndex) {
        ImGui::PushID(int(arrayIndex));
        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding();
        if (arrayLength == 1) ImGui::TextUnformatted(uniform.label.c_str());
        else ImGui::Text("%s [%zu]", uniform.label.c_str(), arrayIndex);
        Tooltip(uniform.tooltip);
        ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1);
        switch (uniform.type) {
        case reshade::api::format::r32_typeless: {
            std::array<bool, 16> value{}; runtime->get_uniform_value_bool(uniform.handle, value.data(), components, arrayIndex);
            bool rowChanged = false;
            if (uniform.uiType == "button" && components == 1) {
                const bool pressed = ImGui::Button(english ? "Apply" : "执行", ImVec2(-1, 0));
                rowChanged = value[0] != pressed; value[0] = pressed;
            } else for (size_t component = 0; component < components; ++component) {
                ImGui::PushID(int(component));
                const char* label = component < uniform.items.size() ? uniform.items[component].c_str() : "##value";
                rowChanged |= ImGui::Checkbox(label, &value[component]); ImGui::PopID();
                if (component + 1 < components && uniform.items.empty()) ImGui::SameLine();
            }
            if (rowChanged) { runtime->set_uniform_value_bool(uniform.handle, value.data(), components, arrayIndex); changed = true; }
            break;
        }
        case reshade::api::format::r32_float: {
            std::array<float, 16> value{}; runtime->get_uniform_value_float(uniform.handle, value.data(), components, arrayIndex);
            if (Numeric(runtime, uniform, components, value.data(), ImGuiDataType_Float)) {
                runtime->set_uniform_value_float(uniform.handle, value.data(), components, arrayIndex); changed = true;
            }
            break;
        }
        case reshade::api::format::r32_sint: {
            std::array<int32_t, 16> value{}; runtime->get_uniform_value_int(uniform.handle, value.data(), components, arrayIndex);
            if (Numeric(runtime, uniform, components, value.data(), ImGuiDataType_S32)) {
                runtime->set_uniform_value_int(uniform.handle, value.data(), components, arrayIndex); changed = true;
            }
            break;
        }
        case reshade::api::format::r32_uint: {
            std::array<uint32_t, 16> value{}; runtime->get_uniform_value_uint(uniform.handle, value.data(), components, arrayIndex);
            if (Numeric(runtime, uniform, components, value.data(), ImGuiDataType_U32)) {
                runtime->set_uniform_value_uint(uniform.handle, value.data(), components, arrayIndex); changed = true;
            }
            break;
        }
        default: ImGui::TextDisabled("%s", english ? "Unsupported parameter type" : "不支持的参数类型"); break;
        }
        ImGui::TableNextColumn();
        if (arrayIndex == 0 && ImGui::Button(english ? "Reset" : "重置", ImVec2(-1, 0))) { runtime->reset_uniform_value(uniform.handle); changed = true; }
        ImGui::PopID();
    }
    ImGui::EndDisabled(); ImGui::PopID(); return changed;
}
} // namespace ReShadeUiDetail

// Called on the owner's serialized render/UI thread. Handles are enumerated each draw and
// never retained across an effect reload. The owner saves the per-game preset when true.
inline bool DrawReShadeUi(reshade::api::effect_runtime* runtime, bool english, bool* reloadRequested = nullptr) {
    using namespace ReShadeUiDetail;
    if (!ImGui::CollapsingHeader(english ? "Custom FX" : "自定义 FX / Custom FX", ImGuiTreeNodeFlags_DefaultOpen)) return false;
    ImGui::TextWrapped("%s", english ?
        "Custom .fx effects run on the final image, including game UI. Copy their .fxh includes and textures together. Changes save per game." :
        "自定义 .fx 在最终画面上执行，也会影响游戏 UI。请同时放入依赖的 .fxh 和纹理。参数按游戏保存。");
    if (!runtime) { ImGui::TextDisabled("%s", english ? "Waiting for the post-processing runtime..." : "等待后处理运行时..."); return false; }
    if (reloadRequested && ImGui::Button(english ? "Reload custom FX" : "重新加载自定义 FX")) *reloadRequested = true;
    const auto effects = Enumerate(runtime, english);
    if (effects.empty()) ImGui::TextDisabled("%s", english ? "No compiled custom FX. Check the post-processing folder and effect log." :
        "没有已编译的自定义 FX。请检查 post-processing 文件夹和效果日志。");
    bool changed = false;
    for (const auto& effect : effects) {
        ImGui::PushID(effect.file.c_str());
        if (ImGui::CollapsingHeader(effect.file.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
            if (ImGui::BeginTable("ReShadeValues", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
                const float unit = ImGui::GetFontSize();
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch, 1);
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, unit * 8.5f);
                ImGui::TableSetupColumn("Reset", ImGuiTableColumnFlags_WidthFixed, unit * 3.6f);
                for (const auto& technique : effect.techniques) {
                    if (technique.hidden) continue;
                    ImGui::PushID(technique.name.c_str());
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(technique.label.c_str()); Tooltip(technique.tooltip);
                    ImGui::TableNextColumn(); bool enabled = runtime->get_technique_state(technique.handle);
                    if (ImGui::Checkbox(english ? "Enable" : "启用", &enabled)) { runtime->set_technique_state(technique.handle, enabled); changed = true; }
                    ImGui::TableNextColumn(); ImGui::PopID();
                }
                std::string category;
                for (const auto& uniform : effect.uniforms) {
                    if (category != uniform.category && !uniform.category.empty()) {
                        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextUnformatted(uniform.category.c_str());
                        category = uniform.category;
                    }
                    if (!uniform.text.empty()) {
                        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::TextWrapped("%s", uniform.text.c_str());
                    }
                    changed |= DrawUniform(runtime, uniform, english);
                }
                ImGui::EndTable();
            }
            if (!effect.uniforms.empty() && ImGui::Button(english ? "Reset parameter values" : "重置参数数值")) {
                ResetEffectValues(runtime, effect); changed = true;
            }
        }
        ImGui::PopID();
    }
    return changed;
}
} // namespace DXL
