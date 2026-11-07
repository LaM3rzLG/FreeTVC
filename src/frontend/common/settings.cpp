// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/common/settings.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <system_error>

#include "core/message.hpp"

namespace freetvc {

namespace {

constexpr std::string_view kFileName = "freetvc.ini";
constexpr std::string_view kHeader =
    "; freeTVC settings (docs/design/ui.md \"Settings file and ROM search\"). The Settings window writes its keys as they\n"
    "; change; on exit freeTVC rewrites preset, skin, keymap, language, the cards and their media. Command-line options are\n"
    "; not saved. Keys: rom_dir, preset, skin, keymap, language, audio, volume, fullscreen, full_raster, disk_card, disk_rom,\n"
    "; gamecard, multichip, sid_card, card256, card256_usb, card256_flash, twodfx, nanosd, nanosd_sd, nanosd_menu, nanosd_sort,\n"
    "; printer, serial_card, serial_line, serial_flow, video_dir, screenshot_dir, save_dir, monitor, mcp, mcp_allow_monitor, dzrp, dzrp_banks, gdb, renderer,\n"
    "; picture, colour_switch, keys, recent.\n";

std::string_view trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(" \t\r") - first + 1);
}

// The key of a `key = value` line, without its comment; empty for comments, blank and malformed lines
std::string_view line_key(std::string_view line) {
    const std::string_view content = trim(line.substr(0, line.find(';')));
    const auto eq = content.find('=');
    return eq == std::string_view::npos ? std::string_view{} : trim(content.substr(0, eq));
}

std::optional<bool> parse_flag(std::string_view value) {
    if (value == "yes" || value == "true" || value == "1") {
        return true;
    }
    if (value == "no" || value == "false" || value == "0") {
        return false;
    }
    return std::nullopt;
}

}  // namespace

std::filesystem::path settings_path(const std::filesystem::path& program_dir, const std::filesystem::path& user_dir,
                                    bool* portable) {
    std::error_code ec;
    const bool next_to_program = !program_dir.empty() && std::filesystem::is_regular_file(program_dir / kFileName, ec);
    if (portable) {
        *portable = next_to_program;
    }
    if (next_to_program) {
        return program_dir / kFileName;
    }
    return user_dir.empty() ? std::filesystem::path() : user_dir / kFileName;
}

