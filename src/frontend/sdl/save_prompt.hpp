// SPDX-License-Identifier: GPL-3.0-or-later
// The game save prompt (design/ui.md "Game save prompt"): the window dims and asks for the save's name in its middle.
#pragma once

#include <array>
#include <functional>
#include <string>
#include <string_view>

#include "frontend/sdl/skin.hpp"
#include "frontend/sdl/skin_style.hpp"

namespace freetvc::sdl {

class SavePrompt {
public:
    // Opens with `name` in the field, selected; `game` is shown as where the save goes
    void open(std::string_view name, std::string_view game);
    bool is_open() const { return open_; }
    // Over everything else while open, its text as large as the strip's menus. True once the user saved: the name is in
    // `name`. A name `exists` knows asks first whether to replace that save. Closed by Save, Enter, Cancel or Escape.
    bool draw(const Skin& skin, const SkinFonts& fonts, float scale, const std::function<bool(const std::string&)>& exists,
              std::string& name);

private:
    bool open_ = false;
    bool focus_ = false;    // the field takes the keyboard once, when the prompt opens
    bool replace_ = false;  // asking whether to replace the save of that name
    std::string game_;
    std::array<char, 128> text_{};
};

}  // namespace freetvc::sdl
