// SPDX-License-Identifier: GPL-3.0-or-later
// The TVC256k++ overlay (design/cards.md "Overlay"): power-on pictures, mixing, timing, sprites, collisions, IRQ, functions.
#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/cards/card256.hpp"
#include "core/machine/machine.hpp"
#include "core/snapshot/snapshot.hpp"

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

constexpr std::uint64_t kFrameTicks = freetvc::kTStatesPerFrame * freetvc::kTicksPerTState;
constexpr std::uint8_t kIndex = 0x10;
constexpr std::uint8_t kData = 0x11;
// Paper character (0, 0) in the framebuffer with the ROM's CRTC values (video_test.cpp)
constexpr int kPaperX = 200;
constexpr int kPaperY = 49;

// ROM padded with FFh, a font whose byte i is i, a picture whose BMP row r (bottom-up) is all r
std::vector<std::uint8_t> image() {
    std::vector<std::uint8_t> v(freetvc::Card256::kImageSize, 0xFF);
    for (std::size_t i = 0; i < freetvc::Card256::kFontSize; ++i) {
        v[freetvc::Card256::kRomSize + i] = static_cast<std::uint8_t>(i);
    }
    for (std::size_t r = 0; r < 240; ++r) {
        for (std::size_t j = 0; j < 128; ++j) {
            v[freetvc::Card256::kRomSize + freetvc::Card256::kFontSize + r * 128 + j] = static_cast<std::uint8_t>(r);
        }
    }
    return v;
}

freetvc::Card256& card(freetvc::Machine& m) {
    return const_cast<freetvc::Card256&>(dynamic_cast<const freetvc::Card256&>(*m.cards().card(0)));
}

// U0, U1, VID, CART; the CPU spins on DI / JR $; the CRTC as the ROM sets it; palette 0 = colour 1, border colour 3
void setup(freetvc::Machine& m) {
    m.io_write(0x02, 0x10);
    m.ram()[0] = 0xF3;
    m.ram()[1] = 0x18;
    m.ram()[2] = 0xFE;
    m.jump(0x0000);
    const std::array<std::uint8_t, 16> crtc{99, 64, 75, 50, 77, 2, 60, 66, 0, 3, 3, 3, 0, 0, 14, 255};
    for (std::uint8_t r = 0; r < crtc.size(); ++r) {
        m.io_write(0x70, r);
        m.io_write(0x71, crtc[r]);
    }
    m.io_write(0x60, 0x01);  // palette 0: B
    m.io_write(0x00, 0x0A);  // border: R and B (IGRB 3)
}

freetvc::MachineConfig config() {
    freetvc::MachineConfig c;
    c.cards[0] = {freetvc::CardKind::Card256, image()};
    return c;
}

void set(freetvc::Machine& m, std::uint8_t index, std::uint8_t value) {
    m.io_write(kIndex, index);
    m.io_write(kData, value);
}

std::uint8_t get(freetvc::Machine& m, std::uint8_t index) {
    m.io_write(kIndex, index);
    return m.io_read(kData);
}

void frames(freetvc::Machine& m, unsigned n) { m.run({.max_ticks = n * kFrameTicks}); }

// Overlay pixel p of overlay line `line` (21 = the first paper line)
std::uint8_t pixel(const freetvc::Machine& m, unsigned p, unsigned line, unsigned dot = 0) {
    const std::size_t x = kPaperX + 2 * p + dot;
    const std::size_t y = kPaperY + line - 21;
    return m.framebuffer().pixels[y * freetvc::Framebuffer::kWidth + x];
}

std::uint8_t& fast(freetvc::Machine& m, std::uint32_t at) { return card(m).fast_ram()[at]; }

void fill_bitmap(freetvc::Machine& m, std::uint8_t value) {
    for (std::uint32_t i = 0; i < 128 * 240; ++i) {
        fast(m, 0x8000 + i) = value;
    }
}

// A 2-colour sprite phase: row 0 = `row0`, the rest empty
void sprite_phase(freetvc::Machine& m, unsigned phase, std::array<std::uint8_t, 3> row0) {
    for (unsigned i = 0; i < 63; ++i) {
        fast(m, phase * 64 + i) = i < 3 ? row0[i] : 0;
    }
}