Settings parse_settings(std::string_view text, const std::filesystem::path& base, std::vector<std::string>& errors) {
    Settings s;
    int number = 0;
    while (!text.empty()) {
        const auto newline = text.find('\n');
        const std::string_view line = text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
        ++number;
        const std::string_view content = trim(line.substr(0, line.find(';')));
        if (content.empty()) {
            continue;
        }
        const auto eq = content.find('=');
        const auto fail = [&](const std::string& why) { errors.push_back(msg("%s line %d: %s", std::string(kFileName).c_str(), number, why.c_str())); };
        if (eq == std::string_view::npos) {
            fail(msg("expected key = value"));
            continue;
        }
        const std::string_view key = trim(content.substr(0, eq));
        const std::string value(trim(content.substr(eq + 1)));
        if (value.empty()) {
            fail(msg("no value for '%s'", std::string(key).c_str()));
        } else if (key == "rom_dir" || key == "card256_flash" || key == "video_dir" || key == "screenshot_dir" || key == "save_dir") {
            const std::filesystem::path dir(value);
            (key == "rom_dir" ? s.rom_dir : key == "video_dir" ? s.video_dir : key == "screenshot_dir" ? s.screenshot_dir : key == "save_dir" ? s.save_dir : s.card256_flash) = (dir.is_relative() && !base.empty() ? base / dir : dir).lexically_normal().string();
        } else if (key == "recent") {
            s.recent = value;
        } else if (key == "nanosd_sd") {
            const std::filesystem::path dir(value);
            s.nanosd_sd = value == "none" ? value : (dir.is_relative() && !base.empty() ? base / dir : dir).lexically_normal().string();
        } else if (key == "nanosd_sort") {
            if (value == "none" || value == "name" || value == "size") {
                s.nanosd_sort = static_cast<std::uint8_t>(value == "none" ? 0 : value == "name" ? 1 : 2);
            } else {
                fail(msg("nanosd_sort is none, name or size"));
            }
        } else if (key == "card256_usb") {
            const std::filesystem::path dir(value);
            s.card256_usb = value == "none" || value == "mouse" ? value
                                                                : (dir.is_relative() && !base.empty() ? base / dir : dir).lexically_normal().string();
        } else if (key == "printer") {
            const std::filesystem::path file(value);
            s.printer = value == "off" || value == "on" ? value : (file.is_relative() && !base.empty() ? base / file : file).lexically_normal().string();
        } else if (key == "serial_line") {
            if (value == "off" || value == "on" || value == "listen" || value.rfind("listen:", 0) == 0 || value.rfind("connect:", 0) == 0) {
                s.serial_line = value;
            } else {
                fail(msg("serial_line is off, on, listen[:PORT] or connect:PORT"));
            }
        } else if (key == "serial_flow") {
            if (value == "rts" || value == "none") {
                s.serial_flow = value;
            } else {
                fail(msg("serial_flow is rts or none"));
            }
        } else if (key == "sid_card") {
            if (value == "off" || value == "6581" || value == "8580") {
                s.sid_card = value;
            } else {
                fail(msg("sid_card is off, 6581 or 8580"));
            }
        } else if (key == "disk_rom") {
            const std::filesystem::path file(value);
            s.disk_rom = value == "1.1" || value == "1.2" ? value : (file.is_relative() && !base.empty() ? base / file : file).lexically_normal().string();
        } else if (key == "volume") {
            const int volume = std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; }) && value.size() <= 3 ? std::stoi(value) : -1;
            if (volume >= 0 && volume <= 100) {
                s.volume = volume;
            } else {
                fail(msg("volume is 0 to 100"));
            }
        } else if (key == "monitor" || key == "mcp" || key == "dzrp" || key == "gdb") {
            const bool digits = value.size() <= 5 && std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; });
            const long port = digits ? std::stol(value) : 0;
            if (value == "off" || value == "on" || (digits && port >= 1 && port <= 65535)) {
                (key == "monitor" ? s.monitor : key == "mcp" ? s.mcp : key == "dzrp" ? s.dzrp : s.gdb) =
                    value == "off" ? kServerOff : value == "on" ? kServerDefaultPort : static_cast<int>(port);
            } else {
                fail(msg("%s is off, on or a port number", std::string(key).c_str()));
            }
        } else if (key == "renderer") {
            if (value == "gl" || value == "sdl") {
                s.renderer = value;
            } else {
                fail(msg("renderer is gl or sdl"));
            }
        } else if (key == "picture") {
            s.picture = value;
        } else if (key == "keys") {
            s.keys = value;
        } else if (key == "colour_switch") {
            if (value == "colour" || value == "bw") {
                s.colour_switch = value == "colour";
            } else {
                fail(msg("colour_switch is colour or bw"));
            }
        } else if (key == "preset") {
            s.preset = value;
        } else if (key == "skin") {
            s.skin = value;
        } else if (key == "language") {
            if (std::all_of(value.begin(), value.end(), [](char c) { return c >= 'a' && c <= 'z'; })) {
                s.language = value;
            } else {
                fail(msg("language is a code such as en or hu"));
            }
        } else if (key == "keymap") {
            if (value == "symbolic" || value == "positional") {
                s.keymap = value;
            } else {
                fail(msg("keymap is symbolic or positional"));
            }
        } else if ((key == "gamecard" || key == "multichip" || key == "card256" || key == "twodfx" || key == "nanosd" ||
                    key == "serial_card") &&
                   value.size() == 1 && value[0] >= '0' && value[0] <= '3') {
            const auto slot = static_cast<unsigned>(value[0] - '0');
            (key == "gamecard"    ? s.gamecard
             : key == "multichip" ? s.multichip
             : key == "card256"   ? s.card256
             : key == "twodfx"    ? s.twodfx
             : key == "nanosd"    ? s.nanosd
                                  : s.serial_card) = true;
            (key == "gamecard"    ? s.gamecard_slot
             : key == "multichip" ? s.multichip_slot
             : key == "card256"   ? s.card256_slot
             : key == "twodfx"    ? s.twodfx_slot
             : key == "nanosd"    ? s.nanosd_slot
                                  : s.serial_slot) = slot;
        } else if (key == "audio" || key == "full_raster" || key == "disk_card" || key == "gamecard" || key == "multichip" ||
                   key == "card256" || key == "twodfx" || key == "nanosd" || key == "nanosd_menu" || key == "serial_card" || key == "fullscreen" ||
                   key == "dzrp_banks" || key == "mcp_allow_monitor") {
            const auto flag = parse_flag(value);
            if (!flag) {
                fail(msg("%s is yes or no", std::string(key).c_str()));
            } else {
                (key == "audio"         ? s.audio
                 : key == "full_raster" ? s.full_raster
                 : key == "disk_card"   ? s.disk_card
                 : key == "gamecard"    ? s.gamecard
                 : key == "multichip"   ? s.multichip
                 : key == "nanosd"      ? s.nanosd
                 : key == "twodfx"      ? s.twodfx
                 : key == "nanosd_menu" ? s.nanosd_menu
                 : key == "serial_card" ? s.serial_card
                 : key == "fullscreen"  ? s.fullscreen
                 : key == "dzrp_banks"  ? s.dzrp_banks
                 : key == "mcp_allow_monitor" ? s.mcp_allow_monitor
                                        : s.card256) = flag;
            }
        } else {
            fail(msg("unknown key '%s'", std::string(key).c_str()));
        }
    }
    return s;
}

