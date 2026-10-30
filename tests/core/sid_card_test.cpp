// SPDX-License-Identifier: GPL-3.0-or-later
// The SID card without system ROMs (design/cards.md "SID card"): its ports, reads, sound, copies, snapshots, reset.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/machine/machine.hpp"
#include "core/snapshot/machine_state.hpp"
#include "core/snapshot/snapshot.hpp"

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

constexpr std::uint64_t kTicksPerSecond = 6'250'000;
constexpr std::uint8_t kSid = 0xE0;

freetvc::MachineConfig config_with_sid(freetvc::SidModel model = freetvc::SidModel::Mos6581) {
    freetvc::MachineConfig config;
    config.sid.model = model;
    return config;
}

// A budget shorter than the next instruction runs nothing, so the last few ticks may stay undone
void run_ticks(freetvc::Machine& m, std::uint64_t ticks, std::uint64_t slice) {
    for (std::uint64_t done = 0; done < ticks;) {
        const std::uint64_t ran = m.run({.max_ticks = std::min(ticks - done, slice)}).ticks_run;
        if (ran == 0) {
            break;
        }
        done += ran;
    }
}

std::vector<float> drain(freetvc::Machine& m) {
    std::vector<float> out(m.audio_available());
    m.read_audio(out);
    return out;
}

// Runs `seconds` in `slice`-tick runs and returns the audio
std::vector<float> listen(freetvc::Machine& m, double seconds, std::uint64_t slice = kTicksPerSecond / 50) {
    std::vector<float> all;
    const auto total = static_cast<std::uint64_t>(seconds * kTicksPerSecond);
    for (std::uint64_t done = 0; done < total;) {
        const std::uint64_t ran = m.run({.max_ticks = std::min(total - done, slice)}).ticks_run;
        if (ran == 0) {
            break;
        }
        done += ran;
        const std::vector<float> chunk = drain(m);
        all.insert(all.end(), chunk.begin(), chunk.end());
    }
    return all;
}

double rising_crossings_per_second(const std::vector<float>& s, unsigned rate) {
    float lo = 0.0f;
    float hi = 0.0f;
    for (const float v : s) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    const float mid = (lo + hi) / 2;
    std::size_t count = 0;
    for (std::size_t i = 1; i < s.size(); ++i) {
        count += s[i - 1] < mid && s[i] >= mid;
    }
    return static_cast<double>(count) * rate / static_cast<double>(s.size());
}

float peak_to_peak(const std::vector<float>& s) {
    if (s.empty()) {
        return 0.0f;
    }
    const auto [lo, hi] = std::minmax_element(s.begin(), s.end());
    return *hi - *lo;
}

// Voice 1: a sawtooth of `freq` (16-bit register value), attack 0, sustain 15, master volume 15
void play_saw(freetvc::Machine& m, unsigned freq) {
    m.io_write(kSid + 0x00, static_cast<std::uint8_t>(freq));
    m.io_write(kSid + 0x01, static_cast<std::uint8_t>(freq >> 8));
    m.io_write(kSid + 0x05, 0x00);
    m.io_write(kSid + 0x06, 0xF0);
    m.io_write(kSid + 0x18, 0x0F);
    m.io_write(kSid + 0x04, 0x21);
}

void test_absent() {
    freetvc::Machine m;
    check(m.sid_card() == nullptr, "no SID card by default");
    check(m.io_read(0xE0) == 0xFF && m.io_read(0xFB) == 0xFF, "E0h-FFh read FFh without the card");
    m.io_write(0xF8, 0x0F);
    check(m.io_read(0xF8) == 0xFF, "writes go nowhere without the card");
}

void test_ports_and_reads() {
    freetvc::Machine m(config_with_sid());
    check(m.sid_card() != nullptr, "the SID card is in");
    // Voice 3 noise: OSC3 changes; the write-only registers read back the data bus, which fades (6581: 1D00h clocks)
    m.io_write(kSid + 0x0E, 0x00);
    m.io_write(kSid + 0x0F, 0x80);
    m.io_write(kSid + 0x12, 0x80);
    std::vector<std::uint8_t> osc;
    for (int i = 0; i < 16; ++i) {
        run_ticks(m, 1000, 1000);
        osc.push_back(m.io_read(kSid + 0x1B));
    }
    std::sort(osc.begin(), osc.end());
    check(std::unique(osc.begin(), osc.end()) - osc.begin() > 4, "OSC3 follows voice 3's noise");
    m.io_write(kSid + 0x05, 0x5A);
    check(m.io_read(kSid + 0x05) == 0x5A, "a write-only register reads the bus value just written");
    run_ticks(m, kTicksPerSecond / 50, kTicksPerSecond / 50);
    check(m.io_read(kSid + 0x05) == 0x00, "the bus value has faded 20 ms later");
    check(m.sid_card()->peek(0x05) == 0x5A, "peek shows the register");
    // The port's A4-A0 reach the chip: FFh is register 1Fh, which reads the bus
    m.io_write(kSid + 0x1F, 0x33);
    check(m.io_read(0xFF) == 0x33, "1Fh reads the bus value");

    // ENV3: voice 3 gated with attack 2 ms rises to FFh; release 0 drops it
    m.io_write(kSid + 0x13, 0x00);
    m.io_write(kSid + 0x14, 0xF0);
    m.io_write(kSid + 0x12, 0x11);
    run_ticks(m, kTicksPerSecond / 100, 5000);
    check(m.io_read(kSid + 0x1C) == 0xFF, "ENV3 reaches FFh after the attack");
    m.io_write(kSid + 0x12, 0x10);
    run_ticks(m, kTicksPerSecond / 20, 5000);
    check(m.io_read(kSid + 0x1C) == 0x00, "ENV3 falls to 0 after the release");
}

