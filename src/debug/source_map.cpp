// SPDX-License-Identifier: GPL-3.0-or-later
#include "debug/source_map.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>

namespace freetvc::debug {

namespace {

constexpr std::uint16_t kRangeLimit = 256;  // a line start covers at most this many bytes

std::vector<std::string_view> split_lines(std::string_view text) {
    std::vector<std::string_view> lines;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        lines.push_back(line);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
    }
    return lines;
}

std::vector<std::string_view> split(std::string_view text, char separator) {
    std::vector<std::string_view> parts;
    while (true) {
        const std::size_t at = text.find(separator);
        parts.push_back(text.substr(0, at));
        if (at == std::string_view::npos) {
            return parts;
        }
        text.remove_prefix(at + 1);
    }
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return s;
}

template <typename T>
std::optional<T> number(std::string_view s, int base) {
    T value{};
    const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), value, base);
    if (error != std::errc{} || end != s.data() + s.size() || s.empty()) {
        return std::nullopt;
    }
    return value;
}

bool starts_with_line(std::string_view text, std::string_view prefix) {
    return text.starts_with(prefix) || text.find("\n" + std::string(prefix)) != std::string_view::npos;
}

// "file:line" or a z88dk C location "file::function::level::block:line"
std::optional<std::pair<std::string_view, std::uint32_t>> location(std::string_view s) {
    const std::size_t colon = s.rfind(':');
    if (colon == std::string_view::npos || colon == 0) {
        return std::nullopt;
    }
    const auto line = number<std::uint32_t>(s.substr(colon + 1), 10);
    const std::size_t functions = s.find("::");
    const std::string_view file = s.substr(0, functions != std::string_view::npos ? functions : colon);
    if (!line || file.empty()) {
        return std::nullopt;
    }
    return std::pair{file, *line};
}

}  // namespace

SourceMap::Format SourceMap::detect(std::string_view text) {
    if (text.starts_with("|SLD.data.version|")) {
        return Format::Sld;
    }
    if (starts_with_line(text, "# file opened: ")) {
        return Format::SjasmList;
    }
    if (text.find("ASxxxx Linker") != std::string_view::npos) {
        return Format::SdccMap;
    }
    for (std::string_view line : split_lines(text)) {
        line = trim(line);
        if (line.empty()) {
            continue;
        }
        if (line.starts_with("DEF ")) {
            return Format::SdccNoi;
        }
        if (line.size() > 2 && line[1] == ':' && std::string_view("MFSLT").find(line[0]) != std::string_view::npos) {
            return Format::SdccCdb;
        }
        const std::size_t equals = line.find(" = $");
        if (equals != std::string_view::npos && line.find(" ; ", equals) != std::string_view::npos) {
            return Format::Z88dkMap;
        }
        break;
    }
    return Format::Plain;
}

const char* SourceMap::format_name(Format format) {
    switch (format) {
    case Format::Sld:
        return "sjasmplus SLD";
    case Format::SjasmList:
        return "sjasmplus listing";
    case Format::SdccNoi:
        return "SDCC NoICE";
    case Format::SdccCdb:
        return "SDCC CDB";
    case Format::SdccMap:
        return "SDCC map";
    case Format::Z88dkMap:
        return "z88dk map";
    default:
        return "labels";
    }
}

std::uint32_t SourceMap::file_index(std::string_view name, std::string_view directory) {
    std::string path(name);
    const bool absolute = name.starts_with('/') || name.starts_with('\\') || (name.size() > 1 && name[1] == ':');
    if (!absolute && !directory.empty()) {
        path = std::string(directory) + "/" + path;
    }
    const auto found = std::find(files_.begin(), files_.end(), path);
    if (found != files_.end()) {
        return static_cast<std::uint32_t>(found - files_.begin());
    }
    files_.push_back(path);
    return static_cast<std::uint32_t>(files_.size() - 1);
}

void SourceMap::add(std::uint16_t address, std::uint32_t file, std::uint32_t line, std::uint8_t rank, bool exact, std::uint32_t source) {
    entries_.push_back({address, file, line, rank, exact, source});
}

