// SPDX-License-Identifier: GPL-3.0-or-later
// Z80 disassembly text in sjasmplus syntax, with the TVC's inline-data calls (design/debug-and-remote.md "Disassembler").
#pragma once

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <vector>

#include "debug/symbols.hpp"
#include "debug/z80_decode.hpp"

namespace freetvc {
class Machine;
}

namespace freetvc::debug {

struct DisasmOptions {
    // RST 30h and RST 08h take the next byte as data, RST 18h its BASIC ops, a CALL to `string_calls` a
    // length-prefixed string (rom.md §2, §4, §6)
    bool tvc_inline = true;
    std::function<std::string(std::uint16_t)> label;  // name for an address operand, empty if none
    std::vector<std::uint16_t> string_calls;
};

struct Disassembly {
    std::uint8_t length = 1;  // instruction plus inline data
    std::string text;         // upper case; sjasmplus assembles it back to the same bytes unless `alias`
    std::string comment;      // system call names, notes on undocumented forms
    Decoded decoded;
    bool undocumented = false;
    bool alias = false;  // a second encoding of an instruction: the text assembles to the usual bytes
};

// `bytes` starts at the instruction (4 bytes are enough; missing ones count as 00h).
Disassembly disassemble(std::span<const std::uint8_t> bytes, std::uint16_t address, const DisasmOptions& options = {});

// Reads the bytes at `address` through the machine's current paging.
Disassembly disassemble(const Machine& machine, std::uint16_t address, const DisasmOptions& options = {});

// Labels from the table under the machine's current paging.
std::function<std::string(std::uint16_t)> symbol_labels(const SymbolTable& table, const Machine& machine);
// Those labels, and the ROM's inline-print routine (the symbol print_inline) as a string call.
DisasmOptions tvc_disasm_options(const SymbolTable& table, const Machine& machine);

// Name of a system call function code (rom.md §5), or nullptr.
const char* system_call_name(std::uint8_t code);

// "1Fh", "0FFh", "0C000h": the TVC manuals' hex style, with a leading 0 before a letter.
std::string hex8(unsigned value);
std::string hex16(unsigned value);

}  // namespace freetvc::debug
