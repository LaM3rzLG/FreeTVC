// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/sdl/frame_pacer.hpp"

#include <algorithm>
#include <cmath>

namespace freetvc::sdl {

namespace {
constexpr double kTrackSeconds = 1.0;  // how fast the display clock follows the machine
constexpr double kResyncFrames = 4.0;  // further off than this, jump instead of gliding
}  // namespace

void FramePacer::frame_done(const Framebuffer& frame) {
    ++produced_;
    Slot& slot = slots_[produced_ % kSlots];
    slot.index = produced_;
    slot.frame.pixels = frame.pixels;
    if (next_) {
        next_->frame_done(frame);
    }
}

void FramePacer::reset() {
    produced_ = 0;
    seen_ = 0;
    started_ = false;
}

const Framebuffer* FramePacer::pick(std::uint64_t now_ns, std::uint64_t machine_frames) {
    if (started_ && machine_frames - produced_ != base_) {
        reset();  // a snapshot, rewind or reset moved the machine's frame count
    }
    if (produced_ == 0) {
        return nullptr;
    }
    if (produced_ != seen_) {
        seen_ = produced_;
        arrival_ns_ = now_ns;
    }
    // Where the machine is now, in frames: the newest frame plus the time since it came, at most one frame
    const double since = static_cast<double>(now_ns - arrival_ns_) / static_cast<double>(kFrameNs);
    const double target = static_cast<double>(produced_) + std::min(since, 1.0) - kLag;
    if (!started_) {
        started_ = true;
        base_ = machine_frames - produced_;
        pos_ = target;
        shown_ = 0;
    } else {
        double dt = static_cast<double>(now_ns - last_ns_) / 1e9;
        if (refresh_hz_ > 0.0) {
            dt = std::max(1.0, std::round(dt * refresh_hz_)) / refresh_hz_;
        }
        pos_ += dt * 1e9 / static_cast<double>(kFrameNs);
        const double error = target - pos_;
        pos_ = std::abs(error) > kResyncFrames ? target : pos_ + error * std::min(dt / kTrackSeconds, 1.0);
    }
    last_ns_ = now_ns;

    const std::uint64_t oldest = produced_ > kSlots ? produced_ - kSlots + 1 : 1;
    const double floor_pos = std::floor(pos_);
    std::uint64_t want = floor_pos < static_cast<double>(oldest) ? oldest : static_cast<std::uint64_t>(floor_pos);
    if (want > produced_) {
        ++late_;
        want = produced_;
    }
    // A present that comes late shows the next frame, not the one after it: at most one frame behind, the later ones catch up
    if (shown_ != 0 && want == shown_ + 2) {
        --want;
    }
    shown_ = want;
    shown_frame_ = base_ + want;
    mix_ = static_cast<float>(std::clamp(pos_ - static_cast<double>(want), 0.0, 1.0));
    return &slots_[want % kSlots].frame;
}

const Framebuffer* FramePacer::next_frame(float& mix) const {
    mix = mix_;
    return started_ && shown_ != 0 && shown_ < produced_ ? &slots_[(shown_ + 1) % kSlots].frame : nullptr;
}

}  // namespace freetvc::sdl
