// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/sdl/keyboard_editor.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <string>

#include "frontend/sdl/deck.hpp"
#include "frontend/sdl/play_view.hpp"
#include "frontend/sdl/text.hpp"

namespace freetvc::sdl {

namespace {

constexpr float kUnit = 36.0f;  // a key's pitch at scale 1, in window pixels
constexpr float kRows = 5.0f;
constexpr float kWidthUnits = 27.0f;  // keyboard 18.5, gap 1, two joysticks 3.5 each with 0.5 between

// tvc-hardware.md §7 "Physical layout"; width in key units, an empty name is a gap
struct Cap {
    std::string_view name;
    float width = 1.0f;
};
constexpr Cap kRow0[] = {{"Í"}, {"0"}, {"1"}, {"2"}, {"3"}, {"4"}, {"5"}, {"6"}, {"7"}, {"8"}, {"9"}, {"Ö"}, {"Ü"}, {"Ó"}, {"*"}, {"^"},
                         {"", 0.5f}, {"INS"}, {"ESC"}};
constexpr Cap kRow1[] = {{"", 0.5f}, {"@"}, {";"}, {"Q"}, {"W"}, {"E"}, {"R"}, {"T"}, {"Z"}, {"U"}, {"I"}, {"O"}, {"P"}, {"Ő"}, {"Ú"},
                         {"["}, {"]"}, {"", 1.0f}, {"DEL"}};
constexpr Cap kRow2[] = {{"", 0.75f}, {"<"}, {"\\"}, {"A"}, {"S"}, {"D"}, {"F"}, {"G"}, {"H"}, {"J"}, {"K"}, {"L"}, {"É"}, {"Á"},
                         {"Ű"}, {"RETURN", 2.75f}};
constexpr Cap kRow3[] = {{"LOCK", 1.25f}, {"SHIFT", 1.25f}, {"Y"}, {"X"}, {"C"}, {"V"}, {"B"}, {"N"}, {"M"}, {","}, {"."}, {"-"},
                         {"SHIFT", 2.0f}};
constexpr Cap kRow4[] = {{"", 2.5f}, {"CTRL", 1.5f}, {"SPACE", 7.0f}, {"ALT", 1.5f}};

std::string utf8(char32_t c) {
    std::string s;
    if (c < 0x80) {
        s += static_cast<char>(c);
    } else if (c < 0x800) {
        s += static_cast<char>(0xC0 | (c >> 6));
        s += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        s += static_cast<char>(0xE0 | (c >> 12));
        s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (c & 0x3F));
    }
    return s;
}

// The second legend of a number or sign key: its shifted character
std::string shifted_legend(MatrixKey key) {
    const CharKey* plain = nullptr;
    const CharKey* shifted = nullptr;
    for (const CharKey& c : char_keys()) {
        if (c.key == key) {
            (c.shift ? shifted : plain) = &c;
        }
    }
    return plain && shifted && !is_letter(plain->ch) ? utf8(shifted->ch) : std::string();
}

// What the user's keyboard shows on a host key
std::string host_label(SDL_Scancode scancode, int shift) {
    std::string label;
    for (const HostKeyName& h : host_key_names()) {
        if (h.scancode == scancode && !h.label.empty()) {
            label = std::string(h.label);
        }
    }
    if (label.empty()) {
        const char* name = SDL_GetKeyName(SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, false));
        label = name && *name ? name : std::string(host_key_name(scancode));
    }
    return label + (shift == 1 ? "+Shift" : "");
}

bool is_modifier(std::string_view name) {
    return name == "INS" || name == "ESC" || name == "DEL" || name == "RETURN" || name == "LOCK" || name == "SHIFT" ||
           name == "CTRL" || name == "ALT";
}

struct Drawer {
    const Keymap& keymap;
    const Skin& skin;
    const SkinFonts& fonts;
    float scale;
    float unit;
    std::array<bool, kMatrixKeys> lit{};
    std::optional<MatrixKey>& waiting;
    KeyboardEditorActions& actions;

