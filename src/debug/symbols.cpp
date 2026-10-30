// SPDX-License-Identifier: GPL-3.0-or-later
#include "debug/symbols.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>

#include "core/machine/machine.hpp"
#include "core/snapshot/snapshot.hpp"
#include "debug/rom_symbol_files.hpp"

namespace freetvc::debug {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

bool name_start(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '@' || c == '?'; }

bool name_char(char c) { return name_start(c) || std::isdigit(static_cast<unsigned char>(c)) || c == '!'; }

bool iequal(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
               return std::toupper(static_cast<unsigned char>(x)) == std::toupper(static_cast<unsigned char>(y));
           });
}

std::optional<std::uint32_t> parse_value(std::string_view s) {
    int base = 10;
    if (s.size() > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        s.remove_prefix(2);
        base = 16;
    } else if (s.size() > 1 && s[0] == '$') {
        s.remove_prefix(1);
        base = 16;
    } else if (s.size() > 1 && (s.back() == 'h' || s.back() == 'H')) {
        s.remove_suffix(1);
        base = 16;
    }
    std::uint32_t value = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), value, base);
    if (ec != std::errc{} || end != s.data() + s.size()) {
        return std::nullopt;
    }
    return value;
}

std::uint32_t home_key(BankAddress where) { return static_cast<std::uint32_t>(where.bank) << 16 | where.offset; }

}  // namespace

LoadResult SymbolTable::load_text(std::string_view text, std::string_view source, std::optional<Placement> placement) {
    LoadResult result;
    std::size_t line_no = 0;
    while (!text.empty()) {
        const std::size_t eol = text.find('\n');
        std::string_view line = text.substr(0, eol);
        text = eol == std::string_view::npos ? std::string_view{} : text.substr(eol + 1);
        ++line_no;
        line = trim(line.substr(0, line.find(';')));
        if (line.empty()) {
            continue;
        }
        const auto fail = [&](const char* why) {
            result.errors.push_back("line " + std::to_string(line_no) + ": " + why);
        };
        if (!name_start(line.front())) {
            fail("expected a name");
            continue;
        }
        std::size_t n = 1;
        while (n < line.size() && name_char(line[n])) {
            ++n;
        }
        const std::string_view name = line.substr(0, n);
        std::string_view rest = trim(line.substr(n));
        if (!rest.empty() && rest.front() == ':') {
            rest = trim(rest.substr(1));
        }
        if (!rest.empty() && rest.front() == '=') {
            rest = trim(rest.substr(1));
        } else if (rest.size() > 3 && iequal(rest.substr(0, 3), "EQU") && std::isspace(static_cast<unsigned char>(rest[3]))) {
            rest = trim(rest.substr(3));
        } else {
            fail("expected '=' or EQU");
            continue;
        }
        const auto value = parse_value(rest);
        if (!value) {
            fail("bad value");
            continue;
        }
        if (*value > 0xFFFF) {
            continue;  // sjasmplus also lists constants that are no addresses
        }
        Symbol symbol{std::string(name), static_cast<std::uint16_t>(*value), std::nullopt, std::string(source)};
        if (placement) {
            if (symbol.address < placement->first || symbol.address > placement->last) {
                continue;
            }
            symbol.home = BankAddress{placement->bank, static_cast<std::uint16_t>(symbol.address - placement->base)};
        }
        add(std::move(symbol));
        ++result.loaded;
    }
    return result;
}

void SymbolTable::add(Symbol symbol) {
    if (symbol.generation == 0) {
        const auto [g, inserted] = generations_.try_emplace(symbol.source, 0);
        if (inserted) {
            g->second = ++next_generation_;
        }
        symbol.generation = g->second;
    }
    symbols_.push_back(std::move(symbol));
    const std::size_t i = symbols_.size() - 1;
    const Symbol& s = symbols_[i];
    if (const auto [named, inserted] = by_name_.emplace(s.name, i); !inserted && symbols_[named->second].generation < s.generation) {
        named->second = i;
    }
    if (s.home) {
        by_home_.emplace(home_key(*s.home), i);
    } else {
        by_address_.emplace(s.address, i);
    }
}

std::size_t SymbolTable::remove_source(std::string_view source) {
    const std::size_t before = symbols_.size();
    std::erase_if(symbols_, [&](const Symbol& s) { return s.source == source; });
    generations_.erase(std::string(source));
    reindex();
    return before - symbols_.size();
}

bool SymbolTable::remove_name(std::string_view name) {
    const std::size_t before = symbols_.size();
    std::erase_if(symbols_, [&](const Symbol& s) { return s.name == name; });
    reindex();
    return symbols_.size() != before;
}

