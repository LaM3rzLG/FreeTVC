// SPDX-License-Identifier: GPL-3.0-or-later
// The TVC256k++ without ROMs: registers, U2/U3 paging, PSRAM, ROM banks, border snoop, reset, functions, snapshots.
#include <bit>
#include <cmath>
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

constexpr std::uint64_t kTicksPerSecond = 6'250'000;
constexpr std::uint8_t kIndex = 0x10;  // slot 0
constexpr std::uint8_t kData = 0x11;
constexpr std::uint8_t kAllRam = 0xB0;  // port 02h: U0, U1, U2, U3

std::vector<std::uint8_t> banked_rom() {
    std::vector<std::uint8_t> rom(freetvc::Card256::kRomSize);
    for (std::size_t i = 0; i < rom.size(); ++i) {
        rom[i] = static_cast<std::uint8_t>(i < 0x1800 ? 0xA0 : 0xB1);
    }
    return rom;
}

// SYS is DI / HALT, so the CPU never wanders through the card's memory
freetvc::MachineConfig config(freetvc::Model model = freetvc::Model::Tvc64kPlus) {
    freetvc::MachineConfig c;
    c.model = model;
    c.roms.sys = {0xF3, 0x76};
    c.cards[0] = {freetvc::CardKind::Card256, banked_rom()};
    return c;
}

// The test reaches into the card's memory directly
freetvc::Card256& card(freetvc::Machine& m) {
    return const_cast<freetvc::Card256&>(dynamic_cast<const freetvc::Card256&>(*m.cards().card(0)));
}

void set(freetvc::Machine& m, std::uint8_t index, std::uint8_t value) {
    m.io_write(kIndex, index);
    m.io_write(kData, value);
}

std::uint8_t get(freetvc::Machine& m, std::uint8_t index) {
    m.io_write(kIndex, index);
    return m.io_read(kData);
}

// The parameter block at C1h = 0: fast-RAM page of U2, offset 0, as the CPU sees it at 8000h
void params(freetvc::Machine& m, const std::vector<std::uint8_t>& bytes) {
    set(m, 0xC1, 0x00);
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        m.poke(static_cast<std::uint16_t>(0x8000 + i), bytes[i]);
    }
}

std::uint8_t call(freetvc::Machine& m, std::uint8_t code) {
    set(m, 0xC0, code);
    check(get(m, 0xC0) == 0xFF, "C0h reads FFh once the function ran");
    return get(m, 0xC2);
}

