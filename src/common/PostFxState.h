#pragma once
#include "ColorGradingSettings.h"
#include "JsonScalar.h"
#include "PostFxCatalog.h"
#include <charconv>
#include <string_view>
#include <vector>

namespace DXL {
// A quoted scalar in the existing per-game JSON contains this versioned array.
// Keep shader filenames as data, including UTF-8 and punctuation.
inline std::string SerializePostFxState(const std::vector<PostFxSetting>& effects) {
    std::string out = "[1";
    size_t count = 0;
    for (const auto& effect : effects) {
        if (++count > 64) break;
        out += ",[" + JsonScalar::Quote(effect.file) + "," + (effect.enabled ? "true" : "false") + ",[";
        for (size_t i = 0; i < effect.values.size() && i < 16; ++i) {
            if (i) out += ',';
            char number[64]{};
            const float value = std::isfinite(effect.values[i]) ? effect.values[i] : 0.0f;
            const auto converted = std::to_chars(number, number + sizeof(number), value);
            if (converted.ec == std::errc{}) out.append(number, converted.ptr);
            else out += '0';
        }
        out += "]]";
    }
    return out + ']';
}

inline std::vector<PostFxSetting> DeserializePostFxState(std::string_view text) {
    if (text.size() > 128 * 1024) return {};
    size_t pos = 0;
    const auto white = [&] { while (pos < text.size() && (text[pos] == ' ' || text[pos] == '\t' || text[pos] == '\r' || text[pos] == '\n')) ++pos; };
    const auto take = [&](char ch) { white(); if (pos == text.size() || text[pos] != ch) return false; ++pos; return true; };
    const auto word = [&](std::string_view value) { white(); if (text.substr(pos, value.size()) != value) return false; pos += value.size(); return true; };
    const auto string = [&](std::string& value) {
        white(); if (pos == text.size() || text[pos] != '"') return false;
        const size_t start = pos++;
        bool escaped = false;
        for (; pos < text.size(); ++pos) {
            if (escaped) { escaped = false; continue; }
            if (text[pos] == '\\') { escaped = true; continue; }
            if (text[pos] == '"') { ++pos; return JsonScalar::Decode(text.substr(start, pos - start), value); }
        }
        return false;
    };
    std::vector<PostFxSetting> result;
    if (!take('[') || !word("1")) return {};
    while (!take(']')) {
        if (result.size() == 64 || !take(',') || !take('[')) return {};
        PostFxSetting setting;
        if (!string(setting.file) || !ValidatePostFxFilename(setting.file) || !take(',')) return {};
        if (std::any_of(result.begin(), result.end(), [&](const auto& other) { return PostFxFilenameEqual(other.file, setting.file); })) return {};
        if (word("true")) setting.enabled = true;
        else if (word("false")) setting.enabled = false;
        else return {};
        if (!take(',') || !take('[')) return {};
        white();
        if (!take(']')) {
            for (;;) {
                if (setting.values.size() == 16) return {};
                white(); float number = 0;
                const auto parsed = std::from_chars(text.data() + pos, text.data() + text.size(), number);
                if (parsed.ec != std::errc{} || parsed.ptr == text.data() + pos || !std::isfinite(number)) return {};
                pos = static_cast<size_t>(parsed.ptr - text.data()); setting.values.push_back(number);
                if (take(']')) break;
                if (!take(',')) return {};
            }
        }
        if (!take(']')) return {};
        result.push_back(std::move(setting));
    }
    white();
    return pos == text.size() ? result : std::vector<PostFxSetting>{};
}
} // namespace DXL