SourceMap::Loaded SourceMap::load(std::string_view text, std::string_view path, std::string_view directory, SymbolTable& symbols) {
    Loaded result;
    result.format = detect(text);
    if (result.format == Format::Plain) {
        const LoadResult plain = symbols.load_text(text, path);
        result.labels = plain.loaded;
        result.errors = plain.errors;
        return result;
    }
    const auto source = static_cast<std::uint32_t>(sources_.size());
    sources_.emplace_back(path);
    const std::size_t entries_before = entries_.size();
    const auto label = [&](std::string name, std::uint32_t value) {
        symbols.add({std::move(name), static_cast<std::uint16_t>(value & 0xFFFF), std::nullopt, std::string(path)});
        ++result.labels;
    };
    const std::vector<std::string_view> lines = split_lines(text);

    switch (result.format) {
    case Format::Sld:
        // file|line[:col]|definition file|definition line|page|value|type|data
        for (std::size_t n = 1; n < lines.size(); ++n) {
            const std::vector<std::string_view> f = split(lines[n], '|');
            if (f.size() < 8 || f[0].empty()) {
                continue;
            }
            const auto line = number<std::uint32_t>(split(f[1], ':')[0], 10);
            const auto page = number<int>(f[4], 10);
            const auto value = number<std::int64_t>(f[5], 10);
            if (!line || !page || !value) {
                result.errors.push_back("line " + std::to_string(n + 1) + ": bad fields");
                continue;
            }
            if (f[6] == "T" && *page >= 0) {
                add(static_cast<std::uint16_t>(*value & 0xFFFF), file_index(f[0], directory), *line, 0, true, source);
            } else if (f[6] == "L") {
                // module,main,local[,+traits]: module and struct markers are not addresses
                const std::vector<std::string_view> d = split(f[7], ',');
                if (f[7].find("+module") != std::string_view::npos || f[7].find("+endmod") != std::string_view::npos ||
                    f[7].find("+struct_def") != std::string_view::npos || f[7].find("+macro") != std::string_view::npos) {
                    continue;
                }
                std::string name;
                for (std::size_t i = 0; i < 3 && i < d.size(); ++i) {
                    if (!d[i].empty()) {
                        name += (name.empty() ? "" : ".") + std::string(d[i]);
                    }
                }
                if (!name.empty()) {
                    label(name, static_cast<std::uint32_t>(*value));
                }
            }
        }
        break;

    case Format::SjasmList: {
        // "  11    8005 06 03       >    ld b,3": line number (with + per include level), address, up to 4 bytes from the
        // column after the address, and the label table after "Value    Label"
        std::vector<std::uint32_t> files;
        bool table = false;
        for (const std::string_view line : lines) {
            if (line.starts_with("# file opened: ")) {
                files.push_back(file_index(line.substr(15), directory));
                continue;
            }
            if (line.starts_with("# file closed: ")) {
                if (!files.empty()) {
                    files.pop_back();
                }
                continue;
            }
            if (line.starts_with("Value    Label")) {
                table = true;
                continue;
            }
            if (table) {
                // "0x8000   start", "0x1234 X value"
                if (line.starts_with("0x")) {
                    const std::size_t end = line.find(' ');
                    const auto value = number<std::uint32_t>(line.substr(2, end == std::string_view::npos ? end : end - 2), 16);
                    const std::size_t last = line.find_last_of(' ');
                    if (value && last != std::string_view::npos && last + 1 < line.size()) {
                        label(std::string(line.substr(last + 1)), *value);
                    }
                }
                continue;
            }
            std::size_t i = 0;
            while (i < line.size() && line[i] == ' ') {
                ++i;
            }
            const std::size_t digits = i;
            while (i < line.size() && std::isdigit(static_cast<unsigned char>(line[i]))) {
                ++i;
            }
            const auto number_of_line = number<std::uint32_t>(line.substr(digits, i - digits), 10);
            while (i < line.size() && line[i] != ' ') {
                ++i;  // the include level marks
            }
            while (i < line.size() && line[i] == ' ') {
                ++i;
            }
            if (!number_of_line || files.empty() || i + 4 > line.size()) {
                continue;
            }
            const auto address = number<std::uint16_t>(line.substr(i, 4), 16);
            if (!address || (i + 4 < line.size() && line[i + 4] != ' ')) {
                continue;
            }
            if (line.size() < i + 7) {
                continue;  // no bytes after the address
            }
            if (number<unsigned>(line.substr(i + 5, 2), 16)) {
                add(*address, files.back(), *number_of_line, 0, true, source);
            }
        }
        break;
    }

    case Format::SdccNoi:
    case Format::SdccCdb:
    case Format::SdccMap: {
        // C$file$line$..: a C line; A$module$line: a line of the generated module.asm; G$name$..: a C global;
        // Fmodule$name$..: a static; other $ names (XG, L, area sizes) are not labels
        const auto sdcc = [&](std::string_view name, std::uint32_t value) {
            if (name.starts_with("s__") || name.starts_with("l__") || name.starts_with(".__")) {
                return;
            }
            if (name.find('$') == std::string_view::npos) {
                label(std::string(name), value);
                return;
            }
            const std::vector<std::string_view> p = split(name, '$');
            const std::uint32_t line = p.size() > 2 ? number<std::uint32_t>(p[2], 10).value_or(0) : 0;
            if (p[0] == "C" && line > 0) {
                add(static_cast<std::uint16_t>(value), file_index(p[1], directory), line, 0, false, source);
            } else if (p[0] == "A" && line > 0) {
                add(static_cast<std::uint16_t>(value), file_index(std::string(p[1]) + ".asm", directory), line, 1, false, source);
            } else if ((p[0] == "G" || (p[0].size() > 1 && p[0][0] == 'F')) && p.size() > 1 && !p[1].empty()) {
                label(std::string(p[1]), value);
            }
        };
        bool globals = result.format != Format::SdccMap;
        for (const std::string_view raw : lines) {
            const std::string_view line = trim(raw);
            if (result.format == Format::SdccNoi && line.starts_with("DEF ")) {
                const std::vector<std::string_view> p = split(line, ' ');
                if (p.size() >= 3 && p[2].starts_with("0x")) {
                    if (const auto value = number<std::uint32_t>(p[2].substr(2), 16)) {
                        sdcc(p[1], *value);
                    }
                }
            } else if (result.format == Format::SdccCdb && line.starts_with("L:")) {
                const std::size_t colon = line.rfind(':');
                if (const auto value = number<std::uint32_t>(line.substr(colon + 1), 16); value && colon > 2) {
                    sdcc(line.substr(2, colon - 2), *value);
                }
            } else if (result.format == Format::SdccMap) {
                // "     00008000  _twice     main" under each "Value  Global" heading
                if (line.starts_with("Value")) {
                    globals = true;
                    continue;
                }
                const std::size_t space = line.find(' ');
                if (!globals || space == std::string_view::npos) {
                    continue;
                }
                if (const auto value = number<std::uint32_t>(line.substr(0, space), 16)) {
                    const std::string_view rest = trim(line.substr(space));
                    sdcc(rest.substr(0, rest.find(' ')), *value);
                }
            }
        }
        break;
    }

    case Format::Z88dkMap:
        // "name = $8000 ; addr, local, , module, section, file:line"
        for (const std::string_view line : lines) {
            const std::size_t equals = line.find(" = $");
            const std::size_t semicolon = line.find(" ; ");
            if (equals == std::string_view::npos || semicolon == std::string_view::npos || semicolon < equals) {
                continue;
            }
            const std::string_view name = trim(line.substr(0, equals));
            const auto value = number<std::uint32_t>(trim(line.substr(equals + 4, semicolon - equals - 4)), 16);
            const std::vector<std::string_view> f = split(line.substr(semicolon + 3), ',');
            if (!value || *value > 0xFFFF || f.size() < 6) {
                continue;  // 32-bit constants are not addresses
            }
            const std::string_view section = trim(f[4]);
            if (name.starts_with("__C_LINE_") || name.starts_with("__ASM_LINE_")) {
                const bool c = name.starts_with("__C_LINE_");
                const auto where = location(trim(f[5]));
                // Lines without a section come from the preprocessor's temporary file, all at one address
                if (where && (!c || !section.empty()) && !where->first.ends_with(".i")) {
                    add(static_cast<std::uint16_t>(*value), file_index(where->first, directory), where->second, c ? 0 : 1, false, source);
                }
            } else if (!name.starts_with("__CDBINFO__")) {
                label(std::string(name), *value);
            }
        }
        break;

    default:
        break;
    }
    result.lines = entries_.size() - entries_before;
    std::stable_sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) { return a.address < b.address; });
    return result;
}