void test_ports() {
    freetvc::Machine m(config());
    check(m.io_read(0x5A) == 0xFF, "port 5Ah: slot 0 reads 11 (IDA/IDB not connected)");
    check(m.io_read(0x12) == 1 && m.io_read(0x16) == 1, "both increments start at 1");
    check(m.io_read(0x13) == 0 && m.io_read(0x17) == 0, "+3 and +7 read 00h");
    // Sprite 3's X through the data port, the index stepping by one
    m.io_write(kIndex, 0x06);
    m.io_write(kData, 0x34);
    m.io_write(kData, 0x01);
    check(m.io_read(kIndex) == 0x08, "the index grows after each data access");
    check(card(m).peek_register(0x06) == 0x34 && card(m).peek_register(0x07) == 0x01, "sprite 3 X = 0134h");
    // Set 1 with increment 2, through the +8 mirror
    m.io_write(0x1E, 2);
    m.io_write(0x1C, 0x40);
    m.io_write(0x1D, 7);
    m.io_write(0x1D, 9);
    check(card(m).peek_register(0x40) == 7 && card(m).peek_register(0x42) == 9, "set 1 steps by 2 (via the +8 mirror)");
    check(m.io_read(0x14) == 0x44, "set 1's index read back");
    // Flags normalised and the bit words rebuilt
    set(m, 0x75, 0x80);
    check(get(m, 0x75) == 1 && get(m, 0x90) == 0x20 && get(m, 0x91) == 0, "enable 5 made 1, enable word bit 5");
    set(m, 0x91, 0x81);
    check(get(m, 0x78) == 1 && get(m, 0x7F) == 1 && get(m, 0x91) == 0x81, "enable high byte sets sprites 8 and 15");
    check(get(m, 0x92) == 0xFF && get(m, 0x95) == 0xFF, "power-on: foreground and colour mode on for all sprites");
    // Masks and read-only registers
    set(m, 0xA0, 0xFF);
    set(m, 0xA5, 0xFF);
    set(m, 0xA7, 0xFF);
    check(get(m, 0xA0) == 3 && get(m, 0xA5) == 0x87 && get(m, 0xA7) == 0x0F, "video mode, scroll and border masked");
    set(m, 0xA9, 200);
    check(get(m, 0xA9) == 200, "screen height 200");
    set(m, 0xA9, 250);
    check(get(m, 0xA9) == 200, "screen height 250 refused");
    set(m, 0xC3, 9);
    check(get(m, 0xC3) == 0 && get(m, 0xC4) == 24 && get(m, 0xB6) == 8, "version 0.24 and 8 MiB PSRAM, read only");
    set(m, 0xC5, 9);
    check(get(m, 0xC5) == 3 && m.memory().cards().mem_read(0, 0x1810) == 3, "drive select clamped, kept at D810h");
    card(m).mem_write(0x1900, 1);
    card(m).mem_write(0x1901, '.');
    set(m, 0xD6, 0x05);
    check(get(m, 0xD6) == 0x8C, "storage command on the empty DSK drive: no volume (8Ch)");
    set(m, 0xD6, 0x40);
    check(get(m, 0xD6) == 0xFF, "unknown storage command: FFh");
    set(m, 0xD5, 0xFF);
    check(get(m, 0xD5) == 0x7F, "mouse speed b6-0");
    set(m, 0xE3, 0x5A);
    check(get(m, 0xE3) == 0, "a vSID write reads back only once its block is computed");
    m.run({.max_ticks = 20000});  // past the first block's end (3072 vSID clocks = 19487 ticks)
    check(get(m, 0xE3) == 0x5A, "vSID register read back");
    check(get(m, 0xAA) == 0 && get(m, 0xD0) == 0, "unused register and no USB device read 0");
    // Reading the collision word's low byte clears it
    set(m, 0x9A, 0x0F);
    check(get(m, 0x9A) == 0x0F, "collision mask stored");
    // Border: port 00h IGRB in b7, b5, b3, b1 becomes IRGB
    m.io_write(0x00, 0x02);
    check(get(m, 0xA7) == 0x01, "border B");
    m.io_write(0x00, 0x08);
    check(get(m, 0xA7) == 0x04, "border R");
    m.io_write(0x00, 0x20);
    check(get(m, 0xA7) == 0x02, "border G");
    m.io_write(0x00, 0xAA);
    check(get(m, 0xA7) == 0x0F, "border IRGB");
}

void test_memory() {
    freetvc::Machine m(config());
    m.io_write(0x02, kAllRam);
    m.poke(0x8000, 0x11);
    m.poke(0xC000, 0x22);
    freetvc::Card256& c = card(m);
    check(c.fast_ram()[4 * 0x4000] == 0x11 && c.fast_ram()[5 * 0x4000] == 0x22, "U2/U3 are fast-RAM pages 4/5");
    check(m.memory().ram()[2 * 0x4000] == 0, "the board's U2 is not written");
    set(m, 0xB0, 0x09);
    m.poke(0x8000, 0x33);
    check(c.fast_ram()[9 * 0x4000] == 0x33 && m.memory().read(0x8000) == 0x33, "B0h = 9 pages fast-RAM page 9 in");
    set(m, 0xB0, 0x1C);
    check(get(m, 0xB0) == 0x0C, "B0h masked to 0Fh");
    set(m, 0xB0, 0xFF);
    m.poke(0x8000, 0x44);
    check(m.memory().ram()[2 * 0x4000] == 0x44, "B0h = FFh: the board's own U2");
    // Machine::load honours the card too
    set(m, 0xB0, 0x04);
    m.load(0xBFFF, std::vector<std::uint8_t>{0x55, 0x66});
    check(c.fast_ram()[4 * 0x4000 + 0x3FFF] == 0x55 && c.fast_ram()[5 * 0x4000] == 0x66, "load() into the card's pages");

    // A bare 32k board: U2/U3 only from the card
    freetvc::Machine k(config(freetvc::Model::Tvc32k));
    k.io_write(0x02, kAllRam);
    k.poke(0x8000, 0x77);
    check(k.memory().read(0x8000) == 0x77, "32k board: U2 from the card");
    set(k, 0xB0, 0xFF);
    check(k.memory().read(0x8000) == 0xFF, "32k board, B0h = FFh: open bus");

    // ROM banks and card RAM in IOMEM
    m.io_write(0x02, 0xF0);
    m.io_write(0x03, 0x00);
    check(m.memory().read(0xC000) == 0xA0 && m.memory().read(0xD7FF) == 0xA0, "ROM bank 0 at C000-D7FF");
    m.poke(0xC000, 0x00);
    check(m.memory().read(0xC000) == 0xA0, "the ROM bank is not writable");
    m.poke(0xD800, 0x5C);
    check(m.memory().read(0xD800) == 0x5C, "card RAM at D800");
    set(m, 0xB7, 1);
    check(m.memory().read(0xC000) == 0xB1 && get(m, 0xB7) == 1, "ROM bank 1");
    set(m, 0xB7, 2);
    check(get(m, 0xB7) == 1, "B7h = 2 ignored");
    check(m.memory().read(0xD800) == 0x5C, "card RAM stays across banks");
}

