// SPDX-License-Identifier: GPL-3.0-or-later
// The About window (design/ui.md "About window"): the L@M3rZ logo, version and build, credits, a raster bar and a
// sine scroller in the TVC's own colours.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "frontend/sdl/display.hpp"
#include "frontend/sdl/skin.hpp"
#include "frontend/sdl/skin_style.hpp"
#include "imgui.h"

namespace freetvc::sdl {

class AboutView {
public:
    void open() {
        open_ = true;
        raise_ = true;
    }
    void close() { open_ = false; }
    bool is_open() const { return open_; }
    // Over everything else while open; `logo` is the PNG, `renderer` what draws. True while it holds the keyboard.
    bool draw(Display& display, const std::filesystem::path& logo, std::string_view renderer, const Skin& skin, const SkinFonts& fonts,
              float scale);
    // Frees the logo (before the display goes)
    void release(Display& display);
    // Version, build and runtime lines as plain English text, for bug reports
    static std::string build_text(std::string_view renderer);

private:
    bool load_logo(Display& display, const std::filesystem::path& path, int width, int height);

    bool open_ = false;
    bool raise_ = false;  // bring both windows to the front once: focusing every frame would cancel clicks
    ImTextureID logo_ = ImTextureID_Invalid;
    SDL_Surface* source_ = nullptr;  // the PNG as ARGB8888, resampled whenever the logo's size on screen changes
    int logo_width_ = 0;
    int logo_height_ = 0;
    bool logo_missing_ = false;
    double copied_at_ = -10.0;
    float rest_height_ = 0.0f;  // the window's height without the logo, from the frame before
};

}  // namespace freetvc::sdl
