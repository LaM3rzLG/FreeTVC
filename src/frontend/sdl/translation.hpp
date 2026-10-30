// SPDX-License-Identifier: GPL-3.0-or-later
// Translation catalogs: gettext .po files read without libintl (design/ui.md "Language").
#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace freetvc::sdl {

struct TextHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept { return std::hash<std::string_view>{}(s); }
};

struct Catalog {
    std::string code;  // the header's Language, such as "hu"
    std::string name;  // X-Language-Name, the language's own name, such as "Magyar"
    // msgid (with "context\x04" in front when it has a msgctxt) to msgstr; untranslated and fuzzy entries are left out
    std::unordered_map<std::string, std::string, TextHash, std::equal_to<>> entries;
};

// Malformed lines are reported in `errors` as "line N: ..." and skipped.
Catalog parse_po(std::string_view text, std::vector<std::string>& errors);

struct LanguageFile {
    std::string code;
    std::string name;
    std::filesystem::path path;
};

// The .po files in a folder, sorted by code; files without a Language header are skipped.
std::vector<LanguageFile> list_languages(const std::filesystem::path& dir);

// The first of the OS's preferred languages ("hu", "en", ...) that is English or has a catalog; English if none.
std::string pick_language(const std::vector<std::string>& preferred, const std::vector<LanguageFile>& available);

}  // namespace freetvc::sdl
