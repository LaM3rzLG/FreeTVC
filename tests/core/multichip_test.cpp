// SPDX-License-Identifier: GPL-3.0-or-later
// The Multichip sound card without system ROMs (design/cards.md "Multichip"): its three chips, timer, banks, snapshots.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/cards/chip_clock.hpp"
#include "core/cards/multichip.hpp"
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
constexpr std::uint8_t kBase = 0x30;  // slot 2
constexpr std::uint8_t kSn = kBase + 0;
constexpr std::uint8_t kSaaData = kBase + 2;
constexpr std::uint8_t kSaaAddress = kBase + 3;
constexpr std::uint8_t kYmAddress = kBase + 4;
constexpr std::uint8_t kYmData = kBase + 5;
constexpr std::uint8_t kBank = kBase + 6;

std::vector<std::uint8_t> banked_rom() {
    std::vector<std::uint8_t> rom(0x8000);
    for (std::size_t i = 0; i < rom.size(); ++i) {
        rom[i] = static_cast<std::uint8_t>(0xB0 | (i >> 13));
    }
    return rom;
}

freetvc::MachineConfig config_with_card() {
    freetvc::MachineConfig config;
    config.cards[freetvc::kMultichipSlot] = {freetvc::CardKind::Multichip, banked_rom()};
    return config;
}

