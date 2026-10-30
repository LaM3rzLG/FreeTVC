// SPDX-License-Identifier: GPL-3.0-or-later
// Types every character of the key map at the BASIC prompt and reads it back from the OS screen copy at 0100h
// (64 TVC codes per row, tvc-hardware.md §3), then runs PRINT 6*7 and looks for 42.
// usage: typing_test <rom dir> <preset> [HOLD GAP AFTER_RETURN]; exit 77 (CTest skip) when the ROM files are missing.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/input/key_script.hpp"
#include "core/input/keyboard.hpp"
#include "core/machine/machine.hpp"
#include "frontend/common/presets.hpp"

namespace {

constexpr int kExitSkip = 77;
constexpr std::uint64_t kSpaceFrame = 330;  // dismisses the start-up logo, as in boot_test
constexpr std::uint64_t kTypeFrame = 400;
constexpr unsigned kPromptRow = 5;  // "ok"; the typed line starts on the next row
constexpr unsigned kColumns = 32;   // BASIC starts in the 4-colour mode: 32 characters per 64-byte copy row

void run_to(freetvc::Machine& m, const std::vector<freetvc::KeyEvent>& events, std::size_t& next, std::uint64_t& frame,
            std::uint64_t end) {
    for (; frame < end; ++frame) {
        freetvc::apply_key_events(m, events, next, frame);
        m.run({.max_ticks = freetvc::kTStatesPerFrame * freetvc::kTicksPerTState});
    }
}

std::string screen_row(const freetvc::Machine& m, unsigned row) {
    std::string text;
    for (unsigned c = 0; c < 64; ++c) {
        const std::uint8_t code = m.ram()[0x100 + row * 64 + c];
        text.push_back(code >= 0x20 && code < 0x7F ? static_cast<char>(code) : '.');
    }
    return text.substr(0, text.find_last_not_of(' ') + 1);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3 && argc != 6) {
        std::fprintf(stderr, "usage: typing_test <rom dir> <preset> [HOLD GAP AFTER_RETURN]\n");
        return 2;
    }
    const freetvc::Preset* preset = freetvc::find_preset(argv[2]);
    if (!preset) {
        std::fprintf(stderr, "unknown preset '%s'\n", argv[2]);
        return 2;
    }
    std::string error;
    const auto config = freetvc::load_preset(*preset, argv[1], error);
    if (!config) {
        std::printf("SKIP: %s (set FREETVC_ROM_DIR)\n", error.c_str());
        return kExitSkip;
    }
    freetvc::TypingTiming timing;
    if (argc == 6) {
        timing = {static_cast<unsigned>(std::atoi(argv[3])), static_cast<unsigned>(std::atoi(argv[4])),
                  static_cast<unsigned>(std::atoi(argv[5]))};
    }

    // Every printable character once, in key-map order
    std::u32string all;
    std::vector<std::uint8_t> expected;
    for (const freetvc::CharKey& entry : freetvc::char_keys()) {
        if (entry.code >= 0x20) {
            all.push_back(entry.ch);
            expected.push_back(entry.code);
        }
    }

    std::vector<freetvc::KeyEvent> events{{kSpaceFrame, freetvc::kKeySpace, true}, {kSpaceFrame + 5, freetvc::kKeySpace, false}};
    const freetvc::TypedText line = freetvc::type_text(all, kTypeFrame, timing);
    events.insert(events.end(), line.events.begin(), line.events.end());

    freetvc::Machine m(*config);
    std::size_t next = 0;
    std::uint64_t frame = 0;
    run_to(m, events, next, frame, line.end_frame + 10);

    unsigned mismatches = 0;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const std::size_t row = kPromptRow + 1 + i / kColumns;
        const std::uint8_t got = m.ram()[0x100 + row * 64 + i % kColumns];
        if (got != expected[i]) {
            if (++mismatches <= 10) {
                std::printf("  char %zu U+%04X: screen %02Xh, expected %02Xh\n", i, static_cast<unsigned>(all[i]), got,
                            expected[i]);
            }
        }
    }
    std::printf("%s: typed %zu characters (hold %u, gap %u, after RETURN %u, %llu frames), %u wrong\n", argv[2],
                all.size(), timing.hold, timing.gap, timing.after_return,
                static_cast<unsigned long long>(line.end_frame - kTypeFrame), mismatches);

    // RETURN on the junk line, then a real command; its answer must appear below
    const freetvc::TypedText command = freetvc::type_text(U"\nPRINT 6*7\n", frame, timing);
    events.insert(events.end(), command.events.begin(), command.events.end());
    run_to(m, events, next, frame, command.end_frame + 50);
    bool answered = false;
    for (unsigned row = kPromptRow + 1; row < 24; ++row) {
        const std::string text = screen_row(m, row);
        answered = answered || text == " 42";
        if (!text.empty()) {
            std::printf("  row %u '%s'\n", row, text.c_str());
        }
    }

    const bool ok = mismatches == 0 && answered;
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
