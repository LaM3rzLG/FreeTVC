// SPDX-License-Identifier: GPL-3.0-or-later
// What the machine tells an attached tape deck, at the hardware's points (design/media.md "Tape deck").
#pragma once

#include <cstdint>

namespace freetvc {

class TapePort {
public:
    virtual ~TapePort() = default;
    // IN 59h: the signal level at the tape position (b5); `tick` is the machine's half-T time.
    virtual bool tape_input(std::uint64_t tick) = 0;
    // The output flip-flop (any access to 50h-57h) changed to `level`.
    virtual void tape_output(std::uint64_t tick, bool level) = 0;
    // Port 05h b6/b7: either motor bit moves the one deck.
    virtual void tape_motor(std::uint64_t tick, bool on) = 0;
};

}  // namespace freetvc
