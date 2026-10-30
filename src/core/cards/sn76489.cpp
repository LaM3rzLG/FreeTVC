// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/cards/sn76489.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "core/audio/synth.hpp"
#include "core/snapshot/snapshot.hpp"

namespace freetvc {

namespace {

constexpr std::uint32_t kShiftReset = 0x10000;  // 15 bits as MAME keeps them for the SN76489A: taps at b2 and b3, output b0

// 2 dB a step (TI manual Table 1); 15 is off
float volume(unsigned attenuation) {
    return attenuation >= 15 ? 0.0f : static_cast<float>(std::pow(10.0, -0.1 * attenuation));
}

}  // namespace

void Sn76489::power_on() {
    regs_.fill(0);
    latched_ = 0;
    period_ = {0x400, 0x400, 0x400, 32};
    count_.fill(0);
    high_.fill(false);
    shift_ = kShiftReset;
    running_ = false;
    next_step_ = 0;
    next_frac_ = 0;
    level_ = 0.0f;
}

void Sn76489::mute_all() {
    for (unsigned c = 0; c < 4; ++c) {
        regs_[c * 2 + 1] = 15;
    }
}

void Sn76489::add_steps(std::uint64_t steps) {
    // 16 clocks are 16 * 6 250 000 / clock_hz_ ticks
    const std::uint64_t total = next_frac_ + steps * 100'000'000ull;
    next_step_ += total / clock_hz_;
    next_frac_ = static_cast<std::uint32_t>(total % clock_hz_);
}

void Sn76489::start_clock(std::uint64_t tick) {
    running_ = true;
    next_step_ = tick;
    next_frac_ = 0;
    add_steps(1);
}

float Sn76489::output_level() const {
    float sum = 0.0f;
    for (unsigned c = 0; c < 4; ++c) {
        if (high_[c]) {
            sum += volume(regs_[c * 2 + 1] & 15);
        }
    }
    return sum * kChannelPeak;
}

void Sn76489::emit(std::uint64_t tick, AudioSynth& out) {
    const float level = output_level();
    if (level != level_) {
        out.add_step(tick, level - level_);
        level_ = level;
    }
}

void Sn76489::advance_to(std::uint64_t tick, AudioSynth& out) {
    while (running_ && next_step_ <= tick) {
        // Jump to the next step at which a counter runs out, or to the last step before `tick`
        std::int32_t steps = std::numeric_limits<std::int32_t>::max();
        for (const std::int32_t c : count_) {
            steps = std::min(steps, std::max(c, 1));
        }
        // Steps due by `tick`: those whose time next_step_ + (next_frac_ + k * 1e8) / clock_hz_ is at most `tick`
        const std::uint64_t span = (std::min<std::uint64_t>(tick - next_step_, 1u << 30) + 1) * clock_hz_ - next_frac_;
        const std::uint64_t available = (span + 100'000'000ull - 1) / 100'000'000ull;
        const auto n = static_cast<std::int32_t>(std::min<std::uint64_t>(static_cast<std::uint64_t>(steps), available));
        const std::uint64_t at = next_step_ + (next_frac_ + static_cast<std::uint64_t>(n - 1) * 100'000'000ull) / clock_hz_;
        bool changed = false;
        for (unsigned c = 0; c < 3; ++c) {
            count_[c] -= n;
            if (count_[c] <= 0) {
                high_[c] = !high_[c];
                count_[c] = period_[c];
                changed = true;
            }
        }
        count_[3] -= n;
        if (count_[3] <= 0) {
            // White noise feeds back b2 XOR b3, periodic noise b2 alone
            const bool feedback = ((shift_ & 4) != 0) != ((regs_[6] & 4) != 0 && (shift_ & 8) != 0);
            shift_ = (shift_ >> 1) | (feedback ? kShiftReset : 0);
            high_[3] = (shift_ & 1) != 0;
            count_[3] = period_[3];
            changed = true;
        }
        add_steps(static_cast<std::uint64_t>(n));
        if (changed) {
            emit(at, out);
        }
    }
}

void Sn76489::write(std::uint8_t value, std::uint64_t tick, AudioSynth& out) {
    advance_to(tick, out);
    if (!running_) {
        return;
    }
    unsigned r = latched_;
    if (value & 0x80) {
        r = (value >> 4) & 7;
        latched_ = r;
        regs_[r] = static_cast<std::uint16_t>((regs_[r] & 0x3F0) | (value & 0x0F));
    } else if (r == 0 || r == 2 || r == 4) {
        // A data byte gives a tone's six high bits; to the other registers, its low four bits
        regs_[r] = static_cast<std::uint16_t>((regs_[r] & 0x0F) | ((value & 0x3F) << 4));
    } else {
        regs_[r] = static_cast<std::uint16_t>((regs_[r] & 0x3F0) | (value & 0x0F));
    }
    if (r == 0 || r == 2 || r == 4) {
        // Every TI-made chip takes a period of 0 as 400h (MAME's notes)
        period_[r / 2] = regs_[r] != 0 ? regs_[r] : 0x400;
        if (r == 4 && (regs_[6] & 3) == 3) {
            period_[3] = static_cast<std::uint16_t>(period_[2] << 1);
        }
    } else if (r == 6) {
        // N/512, N/1024, N/2048 or tone 3's rate; every write clears the shift register (TI manual §2)
        period_[3] = (regs_[6] & 3) == 3 ? static_cast<std::uint16_t>(period_[2] << 1) : static_cast<std::uint16_t>(32u << (regs_[6] & 3));
        shift_ = kShiftReset;
    }
    emit(tick, out);
}

void Sn76489::set_clock(bool running, std::uint64_t tick, AudioSynth& out) {
    if (running == running_) {
        return;
    }
    advance_to(tick, out);
    running_ = running;
    next_step_ = tick;
    next_frac_ = 0;
    add_steps(1);
}

void Sn76489::save(ChunkWriter& w) const {
    for (const std::uint16_t r : regs_) {
        w.u16(r);
    }
    w.u8(static_cast<std::uint8_t>(latched_));
    for (unsigned c = 0; c < 4; ++c) {
        w.u16(period_[c]);
        w.u32(static_cast<std::uint32_t>(count_[c]));
        w.boolean(high_[c]);
    }
    w.u32(shift_);
    w.boolean(running_);
    w.u64(next_step_);
    w.f32(level_);
}

void Sn76489::load(ChunkReader& r) {
    for (std::uint16_t& v : regs_) {
        v = static_cast<std::uint16_t>(r.u16() & 0x3FF);
    }
    latched_ = r.u8() & 7u;
    for (unsigned c = 0; c < 4; ++c) {
        period_[c] = std::max<std::uint16_t>(1, r.u16());
        count_[c] = static_cast<std::int32_t>(r.u32());
        high_[c] = r.boolean();
    }
    shift_ = r.u32() & 0x1FFFF;
    running_ = r.boolean();
    next_step_ = r.u64();
    next_frac_ = 0;
    level_ = r.f32();
}

void Sn76489::save_phase(ChunkWriter& w) const { w.u32(next_frac_); }

void Sn76489::load_phase(ChunkReader& r) { next_frac_ = r.u32() % clock_hz_; }

}  // namespace freetvc
