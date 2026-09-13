#pragma once
#include "../src/core/ReShadeUi.h"
#include "imgui_internal.h"
#include <cstdio>
#include <stdexcept>

// Exercise production controls against actual compiled ReShade uniforms. The GPU
// fixture separately renders shader pixels; this checks the UI interaction path.
inline void VerifyReShadeUiDraw(reshade::api::effect_runtime* runtime) {
    using namespace DXL::ReShadeUiDetail;
    const auto require = [](bool okay, const char* message) { if (!okay) throw std::runtime_error(message); };
    struct Context {
        ImGuiContext* previous = ImGui::GetCurrentContext();
        ImGuiContext* created = ImGui::CreateContext();
        ~Context() { ImGui::DestroyContext(created); ImGui::SetCurrentContext(previous); }
    } context;
    auto& io = ImGui::GetIO();
    io.IniFilename = nullptr; io.DisplaySize = ImVec2(1000, 2200); io.DeltaTime = 1.0f / 60;
    io.Fonts->AddFontDefault();
    unsigned char* pixels = nullptr; int width = 0, height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height); io.Fonts->SetTexID(ImTextureID(1));
    const auto effects = Enumerate(runtime, true);
    const auto effect = std::find_if(effects.begin(), effects.end(), [](const auto& item) { return item.file == "90-DXL-UI-Validation.fx"; });
    require(effect != effects.end(), "Real ReShade effect available for production UI");
    const auto uniform = std::find_if(effect->uniforms.begin(), effect->uniforms.end(), [](const auto& item) { return item.name == "Strength"; });
    require(uniform != effect->uniforms.end(), "Real uniform available for production UI");
    const auto technique = runtime->find_technique(effect->file.c_str(), "UiPrimary");
    runtime->set_technique_state(technique, true); runtime->set_uniform_value_float(uniform->handle, 1.5f);
    ImVec2 reset;
    const auto frame = [&](ImVec2 mouse, bool pressed, bool fullUi, bool english) {
        io.MousePos = mouse; io.MouseDown[0] = pressed;
        ImGui::NewFrame(); ImGui::SetNextWindowPos(ImVec2(0, 0)); ImGui::SetNextWindowSize(ImVec2(960, 2100));
        ImGui::Begin("ReShade production controls", nullptr, ImGuiWindowFlags_NoSavedSettings);
        bool changed = false;
        if (fullUi) changed = DXL::DrawReShadeUi(runtime, english);
        else if (ImGui::BeginTable("SingleUniform", 3, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Label", ImGuiTableColumnFlags_WidthStretch, 1);
            ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed, 180);
            ImGui::TableSetupColumn("Reset", ImGuiTableColumnFlags_WidthFixed, 70);
            changed = DrawUniform(runtime, *uniform, true);
            auto* table = ImGui::GetCurrentTable(); const auto& column = table->Columns[2];
            reset = ImVec2((column.WorkMinX + column.WorkMaxX) * .5f, (table->RowPosY1 + table->RowPosY2) * .5f);
            ImGui::EndTable();
        }
        ImGui::End(); ImGui::Render();
        require(ImGui::GetDrawData()->TotalVtxCount > 0, "Production ReShade UI generates draw commands");
        return changed;
    };
    frame(ImVec2(-100, -100), false, false, true); frame(ImVec2(-100, -100), false, false, true);
    const auto position = reset;
    frame(position, false, false, true); frame(position, true, false, true);
    require(frame(position, false, false, true), "Clicking production Reset reports a change");
    float value = 0; runtime->get_uniform_value_float(uniform->handle, &value, 1);
    require(std::abs(value - .25f) < .0001f && runtime->get_technique_state(technique), "Production reset click updates actual uniform and preserves technique");
    frame(ImVec2(-100, -100), false, true, true);
    frame(ImVec2(-100, -100), false, true, false);
    runtime->set_technique_state(technique, false);
    std::puts("PASS: actual compiled ReShade uniform controls, reset mouse interaction and bilingual production UI draw");
}
