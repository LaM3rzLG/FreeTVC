// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/cards/sid_card.hpp"

#include <algorithm>

#include "core/audio/synth.hpp"
#include "core/snapshot/snapshot.hpp"
#include "sid.h"

namespace freetvc {

namespace {

constexpr std::uint8_t kSaveVersion = 1;
// reSID's full 16-bit swing: 0.75 (the TVC tone's loudest square) + 3 dB set by listening ❓ (no card known)
constexpr float kScale = 0.75f * 1.4125375f / 32768.0f;

reSID::chip_model chip_model(SidModel model) { return model == SidModel::Mos8580 ? reSID::MOS8580 : reSID::MOS6581; }

void write_state(ChunkWriter& w, const reSID::SID::State& s) {
    for (const char r : s.sid_register) {
        w.u8(static_cast<std::uint8_t>(r));
    }
    w.u8(static_cast<std::uint8_t>(s.bus_value));
    w.u32(static_cast<std::uint32_t>(s.bus_value_ttl));
    w.u32(static_cast<std::uint32_t>(s.write_pipeline));
    w.u8(static_cast<std::uint8_t>(s.write_address));
    w.u8(static_cast<std::uint8_t>(s.voice_mask));
    for (unsigned i = 0; i < 3; ++i) {
        w.u32(s.accumulator[i]);
        w.u32(s.shift_register[i]);
        w.u32(static_cast<std::uint32_t>(s.shift_register_reset[i]));
        w.u32(static_cast<std::uint32_t>(s.shift_pipeline[i]));
        w.u16(static_cast<std::uint16_t>(s.pulse_output[i]));
        w.u32(static_cast<std::uint32_t>(s.floating_output_ttl[i]));
        w.u16(static_cast<std::uint16_t>(s.rate_counter[i]));
        w.u16(static_cast<std::uint16_t>(s.rate_counter_period[i]));
        w.u16(static_cast<std::uint16_t>(s.exponential_counter[i]));
        w.u16(static_cast<std::uint16_t>(s.exponential_counter_period[i]));
        w.u8(static_cast<std::uint8_t>(s.envelope_counter[i]));
        w.u8(static_cast<std::uint8_t>(s.envelope_state[i]));
        w.boolean(s.hold_zero[i]);
        w.u32(static_cast<std::uint32_t>(s.envelope_pipeline[i]));
        w.boolean(s.msb_rising[i]);
        w.u16(static_cast<std::uint16_t>(s.noise_output[i]));
        w.u16(static_cast<std::uint16_t>(s.tri_saw_pipeline[i]));
        w.u16(static_cast<std::uint16_t>(s.osc3[i]));
        w.u16(static_cast<std::uint16_t>(s.waveform_output[i]));
    }
    for (const int v : {s.filter_Vhp, s.filter_Vbp, s.filter_Vbp_x, s.filter_Vbp_vc, s.filter_Vlp, s.filter_Vlp_x, s.filter_Vlp_vc,
                        s.filter_ve, s.filter_v1, s.filter_v2, s.filter_v3, s.extfilt_vlp, s.extfilt_vhp}) {
        w.u32(static_cast<std::uint32_t>(v));
    }
}

void read_state(ChunkReader& r, reSID::SID::State& s) {
    for (char& reg : s.sid_register) {
        reg = static_cast<char>(r.u8());
    }
    s.bus_value = r.u8();
    s.bus_value_ttl = static_cast<int>(r.u32());
    s.write_pipeline = static_cast<int>(r.u32());
    s.write_address = r.u8();
    s.voice_mask = r.u8();
    for (unsigned i = 0; i < 3; ++i) {
        s.accumulator[i] = r.u32() & 0xFFFFFFu;
        s.shift_register[i] = r.u32() & 0x7FFFFFu;
        s.shift_register_reset[i] = static_cast<int>(r.u32());
        s.shift_pipeline[i] = static_cast<int>(r.u32());
        s.pulse_output[i] = r.u16();
        s.floating_output_ttl[i] = static_cast<int>(r.u32());
        s.rate_counter[i] = r.u16();
        s.rate_counter_period[i] = r.u16();
        s.exponential_counter[i] = r.u16();
        s.exponential_counter_period[i] = r.u16();
        s.envelope_counter[i] = r.u8();
        const std::uint8_t stage = r.u8();
        if (stage > reSID::EnvelopeGenerator::RELEASE) {
            r.fail();
        }
        s.envelope_state[i] = static_cast<reSID::EnvelopeGenerator::State>(stage);
        s.hold_zero[i] = r.boolean();
        s.envelope_pipeline[i] = static_cast<int>(r.u32());
        s.msb_rising[i] = r.boolean();
        s.noise_output[i] = r.u16();
        s.tri_saw_pipeline[i] = r.u16() & 0xFFFu;
        s.osc3[i] = r.u16() & 0xFFFu;
        s.waveform_output[i] = r.u16() & 0xFFFu;
    }
    for (int* v : {&s.filter_Vhp, &s.filter_Vbp, &s.filter_Vbp_x, &s.filter_Vbp_vc, &s.filter_Vlp, &s.filter_Vlp_x, &s.filter_Vlp_vc,
                   &s.filter_ve, &s.filter_v1, &s.filter_v2, &s.filter_v3, &s.extfilt_vlp, &s.extfilt_vhp}) {
        *v = static_cast<int>(r.u32());
    }
}

}  // namespace

SidCard::SidCard(const SidCardConfig& config) : config_(config), clock_{config.clock_hz} { power_on(); }

SidCard::~SidCard() = default;

SidCard::SidCard(const SidCard& other)
    : config_(other.config_), clock_(other.clock_), done_(other.done_), last_write_(other.last_write_), level_(other.level_) {
    build();
    sid_->write_state(other.sid_->read_state());
}

SidCard& SidCard::operator=(const SidCard& other) {
    if (this != &other) {
        config_ = other.config_;
        clock_ = other.clock_;
        build();
        sid_->write_state(other.sid_->read_state());
        done_ = other.done_;
        last_write_ = other.last_write_;
        level_ = other.level_;
    }
    return *this;
}

void SidCard::build() {
    sid_ = std::make_unique<reSID::SID>();
    sid_->set_chip_model(chip_model(config_.model));
    sid_->adjust_filter_bias(0.0);  // reSID leaves the 6581 filter's bias uninitialised; 0 mV is VICE's and sidplayfp's default
    sid_->set_sampling_parameters(config_.clock_hz, reSID::SAMPLE_FAST, 48000);
}

void SidCard::power_on() {
    build();
    sid_->reset();
    done_ = 0;
    last_write_ = UINT64_MAX;
    level_ = 0.0f;
}

void SidCard::clock_to(std::uint64_t clocks, AudioSynth& out) {
    while (done_ < clocks) {
        const std::uint64_t step_end = (done_ / kClocksPerSample + 1) * kClocksPerSample;
        const std::uint64_t to = std::min(step_end, clocks);
        sid_->clock(static_cast<reSID::cycle_count>(to - done_));
        done_ = to;
        if (done_ == step_end) {
            const float level = static_cast<float>(sid_->output()) * kScale;
            if (level != level_) {
                out.add_step(clock_.tick_of(done_), level - level_);
                level_ = level;
            }
        }
    }
}

void SidCard::advance_to(std::uint64_t tick, AudioSynth& out) {
    const std::uint64_t begun = clock_.last_clock_by(tick) + 1;
    clock_to(begun / kClocksPerSample * kClocksPerSample, out);
}

void SidCard::reset(std::uint64_t tick, AudioSynth& out) {
    clock_to(clock_.last_clock_by(tick) + 1, out);
    sid_->reset();
}

std::uint8_t SidCard::read(unsigned reg, std::uint64_t tick, AudioSynth& out) {
    clock_to(clock_.last_clock_by(tick) + 1, out);
    return static_cast<std::uint8_t>(sid_->read(reg & 31));
}

void SidCard::write(unsigned reg, std::uint8_t value, std::uint64_t tick, AudioSynth& out) {
    clock_to(clock_.last_clock_by(tick) + 1, out);
    // reSID holds an 8580 write for one clock, and a second write in the same clock would replace it
    if (config_.model == SidModel::Mos8580 && done_ == last_write_) {
        clock_to(done_ + 1, out);
    }
    sid_->write(reg & 31, value);
    last_write_ = done_;
}

std::uint8_t SidCard::peek(unsigned reg) const {
    return static_cast<std::uint8_t>(sid_->read_state().sid_register[reg & 31]);
}

void SidCard::save(ChunkWriter& w) const {
    w.u8(kSaveVersion);
    w.u64(done_);
    w.u64(last_write_);
    w.f32(level_);
    write_state(w, sid_->read_state());
}

void SidCard::load(ChunkReader& r) {
    const std::uint8_t version = r.u8();
    if (version < 1 || version > kSaveVersion) {
        r.fail();  // a layout this version does not know
        return;
    }
    const std::uint64_t done = r.u64();
    const std::uint64_t last_write = r.u64();
    const float level = r.f32();
    reSID::SID::State state;
    read_state(r, state);
    if (r.failed()) {
        return;
    }
    build();
    sid_->write_state(state);
    done_ = done;
    last_write_ = last_write;
    level_ = level;
}

}  // namespace freetvc
