// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/sdl/settings_view.hpp"

#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <filesystem>

#include "frontend/sdl/deck.hpp"
#include "frontend/sdl/text.hpp"
#include "imgui_internal.h"

namespace freetvc::sdl {

namespace {

constexpr float kContentWidth = 360.0f;
constexpr SettingsSection kSections[] = {SettingsSection::Machine, SettingsSection::Picture, SettingsSection::Sound,
                                         SettingsSection::Keyboard, SettingsSection::Interface, SettingsSection::Remote};

struct Sheet {
    const Skin& skin;
    const SkinFonts& fonts;
    float scale;
};

void heading(const Sheet& s, const char* text) {
    ImGui::Dummy({0.0f, 6.0f * s.scale});
    ImGui::PushFont(s.fonts.strong, 0.0f);
    ImGui::TextUnformatted(text);
    ImGui::PopFont();
    const ImVec2 at = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddRectFilled(at, {at.x + ImGui::GetContentRegionAvail().x, at.y + s.scale}, to_imgui(s.skin.rib));
    ImGui::Dummy({0.0f, 3.0f * s.scale});
}

void note(const Sheet& s, const char* text) {
    ImGui::PushStyleColor(ImGuiCol_Text, to_imgui(s.skin.muted));
    ImGui::TextWrapped("%s", text);
    ImGui::PopStyleColor();
}

// A setting the command line decides for this run
void given(const Sheet& s, bool is_given, const char* option) {
    if (!is_given) {
        return;
    }
    ImGui::PushStyleColor(ImGuiCol_Text, to_imgui(s.skin.role(Role::Notice).on_case));
    ImGui::TextWrapped("%s", tr_format("Set by %s for this run; a change here is saved", option).c_str());
    ImGui::PopStyleColor();
}

bool check(const char* label, bool value) {
    bool v = value;
    return ImGui::Checkbox(label, &v);
}

const char* section_name(SettingsSection section) {
    switch (section) {
    case SettingsSection::Machine:
        return tr("Machine");
    case SettingsSection::Picture:
        return tr("Picture");
    case SettingsSection::Sound:
        return tr("Sound");
    case SettingsSection::Keyboard:
        return tr("Keyboard and joysticks");
    case SettingsSection::Interface:
        return tr("Interface");
    case SettingsSection::Remote:
        return tr("Remote");
    }
    return "";
}

void draw_machine(const Sheet& s, const PlayStatus& status, const SettingsStatus& st, PlayActions& play, SettingsActions& a) {
    // The model and the cards are in the strip's machine and Cards menus
    heading(s, tr("ROM folder"));
    note(s, tr_format("In use: %s", std::string(st.rom_source).c_str()).c_str());
    if (st.rom_dir.empty()) {
        note(s, tr("No folder chosen: roms/ next to the program, then roms/ in the user data folder."));
    } else {
        ImGui::PushFont(s.fonts.data, 0.0f);
        ImGui::TextWrapped("%s", std::string(st.rom_dir).c_str());
        ImGui::PopFont();
    }
    given(s, st.rom_dir_given, "--rom-dir / FREETVC_ROM_DIR");
    ImGui::BeginDisabled(!status.can_browse);
    a.choose_rom_dir |= ImGui::Button(tr("Choose folder..."));
    ImGui::EndDisabled();
    if (!st.rom_dir.empty()) {
        ImGui::SameLine();
        a.default_rom_dir |= ImGui::Button(tr("Default places"));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(st.downloading);
    play.fetch_roms |= ImGui::Button(st.downloading ? tr("Downloading...") : tr("Download ROMs"));
    ImGui::EndDisabled();
}

// A slider for one picture value; true while it changes, `done` when it is let go
bool picture_slider(const char* label, float& value, float min, float max, const char* format, bool& done) {
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
    const bool changed = ImGui::SliderFloat(label, &value, min, max, format);
    done |= ImGui::IsItemDeactivatedAfterEdit();
    return changed;
}

// The shipped pictures by what they show; another file by its name
const char* picture_label(const std::string& name) {
    if (name == "bw-tv") {
        return tr("Black and white TV");
    }
    if (name == "pal-tv") {
        return tr("CRT TV emulation");
    }
    if (name == "rgb-monitor") {
        return tr("RGB monitor emulation");
    }
    if (name == "sharp") {
        return tr("Sharp picture");
    }
    return name.c_str();
}

void draw_picture(const Sheet& s, const PlayStatus& status, const SettingsStatus& st, PlayActions& play, SettingsActions& a) {
    heading(s, tr("Picture"));
    given(s, st.picture_given, "--picture");
    for (const PictureEntry& entry : st.pictures) {
        const bool current = st.picture && entry.name == st.picture->name;
        if (ImGui::RadioButton(picture_label(entry.name), current) && !current) {
            a.picture = entry.path.string();
        }
    }
    if (!st.picture_error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, to_imgui(s.skin.role(Role::Notice).on_case));
        ImGui::TextWrapped("%s", std::string(st.picture_error).c_str());
        ImGui::PopStyleColor();
    }
    if (!st.effects) {
        note(s, tr("Only sharp pixels here: the picture model needs OpenGL 3.3."));
    }
    if (st.picture && st.effects && st.picture->signal != Signal::Sharp) {
        Picture p = *st.picture;
        bool changed = false;
        bool done = false;
        if (p.signal == Signal::Pal || p.signal == Signal::Mono) {
            heading(s, tr("Signal"));
            changed |= picture_slider(tr("Luma bandwidth"), p.luma_mhz, 0.5f, 6.0f, "%.1f MHz", done);
            if (p.signal == Signal::Pal) {
                changed |= picture_slider(tr("Chroma bandwidth"), p.chroma_mhz, 0.3f, 3.0f, "%.1f MHz", done);
                changed |= picture_slider(tr("Saturation"), p.saturation, 0.0f, 2.0f, "%.2f", done);
                changed |= picture_slider(tr("Chroma phase error"), p.phase_error, -45.0f, 45.0f, "%.0f deg", done);
                if (check(tr("PAL delay line"), p.delay_line)) {
                    p.delay_line = !p.delay_line;
                    changed = done = true;
                }
                note(s, tr("Without the delay line a phase error shows as Hanover bars: lines of alternating hue."));
            } else {
                note(s, tr("A black and white set has no chroma trap: the subcarrier shows as fine dots in coloured areas."));
            }
        } else {
            heading(s, tr("Beam"));
            changed |= picture_slider(tr("Spot width"), p.spot, 0.1f, 2.0f, "%.2f px", done);
        }
        heading(s, tr("Tube"));
        changed |= picture_slider(tr("Beam on black"), p.beam_min, 0.1f, 1.0f, "%.2f", done);
        changed |= picture_slider(tr("Beam on white"), p.beam_max, 0.1f, 1.0f, "%.2f", done);
        p.beam_max = std::max(p.beam_max, p.beam_min);
        const char* masks[] = {tr("No mask"), tr("Aperture grille"), tr("Shadow mask")};
        for (int m = 0; m < 3; ++m) {
            if (ImGui::RadioButton(masks[m], p.mask == static_cast<Mask>(m)) && p.mask != static_cast<Mask>(m)) {
                p.mask = static_cast<Mask>(m);
                changed = done = true;
            }
            if (m < 2) {
                ImGui::SameLine();
            }
        }
        if (p.mask != Mask::None) {
            changed |= picture_slider(tr("Mask strength"), p.mask_strength, 0.0f, 1.0f, "%.2f", done);
            changed |= picture_slider(tr("Mask pitch"), p.mask_pitch, 2.0f, 12.0f, "%.1f px", done);
        }
        changed |= picture_slider(tr("Glow"), p.glow, 0.0f, 1.0f, "%.2f", done);
        changed |= picture_slider(tr("Curvature"), p.curvature, 0.0f, 0.2f, "%.3f", done);
        changed |= picture_slider(tr("Vignette"), p.vignette, 0.0f, 1.0f, "%.2f", done);
        changed |= picture_slider(tr("Tube gamma"), p.crt_gamma, 1.0f, 3.0f, "%.2f", done);
        changed |= picture_slider(tr("Brightness"), p.brightness, 0.2f, 2.0f, "%.2f", done);
        if (changed || done) {
            a.picture_edit = p;
            a.picture_edit_done = done;
        }
        if (st.picture_edited) {
            note(s, tr("Your values are kept in the user data folder's copy of this picture."));
            a.picture_reset |= ImGui::Button(tr("Back to the shipped values"));
        }
    }

    heading(s, tr("The TVC's rear switch"));
    if (ImGui::RadioButton(tr("Colour"), st.colour_switch) && !st.colour_switch) {
        a.colour_switch = 1;
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(tr("Black and white"), !st.colour_switch) && st.colour_switch) {
        a.colour_switch = 0;
    }
    note(s, tr("Off, the TVC sends no colour subcarrier: a TV shows grey. Programs can read it at port 59h."));

    heading(s, tr("Screen"));
    play.toggle_fullscreen |= check(tr("Full screen (F11)"), status.fullscreen);
    if (check(tr("Start in full screen"), st.fullscreen_at_start)) {
        a.fullscreen_at_start = st.fullscreen_at_start ? 0 : 1;
    }
    play.toggle_full_raster |= check(tr("Full raster (debug)"), status.full_raster);
    note(s, tr("Full raster shows the whole beam path, sync and blanking included, with the TV area outlined."));

    heading(s, tr("Drawing"));
    ImGui::PushFont(s.fonts.data, 0.0f);
    ImGui::TextWrapped("%s", std::string(st.display).c_str());
    ImGui::PopFont();
    if (check(tr("Use SDL's renderer (no picture model)"), st.renderer_sdl)) {
        a.renderer_sdl = st.renderer_sdl ? 0 : 1;
    }
    note(s, tr("From the next start. For a graphics driver without OpenGL 3.3, which freeTVC also finds by itself."));
}

void draw_sound(const Sheet& s, const PlayStatus& status, const SettingsStatus& st, PlayActions& play, SettingsActions& a) {
    heading(s, tr("Sound"));
    ImGui::BeginDisabled(!status.audio);
    play.toggle_mute |= check(tr("Mute"), status.muted);
    int volume = st.volume;
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.6f);
    if (ImGui::SliderInt(tr("Volume"), &volume, 0, 100, "%d %%")) {
        a.volume = volume;
    }
    ImGui::EndDisabled();
    if (!status.audio) {
        note(s, tr("No sound device is open in this run."));
    }
    if (check(tr("Sound output"), st.audio_at_start)) {
        a.audio_at_start = st.audio_at_start ? 0 : 1;
    }
    given(s, st.audio_given, "--no-audio");
    note(s, tr("From the next start. With sound the emulation keeps time by the sound device, without it by the clock."));
}

void draw_keyboard(const Sheet& s, const PlayStatus& status, const SettingsStatus& st, PlayActions& play, SettingsActions& a) {
    heading(s, tr("Host keyboard"));
    given(s, st.keymap_given, "--keymap");
    if (ImGui::RadioButton(tr("Symbolic"), status.mapping == KeyMapping::Symbolic) && status.mapping != KeyMapping::Symbolic) {
        play.mapping = static_cast<int>(KeyMapping::Symbolic);
    }
    note(s, tr("The character a host key gives is typed on the TVC, with SHIFT as the TVC needs it."));
    if (ImGui::RadioButton(tr("Positional"), status.mapping == KeyMapping::Positional) && status.mapping != KeyMapping::Positional) {
        play.mapping = static_cast<int>(KeyMapping::Positional);
    }
    note(s, tr("Each host key is the TVC key the layout below gives it; for games."));

    heading(s, tr("Key layout"));
    given(s, st.layout_given, "--keys");
    for (const KeymapEntry& entry : st.layouts) {
        if (ImGui::RadioButton(entry.name.c_str(), entry.name == st.layout) && entry.name != st.layout) {
            a.layout = entry.path.string();
        }
    }
    a.edit_layout |= ImGui::Button(tr("Edit the layout..."));
    note(s, tr("hungarian: the keys by their place on a Hungarian PC keyboard, the TVC's extra keys on F1-F8. "
               "us: digits and letters by their legend on a US keyboard."));

    heading(s, tr("Joysticks"));
    note(s, tr("Joystick 1: the arrow keys, fire on Right Ctrl. Joystick 2: keypad 8, 2, 4, 6, fire on keypad 0. "
               "Gamepads 1 to 4 drive joysticks 1 to 4; 3 and 4 are the GameCard's."));
}

void draw_interface(const Sheet& s, const PlayStatus& status, const SettingsStatus& st, PlayActions& play, SettingsActions& a) {
    heading(s, tr("Text"));
    // A list, not a slider: every new size builds the fonts again and moves the sheet under the mouse
    static constexpr int kSizes[] = {12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32, 36};
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("00 pt").x + ImGui::GetFrameHeight() * 2.0f);
    const std::string size_text = std::to_string(st.text_size) + " pt";
    if (ImGui::BeginCombo("##text_size", size_text.c_str(), ImGuiComboFlags_HeightLargest)) {
        for (const int size : kSizes) {
            const std::string text = std::to_string(size) + " pt";
            if (ImGui::Selectable(text.c_str(), size == st.text_size) && size != st.text_size) {
                a.text_size = size;
            }
        }
        ImGui::EndCombo();
    }
    note(s, tr("Text size in points, for the menus, this window and the strip."));

