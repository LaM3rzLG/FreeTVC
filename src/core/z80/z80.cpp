// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/z80/z80.hpp"

namespace freetvc {

Z80::Z80() : state_{} { ftvc_z80_init(state_); }

Z80::Pins Z80::reset() { return ftvc_z80_init(state_); }

Z80::Pins Z80::tick(Pins pins) { return ftvc_z80_tick(state_, pins); }

Z80::Pins Z80::prefetch(std::uint16_t pc) { return ftvc_z80_prefetch(state_, pc); }

bool Z80::instruction_done() const { return ftvc_z80_opdone(state_); }

Z80Registers Z80::registers() const {
    Z80Registers regs{};
    ftvc_z80_get_regs(state_, &regs);
    return regs;
}

void Z80::set_registers(const Z80Registers& regs) { ftvc_z80_set_regs(state_, &regs); }

Z80Internal Z80::internal() const {
    Z80Internal in{};
    ftvc_z80_get_internal(state_, &in);
    return in;
}

void Z80::set_internal(const Z80Internal& in) { ftvc_z80_set_internal(state_, &in); }

}  // namespace freetvc
