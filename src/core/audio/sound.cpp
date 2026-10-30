// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/audio/sound.hpp"

namespace freetvc {

namespace {

constexpr float kLevelScale = 0.75f / 15.0f;  // amplitude 15 gives 0.75 at the synth input

// The 3.125 MHz clock ticks on even half-T ticks (tick 0 = power-on).
constexpr std::uint64_t next_clock(std::uint64_t tick) { return tick + (tick & 1); }

}  // namespace

void Sound::power_on() {
    pitch_ = 0;
    chain_ = 0;
    next_reload_ = period();
    tone_enabled_ = false;
    irq_enabled_ = false;
    amplitude_ = 0;
    txc_edges_ = 0;
}

void Sound::set_pitch(std::uint16_t pitch, std::uint64_t tick) {
    const bool was_stopped = pitch_ == kStopPitch;
    pitch_ = pitch;
    // A new value is loaded at the next reload; leaving 4095 restarts the count from the new value ❓
    if (pitch_ == kStopPitch) {
        next_reload_ = kNever;
    } else if (was_stopped) {
        next_reload_ = next_clock(tick) + period();
    }
}

void Sound::set_pitch_low(std::uint8_t value, std::uint64_t tick, AudioSynth&) {
    set_pitch(static_cast<std::uint16_t>((pitch_ & 0xF00) | value), tick);
}

void Sound::set_control(std::uint8_t value, std::uint64_t tick, AudioSynth& out) {
    set_pitch(static_cast<std::uint16_t>((value & 0x0F) << 8 | (pitch_ & 0xFF)), tick);
    tone_enabled_ = (value & 0x10) != 0;
    // Enabling mid-period gives no IRQ at once: only the next 1->0 edge of b3 counts ❓
    irq_enabled_ = (value & 0x20) != 0;
    output(tick, out);
}

void Sound::set_amplitude(std::uint8_t value, std::uint64_t tick, AudioSynth& out) {
    amplitude_ = (value >> 2) & 0x0F;
    output(tick, out);
}

void Sound::instart(std::uint64_t tick, AudioSynth& out) {
    chain_ = 0;
    next_reload_ = pitch_ == kStopPitch ? kNever : next_clock(tick) + period();
    output(tick, out);
}

void Sound::output(std::uint64_t tick, AudioSynth& out) const {
    out.set_level(tick, static_cast<float>(level()) * kLevelScale);
}

unsigned Sound::step_to(std::uint64_t tick, AudioSynth& out) {
    const std::uint64_t step = period();
    if (!tone_enabled_ && !irq_enabled_) {
        // Nothing observable: skip whole reloads at once
        const std::uint64_t reloads = (tick - next_reload_) / step + 1;
        txc_edges_ += (reloads + 1 - (chain_ & 1)) / 2;  // b0 rises on every reload that makes the chain odd
        chain_ = static_cast<unsigned>((chain_ + reloads) & 15);
        next_reload_ += reloads * step;
        return 0;
    }
    unsigned edges = 0;
    while (next_reload_ <= tick) {
        chain_ = (chain_ + 1) & 15;
        txc_edges_ += chain_ & 1;
        if (chain_ == 8 || chain_ == 0) {
            // The IRQ comes on b3's 1->0 edge ([OS]): a full period after INSTART, as the ROM's tape timing needs
            if (chain_ == 0 && irq_enabled_) {
                ++edges;
            }
            if (tone_enabled_) {
                output(next_reload_, out);
            }
        }
        next_reload_ += step;
    }
    return edges;
}

}  // namespace freetvc
