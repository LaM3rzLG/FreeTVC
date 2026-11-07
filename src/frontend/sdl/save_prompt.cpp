// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/sdl/save_prompt.hpp"

#include <algorithm>
#include <cfloat>

#include "frontend/sdl/text.hpp"
#include "imgui.h"

namespace freetvc::sdl {

namespace {

ImVec4 vec(Colour c) { return {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f}; }

}  // namespace

void SavePrompt::open(std::string_view name, std::string_view game) {
    text_.fill('\0');
    std::copy_n(name.begin(), std::min(name.size(), text_.size() - 1), text_.begin());
    game_ = game;
    open_ = true;
    focus_ = true;
    replace_ = false;
}

bool SavePrompt::draw(const Skin& skin, const SkinFonts& fonts, float scale, const std::function<bool(const std::string&)>& exists,
                      std::string& name) {
    if (!open_) {
        return false;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav;
    // The machine stands still under a dimmed window
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 170));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##save_backdrop", nullptr, kFlags | ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoBringToFrontOnFocus);
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    // The strip menus' text size
    const float text_px = skin.font_strip_size > 0.0f ? skin.font_strip_size : skin.font_ui_size;
    ImGui::PushFont(fonts.ui, text_px);
    const float width = std::max(360.0f * scale, ImGui::CalcTextSize("M").x * 30.0f);
    const float pad = 16.0f * scale;
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSize({width, 0.0f});
    if (focus_) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::PushStyleColor(ImGuiCol_WindowBg, vec(skin.deck));
    ImGui::PushStyleColor(ImGuiCol_Border, vec(skin.rib));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {pad, pad});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##save_prompt", nullptr, kFlags);
    const float inner = width - 2.0f * pad;
    const float button = (inner - ImGui::GetStyle().ItemSpacing.x) * 0.5f;

    ImGui::PushFont(fonts.strong, text_px);
    const char* title = tr("Game save");
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner - ImGui::CalcTextSize(title).x) * 0.5f);
    ImGui::TextUnformatted(title);
    ImGui::PopFont();
    ImGui::TextDisabled("%s", game_.c_str());
    ImGui::Spacing();

    bool save = false;
    bool cancel = false;
    if (!replace_) {
        ImGui::TextUnformatted(tr("Name of the save:"));
        if (focus_) {
            ImGui::SetKeyboardFocusHere();
            focus_ = false;
        }
        ImGui::SetNextItemWidth(inner);
        const bool entered = ImGui::InputText("##save_name", text_.data(), text_.size(),
                                              ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
        ImGui::Spacing();
        const bool named = text_[0] != '\0';
        ImGui::BeginDisabled(!named);
        const bool chosen = ImGui::Button(tr("Save"), {button, 0.0f}) || (entered && named);
        ImGui::EndDisabled();
        ImGui::SameLine();
        cancel = ImGui::Button(tr("Cancel"), {button, 0.0f}) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
        if (chosen && exists(text_.data())) {
            replace_ = true;
        } else {
            save = chosen;
        }
    } else {
        ImGui::TextWrapped("%s", tr_format("A save named %s is already there. Replace it?", text_.data()).c_str());
        ImGui::Spacing();
        save = ImGui::Button(tr("Replace"), {button, 0.0f}) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
               ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false);
        ImGui::SameLine();
        if (ImGui::Button(tr("Cancel"), {button, 0.0f}) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
            replace_ = false;  // back to the name
            focus_ = true;
        }
    }
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    ImGui::PopFont();

    if (save) {
        name = text_.data();
        open_ = false;
        return true;
    }
    if (cancel) {
        open_ = false;
    }
    return false;
}

}  // namespace freetvc::sdl
