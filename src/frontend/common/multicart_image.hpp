// SPDX-License-Identifier: GPL-3.0-or-later
// MultiCart v2 flash images (design/cards.md "MultiCart v2"): chip 0 then chip 1, 512 KiB each. The layout is the
// one Vass Sándor's ROM builder (MC21RM, software/rom-builder/unit1.pas) writes and the firmware 2.1.x reads.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace freetvc {

struct RomPlace;

struct MultiCartEntry {
    enum Type : std::uint8_t { Basic = 1, Cart = 2, Cas = 3, Bin = 4, Dat = 5 };
    std::uint8_t type = 0;
    unsigned bank = 0;    // 0-63; 32 and up are on the second chip
    unsigned block = 0;   // the first 256-byte block in the bank
    unsigned blocks = 0;  // size in blocks
    std::string name;     // UTF-8, trailing spaces cut
};

struct MultiCartFile {
    std::string name;  // UTF-8; the directory keeps 16 characters
    std::vector<std::uint8_t> bytes;
    bool cas = false;  // by extension; a file starting with MOPS is a cartridge either way
};

// "BASIC", "CART", "CAS", "BIN", "DAT" or "?"
const char* multicart_type_name(std::uint8_t type);
// The firmware's header: MOPS, JR at +4, version 2.1 or later at +6, the BAM pointer at +8 inside the window
bool is_multicart_firmware(std::span<const std::uint8_t> image);
// A whole image: the firmware's header and more than 64 KiB, up to two chips
bool is_multicart_image(std::span<const std::uint8_t> image);
// The firmware's directory, without its first entry (back to BASIC); empty when the image has none
std::vector<MultiCartEntry> multicart_directory(std::span<const std::uint8_t> image);
// A full two-chip image from the firmware and files in the given order: the firmware's first 12 KiB and its first
// directory entry, the BAM, the directory; carts in whole 16 KiB banks, CAS payloads in chains of 254-byte blocks.
std::optional<std::vector<std::uint8_t>> build_multicart_image(std::span<const std::uint8_t> firmware,
                                                               const std::vector<MultiCartFile>& files, std::string& error);
// A host file as a MultiCartFile: the name is its stem; CAS by extension
std::optional<MultiCartFile> read_multicart_file(const std::string& path, std::string& error);
// build_multicart_image() into the file `out`, with the firmware multicart-<language>.rom ("en", "hu", "es") from the
// ROM search and the host files in order
bool write_multicart_image(const std::string& out, const std::vector<std::string>& files, const std::string& language,
                           const std::vector<RomPlace>& places, std::string& error);

}  // namespace freetvc
