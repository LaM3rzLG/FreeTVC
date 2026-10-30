// SPDX-License-Identifier: GPL-3.0-or-later
// PNG of a framebuffer area, for the monitor and MCP screenshots: palette indexes, compressed with our own deflate.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/video/framebuffer.hpp"

namespace freetvc::remote {

// Indexed PNG of `area`, one pixel per framebuffer pixel, the palette from kRgbPalette (blanking is index 16).
std::vector<std::uint8_t> encode_png(const Framebuffer& frame, ScreenArea area);

bool write_png(const std::string& path, const Framebuffer& frame, ScreenArea area, std::string& error);

}  // namespace freetvc::remote
