// SPDX-License-Identifier: GPL-3.0-or-later
// Where the GUI draws (design/ui.md "Picture"): OpenGL 3.3 with the picture model, else SDL's renderer without it.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <SDL3/SDL.h>

#include "core/video/framebuffer.hpp"
#include "frontend/sdl/picture.hpp"
#include "frontend/sdl/skin.hpp"
#include "imgui.h"

namespace freetvc::sdl {

class Display {
public:
    virtual ~Display() = default;
    // OpenGL when `want_gl` and the driver has 3.3 core, else SDL's renderer; null (error set) when neither works.
    // Creates the window and sets up Dear ImGui's platform and renderer backends.
    static std::unique_ptr<Display> create(const char* title, int width, int height, SDL_WindowFlags flags, bool want_gl, std::string& error);

    SDL_Window* window() const { return window_; }
    virtual std::string_view name() const = 0;  // for stderr and the settings window
    virtual bool effects() const = 0;            // the picture model runs here

    // ARGB8888 textures, nearest filtering; pixels are rows of `pitch` bytes
    virtual ImTextureID create_texture(int width, int height) = 0;
    virtual void update_texture(ImTextureID texture, const void* pixels, int pitch) = 0;
    virtual void destroy_texture(ImTextureID texture) = 0;
    virtual void set_vsync(bool on) = 0;

    virtual void new_frame() = 0;
    // After ImGui::Render(): clears to `clear`, draws ImGui's lists and shows the frame
    virtual void present(Colour clear) = 0;

    // The picture model (no-ops without effects). set_picture() returns false with error set when its shaders fail.
    virtual bool set_picture(const Picture& picture, std::string& error) {
        (void)picture;
        (void)error;
        return true;
    }
    // Runs the signal passes on `screen` (the framebuffer texture) before ImGui renders; `colour` is the rear switch
    virtual void prepare_picture(ImTextureID screen, bool colour, std::uint64_t frame) {
        (void)screen;
        (void)colour;
        (void)frame;
    }
    // Adds the tube to `draw` over [min, max], showing framebuffer `area`; false when the plain image must be drawn
    virtual bool draw_picture(ImDrawList* draw, ImVec2 min, ImVec2 max, ScreenArea area) {
        (void)draw;
        (void)min;
        (void)max;
        (void)area;
        return false;
    }

protected:
    SDL_Window* window_ = nullptr;
};

}  // namespace freetvc::sdl
