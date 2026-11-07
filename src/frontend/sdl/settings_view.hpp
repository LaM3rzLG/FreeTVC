// SPDX-License-Identifier: GPL-3.0-or-later
// The settings window: a sheet at the right of the window, every freetvc.ini key in sections (design/ui.md "Settings window").
#pragma once

#include <array>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "frontend/sdl/keymap.hpp"
#include "frontend/sdl/picture.hpp"
#include "frontend/sdl/play_view.hpp"
#include "frontend/sdl/skin.hpp"
#include "frontend/sdl/skin_style.hpp"

namespace freetvc::sdl {

enum class SettingsSection { Machine, Picture, Sound, Keyboard, Interface, Remote };

struct ServerStatus {
    bool on = false;
    int port = 0;          // the port asked for (0 = any free one)
    int listening = 0;     // the port it listens on while on
    bool given = false;    // set on the command line for this run
    std::string_view error;  // why the last start failed
};

struct SettingsStatus {
    std::string_view file;  // freetvc.ini's path
    bool portable = false;
    std::string_view rom_source;  // "DIR (why)" of the ROMs in use
    std::string_view rom_dir;     // freetvc.ini's rom_dir; empty = the default places
    bool rom_dir_given = false;   // --rom-dir or FREETVC_ROM_DIR decides this run
    bool downloading = false;
    std::string_view disk_rom;    // "1.1", "1.2" or a file
    bool disk_rom_given = false;
    bool preset_given = false;
    bool skin_given = false;
    bool keymap_given = false;
    bool language_given = false;
    bool audio_at_start = true;
    bool audio_given = false;
    int volume = 100;
    bool fullscreen_at_start = false;
    // The picture
    std::string_view display;      // what draws: OpenGL ... or SDL renderer (...)
    bool effects = false;          // the picture model runs on it
    bool renderer_sdl = false;     // freetvc.ini asks for SDL's renderer
    std::span<const PictureEntry> pictures;
    const Picture* picture = nullptr;  // the one in use
    bool picture_given = false;
    bool picture_edited = false;   // it is the user data folder's copy
    std::string_view picture_error;
    bool colour_switch = true;     // the TVC's rear switch
    std::span<const KeymapEntry> layouts;  // host key layouts
    std::string_view layout;       // the one in use
    bool layout_given = false;
    // Text monitor, MCP, DZRP, GDB stub
    std::array<ServerStatus, 4> servers{};
    std::string_view mcp_token;
    bool mcp_allow_monitor = false;
    bool dzrp_banks = false;
    int text_size = 22;              // the menus' and this sheet's text, in points
    int strip_size = 100;            // the status strip's height and keys, in percent
};

enum Server : std::size_t { kMonitorServer, kMcpServer, kDzrpServer, kGdbServer };

struct SettingsActions {
    bool choose_rom_dir = false;  // folder dialog, then it becomes rom_dir
    bool default_rom_dir = false;  // rom_dir removed: the default places
    std::string disk_rom;          // "1.1" or "1.2"; empty = no change
    bool choose_disk_rom = false;  // file dialog, then that image
    int volume = -1;
    int audio_at_start = -1;       // 0 or 1; -1 = no change
    int fullscreen_at_start = -1;
    std::array<int, 4> server_on{-1, -1, -1, -1};    // 0 off, 1 on
    std::array<int, 4> server_port{-1, -1, -1, -1};  // a new port
    int mcp_allow_monitor = -1;
    int dzrp_banks = -1;
    bool copy_mcp_token = false;
    std::string picture;               // a picture file's path to switch to
    std::optional<Picture> picture_edit;  // new values, shown at once
    bool picture_edit_done = false;    // a slider let go: the values go into the user data folder's copy
    bool picture_reset = false;        // that copy removed: the shipped values back
    int colour_switch = -1;            // 0 B/W, 1 colour
    int renderer_sdl = -1;
    std::string layout;                // a layout file's path to switch to
    bool edit_layout = false;          // the keyboard editor opens
    int text_size = -1;                // a new text size in points
    int strip_size = -1;               // a new strip size in percent
};

class SettingsView {
public:
    void toggle() { open_ = !open_; }
    void close() { open_ = false; }
    bool is_open() const { return open_; }
    // The sheet's width, which the screen (Play) and the dock space (Bench) give up; 0 while closed.
    float width(const Skin& skin, const SkinFonts& fonts, float scale, float window_width) const;
    // From `top` down to the strip. The sheet's changes that are also menu actions (preset, cards, skin...) go to `play`.
    SettingsActions draw(const PlayStatus& status, const SettingsStatus& settings, const Skin& skin, const SkinFonts& fonts,
                         float top, PlayActions& play);

private:
    bool open_ = false;
    SettingsSection section_ = SettingsSection::Machine;
    std::array<int, 4> port_edit_{};  // the port fields while being typed into
    std::array<bool, 4> port_editing_{};
};

}  // namespace freetvc::sdl
