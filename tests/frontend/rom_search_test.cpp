// SPDX-License-Identifier: GPL-3.0-or-later
// ROM search: the order of the places, first match wins, file names first, then CRC-32 under any name.
// usage: rom_search_test <rom dir> <scratch dir>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/cards/card256.hpp"
#include "frontend/common/presets.hpp"

namespace fs = std::filesystem;

namespace {

int g_failures = 0;

void check(bool ok, const char* what, const std::string& detail = {}) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n%s\n", what, detail.c_str());
    }
}

void put(const fs::path& from, const fs::path& to) {
    fs::create_directories(to.parent_path());
    fs::copy_file(from, to, fs::copy_options::overwrite_existing);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::printf("usage: rom_search_test <rom dir> <scratch dir>\n");
        return 2;
    }
    const fs::path roms = argv[1];
    if (!fs::exists(roms / "sys22.bin") || !fs::exists(roms / "ext1x.bin")) {
        std::printf("rom_search_test: no ROMs in %s, skipped\n", roms.string().c_str());
        return 77;
    }
    const fs::path scratch = fs::path(argv[2]) / "rom_search";
    fs::remove_all(scratch);
    const fs::path program = scratch / "program";
    const fs::path user = scratch / "user";
    const fs::path cli = scratch / "cli";
    fs::create_directories(cli);
#if defined(_WIN32)
    _putenv_s("FREETVC_ROM_DIR", "");
#else
    unsetenv("FREETVC_ROM_DIR");