std::size_t SourceMap::remove_source(std::string_view path) {
    std::size_t removed = 0;
    for (std::uint32_t s = 0; s < sources_.size(); ++s) {
        if (sources_[s] != path) {
            continue;
        }
        const std::size_t before = entries_.size();
        std::erase_if(entries_, [&](const Entry& e) { return e.source == s; });
        removed += before - entries_.size();
        sources_[s].clear();
    }
    return removed;
}

void SourceMap::clear() {
    entries_.clear();
    files_.clear();
    sources_.clear();
}

std::optional<SourceLine> SourceMap::lookup(std::uint16_t address, std::optional<std::uint32_t> file) const {
    // Walk back from the address: the first record of each file decides that file's line here
    auto it = std::upper_bound(entries_.begin(), entries_.end(), address, [](std::uint16_t a, const Entry& e) { return a < e.address; });
    std::vector<std::uint32_t> seen;
    const Entry* best = nullptr;
    while (it != entries_.begin()) {
        --it;
        if (address - it->address >= kRangeLimit) {
            break;
        }
        if ((file && it->file != *file) || std::find(seen.begin(), seen.end(), it->file) != seen.end()) {
            continue;
        }
        seen.push_back(it->file);
        if (it->exact && it->address != address) {
            continue;
        }
        // The source (C) line before a generated listing; between loads that overlap, the later load
        if (!best || it->rank < best->rank || (it->rank == best->rank && it->source > best->source)) {
            best = &*it;
        }
    }
    if (!best) {
        return std::nullopt;
    }
    return SourceLine{best->file, best->line};
}

std::optional<SourceLine> SourceMap::at(std::uint16_t address) const { return lookup(address, std::nullopt); }

std::optional<SourceLine> SourceMap::at(std::uint16_t address, std::uint32_t file) const { return lookup(address, file); }

std::optional<std::uint16_t> SourceMap::address_of(SourceLine line) const {
    for (const Entry& e : entries_) {
        if (e.file == line.file && e.line == line.line) {
            return e.address;  // entries are by address: the first is the lowest
        }
    }
    return std::nullopt;
}

std::vector<std::uint32_t> SourceMap::code_lines(std::uint32_t file) const {
    std::vector<std::uint32_t> lines;
    for (const Entry& e : entries_) {
        if (e.file == file) {
            lines.push_back(e.line);
        }
    }
    std::sort(lines.begin(), lines.end());
    lines.erase(std::unique(lines.begin(), lines.end()), lines.end());
    return lines;
}

}  // namespace freetvc::debug
