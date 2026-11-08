// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <cstdint>
#include <vector>

namespace freetvc::sdl {

// Chooses between vsync and timing presents itself at the display's rate (design/ui.md "Frame pacing"):
// vsync stays while presents follow the display; capped below it (macOS OpenGL: 60 Hz on a 180 Hz display)
// or running far above it (vsync ignored), the loop waits for each refresh itself.
class PresentTiming {
public:
    enum class Mode { Vsync, SelfTimed };

    // After each present: host time, the window's display and its refresh rate (0 = unknown).
    // Returns how long to wait before the next pass, in ns.
    std::uint64_t presented(std::uint64_t now_ns, std::uint32_t display, float refresh_hz);
    Mode mode() const { return mode_; }
    // True once after the mode changed: the caller switches vsync to mode() == Vsync.
    bool take_change();
    // The median present rate the decision was made from, in Hz.
    double measured_hz() const { return measured_hz_; }

private:
    static constexpr std::uint64_t kSettleNs = 500'000'000;   // ignored after a start or display change
    static constexpr std::uint64_t kMeasureNs = 1'000'000'000;
    static constexpr double kTolerance = 0.12;                // present rate this far from the display's: not following it

    void restart(std::uint64_t now_ns, std::uint32_t display, float refresh_hz);

    Mode mode_ = Mode::Vsync;
    bool changed_ = false;
    bool decided_ = false;
    std::uint32_t display_ = 0;
    float refresh_hz_ = 0.0f;
    std::uint64_t since_ns_ = 0;
    std::uint64_t last_ns_ = 0;
    std::uint64_t next_ns_ = 0;
    std::vector<std::uint64_t> intervals_;
    double measured_hz_ = 0.0;
};

}  // namespace freetvc::sdl
