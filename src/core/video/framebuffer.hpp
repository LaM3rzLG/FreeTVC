// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace freetvc {

// Indexed picture of the whole raster, blanking included, as the beam traces it: x from the start of HSYNC, y from the first line after VSYNC.
struct Framebuffer {
    // 100 CRTC characters x 8 pixels at the 12.5 MHz 2-colour shift clock; 314 lines per frame (tvc-hardware.md §2, §5).
    static constexpr int kWidth = 800;
    static constexpr int kHeight = 314;

    // Indexes 0-15 are IGRB colours (b3 I, b2 G, b1 R, b0 B); 16 is blanking.
    static constexpr std::uint8_t kBlank = 16;
    static constexpr std::size_t kColours = 17;

    std::vector<std::uint8_t> pixels = std::vector<std::uint8_t>(static_cast<std::size_t>(kWidth) * kHeight, kBlank);
};

// A rectangle of the framebuffer, in framebuffer pixels
struct ScreenArea {
    int x;
    int y;
    int width;
    int height;
};

// What a PAL TV shows: 52 us of each line from 10.5 us after the HSYNC edge, lines 23-309 (design/ui.md "Play")
inline constexpr ScreenArea kTvArea{131, 23, 650, 287};
// Everything the beam traces, sync and blanking included
inline constexpr ScreenArea kFullRaster{0, 0, Framebuffer::kWidth, Framebuffer::kHeight};

// 0xRRGGBB per index. [OS]: component = C * (4/7 + I * 3/7), so dark = 146 and bright = 255; I alone is a second black (tvc-hardware.md §5, levels ❓).
inline constexpr std::array<std::uint32_t, Framebuffer::kColours> kRgbPalette = [] {
    std::array<std::uint32_t, Framebuffer::kColours> rgb{};
    for (std::uint32_t i = 0; i < 16; ++i) {
        const std::uint32_t level = (i & 8) ? 255 : 146;
        const std::uint32_t r = (i & 2) ? level : 0;
        const std::uint32_t g = (i & 4) ? level : 0;
        const std::uint32_t b = (i & 1) ? level : 0;
        rgb[i] = r << 16 | g << 8 | b;
    }
    rgb[Framebuffer::kBlank] = 0;
    return rgb;
}();

}  // namespace freetvc
