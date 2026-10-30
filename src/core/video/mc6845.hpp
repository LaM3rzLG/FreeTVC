// SPDX-License-Identifier: GPL-3.0-or-later
// MC6845 CRT controller, stepped once per character clock (1.5625 MHz, 640 ns; tvc-hardware.md §5).
#pragma once

#include <array>
#include <cstdint>

namespace freetvc {

class ChunkWriter;
class ChunkReader;

class Mc6845 {
public:
    // Snapshot fields (core/snapshot/machine_state.cpp)
    void save(ChunkWriter& w) const;
    void load(ChunkReader& r);
    static constexpr unsigned kRegisters = 18;
    static constexpr unsigned kVsyncLines = 16;  // fixed on the MC6845; R3 b7-4 are ignored

    // Signals for the character being fetched in this clock.
    struct Output {
        std::uint16_t ma = 0;  // 14-bit memory address
        std::uint8_t ra = 0;   // raster address
        bool display = false;  // display enable
        bool hsync = false;
        bool vsync = false;
        bool cursor = false;
    };

    // Registers and counters to zero (power-on); the chip's RESET pin clears only the counters.
    void power_on();
    void reset_counters();

    void select(std::uint8_t reg) { address_ = reg & 0x1F; }
    void write(std::uint8_t value);
    // Only R14-R17 are readable on the MC6845; the rest read 00h (variant ❓, tvc-hardware.md §5).
    std::uint8_t read() const;

    // Outputs of this character, then the counters advance by one character.
    Output step();

    const std::array<std::uint8_t, kRegisters>& registers() const { return regs_; }
    std::uint8_t selected() const { return address_; }
    unsigned column() const { return h_; }
    unsigned row() const { return row_; }
    unsigned raster() const { return ra_; }
    // The memory address of the next character
    std::uint16_t ma() const { return ma_; }
    std::uint16_t start_address() const { return static_cast<std::uint16_t>((regs_[12] & 0x3F) << 8 | regs_[13]); }
    std::uint16_t cursor_address() const { return static_cast<std::uint16_t>((regs_[14] & 0x3F) << 8 | regs_[15]); }

private:
    void end_of_line();
    void start_frame();
    void check_row_start();
    bool cursor_visible() const;

    std::array<std::uint8_t, kRegisters> regs_{};
    std::uint8_t address_ = 0;

    unsigned h_ = 0;
    unsigned row_ = 0;
    unsigned ra_ = 0;
    bool in_adjust_ = false;
    unsigned adjust_lines_ = 0;
    std::uint16_t ma_ = 0;
    std::uint16_t row_start_ = 0;
    std::uint16_t next_row_start_ = 0;
    bool hde_ = false;
    bool vde_ = false;
    unsigned hsync_left_ = 0;
    unsigned vsync_left_ = 0;
    unsigned field_ = 0;
};

}  // namespace freetvc
