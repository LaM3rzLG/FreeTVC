// SPDX-License-Identifier: GPL-3.0-or-later
// The MultiCart v2 without system ROMs (design/cards.md "MultiCart v2"): the PLA against the board's tvcpla.bin,
// the register and its reset-on-read latch, the RAM, the SST39SF040's commands and status, snapshots, the image builder;
// usage: multicart_test [tvcpla.bin].
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <cstdio>
#include <string>
#include <vector>

#include "core/bus/multicart.hpp"
#include "core/machine/machine.hpp"
#include "core/snapshot/snapshot.hpp"
#include "frontend/common/multicart_image.hpp"
#include "frontend/common/presets.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;
using freetvc::MultiCart;
using freetvc::MultiCartFlash;

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

constexpr std::uint64_t kTicksPerMs = 6'250;

MultiCartFlash erased_flash() {
    MultiCartFlash f;
    f.bytes.assign(MultiCartFlash::kSize, 0xFF);
    return f;
}

// A flash command cycle at chip address `address` through the register at C002h (no reset on read)
void command(MultiCart& mc, MultiCartFlash& f, unsigned chip, std::uint32_t address, std::uint8_t value, std::uint64_t tick) {
    mc.write(0x0002, static_cast<std::uint8_t>((chip ? MultiCart::kChip1 : 0) | (address >> 14)), tick, &f);
    mc.write(static_cast<std::uint16_t>(address & 0x3FFF), value, tick, &f);
}

void unlock(MultiCart& mc, MultiCartFlash& f, unsigned chip, std::uint64_t tick) {
    command(mc, f, chip, 0x5555, 0xAA, tick);
    command(mc, f, chip, 0x2AAA, 0x55, tick);
}

void test_pla(const char* path) {
    std::string error;
    const auto table = path ? freetvc::read_file(path, 0x20000, error) : std::nullopt;
    if (!table || table->size() != 0x20000) {
        std::printf("note: no tvcpla.bin, the PLA table check is skipped\n");
        return;
    }
    unsigned mismatches = 0;
    for (std::uint32_t i = 0; i < 0x20000; ++i) {
        mismatches += MultiCart::pla(i) != (*table)[i];
    }
    check(mismatches == 0, "the PLA logic equals tvcpla.bin for all 131072 inputs (" + std::to_string(mismatches) + " differ)");
}

void test_register() {
    MultiCartFlash f = erased_flash();
    f.bytes[0] = 0x11;                      // chip 0 page 0
    f.bytes[0x2000] = 0x22;
    f.bytes[3 * 0x4000 + 0x0100] = 0x33;    // chip 0 page 3
    f.bytes[0x80000 + 0x0100] = 0x44;       // chip 1 page 0
    MultiCart mc;
    check(mc.reg() == 0 && mc.reset_on_read(), "power-on: register 0, reset on read");
    mc.write(0x0000, 3, 0, &f);
    check(mc.reg() == 3 && mc.reset_on_read(), "a write at C000h sets the register and enables the reset");
    check(mc.read(0x0100, 0, &f) == 0x33, "page 3 of chip 0");
    check(mc.read(0x0000, 0, &f) == 0x11 && mc.reg() == 0, "a read of C000h clears the register and reads page 0");
    mc.write(0x0003, MultiCart::kChip1, 0, &f);
    check(mc.reg() == MultiCart::kChip1 && !mc.reset_on_read(), "C003h (A1 set) disables the reset on read");
    check(mc.read(0x0001, 0, &f) == 0xFF && mc.reg() == MultiCart::kChip1, "a read of C001h keeps it");
    check(mc.read(0x0100, 0, &f) == 0x44, "chip 1");
    mc.write(0x0004, 0x55, 0, &f);
    check(mc.reg() == MultiCart::kChip1, "C004h is not the register (A2 decoded)");
    mc.write(0x0200, 0x55, 0, &f);
    check(mc.reg() == MultiCart::kChip1, "C200h is not the register (A9 through the 74HCT32)");

    // Relocated: the register moves to E000h-E003h, C000h becomes flash
    mc.write(0x0000, MultiCart::kRelocated | 3, 0, &f);
    check(mc.reg() == (MultiCart::kRelocated | 3), "relocated register");
    mc.write(0x0000, 0x00, 0, &f);
    check(mc.reg() == (MultiCart::kRelocated | 3), "a write at C000h goes to the flash once relocated");
    check(mc.read(0x0100, 0, &f) == 0x33 && mc.read(0x0000, 0, &f) == 0xFF && mc.reg() == (MultiCart::kRelocated | 3),
          "a read of C000h does not reset a relocated register");
    check(mc.read(0x2000, 0, &f) == 0x22 && mc.reg() == 0, "a read of E000h does, and reads page 0 after the clear");
    mc.write(0x2000, 7, 0, &f);
    check(mc.reg() == 0, "E000h is not the register unless relocated");

    mc.power_on();
    check(mc.reg() == 0 && mc.reset_on_read(), "power-on clears both");
    check(mc.read(0x0100, 0, nullptr) == 0xFF, "no flash attached reads FFh");
}

