// SPDX-License-Identifier: GPL-3.0-or-later
// Symbol table: plain `label = addr`, sjasmplus --sym/--exp, built-in ROM symbols (design/debug-and-remote.md "Symbols").
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "core/bus/memory_map.hpp"

namespace freetvc {
class Machine;
}

namespace freetvc::debug {

struct Symbol {
    std::string name;
    std::uint16_t address = 0;  // CPU address as written in the file
    std::optional<BankAddress> home;  // banked symbols: where the address lives (ROM symbols appear at several addresses)
    std::string source;
    std::uint32_t generation = 0;  // the load of its source; a later load wins where names or addresses clash
};

// How a file's addresses map to a bank: CPU address `base` is offset 0; only addresses in [first, last] are taken.
struct Placement {
    Bank bank = Bank::Open;
    std::uint16_t base = 0;
    std::uint16_t first = 0;
    std::uint16_t last = 0xFFFF;
};

struct LoadResult {
    std::size_t loaded = 0;
    std::vector<std::string> errors;  // "line N: ..."
};

class SymbolTable {
public:
    // Lines `name = value`, `name EQU value` or `name: EQU value`; `;` starts a comment. Values: 0x1F, $1F, 1Fh, 31.
    LoadResult load_text(std::string_view text, std::string_view source, std::optional<Placement> placement = std::nullopt);
    void add(Symbol symbol);
    // Drops every symbol loaded from `source`.
    std::size_t remove_source(std::string_view source);
    // Drops every symbol with this exact name; false if there was none.
    bool remove_name(std::string_view name);
    void clear();

    std::size_t size() const { return symbols_.size(); }
    const std::vector<Symbol>& symbols() const { return symbols_; }

    // Exact name first (from the latest load that has it), then a unique case-insensitive match.
    const Symbol* find(std::string_view name) const;
    // Name for a CPU address that maps to `where`: a banked symbol whose home matches (preferring one written at this
    // address), else an unbanked symbol with this address, from the latest load and first in its file.
    const Symbol* at(std::uint16_t address, BankAddress where) const;
    // The same with the machine's current paging.
    const Symbol* at(std::uint16_t address, const Machine& machine) const;

private:
    void reindex();

    std::vector<Symbol> symbols_;
    std::unordered_map<std::string, std::size_t> by_name_;
    std::unordered_multimap<std::uint32_t, std::size_t> by_home_;      // bank << 16 | offset
    std::unordered_multimap<std::uint16_t, std::size_t> by_address_;  // unbanked only
    std::unordered_map<std::string, std::uint32_t> generations_;  // by source; dropped with it, so a reload is newer
    std::uint32_t next_generation_ = 0;
};

// CRC-32 (IEEE, as zlib and MAME use).
std::uint32_t crc32(std::span<const std::uint8_t> bytes);

// Recognises the SYS and EXT images by CRC-32 and loads our ROM symbol files (docs/hardware/rom-symbols/) for them;
// returns the names of the files loaded.
std::vector<std::string> load_rom_symbols(SymbolTable& table, const Machine& machine);

}  // namespace freetvc::debug
