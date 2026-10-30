// SPDX-License-Identifier: GPL-3.0-or-later
// The SID card (design/cards.md "SID card"): a MOS 6581 or 8580 on the vendored reSID at ports E0h-FFh, outside
// the slots, as WinTVC 1.4 places it; no real card's documentation is known.
#pragma once

#include <cstdint>
#include <memory>

#include "core/cards/chip_clock.hpp"
#include "core/machine/config.hpp"

namespace reSID {
class SID;
}

namespace freetvc {

class AudioSynth;
class ChunkWriter;
class ChunkReader;

class SidCard {
public:
    static constexpr std::uint8_t kFirstPort = 0xE0;  // A7-A5 high; A4-A0 reach the chip
    // reSID runs in steps of this many chip clocks between accesses; its output is taken at each step (~246 kHz)
    static constexpr std::uint64_t kClocksPerSample = 4;

    explicit SidCard(const SidCardConfig& config);
    ~SidCard();
    // reSID's voices point at each other, so a copy builds a new chip and restores the state into it
    SidCard(const SidCard& other);
    SidCard& operator=(const SidCard& other);

    void power_on();
    // The /RES line at `tick`
    void reset(std::uint64_t tick, AudioSynth& out);
    // Clocks up to the last sample step at or before `tick`; accesses clock to their own tick
    void advance_to(std::uint64_t tick, AudioSynth& out);
    // Register 0-31 (A4-A0); writes to 19h-1Fh and reads of 00h-18h and 1Dh-1Fh meet the chip's fading data bus
    std::uint8_t read(unsigned reg, std::uint64_t tick, AudioSynth& out);
    void write(unsigned reg, std::uint8_t value, std::uint64_t tick, AudioSynth& out);

    const SidCardConfig& config() const { return config_; }
    float level() const { return level_; }
    // The registers as last written (00h-18h), and OSC3/ENV3, without touching the data bus
    std::uint8_t peek(unsigned reg) const;

    void save(ChunkWriter& w) const;
    void load(ChunkReader& r);

private:
    void build();
    void clock_to(std::uint64_t clocks, AudioSynth& out);

    SidCardConfig config_;
    ChipClock clock_;
    std::unique_ptr<reSID::SID> sid_;
    std::uint64_t done_ = 0;  // chip clocks run since power-on
    std::uint64_t last_write_ = UINT64_MAX;  // done_ at the last write: an 8580's write lands a clock later
    float level_ = 0.0f;      // the level last given to the synth
};

}  // namespace freetvc