void sprite(freetvc::Machine& m, unsigned n, unsigned x, unsigned y, std::uint8_t colour, std::uint8_t phase, bool front, bool full) {
    set(m, static_cast<std::uint8_t>(2 * n), static_cast<std::uint8_t>(x));
    set(m, static_cast<std::uint8_t>(2 * n + 1), static_cast<std::uint8_t>(x >> 8));
    set(m, static_cast<std::uint8_t>(0x20 + 2 * n), static_cast<std::uint8_t>(y));
    set(m, static_cast<std::uint8_t>(0x21 + 2 * n), static_cast<std::uint8_t>(y >> 8));
    set(m, static_cast<std::uint8_t>(0x40 + n), colour);
    set(m, static_cast<std::uint8_t>(0x50 + n), phase);
    set(m, static_cast<std::uint8_t>(0x60 + n), front);
    set(m, static_cast<std::uint8_t>(0x80 + n), full);
    set(m, static_cast<std::uint8_t>(0x70 + n), 1);
}

void test_power_on() {
    freetvc::Machine m(config());
    check(fast(m, 0x1000) == 0 && fast(m, 0x17FF) == 0xFF, "the font at 1000h (A3h = 02)");
    check(fast(m, 0x8000) == 239 && fast(m, 0x8000 + 239 * 128 + 5) == 0, "the picture at 8000h, its bottom-up rows turned");
    check(fast(m, 0) == 0 && fast(m, 1) == 127 && fast(m, 64) == 255 && fast(m, 128 + 3) == 0x3C, "balloon, rectangle, Videoton letters");
    check(fast(m, 0x4000) == 0x88 && fast(m, 0x4000 + 20 * 12 + 4) == 0x77, "the hat at phase 40h, rows turned");
    check(fast(m, 0x400) == 0x20 && fast(m, 0x400 + 300) == 0x20 && fast(m, 0x400 + 1000) == 0x20, "screen RAM cleared to spaces");
    check(fast(m, 0x800) == 0x0F && fast(m, 0x800 + 959) == 0x0F && fast(m, 0x800 + 961) == 0x01, "colour RAM: text colour, then i & 0Fh");
    freetvc::MachineConfig c;
    c.cards[0] = {freetvc::CardKind::Card256, std::vector<std::uint8_t>(freetvc::Card256::kRomSize, 0xFF)};
    freetvc::Machine bare(c);
    check(fast(bare, 0x1000) == 0 && fast(bare, 0x1001) == 0 && fast(bare, 0x8000) == 0, "no font or picture: zeros");
    check(fast(bare, 128 + 3) == 0x3C, "the sprites come from the firmware itself");
}

void test_mixing() {
    freetvc::Machine m(config());
    setup(m);
    fill_bitmap(m, 0x88);
    fast(m, 0x8000) = 0x2A;           // line 21: IRGB 2 (G), then A (I + G)
    fast(m, 0x8000 + 127) = 0x84;     // the last two pixels: transparent, R
    set(m, 0xA0, 3);
    frames(m, 4);
    check(pixel(m, 0, 21) == 0x4 && pixel(m, 0, 21, 1) == 0x4, "IRGB 2 shows as IGRB 4 (green), two dots wide");
    check(pixel(m, 1, 21) == 0xC, "IRGB A shows as IGRB C");
    check(pixel(m, 2, 21) == 0x1, "colour 8 lets the TVC's own pixel through");
    check(pixel(m, 254, 21) == 0x1 && pixel(m, 255, 21) == 0x2, "the 256th pixel: R as IGRB 2");
    check(pixel(m, 0, 21, -1) == 0x3, "the border left of the paper is the TVC's");
    check(pixel(m, 256, 21) == 0x3, "the border right of the paper is the TVC's");
    check(pixel(m, 0, 22) == 0x1, "line 22: transparent");
    // 16-colour mode: B9 latches at 3.125 MHz, one overlay pixel per TVC pixel
    m.io_write(0x06, 0x02);
    frames(m, 2);
    check(pixel(m, 0, 21) == 0x4 && pixel(m, 1, 21) == 0x4, "16-colour mode: the even pixel over four dots");
    check(pixel(m, 254, 21) == 0x0 && pixel(m, 255, 21) == 0x0, "16-colour mode: the odd 256th pixel is not shown (VRAM 00h shows)");
}

