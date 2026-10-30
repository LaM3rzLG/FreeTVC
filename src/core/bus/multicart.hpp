// SPDX-License-Identifier: GPL-3.0-or-later
// Vass Sándor's MultiCart v2 (board seleprom6-7, design/cards.md "MultiCart v2"): a PLA (a 128 KiB flash used as
// a look-up table, tvcpla.pas) decodes the cartridge window; a 74HCT273 holds the register (b4-0 page, b5 second chip,
// b6 RAM, b7 register at E000 instead of C000), a 74HCT74 the reset-on-read latch; two SST39SF040 and 128 KiB SRAM.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "core/bus/jedec_flash.hpp"

namespace freetvc {

class ChunkWriter;
class ChunkReader;

// The two flash chips' bytes, kept outside the machine state like a disk; the front end writes them back.
struct MultiCartFlash {
    static constexpr std::size_t kSize = 2 * std::size_t{kSst39sf040.size};
    std::vector<std::uint8_t> bytes;  // chip 0 then chip 1; empty = no MultiCart
    bool dirty = false;
    std::uint64_t last_write = 0;  // machine tick of the last change
};

class MultiCart {
public:
    static constexpr std::size_t kRamSize = 0x20000;
    static constexpr std::uint8_t kChip1 = 0x20;
    static constexpr std::uint8_t kRam = 0x40;
    static constexpr std::uint8_t kRelocated = 0x80;

    // PLA address inputs as the board wires them
    enum PlaInput : std::uint32_t {
        kInA13 = 1u << 9,      // CPU A13
        kInBrd = 1u << 10,     // /BRD, low on a read
        kInWindow = 1u << 11,  // /OE from the second 74HCT123, low while the outputs are enabled
        kInRelocated = 1u << 12,
        kInRam = 1u << 13,
        kInChip1 = 1u << 14,
        kInResetOnRead = 1u << 15,  // RESENA, the 74HCT74's /Q
        kInPowerOn = 1u << 16,      // /RST from the power-on RC circuit
    };
    // PLA data outputs, all active low except D7 (not wired on the board)
    enum PlaOutput : std::uint8_t {
        kOutRomCe0 = 1u << 0,
        kOutRomCe1 = 1u << 1,
        kOutRamCe = 1u << 2,
        kOutMemRd = 1u << 3,
        kOutMemWr = 1u << 4,
        kOutRegWr = 1u << 5,
        kOutRegClr = 1u << 6,
    };
    // The PLA's byte for its 17 address inputs (the logic of tvcpla.pas)
    static std::uint8_t pla(std::uint32_t inputs);

    MultiCart();
    void power_on();
    // CPU cycles at `offset` in the 16 KiB window (A13-A0), at machine `tick`
    std::uint8_t read(std::uint16_t offset, std::uint64_t tick, MultiCartFlash* flash);
    void write(std::uint16_t offset, std::uint8_t value, std::uint64_t tick, MultiCartFlash* flash);
    // Debugger access: what the window holds now, without side effects; writes reach the RAM only
    std::uint8_t peek(std::uint16_t offset, const MultiCartFlash* flash) const;
    void poke(std::uint16_t offset, std::uint8_t value);
    // The 16 KiB the register maps (a flash page or a RAM page); empty without the flash
    std::span<std::uint8_t> window(MultiCartFlash* flash);

    std::uint8_t reg() const { return reg_; }
    bool reset_on_read() const { return !q_; }
    const JedecFlash& chip(unsigned n) const { return chips_[n & 1]; }
    std::span<const std::uint8_t> ram() const { return ram_; }
    void save(ChunkWriter& w) const;
    void load(ChunkReader& r);

private:
    std::uint32_t inputs(std::uint16_t offset, bool read) const;
    std::uint32_t flash_address(std::uint16_t offset) const;
    std::size_t ram_address(std::uint16_t offset) const;

    std::uint8_t reg_ = 0;
    bool q_ = false;  // the 74HCT74 latches A1 at the end of a register write
    std::vector<std::uint8_t> ram_;
    std::array<JedecFlash, 2> chips_{JedecFlash(kSst39sf040), JedecFlash(kSst39sf040)};
};

}  // namespace freetvc
