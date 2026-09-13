#pragma once
#include "../common/ColorGradingParameters.h"
#include "../common/PostFxCatalog.h"
#include "ColorGrading.h"
#include "imgui.h"
#include <filesystem>
#include <cwctype>
#include <vector>

namespace DXL {
// No settings-file writes here: the owning overlay debounces persistence with
// NR controls under its existing state lock.
class ColorGradingUi {
public:
    bool Draw(ColorGradingSettings& settings, HMODULE module, bool english) {
        const auto text = [english](const char* zh, const char* en) { return english ? en : zh; };
        bool changed = false;
        ImGui::TextWrapped("%s", text("独立开关，实时生效并按游戏自动保存。关闭或保持默认值的项目不参与处理。",
            "Changes apply live and save per game. Disabled or neutral effects skip processing."));
        if (ImGui::CollapsingHeader(text("基础调色 / Basic grading", "Basic grading"), ImGuiTreeNodeFlags_DefaultOpen))
            changed |= DrawRows(settings, 0, 8, english, "BasicColor");

        ImGui::TextWrapped("%s", text("基础调色保留游戏内处理位置，可能受游戏后续曝光和色调映射影响。以下后处理作用于最终画面，也会影响游戏 UI。",
            "Basic grading stays in the game processing path and may be affected by later exposure and tone mapping. Post-processing below affects the final image, including game UI."));

        if (ImGui::CollapsingHeader("LUT", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (!_scanned) Refresh(module);
            ImGui::SetNextItemWidth(-1);
            const char* preview = settings.lutFile.empty() ? text("选择 PNG LUT...", "Choose a PNG LUT...") : settings.lutFile.c_str();
            if (ImGui::BeginCombo("##LutFile", preview)) {
                for (const auto& name : _files) {
                    const bool selected = name == settings.lutFile;
                    if (ImGui::Selectable(name.c_str(), selected)) { settings.lutFile = name; changed = true; }
                    if (selected) ImGui::SetItemDefaultFocus();
                }
                if (_files.empty()) ImGui::TextDisabled("%s", text("lut 文件夹中没有 PNG LUT", "No PNG LUTs in the lut folder"));
                ImGui::EndCombo();
            }
            if (ImGui::BeginTable("LutControl", 4, ImGuiTableFlags_SizingStretchProp)) {
                const float unit = ImGui::GetFontSize();
                ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch, 1);
                ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
                ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, unit * 7);
                ImGui::TableSetupColumn("Reset", ImGuiTableColumnFlags_WidthFixed, unit * 3.6f);
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(text("LUT 强度 / Intensity", "LUT intensity"));
                ImGui::TableNextColumn(); changed |= ImGui::Checkbox("##lutEnabled", &settings.lutEnabled);
                ImGui::TableNextColumn(); ImGui::BeginDisabled(!settings.lutEnabled); ImGui::SetNextItemWidth(-1);
                changed |= ImGui::SliderFloat("##lutIntensity", &settings.lutIntensity, 0, 1, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::EndDisabled(); ImGui::TableNextColumn();
                if (ImGui::Button(text("重置##lut", "Reset##lut"), ImVec2(-1, 0))) { settings.lutIntensity = 1; changed = true; }
                ImGui::EndTable();
            }
            if (ImGui::Button(text("刷新 LUT / Refresh LUTs", "Refresh LUTs"))) { Refresh(module); ++settings.lutReloadRevision; changed = true; }
            ImGui::TextWrapped("%s", text("将 PNG 颜色查找表放入工具目录的 lut 文件夹。支持条带及网格排列；制作方法见 lut/README.md。",
                "Place PNG color lookup tables in the tool's lut folder. Strips and tiled grids are supported; see lut/README.md."));
        }
        if (ImGui::CollapsingHeader(text("常用滤镜 / Filters", "Filters"), ImGuiTreeNodeFlags_DefaultOpen)) {
            changed |= DrawRows(settings, 8, ColorGradingParameters.size(), english, "ColorFilters");
            changed |= DrawBloom(settings, english);
        }
        changed |= DrawFx(settings, module, english);
        // Ctrl+click numeric entry must not persist NaN/Inf into JSON.
        for (const auto& entry : ColorGradingParameters) {
            auto& value = (settings.*entry.member).value;
            const float normalized = std::isfinite(value) ? std::clamp(value, entry.minimum, entry.maximum) : entry.neutral;
            if (!std::isfinite(value) || normalized != value) { value = normalized; changed = true; }
        }
        for (const auto& entry : BloomParameters) {
            auto& value = settings.*entry.member;
            const float normalized = std::isfinite(value) ? std::clamp(value, entry.minimum, entry.maximum) : entry.defaultValue;
            if (!std::isfinite(value) || normalized != value) { value = normalized; changed = true; }
        }
        if (!std::isfinite(settings.lutIntensity)) { settings.lutIntensity = 1; changed = true; }
        else settings.lutIntensity = std::clamp(settings.lutIntensity, 0.0f, 1.0f);
        return changed;
    }
private:
    static bool DrawBloom(ColorGradingSettings& settings, bool english) {
        bool changed = false;
        if (!ImGui::TreeNode(english ? "Bloom details" : "柔光细节 / Bloom details")) return false;
        if (!settings.bloom.enabled) ImGui::TextDisabled("%s", english ? "Enable Bloom above to apply these settings." : "开启上方的柔光开关后生效。");
        if (ImGui::BeginTable("BloomDetails", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
            const float unit = ImGui::GetFontSize();
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch, 1);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, unit * 7);
            ImGui::TableSetupColumn("Reset", ImGuiTableColumnFlags_WidthFixed, unit * 3.6f);
            for (const auto& entry : BloomParameters) {
                ImGui::PushID(entry.key);
                ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(english ? entry.english : entry.chinese);
                if (ImGui::IsItemHovered()) {
                    ImGui::BeginTooltip(); ImGui::PushTextWrapPos(unit * 28);
                    ImGui::TextUnformatted(english ? entry.hintEnglish : entry.hintChinese);
                    ImGui::PopTextWrapPos(); ImGui::EndTooltip();
                }
                ImGui::TableNextColumn(); ImGui::SetNextItemWidth(-1);
                ImGui::BeginDisabled(!settings.bloom.enabled);
                changed |= ImGui::SliderFloat("##value", &(settings.*entry.member), entry.minimum, entry.maximum, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::EndDisabled(); ImGui::TableNextColumn();
                if (ImGui::Button(english ? "Reset" : "重置", ImVec2(-1, 0))) { settings.*entry.member = entry.defaultValue; changed = true; }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        ImGui::TreePop(); return changed;
    }
    using FxStamp = std::vector<std::pair<std::string, uint64_t>>;
    static FxStamp ScanFxStamp(HMODULE module) {
        FxStamp result;
        try {
            const auto root = PostFxRoot(module);
            const DWORD attributes = GetFileAttributesW(root.c_str());
            if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return result;
            for (const auto& file : std::filesystem::directory_iterator(root)) {
                const auto name = PostFxUtf8(file.path().filename().wstring());
                if (!ValidatePostFxFilename(name)) continue;
                WIN32_FILE_ATTRIBUTE_DATA info{};
                if (GetFileAttributesExW(file.path().c_str(), GetFileExInfoStandard, &info))
                    result.emplace_back(name, ((uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) |
                        info.ftLastWriteTime.dwLowDateTime) ^ (uint64_t(info.nFileSizeLow) * 1099511628211ull) ^ info.dwFileAttributes);
            }
            std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return PostFxFilenameLess(a.first, b.first); });
            if (result.size() > 64) result.resize(64);
        } catch (...) {}
        return result;
    }
    bool DrawFx(ColorGradingSettings& settings, HMODULE module, bool english) {
        const auto text = [english](const char* zh, const char* en) { return english ? en : zh; };
        bool changed = false;
        const bool expanded = ImGui::CollapsingHeader(text("扩展后处理 / Post-processing extensions", "Post-processing extensions"), ImGuiTreeNodeFlags_DefaultOpen);
        if (!expanded) return false;
        ImGui::TextWrapped("%s", text("将 .fx 及其依赖放入 post-processing 文件夹。这里显示 DXL 格式效果，ReShade 格式效果显示在下方的自定义 FX 中。",
            "Place .fx files and their dependencies in post-processing. DXL-format effects appear here; native ReShade effects appear in Custom FX below."));
        const bool refresh = ImGui::Button(text("刷新效果 / Refresh effects", "Refresh effects"));
        const auto now = GetTickCount64();
        if (refresh || !_fxScanned || now - _fxLastScan >= 2000) {
            auto stamp = ScanFxStamp(module);
            if (refresh || !_fxScanned || stamp != _fxStamp) {
                _fxDefinitions = EnumeratePostFx(module); _fxStamp = std::move(stamp);
                ++settings.fxReloadRevision;
            }
            _fxScanned = true; _fxLastScan = now;
        }
        if (_fxDefinitions.empty()) ImGui::TextDisabled("%s", text("尚未找到 DXL 格式 .fx 文件", "No DXL-format .fx files found"));
        for (const auto& definition : _fxDefinitions) {
            auto found = std::find_if(settings.fx.begin(), settings.fx.end(), [&](const auto& item) { return PostFxFilenameEqual(item.file, definition.file); });
            if (found == settings.fx.end()) {
                if (settings.fx.size() >= 64) {
                    const auto obsolete = std::find_if(settings.fx.begin(), settings.fx.end(), [&](const auto& item) {
                        return !item.enabled && std::none_of(_fxDefinitions.begin(), _fxDefinitions.end(), [&](const auto& file) { return PostFxFilenameEqual(item.file, file.file); });
                    });
                    if (obsolete != settings.fx.end()) { settings.fx.erase(obsolete); changed = true; }
                }
                if (settings.fx.size() >= 64) { ImGui::TextDisabled("%s", text("最多保存 64 个扩展效果。", "Up to 64 effect settings can be saved.")); break; }
                settings.fx.push_back({definition.file, false, {}}); found = std::prev(settings.fx.end()); changed = true;
            }
            auto& effect = *found;
            const size_t oldSize = effect.values.size();
            if (oldSize < definition.parameters.size()) {
                effect.values.resize(definition.parameters.size());
                for (size_t i = oldSize; i < definition.parameters.size(); ++i) effect.values[i] = definition.parameters[i].defaultValue;
                changed = true;
            }
            ImGui::PushID(definition.file.c_str());
            if (ImGui::CollapsingHeader(definition.file.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                if (ImGui::BeginTable("FxValues", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
                    const float unit = ImGui::GetFontSize();
                    ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch, 1);
                    ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, unit * 8.5f);
                    ImGui::TableSetupColumn("Reset", ImGuiTableColumnFlags_WidthFixed, unit * 3.6f);
                    ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted((english ? definition.titleEn : definition.titleZh).c_str());
                    ImGui::TableNextColumn();
                    changed |= ImGui::Checkbox(text("启用 / Enable", "Enable"), &effect.enabled);
                    ImGui::TableNextColumn();
                    if (ImGui::Button(text("重置", "Reset"), ImVec2(-1, 0))) {
                        for (size_t i = 0; i < definition.parameters.size(); ++i) effect.values[i] = definition.parameters[i].defaultValue;
                        changed = true;
                    }
                    for (size_t i = 0; i < definition.parameters.size(); ++i) {
                        const auto& parameter = definition.parameters[i];
                        auto& value = effect.values[i];
                        const float normalized = std::isfinite(value) ? std::clamp(value, parameter.minimum, parameter.maximum) : parameter.defaultValue;
                        if (!std::isfinite(value) || value != normalized) { value = normalized; changed = true; }
                        ImGui::PushID(parameter.key.c_str());
                        ImGui::TableNextRow(); ImGui::TableNextColumn(); ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted((english ? parameter.titleEn : parameter.titleZh).c_str());
                        ImGui::TableNextColumn(); ImGui::BeginDisabled(!effect.enabled); ImGui::SetNextItemWidth(-1);
                        changed |= ImGui::SliderFloat("##value", &value, parameter.minimum, parameter.maximum, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                        ImGui::EndDisabled(); ImGui::TableNextColumn();
                        if (ImGui::Button(text("重置", "Reset"), ImVec2(-1, 0))) { value = parameter.defaultValue; changed = true; }
                        // A manual numeric entry can contain a non-finite value in this same frame.
                        if (!std::isfinite(value)) { value = parameter.defaultValue; changed = true; }
                        else value = std::clamp(value, parameter.minimum, parameter.maximum);
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                if (!definition.error.empty()) ImGui::TextWrapped("%s", definition.error.c_str());
            }
            ImGui::PopID();
        }
        // Keep removed files' saved values, but let players turn an enabled missing effect off.
        for (auto& effect : settings.fx) {
            if (!effect.enabled || std::any_of(_fxDefinitions.begin(), _fxDefinitions.end(), [&](const auto& item) { return PostFxFilenameEqual(item.file, effect.file); })) continue;
            ImGui::PushID(effect.file.c_str());
            ImGui::TextUnformatted(effect.file.c_str()); ImGui::SameLine();
            changed |= ImGui::Checkbox(text("文件未找到 / File missing", "File missing"), &effect.enabled);
            ImGui::PopID();
        }
        return changed;
    }
    static bool DrawRows(ColorGradingSettings& settings, size_t begin, size_t end, bool english, const char* id) {
        bool changed = false;
        if (ImGui::BeginTable(id, 4, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_BordersInnerH)) {
            const float unit = ImGui::GetFontSize();
            ImGui::TableSetupColumn("Effect", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("On", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFrameHeight());
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, unit * 7.0f);
            ImGui::TableSetupColumn("Reset", ImGuiTableColumnFlags_WidthFixed, unit * 3.6f);
            for (size_t i = begin; i < end; ++i) {
                const auto& entry = ColorGradingParameters[i];
                auto& control = settings.*entry.member;
                ImGui::PushID(entry.key);
                ImGui::TableNextRow(); ImGui::TableNextColumn();
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(english ? entry.english : entry.chinese);
                ImGui::TableNextColumn();
                changed |= ImGui::Checkbox("##on", &control.enabled);
                ImGui::TableNextColumn();
                ImGui::BeginDisabled(!control.enabled); ImGui::SetNextItemWidth(-1);
                changed |= ImGui::SliderFloat("##value", &control.value, entry.minimum, entry.maximum, "%.2f", ImGuiSliderFlags_AlwaysClamp);
                ImGui::EndDisabled();
                ImGui::TableNextColumn();
                if (ImGui::Button(english ? "Reset" : "重置", ImVec2(-1, 0))) {
                    control.value = entry.neutral; changed = true;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        return changed;
    }
    void Refresh(HMODULE module) {
        _files.clear(); _scanned = true;
        wchar_t path[32768]{};
        const auto length = GetModuleFileNameW(module, path, DWORD(std::size(path)));
        if (!length || length >= std::size(path)) return;
        std::error_code ec;
        const auto root = std::filesystem::path(path).parent_path() / L"lut";
        std::filesystem::directory_iterator it(root, ec), last;
        for (; !ec && it != last; it.increment(ec)) {
            if (!it->is_regular_file(ec)) continue;
            const auto utf8 = it->path().filename().u8string();
            std::string name(reinterpret_cast<const char*>(utf8.data()), utf8.size());
            if (ColorGrading::ValidateLutFilename(name)) _files.push_back(std::move(name));
        }
        std::sort(_files.begin(), _files.end());
    }
    bool _scanned = false;
    std::vector<std::string> _files;
    bool _fxScanned = false;
    uint64_t _fxLastScan = 0;
    FxStamp _fxStamp;
    std::vector<PostFxDefinition> _fxDefinitions;
};
} // namespace DXL
