// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/audio/synth.hpp"

#include <algorithm>
#include <cmath>

namespace freetvc {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kCutoff = 0.41;  // cycles per output sample: about 19.7 kHz at 48 kHz, stopband from Nyquist (Blackman)

}  // namespace

void AudioSynth::configure(unsigned rate, float highpass_hz, std::size_t max_buffered) {
    rate_ = rate;
    max_buffered_ = max_buffered;
    highpass_ = rate != 0 && highpass_hz > 0.0f ? std::exp(-2.0 * kPi * highpass_hz / rate) : 0.0;
    // Windowed-sinc impulse per phase, taken mid-sample: tap k covers x = k + 1/2 - kTaps/2 - phase/kPhases from the (delayed) step
    kernel_.assign(static_cast<std::size_t>(kPhases) * kTaps, 0.0f);
    constexpr double half = kTaps / 2.0;
    for (unsigned p = 0; p < kPhases; ++p) {
        double taps[kTaps];
        double sum = 0.0;
        for (unsigned k = 0; k < kTaps; ++k) {
            const double x = k + 0.5 - half - static_cast<double>(p) / kPhases;
            const double sinc = x == 0.0 ? 1.0 : std::sin(2.0 * kPi * kCutoff * x) / (2.0 * kPi * kCutoff * x);
            const double window = std::abs(x) >= half ? 0.0 : 0.42 + 0.5 * std::cos(kPi * x / half) + 0.08 * std::cos(2.0 * kPi * x / half);
            taps[k] = sinc * window;
            sum += taps[k];
        }
        for (unsigned k = 0; k < kTaps; ++k) {
            kernel_[p * kTaps + k] = static_cast<float>(taps[k] / sum);
        }
    }
    clear();
}

void AudioSynth::clear() {
    deltas_.clear();
    base_ = 0;
    level_ = 0.0f;
    integral_ = 0.0;
    last_input_ = 0.0;
    last_output_ = 0.0;
    ready_.clear();
    ready_start_ = 0;
    dropped_ = 0;
}

void AudioSynth::set_level(std::uint64_t tick, float level) {
    const float delta = level - level_;
    level_ = level;
    add_step(tick, delta);
}

void AudioSynth::add_step(std::uint64_t tick, float delta) {
    if (rate_ == 0 || delta == 0.0f) {
        return;
    }
    const std::uint64_t position = sample_position(tick);
    const std::uint64_t sample = std::max(position / kTickRate, base_);
    const auto phase = static_cast<unsigned>(position % kTickRate * kPhases / kTickRate);
    const std::size_t offset = static_cast<std::size_t>(sample - base_);
    if (deltas_.size() < offset + kTaps) {
        deltas_.resize(offset + kTaps, 0.0f);
    }
    const float* taps = &kernel_[static_cast<std::size_t>(phase) * kTaps];
    for (unsigned k = 0; k < kTaps; ++k) {
        deltas_[offset + k] += delta * taps[k];
    }
}

void AudioSynth::end_at(std::uint64_t tick) {
    if (rate_ == 0) {
        return;
    }
    const std::uint64_t end = sample_position(tick) / kTickRate;
    if (end <= base_) {
        return;
    }
    const auto count = static_cast<std::size_t>(end - base_);
    if (ready_start_ > 0 && ready_start_ * 2 >= ready_.size()) {
        ready_.erase(ready_.begin(), ready_.begin() + static_cast<std::ptrdiff_t>(ready_start_));
        ready_start_ = 0;
    }
    for (std::size_t i = 0; i < count; ++i) {
        integral_ += i < deltas_.size() ? deltas_[i] : 0.0f;
        double out = integral_;
        if (highpass_ != 0.0) {
            out = integral_ - last_input_ + highpass_ * last_output_;
            last_input_ = integral_;
            last_output_ = out;
        }
        ready_.push_back(static_cast<float>(out));
    }
    deltas_.erase(deltas_.begin(), deltas_.begin() + static_cast<std::ptrdiff_t>(std::min(count, deltas_.size())));
    base_ = end;
    if (available() > max_buffered_) {
        const std::size_t excess = available() - max_buffered_;
        ready_start_ += excess;
        dropped_ += excess;
    }
}

std::size_t AudioSynth::read(std::span<float> out) {
    const std::size_t count = std::min(out.size(), available());
    std::copy_n(ready_.begin() + static_cast<std::ptrdiff_t>(ready_start_), count, out.begin());
    ready_start_ += count;
    if (ready_start_ == ready_.size()) {
        ready_.clear();
        ready_start_ = 0;
    }
    return count;
}

}  // namespace freetvc
