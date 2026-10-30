// SPDX-License-Identifier: GPL-3.0-or-later
// Writes PNGs of test framebuffers with the exact palette indexes they hold; png_check.py decodes them with zlib.
// usage: png_test <work dir>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/video/framebuffer.hpp"
#include "remote/png.hpp"

namespace {

using freetvc::Framebuffer;
using freetvc::ScreenArea;

void write(const std::filesystem::path& dir, const std::string& name, const Framebuffer& frame, ScreenArea area) {
    const std::vector<std::uint8_t> png = freetvc::remote::encode_png(frame, area);
    area.width = std::min(area.width, Framebuffer::kWidth - area.x);  // the encoder keeps areas inside the frame
    area.height = std::min(area.height, Framebuffer::kHeight - area.y);
    std::ofstream(dir / (name + ".png"), std::ios::binary).write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    std::ofstream index(dir / (name + ".idx"), std::ios::binary);
    for (int y = 0; y < area.height; ++y) {
        for (int x = 0; x < area.width; ++x) {
            index.put(static_cast<char>(frame.pixels[static_cast<std::size_t>(area.y + y) * Framebuffer::kWidth + static_cast<std::size_t>(area.x + x)]));
        }
    }
    std::ofstream(dir / (name + ".txt")) << area.width << " " << area.height << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: png_test <work dir>\n");
        return 2;
    }
    const std::filesystem::path dir = argv[1];
    std::filesystem::create_directories(dir);
    {
        std::ofstream palette(dir / "palette.bin", std::ios::binary);
        for (const std::uint32_t rgb : freetvc::kRgbPalette) {
            palette.put(static_cast<char>(rgb >> 16)).put(static_cast<char>(rgb >> 8)).put(static_cast<char>(rgb));
        }
    }
    Framebuffer frame;
    for (int y = 0; y < Framebuffer::kHeight; ++y) {
        for (int x = 0; x < Framebuffer::kWidth; ++x) {
            frame.pixels[static_cast<std::size_t>(y) * Framebuffer::kWidth + static_cast<std::size_t>(x)] =
                static_cast<std::uint8_t>((x / 8 + y) % Framebuffer::kColours);
        }
    }
    write(dir, "colours", frame, freetvc::kFullRaster);
    std::uint32_t seed = 12345;
    for (std::uint8_t& p : frame.pixels) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        p = static_cast<std::uint8_t>(seed % Framebuffer::kColours);
    }
    write(dir, "noise", frame, freetvc::kTvArea);
    std::fill(frame.pixels.begin(), frame.pixels.end(), Framebuffer::kBlank);
    write(dir, "blank", frame, freetvc::kFullRaster);
    // Text-like: a border, black paper, rows of 8 x 10 glyph cells from a few shapes
    for (int y = 0; y < Framebuffer::kHeight; ++y) {
        for (int x = 0; x < Framebuffer::kWidth; ++x) {
            const bool paper = x >= 150 && x < 662 && y >= 49 && y < 289;
            std::uint8_t c = paper ? 0 : 1;
            if (paper && (y - 49) % 10 < 8 && (x / 8 + (y - 49) / 10) % 5 != 0) {
                c = (((x * 7 + (y - 49) % 10 * 3 + (x / 8 % 3)) >> 2) & 1) ? 12 : 0;
            }
            frame.pixels[static_cast<std::size_t>(y) * Framebuffer::kWidth + static_cast<std::size_t>(x)] = c;
        }
    }
    write(dir, "text", frame, freetvc::kTvArea);
    write(dir, "corner", frame, {790, 300, 10, 14});
    write(dir, "pixel", frame, {0, 0, 1, 1});
    write(dir, "clamped", frame, {700, 280, 500, 500});
    std::printf("png_test: wrote 7 images to %s\n", dir.string().c_str());
    return 0;
}
