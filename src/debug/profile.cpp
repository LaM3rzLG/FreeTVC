// SPDX-License-Identifier: GPL-3.0-or-later
#include "debug/profile.hpp"

#include <algorithm>

#include "debug/z80_decode.hpp"

namespace freetvc::debug {

namespace {

// The target of a taken CALL or RST from its bytes (a DD/FD prefix may come first)
std::uint16_t call_target(const std::array<std::uint8_t, 4>& bytes, std::uint8_t length) {
    if (length >= 3) {
        const std::uint8_t op = bytes[length - 3];
        if (op == 0xCD || (op & 0xC7) == 0xC4) {
            return static_cast<std::uint16_t>(bytes[length - 2] | bytes[length - 1] << 8);
        }
    }
    return static_cast<std::uint16_t>(bytes[length - 1] & 0x38);
}

}  // namespace

std::size_t Profile::index(BankAddress where) {
    if (where.bank == Bank::Open || where.offset >= kBankSize) {
        return ~std::size_t{0};
    }
    return static_cast<std::size_t>(where.bank) * kBankSize + where.offset;
}

std::uint32_t Profile::count(BankAddress where) const {
    const std::size_t i = index(where);
    return i < counts_.size() ? counts_[i] : 0;
}

std::uint64_t Profile::ticks(BankAddress where) const {
    const std::size_t i = index(where);
    return i < ticks_.size() ? ticks_[i] : 0;
}

std::uint64_t Profile::inclusive(std::size_t routine, std::uint64_t now) const {
    if (routine >= routines_.size()) {
        return 0;
    }
    std::uint64_t total = routines_[routine].inclusive;
    for (const Frame& f : stack_) {
        if (f.routine == routine && f.outermost && now >= f.start) {
            total += now - f.start;
        }
    }
    return total;
}

std::size_t Profile::routine(BankAddress entry) {
    const std::uint32_t key = static_cast<std::uint32_t>(entry.bank) << 16 | entry.offset;
    const auto [it, inserted] = by_entry_.try_emplace(key, routines_.size());
    if (inserted) {
        RoutineStats r;
        r.entry = entry;
        routines_.push_back(r);
        active_.push_back(0);
    }
    return it->second;
}

void Profile::pop() {
    const Frame f = stack_.back();
    stack_.pop_back();
    --active_[f.routine];
    if (f.outermost) {
        routines_[f.routine].inclusive += previous_ticks_ - f.start;
    }
}

void Profile::clear() {
    std::fill(counts_.begin(), counts_.end(), 0u);
    std::fill(ticks_.begin(), ticks_.end(), 0u);
    routines_.clear();
    active_.clear();
    by_entry_.clear();
    stack_.clear();
    started_ = false;
    total_ticks_ = 0;
    instructions_ = 0;
}

void Profile::resume() {
    stack_.clear();
    std::fill(active_.begin(), active_.end(), 0u);
    started_ = false;
}

void Profile::instruction(const Machine& machine, std::uint16_t pc, BankAddress where) {
    const std::uint64_t now = machine.ticks();
    const std::uint16_t sp = machine.cpu().registers().sp;
    if (counts_.empty()) {
        counts_.assign(kBankCount * kBankSize, 0);
        ticks_.assign(kBankCount * kBankSize, 0);
    }
    if (started_ && now < previous_ticks_) {
        resume();  // time went back (a snapshot or rewind): nothing to credit
    }
    if (!started_) {
        started_ = true;
        outside_ = routine({Bank::Open, 0});
    } else {
        // The previous instruction ran until now
        const std::uint64_t spent = now - previous_ticks_;
        total_ticks_ += spent;
        ++instructions_;
        if (const std::size_t i = index(previous_where_); i < counts_.size()) {
            ++counts_[i];
            ticks_[i] += spent;
        }
        routines_[previous_routine_].exclusive += spent;

        previous_ticks_ = now;
        while (!stack_.empty() && sp > stack_.back().sp) {
            pop();  // returned, or dropped its return address
        }
        // A taken CALL or RST pushed its return address; an interrupt right after it pushed another
        const bool interrupt = machine.entered_interrupt();
        const Decoded d = decode(previous_bytes_);
        const auto callee_sp = static_cast<std::uint16_t>(previous_sp_ - 2);
        if (d.flow == Flow::Call && (sp == callee_sp || (interrupt && sp == static_cast<std::uint16_t>(previous_sp_ - 4)))) {
            push(routine(machine.memory().locate(call_target(previous_bytes_, d.length))), callee_sp, now);
        }
        if (interrupt) {
            push(routine(where), sp, now);
        }
    }
    previous_ticks_ = now;
    previous_where_ = where;
    previous_sp_ = sp;
    for (unsigned i = 0; i < previous_bytes_.size(); ++i) {
        previous_bytes_[i] = machine.peek(static_cast<std::uint16_t>(pc + i));
    }
    previous_routine_ = stack_.empty() ? outside_ : stack_.back().routine;
}

void Profile::push(std::size_t routine, std::uint16_t sp, std::uint64_t now) {
    if (stack_.size() >= kMaxDepth) {
        --active_[stack_.front().routine];
        stack_.erase(stack_.begin());
    }
    const bool outermost = active_[routine] == 0;
    ++active_[routine];
    ++routines_[routine].calls;
    stack_.push_back({routine, sp, now, outermost});
}

}  // namespace freetvc::debug
