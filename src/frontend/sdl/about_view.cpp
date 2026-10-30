// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/sdl/about_view.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <cstdio>

#include <SDL3/SDL.h>

#include "build_info.hpp"
#include "core/version.hpp"
#include "core/video/framebuffer.hpp"
#include "frontend/sdl/text.hpp"
#include "imgui_internal.h"

namespace freetvc::sdl {

namespace {

constexpr float kWidth = 600.0f;  // logical pixels; the height follows the content
constexpr const char* kRepository = "https://github.com/LaM3rzLG/FreeTVC";
constexpr std::array<unsigned, 7> kBright{9, 10, 11, 12, 13, 14, 15};  // the TVC's bright colours, I alone (black) left out

// TVC palette entry `index` at brightness `level` (0-1)
ImU32 tvc(unsigned index, float level = 1.0f) {
    const std::uint32_t rgb = kRgbPalette[index];
    const auto c = [&](unsigned shift) { return static_cast<int>(static_cast<float>(rgb >> shift & 0xFF) * level); };
    return IM_COL32(c(16), c(8), c(0), 255);
}

ImVec4 vec(Colour c) {
    return ImVec4(c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f);
}

std::string sdl_version() {
    const int v = SDL_GetVersion();
    char text[32];
    std::snprintf(text, sizeof text, "%d.%d.%d", SDL_VERSIONNUM_MAJOR(v), SDL_VERSIONNUM_MINOR(v), SDL_VERSIONNUM_MICRO(v));
    return text;
}

// Copper bars: three bands of TVC colour, shaded dark-bright-dark, bouncing through the strip on sines
void raster_bars(ImDrawList* draw, ImVec2 p0, ImVec2 p1, float scale, double t) {
    draw->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, 255));
    const float height = p1.y - p0.y;
    const float line = std::max(1.0f, std::round(scale));
    const int lines = 14;
    for (int bar = 0; bar < 3; ++bar) {
        const double phase = t * 2.1 + bar * 2.0944;
        const float centre = p0.y + height * 0.5f + static_cast<float>(std::sin(phase)) * (height * 0.5f - lines * line * 0.5f);
        const unsigned colour = kBright[(static_cast<unsigned>(t * 0.7) + static_cast<unsigned>(bar) * 2) % kBright.size()];
        for (int j = 0; j < lines; ++j) {
            const float level = 0.12f + 0.88f * std::sin(3.14159265f * (static_cast<float>(j) + 0.5f) / lines);
            const float y = centre + (static_cast<float>(j) - lines * 0.5f) * line;
            draw->AddRectFilled({p0.x, std::max(p0.y, y)}, {p1.x, std::min(p1.y, y + line)}, tvc(colour, level));
        }
    }
}

// Text one glyph at a time: each glyph lifted by `wave` and coloured through the bright TVC colours
float rainbow_text(ImDrawList* draw, ImFont* font, float size, ImVec2 at, const char* text, double t, float wave, float clip_left,
                   float clip_right) {
    float x = at.x;
    unsigned glyph = 0;
    for (const char* s = text; *s;) {
        unsigned int c = 0;
        const int bytes = ImTextCharFromUtf8(&c, s, nullptr);
        const char* e = s + std::max(bytes, 1);
        const float advance = font->CalcTextSizeA(size, FLT_MAX, 0.0f, s, e).x;
        if (x + advance >= clip_left && x <= clip_right) {
            const float y = at.y + wave * static_cast<float>(std::sin(t * 3.2 + x * 0.018));
            const unsigned colour = kBright[(glyph + static_cast<unsigned>(t * 5.0)) % kBright.size()];
            draw->AddText(font, size, {x, y}, tvc(colour), s, e);
        }
        x += advance;
        s = e;
        ++glyph;
    }
    return x - at.x;
}

}  // namespace

std::string AboutView::build_text(std::string_view renderer) {
    std::string text = std::string("freeTVC ") + kVersion + "\n";
    text += std::string("Build: ") + build::kCommit + (build::kModified ? " (modified)" : "") + ", " + build::kCommitDate + "\n";
    text += std::string("Built: ") + build::kBuildDate + " with " + build::kCompiler + "\n";
    text += std::string("Platform: ") + build::kPlatform + ", SDL " + sdl_version() + "\n";
    text += "Renderer: " + std::string(renderer) + "\n";
    return text;
}

