#pragma once
#include "../../third_party/reshade/include/reshade_api.hpp"
#include "../common/PostFxCatalog.h"
#include <array>
#include <limits>

namespace DXL {
namespace ReShadeUiDetail {
using Runtime = reshade::api::effect_runtime;
using UniformHandle = reshade::api::effect_uniform_variable;
using TechniqueHandle = reshade::api::effect_technique;

// Read metadata from the compiled runtime. Shader source is never parsed or rewritten by this UI.
template<class Getter> inline std::string ReadString(Getter getter, bool keepEmbeddedNulls = false) {
    size_t size = 0;
    getter(nullptr, &size);
    if (!size || size > 1024 * 1024) return {};
    std::string value(size, '\0');
    getter(value.data(), &size);
    if (size < value.size()) value.resize(size);
    if (keepEmbeddedNulls) { while (!value.empty() && value.back() == '\0') value.pop_back(); }
    else if (const auto end = value.find('\0'); end != std::string::npos) value.resize(end);
    return value;
}
inline std::string Annotation(Runtime* runtime, UniformHandle uniform, const char* key, bool items = false) {
    return ReadString([&](char* out, size_t* size) {
        runtime->get_annotation_string_from_uniform_variable(uniform, key, out, size);
    }, items);
}
inline std::string Annotation(Runtime* runtime, TechniqueHandle technique, const char* key) {
    return ReadString([&](char* out, size_t* size) {
        runtime->get_annotation_string_from_technique(technique, key, out, size);
    });
}
inline std::vector<std::string> Items(const std::string& text) {
    std::vector<std::string> result;
    for (size_t begin = 0; begin < text.size();) {
        const auto end = text.find('\0', begin);
        result.push_back(text.substr(begin, end == std::string::npos ? end : end - begin));
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return result;
}
struct Uniform {
    UniformHandle handle{};
    std::string name, label, tooltip, category, uiType, text;
    std::vector<std::string> items;
    reshade::api::format type = reshade::api::format::unknown;
    uint32_t rows = 0, columns = 0, arrayLength = 0;
    bool readOnly = false;
};
struct Technique {
    TechniqueHandle handle{};
    std::string name, label, tooltip;
    bool hidden = false;
};
struct Effect {
    std::string file;
    std::vector<Technique> techniques;
    std::vector<Uniform> uniforms;
};
template<class Handle> inline std::string LocalizedAnnotation(Runtime* runtime, Handle handle, const char* key, bool english) {
    auto value = Annotation(runtime, handle, (std::string(key) + (english ? "_en" : "_zh")).c_str());
    return value.empty() ? Annotation(runtime, handle, key) : value;
}
inline std::vector<Effect> Enumerate(Runtime* runtime, bool english) {
    std::vector<Effect> effects;
    if (!runtime) return effects;
    runtime->enumerate_techniques(nullptr, [&](Runtime* owner, TechniqueHandle handle) {
        auto file = ReadString([&](char* out, size_t* size) { owner->get_technique_effect_name(handle, out, size); });
        if (file.empty()) return;
        auto found = std::find_if(effects.begin(), effects.end(), [&](const auto& effect) { return effect.file == file; });
        if (found == effects.end()) { effects.push_back({std::move(file), {}, {}}); found = std::prev(effects.end()); }
        Technique technique;
        technique.handle = handle;
        technique.name = ReadString([&](char* out, size_t* size) { owner->get_technique_name(handle, out, size); });
        technique.label = LocalizedAnnotation(owner, handle, "ui_label", english);
        if (technique.label.empty()) technique.label = technique.name;
        technique.tooltip = LocalizedAnnotation(owner, handle, "ui_tooltip", english);
        owner->get_annotation_bool_from_technique(handle, "hidden", &technique.hidden, 1);
        found->techniques.push_back(std::move(technique));
    });
    std::sort(effects.begin(), effects.end(), [](const auto& a, const auto& b) { return PostFxFilenameLess(a.file, b.file); });
    for (auto& effect : effects) {
        runtime->enumerate_uniform_variables(effect.file.c_str(), [&](Runtime* owner, UniformHandle handle) {
            bool hidden = false;
            owner->get_annotation_bool_from_uniform_variable(handle, "hidden", &hidden, 1);
            if (hidden || !Annotation(owner, handle, "source").empty()) return;
            Uniform uniform;
            uniform.handle = handle;
            owner->get_uniform_variable_type(handle, &uniform.type, &uniform.rows, &uniform.columns, &uniform.arrayLength);
            uniform.name = ReadString([&](char* out, size_t* size) { owner->get_uniform_variable_name(handle, out, size); });
            uniform.label = LocalizedAnnotation(owner, handle, "ui_label", english);
            if (uniform.label.empty()) uniform.label = uniform.name;
            uniform.tooltip = LocalizedAnnotation(owner, handle, "ui_tooltip", english);
            uniform.category = LocalizedAnnotation(owner, handle, "ui_category", english);
            uniform.text = LocalizedAnnotation(owner, handle, "ui_text", english);
            uniform.uiType = Annotation(owner, handle, "ui_type");
            owner->get_annotation_bool_from_uniform_variable(handle, "noedit", &uniform.readOnly, 1);
            uniform.items = Items(Annotation(owner, handle, "ui_items", true));
            effect.uniforms.push_back(std::move(uniform));
        });
    }
    return effects;
}
inline void ResetEffectValues(Runtime* runtime, const Effect& effect) {
    // Technique states are intentionally untouched; reset only user-controlled values.
    for (const auto& uniform : effect.uniforms) if (!uniform.readOnly) runtime->reset_uniform_value(uniform.handle);
}
} // namespace ReShadeUiDetail
} // namespace DXL
