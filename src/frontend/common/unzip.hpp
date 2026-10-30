// SPDX-License-Identifier: GPL-3.0-or-later
// One member of a ZIP archive, for the ROM autofetch (the TVC256k++'s flash image comes zipped).
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace freetvc {

// Raw DEFLATE data (RFC 1951) inflated; nothing when it is broken
std::optional<std::vector<std::uint8_t>> inflate(std::span<const std::uint8_t> data);

// The member `name` of a ZIP archive, stored or deflated, found through its central directory and checked by its CRC-32;
// nothing when it is missing or broken
std::optional<std::vector<std::uint8_t>> unzip_member(std::span<const std::uint8_t> zip, std::string_view name);

}  // namespace freetvc