bool AboutView::load_logo(Display& display, const std::filesystem::path& path, int width, int height) {
    if (logo_ != ImTextureID_Invalid && logo_width_ == width && logo_height_ == height) {
        return true;
    }
    if (logo_missing_ || width <= 0 || height <= 0) {
        return false;
    }
    if (!source_) {
        SDL_Surface* png = SDL_LoadPNG(path.string().c_str());
        source_ = png ? SDL_ConvertSurface(png, SDL_PIXELFORMAT_ARGB8888) : nullptr;
        SDL_DestroySurface(png);
        if (!source_) {
            std::fprintf(stderr, "freetvc: %s: %s\n", path.string().c_str(), SDL_GetError());
            logo_missing_ = true;
            return false;
        }
    }
    SDL_Surface* sized = SDL_ScaleSurface(source_, width, height, SDL_SCALEMODE_LINEAR);
    if (!sized) {
        logo_missing_ = true;
        return false;
    }
    if (logo_ != ImTextureID_Invalid) {
        display.destroy_texture(logo_);
    }
    logo_ = display.create_texture(width, height);
    display.update_texture(logo_, sized->pixels, sized->pitch);
    SDL_DestroySurface(sized);
    logo_width_ = width;
    logo_height_ = height;
    return true;
}

void AboutView::release(Display& display) {
    if (logo_ != ImTextureID_Invalid) {
        display.destroy_texture(logo_);
        logo_ = ImTextureID_Invalid;
    }
    SDL_DestroySurface(source_);
    source_ = nullptr;
}