void test_timing() {
    freetvc::Machine m(config());
    setup(m);
    fill_bitmap(m, 0x88);
    set(m, 0xA0, 3);
    frames(m, 3);
    m.run({.max_ticks = 2 * kFrameTicks, .to_next_frame = true});
    check(get(m, 0xA8) == static_cast<std::uint8_t>(261), "at the frame's start (in VSYNC) A8h holds 261 & FFh");
    // During the paper the counter is one line ahead of the shown line; before the paper starts, equal to it
    bool ahead = false;
    bool equal = false;
    bool other = false;
    for (unsigned i = 0; i < 600; ++i) {
        m.run({.max_ticks = 2 * 137});
        const unsigned line = m.video().beam_line();
        if (line > kPaperY + 10 && line < kPaperY + 200) {
            const unsigned shown = line - kPaperY + 21;
            const std::uint8_t a8 = get(m, 0xA8);
            ahead |= a8 == shown + 1;
            equal |= a8 == shown;
            other |= a8 != shown && a8 != shown + 1;
        }
    }
    check(ahead && equal && !other, "A8h = the shown line, + 1 from its paper's start");
    // A9h shortens the overlay: below it the pins keep the last pixel
    fill_bitmap(m, 0x22);
    set(m, 0xA9, 100);
    frames(m, 3);
    check(pixel(m, 5, 21 + 99) == 0x4 && pixel(m, 5, 21 + 100) == 0x4, "after line 100 the held pixel (G) stays");
    fast(m, 0x8000 + 99 * 128 + 127) = 0x28;
    frames(m, 2);
    check(pixel(m, 5, 21 + 150) == 0x1, "the held pixel is the last line's 256th (transparent here)");
}

void test_sprites() {
    freetvc::Machine m(config());
    setup(m);
    sprite_phase(m, 1, {0xFF, 0x00, 0x01});
    sprite(m, 0, 24, 21, 0x0C, 1, true, false);  // IRGB C = I + R -> IGRB A
    frames(m, 4);
    check(pixel(m, 0, 21) == 0xA && pixel(m, 7, 21) == 0xA && pixel(m, 8, 21) == 0x1, "2-colour sprite at (24;21): the paper's first pixel");
    check(pixel(m, 23, 21) == 0xA && pixel(m, 0, 22) == 0x1, "its row 0 only");
    // Priority: a back sprite under an opaque bitmap, a front one over it
    fill_bitmap(m, 0x88);
    fast(m, 0x8000) = 0x22;
    set(m, 0xA0, 3);
    set(m, 0x60, 0);
    frames(m, 2);
    check(pixel(m, 0, 21) == 0x4 && pixel(m, 2, 21) == 0xA, "a back sprite shows only through colour 8");
    set(m, 0x60, 1);
    frames(m, 2);
    check(pixel(m, 0, 21) == 0xA, "a front sprite covers the picture");
    // 16-colour sprite: the hat's rows, phase 40h
    sprite(m, 1, 24 + 100, 21, 0, 0x40, true, true);
    frames(m, 2);
    bool hat = true;
    for (unsigned row = 0; row < 21; ++row) {
        for (unsigned p = 0; p < 24; ++p) {
            const std::uint8_t b = fast(m, 0x4000 + row * 12 + p / 2);
            const unsigned c = (p & 1) ? b & 0x0F : b >> 4;
            const std::uint8_t igrb = static_cast<std::uint8_t>((c & 9) | ((c & 2) << 1) | ((c & 4) >> 1));
            hat &= pixel(m, 100 + p, 21 + row) == (c == 8 ? 0x1 : igrb);
        }
    }
    check(hat, "the 16-colour hat at phase 40h, pixel for pixel");
}

