// SPDX-License-Identifier: GPL-3.0-or-later
// A Skin turned into Dear ImGui style colours, sizes and fonts.
#pragma once

#include <filesystem>
#include <string>

#include "frontend/sdl/skin.hpp"
#include "imgui.h"

namespace freetvc::sdl {

ImU32 to_imgui(Colour colour);

// Sets every ImGuiStyle colour and size from the skin, sizes times the display content scale.
void apply_style(const Skin& skin, float scale, ImGuiStyle& style);

struct SkinFonts {
    ImFont* ui = nullptr;
    ImFont* strong = nullptr;
    ImFont* data = nullptr;
};

// Replaces every font in the atlas. Fonts are looked up in skin_dir, then font_dir; a missing one falls back to
// ImGui's built-in font and is named in warning.
SkinFonts load_fonts(const Skin& skin, const std::filesystem::path& skin_dir, const std::filesystem::path& font_dir,
                     std::string& warning);

}  // namespace freetvc::sdl