void test_pitch() {
    // F = 7493 (a PAL C64's A4 = 440 Hz)
    for (const auto model : {freetvc::SidModel::Mos6581, freetvc::SidModel::Mos8580}) {
        freetvc::Machine m(config_with_sid(model));
        play_saw(m, 7493);
        const std::vector<float> s = listen(m, 1.0);
        const std::vector<float> tail(s.begin() + static_cast<std::ptrdiff_t>(s.size() / 4), s.end());
        const double f = rising_crossings_per_second(tail, m.audio_rate());
        const std::string name = model == freetvc::SidModel::Mos8580 ? "8580" : "6581";
        check(std::abs(f - 440.0) < 2.0, name + ": sawtooth at 440 Hz (" + std::to_string(f) + ")");
        check(peak_to_peak(tail) > 0.1f && peak_to_peak(tail) < 1.5f, name + ": audible and in range (" + std::to_string(peak_to_peak(tail)) + ")");
    }
}

void test_copy_and_slicing() {
    freetvc::Machine a(config_with_sid());
    play_saw(a, 5000);
    a.io_write(kSid + 0x17, 0xF1);  // voice 1 through the filter, resonance 15
    a.io_write(kSid + 0x16, 0x40);
    a.io_write(kSid + 0x18, 0x1F);  // low-pass
    run_ticks(a, kTicksPerSecond / 10, kTicksPerSecond / 50);
    drain(a);
    freetvc::Machine b(a);
    freetvc::Machine c(a);
    const std::vector<float> sa = listen(a, 0.5);
    const std::vector<float> sb = listen(b, 0.5);
    const std::vector<float> sc = listen(c, 0.5, 777);
    check(!sa.empty() && sa == sb, "a copied machine plays the same samples");
    check(sa == sc, "the samples do not depend on how the run is sliced");
}

void test_snapshot() {
    freetvc::Machine a(config_with_sid(freetvc::SidModel::Mos8580));
    play_saw(a, 3000);
    a.io_write(kSid + 0x0E, 0x34);
    a.io_write(kSid + 0x0F, 0x12);
    a.io_write(kSid + 0x12, 0x41);
    a.io_write(kSid + 0x13, 0x22);
    a.io_write(kSid + 0x14, 0xA4);
    a.io_write(kSid + 0x17, 0x24);
    a.io_write(kSid + 0x16, 0x80);
    run_ticks(a, kTicksPerSecond / 7, 12345);
    a.io_write(kSid + 0x11, 0x99);  // a write just before the snapshot: the bus value and the MOS8580's pipeline
    freetvc::Snapshot snap;
    a.save_state(snap);
    drain(a);

    std::string error;
    const auto needs = freetvc::snapshot_needs(snap, error);
    check(needs.has_value() && needs->sid.model == freetvc::SidModel::Mos8580, "the snapshot names the SID card");
    check(!freetvc::snapshot_matches(freetvc::MachineConfig{}, *needs), "a machine without the card does not match");
    freetvc::Machine none;
    check(!none.load_state(snap, error), "loading into a machine without the card fails");

    freetvc::Machine b(config_with_sid(freetvc::SidModel::Mos8580));
    check(b.load_state(snap, error), "the snapshot loads: " + error);
    std::vector<std::uint8_t> ra;
    std::vector<std::uint8_t> rb;
    for (int i = 0; i < 20; ++i) {
        run_ticks(a, 3001, 3001);
        run_ticks(b, 3001, 3001);
        ra.push_back(a.io_read(kSid + 0x1B));
        ra.push_back(a.io_read(kSid + 0x1C));
        rb.push_back(b.io_read(kSid + 0x1B));
        rb.push_back(b.io_read(kSid + 0x1C));
    }
    check(ra == rb, "OSC3 and ENV3 continue alike after a snapshot");
    const std::vector<float> sa = listen(a, 0.5);
    const std::vector<float> sb = listen(b, 0.5);
    check(!sa.empty() && sa == sb, "the samples continue alike after a snapshot");

    freetvc::Snapshot again;
    b.save_state(again);
    freetvc::Snapshot first;
    a.save_state(first);
    check(again.encode() == first.encode(), "both machines save the same snapshot");
}

void test_reset() {
    freetvc::Machine m(config_with_sid());
    play_saw(m, 7493);
    listen(m, 0.2);
    m.reset();
    check(m.sid_card()->peek(0x18) == 0x00 && m.sid_card()->peek(0x04) == 0x00, "reset clears the registers");
    const std::vector<float> s = listen(m, 0.5);
    const std::vector<float> tail(s.begin() + static_cast<std::ptrdiff_t>(s.size() / 2), s.end());
    check(peak_to_peak(tail) < 0.01f, "silent after a reset (" + std::to_string(peak_to_peak(tail)) + ")");
}

}  // namespace

int main() {
    test_absent();
    test_ports_and_reads();
    test_pitch();
    test_copy_and_slicing();
    test_snapshot();
    test_reset();
    if (g_failures == 0) {
        std::printf("sid card: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