SettingsFile load_settings(const std::filesystem::path& program_dir, const std::filesystem::path& user_dir) {
    SettingsFile file;
    file.path = settings_path(program_dir, user_dir, &file.portable);
    if (file.path.empty()) {
        return file;
    }
    std::ifstream in(file.path, std::ios::binary);
    if (!in) {
        return file;  // not written yet
    }
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    file.values = parse_settings(text, file.path.parent_path(), file.errors);
    return file;
}

std::string update_settings_text(std::string_view text, const SettingValues& values) {
    std::string out = text.empty() ? std::string(kHeader) : std::string();
    std::vector<bool> written(values.size(), false);
    std::size_t pos = 0;
    while (pos < text.size()) {
        const auto newline = text.find('\n', pos);
        const std::string_view line = text.substr(pos, newline == std::string_view::npos ? std::string_view::npos : newline - pos);
        pos = newline == std::string_view::npos ? text.size() : newline + 1;
        const std::string_view key = line_key(line);
        bool replaced = false;
        for (std::size_t i = 0; i < values.size() && !key.empty() && !replaced; ++i) {
            if (key == values[i].first) {
                written[i] = replaced = true;
                if (values[i].second.empty()) {
                    break;  // an empty value removes the key
                }
                out += values[i].first + " = " + values[i].second;
                if (const auto comment = line.find(';'); comment != std::string_view::npos) {
                    out += "  " + std::string(line.substr(comment));
                }
                out += "\n";
            }
        }
        if (!replaced) {
            out += line;
            out += "\n";
        }
    }
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (!written[i] && !values[i].second.empty()) {
            out += values[i].first + " = " + values[i].second + "\n";
        }
    }
    return out;
}

bool save_settings(const std::filesystem::path& path, const SettingValues& values, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::string text;
    if (std::ifstream in(path, std::ios::binary); in) {
        text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    const std::filesystem::path temporary = path.string() + ".new";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        out << update_settings_text(text, values);
        if (!out) {
            error = msg("cannot write '%s'", temporary.string().c_str());
            return false;
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        error = msg("cannot replace '%s': %s", path.string().c_str(), ec.message().c_str());
        return false;
    }
    return true;
}

}  // namespace freetvc
