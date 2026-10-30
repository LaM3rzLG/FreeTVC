// SPDX-License-Identifier: GPL-3.0-or-later
// The tape deck on a real ROM set (design/media.md "Tape deck"): the ROM's own LOAD reads a CAS played as a tape
// (autorun, RUN, several sectors), a damaged tape fails, and SAVE records the signal our encoder writes.
// usage: tape_rom_test <rom dir> <preset>; exit 77 (CTest skip) when the ROM files are missing.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "core/input/key_script.hpp"
#include "core/machine/machine.hpp"
#include "core/media/media.hpp"
#include "core/media/tape_signal.hpp"
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

// 10 PRINT "HELLO" as BASIC stores it (media_rom_test), with a REM of `pad` bytes to stretch it over sectors
freetvc::CasFile hello_cas(bool autorun, std::size_t pad = 0) {
    std::vector<std::uint8_t> program = {0x0D, 0x0A, 0x00, 0xDD, 0x20, 0x22, 'H', 'E', 'L', 'L', 'O', 0x22, 0xFF};
    if (pad > 0) {
        // 20 REM xxx...: length byte, line number, REM token (FEh), the text, FFh
        const std::size_t length = 4 + pad + 1;
        program.push_back(static_cast<std::uint8_t>(length));
        program.insert(program.end(), {0x14, 0x00, 0xFE});
        for (std::size_t i = 0; i < pad; ++i) {
            program.push_back(static_cast<std::uint8_t>('A' + i % 26));
        }
        program.push_back(0xFF);
    }
    program.push_back(0x00);
    freetvc::CasFile cas;
    cas.stream.assign(16, 0);
    cas.stream[1] = 0x01;
    cas.stream[2] = static_cast<std::uint8_t>(program.size());
    cas.stream[3] = static_cast<std::uint8_t>(program.size() >> 8);
    cas.stream[4] = autorun ? 0xFF : 0x00;
    cas.stream.insert(cas.stream.end(), program.begin(), program.end());
    return cas;
}

bool screen_has(const freetvc::Machine& m, const std::string& text) {
    std::string screen;
    for (unsigned i = 0; i < 24 * 64; ++i) {
        const std::uint8_t b = m.ram()[0x100 + i];
        screen.push_back(b >= 0x20 && b < 0x7F ? static_cast<char>(b) : '.');
    }
    return screen.find(text) != std::string::npos;
}

std::uint64_t run_autostart(freetvc::Machine& m, freetvc::Media& media) {
    const std::uint64_t start = m.frame_count();
    while (media.busy()) {
        media.run_slice(m);
    }
    media.service(m);
    return m.frame_count() - start;
}

void run_frames(freetvc::Machine& m, freetvc::Media& media, const freetvc::TypedText& typed, std::uint64_t frames) {
    std::size_t next = 0;
    const std::uint64_t end = m.frame_count() + frames;
    while (m.frame_count() < end) {
        freetvc::apply_key_events(m, typed.events, next, m.frame_count());
        media.service(m);
        m.run({.max_ticks = kFrameTicks});
    }
}

bool ram_contains(const freetvc::Machine& m, const std::vector<std::uint8_t>& bytes) {
    return std::search(m.ram().begin(), m.ram().end(), bytes.begin(), bytes.end()) != m.ram().end();
}

freetvc::TapeSignal tape_of(const freetvc::CasFile& cas, const char* name) {
    const std::vector<freetvc::TapeProgram> programs{freetvc::cas_program(cas, name)};
    return freetvc::encode_tape(programs);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: tape_rom_test <rom dir> <preset>\n");
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
    freetvc::Machine m(*config);
    freetvc::Media media;
    m.set_os_call_handler(&media);
    m.set_tape_port(&media.deck());

    // No autorun: the ROM reads header and data block through 59h, then RUN at the prompt
    media.deck().insert(tape_of(hello_cas(false), "HELLO"));
    check(media.autostart_tape(m), "tape autostart starts");
    const std::uint64_t frames = run_autostart(m, media);
    check(media.state() == freetvc::AutostartState::Done, "tape autostart done: " + media.autostart_result());
    run_frames(m, media, {}, 20);
    check(screen_has(m, "HELLO"), "the program printed HELLO after RUN");
    check(!media.deck().motor(), "the ROM stopped the motor after the load");

    // Autorun: done at the close, the ROM starts the program
    media.deck().insert(tape_of(hello_cas(true), "HELLO"));
    media.autostart_tape(m);
    run_autostart(m, media);
    check(media.state() == freetvc::AutostartState::Done, "autorun tape done: " + media.autostart_result());
    run_frames(m, media, {}, 20);
    check(screen_has(m, "HELLO"), "autorun printed HELLO");

    // Three sectors (a 600-byte REM): the sector chain and the per-sector CRCs
    const freetvc::CasFile big = hello_cas(false, 600);
    media.deck().insert(tape_of(big, "BIG"));
    media.autostart_tape(m);
    run_autostart(m, media);
    check(media.state() == freetvc::AutostartState::Done, "three-sector tape done: " + media.autostart_result());
    check(ram_contains(m, std::vector<std::uint8_t>(big.block().begin(), big.block().end())), "the whole program is in RAM");

    // A damaged tape: one bit of the data block flipped from "0" to "1" fails the CRC
    freetvc::TapeSignal damaged = tape_of(big, "BIG");
    {
        // A "0" cycle near the middle of the data block becomes a "1" cycle of the same total change
        const std::uint64_t target = damaged.program_ends.front() - 200 * 3450;
        std::uint64_t at = 0;
        for (std::size_t i = 0; i + 1 < damaged.runs.size(); ++i) {
            if (at >= target && damaged.runs[i] == 1725 && damaged.runs[i + 1] == 1725) {
                damaged.runs[i] = 1213;
                damaged.runs[i + 1] = 1212;
                break;
            }
            at += damaged.runs[i];
        }
    }
    media.deck().insert(damaged);
    media.autostart_tape(m);
    run_autostart(m, media);
    check(media.state() == freetvc::AutostartState::Failed, "a damaged tape fails: " + media.autostart_result());

    // SAVE records the signal: decoded, it holds the blocks our encoder writes for the same program
    media.deck().insert(tape_of(hello_cas(false), "HELLO"));
    media.autostart_tape(m);
    run_autostart(m, media);
    media.deck().insert_blank();
    media.deck().record();
    run_frames(m, media, freetvc::type_text(U"SAVE \"REC\"\n", m.frame_count() + 1), 1500);
    media.deck().stop();
    const auto recorded = freetvc::decode_tape(media.deck().signal());
    const auto expected = freetvc::tape_blocks(freetvc::cas_program(hello_cas(false), "REC"));
    check(recorded.size() == 2, "SAVE wrote two blocks (" + std::to_string(recorded.size()) + ")");
    if (recorded.size() == 2) {
        check(recorded[1] == expected[1], "the recorded data block equals ours");
        check(recorded[0] == expected[0], "the recorded header block equals ours");
    }
    // ...and the recording loads again
    media.deck().rewind();
    media.autostart_tape(m);
    run_autostart(m, media);
    check(media.state() == freetvc::AutostartState::Done, "the recording loads: " + media.autostart_result());

    if (failures == 0) {
        std::printf("PASS (tape autostart %llu frames)\n", static_cast<unsigned long long>(frames));
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