void test_ram() {
    MultiCartFlash f = erased_flash();
    MultiCart mc;
    mc.write(0x0002, MultiCart::kRam | 3, 0, &f);
    mc.write(0x1975, 0x64, 0, &f);
    check(mc.read(0x1975, 0, &f) == 0x64 && mc.ram()[3 * 0x4000 + 0x1975] == 0x64, "RAM page 3");
    mc.write(0x0002, MultiCart::kRam | 11, 0, &f);
    check(mc.read(0x1975, 0, &f) == 0x64, "128 KiB: page 11 is page 3 again (the firmware's size test)");
    check(mc.read(0x0000, 0, &f) == 0x00 && mc.reg() == (MultiCart::kRam | 11), "C000h reads RAM; no reset after C002h");
    mc.write(0x0000, MultiCart::kRam | 1, 0, &f);
    mc.write(0x0000, 0x99, 0, &f);
    check(mc.reg() == 0x99, "C000h stays the register over RAM");
    check(std::all_of(f.bytes.begin(), f.bytes.end(), [](std::uint8_t b) { return b == 0xFF; }) && !f.dirty, "RAM never writes the flash");
}

void test_flash() {
    MultiCartFlash f = erased_flash();
    MultiCart mc;
    std::uint64_t t = 1000;
    // Software ID on both chips (Table 3-1), exit with F0h anywhere
    for (unsigned chip = 0; chip < 2; ++chip) {
        unlock(mc, f, chip, t);
        command(mc, f, chip, 0x5555, 0x90, t);
        mc.write(0x0002, chip ? MultiCart::kChip1 : 0, t, &f);
        check(mc.read(0x0000, t, &f) == 0xBF && mc.read(0x0001, t, &f) == 0xB7, "ID mode: BFh, B7h");
        mc.write(0x00FF, 0xF0, t, &f);
        check(mc.read(0x0000, t, &f) == 0xFF, "F0h leaves ID mode");
    }
    // Byte program: 14 µs with Data# polling and the toggle bit, commands ignored meanwhile
    unlock(mc, f, 1, t);
    command(mc, f, 1, 0x5555, 0xA0, t);
    command(mc, f, 1, 0x12345, 0x5A, t);
    check(f.bytes[0x80000 + 0x12345] == 0x5A && f.dirty && f.last_write == t, "byte programmed on chip 1, the flash dirty");
    mc.write(0x0002, MultiCart::kChip1 | (0x12345 >> 14), t, &f);
    const std::uint8_t s1 = mc.read(0x12345 & 0x3FFF, t + 1, &f);
    const std::uint8_t s2 = mc.read(0x12345 & 0x3FFF, t + 2, &f);
    check((s1 & 0x80) == 0x80 && (s1 & 0x40) && !(s2 & 0x40), "while busy: DQ7 = not data, DQ6 toggles, high first");
    check(mc.read(0x12345 & 0x3FFF, t + 88, &f) == 0x5A, "done after 14 µs");
    // Programming only clears bits
    unlock(mc, f, 1, t + 100);
    command(mc, f, 1, 0x5555, 0xA0, t + 100);
    command(mc, f, 1, 0x12345, 0xF0, t + 100);
    check(f.bytes[0x80000 + 0x12345] == 0x50, "program ANDs into the cell");
    // A broken sequence aborts; a write without the unlock does nothing
    f.dirty = false;
    command(mc, f, 0, 0x5555, 0xAA, t + 200);
    command(mc, f, 0, 0x2AAB, 0x55, t + 200);
    command(mc, f, 0, 0x5555, 0xA0, t + 200);
    command(mc, f, 0, 0x0100, 0x00, t + 200);
    check(f.bytes[0x0100] == 0xFF && !f.dirty, "a wrong address aborts the sequence");
    // A14-A0 only: 0x7D555 is 5555h too
    command(mc, f, 0, 0x7D555, 0xAA, t + 300);
    command(mc, f, 0, 0x2AAA, 0x55, t + 300);
    command(mc, f, 0, 0x5555, 0xA0, t + 300);
    command(mc, f, 0, 0x0100, 0x12, t + 300);
    check(f.bytes[0x0100] == 0x12, "command addresses compare A14-A0");
    // Sector erase: 4 KiB, 18 ms, DQ7 reads 0; a command during it is ignored
    f.bytes[0x0FFF] = 0;
    f.bytes[0x1000] = 0;
    t = 10 * kTicksPerMs;
    unlock(mc, f, 0, t);
    command(mc, f, 0, 0x5555, 0x80, t);
    unlock(mc, f, 0, t);
    command(mc, f, 0, 0x0075, 0x30, t);
    check(f.bytes[0x0100] == 0xFF && f.bytes[0x0FFF] == 0xFF && f.bytes[0x1000] == 0x00, "sector 0 erased, sector 1 kept");
    mc.write(0x0002, 0, t, &f);
    check((mc.read(0x0100, t + 17 * kTicksPerMs, &f) & 0x80) == 0, "erasing: DQ7 reads 0");
    unlock(mc, f, 0, t + kTicksPerMs);
    command(mc, f, 0, 0x5555, 0xA0, t + kTicksPerMs);
    command(mc, f, 0, 0x0200, 0x00, t + kTicksPerMs);
    check(f.bytes[0x0200] == 0xFF, "a program during the erase is ignored");
    mc.write(0x0002, 0, t, &f);
    check(mc.read(0x0100, t + 18 * kTicksPerMs, &f) == 0xFF, "erased after 18 ms");
    // Chip erase: chip 0 only, 70 ms
    t = 100 * kTicksPerMs;
    f.bytes[0x7FFFF] = 0;
    unlock(mc, f, 0, t);
    command(mc, f, 0, 0x5555, 0x80, t);
    unlock(mc, f, 0, t);
    command(mc, f, 0, 0x5555, 0x10, t);
    check(f.bytes[0x7FFFF] == 0xFF && f.bytes[0x80000 + 0x12345] == 0x50, "chip erase clears chip 0 only");
    check(mc.chip(0).busy(t + 69 * kTicksPerMs) && !mc.chip(0).busy(t + 70 * kTicksPerMs), "chip erase takes 70 ms");
}

