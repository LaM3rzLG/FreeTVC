// SPDX-License-Identifier: GPL-3.0-or-later
// docs/hardware/rom-symbols/*.sym compiled in at build time (cmake/EmbedText.cmake).
#pragma once

#include <string_view>

namespace freetvc::debug {

// Text of the file with this base name (e.g. "tvc-sys22.sym"); empty if unknown.
std::string_view rom_symbol_file(std::string_view name);

}  // namespace freetvc::debug
