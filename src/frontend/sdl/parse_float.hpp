// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <locale>
#include <sstream>
#include <string>
#include <string_view>

namespace freetvc::sdl {

// The whole of `s` as a float in the C locale (std::from_chars for float needs macOS 26)
inline bool parse_float(std::string_view s, float& value) {
    std::istringstream in{std::string(s)};
    in.imbue(std::locale::classic());
    in >> std::noskipws >> value;
    return !s.empty() && !in.fail() && in.peek() == std::char_traits<char>::eof();
}

}  // namespace freetvc::sdl
