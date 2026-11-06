// SPDX-License-Identifier: GPL-3.0-or-later
// Pieces Play and Bench share: the screen picture and the deck rows (status strip, run bar) (design/ui.md).
#pragma once

#include <functional>
#include <span>
#include <string_view>

#include "core/video/framebuffer.hpp"
#include "frontend/sdl/skin.hpp"
#include "frontend/sdl/skin_style.hpp"
#include "imgui.h"

namespace freetvc::sdl {

inline ImVec2 operator+(ImVec2 a, ImVec2 b) { return {a.x + b.x, a.y + b.y}; }
inline ImVec2 operator-(ImVec2 a, ImVec2 b) { return {a.x - b.x, a.y - b.y}; }

ImU32 with_alpha(Colour c, float alpha);

// Where the picture landed and which framebuffer area it shows.
struct ScreenRect {
    ImVec2 min;
    ImVec2 size;
    ScreenArea area;
    // Window position of framebuffer pixel (x, y), which may lie outside the picture.
    ImVec2 at(float x, float y) const;
};

// Draws the tube over a rectangle instead of the plain image; false when it does not
using PictureHook = std::function<bool(ImDrawList* draw, ImVec2 min, ImVec2 max, ScreenArea area)>;

// The largest picture of the framebuffer area that fits, centred, at the PAL pixel aspect; in full raster mode the TV area is outlined
ScreenRect draw_screen(ImDrawList* draw, ImTextureID screen, bool full_raster, ImVec2 min, ImVec2 size, Colour outline,
                       const PictureHook* picture = nullptr);

// A row of deck items in the current window: text on the deck, the key colour under the mouse
class DeckRow {
public:
    DeckRow(const Skin& skin, float height) : skin_(skin), height_(height) {}

    // Disabled items are drawn in the muted colour and never report a click; local_x > 0 places the item there.
    bool button(const char* id, const char* text, ImFont* font, float size, Colour colour, bool enabled = true,
                float local_x = 0.0f);
    void text(const char* text, ImFont* font, float size, Colour colour);
    void divider();
    // The four expansion slots; a slot with a card (its name, empty = none) is lit in the lettering colour; true when clicked
    bool slot_ribs(float scale, std::span<const std::string_view> cards);
    // A square tape-transport key: a play triangle, or a stop square when `stop`
    bool transport(const char* id, bool stop, Colour colour);
    // The REC key: corner marks, a dot (`dot` colour; dimmed when `lit` is false) and REC
    bool rec(const char* id, ImFont* font, float size, Colour colour, Colour dot, bool lit);
    // The machine key: a small TVC seen from the front (case, slot ribs, keyboard, joystick)
    bool tvc(const char* id, Colour colour);
    // The screenshot key: corner marks around a camera, as wide as the REC key
    bool camera(const char* id, ImFont* font, float size, Colour colour);
    void draw_hover(ImVec2 pos, ImVec2 size) const;
    float height() const { return height_; }

private:
    const Skin& skin_;
    float height_;
};

}  // namespace freetvc::sdl
