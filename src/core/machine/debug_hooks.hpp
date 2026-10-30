// SPDX-License-Identifier: GPL-3.0-or-later
// What the machine tells an attached debugger (design/debug-and-remote.md "Debug core").
#pragma once

#include <array>
#include <cstdint>

namespace freetvc {

class Machine;

// Access kinds a checkpoint can watch; also the bits of the flag maps below.
enum Access : std::uint8_t {
    kAccessExec = 1,
    kAccessRead = 2,  // memory reads other than opcode fetches
    kAccessWrite = 4,
    kAccessIn = 8,
    kAccessOut = 16,
    kAccessIrq = 32,   // the first instruction of an interrupt handler (after INTA)
    kAccessLine = 64,  // the beam starts a framebuffer row; the address is the row
};

// The machine calls these only where a flag is set, so with no flags the cost is one test per bus cycle.
class DebugHooks {
public:
    std::array<std::uint8_t, 0x10000> memory_flags{};  // kAccessExec/Read/Write per CPU address
    std::array<std::uint8_t, 0x100> port_flags{};      // kAccessIn/Out per port (A0-A7)
    std::array<std::uint8_t, 512> line_flags{};        // non-zero: report kAccessLine for this row
    bool every_instruction = false;                    // call on_instruction at every boundary
    bool watch_interrupts = false;                     // call on_instruction after every interrupt acknowledge
    bool watch_lines = false;                          // look at line_flags at every new row

    DebugHooks() = default;
    DebugHooks(const DebugHooks&) = delete;
    DebugHooks& operator=(const DebugHooks&) = delete;
    virtual ~DebugHooks() = default;

    // Before the instruction at pc (not while halted); true stops there. `restart`: the first fetch after jump().
    virtual bool on_instruction(const Machine& machine, std::uint16_t pc, bool restart) = 0;
    // After a flagged access; true stops at the next instruction boundary.
    virtual bool on_access(const Machine& machine, Access kind, std::uint16_t address, std::uint8_t value) = 0;
};

}  // namespace freetvc
