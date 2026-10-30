// SPDX-License-Identifier: GPL-3.0-or-later
// Z80 instruction length and control flow from the opcode bytes (Zilog tables, x/y/z opcode fields).
#pragma once

#include <cstdint>
#include <span>

namespace freetvc::debug {

enum class Flow : std::uint8_t {
    Next,    // always continues with the next instruction
    Jump,    // JP, JR, JP (HL/IX/IY)
    Branch,  // JP cc, JR cc, DJNZ
    Call,    // CALL, CALL cc, RST
    Return,  // RET, RET cc, RETI, RETN
    Repeat,  // LDIR, CPIR, INIR, OTIR and their decrementing forms
    Halt,
};

struct Decoded {
    std::uint8_t length = 1;
    Flow flow = Flow::Next;
    bool conditional = false;  // JP cc, JR cc, DJNZ, CALL cc, RET cc
};

// `bytes` starts at the opcode; missing bytes count as 00h. A DD/FD before DD, FD or ED joins the next instruction, as in z80.h.
Decoded decode(std::span<const std::uint8_t> bytes);

}  // namespace freetvc::debug
