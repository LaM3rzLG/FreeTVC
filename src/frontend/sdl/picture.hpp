// SPDX-License-Identifier: GPL-3.0-or-later
// The picture model's settings (design/ui.md "Picture"): plain `key = value` files like the skins. No SDL or GL here.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace freetvc::sdl {

// The path from the TVC to the screen: sharp pixels, the RGB DIN socket on a monitor, the RF/composite PAL signal on a
// colour set, the same signal on a black and white set
enum class Signal { Sharp, Rgb, Pal, Mono };
enum class Mask { None, Grille, Shadow };

// Member defaults are the shipped "pal-tv" file's values, except signal (sharp)
struct Picture {
    std::string name = "sharp";
    Signal signal = Signal::Sharp;
    float luma_mhz = 3.5f;       // the set's luma path, -3 dB
    float chroma_mhz = 1.3f;     // U and V after demodulation, -3 dB
    bool delay_line = true;      // PAL: each line's chroma averaged with the line before
    float phase_error = 0.0f;    // degrees of chroma phase error; without the delay line it shows as Hanover bars
    float saturation = 1.0f;
    float beam_min = 0.28f;      // the beam's vertical sigma for black, in line pitches
    float beam_max = 0.46f;      // and for full white: brighter lines are wider
    float spot = 0.55f;          // the beam's horizontal sigma, in TVC pixels (RGB; PAL is limited by its bandwidth)
    Mask mask = Mask::None;
    float mask_strength = 0.3f;  // 0-1
    float mask_pitch = 3.0f;     // window pixels per mask period
    float glow = 0.0f;           // halation around bright areas, 0-1
    float curvature = 0.0f;      // 0-0.2
    float vignette = 0.0f;       // corner darkening, 0-1
    float crt_gamma = 2.4f;      // the tube's
    float brightness = 1.0f;

    bool operator==(const Picture&) const = default;
};

// Applies "key = value" lines on top of picture; ';' starts a comment. Returns "line N: ..." messages (msg()).
std::vector<std::string> parse_picture(std::string_view text, Picture& picture);
std::optional<Picture> load_picture_file(const std::filesystem::path& path, std::string& error);
// The file's text for these values, every key with its value (for the settings window's copy in the user folder)
std::string picture_text(const Picture& picture);

struct PictureEntry {
    std::string name;  // file name without ".picture"
    std::filesystem::path path;
};
// Every *.picture in dirs, sorted by name; the earlier dir wins a name.
std::vector<PictureEntry> list_pictures(const std::vector<std::filesystem::path>& dirs);

}  // namespace freetvc::sdl
