// SPDX-License-Identifier: GPL-3.0-or-later
// data/roms.txt compiled in at build time (cmake/EmbedText.cmake).
#pragma once

#include <string_view>

namespace freetvc {

// Text of the file with this base name ("roms.txt"); empty if unknown.
std::string_view rom_list_text(std::string_view name);

}  // namespace freetvc
