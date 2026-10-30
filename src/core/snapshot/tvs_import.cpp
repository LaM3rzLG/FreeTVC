// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/snapshot/tvs_import.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#include "core/machine/machine.hpp"
#include "core/message.hpp"

namespace freetvc {

namespace {

// Section offsets of the fixed layout; the payload follows each 4-byte tag
struct Section {
    std::size_t at;
    const char* tag;
};
constexpr Section kCpu{0x00004, "CPU "};
constexpr Section kIout{0x00063, "IOUT"};
constexpr Section kBram{0x0026F, "BRAM"};
constexpr Section kVram{0x10273, "VRAM"};
constexpr Section kCrtc{0x18277, "CRTC"};
constexpr std::array<Section, 8> kSections{{{0x00000, "TS01"}, kCpu, kIout, {0x0016B, "IOIN"}, kBram, kVram, kCrtc, {0x18413, "IRQ "}}};

std::uint16_t word(std::span<const std::uint8_t> b, std::size_t at) { return static_cast<std::uint16_t>(b[at] | b[at + 1] << 8); }

}  // namespace

bool import_tvs(Machine& machine, std::span<const std::uint8_t> file, std::string& error) {
    if (file.size() != kTvsSize) {
        error = msg("not a WinTVC snapshot (%zu bytes, a .tvs has %zu)", file.size(), static_cast<std::size_t>(kTvsSize));
        return false;
    }
    for (const Section& s : kSections) {
        if (std::memcmp(file.data() + s.at, s.tag, 4) != 0) {
            error = msg("not a WinTVC snapshot (no %s section)", s.tag);
            return false;
        }
    }
    const auto cpu = file.subspan(kCpu.at + 4, 0x5B);
    const auto ports = file.subspan(kIout.at + 4, 0x100);
    const auto ram = file.subspan(kBram.at + 4, 0x10000);
    const auto vram = file.subspan(kVram.at + 4, 0x8000);
    const auto crtc = file.subspan(kCrtc.at + 4, 0x6C);

    machine.power_on();
    std::copy(ram.begin(), ram.end(), machine.ram().begin());

    // Ports as the WinTVC loader replays them: paging, video paging and mode first, then palette, border and sound
    for (const std::uint8_t p : {0x02, 0x03, 0x0C, 0x0D, 0x0E, 0x0F, 0x06, 0x60, 0x61, 0x62, 0x63, 0x00, 0x04, 0x05}) {
        machine.io_write(p, ports[p]);
    }
    for (std::uint8_t r = 0; r < 16; ++r) {
        machine.io_write(0x70, r);
        machine.io_write(0x71, crtc[0x48 + r]);
    }
    machine.io_write(0x70, ports[0x70]);

    // VRAM: the displayed bank, and on the 64k+ the bank after it (bank 3's second half is U0 and is skipped)
    const unsigned shown = machine.memory().displayed_bank();
    for (unsigned half = 0; half < 2; ++half) {
        const unsigned bank = shown + half;
        if (bank > 3) {
            break;
        }
        const std::span<std::uint8_t> bytes = machine.bank_bytes(static_cast<Bank>(static_cast<unsigned>(Bank::Vid0) + bank));
        if (bytes.empty()) {
            break;
        }
        std::copy_n(vram.begin() + static_cast<std::ptrdiff_t>(half * 0x4000), 0x4000, bytes.begin());
    }

    Z80Registers regs{};
    regs.af = word(cpu, 0x00);
    regs.bc = word(cpu, 0x02);
    regs.de = word(cpu, 0x04);
    regs.hl = word(cpu, 0x06);
    regs.ix = word(cpu, 0x08);
    regs.iy = word(cpu, 0x0A);
    regs.sp = word(cpu, 0x0C);
    regs.af2 = word(cpu, 0x10);
    regs.bc2 = word(cpu, 0x12);
    regs.de2 = word(cpu, 0x14);
    regs.hl2 = word(cpu, 0x16);
    regs.i = cpu[0x18];
    regs.r = cpu[0x19];
    regs.iff1 = cpu[0x1A] != 0;
    regs.iff2 = cpu[0x1B] != 0;
    regs.im = std::min<std::uint8_t>(cpu[0x1C], 2);
    regs.wz = 0;
    regs.q = 0;
    machine.cpu().set_registers(regs);
    // A halted WinTVC CPU keeps PC on the HALT, so starting there halts again
    machine.jump(word(cpu, 0x0E));
    return true;
}

}  // namespace freetvc
