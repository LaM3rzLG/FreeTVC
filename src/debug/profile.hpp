// SPDX-License-Identifier: GPL-3.0-or-later
// Execution profile: T-states per instruction address and per routine through a shadow call stack (design/ui.md "Bench", 7h).
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "core/machine/machine.hpp"

namespace freetvc::debug {

struct RoutineStats {
    BankAddress entry;            // Bank::Open: time outside every call seen since recording started
    std::uint64_t calls = 0;
    std::uint64_t exclusive = 0;  // ticks (half-T) in its own instructions
    std::uint64_t inclusive = 0;  // ticks of its finished outermost calls, callees included
};

class Profile {
public:
    static constexpr std::size_t kBankSize = 0x4000;
    static constexpr std::size_t kMaxDepth = 64;  // deeper calls drop the oldest frame (code that never returns)

    // Every instruction start while recording; the time since the previous start is that instruction's.
    void instruction(const Machine& machine, std::uint16_t pc, BankAddress where);
    // Forgets everything; the next instruction starts a new recording.
    void clear();
    // After a pause in recording: keeps the counts, forgets the calls in progress and the time in between.
    void resume();

    std::uint32_t count(BankAddress where) const;
    std::uint64_t ticks(BankAddress where) const;
    const std::vector<RoutineStats>& routines() const { return routines_; }
    // Inclusive ticks with the running part of an outermost call still on the stack, up to `now`.
    std::uint64_t inclusive(std::size_t routine, std::uint64_t now) const;
    std::uint64_t total_ticks() const { return total_ticks_; }
    std::uint64_t instructions() const { return instructions_; }
    std::size_t depth() const { return stack_.size(); }

private:
    struct Frame {
        std::size_t routine = 0;
        std::uint16_t sp = 0;  // SP inside the routine at its entry; a higher SP has left it
        std::uint64_t start = 0;
        bool outermost = false;  // no deeper frame of the same routine was active at entry
    };
    static std::size_t index(BankAddress where);
    std::size_t routine(BankAddress entry);
    void pop();
    void push(std::size_t routine, std::uint16_t sp, std::uint64_t now);

    std::vector<std::uint32_t> counts_;
    std::vector<std::uint64_t> ticks_;
    std::vector<RoutineStats> routines_;
    std::vector<unsigned> active_;  // frames of each routine on the stack
    std::unordered_map<std::uint32_t, std::size_t> by_entry_;
    std::vector<Frame> stack_;

    bool started_ = false;
    std::uint64_t previous_ticks_ = 0;
    BankAddress previous_where_;
    std::size_t previous_routine_ = 0;
    std::size_t outside_ = 0;  // the routine entry of time outside every call
    std::uint16_t previous_sp_ = 0;
    std::array<std::uint8_t, 4> previous_bytes_{};
    std::uint64_t total_ticks_ = 0;
    std::uint64_t instructions_ = 0;
};

}  // namespace freetvc::debug
