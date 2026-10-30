// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/sdl/picture.hpp"

#include <charconv>
#include <cstdio>
#include <fstream>
#include <map>
#include <sstream>
#include <system_error>

#include "core/message.hpp"

namespace freetvc::sdl {

namespace {

struct Number {
    std::string_view key;
    float Picture::* field;
    float min;
    float max;
};
const Number kNumbers[] = {
    {"luma.bandwidth", &Picture::luma_mhz, 0.5f, 6.0f},
    {"chroma.bandwidth", &Picture::chroma_mhz, 0.3f, 3.0f},
    {"chroma.phase.error", &Picture::phase_error, -45.0f, 45.0f},
    {"saturation", &Picture::saturation, 0.0f, 2.0f},
    {"beam.min", &Picture::beam_min, 0.1f, 1.0f},
    {"beam.max", &Picture::beam_max, 0.1f, 1.0f},
    {"spot", &Picture::spot, 0.1f, 2.0f},
    {"mask.strength", &Picture::mask_strength, 0.0f, 1.0f},
    {"mask.pitch", &Picture::mask_pitch, 2.0f, 12.0f},
    {"glow", &Picture::glow, 0.0f, 1.0f},
    {"curvature", &Picture::curvature, 0.0f, 0.2f},
    {"vignette", &Picture::vignette, 0.0f, 1.0f},
    {"gamma.crt", &Picture::crt_gamma, 1.0f, 3.0f},
    {"brightness", &Picture::brightness, 0.2f, 2.0f},
};

constexpr std::string_view kSignals[] = {"sharp", "rgb", "pal", "mono"};
constexpr std::string_view kMasks[] = {"none", "grille", "shadow"};

std::string_view trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(" \t\r") - first + 1);
}

// Empty when the line was applied, else the reason it was not
std::string apply_line(std::string_view key, std::string_view value, Picture& picture) {
    if (key == "name") {
        if (value.empty()) {
            return msg("expected a value");
        }
        picture.name = std::string(value);
        return {};
    }
    if (key == "signal") {
        for (std::size_t i = 0; i < std::size(kSignals); ++i) {
            if (value == kSignals[i]) {
                picture.signal = static_cast<Signal>(i);
                return {};
            }
        }
        return msg("signal is sharp, rgb, pal or mono");
    }
    if (key == "mask") {
        for (std::size_t i = 0; i < std::size(kMasks); ++i) {
            if (value == kMasks[i]) {
                picture.mask = static_cast<Mask>(i);
                return {};
            }
        }
        return msg("mask is none, grille or shadow");
    }
    if (key == "delay.line") {
        if (value == "yes" || value == "no") {
            picture.delay_line = value == "yes";
            return {};
        }
        return msg("expected yes or no");
    }
    for (const Number& n : kNumbers) {
        if (n.key != key) {
            continue;
        }
        float v = 0.0f;
        const auto [end, ec] = std::from_chars(value.data(), value.data() + value.size(), v);
        if (ec != std::errc{} || end != value.data() + value.size() || v < n.min || v > n.max) {
            return msg("%s is a number from %g to %g", std::string(key).c_str(), static_cast<double>(n.min), static_cast<double>(n.max));
        }
        picture.*n.field = v;
        return {};
    }
    return msg("unknown key '%s'", std::string(key).c_str());
}

}  // namespace

std::vector<std::string> parse_picture(std::string_view text, Picture& picture) {
    std::vector<std::string> errors;
    int number = 0;
    while (!text.empty()) {
        const auto newline = text.find('\n');
        std::string_view line = text.substr(0, newline);
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
        ++number;
        line = trim(line.substr(0, line.find(';')));
        if (line.empty()) {
            continue;
        }
        const auto equals = line.find('=');
        const std::string reason = equals == std::string_view::npos
                                       ? msg("expected key = value")
                                       : apply_line(trim(line.substr(0, equals)), trim(line.substr(equals + 1)), picture);
        if (!reason.empty()) {
            errors.push_back(msg("line %d: %s", number, reason.c_str()));
        }
    }
    if (picture.beam_max < picture.beam_min) {
        errors.push_back(msg("beam.max is below beam.min"));
    }
    return errors;
}

std::optional<Picture> load_picture_file(const std::filesystem::path& path, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = msg("cannot read '%s'", path.string().c_str());
        return std::nullopt;
    }
    std::ostringstream text;
    text << file.rdbuf();
    Picture picture;
    const auto errors = parse_picture(text.str(), picture);
    if (!errors.empty()) {
        error = msg("%s %s", path.filename().string().c_str(), errors.front().c_str());
        return std::nullopt;
    }
    return picture;
}

std::string picture_text(const Picture& picture) {
    std::string text = "name = " + picture.name + "\n";
    text += "signal = " + std::string(kSignals[static_cast<std::size_t>(picture.signal)]) + "\n";
    text += "delay.line = " + std::string(picture.delay_line ? "yes" : "no") + "\n";
    text += "mask = " + std::string(kMasks[static_cast<std::size_t>(picture.mask)]) + "\n";
    for (const Number& n : kNumbers) {
        char value[32];
        std::snprintf(value, sizeof value, "%g", static_cast<double>(picture.*n.field));
        text += std::string(n.key) + " = " + value + "\n";
    }
    return text;
}

std::vector<PictureEntry> list_pictures(const std::vector<std::filesystem::path>& dirs) {
    std::map<std::string, std::filesystem::path> found;
    for (const auto& dir : dirs) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (entry.path().extension() == ".picture" && entry.is_regular_file(ec)) {
                found.try_emplace(entry.path().stem().string(), entry.path());
            }
        }
    }
    std::vector<PictureEntry> pictures;
    for (auto& [name, path] : found) {
        pictures.push_back({name, path});
    }
    return pictures;
}

}  // namespace freetvc::sdl
