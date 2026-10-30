// SPDX-License-Identifier: GPL-3.0-or-later
// The TVC256k++'s vSID without ROMs: the engine's sample pipeline (blocks, the write queue and its overflow, the
// one-block delay), waveforms, the envelope's timing, the read-back registers, and the card on the machine's audio
// (pitch, a warm reset that keeps playing, a reboot that stops, a snapshot mid-note).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/cards/card256.hpp"
#include "core/cards/vsid.hpp"
#include "core/machine/machine.hpp"
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
constexpr double kVsidHz = 336e6 / 341;

// Every computed sample up to `clock`
std::vector<std::uint16_t> run(freetvc::VSid& s, std::uint64_t clock) {
    s.compute_to(clock);
    std::vector<std::uint16_t> out;
    while (s.has_output()) {
        out.push_back(s.output_value());
        s.pop_output();
    }
    return out;
}

// Writes after the engine's first calculation: before it the gate counts as already high ([FW] Gate_previous = 1)
constexpr std::uint64_t kStart = 100;

// Voice 1 playing `control` at `freq`, attack 0, sustain 15, master 15
void play(freetvc::VSid& s, std::uint8_t control, unsigned freq) {
    s.queue_write(0, static_cast<std::uint8_t>(freq), kStart);
    s.queue_write(1, static_cast<std::uint8_t>(freq >> 8), kStart);
    s.queue_write(2, 0x00, kStart);
    s.queue_write(3, 0x08, kStart);  // pulse width 50 %
    s.queue_write(5, 0x00, kStart);
    s.queue_write(6, 0xF0, kStart);
    s.queue_write(24, 0x0F, kStart);
    s.queue_write(4, control, kStart);
}

double rising_per_sample(const std::vector<std::uint16_t>& v) {
    const auto [lo, hi] = std::minmax_element(v.begin(), v.end());
    const double mid = (*lo + *hi) / 2.0;
    std::size_t n = 0;
    for (std::size_t i = 1; i < v.size(); ++i) {
        n += v[i - 1] < mid && v[i] >= mid;
    }
    return static_cast<double>(n) / static_cast<double>(v.size());
}

void test_pipeline() {
    freetvc::VSid s;
    s.power_on();
    check(run(s, freetvc::VSid::kBlockClocks - 1).empty(), "no sample before the first block ends");
    const std::vector<std::uint16_t> first = run(s, freetvc::VSid::kBlockClocks);
    check(first.size() == freetvc::VSid::kBlockSamples, "a block of 128 samples");
    check(std::all_of(first.begin(), first.end(), [](std::uint16_t v) { return v == 100; }), "silence: the PWM's floor of 100");
    check(s.output_clock() == 2 * freetvc::VSid::kBlockClocks, "the next block plays one block after it was computed");
    // A write reads back only when its block is computed
    s.queue_write(24, 0x0F, 3100);
    s.compute_to(2 * freetvc::VSid::kBlockClocks - 1);
    check(s.reg(24) == 0, "not yet applied");
    s.compute_to(2 * freetvc::VSid::kBlockClocks);
    check(s.reg(24) == 0x0F, "applied with its block");
    // 256 pending writes wrap the 8-bit queue: it looks empty and they are lost, as on the card
    freetvc::VSid q;
    q.power_on();
    for (int i = 0; i < 256; ++i) {
        q.queue_write(24, 0x0F, 10);
    }
    q.compute_to(freetvc::VSid::kBlockClocks);
    check(q.reg(24) == 0, "256 pending writes are lost");
}