void test_machine() {
    freetvc::MachineConfig config;
    config.roms.cart_type = freetvc::CartType::MultiCart;
    // LD A,42h; LD (C002h),A; LD A,ABh; LD (C010h),A; HALT: RAM page 2 without the reset on read, a byte into it
    config.roms.sys = {0x3E, 0x42, 0x32, 0x02, 0xC0, 0x3E, 0xAB, 0x32, 0x10, 0xC0, 0x76};
    MultiCartFlash f = erased_flash();
    f.bytes[0] = 'M';
    freetvc::Machine m(config);
    m.set_multicart_flash(&f);
    const freetvc::MemoryMap& mem = m.memory();
    check(mem.multicart() != nullptr && m.config().roms.has_cart(), "the board is in");
    check(mem.read(0xC000) == 'M' && mem.multicart()->reg() == 0, "a debugger read of CART (page 3 after power-on) does not reset");
    m.run({.max_ticks = 400});
    check(mem.multicart()->reg() == (MultiCart::kRam | 2) && !mem.multicart()->reset_on_read(), "the CPU wrote the register at C002h");
    check(mem.read(0xC010) == 0xAB && mem.bank_bytes(freetvc::Bank::Cart)[0x10] == 0xAB, "the CPU wrote RAM page 2; CART shows it");
    m.poke(0xC011, 0xCD);
    check(mem.read(0xC011) == 0xCD && mem.multicart()->ram()[2 * 0x4000 + 0x11] == 0xCD, "debugger writes reach the RAM");
    m.reset();
    check(mem.multicart()->reg() == (MultiCart::kRam | 2), "the reset button does not reach the cart");

    freetvc::Snapshot snap;
    m.save_state(snap);
    m.poke(0xC010, 0x00);
    std::string error;
    check(m.load_state(snap, error), "snapshot loads: " + error);
    check(m.memory().multicart()->reg() == (MultiCart::kRam | 2) && m.memory().read(0xC010) == 0xAB, "snapshot: register and RAM back");
    check(m.multicart_flash() == &f, "the flash stays attached across a snapshot load");
    const freetvc::Machine copy(m);
    check(copy.multicart_flash() == nullptr && copy.memory().multicart()->reg() == (MultiCart::kRam | 2),
          "a copy has the board but no flash");
    m.power_on();
    check(m.memory().multicart()->reg() == 0 && m.memory().read(0xC000) == 'M', "power-on clears the register");
}

