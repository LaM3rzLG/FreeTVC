// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/sdl/display.hpp"

#include <cstdio>

#include "frontend/sdl/display_impl.hpp"

namespace freetvc::sdl {

std::unique_ptr<Display> Display::create(const char* title, int width, int height, SDL_WindowFlags flags, bool want_gl, std::string& error) {
    if (want_gl) {
        std::string why;
        if (auto display = make_gl_display(title, width, height, flags, why)) {
            return display;
        }
        std::fprintf(stderr, "freetvc: no OpenGL 3.3 (%s); SDL's renderer without the picture model\n", why.c_str());
    }
    return make_sdl_display(title, width, height, flags, error);
}

}  // namespace freetvc::sdl
