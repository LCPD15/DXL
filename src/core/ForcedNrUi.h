#pragma once
#include "imgui.h"
namespace DXL {
inline bool DrawForcedNrToggle(bool& enabled, bool english) {
    const bool changed = ImGui::Checkbox(english ? "Force compatibility" : "强行兼容", &enabled);
    ImGui::SameLine();
    ImGui::TextWrapped("%s", english
        ? "Try only if toggling NR in-game has no effect."
        : "仅当游戏内开关NR无效时尝试使用。");
    return changed;
}
}
