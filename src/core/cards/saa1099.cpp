// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/cards/saa1099.hpp"

#include <algorithm>
#include <limits>

#include "core/audio/synth.hpp"
#include "core/snapshot/snapshot.hpp"

namespace freetvc {

namespace {

constexpr std::uint64_t kNever = std::numeric_limits<std::uint64_t>::max();

// Data sheet Fig. 5 shapes 0-7: phases of 16 positions, looping or not
constexpr std::uint8_t kPhases[8] = {1, 1, 1, 1, 2, 2, 1, 1};
constexpr bool kLooping[8] = {false, true, false, true, false, true, false, true};

unsigned shape_level(unsigned shape, unsigned phase, unsigned position) {
    switch (shape) {
    case 0:
        return 0;
    case 1:
        return 15;
    case 2:
    case 3:
        return 15 - position;
    case 4:
    case 5:
        return phase == 0 ? position : 15 - position;
    default:
        return position;
    }
}

// The PDM patterns of half the amplitude and of the envelope, ANDed and filtered (SAASound's notes): a * e / 2 rounded up
unsigned enveloped(unsigned amp_half, unsigned env) { return (amp_half * env + 1) / 2 * 4; }

}  // namespace

void Saa1099::power_on() {
    amp_.fill(0);
    tone_.fill(Tone{});
    noise_.fill(Noise{});
    env_.fill(Envelope{});
    for (Tone& t : tone_) {
        t.next_toggle = half_period(t);
    }
    for (Noise& n : noise_) {
        n.next_clock = noise_period(n);
    }
    tone_mix_ = 0;
    noise_mix_ = 0;
    address_ = 0;
    enabled_ = false;
    sync_ = false;
    clock_ = 0;
    level_ = 0.0f;
}

// (511 - offset) << (8 - octave) clocks of 8 MHz: f = 15625 * 2^octave / (511 - offset) (SAM manual's table)
std::uint64_t Saa1099::half_period(const Tone& t) const { return static_cast<std::uint64_t>(511 - t.offset) << (8 - t.octave); }

std::uint64_t Saa1099::noise_period(const Noise& n) const { return n.source < 3 ? 256u << n.source : kNever; }

// An 18-bit Galois LFSR, x^18 + x^11 + x (Jepael's recording of an SAA1099P, quoted by SAASound and MAME)
void Saa1099::clock_noise(Noise& n) { n.lfsr = (n.lfsr & 1) ? (n.lfsr >> 1) ^ 0x20400 : n.lfsr >> 1; }

void Saa1099::toggle(unsigned channel, std::uint64_t at) {
    Tone& t = tone_[channel];
    t.high = !t.high;
    // Generators 1 and 4 clock the envelopes, 0 and 3 the noise generators set to source 3 (data sheet Fig. 1)
    if ((channel == 1 || channel == 4) && env_[channel / 3].enabled && !env_[channel / 3].external) {
        envelope_tick(env_[channel / 3]);
    }
    if ((channel == 0 || channel == 3) && noise_[channel / 3].source == 3) {
        clock_noise(noise_[channel / 3]);
    }
    // Buffered octave and offset act at the end of a half cycle (data sheet "Register description") ❓ the write order
    if (t.pending) {
        t.offset = t.next_offset;
        t.octave = t.next_octave;
        t.pending = false;
    }
    t.next_toggle = at + half_period(t);
}

void Saa1099::envelope_levels(Envelope& e) {
    unsigned level = 0;
    if (!e.ended || e.looping) {
        level = shape_level(e.shape, e.phase, e.position);
        if (e.three_bit) {
            level &= 0x0E;
        }
    }
    e.left = static_cast<std::uint8_t>(level);
    e.right = static_cast<std::uint8_t>(e.invert_right ? (e.three_bit ? 14u : 15u) - level : level);
}

void Saa1099::envelope_load(Envelope& e, std::uint8_t data) {
    e.shape = static_cast<std::uint8_t>((data >> 1) & 7);
    e.looping = kLooping[e.shape];
    e.invert_right = (data & 0x01) != 0;
    e.three_bit = (data & 0x10) != 0;
    e.external = (data & 0x20) != 0;
    e.enabled = (data & 0x80) != 0;
    e.ended = !e.enabled;
    e.phase = 0;
    e.position = 0;
    envelope_levels(e);
}

// Enable and resolution act at once; the rest only at the shape's end, point (3) or (4) (data sheet Fig. 5 notes)
void Saa1099::envelope_control(Envelope& e, std::uint8_t data) {
    const bool enabled = (data & 0x80) != 0;
    if (!enabled && !e.enabled) {
        return;
    }
    e.enabled = enabled;
    if (!enabled) {
        e.ended = true;
        return;
    }
    // Between resolutions the position keeps its upper bits (SAASound's notes from real chips)
    const bool three_bit = (data & 0x10) != 0;
    if (three_bit && !e.three_bit) {
        e.position &= 0x0E;
    } else if (!three_bit && e.three_bit) {
        e.position |= 0x01;
    }
    e.three_bit = three_bit;
    if (e.ended) {
        envelope_load(e, data);
        e.waiting = false;
    } else {
        envelope_levels(e);
        e.waiting = true;
        e.waiting_data = data;
    }
}

// 4-bit resolution steps by 1, 3-bit by 2: 16 or 8 clocks a phase (data sheet: up to 977 Hz or 1.95 kHz)
void Saa1099::envelope_tick(Envelope& e) {
    if (e.ended) {
        return;
    }
    e.position = static_cast<std::uint8_t>(e.position + (e.three_bit ? 2 : 1));
    bool at_end = false;
    if (e.position >= 16) {
        e.position = static_cast<std::uint8_t>(e.position - 16);
        if (++e.phase == kPhases[e.shape]) {
            at_end = true;
            e.phase = 0;
            e.ended = !e.looping;
        }
    }
    if (at_end && e.waiting) {
        e.waiting = false;
        envelope_load(e, e.waiting_data);
    } else {
        envelope_levels(e);
    }
}

unsigned Saa1099::channel_level(unsigned channel) const {
    if (!enabled_ || sync_) {
        return 0;
    }
    const bool tone = tone_[channel].high;
    const bool noise = (noise_[channel / 3].lfsr & 1) != 0;
    const bool tone_on = ((tone_mix_ >> channel) & 1) != 0;
    const bool noise_on = ((noise_mix_ >> channel) & 1) != 0;
    // 0, 1 or 2: tone and noise together give the tone at half level while the noise is high (SAASound's notes)
    unsigned mix = 0;
    if (tone_on && noise_on) {
        mix = tone ? (noise ? 1u : 2u) : 0u;
    } else if (tone_on) {
        mix = tone ? 2u : 0u;
    } else if (noise_on) {
        mix = noise ? 2u : 0u;
    }
    const unsigned left = amp_[channel] & 15u;
    const unsigned right = amp_[channel] >> 4;
    // Envelopes on channels 2 and 5 show where the mixer is low, so with both mixers off the envelope itself sounds
    const Envelope* e = channel == 2 ? &env_[0] : channel == 5 ? &env_[1] : nullptr;
    if (e && e->enabled) {
        return (enveloped(left >> 1, e->left) + enveloped(right >> 1, e->right)) * (2 - mix);
    }
    return (left + right) * mix * 16;
}

void Saa1099::emit(std::uint64_t clock, AudioSynth& out) {
    unsigned sum = 0;
    for (unsigned c = 0; c < 6; ++c) {
        sum += channel_level(c);
    }
    const float level = static_cast<float>(sum) * scale_;
    if (level != level_) {
        out.add_step(kClock.tick_of(clock), level - level_);
        level_ = level;
    }
}

void Saa1099::run_to(std::uint64_t clock, AudioSynth& out) {
    while (!sync_) {
        std::uint64_t next = kNever;
        for (const Tone& t : tone_) {
            next = std::min(next, t.next_toggle);
        }
        for (const Noise& n : noise_) {
            next = std::min(next, n.next_clock);
        }
        if (next > clock) {
            break;
        }
        for (Noise& n : noise_) {
            if (n.next_clock == next) {
                clock_noise(n);
                n.next_clock += noise_period(n);
            }
        }
        for (unsigned c = 0; c < 6; ++c) {
            if (tone_[c].next_toggle == next) {
                toggle(c, next);
            }
        }
        emit(next, out);
    }
    clock_ = std::max(clock_, clock);
}

void Saa1099::advance_to(std::uint64_t tick, AudioSynth& out) { run_to(kClock.last_clock_by(tick), out); }

void Saa1099::write_address(std::uint8_t value, std::uint64_t tick, AudioSynth& out) {
    advance_to(tick, out);
    address_ = static_cast<std::uint8_t>(value & 31);
    // The external envelope clock is the address write pulse; real chips take it only for 24 and 25 (SAASound, MAME)
    if (address_ == 24 || address_ == 25) {
        Envelope& e = env_[address_ - 24];
        if (e.enabled && e.external) {
            envelope_tick(e);
            emit(clock_, out);
        }
    }
}

void Saa1099::write_data(std::uint8_t value, std::uint64_t tick, AudioSynth& out) {
    advance_to(tick, out);
    const unsigned r = address_;
    if (r <= 0x05) {
        amp_[r] = value;
    } else if (r >= 0x08 && r <= 0x0D) {
        Tone& t = tone_[r - 0x08];
        t.next_offset = value;
        t.pending = !sync_;
        if (sync_) {
            t.offset = value;
        }
    } else if (r >= 0x10 && r <= 0x12) {
        for (unsigned i = 0; i < 2; ++i) {
            Tone& t = tone_[(r - 0x10) * 2 + i];
            t.next_octave = static_cast<std::uint8_t>((value >> (4 * i)) & 7);
            t.pending = !sync_;
            if (sync_) {
                t.octave = t.next_octave;
            }
        }
    } else if (r == 0x14) {
        tone_mix_ = static_cast<std::uint8_t>(value & 0x3F);
    } else if (r == 0x15) {
        noise_mix_ = static_cast<std::uint8_t>(value & 0x3F);
    } else if (r == 0x16) {
        for (unsigned i = 0; i < 2; ++i) {
            Noise& n = noise_[i];
            n.source = static_cast<std::uint8_t>((value >> (4 * i)) & 3);
            // The fixed rates come from a free-running divider of the 8 MHz clock ❓
            n.next_clock = n.source < 3 ? (clock_ / noise_period(n) + 1) * noise_period(n) : kNever;
        }
    } else if (r == 0x18 || r == 0x19) {
        envelope_control(env_[r - 0x18], value);
    } else if (r == 0x1C) {
        const bool sync = (value & 0x02) != 0;
        if (sync && !sync_) {
            // Sync holds every generator: tones high with their new data, counters cleared (data sheet RST)
            for (Tone& t : tone_) {
                t.high = true;
                t.offset = t.next_offset;
                t.octave = t.next_octave;
                t.pending = false;
            }
        } else if (!sync && sync_) {
            for (Tone& t : tone_) {
                t.next_toggle = clock_ + half_period(t);
            }
            for (Noise& n : noise_) {
                n.next_clock = n.source < 3 ? clock_ + noise_period(n) : kNever;
            }
        }
        sync_ = sync;
        enabled_ = (value & 0x01) != 0;
    }
    emit(clock_, out);
}

void Saa1099::save(ChunkWriter& w) const {
    for (unsigned c = 0; c < 6; ++c) {
        const Tone& t = tone_[c];
        w.u8(amp_[c]);
        w.u8(t.offset);
        w.u8(t.octave);
        w.u8(t.next_offset);
        w.u8(t.next_octave);
        w.boolean(t.pending);
        w.boolean(t.high);
        w.u64(t.next_toggle);
    }
    for (const Noise& n : noise_) {
        w.u8(n.source);
        w.u32(n.lfsr);
        w.u64(n.next_clock);
    }
    for (const Envelope& e : env_) {
        w.boolean(e.enabled);
        w.boolean(e.ended);
        w.boolean(e.waiting);
        w.u8(e.waiting_data);
        w.u8(static_cast<std::uint8_t>(e.shape | (e.invert_right ? 0x10 : 0) | (e.external ? 0x20 : 0) | (e.three_bit ? 0x40 : 0)));
        w.u8(e.phase);
        w.u8(e.position);
    }
    w.u8(tone_mix_);
    w.u8(noise_mix_);
    w.u8(address_);
    w.boolean(enabled_);
    w.boolean(sync_);
    w.u64(clock_);
    w.f32(level_);
}

void Saa1099::load(ChunkReader& r) {
    for (unsigned c = 0; c < 6; ++c) {
        Tone& t = tone_[c];
        amp_[c] = r.u8();
        t.offset = r.u8();
        t.octave = static_cast<std::uint8_t>(r.u8() & 7);
        t.next_offset = r.u8();
        t.next_octave = static_cast<std::uint8_t>(r.u8() & 7);
        t.pending = r.boolean();
        t.high = r.boolean();
        t.next_toggle = r.u64();
    }
    for (Noise& n : noise_) {
        n.source = static_cast<std::uint8_t>(r.u8() & 3);
        n.lfsr = r.u32() & 0x3FFFF;
        n.next_clock = r.u64();
    }
    for (Envelope& e : env_) {
        e.enabled = r.boolean();
        e.ended = r.boolean();
        e.waiting = r.boolean();
        e.waiting_data = r.u8();
        const std::uint8_t bits = r.u8();
        e.shape = static_cast<std::uint8_t>(bits & 7);
        e.looping = kLooping[e.shape];
        e.invert_right = (bits & 0x10) != 0;
        e.external = (bits & 0x20) != 0;
        e.three_bit = (bits & 0x40) != 0;
        e.phase = static_cast<std::uint8_t>(r.u8() % kPhases[e.shape]);
        e.position = static_cast<std::uint8_t>(r.u8() & 15);
        envelope_levels(e);
    }
    tone_mix_ = static_cast<std::uint8_t>(r.u8() & 0x3F);
    noise_mix_ = static_cast<std::uint8_t>(r.u8() & 0x3F);
    address_ = static_cast<std::uint8_t>(r.u8() & 31);
    enabled_ = r.boolean();
    sync_ = r.boolean();
    clock_ = r.u64();
    level_ = r.f32();
}

}  // namespace freetvc
