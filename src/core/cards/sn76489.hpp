// SPDX-License-Identifier: GPL-3.0-or-later
// TI SN76489A(N) sound generator (design/cards.md "SN76489"): three tones and a noise source, from TI's manual and the
// variant notes of MAME's sn76496.cpp. Time is the machine's half-T ticks; the chip catches up when touched.
#pragma once

#include <array>
#include <cstdint>

namespace freetvc {

class AudioSynth;
class ChunkWriter;
class ChunkReader;

class Sn76489 {
public:
    static constexpr std::uint32_t kGameCardHz = 3'125'000;  // the bus 3M125; the counters step every 32 ticks
    static constexpr float kChannelPeak = 0.75f / 4;  // all four channels at full volume match the TVC's own tone (❓)

    explicit Sn76489(std::uint32_t clock_hz = kGameCardHz) : clock_hz_(clock_hz) {}

    // Registers 0 as MAME starts the SN76489A (every channel at full volume ❓: real chips start undefined), periods 400h,
    // noise register reset; clock stopped.
    void power_on();
    // Every attenuation at 15 instead (the Multichip, ❓): a running clock would sound the full-volume start
    void mute_all();
    // Catches up to `tick`; every change of the summed output goes to `out`.
    void advance_to(std::uint64_t tick, AudioSynth& out);
    // A byte on the data bus; ignored while the clock is stopped (nothing latches it).
    void write(std::uint8_t value, std::uint64_t tick, AudioSynth& out);
    // The clock input: stopped, the counters hold and the output keeps its level.
    void set_clock(bool running, std::uint64_t tick, AudioSynth& out);
    // The card's reset line, between runs: the chip is up to date already
    void stop_clock() { running_ = false; }
    // A clock that runs from `tick` on, with no level change to catch up (the Multichip's oscillator, at power-on)
    void start_clock(std::uint64_t tick);

    bool running() const { return running_; }
    std::uint16_t period(unsigned channel) const { return channel < 4 ? period_[channel] : 0; }
    std::uint8_t attenuation(unsigned channel) const { return channel < 4 ? static_cast<std::uint8_t>(regs_[channel * 2 + 1] & 15) : 15; }
    std::uint8_t noise_control() const { return static_cast<std::uint8_t>(regs_[6] & 7); }
    float level() const { return level_; }

    void save(ChunkWriter& w) const;
    void load(ChunkReader& r);
    // The step time's fraction of a tick, always 0 at 3.125 MHz (outside save() so GameCard snapshots keep their layout)
    void save_phase(ChunkWriter& w) const;
    void load_phase(ChunkReader& r);

private:
    float output_level() const;
    void emit(std::uint64_t tick, AudioSynth& out);
    // Moves next_step_ on by `steps` steps of 16 clocks
    void add_steps(std::uint64_t steps);

    std::array<std::uint16_t, 8> regs_{};  // tone 1-3 period and attenuation, noise control, noise attenuation
    unsigned latched_ = 0;                 // register the next data byte goes to
    std::array<std::uint16_t, 4> period_{};
    std::array<std::int32_t, 4> count_{};
    std::array<bool, 4> high_{};
    std::uint32_t shift_ = 0x10000;
    bool running_ = false;
    std::uint32_t clock_hz_;
    std::uint64_t next_step_ = 0;
    std::uint32_t next_frac_ = 0;  // next_step_'s fraction, in 1/clock_hz_ ticks
    float level_ = 0.0f;  // the level last given to the synth
};

}  // namespace freetvc
