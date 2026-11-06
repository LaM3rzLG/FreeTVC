// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/sdl/deck.hpp"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <string>

#include "frontend/sdl/text.hpp"

namespace freetvc::sdl {


ImU32 with_alpha(Colour c, float alpha) {
    c.a = static_cast<std::uint8_t>(c.a * std::clamp(alpha, 0.0f, 1.0f));
    return to_imgui(c);
}

ImVec2 ScreenRect::at(float x, float y) const {
    return {min.x + (x - static_cast<float>(area.x)) * size.x / static_cast<float>(area.width),
            min.y + (y - static_cast<float>(area.y)) * size.y / static_cast<float>(area.height)};
}

ScreenRect draw_screen(ImDrawList* draw, ImTextureID screen, bool full_raster, ImVec2 min, ImVec2 size, Colour outline,
                       const PictureHook* picture) {
    const ScreenArea area = full_raster ? kFullRaster : kTvArea;
    const float frame_w = static_cast<float>(area.width) * kPixelAspect;
    const float frame_h = static_cast<float>(area.height);
    const float scale = std::max(0.0f, std::min(size.x / frame_w, size.y / frame_h));
    const ImVec2 size_px{frame_w * scale, frame_h * scale};
    const ImVec2 p0{min.x + (size.x - size_px.x) / 2.0f, min.y + (size.y - size_px.y) / 2.0f};
    const float fb_w = static_cast<float>(Framebuffer::kWidth);
    const float fb_h = static_cast<float>(Framebuffer::kHeight);
    if (!picture || !*picture || !(*picture)(draw, p0, p0 + size_px, area)) {
        draw->AddImage(screen, p0, p0 + size_px, {area.x / fb_w, area.y / fb_h},
                       {(area.x + area.width) / fb_w, (area.y + area.height) / fb_h});
    }
    const ScreenRect rect{p0, size_px, area};
    if (full_raster) {
        const ImVec2 tv0 = rect.at(static_cast<float>(kTvArea.x), static_cast<float>(kTvArea.y));
        const ImVec2 tv1 = rect.at(static_cast<float>(kTvArea.x + kTvArea.width), static_cast<float>(kTvArea.y + kTvArea.height));
        draw->AddRect(tv0, tv1, with_alpha(outline, 0.6f));
    }
    return rect;
}

bool DeckRow::button(const char* id, const char* text, ImFont* font, float size, Colour colour, bool enabled, float local_x) {
    const ImVec2 text_size = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const float pad = ImGui::GetStyle().FramePadding.x;
    ImGui::SameLine(local_x, 0.0f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, {text_size.x + pad * 2.0f, height_});
    if (enabled) {
        draw_hover(pos, {text_size.x + pad * 2.0f, height_});
    }
    ImGui::GetWindowDrawList()->AddText(font, size, {pos.x + pad, pos.y + (height_ - text_size.y) / 2.0f},
                                        to_imgui(enabled ? colour : skin_.muted), text);
    return clicked && enabled;
}

void DeckRow::text(const char* text, ImFont* font, float size, Colour colour) {
    const ImVec2 text_size = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const float pad = ImGui::GetStyle().FramePadding.x;
    ImGui::SameLine(0.0f, 0.0f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy({text_size.x + pad * 2.0f, height_});
    ImGui::GetWindowDrawList()->AddText(font, size, {pos.x + pad, pos.y + (height_ - text_size.y) / 2.0f}, to_imgui(colour), text);
}

void DeckRow::divider() {
    ImGui::SameLine(0.0f, 0.0f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    ImGui::Dummy({1.0f, height_});
    const float inset = height_ * 0.25f;
    ImGui::GetWindowDrawList()->AddLine({pos.x, pos.y + inset}, {pos.x, pos.y + height_ - inset}, to_imgui(skin_.rib));
}

bool DeckRow::slot_ribs(float scale, std::span<const std::string_view> cards) {
    const float rib_w = 5.0f * scale;
    const float slant = 3.0f * scale;
    const float gap = 4.0f * scale;
    const float rib_h = height_ * 0.56f;
    const float pad = 10.0f * scale;
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = pad * 2.0f + 4.0f * rib_w + 3.0f * gap + slant;
    const bool clicked = ImGui::InvisibleButton("##slots", {width, height_});
    draw_hover(pos, {width, height_});
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float top = pos.y + (height_ - rib_h) / 2.0f;
    for (int i = 0; i < 4; ++i) {
        const float x = pos.x + pad + static_cast<float>(i) * (rib_w + gap);
        const bool lit = static_cast<std::size_t>(i) < cards.size() && !cards[static_cast<std::size_t>(i)].empty();
        draw->AddQuadFilled({x + slant, top}, {x + slant + rib_w, top}, {x + rib_w, top + rib_h}, {x, top + rib_h},
                            to_imgui(lit ? skin_.label : skin_.rib));
    }
    if (ImGui::IsItemHovered()) {
        std::string text;
        char line[160];
        for (std::size_t i = 0; i < 4; ++i) {
            const std::string card(i < cards.size() ? cards[i] : std::string_view{});
            std::snprintf(line, sizeof line, tr("Slot %zu (ports %zu0h-%zuFh): %s"), i, i + 1, i + 1, card.empty() ? tr("empty") : card.c_str());
            text += (text.empty() ? "" : "\n") + std::string(line);
        }
        ImGui::SetTooltip("%s", text.c_str());
    }
    return clicked;
}

bool DeckRow::transport(const char* id, bool stop, Colour colour) {
    ImGui::SameLine(0.0f, 0.0f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, {height_, height_});
    draw_hover(pos, {height_, height_});
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float side = height_ * 0.36f;
    const ImVec2 c{pos.x + height_ / 2.0f, pos.y + height_ / 2.0f};
    if (stop) {
        draw->AddRectFilled({c.x - side / 2.0f, c.y - side / 2.0f}, {c.x + side / 2.0f, c.y + side / 2.0f}, to_imgui(colour));
    } else {
        draw->AddTriangleFilled({c.x - side * 0.4f, c.y - side / 2.0f}, {c.x + side * 0.6f, c.y}, {c.x - side * 0.4f, c.y + side / 2.0f},
                                to_imgui(colour));
    }
    return clicked;
}

namespace {

// Four rounded corner marks inside the square or rectangle at `min`-`max`
void corners(ImDrawList* draw, ImVec2 min, ImVec2 max, float arm, float thick, ImU32 colour) {
    const float r = thick * 1.2f;
    for (int i = 0; i < 4; ++i) {
        const float x = i & 1 ? max.x : min.x;
        const float y = i & 2 ? max.y : min.y;
        const float dx = i & 1 ? -1.0f : 1.0f;
        const float dy = i & 2 ? -1.0f : 1.0f;
        draw->PathLineTo({x + dx * arm, y});
        draw->PathLineTo({x + dx * r, y});
        draw->PathBezierQuadraticCurveTo({x, y}, {x, y + dy * r});
        draw->PathLineTo({x, y + dy * arm});
        draw->PathStroke(colour, thick);
    }
}

}  // namespace

float rec_width(ImFont* font, float size, float height) {
    const ImVec2 text = font->CalcTextSizeA(size, FLT_MAX, 0.0f, "REC");
    return height * 0.2f * 2.0f + height * 0.18f + text.y * 0.64f + text.y * 0.25f + text.x + height * 0.18f;
}

bool DeckRow::rec(const char* id, ImFont* font, float size, Colour colour, Colour dot, bool lit) {
    ImGui::SameLine(0.0f, 0.0f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const ImVec2 text = font->CalcTextSizeA(size, FLT_MAX, 0.0f, "REC");
    const float inset = height_ * 0.2f;
    const float radius = text.y * 0.32f;
    const float width = rec_width(font, size, height_);
    const bool clicked = ImGui::InvisibleButton(id, {width, height_});
    draw_hover(pos, {width, height_});
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const float thick = std::max(1.0f, height_ * 0.06f);
    corners(draw, {pos.x + inset, pos.y + inset}, {pos.x + width - inset, pos.y + height_ - inset}, height_ * 0.16f, thick, to_imgui(colour));
    const float cy = pos.y + height_ / 2.0f;
    const float dot_x = pos.x + inset + height_ * 0.18f + radius;
    draw->AddCircleFilled({dot_x, cy}, radius, lit ? to_imgui(dot) : with_alpha(dot, 0.35f));
    draw->AddText(font, size, {dot_x + radius + text.y * 0.25f, cy - text.y / 2.0f}, to_imgui(colour), "REC");
    return clicked;
}

bool DeckRow::tvc(const char* id, Colour colour) {
    ImGui::SameLine(0.0f, 0.0f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    // Drawn on a 68 x 45 grid scaled to the strip's height
    const float k = height_ / 45.0f;
    const float width = 68.0f * k;
    const bool clicked = ImGui::InvisibleButton(id, {width, height_});
    draw_hover(pos, {width, height_});
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 c = to_imgui(colour);
    // The drawing at 90 % of the key, centred
    const float g = k * 0.9f;
    const ImVec2 o{pos.x + (width - 68.0f * g) / 2.0f, pos.y + (height_ - 45.0f * g) / 2.0f};
    const float thick = std::max(1.0f, 2.2f * g);
    const auto p = [&](float x, float y) { return ImVec2{o.x + x * g, o.y + y * g}; };
    draw->AddRect(p(6.0f, 9.0f), p(62.0f, 36.0f), c, 2.5f * g, thick);
    for (const float x : {10.0f, 14.0f, 18.0f, 22.0f}) {
        draw->AddLine(p(x, 9.0f), p(x, 18.0f), c, thick);
    }
    draw->AddRect(p(12.0f, 22.0f), p(46.0f, 32.0f), c, 1.5f * g, thick);
    draw->AddLine(p(15.0f, 25.3f), p(43.0f, 25.3f), c, thick);
    draw->AddLine(p(15.0f, 28.7f), p(43.0f, 28.7f), c, thick);
    draw->AddLine(p(54.0f, 31.0f), p(54.0f, 25.0f), c, thick);
    draw->AddCircleFilled(p(54.0f, 24.0f), 2.6f * g, c);
    return clicked;
}

bool DeckRow::camera(const char* id, ImFont* font, float size, Colour colour) {
    ImGui::SameLine(0.0f, 0.0f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float width = rec_width(font, size, height_);
    const bool clicked = ImGui::InvisibleButton(id, {width, height_});
    draw_hover(pos, {width, height_});
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 c = to_imgui(colour);
    const float thick = std::max(1.0f, height_ * 0.06f);
    const float inset = height_ * 0.2f;
    corners(draw, {pos.x + inset, pos.y + inset}, {pos.x + width - inset, pos.y + height_ - inset}, height_ * 0.16f, thick, c);
    const ImVec2 mid{pos.x + width / 2.0f, pos.y + height_ / 2.0f + height_ * 0.03f};
    const float w = height_ * 0.36f;
    const float h = height_ * 0.24f;
    draw->AddRect({mid.x - w / 2.0f, mid.y - h / 2.0f}, {mid.x + w / 2.0f, mid.y + h / 2.0f}, c, thick, thick);
    draw->AddRectFilled({mid.x - w * 0.18f, mid.y - h / 2.0f - h * 0.28f}, {mid.x + w * 0.18f, mid.y - h / 2.0f}, c, thick * 0.5f);
    draw->AddCircle(mid, h * 0.3f, c, 0, thick);
    return clicked;
}

ImVec2 DeckRow::icon_key(const char* id, float grid_w, bool& clicked, float& g) {
    ImGui::SameLine(0.0f, 0.0f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float k = height_ / 45.0f;
    const float width = grid_w * k;
    clicked = ImGui::InvisibleButton(id, {width, height_});
    draw_hover(pos, {width, height_});
    // At 90 % of the key, centred, like the machine key
    g = k * 0.9f;
    return {pos.x + (width - grid_w * g) / 2.0f, pos.y + (height_ - 45.0f * g) / 2.0f};
}

bool DeckRow::keyboard(const char* id, Colour colour) {
    bool clicked = false;
    float g = 0.0f;
    const ImVec2 o = icon_key(id, 60.0f, clicked, g);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 c = to_imgui(colour);
    const float thick = std::max(1.0f, 2.2f * g);
    const auto p = [&](float x, float y) { return ImVec2{o.x + x * g, o.y + y * g}; };
    draw->AddRect(p(5.0f, 11.0f), p(55.0f, 35.0f), c, 2.5f * g, thick);
    for (const float x : {10.0f, 17.0f, 24.0f, 31.0f, 38.0f, 45.0f}) {
        draw->AddLine(p(x, 17.0f), p(x + 4.0f, 17.0f), c, thick);
    }
    for (const float x : {12.0f, 19.0f, 26.0f, 33.0f, 40.0f, 47.0f}) {
        draw->AddLine(p(x, 23.0f), p(x + 3.5f, 23.0f), c, thick);
    }
    draw->AddLine(p(18.0f, 29.0f), p(42.0f, 29.0f), c, thick);
    return clicked;
}

bool DeckRow::speaker(const char* id, Colour colour, bool slashed, Colour slash) {
    bool clicked = false;
    float g = 0.0f;
    const ImVec2 o = icon_key(id, 50.0f, clicked, g);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 c = to_imgui(colour);
    const float thick = std::max(1.0f, 2.2f * g);
    const auto p = [&](float x, float y) { return ImVec2{o.x + x * g, o.y + y * g}; };
    const ImVec2 body[] = {p(8.0f, 18.0f), p(15.0f, 18.0f), p(25.0f, 10.0f), p(25.0f, 35.0f), p(15.0f, 27.0f), p(8.0f, 27.0f)};
    draw->AddPolyline(body, 6, c, thick, ImDrawFlags_Closed);
    constexpr float kPi = 3.14159265f;
    draw->PathArcTo(p(25.0f, 22.5f), 8.0f * g, -kPi * 0.27f, kPi * 0.27f);
    draw->PathStroke(c, thick);
    draw->PathArcTo(p(25.0f, 22.5f), 14.0f * g, -kPi * 0.3f, kPi * 0.3f);
    draw->PathStroke(c, thick);
    if (slashed) {
        draw->AddLine(p(6.0f, 38.0f), p(42.0f, 7.0f), to_imgui(slash), thick * 1.2f);
    }
    return clicked;
}

bool DeckRow::cassette(const char* id, Colour colour) {
    bool clicked = false;
    float g = 0.0f;
    const ImVec2 o = icon_key(id, 64.0f, clicked, g);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 c = to_imgui(colour);
    const float thick = std::max(1.0f, 2.2f * g);
    const auto p = [&](float x, float y) { return ImVec2{o.x + x * g, o.y + y * g}; };
    draw->AddRect(p(5.0f, 8.0f), p(59.0f, 39.0f), c, 2.5f * g, thick);
    draw->AddRect(p(13.0f, 13.0f), p(51.0f, 26.0f), c, 1.5f * g, thick);
    draw->AddCircle(p(23.0f, 19.5f), 3.2f * g, c, 0, thick);
    draw->AddCircle(p(41.0f, 19.5f), 3.2f * g, c, 0, thick);
    draw->AddLine(p(26.2f, 19.5f), p(37.8f, 19.5f), c, thick);
    const ImVec2 head[] = {p(15.0f, 39.0f), p(18.5f, 32.0f), p(45.5f, 32.0f), p(49.0f, 39.0f)};
    draw->AddPolyline(head, 4, c, thick, ImDrawFlags_None);
    return clicked;
}

bool DeckRow::floppy(const char* id, Colour colour) {
    bool clicked = false;
    float g = 0.0f;
    const ImVec2 o = icon_key(id, 48.0f, clicked, g);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 c = to_imgui(colour);
    const float thick = std::max(1.0f, 2.2f * g);
    const auto p = [&](float x, float y) { return ImVec2{o.x + x * g, o.y + y * g}; };
    // The sleeve, the hub hole, the head slot and the write-protect notch
    draw->AddRect(p(7.0f, 7.0f), p(41.0f, 39.0f), c, 0.0f, thick);
    draw->AddCircle(p(24.0f, 21.0f), 4.5f * g, c, 0, thick);
    draw->AddRect(p(21.5f, 29.0f), p(26.5f, 37.0f), c, 2.5f * g, thick);
    const ImVec2 notch[] = {p(41.0f, 12.0f), p(38.0f, 12.0f), p(38.0f, 16.0f), p(41.0f, 16.0f)};
    draw->AddPolyline(notch, 4, c, thick, ImDrawFlags_None);
    return clicked;
}

bool DeckRow::cartridge(const char* id, ImFont* font, float size, const char* text, Colour colour) {
    ImGui::SameLine(0.0f, 0.0f);
    const ImVec2 pos = ImGui::GetCursorScreenPos();
    const float k = height_ / 45.0f;
    const float g = k * 0.9f;
    const float icon_w = 36.0f * k;
    const float pad = ImGui::GetStyle().FramePadding.x;
    const ImVec2 text_size = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const float width = icon_w + text_size.x + pad;
    const bool clicked = ImGui::InvisibleButton(id, {width, height_});
    draw_hover(pos, {width, height_});
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const ImU32 c = to_imgui(colour);
    const float thick = std::max(1.0f, 2.0f * g);
    const ImVec2 o{pos.x + (icon_w - 36.0f * g) / 2.0f, pos.y + (height_ - 45.0f * g) / 2.0f};
    const auto p = [&](float x, float y) { return ImVec2{o.x + x * g, o.y + y * g}; };
    // The case with its grip ribs, the contact edge with its teeth
    draw->AddRect(p(6.0f, 9.5f), p(26.0f, 35.5f), c, 1.5f * g, thick);
    for (const float y : {14.5f, 18.5f, 22.5f, 26.5f, 30.5f}) {
        draw->AddLine(p(10.0f, y), p(16.0f, y), c, thick);
    }
    const ImVec2 edge[] = {p(26.0f, 32.5f), p(31.0f, 32.5f), p(31.0f, 12.5f), p(26.0f, 12.5f)};
    draw->AddPolyline(edge, 4, c, thick, ImDrawFlags_None);
    for (const float y : {16.5f, 20.5f, 24.5f, 28.5f}) {
        draw->AddLine(p(31.0f, y), p(28.5f, y), c, thick);
    }
    draw->AddText(font, size, {pos.x + icon_w, pos.y + (height_ - text_size.y) / 2.0f}, c, text);
    return clicked;
}

void DeckRow::draw_hover(ImVec2 pos, ImVec2 size) const {
    if (ImGui::IsItemActive()) {
        ImGui::GetWindowDrawList()->AddRectFilled(pos, pos + size, to_imgui(skin_.key_active));
    } else if (ImGui::IsItemHovered()) {
        ImGui::GetWindowDrawList()->AddRectFilled(pos, pos + size, to_imgui(skin_.key));
    }
}

}  // namespace freetvc::sdl
