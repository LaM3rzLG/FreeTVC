// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/sdl/skin.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>
#include <variant>

namespace freetvc::sdl {

namespace {

constexpr std::string_view kTvcColourNames[8] = {"black", "blue", "red", "magenta", "green", "cyan", "yellow", "white"};
constexpr std::string_view kRoleNames[kRoles] = {"exec", "read", "write", "io", "vram", "notice"};

using Field = std::variant<Colour Skin::*, float Skin::*, std::string Skin::*, bool Skin::*>;

struct NamedField {
    std::string_view key;
    Field field;
};

const NamedField kFields[] = {
    {"name", &Skin::name},
    {"colour.frame", &Skin::frame},
    {"colour.deck", &Skin::deck},
    {"colour.groove", &Skin::groove},
    {"colour.rib", &Skin::rib},
    {"colour.key", &Skin::key},
    {"colour.key.hover", &Skin::key_hover},
    {"colour.key.active", &Skin::key_active},
    {"colour.panel", &Skin::panel},
    {"colour.putty", &Skin::putty},
    {"colour.ink", &Skin::ink},
    {"panel.light", &Skin::panel_light},
    {"colour.label", &Skin::label},
    {"colour.muted", &Skin::muted},
    {"font.ui", &Skin::font_ui},
    {"font.strong", &Skin::font_strong},
    {"font.data", &Skin::font_data},
    {"font.ui.size", &Skin::font_ui_size},
    {"font.data.size", &Skin::font_data_size},
    {"metric.strip.height", &Skin::strip_height},
    {"metric.screen.margin", &Skin::screen_margin},
    {"metric.groove", &Skin::groove_width},
    {"metric.rounding", &Skin::rounding},
};

std::string_view trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(" \t\r");
    return s.substr(first, last - first + 1);
}

std::optional<unsigned> tvc_colour_number(std::string_view name) {
    for (unsigned c = 0; c < 8; ++c) {
        if (name == kTvcColourNames[c]) {
            return c;
        }
    }
    return std::nullopt;
}

// "#rrggbb", "#rrggbbaa", a TVC colour name (bright) or "dark-" + a TVC colour name
std::optional<Colour> parse_colour(std::string_view s) {
    if (s.starts_with("dark-")) {
        const auto c = tvc_colour_number(s.substr(5));
        return c ? std::optional(tvc_role_colour(*c).on_putty) : std::nullopt;
    }
    if (const auto c = tvc_colour_number(s)) {
        return tvc_role_colour(*c).on_case;
    }
    if (s.size() != 7 && s.size() != 9) {
        return std::nullopt;
    }
    if (s[0] != '#') {
        return std::nullopt;
    }
    std::uint8_t bytes[4] = {0, 0, 0, 255};
    for (std::size_t i = 0; i * 2 + 1 < s.size(); ++i) {
        const char* first = s.data() + 1 + i * 2;
        const auto [end, ec] = std::from_chars(first, first + 2, bytes[i], 16);
        if (ec != std::errc{} || end != first + 2) {
            return std::nullopt;
        }
    }
    return Colour{bytes[0], bytes[1], bytes[2], bytes[3]};
}

// A TVC colour name for both variants, or two colours: on case, then on putty
std::optional<RoleColour> parse_role(std::string_view s) {
    if (const auto c = tvc_colour_number(s)) {
        return tvc_role_colour(*c);
    }
    const auto space = s.find_first_of(" \t");
    if (space == std::string_view::npos) {
        return std::nullopt;
    }
    const auto on_case = parse_colour(trim(s.substr(0, space)));
    const auto on_putty = parse_colour(trim(s.substr(space)));
    if (!on_case || !on_putty) {
        return std::nullopt;
    }
    return RoleColour{*on_case, *on_putty};
}

std::optional<float> parse_number(std::string_view s) {
    float value = 0.0f;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value);
    if (ec != std::errc{} || end != s.data() + s.size() || value < 0.0f || value > 1000.0f) {
        return std::nullopt;
    }
    return value;
}

