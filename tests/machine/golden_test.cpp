// SPDX-License-Identifier: GPL-3.0-or-later
// Golden frame: rebuild WinTVC's frozen video state (captured from WinTVC 1.4.1) in our machine, render it and compare the
// colour indexes with WinTVC's F2 framebuffer, which is a crop of the raster at an offset found by search.
// --skip-rows A-B leaves GIF rows A-B out of the verdict (rows the OS rewrites while the beam scans them; they are still reported).
// usage: golden_test <golden dir> <preset> <scene> [--skip-rows A-B]; exit 77 (CTest skip) when the capture is missing.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "core/machine/machine.hpp"
#include "support/mini_json.hpp"

namespace {

constexpr int kExitSkip = 77;
constexpr int kFrames = 3;
// The F2 GIF is our raster from x 136, y 24 (found by search on every capture); display (0, 0) is at GIF (64, 25)
constexpr int kCropX = 136;
constexpr int kCropY = 24;

struct Image {
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> pixels;
};

std::optional<std::string> read_text(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

// Binary PGM (P5, maxval 255) as the capture writes it; no comments
std::optional<Image> read_pgm(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::string magic;
    int maxval = 0;
    Image image;
    if (!(in >> magic >> image.width >> image.height >> maxval) || magic != "P5" || maxval != 255) {
        return std::nullopt;
    }
    in.get();
    image.pixels.resize(static_cast<std::size_t>(image.width) * static_cast<std::size_t>(image.height));
    if (!in.read(reinterpret_cast<char*>(image.pixels.data()), static_cast<std::streamsize>(image.pixels.size()))) {
        return std::nullopt;
    }
    return image;
}

// IGRB index -> port bits: palette 60h-63h `-I-G-R-B`, border 00h `I-G-R-B-`
std::uint8_t palette_port(int igrb) {
    return static_cast<std::uint8_t>((igrb & 8) << 3 | (igrb & 4) << 2 | (igrb & 2) << 1 | (igrb & 1));
}
std::uint8_t border_port(int igrb) { return static_cast<std::uint8_t>(palette_port(igrb) << 1); }

struct RowRange {
    int first = 0;
    int last = -1;
    bool contains(int y) const { return y >= first && y <= last; }
};

std::size_t count_mismatches(const freetvc::Framebuffer& frame, const Image& gold, int dx, int dy, std::size_t limit,
                             RowRange skip) {
    std::size_t count = 0;
    for (int y = 0; y < gold.height && count <= limit; ++y) {
        if (skip.contains(y)) {
            continue;
        }
        const std::uint8_t* ours = &frame.pixels[static_cast<std::size_t>(y + dy) * freetvc::Framebuffer::kWidth + static_cast<std::size_t>(dx)];
        const std::uint8_t* theirs = &gold.pixels[static_cast<std::size_t>(y) * static_cast<std::size_t>(gold.width)];
        for (int x = 0; x < gold.width; ++x) {
            count += ours[x] != theirs[x];
        }
    }
    return count;
}

}  // namespace

int main(int argc, char** argv) {
    RowRange skip;
    if (argc == 6 && std::string(argv[4]) == "--skip-rows" &&
        std::sscanf(argv[5], "%d-%d", &skip.first, &skip.last) == 2 && skip.first <= skip.last) {
        argc = 4;
    }
    if (argc != 4) {
        std::fprintf(stderr, "usage: golden_test <golden dir> <preset> <scene> [--skip-rows A-B]\n");
        return 2;
    }
    const std::string base = std::string(argv[1]) + "/" + argv[2] + "/" + argv[3];
    const auto json_text = read_text(base + ".json");
    const auto vram = read_text(base + ".vram");
    const auto gold = read_pgm(base + ".pgm");
    if (!json_text || !vram || !gold) {
        std::printf("SKIP: no capture at %s.{json,vram,pgm} (testdata/golden)\n", base.c_str());
        return kExitSkip;
    }
    const freetvc::test::JsonValue state = freetvc::test::JsonParser(*json_text).parse();
    if (vram->size() != 0x4000 || gold->width > freetvc::Framebuffer::kWidth || gold->height > freetvc::Framebuffer::kHeight) {
        std::printf("FAIL: bad capture sizes (vram %zu, frame %d x %d)\n", vram->size(), gold->width, gold->height);
        return 1;
    }

    // U0, U1, VID, CART; the CPU spins on DI / JR $ in U0 so it never touches VRAM
    freetvc::Machine m;
    m.io_write(0x02, 0x10);
    m.ram()[0] = 0xF3;
    m.ram()[1] = 0x18;
    m.ram()[2] = 0xFE;
    m.jump(0x0000);
    for (std::uint16_t i = 0; i < 0x4000; ++i) {
        m.poke(static_cast<std::uint16_t>(0x8000 + i), static_cast<std::uint8_t>((*vram)[i]));
    }
    const auto& crtc = state.at("crtc").array;
    for (std::uint8_t r = 0; r < 16; ++r) {
        m.io_write(0x70, r);
        m.io_write(0x71, static_cast<std::uint8_t>(crtc.at(r).as_int()));
    }
    const auto& palette = state.at("palette").array;
    for (std::uint8_t i = 0; i < 4; ++i) {
        m.io_write(static_cast<std::uint8_t>(0x60 + i), palette_port(palette.at(i).as_int()));
    }
    m.io_write(0x00, border_port(state.at("border").as_int()));
    m.io_write(0x06, static_cast<std::uint8_t>(state.at("mode").as_int()));
    m.run({.max_ticks = kFrames * freetvc::kTStatesPerFrame * freetvc::kTicksPerTState});
    const freetvc::Framebuffer& frame = m.framebuffer();

    const std::size_t all = gold->pixels.size();
    const std::size_t at_crop = count_mismatches(frame, *gold, kCropX, kCropY, all, skip);
    std::printf("%s/%s: %zu of %zu pixels differ at crop offset x %d y %d\n", argv[2], argv[3], at_crop, all, kCropX,
                kCropY);
    if (skip.last >= 0) {
        const std::size_t in_skipped = count_mismatches(frame, *gold, kCropX, kCropY, all, RowRange{}) - at_crop;
        std::printf("  rows %d-%d not judged: %zu pixels differ there\n", skip.first, skip.last, in_skipped);
    }
    if (at_crop != 0) {
        // Diagnose: the best offset, then where and which colours differ there
        std::size_t best = all + 1;
        int best_dx = 0;
        int best_dy = 0;
        for (int dy = 0; dy + gold->height <= freetvc::Framebuffer::kHeight; ++dy) {
            for (int dx = 0; dx + gold->width <= freetvc::Framebuffer::kWidth; ++dx) {
                const std::size_t count = count_mismatches(frame, *gold, dx, dy, best, skip);
                if (count < best) {
                    best = count;
                    best_dx = dx;
                    best_dy = dy;
                }
            }
        }
        std::printf("  best offset x %d y %d: %zu pixels differ\n", best_dx, best_dy, best);
        int x0 = gold->width, y0 = gold->height, x1 = -1, y1 = -1, shown = 0;
        std::array<std::array<unsigned, freetvc::Framebuffer::kColours>, 16> pairs{};
        for (int y = 0; y < gold->height; ++y) {
            for (int x = 0; x < gold->width; ++x) {
                const std::uint8_t theirs = gold->pixels[static_cast<std::size_t>(y * gold->width + x)];
                const std::uint8_t ours = frame.pixels[static_cast<std::size_t>(y + best_dy) * freetvc::Framebuffer::kWidth + static_cast<std::size_t>(x + best_dx)];
                if (theirs != ours && !skip.contains(y)) {
                    x0 = std::min(x0, x);
                    y0 = std::min(y0, y);
                    x1 = std::max(x1, x);
                    y1 = std::max(y1, y);
                    ++pairs[theirs & 15][ours];
                    if (shown++ < 8) {
                        std::printf("  (%d, %d): WinTVC %u, ours %u\n", x, y, theirs, ours);
                    }
                }
            }
        }
        std::printf("  differences inside x %d-%d, y %d-%d (WinTVC crop coordinates)\n", x0, x1, y0, y1);
        for (unsigned t = 0; t < 16; ++t) {
            for (unsigned o = 0; o < freetvc::Framebuffer::kColours; ++o) {
                if (pairs[t][o]) {
                    std::printf("  WinTVC %u / ours %u: %u pixels\n", t, o, pairs[t][o]);
                }
            }
        }
        return 1;
    }
    return 0;
}
