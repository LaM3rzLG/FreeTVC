// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/sdl/translation.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>
#include <optional>
#include <system_error>

namespace freetvc::sdl {

namespace {

std::string_view trim(std::string_view s) {
    const auto first = s.find_first_not_of(" \t\r");
    if (first == std::string_view::npos) {
        return {};
    }
    return s.substr(first, s.find_last_not_of(" \t\r") - first + 1);
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// A C string literal's bytes; nullopt when it is not one
std::optional<std::string> unquote(std::string_view s) {
    if (s.size() < 2 || s.front() != '"' || s.back() != '"') {
        return std::nullopt;
    }
    s = s.substr(1, s.size() - 2);
    std::string out;
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '"') {
            return std::nullopt;
        }
        if (c != '\\') {
            out += c;
            continue;
        }
        if (++i == s.size()) {
            return std::nullopt;
        }
        switch (const char e = s[i]) {
        case 'n': out += '\n'; break;
        case 't': out += '\t'; break;
        case 'r': out += '\r'; break;
        case 'a': out += '\a'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'v': out += '\v'; break;
        case '"':
        case '\\':
        case '\'':
        case '?': out += e; break;
        case 'x': {
            int value = 0;
            std::size_t digits = 0;
            while (i + 1 < s.size() && hex_digit(s[i + 1]) >= 0 && digits < 2) {
                value = value * 16 + hex_digit(s[++i]);
                ++digits;
            }
            if (digits == 0) {
                return std::nullopt;
            }
            out += static_cast<char>(value);
            break;
        }
        default:
            if (e < '0' || e > '7') {
                return std::nullopt;
            }
            int value = e - '0';
            for (int n = 0; n < 2 && i + 1 < s.size() && s[i + 1] >= '0' && s[i + 1] <= '7'; ++n) {
                value = value * 8 + (s[++i] - '0');
            }
            out += static_cast<char>(value);
        }
    }
    return out;
}

void read_header(std::string_view header, Catalog& catalog, std::vector<std::string>& errors) {
    while (!header.empty()) {
        const auto newline = header.find('\n');
        const std::string_view line = header.substr(0, newline);
        header = newline == std::string_view::npos ? std::string_view{} : header.substr(newline + 1);
        const auto colon = line.find(':');
        if (colon == std::string_view::npos) {
            continue;
        }
        const std::string_view key = trim(line.substr(0, colon));
        const std::string_view value = trim(line.substr(colon + 1));
        if (key == "Language") {
            catalog.code = value;
        } else if (key == "X-Language-Name") {
            catalog.name = value;
        } else if (key == "Content-Type") {
            const auto charset = value.find("charset=");
            if (charset != std::string_view::npos) {
                std::string name(trim(value.substr(charset + 8)));
                std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
                if (name != "UTF-8" && name != "CHARSET") {
                    errors.push_back("header: the charset is " + name + ", not UTF-8");
                }
            }
        }
    }
}

}  // namespace

Catalog parse_po(std::string_view text, std::vector<std::string>& errors) {
    Catalog catalog;
    struct Entry {
        std::optional<std::string> context, id, str;
        bool plural = false;
        bool fuzzy = false;
    } entry;
    std::string* field = nullptr;
    const auto finish = [&] {
        if (entry.id) {
            if (entry.id->empty() && !entry.context) {
                read_header(entry.str.value_or(""), catalog, errors);
            } else if (!entry.fuzzy && !entry.plural && entry.str && !entry.str->empty()) {
                catalog.entries[entry.context ? *entry.context + '\x04' + *entry.id : *entry.id] = *entry.str;
            }
        }
        entry = Entry{};
        field = nullptr;
    };
    int number = 0;
    while (!text.empty()) {
        const auto newline = text.find('\n');
        const std::string_view line = trim(text.substr(0, newline));
        text = newline == std::string_view::npos ? std::string_view{} : text.substr(newline + 1);
        ++number;
        const auto fail = [&](const std::string& why) { errors.push_back("line " + std::to_string(number) + ": " + why); };
        if (line.empty()) {
            finish();
            continue;
        }
        if (line.front() == '#') {
            if (line.starts_with("#~")) {
                continue;  // an obsolete entry
            }
            if (entry.str) {
                finish();
            }
            if (line.starts_with("#,") && line.find("fuzzy") != std::string_view::npos) {
                entry.fuzzy = true;
            }
            continue;
        }
        if (line.front() == '"') {
            const auto part = unquote(line);
            if (!part) {
                fail("not a string");
            } else if (!field) {
                fail("a string without a keyword");
            } else {
                *field += *part;
            }
            continue;
        }
        const auto space = line.find_first_of(" \t");
        const std::string_view keyword = line.substr(0, space);
        const auto value = space == std::string_view::npos ? std::nullopt : unquote(trim(line.substr(space)));
        if (!value) {
            fail("expected a keyword and a string");
            field = nullptr;
            continue;
        }
        if ((keyword == "msgctxt" || keyword == "msgid") && entry.str) {
            finish();
        }
        if (keyword == "msgctxt") {
            field = &entry.context.emplace(*value);
        } else if (keyword == "msgid") {
            field = &entry.id.emplace(*value);
        } else if (keyword == "msgid_plural") {
            entry.plural = true;
            field = nullptr;
        } else if (keyword == "msgstr") {
            field = &entry.str.emplace(*value);
        } else if (keyword.starts_with("msgstr[")) {
            entry.plural = true;
            entry.str.emplace();
            field = nullptr;
        } else {
            fail("unknown keyword '" + std::string(keyword) + "'");
            field = nullptr;
        }
    }
    finish();
    return catalog;
}

std::vector<LanguageFile> list_languages(const std::filesystem::path& dir) {
    std::vector<LanguageFile> found;
    std::error_code ec;
    for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".po" || !it->is_regular_file(ec)) {
            continue;
        }
        std::ifstream in(it->path(), std::ios::binary);
        const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
        std::vector<std::string> errors;
        const Catalog catalog = parse_po(text, errors);
        if (!catalog.code.empty()) {
            found.push_back({catalog.code, catalog.name.empty() ? catalog.code : catalog.name, it->path()});
        }
    }
    std::sort(found.begin(), found.end(), [](const LanguageFile& a, const LanguageFile& b) { return a.code < b.code; });
    return found;
}

std::string pick_language(const std::vector<std::string>& preferred, const std::vector<LanguageFile>& available) {
    for (const std::string& locale : preferred) {
        std::string code = locale.substr(0, locale.find_first_of("_-."));
        std::transform(code.begin(), code.end(), code.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (code == "en") {
            return code;
        }
        for (const LanguageFile& file : available) {
            if (file.code == code) {
                return code;
            }
        }
    }
    return "en";
}

}  // namespace freetvc::sdl
