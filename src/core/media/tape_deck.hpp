// SPDX-License-Identifier: GPL-3.0-or-later
// The tape deck (design/media.md "Tape deck"): a TapeSignal played to 59h b5 and recorded from the 50h-57h
// flip-flop. The tape moves while PLAY or RECORD is down and the motor is on; its position follows the machine's ticks.
#pragma once

#include <cstddef>
#include <cstdint>

#include "core/machine/tape_port.hpp"
#include "core/media/tape_signal.hpp"

namespace freetvc {

enum class TapeState { Empty, Stopped, Playing, Recording };

class TapeDeck final : public TapePort {
public:
    // At the start, stopped.
    void insert(TapeSignal signal);
    // A blank tape, stopped at the start.
    void insert_blank();
    void eject();
    bool loaded() const { return state_ != TapeState::Empty; }

    void play();
    // Records from the current position on; what was after it is lost.
    void record();
    void stop();
    void rewind();
    // Snapshot load: the same tape is already in; a recording comes back stopped.
    void restore(TapeState state, bool motor, std::uint64_t position, std::uint64_t last_tick);
    std::uint64_t last_tick() const { return last_tick_; }

    TapeState state() const { return state_; }
    bool motor() const { return motor_; }
    bool moving() const { return motor_ && (state_ == TapeState::Playing || state_ == TapeState::Recording); }
    std::uint64_t position() const { return position_; }
    std::uint64_t length() const { return length_; }
    // The tape, with what was recorded on it.
    const TapeSignal& signal() const { return signal_; }

    bool tape_input(std::uint64_t tick) override;
    void tape_output(std::uint64_t tick, bool level) override;
    void tape_motor(std::uint64_t tick, bool on) override;

private:
    void advance(std::uint64_t tick);
    // Moves the read cursor to the run that holds position_
    void seek();
    void flush_recording();

    TapeSignal signal_;
    TapeState state_ = TapeState::Empty;
    bool motor_ = false;
    std::uint64_t position_ = 0;
    std::uint64_t length_ = 0;
    std::uint64_t last_tick_ = 0;

    std::size_t run_ = 0;         // read cursor: the run that holds position_
    std::uint64_t run_start_ = 0;
    bool run_level_ = false;

    bool record_level_ = false;   // the output level since record_edge_
    std::uint64_t record_edge_ = 0;
};

}  // namespace freetvc
