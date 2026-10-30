// SPDX-License-Identifier: GPL-3.0-or-later
// Machine presets and ROM files for the front ends: the only place that turns file names into a MachineConfig.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "core/machine/config.hpp"

namespace freetvc {

// ROM file names are those of data/roms.txt; the repository holds no ROMs.
struct Preset {
    std::string_view name;
    Model model;
    std::string_view sys;
    std::string_view ext;
};

// WinTVC's presets 0-2, 4 and 5 (design/machine.md "Presets"); the patched Cyrillic one is left out.
inline constexpr std::array<Preset, 5> kPresets{{
    {"32k-1.2", Model::Tvc32k, "sys12.bin", "ext1x.bin"},
    {"32k+hbm-1.2", Model::Tvc32kHbm, "sys12.bin", "ext1x.bin"},
    {"64k-1.2", Model::Tvc64k, "sys12.bin", "ext1x.bin"},
    {"64k+-2.1", Model::Tvc64kPlus, "sys21.bin", "ext2x.bin"},
    {"64k+-2.2", Model::Tvc64kPlus, "sys22.bin", "ext2x.bin"},
}};

const Preset* find_preset(std::string_view name);
std::optional<Model> parse_model(std::string_view name);

// Reads a whole file of at most max_size bytes; on failure returns nothing and sets error.
std::optional<std::vector<std::uint8_t>> read_file(const std::string& path, std::size_t max_size, std::string& error);

// An EXT ROM in its 8 KiB socket image with FFh below F000h (MAME's and the chip dump's EXT 1.x) becomes the 4 KiB
// ROM, so every source gives the same bytes and CRC-32 (snapshots and prompt states compare them).
void normalise_ext(std::vector<std::uint8_t>& ext);

// Model and SYS/EXT images of a preset, read from rom_dir.
std::optional<MachineConfig> load_preset(const Preset& preset, const std::string& rom_dir, std::string& error);

// A folder the ROM search looks in, and why (design/ui.md "Settings file and ROM search").
struct RomPlace {
    std::string why;  // "--rom-dir", "FREETVC_ROM_DIR", "freetvc.ini", "next to the program", "user data folder"
    std::filesystem::path dir;
};

// Search order, first match wins: --rom-dir, FREETVC_ROM_DIR, rom_dir in freetvc.ini, roms/ next to the program,
// roms/ in the user data folder. An empty argument leaves that place out.
std::vector<RomPlace> rom_places(const std::string& from_command_line, const std::string& from_settings,
                                 const std::filesystem::path& program_dir, const std::filesystem::path& user_dir);

// The preset from the first place that has both its ROMs: the expected file names first, else any file whose CRC-32 is a
// known image of that ROM. On failure the error names every place searched. `found` gets the place used.
std::optional<MachineConfig> find_preset_roms(const Preset& preset, const std::vector<RomPlace>& places, std::string& error,
                                              const RomPlace** found = nullptr);

// The HBF card's EPROM: hbf.rom, else a file with a known HBF ROM's CRC-32, from the first place that has one.
inline constexpr std::string_view kDiskCardRom = "hbf.rom";
// The same card with VT-DOS 1.2; hbf.rom is VT-DOS 1.1.
inline constexpr std::string_view kDiskCardRom12 = "hbf12.rom";
std::optional<std::vector<std::uint8_t>> find_disk_card_rom(const std::vector<RomPlace>& places, std::string& error);
// The card's EPROM by choice: "" or "1.1" hbf.rom, "1.2" hbf12.rom (both through the search), else a 16 KiB file.
std::optional<std::vector<std::uint8_t>> disk_card_rom(std::string_view choice, const std::vector<RomPlace>& places, std::string& error);
// The GameCard's EPROM, found the same way.
inline constexpr std::string_view kGameCardRom = "gamecard.rom";
// The Multichip sound card's EEPROM, found the same way.
inline constexpr std::string_view kMultichipRom = "multichip.rom";
// The NanoSD's EEPROM, found the same way.
inline constexpr std::string_view kNanoSdRom = "nanosd.rom";
// The MultiCart v2 firmware 2.1.14 in its three languages, for the image builder
inline constexpr std::string_view kMultiCartFirmwareEn = "multicart-en.rom";
inline constexpr std::string_view kMultiCartFirmwareHu = "multicart-hu.rom";
inline constexpr std::string_view kMultiCartFirmwareEs = "multicart-es.rom";
// The SD Dual cartridge's firmware, and its "gamer mode" build, found the same way.
inline constexpr std::string_view kSdDualRom = "sddual.rom";
inline constexpr std::string_view kSdDualGamerRom = "sddual-gm.rom";
// The TVC256k++'s ROM, found the same way.
inline constexpr std::string_view kCard256Rom = "card256.rom";
// The TVC256k++'s power-on font and picture from its firmware sources, found the same way.
inline constexpr std::string_view kCard256Font = "card256-font.bin";
inline constexpr std::string_view kCard256Picture = "card256-picture.bmp";
// The TVC256k++'s flash drive: the author's littlefs image, found the same way.
inline constexpr std::string_view kCard256Flash = "card256-flash.bin";
// The card's image: the ROM padded to 12 KiB, then the font and the picture's pixel rows; the ROM alone when neither is
// found. `notes` gets a line for each one missing.
std::vector<std::uint8_t> card256_image(std::vector<std::uint8_t> rom, const std::vector<RomPlace>& places, std::vector<std::string>& notes);
// Where the TVC256k++ goes without a slot given: slot 0 (the manual's), else the first free one; kCardSlots when full.
unsigned card256_default_slot(const std::array<CardConfig, kCardSlots>& cards);
// A card ROM `file` of at most `max_size` bytes: by name, else by a known CRC-32, from the first place that has one.
std::optional<std::vector<std::uint8_t>> find_card_rom(std::string_view file, std::size_t max_size, const std::vector<RomPlace>& places,
                                                       std::string& error);

// The running executable's folder (for headless; the GUI asks SDL).
std::filesystem::path program_directory(const char* argv0);
// The user data folder, as SDL_GetPrefPath(nullptr, "freeTVC") names it (for headless; the GUI asks SDL).
std::filesystem::path user_data_directory();

}  // namespace freetvc