#endif

    // The order, with unset places left out
    const auto with_ini = freetvc::rom_places(cli.string(), (scratch / "ini").string(), program, user);
    check(with_ini.size() == 4 && with_ini[1].why == "freetvc.ini" && with_ini[2].why == "next to the program",
          "freetvc.ini's rom_dir comes after --rom-dir and before the program's folder");
    auto places = freetvc::rom_places(cli.string(), "", program, user);
    check(places.size() == 3 && places[0].why == "--rom-dir" && places[1].dir == program / "roms" && places[2].dir == user / "roms",
          "order: --rom-dir, next to the program, user data folder");
    check(freetvc::rom_places("", "", {}, {}).empty(), "nothing set, nothing searched");

    const freetvc::Preset& p22 = *freetvc::find_preset("64k+-2.2");
    std::string error;
    const freetvc::RomPlace* found = nullptr;

    // Nowhere: the error names every place
    check(!freetvc::find_preset_roms(p22, places, error, &found), "no ROMs anywhere");
    check(error.find("--rom-dir: " + cli.string()) != std::string::npos && error.find("next to the program") != std::string::npos &&
              error.find("user data folder") != std::string::npos && error.find("no sys22.bin") != std::string::npos,
          "the error lists the places", error);

    // Renamed files in the user data folder, recognised by CRC-32
    put(roms / "sys22.bin", user / "roms" / "TVC22_D6.64");
    put(roms / "ext2x.bin", user / "roms" / "tvc22_d7.64");
    auto config = freetvc::find_preset_roms(p22, places, error, &found);
    check(config && found && found->why == "user data folder", "found by CRC-32 in the user data folder", error);
    check(config && config->roms.sys.size() == 0x4000 && config->roms.ext.size() == 0x2000, "both images read");

    // First match wins: the program folder comes before the user data folder
    put(roms / "sys22.bin", program / "roms" / "sys22.bin");
    put(roms / "ext2x.bin", program / "roms" / "ext2x.bin");
    config = freetvc::find_preset_roms(p22, places, error, &found);
    check(config && found && found->why == "next to the program", "the earlier place wins");

    // A place needs both files: a lone SYS in --rom-dir is passed over
    put(roms / "sys22.bin", cli / "sys22.bin");
    config = freetvc::find_preset_roms(p22, places, error, &found);
    check(config && found && found->why == "next to the program", "half a set is passed over");

    // A file of the wrong image is not taken for another one; EXT 1.x also as MAME's 8 KiB socket image
    const freetvc::Preset& p12 = *freetvc::find_preset("64k-1.2");
    const fs::path mixed = scratch / "mixed";
    put(roms / "sys22.bin", mixed / "a.rom");
    put(roms / "sys12.bin", mixed / "b.rom");
    std::string ext1x;
    {
        std::ifstream in(roms / "ext1x.bin", std::ios::binary);
        ext1x.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (ext1x.size() == 0x2000) {
            ext1x.erase(0, 0x1000);  // the autofetch writes the socket image; its upper half is the ROM
        }
        std::ofstream out(mixed / "c.rom", std::ios::binary);
        out << std::string(0x1000, '\xFF') << ext1x;
    }
    config = freetvc::find_preset_roms(p12, {{"test", mixed}}, error, &found);
    check(config.has_value(), "SYS 1.2 among other ROMs, EXT 1.x padded to 8 KiB", error);
    if (config) {
        std::ifstream in(roms / "sys12.bin", std::ios::binary);
        const std::vector<char> sys12((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        check(config->roms.sys.size() == sys12.size() &&
                  std::equal(sys12.begin(), sys12.end(), config->roms.sys.begin(),
                             [](char a, std::uint8_t b) { return static_cast<std::uint8_t>(a) == b; }),
              "the SYS 1.2 file was taken, not SYS 2.2");
        check(config->roms.ext.size() == ext1x.size() &&
                  std::equal(ext1x.begin(), ext1x.end(), config->roms.ext.begin(),
                             [](char a, std::uint8_t b) { return static_cast<std::uint8_t>(a) == b; }),
              "the 8 KiB EXT socket image becomes the 4 KiB ROM (normalise_ext)");
    }

    // The TVC256k++ image: ROM padded with FFh, then the font and the BMP's pixel rows
    {
        using freetvc::Card256;
        const fs::path pics = scratch / "card256";
        fs::create_directories(pics);
        std::string bmp(Card256::kPictureHeader + Card256::kPictureSize, '\0');
        const auto le = [&](std::size_t at, unsigned v, unsigned n) {
            for (unsigned i = 0; i < n; ++i) {
                bmp[at + i] = static_cast<char>(v >> (8 * i));
            }
        };
        bmp[0] = 'B';
        bmp[1] = 'M';
        le(10, 118, 4);
        le(18, 256, 4);
        le(22, 240, 4);
        le(28, 4, 2);
        for (std::size_t i = 0; i < Card256::kPictureSize; ++i) {
            bmp[Card256::kPictureHeader + i] = static_cast<char>(i / 128);
        }
        std::ofstream(pics / "card256-picture.bmp", std::ios::binary) << bmp;
        std::string font(Card256::kFontSize, '\0');
        for (std::size_t i = 0; i < font.size(); ++i) {
            font[i] = static_cast<char>(i);
        }
        std::ofstream(pics / "anything.bin", std::ios::binary) << font;
        std::vector<std::string> notes;
        auto image = freetvc::card256_image({0xAA, 0xBB}, {{"test", pics}}, notes);
        check(image.size() == Card256::kImageSize && image[0] == 0xAA && image[2] == 0xFF && image[Card256::kRomSize - 1] == 0xFF,
              "card256 image: the ROM padded with FFh to 12 KiB");
        check(notes.size() == 1 && image[Card256::kRomSize + 5] == 0, "no font file: zeros and a note");
        check(image[Card256::kRomSize + Card256::kFontSize + 128 * 7] == 7 && image.back() == 239, "the picture's rows as stored");
        // By its known CRC-32 under any name: the real font's CRC is not this one, so name it
        fs::rename(pics / "anything.bin", pics / "card256-font.bin");
        le(18, 255, 4);
        std::ofstream(pics / "card256-picture.bmp", std::ios::binary) << bmp;
        notes.clear();
        image = freetvc::card256_image({}, {{"test", pics}}, notes);
        check(image.size() == Card256::kImageSize && image[0] == 0xFF && image[Card256::kRomSize + 5] == 5, "card256 image: the font by name");
        check(notes.size() == 1 && notes[0].find("not the 256 x 240") != std::string::npos &&
                  image[Card256::kRomSize + Card256::kFontSize + 128 * 7] == 0,
              "a BMP of another size is left out with a note", notes.empty() ? std::string() : notes[0]);
        fs::remove_all(pics);
        fs::create_directories(pics);
        notes.clear();
        image = freetvc::card256_image({1, 2, 3}, {{"test", pics}}, notes);
        check(image == std::vector<std::uint8_t>{1, 2, 3} && notes.size() == 2, "neither file: the ROM alone, two notes");
    }

    fs::remove_all(scratch);
    if (g_failures == 0) {
        std::printf("rom_search_test: all checks passed\n");
        return 0;
    }
    std::printf("rom_search_test: %d check(s) failed\n", g_failures);
    return 1;
}