    heading(s, tr("Status strip"));
    static constexpr int kStripSizes[] = {75, 100, 125, 150, 175, 200, 250};
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("000 %").x + ImGui::GetFrameHeight() * 2.0f);
    const std::string strip_text = std::to_string(st.strip_size) + " %";
    if (ImGui::BeginCombo("##strip_size", strip_text.c_str(), ImGuiComboFlags_HeightLargest)) {
        for (const int size : kStripSizes) {
            const std::string text = std::to_string(size) + " %";
            if (ImGui::Selectable(text.c_str(), size == st.strip_size) && size != st.strip_size) {
                a.strip_size = size;
            }
        }
        ImGui::EndCombo();
    }
    note(s, tr("The height of the strip at the bottom and of its keys."));

    heading(s, tr("Language"));
    given(s, st.language_given, "--language");
    if (ImGui::RadioButton("English", status.language == "en") && status.language != "en") {
        play.language = "en";
    }
    for (const LanguageFile& file : status.languages) {
        if (ImGui::RadioButton(file.name.c_str(), status.language == file.code) && status.language != file.code) {
            play.language = file.code;
        }
    }

    heading(s, tr("Skin"));
    given(s, st.skin_given, "--skin");
    for (const SkinEntry& entry : status.skins) {
        if (ImGui::RadioButton(entry.name.c_str(), entry.name == status.skin_name) && entry.name != status.skin_name) {
            play.skin = entry.path.string();
        }
    }
    play.reload_skin |= ImGui::Button(tr("Reload skin"));
    note(s, tr("A skin file is watched while in use: saving it in an editor applies it."));

    heading(s, tr("Settings file"));
    ImGui::PushFont(s.fonts.data, 0.0f);
    ImGui::TextWrapped("%s", std::string(st.file).c_str());
    ImGui::PopFont();
    note(s, st.portable ? tr("Next to the program (portable): this folder keeps its own settings.")
                        : tr("In the user data folder. A freetvc.ini next to the program is used instead when there is one."));
}

