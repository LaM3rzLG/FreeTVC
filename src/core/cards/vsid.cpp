// SPDX-License-Identifier: GPL-3.0-or-later
// Written from [FW] tvc-sid.c (setregister_SID, reset_SID, calculate_SID_voices, prepare_unfiltered_output,
// PULSE_PSEUDO_MULTI_SAMPLING) and tvc-sound.c (calculate_sound_samples), step by step with their quirks.
#include "core/cards/vsid.hpp"

#include "core/snapshot/snapshot.hpp"

namespace freetvc {

namespace {

constexpr std::uint8_t kTriangle = 0x10;
constexpr std::uint8_t kTest = 0x08;
constexpr std::uint8_t kRing = 0x04;
constexpr std::uint8_t kSync = 0x02;
constexpr std::uint8_t kGate = 0x01;

// The MCU's division: by zero it gives 0 (Cortex-M33 UDIV without the divide-by-zero trap)
std::uint32_t div0(std::uint32_t a, std::uint32_t b) { return b == 0 ? 0 : a / b; }

// The sum of a pulse segment's output over the accumulator's top 12 bits ([FW] _PULSE_SEGMENT_PSEUDO_MULTI_SAMPLING)
std::uint32_t pulse_segment(std::uint32_t start, std::uint32_t end, std::uint32_t width) {
    if (end < width) {
        return 0;
    }
    if (start >= width) {
        return 0xFFF * (end - start);
    }
    return 0xFFF * (end - width);
}

// The pulse output averaged since the last calculation ([FW] PULSE_PSEUDO_MULTI_SAMPLING)
std::uint32_t pulse(std::uint32_t prev, std::uint32_t cur, std::uint32_t width) {
    if (prev == cur) {
        return cur >= width ? 0xFFF : 0;
    }
    if (prev < cur) {
        return pulse_segment(prev, cur, width) / (cur - prev);
    }
    return (pulse_segment(prev, 0x1000, width) + pulse_segment(0, cur, width)) / (0x1000 - prev + cur);
}

}  // namespace

void VSid::power_on() {
    voice_ = {};
    for (Voice& v : voice_) {
        v.pulse_width = 0;  // reset_SID over the globals' 2048
    }
    voice_[0].osc = 0xC00000;
    sid_.fill(0);
    master_ = 0;
    queue_ = {};
    read_ = write_ = 0;
    block_ = 0;
    prev_sample_ = 0;
    remaining_ = 0;
    output_.clear();
    output_clock_ = kBlockClocks;
}

void VSid::queue_write(std::uint8_t reg, std::uint8_t value, std::uint64_t clock) {
    // 256 entries and 8-bit positions: a 257th pending write makes the queue look empty, as on the card
    queue_[write_] = {clock, static_cast<std::uint8_t>(reg & 31), value};
    ++write_;
}

void VSid::set_register(unsigned addr, std::uint8_t value) {
    sid_[addr] = value;
    const unsigned n = addr / 7;
    if (n < 3) {
        Voice& v = voice_[n];
        const unsigned base = n * 7;
        switch (addr - base) {
        case 0:
            v.freq = value + (sid_[base + 1] << 8u);
            break;
        case 1:
            v.freq = sid_[base] + (static_cast<unsigned>(value) << 8);
            break;
        case 2:
            v.pulse_width = static_cast<std::uint16_t>(value + ((sid_[base + 3] & 0x0Fu) << 8));
            break;
        case 3:
            v.pulse_width = static_cast<std::uint16_t>(sid_[base + 2] + ((value & 0x0Fu) << 8));
            break;
        case 4:
            v.control = value;
            break;
        case 5:
            v.attack = value >> 4;
            v.decay = value & 0x0F;
            break;
        default:
            v.sustain = value >> 4;
            v.release = value & 0x0F;
            break;
        }
    } else if (addr == 24) {
        master_ = value & 0x0F;  // the filter, its mode and OFF3 change nothing: the card plays the unfiltered sum
    }
}

// Advances the voices by `clocks` at once ([FW] calculate_SID_voices)
void VSid::calculate(std::uint32_t clocks) {
    for (Voice& v : voice_) {
        v.msb_prev = v.msb;
    }
    for (Voice& v : voice_) {
        v.osc = (v.control & kTest) ? 0 : (v.osc + clocks * v.freq) & 0xFFFFFF;
    }
    for (Voice& v : voice_) {
        v.noise_acc += clocks * v.freq;
        const std::uint32_t rises = v.noise_acc >> 19;
        for (std::uint32_t i = 0; i < rises; ++i) {
            v.noise = v.noise << 1 | (((v.noise >> 22) ^ (v.noise >> 17)) & 1);
        }
        v.noise_acc -= rises << 19;
    }
    for (Voice& v : voice_) {
        v.msb = v.osc >= 0x800000;
        v.rising = !v.msb_prev && v.msb;
    }
    // Sync and ring from the previous voice in order 1 <- 3, 2 <- 1, 3 <- 2, each seeing the ones already changed
    for (unsigned i = 0; i < 3; ++i) {
        Voice& v = voice_[i];
        const Voice& src = voice_[(i + 2) % 3];
        if ((v.control & kSync) && src.rising) {
            v.osc = div0(src.osc - 0x800000, src.freq) * v.freq;
        }
    }
    for (unsigned i = 0; i < 3; ++i) {
        Voice& v = voice_[i];
        if ((v.control & kTriangle) && (v.control & kRing)) {
            v.msb ^= voice_[(i + 2) % 3].msb;
        }
    }
    for (Voice& v : voice_) {
        const std::uint32_t top = v.osc >> 12;
        const auto triangle = [&] { return static_cast<std::uint32_t>(((v.msb ? 2047u : 0u) ^ (top & 2047u)) << 1); };
        const auto pulse_out = [&] { return pulse(v.pulse_prev, top, v.pulse_width); };
        switch (v.control >> 4) {
        case 0:
            break;  // [FW] no_wave_resets is false: the output keeps its last value
        case 1:
            v.da = static_cast<std::uint16_t>(triangle());
            break;
        case 2:
            v.da = static_cast<std::uint16_t>(top);
            break;
        case 3:
            v.da = static_cast<std::uint16_t>(kAndMask[triangle() & top] << 4);
            break;
        case 4:
            v.da = static_cast<std::uint16_t>(pulse_out());
            break;
        case 5:
            v.da = static_cast<std::uint16_t>(kAndMask[triangle() & pulse_out()] << 4);
            break;
        case 6:
            v.da = static_cast<std::uint16_t>(kAndMask[top & pulse_out()] << 4);
            break;
        case 7:
            v.da = static_cast<std::uint16_t>(kAndMask[pulse_out() & top & triangle()] << 4);
            break;
        case 8:
            v.da = static_cast<std::uint16_t>(4095 & (v.noise >> 11));
            break;
        default:
            v.da = 0;  // noise with any other waveform
            break;
        }
        v.pulse_prev = top;
    }
    for (Voice& v : voice_) {
        envelope(v, clocks);
    }
    for (Voice& v : voice_) {
        v.volume = static_cast<std::int32_t>(static_cast<std::uint32_t>(v.da) * v.envelope) - 0x80000;
    }
}

// [FW] the ADSR part of calculate_SID_voices: a rate counter (LFSR15) and a volume-dependent divider (LFSR5)
void VSid::envelope(Voice& v, std::uint32_t clocks) {
    const bool gate = v.control & kGate;
    if (!gate && v.gate_prev) {
        v.stage = 4;
        v.lfsr15 = v.lfsr5 = 0;
        v.lfsr15_cmp = kAdsrRate[v.release];
        v.gate_prev = 0;
    } else if (gate && !v.gate_prev) {
        v.stage = 1;
        v.lfsr15 = v.lfsr5 = 0;
        v.lfsr15_cmp = kAdsrRate[v.attack];
        v.gate_prev = 1;
        v.hold_zero = false;
    }
    v.lfsr15 += clocks;
    if (v.lfsr15 < v.lfsr15_cmp) {
        return;
    }
    const auto d15 = static_cast<std::uint16_t>(div0(v.lfsr15, v.lfsr15_cmp));
    v.lfsr15 -= d15 * v.lfsr15_cmp;
    v.lfsr5 += d15;
    if (v.stage != 1 && v.lfsr5 < v.lfsr5_cmp) {
        return;
    }
    const auto d5 = static_cast<std::uint16_t>(div0(v.lfsr5, v.lfsr5_cmp));
    if (d5 >= 1) {
        v.lfsr5 = 0;
    }
    if (v.hold_zero) {
        return;
    }
    const std::uint32_t sustain = (static_cast<std::uint32_t>(v.sustain) << 4) + v.sustain;
    switch (v.stage) {
    case 1:
        v.envelope = std::min<std::uint32_t>(v.envelope + d15, 0x1FE);
        if (v.envelope >= 0xFF) {
            v.envelope = 0xFF - (v.envelope - 0xFF);  // the decay has started already
            v.stage = 2;
            v.hold_zero = false;
            v.lfsr15_cmp = kAdsrRate[v.decay];
        }
        break;
    case 2:
        v.envelope = v.envelope >= d5 ? v.envelope - d5 : 0;
        if (v.envelope <= sustain) {
            v.envelope = sustain;
            v.lfsr15_cmp = kAdsrRate[v.release];  // [FW] quirk: the release rate while sustaining
            v.stage = 3;
        }
        break;
    case 3:
        if (v.envelope > sustain) {
            v.stage = 2;
            v.lfsr15_cmp = kAdsrRate[v.decay];
        }
        break;
    case 4:
        v.envelope = v.envelope >= d5 ? v.envelope - d5 : 0;
        break;
    default:
        break;
    }
    v.lfsr5_cmp = kVolumeDivider[v.envelope & 0xFF];
    if (v.envelope == 0) {
        v.hold_zero = true;
    }
}

// The three voices' mean times the master volume, as the two 8-bit PWM outputs carry it ([FW] prepare_unfiltered_output)
std::uint16_t VSid::output() {
    std::int32_t v = ((voice_[0].volume + voice_[1].volume + voice_[2].volume) / 3 + 0x80000) * master_;
    v = v > 0xFFFFFF ? 0xFFFFFF : v < 0 ? 0 : v;
    // [FW] multiplies in 32-bit int and so wraps above 32869; the division then truncates toward zero
    const auto product = static_cast<std::int32_t>(static_cast<std::uint32_t>((v >> 8) & 0xFFFF) * 65335u);
    const auto pwm = static_cast<std::uint16_t>(product / 65535 + 100);
    sid_[27] = static_cast<std::uint8_t>(voice_[2].osc >> 16);
    sid_[28] = static_cast<std::uint8_t>(voice_[2].envelope);
    return pwm;
}

void VSid::compute_to(std::uint64_t clock) {
    while ((block_ + 1) * kBlockClocks <= clock) {
        std::uint64_t sample_time = block_ * kBlockClocks;
        ++block_;
        for (unsigned i = 0; i < kBlockSamples; ++i) {
            while (read_ != write_) {
                const Write& w = queue_[read_];
                if (w.clock >= sample_time) {
                    break;
                }
                if (w.clock > prev_sample_) {
                    const auto diff = static_cast<std::uint32_t>(w.clock - prev_sample_);
                    calculate(diff);
                    remaining_ = static_cast<std::uint16_t>(remaining_ - diff);
                    prev_sample_ = w.clock;
                }
                set_register(w.reg, w.value);
                ++read_;
            }
            if (remaining_ > 0) {
                calculate(remaining_);
            }
            remaining_ = kClocksPerSample;
            output_.push_back(output());
            prev_sample_ = sample_time;
            sample_time += kClocksPerSample;
        }
    }
}

void VSid::save(ChunkWriter& w) const {
    for (const Voice& v : voice_) {
        for (std::uint32_t x : {v.osc, v.freq, v.noise_acc, v.noise, v.pulse_prev, v.envelope, v.lfsr15, v.lfsr15_cmp, v.lfsr5, v.lfsr5_cmp}) {
            w.u32(x);
        }
        w.u16(v.pulse_width);
        w.u16(v.da);
        for (std::uint8_t x : {v.control, v.msb, v.msb_prev, v.rising, v.gate_prev, v.attack, v.decay, v.sustain, v.release, v.stage}) {
            w.u8(x);
        }
        w.boolean(v.hold_zero);
        w.u32(static_cast<std::uint32_t>(v.volume));
    }
    w.bytes(sid_);
    w.u8(master_);
    w.u8(static_cast<std::uint8_t>(write_ - read_));
    for (std::uint8_t i = read_; i != write_; ++i) {
        w.u64(queue_[i].clock);
        w.u8(queue_[i].reg);
        w.u8(queue_[i].value);
    }
    w.u64(block_);
    w.u64(prev_sample_);
    w.u16(remaining_);
    w.u64(output_clock_);
    w.u16(static_cast<std::uint16_t>(output_.size()));
    for (std::uint16_t s : output_) {
        w.u16(s);
    }
}

void VSid::load(ChunkReader& r) {
    for (Voice& v : voice_) {
        for (std::uint32_t* x : {&v.osc, &v.freq, &v.noise_acc, &v.noise, &v.pulse_prev, &v.envelope, &v.lfsr15, &v.lfsr15_cmp, &v.lfsr5,
                                 &v.lfsr5_cmp}) {
            *x = r.u32();
        }
        v.envelope &= 0x1FF;
        v.pulse_width = r.u16();
        v.da = r.u16();
        for (std::uint8_t* x : {&v.control, &v.msb, &v.msb_prev, &v.rising, &v.gate_prev, &v.attack, &v.decay, &v.sustain, &v.release, &v.stage}) {
            *x = r.u8();
        }
        v.attack &= 15;
        v.decay &= 15;
        v.sustain &= 15;
        v.release &= 15;
        v.hold_zero = r.boolean();
        v.volume = static_cast<std::int32_t>(r.u32());
    }
    r.bytes(sid_);
    master_ = r.u8() & 15;
    read_ = 0;
    write_ = r.u8();
    for (std::uint8_t i = 0; i != write_; ++i) {
        queue_[i].clock = r.u64();
        queue_[i].reg = r.u8() & 31;
        queue_[i].value = r.u8();
    }
    block_ = r.u64();
    prev_sample_ = r.u64();
    remaining_ = r.u16();
    output_clock_ = r.u64();
    output_.clear();
    const unsigned n = r.u16();
    for (unsigned i = 0; i < n && !r.failed(); ++i) {
        output_.push_back(r.u16());
    }
}

}  // namespace freetvc