void test_collisions_and_irq() {
    freetvc::Machine m(config());
    setup(m);
    sprite_phase(m, 1, {0xFF, 0x00, 0x00});
    sprite(m, 0, 50, 30, 1, 1, true, false);
    sprite(m, 3, 54, 30, 2, 1, false, false);
    frames(m, 3);
    check(get(m, 0x97) == 0 && get(m, 0x96) == 0x09, "sprites 0 and 3 overlap: 96h = 09h (front and back alike)");
    check(get(m, 0x96) == 0, "reading 96h cleared it");
    check((m.io_read(0x59) & 0x0F) == 0x0F, "no mask: the slot's /INT stays high");
    set(m, 0x9A, 0x08);
    frames(m, 2);
    check((m.io_read(0x59) & 0x01) == 0, "mask bit 3: /INT of slot 0 low at the frame's end (59h b0 = 0)");
    m.io_write(0x58, 0x00);
    check((m.io_read(0x59) & 0x01) == 0, "58h does not reach the card (/CLRINT not wired)");
    (void)get(m, 0x96);
    check((m.io_read(0x59) & 0x01) == 1, "reading 96h releases /INT");
    // The CPU takes it: IM 1 with EI, the handler at 0038h counts
    m.ram()[0] = 0xED;
    m.ram()[1] = 0x56;  // IM 1
    m.ram()[2] = 0xFB;  // EI
    m.ram()[3] = 0x18;
    m.ram()[4] = 0xFE;  // JR $
    m.ram()[0x38] = 0x3C;  // INC A
    m.ram()[0x39] = 0x18;
    m.ram()[0x3A] = 0xFE;  // JR $: stays in the handler, interrupts off
    m.jump(0x0000);
    frames(m, 3);
    check(m.pc() >= 0x38 && m.pc() <= 0x3A, "the CPU took the card's interrupt");
    set(m, 0x9A, 0);
    // Background collisions and the firmware's mask quirks
    set(m, 0x70, 0);
    fill_bitmap(m, 0x88);
    set(m, 0xA0, 3);
    frames(m, 2);
    (void)get(m, 0x98);
    frames(m, 2);
    check(get(m, 0x98) == 0x08, "bitmap colour 8 counts as background (the mask holds 10h)");
    fill_bitmap(m, 0x00);
    frames(m, 2);
    (void)get(m, 0x98);
    frames(m, 2);
    check(get(m, 0x98) == 0x00, "black bitmap pixels never collide");
    fill_bitmap(m, 0x11);
    frames(m, 2);
    (void)get(m, 0x98);
    frames(m, 2);
    check(get(m, 0x98) == 0x08, "coloured bitmap pixels collide");
    set(m, 0xA0, 0);
    frames(m, 2);
    check(get(m, 0x98) == 0x00, "mode 0 clears 98h on every line");
    // 2-colour text: the firmware's mask has its 4-pixel halves swapped
    for (unsigned i = 0; i < 960; ++i) {
        fast(m, 0x400 + i) = 0;
    }
    for (unsigned i = 0; i < 8; ++i) {
        fast(m, 0x1000 + i) = 0xF0;  // glyph 0: the left four pixels
    }
    set(m, 0xA0, 1);
    sprite_phase(m, 2, {0xF0, 0x00, 0x00});
    sprite(m, 3, 24 + 4, 21 + 8, 2, 2, true, false);  // pixels 4-7 of the cell
    frames(m, 2);
    (void)get(m, 0x98);
    frames(m, 2);
    check(get(m, 0x98) == 0x08, "2-colour text: the glyph's left pixels collide on the right");
    set(m, 0x06, 24);
    frames(m, 2);
    (void)get(m, 0x98);
    frames(m, 2);
    check(get(m, 0x98) == 0x00, "and its drawn pixels do not");
}

void test_text16() {
    freetvc::Machine m(config());
    setup(m);
    for (unsigned i = 0; i < 960; ++i) {
        fast(m, 0x400 + i) = 0;
    }
    fast(m, 0x400 + 1) = 1;  // cell 1 of row 0: code 1
    const std::array<std::uint8_t, 4> row0{0x12, 0x34, 0x56, 0x78};
    for (unsigned k = 0; k < 4; ++k) {
        fast(m, 1 * 32 + k) = row0[k];             // A3h = 02: the 16-colour font is at 0000h (masked to 8 KiB)
        fast(m, 0x1E000 + 1 * 32 + k) = 0x99;      // A3h = 3Fh: 1E000h, not the manual's 1F800h
    }
    set(m, 0xA0, 2);
    frames(m, 4);
    bool row = true;
    for (unsigned p = 0; p < 7; ++p) {
        const unsigned c = p + 1;
        row &= pixel(m, 8 + p, 21) == ((c & 9) | ((c & 2) << 1) | ((c & 4) >> 1));
    }
    check(row && pixel(m, 15, 21) == 0x1, "16-colour text: four bytes, eight pixels, colour 8 transparent");
    set(m, 0xA3, 0x3F);
    frames(m, 2);
    check(pixel(m, 8, 21) == 0x9 && pixel(m, 15, 21) == 0x9, "font base 3Fh reads the 16-colour font at 1E000h");
}

