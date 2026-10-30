// SPDX-License-Identifier: GPL-3.0-or-later
#include "debug/z80_decode.hpp"

namespace freetvc::debug {

namespace {

Decoded plain(std::uint8_t op) {
    const unsigned x = op >> 6;
    const unsigned y = (op >> 3) & 7;
    const unsigned z = op & 7;
    const unsigned p = y >> 1;
    const unsigned q = y & 1;

    switch (x) {
    case 0:
        switch (z) {
        case 0:
            if (y == 2) {
                return {2, Flow::Branch, true};  // DJNZ
            }
            if (y == 3) {
                return {2, Flow::Jump, false};  // JR e
            }
            if (y >= 4) {
                return {2, Flow::Branch, true};  // JR cc,e
            }
            return {1, Flow::Next, false};  // NOP, EX AF,AF'
        case 1:
            return {static_cast<std::uint8_t>(q == 0 ? 3 : 1), Flow::Next, false};  // LD rp,nn / ADD HL,rp
        case 2:
            return {static_cast<std::uint8_t>(p >= 2 ? 3 : 1), Flow::Next, false};  // LD (nn),HL/A, LD HL/A,(nn)
        case 6:
            return {2, Flow::Next, false};  // LD r,n
        default:
            return {1, Flow::Next, false};
        }
    case 1:
        return {1, op == 0x76 ? Flow::Halt : Flow::Next, false};
    case 2:
        return {1, Flow::Next, false};
    default:
        switch (z) {
        case 0:
            return {1, Flow::Return, true};  // RET cc
        case 1:
            if (q == 1 && p == 0) {
                return {1, Flow::Return, false};  // RET
            }
            if (q == 1 && p == 2) {
                return {1, Flow::Jump, false};  // JP (HL)
            }
            return {1, Flow::Next, false};  // POP, EXX, LD SP,HL
        case 2:
            return {3, Flow::Branch, true};  // JP cc,nn
        case 3:
            if (y == 0) {
                return {3, Flow::Jump, false};  // JP nn
            }
            if (y == 2 || y == 3) {
                return {2, Flow::Next, false};  // OUT (n),A / IN A,(n)
            }
            return {1, Flow::Next, false};
        case 4:
            return {3, Flow::Call, true};  // CALL cc,nn
        case 5:
            if (q == 1 && p == 0) {
                return {3, Flow::Call, false};  // CALL nn
            }
            return {1, Flow::Next, false};  // PUSH (the prefixes are handled by decode())
        case 6:
            return {2, Flow::Next, false};  // ALU A,n
        default:
            return {1, Flow::Call, false};  // RST
        }
    }
}

Decoded extended(std::uint8_t op) {
    const unsigned x = op >> 6;
    const unsigned y = (op >> 3) & 7;
    const unsigned z = op & 7;
    if (x == 1 && z == 3) {
        return {4, Flow::Next, false};  // LD (nn),rp / LD rp,(nn)
    }
    if (x == 1 && z == 5) {
        return {2, Flow::Return, false};  // RETN, RETI
    }
    if (x == 2 && z <= 3 && y >= 6) {
        return {2, Flow::Repeat, false};
    }
    return {2, Flow::Next, false};  // undefined ED xx are 2-byte NOPs
}

// Instructions whose (HL) operand becomes (IX+d) / (IY+d) after DD / FD
bool takes_displacement(std::uint8_t op) {
    const unsigned x = op >> 6;
    const unsigned y = (op >> 3) & 7;
    const unsigned z = op & 7;
    switch (x) {
    case 0:
        return op == 0x34 || op == 0x35 || op == 0x36;
    case 1:
        return op != 0x76 && (z == 6 || y == 6);
    case 2:
        return z == 6;
    default:
        return false;
    }
}

}  // namespace

Decoded decode(std::span<const std::uint8_t> bytes) {
    const auto at = [&](std::size_t i) -> std::uint8_t { return i < bytes.size() ? bytes[i] : 0; };
    const std::uint8_t op = at(0);
    switch (op) {
    case 0xCB:
        return {2, Flow::Next, false};
    case 0xED:
        return extended(at(1));
    case 0xDD:
    case 0xFD: {
        const std::uint8_t next = at(1);
        if (next == 0xDD || next == 0xFD || next == 0xED) {
            Decoded d = decode(bytes.size() > 1 ? bytes.subspan(1) : std::span<const std::uint8_t>{});
            d.length = static_cast<std::uint8_t>(d.length + 1);
            return d;
        }
        if (next == 0xCB) {
            return {4, Flow::Next, false};  // DD CB d op
        }
        Decoded d = plain(next);
        d.length = static_cast<std::uint8_t>(d.length + 1 + (takes_displacement(next) ? 1 : 0));
        return d;
    }
    default:
        return plain(op);
    }
}

}  // namespace freetvc::debug