// Empty when the line was applied, else the reason it was not
std::string apply_line(std::string_view key, std::string_view value, Skin& skin) {
    if (key.starts_with("role.")) {
        const auto name = key.substr(5);
        for (std::size_t r = 0; r < kRoles; ++r) {
            if (name == kRoleNames[r]) {
                const auto role = parse_role(value);
                if (!role) {
                    return "expected a TVC colour name or two colours (on case, on putty)";
                }
                skin.roles[r] = *role;
                return {};
            }
        }
        return "unknown role '" + std::string(name) + "'";
    }
    for (const NamedField& named : kFields) {
        if (named.key != key) {
            continue;
        }
        if (const auto* colour = std::get_if<Colour Skin::*>(&named.field)) {
            const auto parsed = parse_colour(value);
            if (!parsed) {
                return "expected #rrggbb, #rrggbbaa or a TVC colour name";
            }
            skin.**colour = *parsed;
        } else if (const auto* number = std::get_if<float Skin::*>(&named.field)) {
            const auto parsed = parse_number(value);
            if (!parsed) {
                return "expected a number from 0 to 1000";
            }
            skin.**number = *parsed;
        } else if (const auto* flag = std::get_if<bool Skin::*>(&named.field)) {
            if (value == "yes" || value == "true" || value == "1") {
                skin.**flag = true;
            } else if (value == "no" || value == "false" || value == "0") {
                skin.**flag = false;
            } else {
                return "expected yes or no";
            }
        } else if (value.empty()) {
            return "expected a value";
        } else {
            skin.*std::get<std::string Skin::*>(named.field) = std::string(value);
        }
        return {};
    }
    return "unknown key '" + std::string(key) + "'";
}

}  // namespace

RoleColour tvc_role_colour(unsigned c) {
    const auto level = [&](unsigned bit, std::uint8_t on) { return static_cast<std::uint8_t>((c & bit) ? on : 0); };
    return {{level(2, 255), level(4, 255), level(1, 255)}, {level(2, 146), level(4, 146), level(1, 146)}};
}

std::vector<std::string> parse_skin(std::string_view text, Skin& skin) {
    std::vector<std::string> errors;
    int line_number = 0;
    while (!text.empty()) {
        const auto newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
        ++line_number;
        line = trim(line.substr(0, line.find(';')));
        if (line.empty()) {
            continue;
        }
        const auto equals = line.find('=');
        std::string reason = equals == std::string_view::npos
                                 ? "expected key = value"
                                 : apply_line(trim(line.substr(0, equals)), trim(line.substr(equals + 1)), skin);
        if (!reason.empty()) {
            errors.push_back("line " + std::to_string(line_number) + ": " + reason);
        }
    }
    return errors;
}

std::optional<Skin> load_skin_file(const std::filesystem::path& path, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "cannot read " + path.string();
        return std::nullopt;
    }
    std::ostringstream text;
    text << file.rdbuf();
    Skin skin;
    const auto errors = parse_skin(text.str(), skin);
    if (!errors.empty()) {
        error = path.filename().string() + " " + errors.front();
        if (errors.size() > 1) {
            error += " (+" + std::to_string(errors.size() - 1) + " more)";
        }
        return std::nullopt;
    }
    return skin;
}

std::vector<SkinEntry> list_skins(const std::vector<std::filesystem::path>& dirs) {
    std::map<std::string, std::filesystem::path> found;
    for (const auto& dir : dirs) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.path().extension() == ".skin" && entry.is_regular_file(ec)) {
                found.try_emplace(entry.path().stem().string(), entry.path());
            }
        }
    }
    std::vector<SkinEntry> skins;
    for (auto& [name, path] : found) {
        skins.push_back({name, path});
    }
    return skins;
}

std::optional<std::filesystem::path> find_skin(std::string_view name_or_path, const std::vector<std::filesystem::path>& dirs) {
    std::error_code ec;
    const std::filesystem::path as_path(name_or_path);
    if (as_path.has_parent_path() || as_path.extension() == ".skin") {
        if (std::filesystem::is_regular_file(as_path, ec)) {
            return as_path;
        }
    }
    for (const SkinEntry& entry : list_skins(dirs)) {
        if (entry.name == name_or_path) {
            return entry.path;
        }
    }
    return std::nullopt;
}

}  // namespace freetvc::sdl
