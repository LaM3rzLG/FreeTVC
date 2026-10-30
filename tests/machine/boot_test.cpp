// SPDX-License-Identifier: GPL-3.0-or-later
// Boots each preset to the BASIC prompt: SPACE dismisses the start-up logo, then the OS screen copy at 0100h
// (64 characters per row, tvc-hardware.md §3) must show the banner and "ok", and the frame must hold drawn text.
// usage: boot_test <rom dir> <preset>; exit 77 (CTest skip) when the ROM files are missing.
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

#include "core/machine/machine.hpp"
#include "frontend/common/presets.hpp"

namespace {

constexpr int kExitSkip = 77;
constexpr int kKeyFrame = 330;  // the 64k RAM test ends near frame 250 (about 15.4 M T)
constexpr int kKeyFrames = 5;
constexpr int kFrames = 450;

std::string screen_row(const freetvc::Machine& m, unsigned row) {
    std::string text;
    for (unsigned c = 0; c < 64; ++c) {
        text.push_back(static_cast<char>(m.ram()[0x100 + row * 64 + c]));
    }
    return text.substr(0, text.find_last_not_of(' ') + 1);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: boot_test <rom dir> <preset>\n");
        return 2;
    }
    const freetvc::Preset* preset = freetvc::find_preset(argv[2]);
    if (!preset) {
        std::fprintf(stderr, "unknown preset '%s'\n", argv[2]);
        return 2;
    }
    std::string error;
    const auto config = freetvc::load_preset(*preset, argv[1], error);
    if (!config) {
        std::printf("SKIP: %s (set FREETVC_ROM_DIR)\n", error.c_str());
        return kExitSkip;
    }

    freetvc::Machine m(*config);
    for (int frame = 0; frame < kFrames; ++frame) {
        if (frame == kKeyFrame || frame == kKeyFrame + kKeyFrames) {
            m.set_key(7, 5, frame == kKeyFrame);  // SPACE
        }
        m.run({.max_ticks = freetvc::kTStatesPerFrame * freetvc::kTicksPerTState});
    }

    // "TV COMPUTER BASIC 2.2" from "64k+-2.2"
    const std::string_view name = preset->name;
    const std::string banner = "TV COMPUTER BASIC " + std::string(name.substr(name.rfind('-') + 1));
    const std::string row0 = screen_row(m, 0);
    const std::string row5 = screen_row(m, 5);

    // Ink pixels in the first six text rows of the display (x 200, y 49 with the ROM's CRTC values): all but the commonest colour
    const freetvc::Framebuffer& fb = m.framebuffer();
    std::array<unsigned, freetvc::Framebuffer::kColours> histogram{};
    for (int y = 49; y < 49 + 6 * 10; ++y) {
        for (int x = 200; x < 200 + 512; ++x) {
            ++histogram[fb.pixels[static_cast<std::size_t>(y) * freetvc::Framebuffer::kWidth + static_cast<std::size_t>(x)]];
        }
    }
    unsigned paper = 0;
    unsigned total = 0;
    for (const unsigned count : histogram) {
        paper = count > paper ? count : paper;
        total += count;
    }
    const unsigned ink = total - paper;

    std::printf("%s: row 0 '%s', row 3 '%s', row 5 '%s', ink pixels %u, frames %llu\n", argv[2], row0.c_str(),
                screen_row(m, 3).c_str(), row5.c_str(), ink, static_cast<unsigned long long>(m.frame_count()));
    const bool ok = row0 == banner && row5 == "ok" && ink > 1000;
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