bool AboutView::draw(Display& display, const std::filesystem::path& logo, std::string_view renderer, const Skin& skin, const SkinFonts& fonts,
                     float scale) {
    if (!open_) {
        return false;
    }
    const double t = ImGui::GetTime();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoNav;

    // The rest of the window dims; a click there closes
    ImGui::SetNextWindowPos(viewport->Pos);
    ImGui::SetNextWindowSize(viewport->Size);
    if (raise_) {
        ImGui::SetNextWindowFocus();
    }
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 160));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##about_backdrop", nullptr, kFlags);
    if (ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        open_ = false;
    }
    ImGui::End();
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();

    const float width = kWidth * scale;
    const float pad = 20.0f * scale;
    const float inner = width - 2.0f * pad;
    ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_Always, {0.5f, 0.5f});
    ImGui::SetNextWindowSizeConstraints({width, 0.0f}, {width, FLT_MAX});
    if (raise_) {
        ImGui::SetNextWindowFocus();
        raise_ = false;
    }
    ImGui::PushStyleColor(ImGuiCol_WindowBg, IM_COL32(0, 0, 0, 255));
    ImGui::PushStyleColor(ImGuiCol_Border, vec(skin.rib));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f * scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {pad, pad});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::Begin("##about", nullptr, kFlags | ImGuiWindowFlags_AlwaysAutoResize);
    ImDrawList* draw = ImGui::GetWindowDrawList();

    // The logo, resampled to its size on screen and shrunk when the window would not fit
    const float aspect = source_ ? static_cast<float>(source_->h) / static_cast<float>(source_->w) : 0.53f;
    const float room = viewport->Size.y - 24.0f * scale - rest_height_;
    const float logo_hf = std::clamp(rest_height_ > 0.0f ? room : inner * aspect, 80.0f * scale, inner * aspect);
    const int logo_h = static_cast<int>(logo_hf);
    const int logo_w = static_cast<int>(logo_hf / aspect);
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (inner - static_cast<float>(logo_w)) * 0.5f);
    const float logo_top = ImGui::GetCursorPosY();
    if (load_logo(display, logo, logo_w, logo_h)) {
        ImGui::Image(ImTextureRef(logo_), {static_cast<float>(logo_w), static_cast<float>(logo_h)});
    } else {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::Dummy({inner, 90.0f * scale});
        rainbow_text(draw, fonts.strong, skin.font_ui_size * scale * 4.0f, {at.x + inner * 0.22f, at.y + 10.0f * scale}, "L@M3rZ", t, 4.0f * scale,
                     at.x, at.x + inner);
    }

    const float logo_shown = ImGui::GetCursorPosY() - logo_top;
    const ImVec2 bars = ImGui::GetCursorScreenPos();
    ImGui::Dummy({inner, 34.0f * scale});
    raster_bars(draw, bars, {bars.x + inner, bars.y + 34.0f * scale}, scale, t);
    ImGui::Dummy({0.0f, 4.0f * scale});

    ImGui::PushFont(fonts.strong, skin.font_ui_size * 2.2f);
    ImGui::PushStyleColor(ImGuiCol_Text, vec(skin.label));
    ImGui::TextUnformatted("freeTVC");
    ImGui::PopStyleColor();
    ImGui::SameLine();
    ImGui::PushStyleColor(ImGuiCol_Text, vec(skin.role(Role::Notice).on_case));
    ImGui::TextUnformatted(kVersion);
    ImGui::PopStyleColor();
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, vec(skin.muted));
    ImGui::TextUnformatted(tr("an emulator of the Videoton TV Computer"));
    ImGui::PopStyleColor();
    ImGui::Dummy({0.0f, 6.0f * scale});

    // Version, build and what runs it
    ImGui::PushFont(fonts.data, 0.0f);
    if (ImGui::BeginTable("##build", 2, ImGuiTableFlags_None, {inner, 0.0f})) {
        ImGui::TableSetupColumn("label", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch);
        const auto row = [&](const char* label, const std::string& value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, vec(skin.muted));
            ImGui::TextUnformatted(label);
            ImGui::PopStyleColor();
            ImGui::TableNextColumn();
            ImGui::PushStyleColor(ImGuiCol_Text, vec(skin.label));
            ImGui::TextWrapped("%s", value.c_str());
            ImGui::PopStyleColor();
        };
        const bool known = std::string_view(build::kCommit) != "unknown";
        row(tr("Build"), known ? std::string(build::kCommit) + (build::kModified ? tr(" (modified)") : "") + ", " + build::kCommitDate
                               : std::string(tr("not from a git checkout")));
        row(tr("Built"), tr_format("%s with %s", build::kBuildDate, build::kCompiler));
        row(tr("Platform"), std::string(build::kPlatform) + ", SDL " + sdl_version());
        row(tr("Renderer"), std::string(renderer));
        ImGui::EndTable();
    }
    ImGui::PopFont();
    ImGui::Dummy({0.0f, 8.0f * scale});

    // Credits: the handle, then the crew in the TVC's colours
    ImGui::PushStyleColor(ImGuiCol_Text, vec(skin.muted));
    ImGui::TextUnformatted(tr("Made by"));
    ImGui::PopStyleColor();
    ImGui::PushFont(fonts.strong, skin.font_ui_size * 1.6f);
    const float name_width = std::max(ImGui::CalcTextSize("LG").x, ImGui::CalcTextSize("Dey").x) + ImGui::GetFontSize() * 0.35f;
    for (const char* name : {"LG", "Dey"}) {
        const ImVec2 at = ImGui::GetCursorScreenPos();
        ImGui::PushStyleColor(ImGuiCol_Text, vec(skin.label));
        ImGui::TextUnformatted(name);
        ImGui::PopStyleColor();
        const float px = ImGui::GetFontSize();
        rainbow_text(draw, fonts.strong, px, {at.x + name_width, at.y}, "/ L@M3rZ", t * 0.6, 2.0f * scale, at.x, at.x + inner);
    }
    ImGui::PopFont();
    ImGui::Dummy({0.0f, 8.0f * scale});

    ImGui::PushStyleColor(ImGuiCol_Text, vec(skin.muted));
    ImGui::TextUnformatted("GPL-3.0-or-later");
    ImGui::PopStyleColor();
    ImGui::SameLine(0.0f, 14.0f * scale);
    ImGui::TextLinkOpenURL("github.com/LaM3rzLG/FreeTVC", kRepository);
    ImGui::Dummy({0.0f, 6.0f * scale});

    // The sine scroller
    const float band = 40.0f * scale;
    const ImVec2 b0 = ImGui::GetCursorScreenPos();
    ImGui::Dummy({inner, band});
    const ImVec2 b1{b0.x + inner, b0.y + band};
    draw->AddRectFilled(b0, b1, IM_COL32(0, 0, 0, 255));
    draw->AddLine(b0, {b1.x, b0.y}, to_imgui(skin.rib), scale);
    draw->AddLine({b0.x, b1.y}, b1, to_imgui(skin.rib), scale);
    const std::string scroll = tr_format(
        "freeTVC %s   ***   the Videoton TV Computer lives on   ***   coded by LG and Dey of L@M3rZ   ***   greetings to everyone who keeps "
        "the TVC alive: the tvc.homeserver.hu archive, the makers of the expansion cards and the authors of the emulators that "
        "showed the way   ***   press Esc to get back to the machine   ***",
        kVersion);
    const float size = skin.font_ui_size * scale * 1.4f;
    const float text_width = fonts.strong->CalcTextSizeA(size, FLT_MAX, 0.0f, scroll.c_str()).x;
    const float travel = static_cast<float>(std::fmod(t * 95.0 * scale, static_cast<double>(text_width + inner)));
    draw->PushClipRect(b0, b1, true);
    rainbow_text(draw, fonts.strong, size, {b1.x - travel, b0.y + (band - size) * 0.5f}, scroll.c_str(), t, 7.0f * scale, b0.x, b1.x);
    draw->PopClipRect();
    ImGui::Dummy({0.0f, 6.0f * scale});

    const bool copied = t - copied_at_ < 2.0;
    if (ImGui::Button(copied ? tr("Copied") : tr("Copy build info"))) {
        SDL_SetClipboardText(build_text(renderer).c_str());
        copied_at_ = t;
    }
    ImGui::SameLine();
    if (ImGui::Button(tr("Close")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        open_ = false;
    }

    rest_height_ = ImGui::GetCursorPosY() + pad - logo_shown;  // this frame's content, not the window's lagging height
    ImGui::End();
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    return true;
}

}  // namespace freetvc::sdl
