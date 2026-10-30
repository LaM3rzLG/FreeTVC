// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/cards/ym3812.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

#include "core/audio/synth.hpp"
#include "core/snapshot/snapshot.hpp"
#include "ymfm_opl.h"

namespace freetvc {

namespace {

constexpr std::uint64_t kNever = std::numeric_limits<std::uint64_t>::max();
constexpr std::size_t kMaxChipState = 1u << 16;

}  // namespace

// ymfm's host: time is the chip clock of the operation in progress
struct Ym3812::Impl final : ymfm::ymfm_interface {
    ymfm::ym3812 chip{*this};
    std::uint64_t now = 0;
    std::array<std::uint64_t, 2> timer_end{kNever, kNever};
    std::uint64_t busy_end = 0;
    bool irq = false;

    void ymfm_set_timer(uint32_t tnum, int32_t duration_in_clocks) override {
        timer_end[tnum & 1] = duration_in_clocks < 0 ? kNever : now + static_cast<std::uint64_t>(duration_in_clocks);
    }
    void ymfm_set_busy_end(uint32_t clocks) override { busy_end = now + clocks; }
    bool ymfm_is_busy() override { return now < busy_end; }
    void ymfm_update_irq(bool asserted) override { irq = asserted; }

    void expire(unsigned tnum) {
        now = timer_end[tnum];
        timer_end[tnum] = kNever;
        m_engine->engine_timer_expired(tnum);
    }
    std::vector<std::uint8_t> state() {
        std::vector<std::uint8_t> buffer;
        ymfm::ymfm_saved_state saved(buffer, true);
        chip.save_restore(saved);
        return buffer;
    }
    void restore(std::vector<std::uint8_t> buffer) {
        ymfm::ymfm_saved_state saved(buffer, false);
        chip.save_restore(saved);
    }
    void copy_from(Impl& other) {
        now = other.now;
        timer_end = other.timer_end;
        busy_end = other.busy_end;
        irq = other.irq;
        restore(other.state());
    }
};

Ym3812::Ym3812() : impl_(std::make_unique<Impl>()) { power_on(); }

Ym3812::~Ym3812() = default;

Ym3812::Ym3812(const Ym3812& other)
    : impl_(std::make_unique<Impl>()), next_sample_(other.next_sample_), scale_(other.scale_), level_(other.level_) {
    impl_->copy_from(*other.impl_);
}

Ym3812& Ym3812::operator=(const Ym3812& other) {
    if (this != &other) {
        impl_->copy_from(*other.impl_);
        next_sample_ = other.next_sample_;
        scale_ = other.scale_;
        level_ = other.level_;
    }
    return *this;
}

void Ym3812::power_on() {
    impl_->now = 0;
    impl_->timer_end = {kNever, kNever};
    impl_->busy_end = 0;
    impl_->chip.reset();
    impl_->irq = false;
    next_sample_ = 0;
    level_ = 0.0f;
}

void Ym3812::reset(std::uint64_t tick) {
    impl_->now = kClock.last_clock_by(tick);
    next_sample_ = std::max(next_sample_, impl_->now / kClocksPerSample + 1);
    impl_->timer_end = {kNever, kNever};
    impl_->chip.reset();
    impl_->irq = false;
}

void Ym3812::advance_to(std::uint64_t tick, AudioSynth& out) {
    const std::uint64_t limit = kClock.last_clock_by(tick);
    for (;;) {
        const std::uint64_t sample_clock = next_sample_ * kClocksPerSample;
        const unsigned t = impl_->timer_end[1] < impl_->timer_end[0] ? 1u : 0u;
        // A timer running out at a sample's clock goes first: its CSM key-on reaches that sample
        if (impl_->timer_end[t] <= limit && impl_->timer_end[t] <= sample_clock) {
            impl_->expire(t);
            continue;
        }
        if (sample_clock > limit) {
            break;
        }
        impl_->now = sample_clock;
        ymfm::ym3812::output_data data;
        impl_->chip.generate(&data);
        const float level = static_cast<float>(data.data[0]) * scale_;
        if (level != level_) {
            out.add_step(kClock.tick_of(sample_clock), level - level_);
            level_ = level;
        }
        ++next_sample_;
    }
}

std::uint8_t Ym3812::read(unsigned a0, std::uint64_t tick, AudioSynth& out) {
    advance_to(tick, out);
    impl_->now = kClock.last_clock_by(tick);
    return impl_->chip.read(a0 & 1);
}

void Ym3812::write(unsigned a0, std::uint8_t value, std::uint64_t tick, AudioSynth& out) {
    advance_to(tick, out);
    impl_->now = kClock.last_clock_by(tick);
    impl_->chip.write(a0 & 1, value);
}

bool Ym3812::irq() const { return impl_->irq; }

std::uint64_t Ym3812::next_event() const {
    const std::uint64_t end = std::min(impl_->timer_end[0], impl_->timer_end[1]);
    return end == kNever ? kNever : kClock.tick_of(end);
}

std::uint32_t Ym3812::sample_rate() const { return impl_->chip.sample_rate(static_cast<std::uint32_t>(kClock.hz)); }

void Ym3812::save(ChunkWriter& w) const {
    w.u64(next_sample_);
    w.f32(level_);
    w.u64(impl_->now);
    w.u64(impl_->timer_end[0]);
    w.u64(impl_->timer_end[1]);
    w.u64(impl_->busy_end);
    w.boolean(impl_->irq);
    w.packed(impl_->state());
}

void Ym3812::load(ChunkReader& r) {
    next_sample_ = r.u64();
    level_ = r.f32();
    impl_->now = r.u64();
    impl_->timer_end[0] = r.u64();
    impl_->timer_end[1] = r.u64();
    impl_->busy_end = r.u64();
    impl_->irq = r.boolean();
    std::vector<std::uint8_t> state = r.packed_vector(kMaxChipState);
    if (!r.failed()) {
        impl_->restore(std::move(state));
    }
}

}  // namespace freetvc
