// SPDX-License-Identifier: GPL-3.0-or-later
// 6845 timing, cursor IRQ, pixel modes, palette, border and blanking with the ROM's CRTC values (tvc-hardware.md §5).
#include <array>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "core/machine/machine.hpp"

namespace {

int g_failures = 0;

void check(bool condition, const char* what, unsigned long long got, unsigned long long expected) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s: got %llu (0x%llX), expected %llu (0x%llX)\n", what, got, got, expected, expected);
    }
}

#define CHECK_EQ(got, expected) check((got) == (expected), #got, (got), (expected))

constexpr std::uint64_t kFrameTicks = freetvc::kTStatesPerFrame * freetvc::kTicksPerTState;

// Display character (0, 0) of raster 0 in the framebuffer: x from HSYNC (R2 = 75 of 100), y from the line after VSYNC (raster 264)
constexpr int kDisplayX = (100 - 75) * 8;
constexpr int kDisplayY = (314 - 265);

// U0, U1, VID, CART; the CPU spins on DI / JR $ in U0 so it never touches VRAM
void setup(freetvc::Machine& m) {
    m.io_write(0x02, 0x10);
    m.ram()[0] = 0xF3;
    m.ram()[1] = 0x18;
    m.ram()[2] = 0xFE;
    m.jump(0x0000);
    // R0-R15 as the ROM writes them (SYS 2.2 C20Ch; [HW] table 7)
    const std::array<std::uint8_t, 16> crtc{99, 64, 75, 50, 77, 2, 60, 66, 0, 3, 3, 3, 0, 0, 14, 255};
    for (std::uint8_t r = 0; r < crtc.size(); ++r) {
        m.io_write(0x70, r);
        m.io_write(0x71, crtc[r]);
    }
}

std::uint8_t pixel(const freetvc::Machine& m, int x, int y) {
    return m.framebuffer().pixels[static_cast<std::size_t>(y) * freetvc::Framebuffer::kWidth + static_cast<std::size_t>(x)];
}

void frame_period() {
    // (77 + 1) rows x 4 lines + 2 adjust lines = 314 lines of 100 characters = 62 800 T
    freetvc::Machine m;
    setup(m);
    m.run({.max_ticks = 3 * kFrameTicks});
    std::vector<std::uint64_t> flips;
    std::uint64_t seen = m.frame_count();
    while (flips.size() < 4) {
        m.run({.max_ticks = 2});
        if (m.frame_count() != seen) {
            seen = m.frame_count();
            flips.push_back(m.ticks());
        }
    }
    for (std::size_t i = 1; i < flips.size(); ++i) {
        CHECK_EQ(flips[i] - flips[i - 1], kFrameTicks);
    }
}

void cursor_irq_once_per_frame() {
    // R14:R15 = 0EFFh, R10 = R11 = 3: one pulse on the last byte of line 239 (row 59, raster 3, column 63)
    freetvc::Machine m;
    setup(m);
    m.run({.max_ticks = 3 * kFrameTicks});
    m.io_write(0x07, 0);
    std::vector<std::uint64_t> pulses;
    const std::uint64_t end = m.ticks() + 3 * kFrameTicks;
    while (m.ticks() < end) {
        m.run({.max_ticks = 2});
        if (m.irq_pending()) {
            pulses.push_back(m.ticks());
            CHECK_EQ(m.video().crtc().row(), 59u);
            CHECK_EQ(m.video().crtc().raster(), 3u);
            CHECK_EQ(m.video().crtc().column(), 64u);  // already advanced past column 63
            m.io_write(0x07, 0);
        }
    }
    CHECK_EQ(pulses.size(), 3u);
    for (std::size_t i = 1; i < pulses.size(); ++i) {
        CHECK_EQ(pulses[i] - pulses[i - 1], kFrameTicks);
    }
}

void cursor_disabled_by_r10() {
    freetvc::Machine m;
    setup(m);
    m.io_write(0x70, 10);
    m.io_write(0x71, 0x23);  // b6-5 = 01: no cursor
    m.run({.max_ticks = 3 * kFrameTicks});
    CHECK_EQ(static_cast<unsigned>(m.irq_pending()), 0u);
}

