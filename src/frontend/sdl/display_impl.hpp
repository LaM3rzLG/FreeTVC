// SPDX-License-Identifier: GPL-3.0-or-later
// The two displays behind Display::create().
#pragma once

#include "frontend/sdl/display.hpp"

namespace freetvc::sdl {

// Null with error set when the window or renderer cannot be made
std::unique_ptr<Display> make_sdl_display(const char* title, int width, int height, SDL_WindowFlags flags, std::string& error);
// Null with error set when there is no OpenGL 3.3 core context; the window it tried is gone again
std::unique_ptr<Display> make_gl_display(const char* title, int width, int height, SDL_WindowFlags flags, std::string& error);

}  // namespace freetvc::sdl
