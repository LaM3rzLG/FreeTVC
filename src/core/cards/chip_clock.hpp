// SPDX-License-Identifier: GPL-3.0-or-later
// A sound chip's own clock against the machine's half-T ticks, both counted from power-on (tick 0): exact, no drift.
#pragma once

#include <cstdint>

namespace freetvc {

struct ChipClock {
    static constexpr std::uint64_t kTickHz = 6'250'000;
    std::uint64_t hz;

    // The tick in which chip clock `c` begins
    constexpr std::uint64_t tick_of(std::uint64_t c) const { return c / hz * kTickHz + c % hz * kTickHz / hz; }
    // The last chip clock that begins in or before `tick`
    constexpr std::uint64_t last_clock_by(std::uint64_t tick) const {
        const std::uint64_t q = (tick + 1) / kTickHz;
        const std::uint64_t r = (tick + 1) % kTickHz;
        return r == 0 ? q * hz - 1 : q * hz + (r * hz - 1) / kTickHz;
    }
};

}  // namespace freetvc