void test_psram() {
    freetvc::Machine m(config());
    m.io_write(0x02, kAllRam);
    freetvc::Card256& c = card(m);
    c.psram_write(3 * 0x4000 + 5, 0xA5);
    set(m, 0xB2, 3);
    check(get(m, 0xB0) == 0x10 && get(m, 0xB2) == 3 && get(m, 0xB3) == 0, "PSRAM page 3 at U2: B0h reads 10h");
    check(m.memory().read(0x8005) == 0xA5, "PSRAM page 3 copied into the window");
    m.poke(0x8006, 0x5A);
    check(c.psram_read(3 * 0x4000 + 6) == 0, "a write lands in the window, not yet in PSRAM");
    set(m, 0xB2, 4);
    check(c.psram_read(3 * 0x4000 + 6) == 0x5A, "mapping another page copies the window back");
    check(m.memory().read(0x8006) == 0, "page 4 in the window");
    set(m, 0xB4, 3);
    check(get(m, 0xB1) == 0x11 && m.memory().read(0xC006) == 0x5A, "PSRAM page 3 at U3: B1h reads 11h");
    check(c.psram_pages_used() == 1, "one PSRAM page used");

    // Copies share PSRAM pages until one writes
    freetvc::Machine copy(m);
    card(copy).psram_write(3 * 0x4000 + 6, 0x99);
    check(c.psram_read(3 * 0x4000 + 6) == 0x5A, "a copy's PSRAM write leaves the original alone");
    check(card(copy).psram_read(3 * 0x4000 + 6) == 0x99, "the copy sees its own write");
}

void test_reset() {
    freetvc::Machine m(config());
    set(m, 0xB0, 0x07);
    set(m, 0xA0, 2);
    set(m, 0x90, 0xFF);
    set(m, 0xA5, 0x83);
    m.run({.max_ticks = 2 * kTicksPerSecond});
    m.reset();
    check(get(m, 0xB0) == 7, "warm reset keeps the paging");
    check(get(m, 0xA0) == 0 && get(m, 0x90) == 0 && get(m, 0xA5) == 0, "warm reset: video mode, sprites, scroll cleared");
    set(m, 0xB0, 0x07);
    m.run({.max_ticks = kTicksPerSecond / 2});
    m.reset();
    check(get(m, 0xB0) == 4, "a second reset within a second reboots the card");
}

std::vector<std::uint8_t> block(freetvc::Machine& m, std::uint16_t at, std::size_t n) {
    std::vector<std::uint8_t> out(n);
    for (std::size_t i = 0; i < n; ++i) {
        out[i] = m.memory().read(static_cast<std::uint16_t>(at + i));
    }
    return out;
}