void draw_server(const Sheet& s, const SettingsStatus& st, std::size_t which, const char* name, const char* option, int& port_edit,
                 bool& editing, SettingsActions& a) {
    const ServerStatus& server = st.servers[which];
    ImGui::PushID(static_cast<int>(which));
    if (check(name, server.on)) {
        a.server_on[which] = server.on ? 0 : 1;
    }
    ImGui::SameLine(ImGui::GetContentRegionAvail().x * 0.55f);
    ImGui::TextUnformatted(tr("port"));
    ImGui::SameLine();
    if (!editing) {
        port_edit = server.port;
    }
    ImGui::SetNextItemWidth(90.0f * s.scale);
    ImGui::InputInt("##port", &port_edit, 0, 0);
    editing = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) {
        port_edit = std::clamp(port_edit, 1, 65535);
        if (port_edit != server.port) {
            a.server_port[which] = port_edit;
        }
    }
    given(s, server.given, option);
    if (!server.error.empty()) {
        ImGui::PushStyleColor(ImGuiCol_Text, to_imgui(s.skin.role(Role::Notice).on_case));
        ImGui::TextWrapped("%s", std::string(server.error).c_str());
        ImGui::PopStyleColor();
    } else if (server.on) {
        note(s, tr_format("Listening on 127.0.0.1:%d", server.listening).c_str());
    }
    ImGui::PopID();
}

