// SPDX-License-Identifier: GPL-3.0-or-later
// C++ adapter over floooh z80.h: nothing outside core/z80 depends on its struct layout.
#pragma once

#include <cstdint>

#include "core/z80/z80_bridge.h"

namespace freetvc {

using Z80Registers = ftvc_z80_regs;
using Z80Internal = ftvc_z80_internal;

class Z80 {
public:
    using Pins = std::uint64_t;

    static constexpr Pins kM1 = 1ULL << FTVC_Z80_PIN_M1;
    static constexpr Pins kMREQ = 1ULL << FTVC_Z80_PIN_MREQ;
    static constexpr Pins kIORQ = 1ULL << FTVC_Z80_PIN_IORQ;
    static constexpr Pins kRD = 1ULL << FTVC_Z80_PIN_RD;
    static constexpr Pins kWR = 1ULL << FTVC_Z80_PIN_WR;
    static constexpr Pins kHALT = 1ULL << FTVC_Z80_PIN_HALT;
    static constexpr Pins kINT = 1ULL << FTVC_Z80_PIN_INT;
    static constexpr Pins kNMI = 1ULL << FTVC_Z80_PIN_NMI;
    static constexpr Pins kRFSH = 1ULL << FTVC_Z80_PIN_RFSH;

    static constexpr std::uint16_t address(Pins pins) { return static_cast<std::uint16_t>(pins); }
    static constexpr std::uint8_t data(Pins pins) { return static_cast<std::uint8_t>(pins >> FTVC_Z80_PIN_D0); }
    static constexpr Pins with_data(Pins pins, std::uint8_t value) {
        return (pins & ~(Pins{0xFF} << FTVC_Z80_PIN_D0)) | (Pins{value} << FTVC_Z80_PIN_D0);
    }

    Z80();

    // Power-on state ("The Undocumented Z80 Documented"); returns the pins for the fetch at 0000h.
    Pins reset();
    // One T-state; the caller answers the returned bus request before the next tick.
    Pins tick(Pins pins);
    // Restart with an opcode fetch at pc.
    Pins prefetch(std::uint16_t pc);
    // True in the tick that holds T1 of the next opcode fetch (the previous instruction is complete).
    bool instruction_done() const;

    // pc is z80.h's internal PC: at an instruction boundary it already points past the opcode being fetched.
    Z80Registers registers() const;
    // Writes everything except pc; use prefetch() to move execution.
    void set_registers(const Z80Registers& regs);
    // Decoder state and pc: with the registers, the whole CPU (snapshots).
    Z80Internal internal() const;
    void set_internal(const Z80Internal& in);

private:
    alignas(FTVC_Z80_STATE_ALIGN) unsigned char state_[FTVC_Z80_STATE_SIZE];
};

}  // namespace freetvc
