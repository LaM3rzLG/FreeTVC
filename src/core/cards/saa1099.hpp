// SPDX-License-Identifier: GPL-3.0-or-later
// Philips SAA1099 at 8 MHz, left and right summed (design/cards.md "SAA1099").
#pragma once

#include <array>
#include <cstdint>

#include "core/cards/chip_clock.hpp"

namespace freetvc {

class AudioSynth;
class ChunkWriter;
class ChunkReader;

class Saa1099 {
public:
    static constexpr ChipClock kClock{8'000'000};
    // One channel at amplitude 15 on one side, tone high, without an envelope (SAASound's 480 units)
    static constexpr unsigned kChannelFull = 480;

    // Every register 0, sound disabled (data sheet: SE resets to 0 at power-up), envelopes off, tones high, noise all ones
    void power_on();
    // Catches up to `tick`; every change of the summed output goes to `out`, set_scale() per unit
    void advance_to(std::uint64_t tick, AudioSynth& out);
    // A0 = 1: the register address (24 and 25 also clock an envelope set to the external clock); A0 = 0: data
    void write_address(std::uint8_t value, std::uint64_t tick, AudioSynth& out);
    void write_data(std::uint8_t value, std::uint64_t tick, AudioSynth& out);
    void set_scale(float scale) { scale_ = scale; }

    bool enabled() const { return enabled_; }
    std::uint8_t amplitude(unsigned channel) const { return channel < 6 ? amp_[channel] : 0; }
    std::uint8_t tone_mix() const { return tone_mix_; }
    std::uint8_t noise_mix() const { return noise_mix_; }
    float level() const { return level_; }

    void save(ChunkWriter& w) const;
    void load(ChunkReader& r);

private:
    struct Tone {
        std::uint8_t offset = 0;
        std::uint8_t octave = 0;
        std::uint8_t next_offset = 0;
        std::uint8_t next_octave = 0;
        bool pending = false;  // new offset or octave waits for the end of the half cycle
        bool high = true;
        std::uint64_t next_toggle = 0;  // in chip clocks
    };
    struct Noise {
        std::uint8_t source = 0;  // 0-2: 31.25, 15.6, 7.8 kHz; 3: tone generator 0 or 3
        std::uint32_t lfsr = 0x3FFFF;
        std::uint64_t next_clock = 0;
    };
    struct Envelope {
        bool enabled = false;
        bool ended = true;  // at the end (point 3) of a single shape, or not started: new data acts at once
        bool looping = false;
        bool invert_right = false;
        bool external = false;
        bool three_bit = false;
        bool waiting = false;  // buffered data for point (3) or (4)
        std::uint8_t waiting_data = 0;
        std::uint8_t shape = 0;
        std::uint8_t phase = 0;
        std::uint8_t position = 0;
        std::uint8_t left = 0;
        std::uint8_t right = 0;
    };

    std::uint64_t half_period(const Tone& t) const;
    std::uint64_t noise_period(const Noise& n) const;
    void toggle(unsigned channel, std::uint64_t at);
    void clock_noise(Noise& n);
    void envelope_control(Envelope& e, std::uint8_t data);
    void envelope_load(Envelope& e, std::uint8_t data);
    void envelope_tick(Envelope& e);
    void envelope_levels(Envelope& e);
    unsigned channel_level(unsigned channel) const;
    void run_to(std::uint64_t clock, AudioSynth& out);
    void emit(std::uint64_t clock, AudioSynth& out);

    std::array<std::uint8_t, 6> amp_{};  // b3-0 left, b7-4 right
    std::array<Tone, 6> tone_{};
    std::array<Noise, 2> noise_{};
    std::array<Envelope, 2> env_{};
    std::uint8_t tone_mix_ = 0;
    std::uint8_t noise_mix_ = 0;
    std::uint8_t address_ = 0;
    bool enabled_ = false;
    bool sync_ = false;
    std::uint64_t clock_ = 0;  // chip clocks handled
    float scale_ = 0.0f;
    float level_ = 0.0f;  // the level last given to the synth
};

}  // namespace freetvc