void test_functions() {
    freetvc::Machine m(config());
    m.io_write(0x02, kAllRam);
    freetvc::Card256& c = card(m);
    const std::size_t page4 = 4 * 0x4000;

    // 11: move within page 5 (U3), overlapping
    for (unsigned i = 0; i < 8; ++i) {
        m.poke(static_cast<std::uint16_t>(0xC100 + i), static_cast<std::uint8_t>(i + 1));
    }
    params(m, {5, 0x02, 0x01, 0x00, 0x01, 0x08, 0x00});
    check(call(m, 11) == 0, "move_short ok");
    check(block(m, 0xC100, 10) == std::vector<std::uint8_t>{1, 2, 1, 2, 3, 4, 5, 6, 7, 8}, "move_short is a memmove");
    params(m, {0x12, 0, 0, 0, 1, 1, 0});
    check(call(m, 11) == 1, "move_short page 12h refused");
    params(m, {5, 0x00, 0x3F, 0x00, 0x00, 0x00, 0x02});
    check(call(m, 11) == 2, "move_short past the page refused");

    // 14 then 13: fast RAM to PSRAM and back
    params(m, {0x00, 0x00, 0x08, 0x00, 0x41, 0x01, 0x04, 0x00, 0x00});
    check(call(m, 14) == 0, "to_slow ok");
    check(c.psram_read(0x080000) == 1 && c.psram_read(0x080003) == 2, "to_slow copied page 5 + 100h");
    params(m, {0x00, 0x02, 0x01, 0x00, 0x00, 0x08, 0x04, 0x00, 0x00});
    check(call(m, 13) == 0, "from_slow ok");
    check(block(m, 0x8200, 4) == std::vector<std::uint8_t>{1, 2, 1, 2}, "from_slow into page 4 + 200h");
    params(m, {0, 0, 0, 0, 0, 0x80, 1, 0, 0});
    check(call(m, 13) == 1, "from_slow past 8 MiB refused");

    // 17: one block into 3 chunks of 2, destination step 4
    for (unsigned i = 0; i < 6; ++i) {
        c.fast_ram()[page4 + 0x300 + i] = static_cast<std::uint8_t>(0x10 + i);
    }
    params(m, {0x00, 0x04, 0x01, 0x00, 0x03, 0x01, 2, 0, 3, 0, 4, 0});
    check(call(m, 17) == 0, "chunks_from_block ok");
    check(block(m, 0x8400, 10) == std::vector<std::uint8_t>{0x10, 0x11, 0, 0, 0x12, 0x13, 0, 0, 0x14, 0x15}, "chunks spread out");
    params(m, {0, 4, 1, 0, 3, 1, 0, 0, 3, 0, 4, 0});
    check(call(m, 17) == 1, "chunk size 0 refused");

    // 41: gather the chunks back (destination step 2, source step 4)
    params(m, {0x00, 0x05, 0x01, 0x00, 0x04, 0x01, 2, 0, 3, 0, 2, 0, 4, 0});
    check(call(m, 41) == 0, "move_chunks_2increments ok");
    check(block(m, 0x8500, 6) == std::vector<std::uint8_t>{0x10, 0x11, 0x12, 0x13, 0x14, 0x15}, "chunks gathered");

    // 19: a 2-colour phase mirrored horizontally: 80 00 01 -> 80 00 01 reversed per byte and swapped
    c.fast_ram()[page4 + 0x600] = 0x80;
    c.fast_ram()[page4 + 0x601] = 0x0F;
    c.fast_ram()[page4 + 0x602] = 0x01;
    params(m, {0x00, 0x06, 0x01, 1, 0, 0});
    check(call(m, 19) == 0, "mirror ok");
    check(block(m, 0x8600, 3) == std::vector<std::uint8_t>{0x80, 0xF0, 0x01}, "mirror reverses the 24 pixels");

    // 20: ZX7 "ABABABAB" (literal A, literal B, match length 6 offset 2, end)
    const std::vector<std::uint8_t> zx7{0x41, 0x4B, 0x42, 0x01, 0x00, 0x00, 0x80};
    for (std::size_t i = 0; i < zx7.size(); ++i) {
        c.fast_ram()[page4 + 0x700 + i] = zx7[i];
    }
    params(m, {0x00, 0x08, 0x01, 0x00, 0x07, 0x01, 0, 0, 0, 0});
    check(call(m, 20) == 0, "zx7 ok");
    check(block(m, 0x8800, 9) == std::vector<std::uint8_t>{'A', 'B', 'A', 'B', 'A', 'B', 'A', 'B', 0}, "zx7 unpacked");
    check(block(m, 0x8006, 4) == std::vector<std::uint8_t>{8, 0, 0, 0}, "zx7 size in bytes 6-9");

    // 37: expressions
    params(m, {0x40, 2, 0x40, 3, 1, 0});
    check(call(m, 37) == 0 && block(m, 0x8000, 2) == std::vector<std::uint8_t>{0x40, 5}, "2 + 3 = 5 (byte)");
    params(m, {0x41, 0xD4, 0xFE, 0x40, 2, 3, 0});
    check(call(m, 37) == 0 && block(m, 0x8000, 3) == std::vector<std::uint8_t>{0x41, 0xA8, 0xFD}, "-300 * 2 = -600 (int16)");
    params(m, {0x43, 0, 0, 0, 0, 0x25, 0x40, 0x40, 2, 3, 13, 0});
    check(call(m, 37) == 0 && block(m, 0x8000, 5) == std::vector<std::uint8_t>{0x42, 5, 0, 0, 0}, "TVC 2.5 * 2 to int = 5");
    params(m, {0x82, 10, 15, 0});
    check(call(m, 37) == 0 && block(m, 0x8000, 5) == std::vector<std::uint8_t>{0x42, 1, 0, 0, 0}, "round(sin(pi/2)) = 1");
    params(m, {0x86, 0x88, 4, 0});
    const std::vector<std::uint8_t> f = block(m, 0x8000, 5);
    (void)call(m, 37);
    const std::vector<std::uint8_t> g = block(m, 0x8000, 5);
    const float q = std::bit_cast<float>(static_cast<std::uint32_t>(g[1] | g[2] << 8 | g[3] << 16 | static_cast<std::uint32_t>(g[4]) << 24));
    check(f[0] == 0x86 && g[0] == 0x44 && q == 0.01f, "1 / 100 = 0.01 (float)");
    params(m, {0x40, 7, 0x40, 0, 4, 0});
    check(call(m, 37) == 0 && block(m, 0x8000, 2) == std::vector<std::uint8_t>{0x40, 0}, "integer division by zero gives 0");
    params(m, {0x40, 7, 10, 0});
    check(call(m, 37) == 3, "sin of an integer: error 3");
    params(m, {0x3F, 0});
    check(call(m, 37) == 1, "unknown item: error 1");

    // Unknown codes and the parameter block in U3
    check(call(m, 42) == 0xFF, "a function code without a function answers FFh");
    check(call(m, 15) == 129, "copying a folder to PSRAM without a pendrive: 129");
    set(m, 0xC1, 0x81);
    c.fast_ram()[5 * 0x4000 + 128] = 0x40;
    c.fast_ram()[5 * 0x4000 + 129] = 9;
    c.fast_ram()[5 * 0x4000 + 130] = 0;
    set(m, 0xC0, 37);
    check(get(m, 0xC2) == 0 && m.memory().read(0xC081) == 9, "C1h b7: the block in U3's page");
    set(m, 0xB1, 0xFF);
    set(m, 0xC0, 37);
    check(get(m, 0xC2) == 0xFF, "no fast-RAM page under the block: FFh");

    // C6h chain: a move, then an unknown code
    params(m, {11, 4, 0x00, 0x09, 0x00, 0x00, 0x01, 0x00, 42});
    set(m, 0xC6, 2);
    check(get(m, 0xC6) == 0xFF && get(m, 0xC2) == 0x82, "chain stops at the unknown second function: 82h");
    check(m.memory().read(0x8900) == 11, "the chain's first function ran (copied 8000h to 8900h)");
    params(m, {12, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF, 0x0F});
    set(m, 0xC6, 1);
    check(get(m, 0xC2) == 1 && m.memory().read(0x8000) == 1, "a failing chain function: index 1, its error over the code");
}