    // One key cap at `at`, `w` units wide; `legend` on it, the host keys under it
    void cap(ImVec2 at, float w, MatrixKey key, const std::string& legend, const std::string& second, bool modifier, int id) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const float gap = 3.0f * scale;
        const ImVec2 size{w * unit - gap, unit - gap};
        ImGui::SetCursorScreenPos(at);
        ImGui::PushID(id);
        ImGui::InvisibleButton("##cap", size, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
        const bool hovered = ImGui::IsItemHovered();
        if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
            waiting = waiting && *waiting == key ? std::nullopt : std::optional<MatrixKey>(key);
        }
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
            actions.clear = key;
            waiting.reset();
        }
        ImGui::PopID();
        const bool down = lit[key_index(key)];
        const ImVec2 p0 = at + ImVec2{0.0f, down ? scale : 0.0f};
        const ImVec2 p1 = p0 + size;
        Colour face = modifier ? skin.key : skin.putty;
        if (hovered) {
            face = modifier ? skin.key_hover : Colour{static_cast<std::uint8_t>(skin.putty.r * 0.93f), static_cast<std::uint8_t>(skin.putty.g * 0.93f),
                                                      static_cast<std::uint8_t>(skin.putty.b * 0.93f)};
        }
        if (down) {
            face = modifier ? skin.key_active : Colour{static_cast<std::uint8_t>(skin.putty.r * 0.8f), static_cast<std::uint8_t>(skin.putty.g * 0.8f),
                                                       static_cast<std::uint8_t>(skin.putty.b * 0.8f)};
        }
        draw->AddRectFilled(p0, p1, to_imgui(face));
        const Colour ink = modifier ? skin.label : skin.ink;
        const RoleColour& notice = skin.role(Role::Notice);
        if (waiting && *waiting == key) {
            draw->AddRect(p0, p1, to_imgui(modifier ? notice.on_case : notice.on_putty), 0.0f, 2.5f * scale);
        }
        const float ui_px = skin.font_ui_size * scale * (modifier ? 0.72f : 0.95f);
        const float small_px = skin.font_ui_size * scale * 0.62f;
        const float pad = 3.0f * scale;
        draw->PushClipRect(p0, p1, true);
        if (!second.empty()) {
            draw->AddText(fonts.ui, small_px, p0 + ImVec2{pad, pad * 0.5f}, to_imgui(ink), second.c_str());
        }
        draw->AddText(fonts.strong, ui_px, p0 + ImVec2{pad, second.empty() ? pad : pad + small_px * 0.8f}, to_imgui(ink), legend.c_str());
        std::string hosts;
        for (const KeyBinding& b : keymap.bindings) {
            if (b.key == key) {
                hosts += (hosts.empty() ? "" : " ") + host_label(b.scancode, b.shift);
            }
        }
        const Colour faint = modifier ? skin.muted : Colour{skin.ink.r, skin.ink.g, skin.ink.b, 150};
        draw->AddText(fonts.data, small_px, {p0.x + pad, p1.y - small_px - pad * 0.5f}, to_imgui(hosts.empty() ? Colour{faint.r, faint.g, faint.b, 90} : faint),
                      hosts.empty() ? "-" : hosts.c_str());
        draw->PopClipRect();
        if (hovered && !hosts.empty()) {
            ImGui::SetTooltip("%s", hosts.c_str());
        }
    }

    void row(ImVec2 at, std::span<const Cap> caps, int& id) {
        float x = 0.0f;
        for (const Cap& c : caps) {
            if (!c.name.empty()) {
                const auto key = find_key(c.name);
                if (key) {
                    cap(at + ImVec2{x * unit, 0.0f}, c.width, *key, c.name == "SPACE" ? std::string() : std::string(c.name),
                        shifted_legend(*key), is_modifier(c.name), id++);
                }
            }
            x += c.width;
        }
    }

    // A joystick's four directions in a cross, fire and accel under it
    void joystick(ImVec2 at, unsigned number, const char* caption, int& id) {
        ImGui::GetWindowDrawList()->AddText(fonts.ui, skin.font_ui_size * scale * 0.8f, at, to_imgui(skin.muted), caption);
        const ImVec2 top = at + ImVec2{0.0f, unit * 0.5f};
        cap(top + ImVec2{unit * 1.25f, 0.0f}, 1.0f, joystick_key(number, JoyLine::Up), "UP", "", true, id++);
        cap(top + ImVec2{unit * 0.25f, unit}, 1.0f, joystick_key(number, JoyLine::Left), "LEFT", "", true, id++);
        cap(top + ImVec2{unit * 2.25f, unit}, 1.0f, joystick_key(number, JoyLine::Right), "RIGHT", "", true, id++);
        cap(top + ImVec2{unit * 1.25f, unit * 2.0f}, 1.0f, joystick_key(number, JoyLine::Down), "DOWN", "", true, id++);
        cap(top + ImVec2{0.0f, unit * 3.1f}, 1.75f, joystick_key(number, JoyLine::Fire), "FIRE", "", true, id++);
        cap(top + ImVec2{unit * 1.75f, unit * 3.1f}, 1.75f, joystick_key(number, JoyLine::Accel), "ACCEL", "", true, id++);
    }
};

}  // namespace

float KeyboardEditor::height(const Skin& skin, float scale) const {
    return open_ ? skin.strip_height * scale + (kRows * kUnit + 22.0f) * scale + skin.groove_width * scale : 0.0f;
}

bool KeyboardEditor::take_key(SDL_Scancode scancode) {
    if (!waiting() || host_key_name(scancode).empty()) {
        return false;  // F9-F12 and Pause have no name: they stay the reserved keys
    }
    taken_ = KeyBinding{scancode, *waiting_, -1};
    waiting_.reset();
    return true;
}

