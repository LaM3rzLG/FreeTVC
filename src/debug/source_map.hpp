// SPDX-License-Identifier: GPL-3.0-or-later
// Source lines and labels from assembler and compiler debug files (design/debug-and-remote.md "Source level").
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "debug/symbols.hpp"

namespace freetvc::debug {

struct SourceLine {
    std::uint32_t file = 0;  // index in SourceMap::files()
    std::uint32_t line = 0;  // 1-based
    bool operator==(const SourceLine&) const = default;
};

class SourceMap {
public:
    enum class Format : std::uint8_t {
        Plain,       // `label = value` and the like: SymbolTable::load_text
        Sld,         // sjasmplus --sld
        SjasmList,   // sjasmplus --lst (what YATE calls an SJASM list)
        SdccNoi,     // SDCC/sdld NoICE file (.noi)
        SdccCdb,     // SDCC debug file (.cdb)
        SdccMap,     // SDCC/sdld map (.map)
        Z88dkMap,    // z88dk/z80asm map (-m)
    };
    static Format detect(std::string_view text);
    static const char* format_name(Format format);

    struct Loaded {
        Format format = Format::Plain;
        std::size_t lines = 0;   // source line records
        std::size_t labels = 0;  // symbols added
        std::vector<std::string> errors;
    };
    // Loads a debug file's lines here and its labels into `symbols` (source = `path`). Relative source names are taken
    // from `directory`. A Plain file goes to SymbolTable::load_text.
    Loaded load(std::string_view text, std::string_view path, std::string_view directory, SymbolTable& symbols);
    // Drops the lines loaded from `path`; file names stay.
    std::size_t remove_source(std::string_view path);
    void clear();

    bool empty() const { return entries_.empty(); }
    const std::vector<std::string>& files() const { return files_; }
    // The line at an instruction: exact records, or the latest line start of a file within 256 bytes before it;
    // where several files have one, the source (C) line before a generated listing's, and the later load first.
    std::optional<SourceLine> at(std::uint16_t address) const;
    std::optional<SourceLine> at(std::uint16_t address, std::uint32_t file) const;
    // The lowest address of a line's code.
    std::optional<std::uint16_t> address_of(SourceLine line) const;
    // The lines of a file that have code, ascending.
    std::vector<std::uint32_t> code_lines(std::uint32_t file) const;

private:
    struct Entry {
        std::uint16_t address = 0;
        std::uint32_t file = 0;
        std::uint32_t line = 0;
        std::uint8_t rank = 0;  // 0 source, 1 generated listing
        bool exact = false;     // this instruction only; else until the file's next record
        std::uint32_t source = 0;
    };
    std::uint32_t file_index(std::string_view name, std::string_view directory);
    void add(std::uint16_t address, std::uint32_t file, std::uint32_t line, std::uint8_t rank, bool exact, std::uint32_t source);
    std::optional<SourceLine> lookup(std::uint16_t address, std::optional<std::uint32_t> file) const;

    std::vector<Entry> entries_;  // by address after every load
    std::vector<std::string> files_;
    std::vector<std::string> sources_;
};

}  // namespace freetvc::debug