void test_snapshot() {
    freetvc::Machine a(config());
    a.io_write(0x02, kAllRam);
    set(a, 0xB0, 0x0A);
    a.poke(0x8123, 0x42);
    card(a).psram_write(0x7FFFFF, 0x24);
    set(a, 0xB4, 0x11);
    set(a, 0x10, 0x99);
    a.io_write(0x00, 0xAA);
    freetvc::Snapshot snap;
    a.save_state(snap);
    freetvc::Machine b(config());
    std::string error;
    check(b.load_state(snap, error), "snapshot loads: " + error);
    check(b.memory().read(0x8123) == 0x42 && get(b, 0xB0) == 0x0A, "fast RAM and paging come back");
    check(card(b).psram_read(0x7FFFFF) == 0x24 && get(b, 0xB4) == 0x11 && get(b, 0xB1) == 0x11, "PSRAM and its mapping come back");
    check(get(b, 0x10) == 0x99 && get(b, 0xA7) == 0x0F, "registers come back");
    check(card(b).psram_pages_used() == 1, "only the used PSRAM page is stored");
}

}  // namespace

int main() {
    test_ports();
    test_memory();
    test_psram();
    test_reset();
    test_functions();
    test_snapshot();
    if (g_failures == 0) {
        std::printf("card256_test: all passed\n");
        return 0;
    }
    std::printf("card256_test: %d failure(s)\n", g_failures);
    return 1;
}