void test_border_and_scroll() {
    freetvc::Machine m(config());
    setup(m);
    fill_bitmap(m, 0x22);
    set(m, 0xA0, 3);
    set(m, 0xA5, 0x80);
    frames(m, 3);
    check(get(m, 0xA7) == 0x05, "port 00h (R + B) snooped into A7h as IRGB 5");
    check(pixel(m, 0, 40) == 0x3 && pixel(m, 7, 40) == 0x3 && pixel(m, 8, 40) == 0x4, "extended border X: 8 pixels of A7h on the left");
    check(pixel(m, 248, 40) == 0x3 && pixel(m, 247, 40) == 0x4, "and on the right");
    set(m, 0xA5, 0x03);
    fast(m, 0x8000 + 19 * 128) = 0x44;
    frames(m, 2);
    check(pixel(m, 2, 40) == 0x1 && pixel(m, 3, 40) == 0x2 && pixel(m, 5, 40) == 0x4, "scroll X 3: the picture starts at pixel 3");
    set(m, 0xA5, 0);
    set(m, 0xA6, 0x80);
    frames(m, 2);
    check(pixel(m, 50, 28) == 0x3 && pixel(m, 50, 29) == 0x4, "extended border Y: lines 21-28 on top");
    check(pixel(m, 50, 253) == 0x4 && pixel(m, 50, 254) == 0x3, "and lines 254-260 below (the firmware's 7)");
}

std::uint8_t call(freetvc::Machine& m, std::uint8_t code, const std::vector<std::uint8_t>& params) {
    set(m, 0xC1, 0x00);  // the block at U2's page (4), offset 0
    for (std::size_t i = 0; i < params.size(); ++i) {
        fast(m, 4 * 0x4000 + static_cast<std::uint32_t>(i)) = params[i];
    }
    set(m, 0xC0, code);
    return get(m, 0xC2);
}

std::uint8_t dot(freetvc::Machine& m, unsigned x, unsigned y) {
    const std::uint8_t b = fast(m, 0x8000 + y * 128 + x / 2);
    return (x & 1) ? b & 0x0F : b >> 4;
}

