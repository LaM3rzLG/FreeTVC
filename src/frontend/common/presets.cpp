// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/common/presets.hpp"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include "core/cards/card256.hpp"
#include "core/message.hpp"
#include "debug/symbols.hpp"

namespace freetvc {

const Preset* find_preset(std::string_view name) {
    for (const Preset& preset : kPresets) {
        if (preset.name == name) {
            return &preset;
        }
    }
    return nullptr;
}

std::optional<Model> parse_model(std::string_view name) {
    if (name == "32k") {
        return Model::Tvc32k;
    }
    if (name == "32k+hbm") {
        return Model::Tvc32kHbm;
    }
    if (name == "64k") {
        return Model::Tvc64k;
    }
    if (name == "64k+") {
        return Model::Tvc64kPlus;
    }
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> read_file(const std::string& path, std::size_t max_size, std::string& error) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = msg("cannot open '%s'", path.c_str());
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (bytes.size() > max_size) {
        error = msg("'%s' is larger than %zu bytes", path.c_str(), max_size);
        return std::nullopt;
    }
    return bytes;
}

void normalise_ext(std::vector<std::uint8_t>& ext) {
    if (ext.size() == 0x2000 && std::all_of(ext.begin(), ext.begin() + 0x1000, [](std::uint8_t b) { return b == 0xFF; })) {
        ext.erase(ext.begin(), ext.begin() + 0x1000);
    }
}

std::optional<MachineConfig> load_preset(const Preset& preset, const std::string& rom_dir, std::string& error) {
    const std::filesystem::path dir(rom_dir);
    MachineConfig config;
    config.model = preset.model;
    auto sys = read_file((dir / preset.sys).string(), 0x4000, error);
    if (!sys) {
        return std::nullopt;
    }
    auto ext = read_file((dir / preset.ext).string(), 0x2000, error);
    if (!ext) {
        return std::nullopt;
    }
    config.roms.sys = std::move(*sys);
    config.roms.ext = std::move(*ext);
    normalise_ext(config.roms.ext);
    return config;
}

namespace {

// CRC-32s of the known images of each file name (the MAME CRCs, rom.md §1)
struct KnownRom {
    std::string_view name;
    std::uint32_t crc;
};
constexpr KnownRom kKnownRoms[] = {
    {"sys12.bin", 0x6DA8809A},
    {"sys21.bin", 0x53C8FF98},
    {"sys22.bin", 0x79FA818C},
    {"ext1x.bin", 0x59C6B9F9},
    {"ext1x.bin", 0x1CBBEAC6},  // the 8 KiB socket image: FFh up to F000h, as MAME stores it
    {"ext2x.bin", 0x05E1C3A8},
    {"hbf.rom", 0xAE34982B},  // WinTVC's HBF card EPROM, VT-DOS 1.1 (the site's FLPDOS11.PGM)
    {"hbf.rom", 0x1466AED4},  // another revision
    {"hbf.rom", 0xB3A567AD},  // MAME d_tvcupm.128 (UPM)
    {"hbf12.rom", 0x2ACF8477},  // VT-DOS 1.2 (the site's FLPDOS12.PGM; MAME's d_tvcdos.128 calls it 1.1)
    {"hbf12.rom", 0xD839417A},  // the site's DOS1.FDR, VT-DOS 1.2 with 4 bytes changed
    {"vtdos11.rom", 0x40DDC741},  // the VT-DOS 1.1 cartridge (vtdosrom.zip VT-DOS11-CART.ROM)
    {"gamecard.rom", 0xF782DB9B},  // dikdom's TVC-GameCard-r1229.BIN
    {"multichip.rom", 0x704D1C27},  // dikdom's TVC-Multichip-Soundcard sndmx-fw-v1.0.1.bin
    {"nanosd.rom", 0xAF3C8EB7},  // dikdom's TVC-NanoSD bin/NanoSDROM-v0.33.bin
    {"multicart-en.rom", 0x7C275018},  // dikdom's TVC-Multicart-v2 software/firmware/multicart_v2.1.14en.bin
    {"multicart-hu.rom", 0x50B96903},  // ... multicart_v2.1.14hu.bin
    {"multicart-es.rom", 0xFFB77D45},  // ... multicart_v2.1.14es.bin
    {"sddual.rom", 0xF758C797},     // Sebestyén Pál's sd_dual_2022_11_09.zip vtdos_cr.bin
    {"sddual-gm.rom", 0x2CC250AF},  // ... sd_dual_2022_11_09_gm.zip vtdos_cr.bin
    {"card256.rom", 0xF759A66F},   // dikdom's TVC256k++ binaries/tvc-256k++.bin, firmware 0.24
    {"card256-font.bin", 0x5B6A2B88},     // its firmware's binaries/c16-hungarian.bin
    {"card256-picture.bmp", 0xEA0740CD},  // its firmware's binaries/tvc-16cimage.bmp
};

// The expected file in dir, else a file of at most max_size bytes whose CRC-32 is a known image of that name
std::optional<std::filesystem::path> find_rom(const std::filesystem::path& dir, std::string_view name, std::size_t max_size) {
    std::error_code ec;
    if (std::filesystem::is_regular_file(dir / name, ec)) {
        return dir / name;
    }
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        std::error_code size_ec;
        if (entry.is_regular_file(size_ec) && entry.file_size(size_ec) <= max_size && !size_ec) {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());  // the same file wins on every run
    for (const auto& path : files) {
        std::string error;
        const auto bytes = read_file(path.string(), max_size, error);
        if (!bytes) {
            continue;
        }
        const std::uint32_t crc = debug::crc32(*bytes);
        for (const KnownRom& known : kKnownRoms) {
            if (known.name == name && known.crc == crc) {
                return path;
            }
        }
    }
    return std::nullopt;
}

}  // namespace

std::vector<RomPlace> rom_places(const std::string& from_command_line, const std::string& from_settings,
                                 const std::filesystem::path& program_dir, const std::filesystem::path& user_dir) {
    std::vector<RomPlace> places;
    if (!from_command_line.empty()) {
        places.push_back({"--rom-dir", from_command_line});
    }
    if (const char* env = std::getenv("FREETVC_ROM_DIR"); env && *env) {
        places.push_back({"FREETVC_ROM_DIR", env});
    }
    if (!from_settings.empty()) {
        places.push_back({"freetvc.ini", from_settings});
    }
    if (!program_dir.empty()) {
        places.push_back({"next to the program", program_dir / "roms"});
    }
    if (!user_dir.empty()) {
        places.push_back({"user data folder", user_dir / "roms"});
    }
    return places;
}

std::optional<MachineConfig> find_preset_roms(const Preset& preset, const std::vector<RomPlace>& places, std::string& error,
                                              const RomPlace** found) {
    std::string searched;
    for (const RomPlace& place : places) {
        const auto sys = find_rom(place.dir, preset.sys, 0x4000);
        const auto ext = find_rom(place.dir, preset.ext, 0x2000);
        if (sys && ext) {
            MachineConfig config;
            config.model = preset.model;
            auto sys_bytes = read_file(sys->string(), 0x4000, error);
            auto ext_bytes = read_file(ext->string(), 0x2000, error);
            if (!sys_bytes || !ext_bytes) {
                return std::nullopt;
            }
            config.roms.sys = std::move(*sys_bytes);
            config.roms.ext = std::move(*ext_bytes);
            normalise_ext(config.roms.ext);
            if (found) {
                *found = &place;
            }
            return config;
        }
        searched += "\n  " + place.why + ": " + place.dir.string() + (sys ? "" : " (no " + std::string(preset.sys) + ")") +
                    (ext ? "" : " (no " + std::string(preset.ext) + ")");
    }
    const std::string none = msg("no ROMs for %s (%s and %s, or files with their CRC-32)", std::string(preset.name).c_str(), std::string(preset.sys).c_str(),
                                 std::string(preset.ext).c_str());
    error = none + "; searched:" + (searched.empty() ? std::string("\n  nowhere") : searched);
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> find_card_rom(std::string_view file, std::size_t max_size, const std::vector<RomPlace>& places,
                                                       std::string& error) {
    std::string searched;
    for (const RomPlace& place : places) {
        if (const auto path = find_rom(place.dir, file, max_size)) {
            return read_file(path->string(), max_size, error);
        }
        searched += "\n  " + place.why + ": " + place.dir.string();
    }
    error = msg("no %s (or a file with its known CRC-32; --fetch-roms downloads it)", std::string(file).c_str()) + "; searched:" +
            (searched.empty() ? std::string("\n  nowhere") : searched);
    return std::nullopt;
}

std::optional<std::vector<std::uint8_t>> find_disk_card_rom(const std::vector<RomPlace>& places, std::string& error) {
    return find_card_rom(kDiskCardRom, 0x4000, places, error);
}

std::optional<std::vector<std::uint8_t>> disk_card_rom(std::string_view choice, const std::vector<RomPlace>& places, std::string& error) {
    if (choice.empty() || choice == "1.1" || choice == "1.2") {
        return find_card_rom(choice == "1.2" ? kDiskCardRom12 : kDiskCardRom, 0x4000, places, error);
    }
    return read_file(std::string(choice), 0x4000, error);
}

std::vector<std::uint8_t> card256_image(std::vector<std::uint8_t> rom, const std::vector<RomPlace>& places, std::vector<std::string>& notes) {
    std::string error;
    const auto font = find_card_rom(kCard256Font, Card256::kFontSize, places, error);
    if (!font) {
        notes.push_back(msg("TVC256k++ without its font, so text modes stay empty until a program loads one: %s",
                            error.substr(0, error.find("; searched")).c_str()));
    }
    auto bmp = find_card_rom(kCard256Picture, Card256::kPictureHeader + Card256::kPictureSize, places, error);
    const auto le = [&](std::size_t at, unsigned n) {
        unsigned v = 0;
        for (unsigned i = 0; i < n; ++i) {
            v |= static_cast<unsigned>((*bmp)[at + i]) << (8 * i);
        }
        return v;
    };
    // The firmware takes the pixels at offset 118 of a bottom-up 256 x 240, 4-bit BMP
    if (bmp && (bmp->size() != Card256::kPictureHeader + Card256::kPictureSize || (*bmp)[0] != 'B' || (*bmp)[1] != 'M' || le(10, 4) != 118 ||
                le(18, 4) != 256 || le(22, 4) != 240 || le(28, 2) != 4)) {
        notes.push_back(msg("TVC256k++: %s is not the 256 x 240 16-colour BMP the card expects; left out", std::string(kCard256Picture).c_str()));
        bmp.reset();
    } else if (!bmp) {
        notes.push_back(msg("TVC256k++ without its power-on picture: %s", error.substr(0, error.find("; searched")).c_str()));
    }
    if (!font && !bmp) {
        return rom;
    }
    const auto rom_size = static_cast<std::ptrdiff_t>(std::min(rom.size(), Card256::kRomSize));
    rom.resize(Card256::kImageSize, 0x00);
    std::fill(rom.begin() + rom_size, rom.begin() + Card256::kRomSize, 0xFF);
    if (font) {
        std::copy_n(font->begin(), std::min(font->size(), Card256::kFontSize), rom.begin() + Card256::kRomSize);
    }
    if (bmp) {
        std::copy_n(bmp->begin() + Card256::kPictureHeader, Card256::kPictureSize, rom.begin() + Card256::kRomSize + Card256::kFontSize);
    }
    return rom;
}

unsigned card256_default_slot(const std::array<CardConfig, kCardSlots>& cards) {
    if (cards[kCard256Slot].kind == CardKind::None) {
        return kCard256Slot;
    }
    for (unsigned i = 0; i < kCardSlots; ++i) {
        if (cards[i].kind == CardKind::None) {
            return i;
        }
    }
    return kCardSlots;
}

std::filesystem::path program_directory(const char* argv0) {
    std::error_code ec;
#if defined(_WIN32)
    std::wstring buffer(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length > 0 && length < buffer.size()) {
        buffer.resize(length);
        return std::filesystem::path(buffer).parent_path();
    }
#else
    const auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
    if (!ec) {
        return self.parent_path();
    }
#endif
    if (!argv0 || !*argv0) {
        return {};
    }
    const auto path = std::filesystem::absolute(argv0, ec);
    return ec ? std::filesystem::path() : path.parent_path();
}

std::filesystem::path user_data_directory() {
#if defined(_WIN32)
    const char* appdata = std::getenv("APPDATA");
    return appdata && *appdata ? std::filesystem::path(appdata) / "freeTVC" : std::filesystem::path();
#elif defined(__APPLE__)
    const char* home = std::getenv("HOME");
    return home && *home ? std::filesystem::path(home) / "Library" / "Application Support" / "freeTVC" : std::filesystem::path();
#else
    if (const char* data = std::getenv("XDG_DATA_HOME"); data && *data) {
        return std::filesystem::path(data) / "freeTVC";
    }
    const char* home = std::getenv("HOME");
    return home && *home ? std::filesystem::path(home) / ".local" / "share" / "freeTVC" : std::filesystem::path();
#endif
}

}  // namespace freetvc
