// SPDX-License-Identifier: GPL-3.0-or-later
// CAS program files (hardware/file-formats.md "CAS"): parsing as the 112-file survey requires, writing clean ones.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace freetvc {

struct CasFile {
    bool buffered = false;  // header byte 00h = 01h; 11h (or anything else, with a warning) = unbuffered
    bool protect = false;   // header byte 01h non-zero (unreliable in the wild, only reported)
    // The bytes the cassette functions serve, from offset 80h. Unbuffered: 16 program-header bytes, then the block.
    std::vector<std::uint8_t> stream;
    std::vector<std::string> warnings;  // header fields that disagree with the file

    // Unbuffered only: the data block after the 16 header bytes.
    std::span<const std::uint8_t> header() const;
    std::span<const std::uint8_t> block() const;
    // Program header byte 4 (84h): non-zero makes the ROM's LOAD run the program.
    bool autorun() const { return !buffered && stream.size() > 4 && stream[4] != 0; }
};

inline constexpr std::size_t kCasHeaderSize = 0x80;
inline constexpr std::size_t kCasProgramHeaderSize = 16;

// Never trusts the record count or byte 01h; the file size decides the length. Nothing on a file too short to hold
// its headers (`error` says why).
std::optional<CasFile> parse_cas(std::span<const std::uint8_t> bytes, std::string& error);

// A file with an exact header: type, protect byte, full records = floor(size / 128), byte 04h = size mod 128.
std::vector<std::uint8_t> build_cas(bool buffered, bool protect, std::span<const std::uint8_t> stream);

// The cassette handler's file-name conversion (EXT 2.x E547h): at most 16 bytes, a-z and the accented
// lower-case codes 90h-98h raised to upper case.
std::string tvc_file_name(std::string_view name);

}  // namespace freetvc
