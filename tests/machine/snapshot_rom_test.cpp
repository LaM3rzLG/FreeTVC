// SPDX-License-Identifier: GPL-3.0-or-later
// TVCS snapshots: a snapshot taken mid-frame, written to bytes and loaded into a freshly powered machine, must run on
// exactly like the machine it came from; saving right after the load gives the same bytes; a wrong board is refused.
// usage: snapshot_rom_test <rom dir> <preset>; exit 77 (CTest skip) when the ROM files are missing.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/machine/machine.hpp"
#include "core/snapshot/machine_state.hpp"
#include "frontend/common/presets.hpp"

namespace {

constexpr int kExitSkip = 77;
int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
}

constexpr std::uint64_t kFrameTicks = freetvc::kTStatesPerFrame * freetvc::kTicksPerTState;

// Frames with SPACE down for the logo, then RETURN twice at the prompt; audio collected all along
std::vector<float> run_script(freetvc::Machine& m, int from, int to) {
    std::vector<float> audio;
    for (int frame = from; frame < to; ++frame) {
        m.set_key(7, 5, frame >= 330 && frame < 335);  // SPACE
        m.set_key(6, 0, (frame >= 420 && frame < 423) || (frame >= 440 && frame < 443));  // RETURN ❓ row/column only matter for equality
        m.run({.max_ticks = kFrameTicks});
        std::vector<float> chunk(m.audio_available());
        m.read_audio(chunk);
        audio.insert(audio.end(), chunk.begin(), chunk.end());
    }
    return audio;
}

bool same_machine(const freetvc::Machine& a, const freetvc::Machine& b) {
    const auto ra = a.cpu().registers();
    const auto rb = b.cpu().registers();
    return a.ticks() == b.ticks() && a.pc() == b.pc() && ra.af == rb.af && ra.bc == rb.bc && ra.de == rb.de && ra.hl == rb.hl &&
           ra.sp == rb.sp && ra.ix == rb.ix && ra.iy == rb.iy && ra.r == rb.r && ra.iff1 == rb.iff1 &&
           std::equal(a.ram().begin(), a.ram().end(), b.ram().begin()) &&
           std::equal(a.memory().vram().begin(), a.memory().vram().end(), b.memory().vram().begin()) &&
           a.framebuffer().pixels == b.framebuffer().pixels && a.frame_count() == b.frame_count() &&
           a.memory().page_register() == b.memory().page_register();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: snapshot_rom_test <rom dir> <preset>\n");
        return 2;
    }
    const freetvc::Preset* preset = freetvc::find_preset(argv[2]);
    if (!preset) {
        std::fprintf(stderr, "unknown preset '%s'\n", argv[2]);
        return 2;
    }
    std::string error;
    auto config = freetvc::load_preset(*preset, argv[1], error);
    if (!config) {
        std::printf("SKIP: %s (set FREETVC_ROM_DIR)\n", error.c_str());
        return kExitSkip;
    }

    // Stop mid-frame and mid-logo, with SPACE held: the snapshot carries the key and the half-drawn frame
    freetvc::Machine original(*config);
    run_script(original, 0, 332);
    // A tone running across the snapshot: pitch C00h, tone gate on, amplitude 15 (tvc-hardware.md §4)
    original.io_write(0x04, 0x00);
    original.io_write(0x05, 0x1C);
    original.io_write(0x06, 0x3C);
    original.run({.max_ticks = kFrameTicks, .max_instructions = 12345});
    freetvc::Snapshot snapshot;
    original.save_state(snapshot);
    const std::vector<std::uint8_t> bytes = snapshot.encode();
    std::vector<float> unread(original.audio_available());  // a load drops samples not yet read, so the original does too
    original.read_audio(unread);
    std::printf("snapshot: %zu bytes, at tick %llu\n", bytes.size(), static_cast<unsigned long long>(original.ticks()));

    const auto decoded = freetvc::Snapshot::decode(bytes, error);
    check(decoded.has_value(), "decode");
    if (!decoded) {
        std::printf("  %s\n", error.c_str());
        return 1;
    }
    const auto needs = freetvc::snapshot_needs(*decoded, error);
    check(needs && freetvc::snapshot_matches(*config, *needs), "the snapshot's needs match the preset");

    freetvc::Machine loaded(*config);
    run_script(loaded, 0, 50);  // somewhere else entirely
    check(loaded.load_state(*decoded, error), "load_state");
    check(same_machine(original, loaded), "state right after the load");
    freetvc::Snapshot again;
    loaded.save_state(again);
    check(again.encode() == bytes, "save after load gives the same bytes");

    const std::vector<float> audio_a = run_script(original, 333, 520);
    const std::vector<float> audio_b = run_script(loaded, 333, 520);
    check(same_machine(original, loaded), "state after 187 more frames with keys");
    check(audio_a == audio_b, "the same audio samples");
    check(std::any_of(audio_a.begin(), audio_a.end(), [](float s) { return s > 0.01f || s < -0.01f; }), "the audio is not silent");

    // Another board is refused and leaves the machine as it was
    freetvc::MachineConfig other = *config;
    other.model = other.model == freetvc::Model::Tvc32k ? freetvc::Model::Tvc64k : freetvc::Model::Tvc32k;
    freetvc::Machine wrong(other);
    const std::uint64_t ticks = wrong.ticks();
    check(!wrong.load_state(*decoded, error) && !error.empty() && wrong.ticks() == ticks, "a snapshot from another board is refused");

    // A truncated file fails cleanly
    const std::vector<std::uint8_t> cut(bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(bytes.size() / 2));
    check(!freetvc::Snapshot::decode(cut, error).has_value(), "a truncated snapshot is refused");

    if (g_failures == 0) {
        std::printf("snapshot_rom_test %s: all checks passed\n", argv[2]);
        return 0;
    }
    std::printf("snapshot_rom_test %s: %d check(s) failed\n", argv[2], g_failures);
    return 1;
}
