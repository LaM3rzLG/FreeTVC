// SPDX-License-Identifier: GPL-3.0-or-later
// Yamaha YM3812 (OPL2) with its YM3014B DAC, on the vendored ymfm (design/cards.md "YM3812").
#pragma once

#include <cstdint>
#include <memory>

#include "core/cards/chip_clock.hpp"

namespace freetvc {

class AudioSynth;
class ChunkWriter;
class ChunkReader;

class Ym3812 {
public:
    static constexpr ChipClock kClock{3'579'545};
    static constexpr std::uint64_t kClocksPerSample = 72;  // 18 operator slots of 4 clocks

    Ym3812();
    ~Ym3812();
    // ymfm keeps a reference to its host, so a copy builds a new chip and restores the state into it
    Ym3812(const Ym3812& other);
    Ym3812& operator=(const Ym3812& other);

    void power_on();
    // The /IC line at `tick`; the samples up to it are dropped (the reset button, between runs)
    void reset(std::uint64_t tick);
    // Catches up to `tick`: samples and timers in time order
    void advance_to(std::uint64_t tick, AudioSynth& out);
    // A0 = 0: status (reads) or register address (writes); A0 = 1: data (reads give FFh)
    std::uint8_t read(unsigned a0, std::uint64_t tick, AudioSynth& out);
    void write(unsigned a0, std::uint8_t value, std::uint64_t tick, AudioSynth& out);
    void set_scale(float scale) { scale_ = scale; }

    // The /IRQ pin, true while low
    bool irq() const;
    // The tick at which the next timer runs out, or UINT64_MAX
    std::uint64_t next_event() const;
    std::uint32_t sample_rate() const;
    float level() const { return level_; }

    void save(ChunkWriter& w) const;
    void load(ChunkReader& r);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    std::uint64_t next_sample_ = 0;  // samples begin every kClocksPerSample chip clocks from power-on
    float scale_ = 0.0f;
    float level_ = 0.0f;  // the level last given to the synth
};

}  // namespace freetvc