// Runs two frames and returns the 8 pixels of display character (0, 0), line 0
std::array<std::uint8_t, 8> first_character(freetvc::Machine& m) {
    m.run({.max_ticks = 2 * kFrameTicks});
    std::array<std::uint8_t, 8> px{};
    for (int k = 0; k < 8; ++k) {
        px[static_cast<std::size_t>(k)] = pixel(m, kDisplayX + k, kDisplayY);
    }
    return px;
}

void check_pixels(const std::array<std::uint8_t, 8>& got, const std::array<std::uint8_t, 8>& expected) {
    for (std::size_t k = 0; k < 8; ++k) {
        CHECK_EQ(got[k], expected[k]);
    }
}

void mode_2_colour() {
    // Bit 7 leftmost; palette 0 = black, 1 = bright white (-I-G-R-B = 55h)
    freetvc::Machine m;
    setup(m);
    m.io_write(0x60, 0x00);
    m.io_write(0x61, 0x55);
    m.poke(0x8000, 0xA5);
    check_pixels(first_character(m), {15, 0, 15, 0, 0, 15, 0, 15});
}

void mode_4_colour() {
    // Pixel k = C1:C0 = bit 3-k : bit 7-k, two dots each. 86h gives indexes 1, 2, 2, 0
    freetvc::Machine m;
    setup(m);
    m.io_write(0x06, 0x01);
    m.io_write(0x60, 0x00);
    m.io_write(0x61, 0x01);  // B
    m.io_write(0x62, 0x04);  // R
    m.io_write(0x63, 0x10);  // G
    m.poke(0x8000, 0x86);
    check_pixels(first_character(m), {1, 1, 2, 2, 2, 2, 0, 0});
}

void mode_16_colour() {
    // Left pixel I G R B = b7 b5 b3 b1, right = b6 b4 b2 b0, four dots each. 9Ch: 1010 (I+R), 0110 (G+R)
    freetvc::Machine m;
    setup(m);
    m.io_write(0x06, 0x02);
    m.poke(0x8000, 0x9C);
    check_pixels(first_character(m), {10, 10, 10, 10, 6, 6, 6, 6});
}

void palette_mirrors() {
    // 60h-63h mirror up to 6Fh
    freetvc::Machine m;
    setup(m);
    m.io_write(0x6D, 0x50);  // palette 1 = bright green (I b6, G b4)
    m.poke(0x8000, 0x80);
    check_pixels(first_character(m), {12, 0, 0, 0, 0, 0, 0, 0});
}

void vram_address_formula() {
    // Row 1, line 2 of the row, column 3: MA = 67, address = 03h | 2 << 6 | 1 << 8 = 0183h
    freetvc::Machine m;
    setup(m);
    m.io_write(0x61, 0x55);
    m.poke(0x8183, 0xFF);
    m.run({.max_ticks = 2 * kFrameTicks});
    CHECK_EQ(pixel(m, kDisplayX + 3 * 8, kDisplayY + 6), 15u);
    CHECK_EQ(pixel(m, kDisplayX + 3 * 8 - 1, kDisplayY + 6), 0u);
    CHECK_EQ(pixel(m, kDisplayX + 3 * 8, kDisplayY + 5), 0u);
}

void border_and_blanking() {
    freetvc::Machine m;
    setup(m);
    m.io_write(0x00, 0xAA);  // border I-G-R-B- = bright white
    m.run({.max_ticks = 2 * kFrameTicks});
    CHECK_EQ(pixel(m, kDisplayX - 1, 100), 15u);        // left border
    CHECK_EQ(pixel(m, kDisplayX + 64 * 8, 100), 15u);   // right border
    CHECK_EQ(pixel(m, kDisplayX, 100), 0u);             // display, palette 0
    CHECK_EQ(pixel(m, 0, 100), freetvc::Framebuffer::kBlank);    // HSYNC (R3 = 2 characters)
    CHECK_EQ(pixel(m, 15, 100), freetvc::Framebuffer::kBlank);
    CHECK_EQ(pixel(m, 16, 100), 15u);
    CHECK_EQ(pixel(m, 400, 5), freetvc::Framebuffer::kBlank);    // VSYNC until MA9 rises (row 71/72)
    CHECK_EQ(pixel(m, 400, kDisplayY - 4), 15u);                 // border rows 72-77 above the display
}