Bytes cas_file(const Bytes& payload) {
    Bytes cas(0x90, 0);
    cas[0] = 0x11;
    cas[0x81] = 0x01;
    cas[0x82] = static_cast<std::uint8_t>(payload.size());
    cas[0x83] = static_cast<std::uint8_t>(payload.size() >> 8);
    cas.insert(cas.end(), payload.begin(), payload.end());
    return cas;
}

void test_builder() {
    Bytes firmware(0x328C, 0xEE);
    const std::uint8_t head[] = {'M', 'O', 'P', 'S', 0x18, 0x0E, 0x21, 0x0E, 0x00, 0xF0};
    std::copy(std::begin(head), std::end(head), firmware.begin());
    std::string error;
    Bytes payload(600);
    for (std::size_t i = 0; i < payload.size(); ++i) {
        payload[i] = static_cast<std::uint8_t>(i);
    }
    Bytes cart(0x2000, 0x5A);
    std::copy(head, head + 4, cart.begin());
    Bytes bad_length = cas_file({1, 2, 3});
    bad_length[0x82] = 0;
    const std::vector<freetvc::MultiCartFile> files{{"Árvíztűrő tükörfúrógép", cas_file(payload), true},
                                                    {"game", cart, false},
                                                    {"raw", bad_length, true}};
    const auto image = freetvc::build_multicart_image(firmware, files, error);
    check(image && image->size() == MultiCartFlash::kSize, "an image is built: " + error);
    if (!image) {
        return;
    }
    const Bytes& rom = *image;
    check(freetvc::is_multicart_image(rom) && rom[0x2FFF] == 0xEE && rom[0x3200] == 1 && rom[0x3201] == 0xEE,
          "the firmware's first 12 KiB and its first entry");
    const auto dir = freetvc::multicart_directory(rom);
    check(dir.size() == 3, "three entries");
    if (dir.size() == 3) {
        check(dir[0].type == 3 && dir[0].bank == 1 && dir[0].block == 0 && dir[0].blocks == 3 && dir[0].name == "Árvíztűrő tükörf",
              "CAS: bank 1 block 0, 3 blocks, 16 characters with the TVC's accents: " + dir[0].name);
        check(dir[1].type == 2 && dir[1].bank == 2 && dir[1].block == 0 && dir[1].blocks == 32, "cart: the next whole bank");
        check(dir[2].type == 4 && dir[2].bank == 1 && dir[2].block == 3 && dir[2].blocks == 1, "no length: BIN, next free block");
    }
    // The CAS chain: <bank, block> links, then FFh and the bytes in the last block
    const std::size_t b0 = 0x4000;
    check(rom[b0] == 1 && rom[b0 + 1] == 1 && rom[b0 + 2] == 0 && rom[b0 + 255] == 253, "first block links to bank 1 block 1");
    check(rom[b0 + 0x200] == 0xFF && rom[b0 + 0x201] == 600 - 2 * 254 && rom[b0 + 0x202] == static_cast<std::uint8_t>(508),
          "last block: FFh, 92 bytes");
    check(rom[0x8000] == 'M' && rom[0x9FFF] == 0x5A && rom[0xA000] == 0, "cart copied into bank 2");
    check(rom[0x3000] == 0xFF && rom[0x3007] == 0xFF && rom[0x3008] == 0x0F && rom[0x3010] == 0xFF && rom[0x3014] == 0x00 &&
              rom[0x3100] == 0x00,
          "BAM: firmware bank, 4 blocks of bank 1, 32 of bank 2, chip 1 empty");
    check(rom[0x80000] == 0 && rom[0x3214 + 3 * 20] == 0, "chips start zeroed, the directory ends with 0");
    const auto too_big = freetvc::build_multicart_image(firmware, {{"x", Bytes(10, 1), false}}, error);
    check(!too_big, "neither CAS nor MOPS is refused");
}

}  // namespace

int main(int argc, char** argv) {
    test_pla(argc > 1 ? argv[1] : nullptr);
    test_register();
    test_ram();
    test_flash();
    test_machine();
    test_builder();
    if (g_failures == 0) {
        std::printf("multicart: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