void draw_remote(const Sheet& s, const SettingsStatus& st, std::array<int, 4>& port_edit, std::array<bool, 4>& editing, SettingsActions& a) {
    heading(s, tr("Servers"));
    note(s, tr("Each listens on 127.0.0.1 only and starts or stops at once."));
    draw_server(s, st, kMonitorServer, tr("Text monitor"), "--remote-monitor", port_edit[0], editing[0], a);
    ImGui::Dummy({0.0f, 4.0f * s.scale});
    draw_server(s, st, kMcpServer, tr("MCP server (HTTP)"), "--mcp-http", port_edit[1], editing[1], a);
    if (st.servers[kMcpServer].on) {
        ImGui::TextUnformatted(tr("Token"));
        ImGui::SameLine();
        ImGui::PushFont(s.fonts.data, 0.0f);
        ImGui::TextUnformatted(std::string(st.mcp_token).c_str());
        ImGui::PopFont();
        ImGui::SameLine();
        a.copy_mcp_token |= ImGui::SmallButton(tr("Copy"));
        note(s, tr("A new token each run, unless --mcp-token gives one. Clients send it as Authorization: Bearer."));
    }
    if (check(tr("Allow monitor commands over MCP"), st.mcp_allow_monitor)) {
        a.mcp_allow_monitor = st.mcp_allow_monitor ? 0 : 1;
    }
    ImGui::Dummy({0.0f, 4.0f * s.scale});
    draw_server(s, st, kDzrpServer, tr("DZRP for DeZog"), "--dzrp", port_edit[2], editing[2], a);
    if (check(tr("TVC banks (needs freeTVC's DeZog patch)"), st.dzrp_banks)) {
        a.dzrp_banks = st.dzrp_banks ? 0 : 1;
    }
    ImGui::Dummy({0.0f, 4.0f * s.scale});
    draw_server(s, st, kGdbServer, tr("GDB stub"), "--gdb", port_edit[3], editing[3], a);
}

}  // namespace