void decode_matches_generator() {
    // The inspector's picture (bank line y = 64 bytes from y * 64, Video::decode) is the generator's in every mode
    for (std::uint8_t mode = 0; mode < 3; ++mode) {
        freetvc::Machine m;
        setup(m);
        m.io_write(0x06, mode);
        for (unsigned p = 0; p < 4; ++p) {
            m.io_write(static_cast<std::uint8_t>(0x60 + p), static_cast<std::uint8_t>(0x11 * p + 0x40));
        }
        for (unsigned a = 0; a < 0x4000; ++a) {
            m.poke(static_cast<std::uint16_t>(0x8000 + a), static_cast<std::uint8_t>(a * 37 + (a >> 6) * 11));
        }
        m.run({.max_ticks = 2 * kFrameTicks});
        unsigned mismatches = 0;
        std::array<std::uint8_t, 8> px{};
        for (unsigned y = 0; y < 240; ++y) {
            for (unsigned x = 0; x < 64; ++x) {
                freetvc::Video::decode(m.peek(static_cast<std::uint16_t>(0x8000 + y * 64 + x)), mode, m.video().palettes(), px.data());
                for (unsigned k = 0; k < 8; ++k) {
                    mismatches += pixel(m, kDisplayX + static_cast<int>(x * 8 + k), kDisplayY + static_cast<int>(y)) != px[k] ? 1u : 0u;
                }
            }
        }
        CHECK_EQ(mismatches, 0u);
    }
}

void displayed_bytes() {
    // ROM values: 60 rows of 64 from 0 = 0000-3BFF
    std::array<std::uint8_t, freetvc::Mc6845::kRegisters> r{99, 64, 75, 50, 77, 2, 60, 66, 0, 3, 3, 3, 0, 0, 14, 255, 0, 0};
    auto shown = freetvc::displayed_vram(r);
    CHECK_EQ(shown.count(), 0x3C00u);
    CHECK_EQ(static_cast<unsigned>(shown[0x3BFF]), 1u);
    CHECK_EQ(static_cast<unsigned>(shown[0x3C00]), 0u);
    // One character row down (R13 = 40h): 0100-3CFF
    r[13] = 0x40;
    shown = freetvc::displayed_vram(r);
    CHECK_EQ(shown.count(), 0x3C00u);
    CHECK_EQ(static_cast<unsigned>(shown[0x00FF]), 0u);
    CHECK_EQ(static_cast<unsigned>(shown[0x0100]), 1u);
    CHECK_EQ(static_cast<unsigned>(shown[0x3CFF]), 1u);
    // 32 characters a row, 2 lines a row: half of each line, lines 0-1 of each 4
    r = {99, 32, 75, 50, 77, 2, 60, 66, 0, 1, 3, 3, 0, 0, 14, 255, 0, 0};
    shown = freetvc::displayed_vram(r);
    CHECK_EQ(shown.count(), 60u * 32u * 2u);
    CHECK_EQ(static_cast<unsigned>(shown[0x0000]), 1u);
    CHECK_EQ(static_cast<unsigned>(shown[0x0080]), 0u);  // RA 2 is not shown
    CHECK_EQ(static_cast<unsigned>(shown[0x0020]), 1u);  // MA 32-63 make the second row: column 32 of row 0
}

}  // namespace

int main() {
    frame_period();
    cursor_irq_once_per_frame();
    cursor_disabled_by_r10();
    mode_2_colour();
    mode_4_colour();
    mode_16_colour();
    palette_mirrors();
    vram_address_formula();
    border_and_blanking();
    decode_matches_generator();
    displayed_bytes();
    if (g_failures == 0) {
        std::printf("video_test: all checks passed\n");
        return 0;
    }
    std::printf("video_test: %d check(s) failed\n", g_failures);
    return 1;
}
