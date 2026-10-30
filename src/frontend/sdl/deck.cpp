// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/sdl/deck.hpp"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <string>

#include "frontend/sdl/text.hpp"

namespace freetvc::sdl {

namespace {

// Width/height of one framebuffer pixel on a 4:3 PAL screen: square-pixel clock 14.75 MHz vs 12.5 MHz, one raster line = two interlaced lines.
constexpr float kPixelAspect = (14.75f / 12.5f) / 2.0f;

}  // namespace

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

void DeckRow::draw_hover(ImVec2 pos, ImVec2 size) const {
    if (ImGui::IsItemActive()) {
        ImGui::GetWindowDrawList()->AddRectFilled(pos, pos + size, to_imgui(skin_.key_active));
    } else if (ImGui::IsItemHovered()) {
        ImGui::GetWindowDrawList()->AddRectFilled(pos, pos + size, to_imgui(skin_.key));
    }
}

}  // namespace freetvc::sdl
