// SPDX-License-Identifier: GPL-3.0-or-later
// Skins: every colour, font and size of the GUI, read from plain text files (design/ui.md "Skins"). No SDL or ImGui here.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace freetvc::sdl {

struct Colour {
    std::uint8_t r = 0;
    std::uint8_t g = 0;
    std::uint8_t b = 0;
    std::uint8_t a = 255;
    bool operator==(const Colour&) const = default;
};

// The fixed meanings of the TVC colours; bright variant on case surfaces, dark variant on putty panels.
enum class Role : std::size_t { Exec, Read, Write, Io, Vram, Notice };
inline constexpr std::size_t kRoles = 6;

struct RoleColour {
    Colour on_case;
    Colour on_putty;
    bool operator==(const RoleColour&) const = default;
};

// Bright (I = 1) and dark (I = 0) variants of TVC colour C (0-7, GRB bits as in IGRB).
RoleColour tvc_role_colour(unsigned c);

// Member defaults are the built-in "bronze" skin; data/skins/bronze.skin must say the same (skin_test).
struct Skin {
    std::string name = "bronze";

    Colour frame{0x2a, 0x23, 0x1e};       // around the screen: the case's upper part
    Colour deck{0x3b, 0x32, 0x2b};        // status strip, panels, popups: the keyboard deck
    Colour groove{0x1c, 0x17, 0x13};      // line between frame and deck
    Colour rib{0x4c, 0x40, 0x37};         // panel header bands, separators, empty slot ribs
    Colour key{0x56, 0x50, 0x48};         // controls: the charcoal modifier keys
    Colour key_hover{0x66, 0x5f, 0x56};
    Colour key_active{0x76, 0x6e, 0x63};
    Colour panel{0x44, 0x3a, 0x32};       // dense data panels (Listing, Memory, Console): a step lighter than the deck
    Colour putty{0xdc, 0xd5, 0xc6};       // dense data panels with panel_light: the letter keys
    Colour ink{0x2b, 0x25, 0x20};         // text on putty
    bool panel_light = false;             // dense data panels in putty and ink instead of panel and label
    Colour label{0xec, 0xe6, 0xdc};       // text on the case: the white lettering
    Colour muted{0xb3, 0xa7, 0x96};       // secondary text on the case

    std::array<RoleColour, kRoles> roles{
        tvc_role_colour(2), tvc_role_colour(4), tvc_role_colour(3),
        tvc_role_colour(5), tvc_role_colour(1), tvc_role_colour(6)};

    std::string font_ui = "AtkinsonHyperlegibleNext-Regular.ttf";
    std::string font_strong = "AtkinsonHyperlegibleNext-Medium.ttf";
    std::string font_data = "AtkinsonHyperlegibleMono-Regular.ttf";
    float font_ui_size = 15.0f;
    float font_data_size = 15.0f;
    float font_strip_size = 0.0f;  // the status strip's text; 0 = font_ui_size (data text in proportion)

    float strip_height = 30.0f;
    float screen_margin = 16.0f;
    float groove_width = 2.0f;
    float rounding = 0.0f;

    const RoleColour& role(Role r) const { return roles[static_cast<std::size_t>(r)]; }
    bool operator==(const Skin&) const = default;
};

// Applies "key = value" lines on top of skin; ';' starts a comment. Returns "line N: ..." messages, empty when all lines were good.
std::vector<std::string> parse_skin(std::string_view text, Skin& skin);

// The built-in skin with the file's lines applied; nothing (and error set) when the file is unreadable or has a bad line.
std::optional<Skin> load_skin_file(const std::filesystem::path& path, std::string& error);

struct SkinEntry {
    std::string name;  // file name without ".skin"
    std::filesystem::path path;
};

// Every *.skin in dirs, sorted by name; when two dirs hold the same name, the earlier dir wins.
std::vector<SkinEntry> list_skins(const std::vector<std::filesystem::path>& dirs);

// A path to an existing file, or the name of a skin in dirs.
std::optional<std::filesystem::path> find_skin(std::string_view name_or_path, const std::vector<std::filesystem::path>& dirs);

}  // namespace freetvc::sdl