void test_waveforms() {
    // Sawtooth: freq 4096 wraps the 24-bit accumulator every 4096 clocks = 170.7 samples
    freetvc::VSid s;
    s.power_on();
    play(s, 0x21, 4096);
    std::vector<std::uint16_t> v = run(s, 40 * freetvc::VSid::kBlockClocks);
    v.erase(v.begin(), v.begin() + 512);
    const double period = 1.0 / rising_per_sample(v);
    check(std::abs(period - 4096.0 / 24) < 1.0, "sawtooth period 4096 clocks: " + std::to_string(period) + " samples");
    const auto [lo, hi] = std::minmax_element(v.begin(), v.end());
    // One voice of three at full volume: the mean (4095 x 255) / 3 x 15 >> 8 = 20380, over the floor of 100
    check(*hi > 20000 && *hi < 20600 && *lo < 300, "one voice's full sawtooth swing: " + std::to_string(*lo) + " to " + std::to_string(*hi));
    // Pulse at 50 %: half the samples high
    freetvc::VSid p;
    p.power_on();
    play(p, 0x41, 4096);
    std::vector<std::uint16_t> w = run(p, 40 * freetvc::VSid::kBlockClocks);
    w.erase(w.begin(), w.begin() + 512);
    const auto [plo, phi] = std::minmax_element(w.begin(), w.end());
    const double high = static_cast<double>(std::count_if(w.begin(), w.end(), [&](std::uint16_t x) { return x > (*plo + *phi) / 2; }));
    check(std::abs(high / static_cast<double>(w.size()) - 0.5) < 0.02, "pulse width 50 %");
    // Test bit: the accumulator holds at 0, so the output stops moving
    freetvc::VSid t;
    t.power_on();
    play(t, 0x29, 4096);
    std::vector<std::uint16_t> u = run(t, 20 * freetvc::VSid::kBlockClocks);
    check(std::all_of(u.end() - 200, u.end(), [&](std::uint16_t x) { return x == u.back(); }), "the test bit holds the output");
    // Voice 3's oscillator and envelope read back at 27 and 28
    freetvc::VSid r;
    r.power_on();
    r.queue_write(14, 0x00, kStart);
    r.queue_write(15, 0x10, kStart);
    r.queue_write(19, 0x00, kStart);
    r.queue_write(20, 0xF0, kStart);
    r.queue_write(18, 0x21, kStart);
    r.compute_to(freetvc::VSid::kBlockClocks);
    check(r.reg(28) == 0xFF, "ENV3 at the top after an attack of rate 0 (about 2 ms)");
    check(r.reg(27) != 0, "OSC3 moves");
}

void test_envelope() {
    // Attack rate 0 takes 8 clocks a step: 255 steps in about 2040 clocks = 85 samples
    freetvc::VSid s;
    s.power_on();
    s.queue_write(14, 0x00, kStart);
    s.queue_write(15, 0x10, kStart);
    s.queue_write(19, 0x90, kStart);  // attack 9: 976 clocks a step, about 250 ms
    s.queue_write(20, 0xF0, kStart);
    s.queue_write(18, 0x21, kStart);
    unsigned blocks = 0;
    while (s.reg(28) < 0xFF && blocks < 200) {
        ++blocks;
        s.compute_to(blocks * freetvc::VSid::kBlockClocks);
    }
    const double ms = blocks * static_cast<double>(freetvc::VSid::kBlockClocks) / kVsidHz * 1000;
    check(ms > 240 && ms < 262, "attack 9 reaches the top in about 250 ms: " + std::to_string(ms));
    // Gate off with release 0: back to 0 within a few ms
    s.queue_write(18, 0x20, blocks * freetvc::VSid::kBlockClocks + 10);
    s.compute_to((blocks + 4) * freetvc::VSid::kBlockClocks);
    check(s.reg(28) == 0, "release 0 empties the envelope (about 6 ms)");
}

freetvc::MachineConfig config() {
    freetvc::MachineConfig c;
    c.model = freetvc::Model::Tvc64kPlus;
    c.roms.sys = {0xF3, 0x76};
    c.cards[0] = {freetvc::CardKind::Card256, std::vector<std::uint8_t>(freetvc::Card256::kRomSize, 0xFF)};
    return c;
}

void set(freetvc::Machine& m, std::uint8_t index, std::uint8_t value) {
    m.io_write(0x10, index);
    m.io_write(0x11, value);
}