void test_functions() {
    freetvc::Machine m(config());
    check(call(m, 0x01, {}) == 0 && fast(m, 0x8000) == 0x88 && fast(m, 0x8000 + 239 * 128 + 127) == 0x88, "01h fills the bitmap with 88h");
    call(m, 0x16, {5});
    check(call(m, 0x15, {}) == 0 && fast(m, 4 * 0x4000) == 5, "15h/16h pen colour");
    call(m, 0x18, {3, 2});
    check(dot(m, 3, 2) == 5 && dot(m, 2, 2) == 8, "18h sets the odd pixel's low nibble");
    check(call(m, 0x17, {3, 2}) == 0 && fast(m, 4 * 0x4000) == 5, "17h reads it back");
    check(call(m, 0x18, {3, 240}) == 1, "18h below the screen height: error 1");
    call(m, 0x19, {0, 0, 10, 0, 10, 0, 10, 0});
    check(dot(m, 0, 10) == 5 && dot(m, 5, 10) == 5 && dot(m, 10, 10) == 5 && dot(m, 11, 10) == 8, "19h horizontal line");
    call(m, 0x1B, {20, 0, 20, 0, 4, 3});
    check(dot(m, 20, 20) == 5 && dot(m, 24, 22) == 5 && dot(m, 20, 23) == 8, "1Bh fills w + 1 pixels and h lines (not h + 1)");
    call(m, 0x16, {8});
    call(m, 0x26, {0, 0, 50, 0, 50, 0, 60, 0, 10, 0, 60, 0, 0x3C});  // clockwise in screen terms
    check(dot(m, 30, 55) == 8, "26h: a clockwise triangle is not drawn");
    call(m, 0x26, {0, 0, 50, 0, 10, 0, 60, 0, 50, 0, 60, 0, 0x3C});
    check(dot(m, 30, 58) == 3 && dot(m, 31, 58) == 0xC && dot(m, 30, 59) == 0xC, "26h: checkerboard, swapped on odd lines");
    call(m, 0x16, {7});
    for (unsigned i = 0; i < 128; ++i) {
        fast(m, 0x8000 + 240 * 128 + i) = 0x88;
    }
    call(m, 0x1D, {100, 100});
    check(dot(m, 0, 239) == 7 && dot(m, 100, 100) == 7 && dot(m, 3, 2) == 5 && dot(m, 30, 58) == 3, "1Dh floods colour 8 only");
    check(fast(m, 0x8000 + 240 * 128) == 0x77, "1Dh also fills the line under the screen height (firmware)");
    // Ellipses: a circle of radius 5 round (50, 150)
    call(m, 0x01, {});
    call(m, 0x16, {9});
    call(m, 0x1E, {50, 0, 150, 0, 5, 5});
    check(dot(m, 55, 150) == 9 && dot(m, 45, 150) == 9 && dot(m, 50, 155) == 9 && dot(m, 50, 145) == 9 && dot(m, 50, 150) == 8,
          "1Eh: the outline's four extremes, the centre empty");
    call(m, 0x1F, {80, 0, 150, 0, 5, 5});
    check(dot(m, 80, 150) == 9 && dot(m, 84, 152) == 9 && dot(m, 86, 150) == 8, "1Fh: filled");
    // Image blocks: 4 x 2 pixels at 20000h, colour 8 in the third
    fast(m, 0x20000) = 0x12;
    fast(m, 0x20001) = 0x8F;
    fast(m, 0x20002) = 0x34;
    fast(m, 0x20003) = 0x56;
    call(m, 0x01, {});
    fast(m, 0x8000 + 100 * 128 + 6) = 0x77;  // pixels 12 and 13 of line 100
    call(m, 0x20, {10, 0, 100, 0, 4, 2, 0x00, 0x00, 0x02});
    check(dot(m, 10, 100) == 1 && dot(m, 11, 100) == 2 && dot(m, 12, 100) == 7 && dot(m, 13, 100) == 0xF && dot(m, 13, 101) == 6,
          "20h: aligned, colour 8 keeps the bitmap's pixel");
    call(m, 0x20, {21, 0, 110, 0, 4, 2, 0x00, 0x00, 0x02});
    check(dot(m, 21, 110) == 1 && dot(m, 22, 110) == 2 && dot(m, 23, 110) == 8 && dot(m, 24, 110) == 0xF && dot(m, 24, 111) == 6,
          "20h: to an odd X");
    call(m, 0x20, {0xFF, 0xFF, 120, 0, 4, 2, 0x00, 0x00, 0x02});
    check(dot(m, 0, 120) == 2 && dot(m, 1, 120) == 8 && dot(m, 2, 120) == 0xF && dot(m, 0, 121) == 4, "20h: X = -1 clips the first pixel");
    call(m, 0x21, {30, 0, 130, 0, 3, 1, 0xFF, 0xFF, 0, 0, 4, 0, 2, 0, 0x00, 0x00, 0x02, 0x1C});
    check(dot(m, 30, 130) == 0xC && dot(m, 31, 130) == 1 && dot(m, 32, 130) == 2, "21h: outside the source the default colour");
    call(m, 0x21, {30, 0, 131, 0, 4, 1, 0, 0, 0, 0, 4, 0, 2, 0, 0x00, 0x00, 0x02, 0x10});
    call(m, 0x21, {30, 0, 132, 0, 4, 1, 0, 0, 0, 0, 4, 0, 2, 0, 0x00, 0x00, 0x02, 0x00});
    check(dot(m, 32, 131) == 8 && dot(m, 32, 132) == 8 && dot(m, 33, 131) == 0xF, "21h: colour 8 written (the bitmap is 8 here too)");
    fast(m, 0x8000 + 133 * 128 + 16) = 0x55;
    call(m, 0x21, {30, 0, 133, 0, 4, 1, 0, 0, 0, 0, 4, 0, 2, 0, 0x00, 0x00, 0x02, 0x10});
    check(dot(m, 32, 133) == 5, "21h: colour 8 skipped when b7-4 of the default colour are set");
    // Checkerboards
    call(m, 0x16, {8});
    call(m, 0x27, {100, 0, 150, 0, 3, 1, 0x12});
    check(dot(m, 100, 150) == 1 && dot(m, 101, 150) == 2 && dot(m, 103, 150) == 2 && dot(m, 100, 151) == 2 && dot(m, 101, 151) == 1,
          "27h: two colours, swapped on odd lines, the last line included");
    call(m, 0x01, {});
    call(m, 0x28, {0, 0, 0x34});
    check(dot(m, 0, 0) == 3 && dot(m, 1, 0) == 4 && dot(m, 0, 1) == 4 && dot(m, 255, 239) == 3, "28h: the whole bitmap as a checkerboard");
    // Text
    call(m, 0x0A, {40, 3});
    check(call(m, 0x09, {}) == 0 && fast(m, 4 * 0x4000) == 31 && fast(m, 4 * 0x4000 + 1) == 3, "0Ah clamps X to 31");
    call(m, 0x08, {0x0E});
    call(m, 0x06, {'A', 'b', '1', 0xF2, 0x90, 0});
    check(fast(m, 0x400 + 3 * 32 + 31) == 1 && fast(m, 0x400 + 4 * 32) == 0x82 && fast(m, 0x400 + 4 * 32 + 1) == '1',
          "06h maps ASCII to screen codes and wraps the line");
    check(fast(m, 0x800 + 3 * 32 + 31) == 0x0E && fast(m, 0x800 + 4 * 32 + 2) == 0x02 && fast(m, 0x400 + 4 * 32 + 2) == 0xF0,
          "F2h sets colour 2, 90h is á");
    call(m, 0x04, {0xCD, 0xAB});
    check(fast(m, 0x400 + 4 * 32 + 3) == 1 && fast(m, 0x400 + 4 * 32 + 4) == 2 && fast(m, 0x400 + 4 * 32 + 6) == 4, "04h prints ABCD");
}

