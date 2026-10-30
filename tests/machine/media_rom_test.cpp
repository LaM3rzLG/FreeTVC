// SPDX-License-Identifier: GPL-3.0-or-later
// Host cassette and fast autostart on a real ROM set (design/media.md): autostart with and without autorun, the
// prompt state as a pure cache, SAVE and LOAD through a folder, a file the ROM refuses, and the OS's error-exit
// reset of an assignment to the cassette.
// usage: media_rom_test <rom dir> <preset>; exit 77 (CTest skip) when the ROM files are missing.
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <vector>

#include "core/input/key_script.hpp"
#include "core/input/keyboard.hpp"
#include "core/machine/machine.hpp"
#include "core/media/media.hpp"
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

class MemoryFolder final : public freetvc::CassetteFolder {
public:
    std::map<std::string, std::vector<std::uint8_t>> files;

    std::optional<std::vector<std::uint8_t>> read(std::string_view name, std::string& found) override {
        const auto it = files.find(std::string(name));
        if (it == files.end()) {
            return std::nullopt;
        }
        found = it->first;
        return it->second;
    }
    bool write(std::string_view name, std::span<const std::uint8_t> cas, std::string&) override {
        files[std::string(name)].assign(cas.begin(), cas.end());
        return true;
    }
    bool writable() const override { return true; }
};

// 10 PRINT "HELLO" as BASIC 2.2 stores it (read back from a SAVE)
freetvc::CasFile hello_cas(bool autorun) {
    const std::vector<std::uint8_t> program = {0x0D, 0x0A, 0x00, 0xDD, 0x20, 0x22, 'H', 'E', 'L', 'L', 'O', 0x22, 0xFF, 0x00};
    freetvc::CasFile cas;
    cas.stream.assign(16, 0);
    cas.stream[1] = 0x01;
    cas.stream[2] = static_cast<std::uint8_t>(program.size());
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

// Runs the autostart to its end in frame slices; returns the frames it took
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

freetvc::TypedText typed(const freetvc::Machine& m, std::u32string_view text) {
    return freetvc::type_text(text, m.frame_count() + 1);
}

bool same_state(const freetvc::Machine& a, const freetvc::Machine& b) {
    const freetvc::Z80Registers ra = a.cpu().registers();
    const freetvc::Z80Registers rb = b.cpu().registers();
    bool vram = true;
    for (freetvc::Bank bank : {freetvc::Bank::Vid0, freetvc::Bank::Vid1, freetvc::Bank::Vid2, freetvc::Bank::Vid3}) {
        const auto va = a.memory().bank_bytes(bank);
        const auto vb = b.memory().bank_bytes(bank);
        vram &= std::equal(va.begin(), va.end(), vb.begin(), vb.end());
    }
    return vram && std::equal(a.ram().begin(), a.ram().end(), b.ram().begin(), b.ram().end()) && a.ticks() == b.ticks() &&
           a.pc() == b.pc() && ra.af == rb.af && ra.bc == rb.bc && ra.de == rb.de && ra.hl == rb.hl &&
           ra.ix == rb.ix && ra.iy == rb.iy && ra.sp == rb.sp && ra.af2 == rb.af2 && ra.iff1 == rb.iff1 && ra.r == rb.r;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: media_rom_test <rom dir> <preset>\n");
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

    // Cold autostart, no autorun: LOAD, then RUN at the next prompt
    freetvc::Machine cold(*config);
    freetvc::Media media;
    cold.set_os_call_handler(&media);
    media.cassette().attach("hello", hello_cas(false));
    check(media.autostart(cold), "autostart starts");
    const std::uint64_t cold_frames = run_autostart(cold, media);
    check(media.state() == freetvc::AutostartState::Done, "cold autostart done: " + media.autostart_result());
    check(media.has_prompt_state(), "prompt state kept");
    run_frames(cold, media, {}, 20);
    check(screen_has(cold, "HELLO"), "the program printed HELLO after RUN");

    // The prompt state is a cache: the same program reaches the same machine state, much sooner
    freetvc::Machine warm(*config);
    warm.set_os_call_handler(&media);
    media.autostart(warm);
    const std::uint64_t warm_frames = run_autostart(warm, media);
    check(media.state() == freetvc::AutostartState::Done, "warm autostart done");
    run_frames(warm, media, {}, 20);
    check(same_state(cold, warm), "restored prompt state ends in the cold boot's state");
    check(warm_frames * 5 < cold_frames, "warm autostart: " + std::to_string(warm_frames) + " frames, cold " +
                                             std::to_string(cold_frames));

    // Autorun: done at the cassette's close, the ROM starts the program itself
    media.cassette().attach("hello", hello_cas(true));
    media.autostart(warm);
    run_autostart(warm, media);
    check(media.state() == freetvc::AutostartState::Done, "autorun autostart done");
    run_frames(warm, media, {}, 20);
    check(screen_has(warm, "HELLO"), "autorun printed HELLO");

    // A program header the ROM refuses ("Bad file", rom.md §6): the autostart fails instead of hanging
    freetvc::CasFile bad = hello_cas(true);
    bad.stream[1] = 0x00;
    media.cassette().attach("bad", bad);
    media.autostart(warm);
    run_autostart(warm, media);
    check(media.state() == freetvc::AutostartState::Failed, "refused file fails: " + media.autostart_result());

    // SAVE and LOAD through a folder; the saved file has an exact header
    MemoryFolder folder;
    media.cassette().set_folder(&folder);
    media.cassette().attach("hello", hello_cas(false));
    media.autostart(warm);
    run_autostart(warm, media);
    run_frames(warm, media, typed(warm, U"SAVE \"Copy\"\nNEW\nLOAD \"COPY\"\nLIST\n"), 400);
    const auto saved = folder.files.find("COPY");
    check(saved != folder.files.end(), "SAVE wrote COPY");
    if (saved != folder.files.end()) {
        const std::vector<std::uint8_t> expected = freetvc::build_cas(false, false, hello_cas(false).stream);
        check(saved->second == expected, "saved file = header + program header + program");
    }
    check(screen_has(warm, "10 PRINT \"HELLO\""), "LOAD \"COPY\" then LIST shows the line");

    // Editor input from the cassette until its end of file, then the OS's default assignment again ([OS] §4.3.1)
    folder.files["SRC"] = freetvc::build_cas(true, false, std::vector<std::uint8_t>{'4', '0', ' ', 'P', 'R', 'I', 'N', 'T', ' ', '4', '\r', '\n'});
    run_frames(warm, media, typed(warm, U"OPEN \"SRC\":POKE 2818,5\n"), 150);
    check(warm.ram()[0x0B02] == 0x02, "editor input assignment back to the editor after end of file");
    run_frames(warm, media, typed(warm, U"CLOSE\nLIST\n"), 150);
    check(screen_has(warm, "40 PRINT 4"), "the line read from the cassette was stored");

    if (failures == 0) {
        std::printf("PASS (cold autostart %llu frames, warm %llu)\n", static_cast<unsigned long long>(cold_frames),
                    static_cast<unsigned long long>(warm_frames));
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
