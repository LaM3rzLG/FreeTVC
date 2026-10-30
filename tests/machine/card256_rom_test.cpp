// SPDX-License-Identifier: GPL-3.0-or-later
// The TVC256k++ ROM's EXT command line on real ROMs; usage: card256_rom_test <rom dir> <preset> <card256.rom>.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/input/key_script.hpp"
#include "core/machine/machine.hpp"
#include "frontend/common/presets.hpp"

namespace {

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

struct Typist {
    std::vector<freetvc::KeyEvent> keys;
    std::size_t next = 0;

    void type(std::u32string_view text, std::uint64_t frame) {
        const auto typed = freetvc::type_text(text, frame);
        keys.insert(keys.end(), typed.events.begin(), typed.events.end());
    }
    void run_to(freetvc::Machine& m, std::uint64_t frame) {
        while (m.frame_count() < frame) {
            freetvc::apply_key_events(m, keys, next, m.frame_count());
            m.run({.max_ticks = kFrameTicks, .to_next_frame = true});
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: card256_rom_test <rom dir> <preset> <card256.rom>\n");
        return 2;
    }
    const freetvc::Preset* preset = freetvc::find_preset(argv[2]);
    if (!preset) {
        std::fprintf(stderr, "unknown preset '%s'\n", argv[2]);
        return 2;
    }
    std::string error;
    auto config = freetvc::load_preset(*preset, argv[1], error);
    auto rom = config ? freetvc::read_file(argv[3], 0x3000, error) : std::nullopt;
    if (!config || !rom) {
        std::printf("SKIP: %s\n", error.c_str());
        return kExitSkip;
    }
    config->cards[freetvc::kCard256Slot] = {freetvc::CardKind::Card256, *rom};
    freetvc::Machine m(*config);
    Typist t;
    std::string text;

    // SPACE leaves the start-up logo, which waits for a key and takes it (tvc-hardware.md §7 "Editor behaviour")
    t.type(U" ", 380);
    t.run_to(m, 440);
    text = screen(m);
    if (config->model == freetvc::Model::Tvc32k) {
        check(text.find("42255") != std::string::npos, "a 32k board with the card has a 64k board's RAM (25871 without)");
    }
    // GRAPHICS 2 as the card's manual advises: HELP fits the screen only there
    t.type(U"GRAPHICS 2\nEXT 0\nVER\nDIR\n", 450);
    t.run_to(m, 750);
    text = screen(m);
    check(text.find("** TVC256k++ IO Card - CLI **") != std::string::npos, "EXT 0 opens the card's command line");
    check(text.find("TVC256k++ Firmware: v0.24") != std::string::npos, "VER reads the version registers");
    check(text.find("IO operation failed (ED:83)") != std::string::npos, "DIR: no drive before 6e");
    const std::string first = text;
    t.type(U"HELP\n", 760);
    t.type(U"X\n", 900);
    t.run_to(m, 980);
    text = screen(m);
    check(text.find("VER: prints the version of the firmware") != std::string::npos, "HELP prints from ROM bank 1");
    check(text.find("TVC256k++ CLI Exit. Bye!") != std::string::npos, "X leaves the command line");
    if (failures != 0) {
        std::printf("screen after DIR:\n%sscreen after X:\n%s", first.c_str(), text.c_str());
        return 1;
    }
    std::printf("card256_rom_test %s: all passed\n", argv[2]);
    return 0;
}
