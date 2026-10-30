// SPDX-License-Identifier: GPL-3.0-or-later
// Tone generator, sound IRQ and band-limited output; expected values from tvc-hardware.md §6 and design/machine.md "Sound" (no ROM images needed).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "core/audio/sound.hpp"
#include "core/audio/synth.hpp"
#include "core/machine/machine.hpp"

namespace {

int g_failures = 0;

void check(bool condition, const char* what, double got, double expected) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s: got %.9g, expected %.9g\n", what, got, expected);
    }
}

// Each argument is evaluated once: several are calls with side effects
void check_eq(double got, double expected, const char* what) { check(got == expected, what, got, expected); }
void check_near(double got, double expected, double tolerance, const char* what) {
    check(std::abs(got - expected) <= tolerance, what, got, expected);
}

#define CHECK_EQ(got, expected) check_eq(static_cast<double>(got), static_cast<double>(expected), #got)
#define CHECK_NEAR(got, expected, tolerance) \
    check_near(static_cast<double>(got), static_cast<double>(expected), static_cast<double>(tolerance), #got)

using freetvc::AudioSynth;
using freetvc::Sound;

// Half-T ticks per counter reload for pitch P
constexpr std::uint64_t reload_ticks(unsigned pitch) { return 2ull * (0x1000 - pitch); }

void irq_after_instart() {
    // [OS]: the IRQ on b3's 1->0 edge, so the first one on the 16th reload after INSTART, then one per 16 reloads
    AudioSynth out;
    Sound s;
    s.power_on();
    s.set_pitch_low(0x00, 990, out);
    s.set_control(0x2F, 1000, out);  // P = F00h, IRQ on, tone off
    s.instart(1001, out);           // the counter starts on the next 3.125 MHz edge, tick 1002
    const std::uint64_t first = 1002 + 16 * reload_ticks(0xF00);
    CHECK_EQ(s.advance_to(first - 1, out), 0u);
    CHECK_EQ(s.chain(), 15u);
    CHECK_EQ(s.advance_to(first, out), 1u);
    CHECK_EQ(s.chain(), 0u);
    const std::uint64_t second = first + 16 * reload_ticks(0xF00);
    CHECK_EQ(s.advance_to(second - 1, out), 0u);
    CHECK_EQ(s.advance_to(second, out), 1u);
    CHECK_EQ(s.advance_to(second + 160 * reload_ticks(0xF00), out), 10u);
}

void irq_gated_and_stopped() {
    AudioSynth out;
    Sound s;
    s.power_on();
    s.set_control(0x1F, 0, out);  // P = F00h, tone on, IRQ off
    s.instart(0, out);
    CHECK_EQ(s.advance_to(100 * 16 * reload_ticks(0xF00), out), 0u);
    // Enabling while b3 is high gives no IRQ at once ❓; its next 1->0 edge does
    constexpr std::uint64_t base = 1'000'000;
    s.instart(base, out);
    s.advance_to(base + 8 * reload_ticks(0xF00), out);
    CHECK_EQ(s.chain(), 8u);
    s.set_control(0x2F, base + 8 * reload_ticks(0xF00), out);
    CHECK_EQ(s.advance_to(base + 16 * reload_ticks(0xF00) - 1, out), 0u);
    CHECK_EQ(s.advance_to(base + 16 * reload_ticks(0xF00), out), 1u);
    // P = 4095 stops the oscillator: no reloads, no IRQ, the chain keeps its state
    s.set_pitch_low(0xFF, base + 30 * reload_ticks(0xF00), out);
    CHECK_EQ(s.pitch(), 0xFFFu);
    CHECK_EQ(s.next_reload(), Sound::kNever);
    const unsigned chain = s.chain();
    CHECK_EQ(s.advance_to(100'000'000, out), 0u);
    CHECK_EQ(s.chain(), chain);
    s.instart(100'000'001, out);
    CHECK_EQ(s.next_reload(), Sound::kNever);
    // Leaving 4095 restarts the count from the new value ❓
    s.set_pitch_low(0x00, 100'000'003, out);
    CHECK_EQ(s.next_reload(), 100'000'004 + reload_ticks(0xF00));
}

void pitch_change_takes_effect_at_reload() {
    AudioSynth out;
    Sound s;
    s.power_on();
    s.set_control(0x0F, 0, out);
    s.instart(0, out);
    CHECK_EQ(s.next_reload(), reload_ticks(0xF00));
    s.set_control(0x0E, 100, out);  // P = E00h while counting from F00h
    CHECK_EQ(s.next_reload(), reload_ticks(0xF00));
    s.advance_to(reload_ticks(0xF00), out);
    CHECK_EQ(s.next_reload(), reload_ticks(0xF00) + reload_ticks(0xE00));
    CHECK_EQ(s.chain(), 1u);
}

void power_on_counts_from_zero() {
    AudioSynth out;
    Sound s;
    s.power_on();
    CHECK_EQ(s.next_reload(), reload_ticks(0));
    CHECK_EQ(s.advance_to(16 * reload_ticks(0) - 1, out), 0u);
    CHECK_EQ(s.chain(), 15u);
}

void output_levels() {
    AudioSynth out;
    Sound s;
    s.power_on();
    s.set_amplitude(0x3C, 0, out);  // 06h b5-2 = 15; b1-0 (video mode) ignored
    CHECK_EQ(s.amplitude(), 15u);
    CHECK_EQ(s.level(), 15u);       // tone gate off: DC at the amplitude (4-bit DAC)
    s.set_amplitude(0x1B, 0, out);  // b5-2 = 6
    CHECK_EQ(s.level(), 6u);
    s.set_control(0x1F, 0, out);  // tone on
    s.instart(0, out);
    CHECK_EQ(s.level(), 0u);  // chain b3 low
    s.advance_to(8 * reload_ticks(0xF00), out);
    CHECK_EQ(s.level(), 6u);
    s.advance_to(16 * reload_ticks(0xF00), out);
    CHECK_EQ(s.level(), 0u);
}

std::vector<float> drain(AudioSynth& synth) {
    std::vector<float> samples(synth.available());
    synth.read(samples);
    return samples;
}

void synth_step_and_dc() {
    AudioSynth synth;
    synth.configure(48000, 0.0f, 1'000'000);
    // Tick 15625 x k is exactly sample 120 x k; a step there reaches half its height (kTaps - 1) / 2 samples later
    synth.set_level(15625 * 10, 0.5f);
    synth.end_at(AudioSynth::kTickRate / 10);
    const std::vector<float> s = drain(synth);
    CHECK_EQ(s.size(), 4800u);
    const std::size_t centre = 1200 + AudioSynth::kTaps / 2 - 1;
    CHECK_NEAR(s[centre], 0.25f, 1e-5f);
    CHECK_NEAR(s[1199], 0.0f, 1e-7f);
    CHECK_NEAR(s[1200 + AudioSynth::kTaps - 1], 0.5f, 1e-5f);
    float worst = 0.0f;
    for (std::size_t i = 1200 + AudioSynth::kTaps - 1; i < s.size(); ++i) {
        worst = std::max(worst, std::abs(s[i] - 0.5f));
    }
    CHECK_NEAR(worst, 0.0f, 1e-5f);
}

void synth_square_keeps_area_and_period() {
    // 1337.76 Hz square (P = F6Eh): mean = half the level, rising crossings every 35.881 samples
    AudioSynth synth;
    synth.configure(48000, 0.0f, 1'000'000);
    const std::uint64_t half = 8 * reload_ticks(0xF6E);
    for (std::uint64_t t = 0, k = 0; t < AudioSynth::kTickRate; t += half, ++k) {
        synth.set_level(t, (k & 1) ? 0.0f : 0.6f);
    }
    synth.end_at(AudioSynth::kTickRate);
    const std::vector<float> s = drain(synth);
    double sum = 0.0;
    std::vector<double> rising;
    for (std::size_t i = 1000; i < 47000; ++i) {
        sum += s[i];
        if (s[i - 1] < 0.3f && s[i] >= 0.3f) {
            rising.push_back(static_cast<double>(i - 1) + (0.3 - s[i - 1]) / (s[i] - s[i - 1]));
        }
    }
    CHECK_NEAR(sum / 46000.0, 0.3, 0.002);
    const double period = (rising.back() - rising.front()) / static_cast<double>(rising.size() - 1);
    CHECK_NEAR(period, 16.0 * 146.0 / 3.125e6 * 48000.0, 0.001);
    float peak = 0.0f;
    for (const float v : s) {
        peak = std::max(peak, std::abs(v));
    }
    CHECK_NEAR(peak, 0.6f * 1.12f, 0.6f * 0.02f);  // band-limited edges ring near the cutoff: about 11 % at the sample points
}

void synth_highpass_and_cap() {
    AudioSynth synth;
    synth.configure(48000, 20.0f, 4800);
    synth.set_level(0, 0.75f);
    synth.end_at(AudioSynth::kTickRate / 2);
    CHECK_EQ(synth.available(), 4800u);
    CHECK_EQ(synth.dropped(), 24000u - 4800u);
    const std::vector<float> s = drain(synth);
    CHECK_NEAR(s.back(), 0.0f, 0.001f);  // 20 Hz: time constant 8 ms, so 0.5 s leaves nothing
    CHECK_EQ(synth.available(), 0u);
}

void synth_chunking_is_invisible() {
    AudioSynth whole;
    AudioSynth pieces;
    whole.configure(48000, 20.0f, 1'000'000);
    pieces.configure(48000, 20.0f, 1'000'000);
    std::vector<float> a;
    std::vector<float> b;
    std::uint64_t t = 0;
    for (unsigned k = 0; k < 2000; ++k, t += 1237 + (k * 7919) % 3001) {
        whole.set_level(t, static_cast<float>(k % 16) / 20.0f);
        pieces.set_level(t, static_cast<float>(k % 16) / 20.0f);
        if (k % 3 == 0) {
            pieces.end_at(t);
            const std::vector<float> part = drain(pieces);
            b.insert(b.end(), part.begin(), part.end());
        }
    }
    whole.end_at(t);
    pieces.end_at(t);
    a = drain(whole);
    const std::vector<float> rest = drain(pieces);
    b.insert(b.end(), rest.begin(), rest.end());
    CHECK_EQ(a.size(), b.size());
    CHECK_EQ(static_cast<unsigned>(a == b), 1u);
}

// SYS ROM program at 0000: DI; LD A,00; OUT (04),A; LD A,2Fh; OUT (05),A; OUT (07),A; IN A,(5Bh); HALT.
// OUT (07) clears the cursor pulse that the unprogrammed 6845 gives right after power-on.
void machine_sound_irq() {
    freetvc::MachineConfig config;
    config.roms.sys = {0xF3, 0x3E, 0x00, 0xD3, 0x04, 0x3E, 0x2F, 0xD3, 0x05, 0xD3, 0x07, 0xDB, 0x5B, 0x76};
    config.roms.sys.resize(0x4000, 0xFF);
    freetvc::Machine m(config);
    m.run({.max_ticks = 1'000'000, .max_instructions = 6});
    const std::uint64_t before_in = m.ticks();
    m.run({.max_ticks = 1'000'000, .max_instructions = 1});  // IN A,(5Bh): 11 T, the I/O read in its last 4
    const std::uint64_t after_in = m.ticks();
    CHECK_EQ((after_in - before_in) / freetvc::kTicksPerTState, 11u);
    CHECK_EQ(m.sound().pitch(), 0xF00u);
    CHECK_EQ(m.sound().chain(), 0u);
    CHECK_EQ(static_cast<unsigned>(m.irq_pending()), 0u);
    // First IRQ 16 x 256 T after INSTART, which fell inside the IN's I/O cycle
    m.run({.max_ticks = 16 * reload_ticks(0xF00) - 8 * freetvc::kTicksPerTState});
    CHECK_EQ(static_cast<unsigned>(m.irq_pending()), 0u);
    m.run({.max_ticks = 8 * freetvc::kTicksPerTState});
    CHECK_EQ(static_cast<unsigned>(m.irq_pending()), 1u);
    // Any write to 07h clears the shared flip-flop; the next IRQ is one tone period later
    m.io_write(0x07, 0);
    m.run({.max_ticks = 16 * reload_ticks(0xF00) - 2 * reload_ticks(0xF00)});
    CHECK_EQ(static_cast<unsigned>(m.irq_pending()), 0u);
    m.run({.max_ticks = 2 * reload_ticks(0xF00)});
    CHECK_EQ(static_cast<unsigned>(m.irq_pending()), 1u);
    // 5Fh mirrors 5Bh; reset clears 05h, so tone and IRQ are off
    m.io_read(0x5F);
    CHECK_EQ(m.sound().chain(), 0u);
    m.reset();
    CHECK_EQ(static_cast<unsigned>(m.sound().irq_enabled()), 0u);
    CHECK_EQ(static_cast<unsigned>(m.sound().tone_enabled()), 0u);
}

void machine_audio_out() {
    // DAC: with the tone gate off, 06h b5-2 is the output level; a step shows up after the high-pass as a decaying pulse
    freetvc::MachineConfig config;
    config.roms.sys = {0x3E, 0x3C, 0xD3, 0x06, 0x76};  // LD A,3Ch; OUT (06),A; HALT
    config.roms.sys.resize(0x4000, 0xFF);
    freetvc::Machine m(config);
    m.run({.max_ticks = AudioSynth::kTickRate / 10});
    CHECK_EQ(m.audio_rate(), 48000u);
    CHECK_EQ(m.sound().level(), 15u);
    std::vector<float> s(m.audio_available());
    CHECK_EQ(s.size(), 4800u);
    m.read_audio(s);
    const float peak = *std::max_element(s.begin(), s.end());
    CHECK_NEAR(peak, 0.75f, 0.05f);
    CHECK_NEAR(s.back(), 0.0f, 0.01f);
    // audio_rate 0: no samples at all
    config.audio_rate = 0;
    freetvc::Machine quiet(config);
    quiet.run({.max_ticks = AudioSynth::kTickRate / 10});
    CHECK_EQ(quiet.audio_available(), 0u);
}

}  // namespace

int main() {
    irq_after_instart();
    irq_gated_and_stopped();
    pitch_change_takes_effect_at_reload();
    power_on_counts_from_zero();
    output_levels();
    synth_step_and_dc();
    synth_square_keeps_area_and_period();
    synth_highpass_and_cap();
    synth_chunking_is_invisible();
    machine_sound_irq();
    machine_audio_out();
    if (g_failures != 0) {
        std::printf("%d check(s) failed\n", g_failures);
        return 1;
    }
    std::printf("sound: all checks passed\n");
    return 0;
}