KeyboardEditorActions KeyboardEditor::draw(const Keymap& keymap, bool positional, bool edited, const Skin& skin, const SkinFonts& fonts,
                                           float scale, float right) {
    KeyboardEditorActions actions;
    if (!open_) {
        return actions;
    }
    if (taken_) {
        actions.bind = taken_;
        taken_.reset();
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float h = height(skin, scale);
    const float groove = skin.groove_width * scale;
    const float bottom = viewport->Pos.y + viewport->Size.y - strip_space(skin, scale);
    const ImVec2 pos{viewport->Pos.x, bottom - h};
    const float w = viewport->Size.x - right;
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize({w, h});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, to_imgui(skin.deck));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##keyboard_editor", nullptr, kFlags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    actions.keys_held = waiting_.has_value() || ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, {pos.x + w, pos.y + groove}, to_imgui(skin.groove));

    // Header band: the layout's name, what to do, the keys to leave
    const float band = skin.strip_height * scale;
    const float ui_px = skin.font_ui_size * scale;
    const float top = pos.y + groove;
    draw->AddRectFilled({pos.x, top}, {pos.x + w, top + band}, to_imgui(skin.rib));
    const std::string title = tr_format("Key layout: %s", keymap.name.c_str());
    draw->AddText(fonts.strong, ui_px, {pos.x + 12.0f * scale, top + (band - ui_px) / 2.0f}, to_imgui(skin.label), title.c_str());
    const float title_w = fonts.strong->CalcTextSizeA(ui_px, FLT_MAX, 0.0f, title.c_str()).x;
    std::string hint;
    if (waiting_) {
        const NamedKey* named = nullptr;
        for (const NamedKey& n : named_keys()) {
            if (n.key == *waiting_ && !named) {
                named = &n;
            }
        }
        hint = tr_format("Press the host key for %s; a click on it again stops", named ? std::string(named->name).c_str() : "?");
    } else {
        hint = positional ? tr("Click a key, then press the host key for it; a right click clears it")
                          : tr("Symbolic mapping: only the host keys that type no character use this layout");
    }
    const auto band_button = [&](const char* id, const char* text, float right_edge) {
        const float pad_x = ImGui::GetStyle().FramePadding.x;
        const float bw = fonts.ui->CalcTextSizeA(ui_px, FLT_MAX, 0.0f, text).x + pad_x * 2.0f;
        const ImVec2 at{right_edge - bw, top};
        ImGui::SetCursorScreenPos(at);
        const bool clicked = ImGui::InvisibleButton(id, {bw, band});
        if (ImGui::IsItemHovered()) {
            draw->AddRectFilled(at, at + ImVec2{bw, band}, to_imgui(skin.key_hover));
        }
        draw->AddText(fonts.ui, ui_px, {at.x + pad_x, top + (band - ui_px) / 2.0f}, to_imgui(skin.label), text);
        return std::pair{clicked, at.x};
    };
    const auto [closed, close_x] = band_button("##kb_close", tr("Close"), pos.x + w - 4.0f * scale);
    if (closed) {
        close();
        actions.close = true;
    }
    float buttons_x = close_x;
    if (edited) {
        const auto [reset, reset_x] = band_button("##kb_reset", tr("Back to the shipped layout"), close_x - 4.0f * scale);
        actions.reset = reset;
        buttons_x = reset_x;
    }
    const RoleColour& notice = skin.role(Role::Notice);
    const ImVec2 hint_at{pos.x + 30.0f * scale + title_w, top + (band - ui_px) / 2.0f};
    draw->PushClipRect({hint_at.x, top}, {buttons_x - 8.0f * scale, top + band}, true);
    draw->AddText(fonts.ui, ui_px, hint_at, to_imgui(waiting_ ? notice.on_case : skin.muted), hint.c_str());
    draw->PopClipRect();

    // The keyboard and the joysticks, as large as the panel allows
    const float body_top = top + band + 8.0f * scale;
    const float unit = std::min(kUnit * scale, (w - 24.0f * scale) / kWidthUnits);
    Drawer d{keymap, skin, fonts, scale, unit, {}, waiting_, actions};
    int count = 0;
    const bool* state = SDL_GetKeyboardState(&count);
    for (int sc = 0; sc < count; ++sc) {
        if (state[sc]) {
            if (const KeyBinding* b = keymap.find(static_cast<SDL_Scancode>(sc))) {
                d.lit[key_index(b->key)] = true;
            }
        }
    }
    const ImVec2 origin{pos.x + 12.0f * scale, body_top};
    int id = 0;
    d.row(origin, kRow0, id);
    d.row(origin + ImVec2{0.0f, unit}, kRow1, id);
    d.row(origin + ImVec2{0.0f, unit * 2.0f}, kRow2, id);
    d.row(origin + ImVec2{0.0f, unit * 3.0f}, kRow3, id);
    d.row(origin + ImVec2{0.0f, unit * 4.0f}, kRow4, id);
    d.joystick(origin + ImVec2{unit * 19.5f, 0.0f}, 1, tr("Joystick 1"), id);
    d.joystick(origin + ImVec2{unit * 23.5f, 0.0f}, 2, tr("Joystick 2"), id);
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered() && waiting_) {
        waiting_.reset();  // a click beside the keys stops waiting
    }
    ImGui::End();
    return actions;
}

}  // namespace freetvc::sdl