// Runs `seconds` and returns the audio, the first 0.1 s dropped (the high-pass settling)
std::vector<float> listen(freetvc::Machine& m, double seconds) {
    std::vector<float> all;
    const auto total = static_cast<std::uint64_t>(seconds * kTicksPerSecond);
    for (std::uint64_t done = 0; done < total;) {
        done += m.run({.max_ticks = std::min<std::uint64_t>(total - done, kTicksPerSecond / 50)}).ticks_run;
        std::vector<float> chunk(m.audio_available());
        m.read_audio(chunk);
        all.insert(all.end(), chunk.begin(), chunk.end());
    }
    const std::size_t skip = std::min<std::size_t>(all.size(), m.audio_rate() / 10);
    return {all.begin() + static_cast<std::ptrdiff_t>(skip), all.end()};
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

void saa(freetvc::Machine& m, std::uint8_t reg, std::uint8_t value) {
    m.io_write(kSaaAddress, reg);
    m.io_write(kSaaData, value);
}

void ym(freetvc::Machine& m, std::uint8_t reg, std::uint8_t value) {
    m.io_write(kYmAddress, reg);
    m.io_write(kYmData, value);
}

void test_chip_clock() {
    bool ok = true;
    for (const std::uint64_t hz : {3'579'545ull, 8'000'000ull, 3'125'000ull}) {
        const freetvc::ChipClock clock{hz};
        for (std::uint64_t tick = 0; tick < 200'000; tick += 7) {
            const std::uint64_t c = clock.last_clock_by(tick);
            ok = ok && clock.tick_of(c) <= tick && clock.tick_of(c + 1) > tick;
        }
        const std::uint64_t day = kTicksPerSecond * 86'400 * 30;
        const std::uint64_t c = clock.last_clock_by(day);
        ok = ok && clock.tick_of(c) <= day && clock.tick_of(c + 1) > day;
    }
    check(ok, "chip clocks and ticks convert both ways without drift, also after 30 days");
}

void test_sn() {
    freetvc::Machine m(config_with_card());
    check(peak_to_peak(listen(m, 0.3)) == 0.0f, "power-on: the SN starts muted (❓) and the others are disabled");
    // The card manual's example: N = 3579545 / (32 * 440) = 254
    m.io_write(kSn, 0x80 | (254 & 0x0F));
    m.io_write(kSn, 254 >> 4);
    m.io_write(kSn, 0x90);
    const double f = rising_crossings_per_second(listen(m, 1.0), m.audio_rate());
    check(std::abs(f - 3579545.0 / (32 * 254)) < 1.0, "SN at 3.579545 MHz, period 254 is 440.4 Hz (got " + std::to_string(f) + ")");
    m.io_write(kBase + 1, 0x9F);
    check(peak_to_peak(listen(m, 1.0)) < 0.01f, "the SN answers at +1 too");
    m.io_write(kBase + 8, 0x90);
    check(peak_to_peak(listen(m, 0.3)) > 0.1f, "and at +8 (A3 is not decoded)");
}

void test_saa() {
    freetvc::Machine m(config_with_card());
    saa(m, 0x00, 0xFF);   // channel 0 at 15 on both sides
    saa(m, 0x08, 227);    // SAM manual: A = tone 227, octave 3, 440.141 Hz
    saa(m, 0x10, 0x03);
    saa(m, 0x14, 0x01);
    check(peak_to_peak(listen(m, 0.3)) == 0.0f, "SE is off at power-up");
    saa(m, 0x1C, 0x01);
    std::vector<float> s = listen(m, 1.0);
    double f = rising_crossings_per_second(s, m.audio_rate());
    check(std::abs(f - 15625.0 * 8 / (511 - 227)) < 1.0, "SAA tone 227 octave 3 is 440.14 Hz (got " + std::to_string(f) + ")");
    const float tone_swing = peak_to_peak(s);

    saa(m, 0x10, 0x04);  // octave 4: acts at the end of a half cycle
    f = rising_crossings_per_second(listen(m, 0.5), m.audio_rate());
    check(std::abs(f - 15625.0 * 16 / (511 - 227)) < 2.0, "an octave up doubles it (got " + std::to_string(f) + ")");

    // Sync holds every generator: no output
    saa(m, 0x1C, 0x03);
    s = listen(m, 0.5);
    check(peak_to_peak(std::vector<float>(s.end() - 2000, s.end())) < 0.01f, "sync (RST) holds the generators silent");
    saa(m, 0x1C, 0x01);

    // Noise generator 0 at 31.25 kHz on channel 0: dense, no tone
    saa(m, 0x14, 0x00);
    saa(m, 0x15, 0x01);
    saa(m, 0x16, 0x00);
    s = listen(m, 0.5);
    const double noise_rate = rising_crossings_per_second(s, m.audio_rate());
    check(noise_rate > 2000 && peak_to_peak(s) > 0.5f * tone_swing, "noise at 31.25 kHz (" + std::to_string(noise_rate) + " rises/s)");
    saa(m, 0x15, 0x00);

    // Envelope 0 on channel 2, mixers off, so the envelope itself sounds; clocked by generator 1's half cycles
    saa(m, 0x02, 0xFF);
    saa(m, 0x09, 0);       // generator 1: 15625 / 511 = 30.58 Hz, 61.15 half cycles a second
    saa(m, 0x10, 0x00);
    saa(m, 0x18, 0x86);    // enabled, repetitive decay, 4-bit
    f = rising_crossings_per_second(listen(m, 3.0), m.audio_rate());
    check(std::abs(f - 15625.0 / 511 * 2 / 16) < 0.2, "repetitive decay repeats every 16 envelope clocks (got " + std::to_string(f) + ")");
    saa(m, 0x18, 0x96);    // 3-bit: steps of 2, 8 clocks a ramp (acts at once)
    f = rising_crossings_per_second(listen(m, 3.0), m.audio_rate());
    check(std::abs(f - 15625.0 / 511 * 2 / 8) < 0.3, "3-bit resolution halves the ramp (got " + std::to_string(f) + ")");

    // External clock: every address write of 24 steps it
    saa(m, 0x18, 0x00);
    listen(m, 0.2);
    saa(m, 0x18, 0xA6);    // enabled, repetitive decay, external clock: starts at 15
    const auto* card = dynamic_cast<const freetvc::MultichipCard*>(m.cards().card(freetvc::kMultichipSlot));
    const float before = card ? card->saa().level() : 0.0f;
    m.io_write(kSaaAddress, 0x18);
    m.io_write(kSaaAddress, 0x18);
    listen(m, 0.01);
    const float after = card ? card->saa().level() : 0.0f;
    // Amplitude 15 under envelope 15, then 13: the PDM AND gives (7 * 15 + 1) / 2 = 53, then (7 * 13 + 1) / 2 = 46
    check(card && after < before && std::abs(after - before * 46.0f / 53.0f) < 1e-5f, "two address writes of 24 step the envelope twice");
}

void test_ym() {
    freetvc::Machine m(config_with_card());
    const auto* card = dynamic_cast<const freetvc::MultichipCard*>(m.cards().card(freetvc::kMultichipSlot));
    check(card && card->ym().sample_rate() == 3579545 / 72, "the YM samples at clock / 72");
    // Channel 0: a sine from the carrier, the modulator at full attenuation, added (connection 1)
    ym(m, 0x20, 0x01);
    ym(m, 0x23, 0x21);
    ym(m, 0x40, 0x3F);
    ym(m, 0x43, 0x00);
    ym(m, 0x60, 0xF0);
    ym(m, 0x63, 0xF0);
    ym(m, 0x80, 0x00);
    ym(m, 0x83, 0x00);
    ym(m, 0xC0, 0x01);
    ym(m, 0xA0, 0x44);
    ym(m, 0xB0, 0x32);  // key on, block 4, F-number 580: 580 * 49716 / 2^16 = 440.0 Hz
    std::vector<float> s = listen(m, 1.0);
    const double f = rising_crossings_per_second(s, m.audio_rate());
    check(std::abs(f - 580.0 * (3579545.0 / 72) / 65536) < 1.0, "YM block 4 F-number 580 is 440.0 Hz (got " + std::to_string(f) + ")");
    check(peak_to_peak(s) > 0.05f, "and it is audible");
    ym(m, 0xB0, 0x12);  // key off

    // Timer 1 at 16 counts of 80 us: the status and the slot's /INT line (port 59h b2 = 0 while pulled)
    check((m.io_read(0x59) & 0x04) != 0, "no slot 2 interrupt before the timer");
    check((m.io_read(kYmAddress) & 0xE0) == 0, "status: no flags");
    ym(m, 0x02, 0xF0);
    ym(m, 0x04, 0x01);
    std::uint64_t waited = 0;
    while ((m.io_read(0x59) & 0x04) != 0 && waited < kTicksPerSecond / 10) {
        waited += m.run({.max_ticks = 25}).ticks_run;  // 4 us
    }
    const double us = static_cast<double>(waited) * 1e6 / kTicksPerSecond;
    check(std::abs(us - 16 * 288 * 1e6 / 3579545) < 10, "timer 1 runs out after 16 * 80.46 us (got " + std::to_string(us) + " us)");
    check((m.io_read(kYmAddress) & 0xC0) == 0xC0, "status: IRQ and timer 1 flags");
    check(m.io_read(kYmData) == 0xFF, "reads at A0 = 1 give FFh");
    ym(m, 0x04, 0x80);  // IRQ reset: the line goes up, the running timer pulls it again
    check((m.io_read(0x59) & 0x04) != 0, "IRQ reset releases the line");
    m.run({.max_ticks = kTicksPerSecond / 500});
    check((m.io_read(0x59) & 0x04) == 0, "the timer keeps running and fires again");
    ym(m, 0x04, 0x00);
    ym(m, 0x04, 0x80);
    m.run({.max_ticks = kTicksPerSecond / 100});
    check((m.io_read(0x59) & 0x04) != 0, "a stopped timer stays quiet");

    ym(m, 0x04, 0x01);
    m.run({.max_ticks = kTicksPerSecond / 500});
    m.reset();
    check((m.io_read(0x59) & 0x04) != 0 && (m.io_read(kYmAddress) & 0xE0) == 0, "reset (/IC) clears the timer and the line");
}

void test_ports_and_banks() {
    freetvc::Machine m(config_with_card());
    check(m.io_read(0x5A) == 0xFF, "port 5Ah: IDA/IDB open, 11");
    check(m.io_read(kSn) == 0xFF && m.io_read(kSaaData) == 0xFF && m.io_read(kBank) == 0xFF, "only the YM drives reads");
    m.io_write(0x02, 0xC0);  // page 3 = IOMEM
    m.io_write(0x03, 0x80);  // slot 2
    check(m.peek(0xC000) == 0xB3 && m.peek(0xDFFF) == 0xB3, "power-on: bank 3 (the latch's /PRE)");
    m.io_write(kBank, 0x02);
    check(m.peek(0xC123) == 0xB2, "bank latch b1-0");
    m.io_write(kBase + 7, 0xFD);
    check(m.peek(0xC123) == 0xB1, "at +7 too, upper bits ignored");
    m.poke(0xC123, 0x55);
    check(m.peek(0xC123) == 0xB1, "the EEPROM's /WE is tied high");
    m.reset();
    m.io_write(0x02, 0xC0);
    m.io_write(0x03, 0x80);
    check(m.peek(0xC000) == 0xB3, "reset: bank 3 again");
}

void test_snapshot() {
    freetvc::Machine a(config_with_card());
    a.io_write(kSn, 0x80 | 5);
    a.io_write(kSn, 7);
    a.io_write(kSn, 0x94);
    saa(a, 0x00, 0x9C);
    saa(a, 0x02, 0xFF);
    saa(a, 0x08, 100);
    saa(a, 0x09, 200);
    saa(a, 0x10, 0x34);
    saa(a, 0x14, 0x01);
    saa(a, 0x15, 0x01);
    saa(a, 0x16, 0x03);
    saa(a, 0x18, 0x8A);
    saa(a, 0x1C, 0x01);
    ym(a, 0x23, 0x21);
    ym(a, 0x43, 0x08);
    ym(a, 0x63, 0xF4);
    ym(a, 0x83, 0x25);
    ym(a, 0xA0, 0x98);
    ym(a, 0xB0, 0x2D);
    ym(a, 0x02, 0x80);
    ym(a, 0x04, 0x01);
    listen(a, 0.3);
    a.run({.max_ticks = 12345});
    std::vector<float> pending(a.audio_available());
    a.read_audio(pending);
    freetvc::Snapshot snap;
    a.save_state(snap);
    freetvc::Machine b(config_with_card());
    std::string error;
    check(b.load_state(snap, error), "snapshot loads: " + error);
    freetvc::Machine c(a);
    const std::vector<float> sa = listen(a, 0.5);
    const std::vector<float> sb = listen(b, 0.5);
    const std::vector<float> sc = listen(c, 0.5);
    check(!sa.empty() && peak_to_peak(sa) > 0.05f && sa == sc, "a copy sounds the same afterwards");
    check(!sa.empty() && sa == sb, "a snapshot sounds the same afterwards");
    check(a.io_read(0x59) == b.io_read(0x59) && a.io_read(kYmAddress) == b.io_read(kYmAddress), "and its timer agrees");
}

}  // namespace

int main() {
    test_chip_clock();
    test_sn();
    test_saa();
    test_ym();
    test_ports_and_banks();
    test_snapshot();
    if (g_failures == 0) {
        std::printf("multichip_test: all passed\n");
        return 0;
    }
    std::printf("multichip_test: %d failures\n", g_failures);
    return 1;
}
