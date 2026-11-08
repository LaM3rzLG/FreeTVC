// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/sdl/present_timing.hpp"

#include <algorithm>
#include <cmath>

namespace freetvc::sdl {

void PresentTiming::restart(std::uint64_t now_ns, std::uint32_t display, float refresh_hz) {
    if (mode_ != Mode::Vsync) {
        mode_ = Mode::Vsync;
        changed_ = true;
    }
    decided_ = false;
    display_ = display;
    refresh_hz_ = refresh_hz;
    since_ns_ = now_ns;
    last_ns_ = 0;
    next_ns_ = 0;
    intervals_.clear();
}

bool PresentTiming::take_change() {
    const bool c = changed_;
    changed_ = false;
    return c;
}

std::uint64_t PresentTiming::presented(std::uint64_t now_ns, std::uint32_t display, float refresh_hz) {
    if (since_ns_ == 0 || display != display_ || std::abs(refresh_hz - refresh_hz_) > 0.5f) {
        restart(now_ns, display, refresh_hz);
    }
    if (mode_ == Mode::SelfTimed) {
        const auto period = static_cast<std::uint64_t>(1e9 / refresh_hz_);
        next_ns_ = next_ns_ == 0 || now_ns > next_ns_ + period ? now_ns + period : next_ns_ + period;
        return next_ns_ > now_ns ? next_ns_ - now_ns : 0;
    }
    if (decided_ || refresh_hz_ <= 0.0f || now_ns - since_ns_ < kSettleNs) {
        return 0;
    }
    if (last_ns_ != 0) {
        intervals_.push_back(now_ns - last_ns_);
    }
    last_ns_ = now_ns;
    if (now_ns - since_ns_ < kSettleNs + kMeasureNs || intervals_.size() < 10) {
        return 0;
    }
    // The median ignores single stalls
    auto mid = intervals_.begin() + static_cast<std::ptrdiff_t>(intervals_.size() / 2);
    std::nth_element(intervals_.begin(), mid, intervals_.end());
    measured_hz_ = 1e9 / static_cast<double>(*mid);
    decided_ = true;
    intervals_.clear();
    if (std::abs(measured_hz_ / refresh_hz_ - 1.0) > kTolerance) {
        mode_ = Mode::SelfTimed;
        changed_ = true;
    }
    return 0;
}

}  // namespace freetvc::sdl
