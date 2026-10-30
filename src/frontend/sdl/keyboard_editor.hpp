// SPDX-License-Identifier: GPL-3.0-or-later
// The keyboard layout editor (design/ui.md "Keyboard editor"): the TVC's keyboard drawn along the bottom of the window,
// each key with the host keys that press it; click a key, then press a host key to bind it.
#pragma once

#include <optional>
#include <string_view>

#include <SDL3/SDL.h>

#include "frontend/sdl/keymap.hpp"
#include "frontend/sdl/skin.hpp"
#include "frontend/sdl/skin_style.hpp"
#include "imgui.h"

namespace freetvc::sdl {

struct KeyboardEditorActions {
    std::optional<KeyBinding> bind;  // a host key pressed for the waiting key
    std::optional<MatrixKey> clear;  // right click: the key loses its host keys
    bool reset = false;              // back to the shipped layout
    bool close = false;
    bool keys_held = false;          // keys stay out of the TVC while the editor has them
};

class KeyboardEditor {
public:
    void open() { open_ = true; }
    void close() {
        open_ = false;
        waiting_.reset();
    }
    bool is_open() const { return open_; }
    bool waiting() const { return open_ && waiting_.has_value(); }
    // The panel's height, which Play's screen and Bench's dock space give up; 0 while closed
    float height(const Skin& skin, float scale) const;
    // While waiting: takes a key-down; false for the reserved host keys (F9-F12, Pause), which keep their use
    bool take_key(SDL_Scancode scancode);
    // Across the window's width minus `right` (the settings sheet), just above the strip
    KeyboardEditorActions draw(const Keymap& keymap, bool positional, bool edited, const Skin& skin, const SkinFonts& fonts, float scale,
                               float right);

private:
    bool open_ = false;
    std::optional<MatrixKey> waiting_;
    std::optional<KeyBinding> taken_;
};

}  // namespace freetvc::sdl
