// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/sdl/skin_style.hpp"

#include <system_error>

namespace freetvc::sdl {

namespace {

ImVec4 vec(Colour c, float alpha = 1.0f) {
    return {c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f * alpha};
}

ImFont* add_font(const std::string& file, const std::filesystem::path& skin_dir, const std::filesystem::path& font_dir,
                 std::string& warning) {
    std::error_code ec;
    for (const auto& dir : {skin_dir, font_dir}) {
        const auto path = dir / file;
        if (std::filesystem::is_regular_file(path, ec)) {
            if (ImFont* font = ImGui::GetIO().Fonts->AddFontFromFileTTF(path.string().c_str())) {
                return font;
            }
        }
    }
    if (warning.empty()) {
        warning = "font " + file + " not found; using the built-in font";
    }
    return nullptr;
}

}  // namespace

ImU32 to_imgui(Colour colour) {
    return IM_COL32(colour.r, colour.g, colour.b, colour.a);
}

void apply_style(const Skin& skin, float scale, ImGuiStyle& style) {
    style = ImGuiStyle();
    style.FontSizeBase = skin.font_ui_size;
    style._NextFrameFontSizeBase = skin.font_ui_size;  // as ImGui's style editor does: NewFrame takes the new size
    style.FontScaleDpi = scale;
    style.WindowRounding = style.ChildRounding = style.FrameRounding = style.PopupRounding = skin.rounding;
    style.ScrollbarRounding = style.GrabRounding = style.TabRounding = skin.rounding;
    style.WindowBorderSize = style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowPadding = {10.0f, 8.0f};
    style.FramePadding = {8.0f, 4.0f};
    style.ItemSpacing = {8.0f, 4.0f};
    style.SeparatorTextBorderSize = 1.0f;
    style.ScaleAllSizes(scale);

    const Colour notice = skin.role(Role::Notice).on_case;
    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = vec(skin.label);
    c[ImGuiCol_TextDisabled] = vec(skin.muted);
    c[ImGuiCol_WindowBg] = c[ImGuiCol_ChildBg] = c[ImGuiCol_PopupBg] = vec(skin.deck);
    c[ImGuiCol_MenuBarBg] = c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgCollapsed] = vec(skin.deck);
    c[ImGuiCol_TitleBgActive] = vec(skin.rib);
    c[ImGuiCol_Border] = vec(skin.groove);
    c[ImGuiCol_BorderShadow] = {0, 0, 0, 0};
    c[ImGuiCol_FrameBg] = c[ImGuiCol_Button] = vec(skin.key);
    c[ImGuiCol_FrameBgHovered] = c[ImGuiCol_ButtonHovered] = vec(skin.key_hover);
    c[ImGuiCol_FrameBgActive] = c[ImGuiCol_ButtonActive] = vec(skin.key_active);
    c[ImGuiCol_Header] = vec(skin.rib);
    c[ImGuiCol_HeaderHovered] = vec(skin.key_hover);
    c[ImGuiCol_HeaderActive] = vec(skin.key_active);
    c[ImGuiCol_Separator] = c[ImGuiCol_SeparatorHovered] = c[ImGuiCol_SeparatorActive] = vec(skin.rib);
    c[ImGuiCol_ScrollbarBg] = vec(skin.deck);
    c[ImGuiCol_ScrollbarGrab] = c[ImGuiCol_SliderGrab] = vec(skin.key);
    c[ImGuiCol_ScrollbarGrabHovered] = vec(skin.key_hover);
    c[ImGuiCol_ScrollbarGrabActive] = c[ImGuiCol_SliderGrabActive] = vec(skin.key_active);
    c[ImGuiCol_CheckMark] = vec(skin.label);
    c[ImGuiCol_CheckboxSelectedBg] = vec(skin.key_active);
    c[ImGuiCol_ResizeGrip] = c[ImGuiCol_ResizeGripHovered] = c[ImGuiCol_ResizeGripActive] = vec(skin.rib);
    c[ImGuiCol_InputTextCursor] = vec(skin.label);
    c[ImGuiCol_Tab] = c[ImGuiCol_TabDimmed] = vec(skin.deck);
    c[ImGuiCol_TabHovered] = vec(skin.key_hover);
    c[ImGuiCol_TabSelected] = vec(skin.rib);  // only the focused panel's tab is lit (Bench)
    c[ImGuiCol_TabDimmedSelected] = vec(skin.deck);
    c[ImGuiCol_TabSelectedOverline] = vec(skin.label);
    c[ImGuiCol_TabDimmedSelectedOverline] = vec(skin.rib);
    c[ImGuiCol_DockingPreview] = vec(skin.key_active, 0.6f);
    c[ImGuiCol_DockingEmptyBg] = vec(skin.frame);
    c[ImGuiCol_PlotLines] = c[ImGuiCol_PlotHistogram] = vec(skin.label);
    c[ImGuiCol_PlotLinesHovered] = c[ImGuiCol_PlotHistogramHovered] = vec(notice);
    c[ImGuiCol_TableHeaderBg] = vec(skin.rib);
    c[ImGuiCol_TableBorderStrong] = c[ImGuiCol_TableBorderLight] = vec(skin.groove);
    c[ImGuiCol_TableRowBg] = {0, 0, 0, 0};
    c[ImGuiCol_TableRowBgAlt] = vec(skin.rib, 0.35f);
    c[ImGuiCol_TextLink] = vec(skin.label);
    c[ImGuiCol_TextSelectedBg] = vec(skin.key_active);
    c[ImGuiCol_TreeLines] = vec(skin.rib);
    c[ImGuiCol_DragDropTarget] = c[ImGuiCol_DragDropTargetBg] = vec(notice, 0.5f);
    c[ImGuiCol_UnsavedMarker] = vec(notice);
    c[ImGuiCol_NavCursor] = c[ImGuiCol_NavWindowingHighlight] = vec(skin.label);
    c[ImGuiCol_NavWindowingDimBg] = c[ImGuiCol_ModalWindowDimBg] = vec(skin.groove, 0.6f);
}

SkinFonts load_fonts(const Skin& skin, const std::filesystem::path& skin_dir, const std::filesystem::path& font_dir,
                     std::string& warning) {
    ImFontAtlas* atlas = ImGui::GetIO().Fonts;
    atlas->Clear();  // fonts and texture; ClearFonts() alone leaves stale glyph pixels in the atlas
    SkinFonts fonts;
    fonts.ui = add_font(skin.font_ui, skin_dir, font_dir, warning);
    fonts.strong = add_font(skin.font_strong, skin_dir, font_dir, warning);
    fonts.data = add_font(skin.font_data, skin_dir, font_dir, warning);
    if (!fonts.ui || !fonts.strong || !fonts.data) {
        ImFont* fallback = atlas->AddFontDefault();
        fonts.ui = fonts.ui ? fonts.ui : fallback;
        fonts.strong = fonts.strong ? fonts.strong : fonts.ui;
        fonts.data = fonts.data ? fonts.data : fallback;
    }
    ImGui::GetIO().FontDefault = fonts.ui;
    return fonts;
}

}  // namespace freetvc::sdl
