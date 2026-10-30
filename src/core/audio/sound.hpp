// SPDX-License-Identifier: GPL-3.0-or-later
// TVC tone generator: 12-bit reloading counter on the 3.125 MHz clock, fixed /16 chain, gates, amplitude (tvc-hardware.md §6).
#pragma once

#include <cstdint>
#include <limits>

#include "core/audio/synth.hpp"

namespace freetvc {

class ChunkWriter;
class ChunkReader;

class Sound {
public:
    // Snapshot fields (core/snapshot/machine_state.cpp)
    void save(ChunkWriter& w) const;
    void load(ChunkReader& r);
    static constexpr std::uint16_t kStopPitch = 0xFFF;  // the oscillator stops ([HW], the sound generator)
    static constexpr std::uint64_t kNever = std::numeric_limits<std::uint64_t>::max();

    // Counter and chain at 0, pitch 0, gates off, amplitude 0 ❓.
    void power_on();

    // Port writes and the INSTART read at `tick` (half-T); each sends the new output level to `out`.
    void set_pitch_low(std::uint8_t value, std::uint64_t tick, AudioSynth& out);  // 04h
    void set_control(std::uint8_t value, std::uint64_t tick, AudioSynth& out);    // 05h: b3-0 pitch b11-8, b4 tone, b5 IRQ
    void set_amplitude(std::uint8_t value, std::uint64_t tick, AudioSynth& out);  // 06h b5-2
    void instart(std::uint64_t tick, AudioSynth& out);                           // 5Bh read: reload, clear the chain

    // Steps every reload at or before `tick`; returns the sound IRQ edges (chain b3 falling, 1->0, while 05h b5 = 1).
    unsigned advance_to(std::uint64_t tick, AudioSynth& out) {
        return next_reload_ <= tick ? step_to(tick, out) : 0;
    }

    // Rising edges of the chain's b0, the bus TXRXCLK a serial card counts (design/cards.md "Serial card"); valid after advance_to()
    std::uint64_t txc_edges() const { return txc_edges_; }
    void set_txc_edges(std::uint64_t edges) { txc_edges_ = edges; }

    std::uint16_t pitch() const { return pitch_; }
    unsigned chain() const { return chain_; }
    std::uint64_t next_reload() const { return next_reload_; }
    bool tone_enabled() const { return tone_enabled_; }
    bool irq_enabled() const { return irq_enabled_; }
    unsigned amplitude() const { return amplitude_; }
    // Output level 0-15: 0 while the tone is gated through and chain b3 is low, otherwise the amplitude.
    unsigned level() const { return tone_enabled_ && chain_ < 8 ? 0 : amplitude_; }

private:
    unsigned step_to(std::uint64_t tick, AudioSynth& out);
    std::uint64_t period() const { return 2 * static_cast<std::uint64_t>(0x1000 - pitch_); }  // in half-T ticks
    void set_pitch(std::uint16_t pitch, std::uint64_t tick);
    void output(std::uint64_t tick, AudioSynth& out) const;

    std::uint16_t pitch_ = 0;
    unsigned chain_ = 0;  // /16 chain; b3 is the tone and the IRQ source
    std::uint64_t next_reload_ = 0;
    bool tone_enabled_ = false;
    bool irq_enabled_ = false;
    unsigned amplitude_ = 0;
    std::uint64_t txc_edges_ = 0;
};

}  // namespace freetvc