namespace {

// The section column: the widest name (in the UI language) after its rib
// The sheet's text: as large as the strip's menus
float text_size(const Skin& skin) { return skin.font_strip_size > 0.0f ? skin.font_strip_size : skin.font_ui_size; }

float sections_width(const Skin& skin, const SkinFonts& fonts, float scale) {
    float widest = 0.0f;
    for (const SettingsSection section : kSections) {
        widest = std::max(widest, fonts.strong->CalcTextSizeA(text_size(skin) * scale, FLT_MAX, 0.0f, section_name(section)).x);
    }
    return widest + 44.0f * scale;
}

}  // namespace

float SettingsView::width(const Skin& skin, const SkinFonts& fonts, float scale, float window_width) const {
    const float content = kContentWidth * scale * text_size(skin) / skin.font_ui_size;
    return open_ ? std::min(sections_width(skin, fonts, scale) + content, window_width * 0.65f) : 0.0f;
}

SettingsActions SettingsView::draw(const PlayStatus& status, const SettingsStatus& settings, const Skin& skin, const SkinFonts& fonts,
                                   float top, PlayActions& play) {
    SettingsActions actions;
    if (!open_) {
        return actions;
    }
    const Sheet s{skin, fonts, status.scale};
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float w = width(skin, fonts, status.scale, viewport->Size.x);
    const float bottom = viewport->Pos.y + viewport->Size.y - strip_space(skin, status.scale);
    const float groove = skin.groove_width * status.scale;
    const ImVec2 pos{viewport->Pos.x + viewport->Size.x - w, top};
    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize({w, bottom - top});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {0.0f, 0.0f});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, to_imgui(skin.deck));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoSavedSettings |
                                        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("##settings", nullptr, kFlags);
    ImGui::PopStyleColor();
    ImGui::PopStyleVar(2);
    // Above Bench's panels, but under its own popups (the text size list)
    if (!ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
        ImGui::BringWindowToDisplayFront(ImGui::GetCurrentWindow());
    }
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        play.menu_open = true;  // keys stay out of the TVC while the sheet has them
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !ImGui::IsAnyItemActive()) {
            open_ = false;
        }
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled(pos, {pos.x + groove, bottom}, to_imgui(skin.groove));

    // Header band: the name and a key to close it
    const float band = skin.strip_height * status.scale;
    const float ui_px = text_size(skin) * status.scale;
    draw->AddRectFilled({pos.x + groove, pos.y}, {pos.x + w, pos.y + band}, to_imgui(skin.rib));
    draw->AddText(fonts.strong, ui_px, {pos.x + groove + 12.0f * status.scale, pos.y + (band - ui_px) / 2.0f}, to_imgui(skin.label),
                  tr("Settings"));
    const char* close_label = tr("Close");
    const float pad_x = ImGui::GetStyle().FramePadding.x;
    const float close_w = fonts.ui->CalcTextSizeA(ui_px, FLT_MAX, 0.0f, close_label).x + pad_x * 2.0f;
    const ImVec2 close_at{pos.x + w - close_w - 4.0f * status.scale, pos.y};
    ImGui::SetCursorScreenPos(close_at);
    if (ImGui::InvisibleButton("##settings_close", {close_w, band})) {
        open_ = false;
    }
    if (ImGui::IsItemHovered()) {
        draw->AddRectFilled(close_at, {close_at.x + close_w, close_at.y + band}, to_imgui(skin.key_hover));
    }
    draw->AddText(fonts.ui, ui_px, {close_at.x + pad_x, pos.y + (band - ui_px) / 2.0f}, to_imgui(skin.label), close_label);

    // Sections down the left, the current one marked with a lit slot rib
    const float body_top = pos.y + band;
    const float sections_w = sections_width(skin, fonts, status.scale);
    const float row_h = skin.strip_height * status.scale;
    const float rib_w = 5.0f * status.scale;
    const float slant = 3.0f * status.scale;
    for (std::size_t i = 0; i < std::size(kSections); ++i) {
        const SettingsSection section = kSections[i];
        const ImVec2 at{pos.x + groove, body_top + 6.0f * status.scale + static_cast<float>(i) * row_h};
        ImGui::SetCursorScreenPos(at);
        ImGui::PushID(static_cast<int>(i));
        if (ImGui::InvisibleButton("##section", {sections_w - groove, row_h})) {
            section_ = section;
        }
        ImGui::PopID();
        const bool current = section == section_;
        if (ImGui::IsItemHovered() && !current) {
            draw->AddRectFilled(at, {at.x + sections_w - groove, at.y + row_h}, to_imgui(skin.key_hover));
        }
        const float rib_top = at.y + row_h * 0.22f;
        const float rib_bottom = at.y + row_h * 0.78f;
        const float x = at.x + 10.0f * status.scale;
        draw->AddQuadFilled({x + slant, rib_top}, {x + slant + rib_w, rib_top}, {x + rib_w, rib_bottom}, {x, rib_bottom},
                            to_imgui(current ? skin.label : skin.rib));
        ImFont* font = current ? fonts.strong : fonts.ui;
        const char* name = section_name(section);
        draw->PushClipRect(at, {at.x + sections_w - groove, at.y + row_h}, true);
        draw->AddText(font, ui_px, {x + rib_w + slant + 8.0f * status.scale, at.y + (row_h - ui_px) / 2.0f},
                      to_imgui(current ? skin.label : skin.muted), name);
        draw->PopClipRect();
    }
    draw->AddRectFilled({pos.x + sections_w, body_top}, {pos.x + sections_w + groove, bottom}, to_imgui(skin.groove));

    // The section's settings, scrolled when they do not fit
    const float pad = 12.0f * status.scale;
    ImGui::SetCursorScreenPos({pos.x + sections_w + groove, body_top});
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, {pad, pad});
    ImGui::PushStyleColor(ImGuiCol_ChildBg, to_imgui(skin.deck));
    ImGui::BeginChild("##settings_body", {w - sections_w - groove, bottom - body_top}, ImGuiChildFlags_AlwaysUseWindowPadding);
    ImGui::PushFont(nullptr, text_size(skin));
    ImGui::PushTextWrapPos(0.0f);
    switch (section_) {
    case SettingsSection::Machine:
        draw_machine(s, status, settings, play, actions);
        break;
    case SettingsSection::Picture:
        draw_picture(s, status, settings, play, actions);
        break;
    case SettingsSection::Sound:
        draw_sound(s, status, settings, play, actions);
        break;
    case SettingsSection::Keyboard:
        draw_keyboard(s, status, settings, play, actions);
        break;
    case SettingsSection::Interface:
        draw_interface(s, status, settings, play, actions);
        break;
    case SettingsSection::Remote:
        draw_remote(s, settings, port_edit_, port_editing_, actions);
        break;
    }
    ImGui::PopTextWrapPos();
    ImGui::PopFont();
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::End();
    return actions;
}

}  // namespace freetvc::sdl
