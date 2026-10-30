// SPDX-License-Identifier: GPL-3.0-or-later
// The NanoSD's ROM v0.33 on real ROMs: its menu, LOAD/SAVE and EXT 0 on a host folder;
// usage: nanosd_rom_test <rom dir> <preset> <nanosd.rom> <scratch dir>.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "core/cards/nanosd.hpp"
#include "core/input/key_script.hpp"
#include "core/machine/machine.hpp"
#include "frontend/common/card256_host.hpp"
#include "frontend/common/presets.hpp"

namespace {

namespace fs = std::filesystem;

constexpr int kExitSkip = 77;
constexpr std::uint64_t kFrameTicks = freetvc::kTStatesPerFrame * freetvc::kTicksPerTState;
int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    }
}

// The OS's ASCII copy of the text screen in U0 (tvc-ram.sym), 24 rows of 64
std::string screen(const freetvc::Machine& m) {
    const auto u0 = m.memory().bank_bytes(freetvc::Bank::U0);
    std::string text;
    for (unsigned row = 0; row < 24; ++row) {
        std::string line(reinterpret_cast<const char*>(u0.data()) + 0x100 + row * 64, 64);
        line.erase(line.find_last_not_of(' ') + 1);
        text += line + "\n";
    }
    return text;
}

struct Rig {
    freetvc::Machine& m;
    std::vector<freetvc::KeyEvent> keys;
    std::size_t next = 0;

    void frame() {
        freetvc::apply_key_events(m, keys, next, m.frame_count());
        m.run({.max_ticks = kFrameTicks, .to_next_frame = true});
    }
    // Runs until `text` is on the screen, at most `frames` more; false if it never came
    bool wait_for(const std::string& text, std::uint64_t frames) {
        for (std::uint64_t i = 0; i < frames; ++i) {
            if (screen(m).find(text) != std::string::npos) {
                return true;
            }
            frame();
        }
        return screen(m).find(text) != std::string::npos;
    }
    void press(freetvc::MatrixKey key) {
        const std::uint64_t f = m.frame_count();
        keys.push_back({f, key, true});
        keys.push_back({f + 4, key, false});
        for (int i = 0; i < 6; ++i) {
            frame();
        }
    }
    void type(std::u32string_view text) {
        const auto typed = freetvc::type_text(text, m.frame_count());
        keys.insert(keys.end(), typed.events.begin(), typed.events.end());
        while (m.frame_count() < typed.end_frame) {
            frame();
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::fprintf(stderr, "usage: nanosd_rom_test <rom dir> <preset> <nanosd.rom> <scratch dir>\n");
        return 2;
    }
    const freetvc::Preset* preset = freetvc::find_preset(argv[2]);
    if (!preset) {
        std::fprintf(stderr, "unknown preset '%s'\n", argv[2]);
        return 2;
    }
    std::string error;
    auto config = freetvc::load_preset(*preset, argv[1], error);
    auto rom = config ? freetvc::read_file(argv[3], freetvc::NanoSd::kRomSize, error) : std::nullopt;
    if (!config || !rom) {
        std::printf("SKIP: %s\n", error.c_str());
        return kExitSkip;
    }
    const fs::path dir = fs::path(argv[4]) / argv[2];
    fs::remove_all(dir);
    fs::create_directories(dir / "games");
    {
        std::ofstream(dir / "Some Long Name.cas", std::ios::binary) << std::string(200, 'x');
    }
    freetvc::PendriveFolder folder(dir);
    freetvc::NanoSdMedia media;
    media.sd = &folder;
    config->cards[freetvc::kNanoSdSlot] = {freetvc::CardKind::NanoSd, *rom};
    freetvc::Machine m(*config);
    m.set_nanosd_media(&media);
    Rig r{m, {}, 0};

    // A fresh Nano's EEPROM shows the file-select menu at power-on
    check(r.wait_for("NanoSD IO Card present, ROM v0.33", 1500), "the card's start-up line");
    check(r.wait_for("SOMELO~1.CAS", 300), "the menu lists the folder: a long name as its 8.3 alias");
    check(screen(m).find("GAMES") != std::string::npos && screen(m).find("<DIR>") != std::string::npos, "a directory in the menu");
    check(media.eeprom_valid && media.menu == 1, "the ROM read parameter 0, so the EEPROM got its ID");
    const std::string menu = screen(m);
    r.press(freetvc::kKeyEsc);
    // SPACE leaves the start-up logo where the ROM has one
    for (int i = 0; i < 3 && !r.wait_for("bytes free", 200); ++i) {
        r.press(freetvc::kKeySpace);
    }
    check(r.wait_for("bytes free", 300), "ESC goes on to BASIC");

    r.type(U"10 PRINT \"NANO\"\nSAVE \"T1\"\n");
    r.wait_for("ok", 300);
    for (int i = 0; i < 100; ++i) {
        r.frame();
    }
    std::ifstream saved(dir / "T1.CAS", std::ios::binary);
    const std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(saved), std::istreambuf_iterator<char>()};
    check(bytes.size() > 128 && (bytes[0] == 0x11 || bytes[0] == 0x01) && std::all_of(bytes.begin() + 1, bytes.begin() + 128, [](std::uint8_t b) { return b == 0; }),
          "SAVE wrote T1.CAS with the firmware's header (type, then zeros)");
    r.type(U"NEW\nLOAD \"T1\"\n");
    check(r.wait_for("Found: T1", 400), "LOAD finds T1 on the card");
    r.type(U"RUN\n");
    check(r.wait_for("\nNANO\n", 200), "and the program runs");

    r.type(U"EXT 0\n");
    check(r.wait_for("** NanoSD IO Card - CLI **", 300), "EXT 0 opens the card's command line");
    r.type(U"VER\nMKDIR D\nCD D\nPWD\n");
    check(r.wait_for("NanoSD Arduino fw v0.32", 300), "VER asks the Nano for its version");
    check(r.wait_for("/D/", 300), "MKDIR, CD and PWD");
    check(fs::is_directory(dir / "D"), "MKDIR made the host folder");
    if (failures != 0) {
        std::printf("menu:\n%sscreen at the end:\n%s", menu.c_str(), screen(m).c_str());
        return 1;
    }
    std::printf("nanosd_rom_test %s: all passed\n", argv[2]);
    return 0;
}
