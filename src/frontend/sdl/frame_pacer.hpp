// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>

#include "core/video/framebuffer.hpp"
#include "core/video/video.hpp"

namespace freetvc::sdl {

// Keeps the last completed frames and hands them to the display on an even clock that trails the machine
// (design/ui.md "Frame pacing"), so a pass that completes two frames does not drop one.
class FramePacer final : public FrameSink {
public:
    static constexpr std::uint64_t kFrameNs = 20'096'000;  // 62 800 T at 3.125 MHz
    static constexpr double kLag = 1.5;                    // frames between the newest frame and the one shown

    void set_next(FrameSink* next) { next_ = next; }
    // The display's refresh rate, or 0: with one, the display clock moves in whole refreshes, not by the jittery time of each pick
    void set_refresh(double hz) { refresh_hz_ = hz; }
    void frame_done(const Framebuffer& frame) override;

    // Forgets the queue: the next pick starts again from the newest frame.
    void reset();
    // The frame to show at host time now_ns, or nullptr when none is queued; machine_frames = Machine::frame_count().
    const Framebuffer* pick(std::uint64_t now_ns, std::uint64_t machine_frames);
    // The machine's number of the frame pick() returned.
    std::uint64_t shown_frame() const { return shown_frame_; }
    // The frame after it, or nullptr when not yet completed; `mix` = how far the display clock is towards it (0-1)
    const Framebuffer* next_frame(float& mix) const;
    std::uint64_t late_count() const { return late_; }

private:
    struct Slot {
        std::uint64_t index = 0;
        Framebuffer frame;
    };
    static constexpr std::size_t kSlots = 8;

    std::array<Slot, kSlots> slots_{};
    FrameSink* next_ = nullptr;
    std::uint64_t produced_ = 0;    // frames received since the last reset
    std::uint64_t arrival_ns_ = 0;  // host time the newest frame was first seen by pick()
    std::uint64_t seen_ = 0;        // produced_ at the last pick
    std::uint64_t base_ = 0;        // machine_frames - produced_, constant while the machine runs on
    bool started_ = false;
    double pos_ = 0.0;              // the display clock, in frames
    std::uint64_t last_ns_ = 0;
    std::uint64_t shown_frame_ = 0;
    std::uint64_t late_ = 0;        // picks that wanted a frame not yet completed
    std::uint64_t shown_ = 0;       // the queue index shown last
    float mix_ = 0.0f;              // pos_ past shown_, 0-1
    double refresh_hz_ = 0.0;
};

}  // namespace freetvc::sdl
