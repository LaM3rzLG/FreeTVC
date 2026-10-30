// SPDX-License-Identifier: GPL-3.0-or-later
// The ROM autofetch inside the program (design/build.md "ROM autofetch"): data/roms.txt compiled in, the system's
// curl as the downloader, every file checked against its pinned SHA-256.
#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace freetvc {

struct RomListPiece {
    std::string name;
    std::string path;  // on the site, or a whole https:// URL
    std::string sha256;
};

struct RomListImage {
    std::string file;
    std::vector<std::string> pieces;  // in socket order
    std::string sha256;
    std::string crc32;
    std::string what;
};

struct RomList {
    std::string site;
    std::vector<RomListPiece> pieces;
    std::vector<RomListImage> images;
    std::string notice;  // the first lines of ROMS-README.txt
};

RomList parse_rom_list(std::string_view text);
// data/roms.txt as built in
const RomList& builtin_rom_list();

// Lower-case hex SHA-256.
std::string sha256_hex(std::span<const std::uint8_t> bytes);

struct CurlCheck {
    std::filesystem::path program;  // empty when not found
    std::string version;            // the first line of `curl --version`
    std::string problem;            // why it cannot be used, with what to do
    bool ok() const { return !program.empty() && problem.empty(); }
};

// Looks for curl on PATH (and in the Windows system folder) and runs `curl --version`.
CurlCheck find_curl();

// Downloads the list's pieces with curl, checks them, and writes the images and ROMS-README.txt into `dir`; files
// already there with the right SHA-256 are kept. `progress` hears each step. False with `error` set.
bool fetch_roms(const CurlCheck& curl, const std::filesystem::path& dir, std::string& error,
                const std::function<void(const std::string&)>& progress = {});

}  // namespace freetvc
