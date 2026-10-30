// SPDX-License-Identifier: GPL-3.0-or-later
// Band-limited step synthesis: a piecewise-constant level, given as timed steps in half-T ticks, becomes PCM samples.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace freetvc {

class ChunkWriter;
class ChunkReader;

class AudioSynth {
public:
    // Snapshot fields; readable samples are dropped. A snapshot from another rate restarts the synthesis at `tick`.
    void save(ChunkWriter& w) const;
    void load(ChunkReader& r, std::uint64_t tick);
    static constexpr std::uint64_t kTickRate = 6'250'000;  // half-T ticks per second
    static constexpr unsigned kTaps = 32;                  // kernel length; output lags the input by (kTaps - 1) / 2 samples
    static constexpr unsigned kPhases = 64;                // sub-sample positions of a step

    // rate 0 disables output; highpass_hz 0 keeps DC (the AC coupling of a TV input is modelled with a one-pole high-pass).
    void configure(unsigned rate, float highpass_hz, std::size_t max_buffered);
    void clear();
    // Drops the readable samples and keeps the synthesis state.
    void discard_ready() {
        ready_.clear();
        ready_start_ = 0;
    }

    // Ticks must not decrease between calls.
    void set_level(std::uint64_t tick, float level);
    // Another source's change of level (a card's sound chip): steps add up, so any order works until end_at() passes them.
    void add_step(std::uint64_t tick, float delta);
    // Every sample that no step at or after `tick` can change becomes readable.
    void end_at(std::uint64_t tick);

    unsigned rate() const { return rate_; }
    std::size_t available() const { return ready_.size() - ready_start_; }
    std::uint64_t dropped() const { return dropped_; }
    // Copies up to out.size() of the oldest readable samples and removes them.
    std::size_t read(std::span<float> out);

private:
    std::uint64_t sample_position(std::uint64_t tick) const { return tick * rate_; }  // in 1/kTickRate samples

    unsigned rate_ = 0;
    double highpass_ = 0.0;  // pole of the high-pass, 0 = bypass
    std::size_t max_buffered_ = 0;
    std::vector<float> kernel_;  // kPhases x kTaps, each phase sums to 1
    std::vector<float> deltas_;  // differences still to integrate; deltas_[0] is sample base_
    std::uint64_t base_ = 0;
    float level_ = 0.0f;
    double integral_ = 0.0;  // double, so rounding does not build up over long runs
    double last_input_ = 0.0;
    double last_output_ = 0.0;
    std::vector<float> ready_;
    std::size_t ready_start_ = 0;
    std::uint64_t dropped_ = 0;
};

}  // namespace freetvc
