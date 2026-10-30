// SPDX-License-Identifier: GPL-3.0-or-later
// The settings file freetvc.ini: `key = value` lines, ';' comments (design/ui.md "Settings file and ROM search").
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace freetvc {

// Every key is optional; a missing one keeps the program's default.
struct Settings {
    std::optional<std::string> rom_dir;  // made absolute against the file's folder
    std::optional<std::string> preset;
    std::optional<std::string> skin;     // a skin name or a path
    std::optional<std::string> keymap;   // symbolic or positional
    std::optional<std::string> language;  // the UI language: en or a translation's code, such as hu
    std::optional<bool> audio;
    std::optional<bool> full_raster;
    std::optional<bool> disk_card;  // the HBF card in slot 0
    std::optional<bool> gamecard;   // the GameCard in slot 1
    std::optional<bool> multichip;  // the Multichip sound card in slot 2
    std::optional<std::string> sid_card;  // the SID card at ports E0h-FFh: off, 6581 or 8580
    std::optional<bool> card256;    // the TVC256k++ in slot 0 or the first free one
    std::optional<std::string> card256_usb;    // its USB port: none, mouse, or the pendrive's folder (made absolute)
    std::optional<std::string> card256_flash;  // its flash drive: an image or a folder (made absolute)
    std::optional<bool> nanosd;                // the NanoSD in slot 3
    std::optional<std::string> nanosd_sd;      // its SD card: none, or a folder (made absolute)
    std::optional<bool> nanosd_menu;           // the Nano's EEPROM: the file-select menu at boot
    std::optional<std::uint8_t> nanosd_sort;   // and its sort order: 0 none, 1 name, 2 size
    std::optional<std::string> printer;        // the printer: off, on (kept in memory), or its file (made absolute)
    std::optional<bool> serial_card;           // the serial card in slot 1
    std::optional<std::string> serial_line;    // its cable: off, on, listen[:PORT] or connect:PORT
    std::optional<std::string> serial_flow;    // rts or none
    std::optional<std::string> disk_rom;       // the disk card's EPROM: 1.1 (hbf.rom), 1.2 (hbf12.rom) or a file (made absolute)
    std::optional<int> volume;                 // 0-100
    std::optional<bool> fullscreen;            // the GUI starts in full screen
    // The remote servers at start: kServerOff, kServerDefaultPort ("on") or a port
    std::optional<int> monitor;
    std::optional<int> mcp;
    std::optional<int> dzrp;
    std::optional<int> gdb;
    std::optional<bool> dzrp_banks;
    std::optional<bool> mcp_allow_monitor;
    std::optional<std::string> renderer;       // gl (default) or sdl
    std::optional<std::string> picture;        // a picture file's name or path
    std::optional<bool> colour_switch;         // the TVC's rear switch: colour (yes) or B/W
    std::optional<std::string> keys;           // the host key layout's name or path
};

inline constexpr int kServerOff = -1;
inline constexpr int kServerDefaultPort = 0;

struct SettingsFile {
    std::filesystem::path path;  // read from and written to; may not exist yet
    bool portable = false;       // next to the program
    Settings values;
    std::vector<std::string> errors;  // "freetvc.ini line N: ..."
};

// Next to the program when a freetvc.ini is there (portable), else in the user data folder.
std::filesystem::path settings_path(const std::filesystem::path& program_dir, const std::filesystem::path& user_dir,
                                    bool* portable = nullptr);
// Unknown keys and bad values are reported in `errors` and skipped; a later line wins over an earlier one.
Settings parse_settings(std::string_view text, const std::filesystem::path& base, std::vector<std::string>& errors);
SettingsFile load_settings(const std::filesystem::path& program_dir, const std::filesystem::path& user_dir);

using SettingValues = std::vector<std::pair<std::string, std::string>>;
// The file's text with these keys set: their lines are rewritten in place (comments, other keys and the order are kept)
// and missing keys are appended; an empty value removes the key.
std::string update_settings_text(std::string_view text, const SettingValues& values);
bool save_settings(const std::filesystem::path& path, const SettingValues& values, std::string& error);

}  // namespace freetvc
