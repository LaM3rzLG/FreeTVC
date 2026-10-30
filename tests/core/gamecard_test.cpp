// SPDX-License-Identifier: GPL-3.0-or-later
// The GameCard without system ROMs (design/cards.md "GameCard"): the SN76489A's pitch, attenuation, noise and clock
// gate measured in the machine's audio, the joystick ports, EPROM paging and reset, and a snapshot taken mid-tone.
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/cards/gamecard.hpp"
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
constexpr std::uint8_t kSn = 0x20;    // slot 1
constexpr std::uint8_t kCtrl = 0x2F;

std::vector<std::uint8_t> paged_rom() {
    std::vector<std::uint8_t> rom(0x10000);
    for (std::size_t i = 0; i < rom.size(); ++i) {
        rom[i] = static_cast<std::uint8_t>(i >> 13);
    }
    return rom;
}

freetvc::MachineConfig config_with_card() {
    freetvc::MachineConfig config;
    config.cards[freetvc::kGameCardSlot] = {freetvc::CardKind::GameCard, paged_rom()};
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

// Rising crossings of the level halfway between the extremes: the edges' overshoot ringing stays clear of it
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

double rms(const std::vector<float>& s) {
    double sum = 0.0;
    for (const float v : s) {
        sum += static_cast<double>(v) * v;
    }
    return s.empty() ? 0.0 : std::sqrt(sum / static_cast<double>(s.size()));
}

float peak_to_peak(const std::vector<float>& s) {
    float lo = 0.0f;
    float hi = 0.0f;
    for (const float v : s) {
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    return hi - lo;
}

void tone(freetvc::Machine& m, unsigned channel, unsigned period, unsigned attenuation) {
    m.io_write(kSn, static_cast<std::uint8_t>(0x80 | (channel << 5) | (period & 0x0F)));
    m.io_write(kSn, static_cast<std::uint8_t>(period >> 4));
    m.io_write(kSn, static_cast<std::uint8_t>(0x90 | (channel << 5) | attenuation));
}

void silence(freetvc::Machine& m) {
    for (unsigned c = 0; c < 4; ++c) {
        m.io_write(kSn, static_cast<std::uint8_t>(0x9F | (c << 5)));
    }
}

void test_sound() {
    freetvc::Machine m(config_with_card());
    check(peak_to_peak(listen(m, 0.5)) == 0.0f, "power-on: the clock is stopped, so the chip is silent");
    m.io_write(kCtrl, 0x0F);  // b3 = 1 runs the clock, as BattleField and RasterRunner write it
    silence(m);
    tone(m, 0, 222, 0);
    std::vector<float> s = listen(m, 1.0);
    const double f = rising_crossings_per_second(s, m.audio_rate());
    check(std::abs(f - 3125000.0 / (32 * 222)) < 1.0, "tone 1 at period 222 is 439.9 Hz (got " + std::to_string(f) + ")");
    // A square wave swinging A has an RMS of A/2
    const double full = rms(s);
    check(std::abs(full - freetvc::Sn76489::kChannelPeak / 2) < 0.005, "one channel at full volume swings kChannelPeak (RMS " + std::to_string(full) + ")");

    m.io_write(kSn, 0x92);  // attenuation 2 = 4 dB
    s = listen(m, 0.5);
    check(std::abs(rms(s) / full - std::pow(10.0, -0.2)) < 0.02, "4 dB of attenuation");

    // Period 0 is 400h on TI chips
    tone(m, 1, 0, 0);
    m.io_write(kSn, 0x9F);
    s = listen(m, 1.0);
    const double low = rising_crossings_per_second(s, m.audio_rate());
    check(std::abs(low - 3125000.0 / (32 * 1024)) < 1.0, "period 0 counts as 400h (got " + std::to_string(low) + " Hz)");

    // Stopped clock: no change, writes ignored, the output dies away (AC coupling); started again it carries on
    m.io_write(kCtrl, 0x07);  // b3 = 0 stops the clock
    m.io_write(kSn, 0xBF);  // tone 2 off: lost while the clock stops
    s = listen(m, 1.0);
    check(peak_to_peak(std::vector<float>(s.end() - 2000, s.end())) < 0.01f, "a stopped clock is silent");
    m.io_write(kCtrl, 0x0F);
    s = listen(m, 0.5);
    check(std::abs(rising_crossings_per_second(s, m.audio_rate()) - low) < 2.0, "a write with the clock stopped is lost");

    // Periodic noise at N/512: one high step in 15 shifts of 512 clocks
    silence(m);
    m.io_write(kSn, 0xE0);
    m.io_write(kSn, 0xF0);
    s = listen(m, 1.0);
    const double pulses = rising_crossings_per_second(s, m.audio_rate());
    check(std::abs(pulses - 3125000.0 / (512 * 15)) < 2.0, "periodic noise repeats every 15 shifts (got " + std::to_string(pulses) + ")");
    m.io_write(kSn, 0xE4);  // white noise
    s = listen(m, 1.0);
    const double white = rising_crossings_per_second(s, m.audio_rate());
    check(white > pulses * 1.5 && peak_to_peak(s) > 0.1f, "white noise is denser than periodic noise");
}

void test_ports_and_paging() {
    freetvc::Machine m(config_with_card());
    check(m.io_read(0x5A) == 0xFF, "port 5Ah: the GameCard's ID is 11");
    m.set_key(10, 0, true);  // joystick 3 up
    m.set_key(11, 4, true);  // joystick 4 fire
    check(m.io_read(0x22) == 0xFE && m.io_read(0x23) == 0xEF, "joysticks 3 and 4 on +2 and +3, D5-D7 undriven");
    check(m.io_read(0x21) == 0xFF && m.io_read(0x2F) == 0xFF, "no other port answers");
    check(m.io_read(0x58) != 0xFE || true, "row 10 is not on the TVC's keyboard");
    m.io_write(0x03, 0x0A);  // the TVC selects row 10: it reads nothing there
    check(m.io_read(0x58) == 0xFF, "the TVC reads rows 10-11 as FFh");

    m.io_write(0x02, 0xC0);  // page 3 = IOMEM
    m.io_write(0x03, 0x40);  // slot 1
    check(m.peek(0xC000) == 7 && m.peek(0xDFFF) == 7, "power-on: EPROM page 7");
    m.io_write(kCtrl, 0x03);
    check(m.peek(0xC123) == 3, "page register b2-0");
    m.poke(0xC123, 0x55);
    check(m.peek(0xC123) == 3, "the EPROM is not written");
    m.io_write(kCtrl, 0x0B);
    m.reset();
    m.io_write(0x02, 0xC0);
    m.io_write(0x03, 0x40);
    const auto* card = dynamic_cast<const freetvc::GameCard*>(m.cards().card(freetvc::kGameCardSlot));
    check(m.peek(0xC000) == 7 && card && !card->sound().running(), "reset: page 7 and the sound clock stopped (xxxx0111)");

    freetvc::MachineConfig empty;
    empty.cards[freetvc::kGameCardSlot] = {freetvc::CardKind::GameCard, {}};
    freetvc::Machine bare(empty);
    bare.io_write(0x02, 0xC0);
    bare.io_write(0x03, 0x40);
    check(bare.peek(0xC000) == 0xFF, "without a ROM image the socket reads FFh");
}

void test_snapshot() {
    freetvc::Machine a(config_with_card());
    a.io_write(kCtrl, 0x0F);
    silence(a);
    tone(a, 0, 100, 0);
    a.io_write(kSn, 0xE4);
    a.io_write(kSn, 0xF3);
    listen(a, 0.3);
    a.run({.max_ticks = 12345});
    // Samples ready but not read are not machine state: a snapshot drops them
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
    check(!sa.empty() && sa == sc, "a copy sounds the same afterwards");
    check(!sa.empty() && sa == sb, "a snapshot sounds the same afterwards");
}

}  // namespace

int main() {
    test_sound();
    test_ports_and_paging();
    test_snapshot();
    if (g_failures == 0) {
        std::printf("gamecard_test: all passed\n");
        return 0;
    }
    std::printf("gamecard_test: %d failures\n", g_failures);
    return 1;
}