std::vector<float> listen(freetvc::Machine& m, double seconds) {
    std::vector<float> all;
    const auto total = static_cast<std::uint64_t>(seconds * kTicksPerSecond);
    for (std::uint64_t done = 0; done < total;) {
        done += m.run({.max_ticks = std::min<std::uint64_t>(total - done, kTicksPerSecond / 50)}).ticks_run;
        std::vector<float> chunk(m.audio_available());
        m.read_audio(chunk);
        all.insert(all.end(), chunk.begin(), chunk.end());
    }
    return all;
}

double frequency(const std::vector<float>& s, unsigned rate) {
    const auto [lo, hi] = std::minmax_element(s.begin(), s.end());
    const float mid = (*lo + *hi) / 2;
    std::size_t n = 0;
    for (std::size_t i = 1; i < s.size(); ++i) {
        n += s[i - 1] < mid && s[i] >= mid;
    }
    return static_cast<double>(n) * rate / static_cast<double>(s.size());
}

float peak(const std::vector<float>& s) {
    float p = 0;
    for (const float v : s) {
        p = std::max(p, std::abs(v));
    }
    return p;
}

// A440 on voice 1: freq = 440 * 2^24 / 985337 = 7491
void note(freetvc::Machine& m) {
    for (const auto& [reg, value] : std::vector<std::pair<std::uint8_t, std::uint8_t>>{
             {0, 7491 & 0xFF}, {1, 7491 >> 8}, {3, 0x08}, {5, 0x00}, {6, 0xF0}, {24, 0x0F}, {4, 0x41}}) {
        set(m, static_cast<std::uint8_t>(0xE0 + reg), value);
    }
}

void test_machine() {
    freetvc::Machine m(config());
    check(peak(listen(m, 0.3)) < 0.01, "silent at power-on");
    note(m);
    std::vector<float> a = listen(m, 0.5);
    a.erase(a.begin(), a.begin() + m.audio_rate() / 10);
    const double f = frequency(a, m.audio_rate());
    check(std::abs(f - 440.0) < 2.0, "A440 on the bus SOUND line: " + std::to_string(f) + " Hz");
    check(peak(a) > 0.05 && peak(a) < 0.5, "a sensible level: " + std::to_string(peak(a)));

    // A snapshot mid-note plays on identically
    freetvc::Snapshot snap;
    m.save_state(snap);
    std::vector<float> after = listen(m, 0.2);
    freetvc::Machine b(config());
    std::string error;
    check(b.load_state(snap, error), "snapshot loads: " + error);
    std::vector<float> again = listen(b, 0.2);
    check(after.size() == again.size() && std::equal(after.begin(), after.end(), again.begin()), "the snapshot plays on identically");

    // The reset button: register 24 reads 0, the engine keeps its volume ([FW] reset_handler)
    m.run({.max_ticks = 2 * kTicksPerSecond});
    m.reset();
    std::vector<float> reset = listen(m, 0.3);
    reset.erase(reset.begin(), reset.begin() + m.audio_rate() / 10);
    check(std::abs(frequency(reset, m.audio_rate()) - 440.0) < 2.0, "a warm reset leaves the note playing");
    m.io_write(0x10, 0xF8);
    check(m.io_read(0x11) == 0, "but register 24 reads 0");
    // A second press within a second reboots the MCU: the output stops
    m.reset();
    std::vector<float> reboot = listen(m, 0.5);
    reboot.erase(reboot.begin(), reboot.begin() + m.audio_rate() / 5);
    check(peak(reboot) < 0.01, "a reboot silences the vSID: " + std::to_string(peak(reboot)));
}

}  // namespace

int main() {
    test_pipeline();
    test_waveforms();
    test_envelope();
    test_machine();
    if (g_failures == 0) {
        std::printf("card256_vsid_test: all passed\n");
        return 0;
    }
    std::printf("card256_vsid_test: %d failure(s)\n", g_failures);
    return 1;
}