void SymbolTable::clear() {
    symbols_.clear();
    generations_.clear();
    reindex();
}

void SymbolTable::reindex() {
    by_name_.clear();
    by_home_.clear();
    by_address_.clear();
    std::vector<Symbol> all = std::move(symbols_);
    symbols_.clear();
    for (Symbol& s : all) {
        add(std::move(s));
    }
}

const Symbol* SymbolTable::find(std::string_view name) const {
    if (const auto it = by_name_.find(std::string(name)); it != by_name_.end()) {
        return &symbols_[it->second];
    }
    const Symbol* match = nullptr;
    for (const Symbol& s : symbols_) {
        if (iequal(s.name, name)) {
            if (match && match->name != s.name) {
                return nullptr;  // ambiguous
            }
            match = match ? match : &s;
        }
    }
    return match;
}

const Symbol* SymbolTable::at(std::uint16_t address, BankAddress where) const {
    const Symbol* banked = nullptr;
    const auto [first, last] = by_home_.equal_range(home_key(where));
    for (auto it = first; it != last; ++it) {
        const Symbol& s = symbols_[it->second];
        if (s.address == address) {
            return &s;
        }
        if (!banked || it->second < static_cast<std::size_t>(banked - symbols_.data())) {
            banked = &s;
        }
    }
    if (banked) {
        return banked;
    }
    const Symbol* plain = nullptr;
    const auto [pfirst, plast] = by_address_.equal_range(address);
    for (auto it = pfirst; it != plast; ++it) {
        const Symbol& s = symbols_[it->second];
        if (!plain || s.generation > plain->generation ||
            (s.generation == plain->generation && it->second < static_cast<std::size_t>(plain - symbols_.data()))) {
            plain = &s;
        }
    }
    return plain;
}

const Symbol* SymbolTable::at(std::uint16_t address, const Machine& machine) const {
    return at(address, machine.memory().locate(address));
}

std::uint32_t crc32(std::span<const std::uint8_t> bytes) { return freetvc::crc32(bytes); }

std::vector<std::string> load_rom_symbols(SymbolTable& table, const Machine& machine) {
    struct File {
        const char* name;
        Placement placement;
    };
    struct Rom {
        std::uint32_t crc;
        std::vector<File> files;
    };
    // CRC-32 of the ROM images (the MAME CRCs, rom.md §1)
    static const std::array<Rom, 5> kRoms{{
        {0x79FA818C, {{"tvc-sys22.sym", {Bank::Sys, 0xC000, 0xC000, 0xFFFF}}, {"tvc-sys22-page0.sym", {Bank::Sys, 0, 0, 0x3FFF}}}},
        // SYS 2.1: only C0E8-CF01 and the boot code 021C-034F equal 2.2 (rom.md §1, tvc-sys22-page0.sym)
        {0x53C8FF98, {{"tvc-sys22.sym", {Bank::Sys, 0xC000, 0xC0E8, 0xCF01}}, {"tvc-sys22-page0.sym", {Bank::Sys, 0, 0x021C, 0x034F}}}},
        {0x6DA8809A, {{"tvc-sys12.sym", {Bank::Sys, 0xC000, 0xC000, 0xFFFF}}, {"tvc-sys12-page0.sym", {Bank::Sys, 0, 0, 0x3FFF}}}},
        {0x05E1C3A8, {{"tvc-ext2x.sym", {Bank::Ext, 0xE000, 0xE000, 0xFFFF}}}},
        {0x59C6B9F9, {{"tvc-ext1x.sym", {Bank::Ext, 0xE000, 0xF000, 0xFFFF}}}},
    }};
    const std::span<const std::uint8_t> sys = machine.memory().bank_bytes(Bank::Sys);
    const std::span<const std::uint8_t> ext = machine.memory().bank_bytes(Bank::Ext);
    const std::array<std::uint32_t, 3> crcs{crc32(sys), crc32(ext), crc32(ext.subspan(0x1000))};

    std::vector<std::string> loaded;
    bool known_sys = false;
    for (const Rom& rom : kRoms) {
        if (std::find(crcs.begin(), crcs.end(), rom.crc) == crcs.end()) {
            continue;
        }
        known_sys |= rom.crc == crcs[0];
        for (const File& file : rom.files) {
            const std::string source = std::string("rom:") + file.name;
            table.load_text(rom_symbol_file(file.name), source, file.placement);
            loaded.push_back(file.name);
        }
    }
    if (known_sys) {
        table.load_text(rom_symbol_file("tvc-ram.sym"), "rom:tvc-ram.sym", Placement{Bank::U0, 0, 0, 0x3FFF});
        loaded.push_back("tvc-ram.sym");
    }
    return loaded;
}

}  // namespace freetvc::debug