void test_reset_and_snapshot() {
    freetvc::Machine m(config());
    setup(m);
    sprite_phase(m, 1, {0xFF, 0x00, 0x00});
    sprite(m, 0, 24, 21, 4, 1, true, false);
    fill_bitmap(m, 0x22);
    set(m, 0xA0, 3);
    frames(m, 3);
    freetvc::Snapshot snap;
    m.run({.max_ticks = kFrameTicks / 3});
    m.save_state(snap);
    freetvc::Machine n(config());
    std::string error;
    check(n.load_state(snap, error), "snapshot loads: " + error);
    frames(m, 2);
    frames(n, 2);
    check(m.framebuffer().pixels == n.framebuffer().pixels, "a snapshot taken mid-frame goes on with the same pictures");
    frames(m, 60);  // more than 1 s after power-on, so not the double press
    m.run({.max_ticks = 2 * kFrameTicks, .to_next_frame = true});
    m.run({.max_ticks = kFrameTicks / 2});
    m.reset();
    check(get(m, 0x70) == 0 && get(m, 0xA0) == 0 && fast(m, 0x8000) == 0x22, "reset: sprites off, mode 0, RAM kept");
    frames(m, 1);
    check(pixel(m, 100, 230) == 0x4, "reset: until the overlay restarts the pins hold the last pixel (G)");
    frames(m, 3);
    check(pixel(m, 0, 21) != 0x4 && pixel(m, 100, 200) != 0x4, "reset: then no overlay");
}

}  // namespace

int main() {
    test_power_on();
    test_mixing();
    test_timing();
    test_sprites();
    test_collisions_and_irq();
    test_text16();
    test_border_and_scroll();
    test_functions();
    test_reset_and_snapshot();
    if (g_failures == 0) {
        std::printf("card256_overlay_test: all passed\n");
        return 0;
    }
    std::printf("card256_overlay_test: %d failure(s)\n", g_failures);
    return 1;
}
