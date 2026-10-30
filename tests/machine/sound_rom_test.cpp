// SPDX-License-Identifier: GPL-3.0-or-later
// Types SOUND PITCH 3950,VOLUME 15,DURATION 50 at the BASIC prompt and checks the machine's audio output:
// silence before, one tone burst of 50 OS ticks (20 ms each), period 16 x (4096 - 3950) T = 35.881 samples at 48 kHz.
// usage: sound_rom_test <rom dir> <preset>; exit 77 (CTest skip) when the ROM files are missing.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
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
constexpr unsigned kPitch = 3950;
constexpr std::size_t kWindow = 480;  // 10 ms at 48 kHz

struct Burst {
    std::size_t start = 0;
    std::size_t end = 0;
};

// Runs of 10 ms windows whose RMS around their own mean is above 0.01
std::vector<Burst> find_bursts(const std::vector<float>& s) {
    std::vector<Burst> bursts;
    bool in_burst = false;
    for (std::size_t w = 0; w + kWindow <= s.size(); w += kWindow) {
        double mean = 0.0;
        for (std::size_t i = w; i < w + kWindow; ++i) {
            mean += s[i];
        }
        mean /= kWindow;
        double power = 0.0;
        for (std::size_t i = w; i < w + kWindow; ++i) {
            power += (s[i] - mean) * (s[i] - mean);
        }
        const bool active = std::sqrt(power / kWindow) > 0.01;
        if (active && !in_burst) {
            bursts.push_back({w, w + kWindow});
        } else if (active) {
            bursts.back().end = w + kWindow;
        }
        in_burst = active;
    }
    return bursts;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: sound_rom_test <rom dir> <preset>\n");
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

    std::vector<freetvc::KeyEvent> events{{kSpaceFrame, freetvc::kKeySpace, true}, {kSpaceFrame + 5, freetvc::kKeySpace, false}};
    const freetvc::TypedText line = freetvc::type_text(U"SOUND PITCH 3950,VOLUME 15,DURATION 50\n", kTypeFrame, {});
    events.insert(events.end(), line.events.begin(), line.events.end());

    freetvc::Machine m(*config);
    std::vector<float> audio;
    std::size_t next = 0;
    for (std::uint64_t frame = 0; frame < line.end_frame + 150; ++frame) {
        freetvc::apply_key_events(m, events, next, frame);
        m.run({.max_ticks = freetvc::kTStatesPerFrame * freetvc::kTicksPerTState});
        const std::size_t old_size = audio.size();
        audio.resize(old_size + m.audio_available());
        m.read_audio(std::span<float>(audio).subspan(old_size));
    }

    const std::vector<Burst> bursts = find_bursts(audio);
    for (const Burst& b : bursts) {
        std::printf("  burst %.3f-%.3f s (%.0f ms)\n", b.start / 48000.0, b.end / 48000.0, (b.end - b.start) / 48.0);
    }
    bool ok = bursts.size() == 1;
    // 50 ticks of the 49.76 Hz cursor IRQ = 1005 ms; the first tick comes part of a frame after the command starts
    const double ms = ok ? (bursts[0].end - bursts[0].start) / 48.0 : 0.0;
    ok = ok && ms >= 970.0 && ms <= 1040.0;
    const double type_end = line.end_frame * static_cast<double>(freetvc::kTStatesPerFrame) / 3.125e6;
    ok = ok && bursts[0].start / 48000.0 > type_end - 1.0;

    // Cycle lengths between rising mean crossings, away from the attack and release
    double shortest = 1e9;
    double longest = 0.0;
    double average = 0.0;
    if (ok) {
        const std::size_t from = bursts[0].start + 4 * kWindow;
        const std::size_t to = bursts[0].end - 4 * kWindow;
        double mean = 0.0;
        for (std::size_t i = from; i < to; ++i) {
            mean += audio[i];
        }
        mean /= static_cast<double>(to - from);
        std::vector<double> rising;
        for (std::size_t i = from + 1; i < to; ++i) {
            const double y0 = audio[i - 1] - mean;
            const double y1 = audio[i] - mean;
            if (y0 < 0.0 && y1 >= 0.0) {
                rising.push_back(static_cast<double>(i - 1) + -y0 / (y1 - y0));
            }
        }
        for (std::size_t i = 1; i < rising.size(); ++i) {
            shortest = std::min(shortest, rising[i] - rising[i - 1]);
            longest = std::max(longest, rising[i] - rising[i - 1]);
        }
        average = (rising.back() - rising.front()) / static_cast<double>(rising.size() - 1);
    }
    const double expected = 16.0 * (4096 - kPitch) / 3.125e6 * 48000.0;
    std::printf("  period: average %.4f, shortest %.3f, longest %.3f samples (expected %.4f); P %03Xh\n", average, shortest,
                longest, expected, m.sound().pitch());
    ok = ok && std::abs(average - expected) < 0.005 && expected - shortest < 0.2 && longest - expected < 0.2;
    ok = ok && m.sound().pitch() == kPitch && !m.sound().tone_enabled();

    std::printf("%s: %s\n", argv[2], ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
