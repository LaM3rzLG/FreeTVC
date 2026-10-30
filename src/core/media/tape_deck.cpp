// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/media/tape_deck.hpp"

#include <algorithm>
#include <utility>

namespace freetvc {

void TapeDeck::insert(TapeSignal signal) {
    signal_ = std::move(signal);
    length_ = signal_.length();
    state_ = TapeState::Stopped;
    rewind();
}

void TapeDeck::insert_blank() { insert({}); }

void TapeDeck::eject() {
    stop();
    signal_ = {};
    state_ = TapeState::Empty;
    length_ = position_ = 0;
}

void TapeDeck::play() {
    if (loaded()) {
        stop();
        state_ = TapeState::Playing;
    }
}

void TapeDeck::record() {
    if (!loaded()) {
        return;
    }
    stop();
    // Cut the tape at the position: the run under it ends there
    seek();
    signal_.runs.resize(run_);
    signal_.program_ends.clear();
    signal_.program_autorun.clear();
    signal_.program_data.clear();
    if (position_ > run_start_) {
        signal_.append(run_level_, position_ - run_start_);
    }
    length_ = signal_.length();
    record_level_ = run_level_;
    record_edge_ = position_;
    state_ = TapeState::Recording;
}

void TapeDeck::stop() {
    if (state_ == TapeState::Recording) {
        flush_recording();
        state_ = TapeState::Stopped;
        run_ = 0;
        run_start_ = 0;
        run_level_ = signal_.first_level;
    } else if (state_ == TapeState::Playing) {
        state_ = TapeState::Stopped;
    }
}

void TapeDeck::rewind() {
    if (state_ == TapeState::Recording) {
        stop();
    }
    position_ = 0;
    run_ = 0;
    run_start_ = 0;
    run_level_ = signal_.first_level;
}

void TapeDeck::restore(TapeState state, bool motor, std::uint64_t position, std::uint64_t last_tick) {
    if (!loaded()) {
        return;
    }
    state_ = state == TapeState::Playing ? TapeState::Playing : TapeState::Stopped;
    motor_ = motor;
    position_ = std::min(position, length_);
    last_tick_ = last_tick;
    run_ = 0;
    run_start_ = 0;
    run_level_ = signal_.first_level;
    seek();
}

void TapeDeck::advance(std::uint64_t tick) {
    // The machine's clock restarts on power-on and when a saved state is restored
    if (tick > last_tick_ && moving()) {
        position_ += tick - last_tick_;
        if (state_ == TapeState::Playing && position_ > length_) {
            position_ = length_;
        }
    }
    last_tick_ = tick;
}

void TapeDeck::seek() {
    if (position_ < run_start_) {
        run_ = 0;
        run_start_ = 0;
        run_level_ = signal_.first_level;
    }
    while (run_ < signal_.runs.size() && run_start_ + signal_.runs[run_] <= position_) {
        run_start_ += signal_.runs[run_];
        run_level_ = !run_level_;
        ++run_;
    }
}

bool TapeDeck::tape_input(std::uint64_t tick) {
    advance(tick);
    if (state_ != TapeState::Playing || position_ >= length_) {
        return false;
    }
    seek();
    return run_level_;
}

void TapeDeck::flush_recording() {
    if (position_ > record_edge_) {
        signal_.append(record_level_, position_ - record_edge_);
        record_edge_ = position_;
        length_ = signal_.length();
    }
}

void TapeDeck::tape_output(std::uint64_t tick, bool level) {
    advance(tick);
    if (state_ == TapeState::Recording && motor_) {
        flush_recording();
    }
    record_level_ = level;
}

void TapeDeck::tape_motor(std::uint64_t tick, bool on) {
    advance(tick);
    if (state_ == TapeState::Recording && motor_ && !on) {
        flush_recording();
    }
    if (state_ == TapeState::Recording && !motor_ && on) {
        record_edge_ = position_;
    }
    motor_ = on;
}

}  // namespace freetvc
