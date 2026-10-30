// SPDX-License-Identifier: GPL-3.0-or-later
#include "debug/debugger.hpp"

#include <algorithm>
#include <utility>

#include "debug/z80_decode.hpp"

namespace freetvc::debug {

namespace {

constexpr std::uint8_t kMemoryKinds = kAccessExec | kAccessRead | kAccessWrite;
constexpr std::uint8_t kPortKinds = kAccessIn | kAccessOut;
constexpr std::uint8_t kUnbankedKinds = kPortKinds | kAccessLine;

std::array<std::uint8_t, 4> bytes_at(const Machine& m, std::uint16_t address) {
    std::array<std::uint8_t, 4> bytes{};
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        bytes[i] = m.peek(static_cast<std::uint16_t>(address + i));
    }
    return bytes;
}

}  // namespace

Debugger::Debugger(Machine& machine) : machine_(machine) {
    hit_log_.resize(kDefaultHitLog);
    machine_.set_debug_hooks(this);
}

Debugger::~Debugger() {
    if (machine_.debug_hooks() == this) {
        machine_.set_debug_hooks(nullptr);
    }
}

unsigned Debugger::add(Checkpoint checkpoint) {
    checkpoint.id = next_id_++;
    checkpoint.hits = 0;
    checkpoints_.push_back(std::move(checkpoint));
    refresh_flags();
    return checkpoints_.back().id;
}

bool Debugger::remove(unsigned id) {
    const auto it = std::find_if(checkpoints_.begin(), checkpoints_.end(), [id](const Checkpoint& c) { return c.id == id; });
    if (it == checkpoints_.end()) {
        return false;
    }
    checkpoints_.erase(it);
    refresh_flags();
    return true;
}

bool Debugger::update(const Checkpoint& checkpoint) {
    for (Checkpoint& c : checkpoints_) {
        if (c.id == checkpoint.id) {
            const std::uint64_t hits = c.hits;
            c = checkpoint;
            c.hits = hits;
            refresh_flags();
            return true;
        }
    }
    return false;
}

const Checkpoint* Debugger::find(unsigned id) const {
    for (const Checkpoint& c : checkpoints_) {
        if (c.id == id) {
            return &c;
        }
    }
    return nullptr;
}

void Debugger::clear_checkpoints() {
    checkpoints_.clear();
    refresh_flags();
}

void Debugger::refresh_flags() {
    memory_flags.fill(0);
    port_flags.fill(0);
    line_flags.fill(0);
    watch_interrupts = false;
    watch_lines = false;
    for (const Checkpoint& c : checkpoints_) {
        if (!c.enabled) {
            continue;
        }
        const std::uint8_t memory = c.access & kMemoryKinds;
        const std::uint8_t ports = c.access & kPortKinds;
        const bool lines = (c.access & kAccessLine) != 0;
        watch_interrupts |= (c.access & kAccessIrq) != 0;
        watch_lines |= lines;
        if (c.offsets && c.bank) {
            // Every CPU address the bank can be paged at; check() then compares bank and offset
            const Bank b = *c.bank;
            const bool ext = b == Bank::Ext;
            const bool card = b >= Bank::Card0 && b <= Bank::Card3;
            const unsigned size = ext || card ? 0x2000 : 0x4000;
            for (unsigned base = ext ? 0xE000 : card ? 0xC000 : 0; base < 0x10000; base += 0x4000) {
                for (unsigned o = c.start; o <= c.end && o < size; ++o) {
                    memory_flags[base + o] |= memory;
                }
            }
            continue;
        }
        for (unsigned a = c.start; a <= c.end; ++a) {
            memory_flags[a] |= memory;
            if (a < port_flags.size()) {
                port_flags[a] |= ports;
            }
            if (lines && a < line_flags.size()) {
                line_flags[a] = 1;
            }
        }
    }
    if (target_ && target_->address) {
        memory_flags[*target_->address] |= kAccessExec;
    }
    checkpoint_flags_ = memory_flags;
    if (heat_on_) {
        for (std::uint8_t& f : memory_flags) {
            f |= kAccessRead | kAccessWrite;
        }
    }
    checkpoint_port_flags_ = port_flags;
    checkpoint_interrupts_ = watch_interrupts;
    if (events_on_) {
        port_flags.fill(kAccessIn | kAccessOut);
        watch_interrupts = true;
    }
    every_instruction = trace_.capacity() > 0 || step_out_sp_.has_value() || (target_ && !target_->address) || heat_on_ || profile_on_;
}

void Debugger::set_heat(bool on) {
    if (on == heat_on_) {
        return;
    }
    heat_on_ = on;
    if (on) {
        heat_.allocate();
    }
    refresh_flags();
}

void Debugger::set_profile(bool on) {
    if (on == profile_on_) {
        return;
    }
    profile_on_ = on;
    profile_.resume();
    refresh_flags();
}

void Debugger::set_events(bool on) {
    if (on == events_on_) {
        return;
    }
    events_on_ = on;
    events_.clear();
    if (on && io_log_.capacity() == 0) {
        io_log_.resize(kIoLog);
    }
    refresh_flags();
}

void Debugger::record_event(const Machine& machine, BeamEvent::Kind kind, std::uint8_t port, std::uint8_t value, std::uint16_t pc) {
    const Video& video = machine.video();
    const BeamEvent event{kind, port, value, video.crtc().selected(), static_cast<std::uint16_t>(video.beam_line()),
                          static_cast<std::uint16_t>(video.beam_column()), pc};
    events_.record(video.frame_count(), event);
    io_log_.push({machine.ticks(), video.frame_count(), event});
}

bool Debugger::check(Access kind, std::uint16_t address, std::uint8_t value, BankAddress where, std::uint16_t pc) {
    bool stop = false;
    for (Checkpoint& c : checkpoints_) {
        if (!c.enabled || !(c.access & kind)) {
            continue;
        }
        if (c.offsets && c.bank) {
            if ((kind & kMemoryKinds) == 0 || where.bank != *c.bank || where.offset < c.start || where.offset > c.end) {
                continue;
            }
        } else if (address < c.start || address > c.end ||
                   (c.bank && (kind & kUnbankedKinds) == 0 && *c.bank != where.bank)) {
            continue;
        }
        const Hit hit{c.id, kind, address, value, where, pc, machine_.ticks()};
        if (c.condition && !c.condition(machine_, hit)) {
            continue;
        }
        ++c.hits;
        if (c.ignore > 0) {
            --c.ignore;
            continue;
        }
        hit_log_.push(hit);
        if (c.stop) {
            stop_hits_.push_back(hit);
            stop = true;
        }
    }
    return stop;
}

bool Debugger::on_instruction(const Machine& machine, std::uint16_t pc, bool restart) {
    bool stop = false;
    const BankAddress where = machine.memory().locate(pc);
    if (heat_on_) {
        instruction_start_ = pc;
        instruction_length_ = decode(bytes_at(machine, pc)).length;
        heat_.mark(where, HeatMap::Exec, static_cast<std::uint32_t>(machine.frame_count() + 1));
    }
    if (profile_on_) {
        profile_.instruction(machine, pc, where);
    }
    if (trace_.capacity() > 0) {
        TraceEntry entry;
        entry.ticks = machine.ticks();
        entry.where = where;
        entry.bytes = bytes_at(machine, pc);
        entry.regs = machine.cpu().registers();
        entry.regs.pc = pc;
        entry.interrupt = machine.entered_interrupt();
        trace_.push(entry);
    }
    if (step_out_sp_) {
        if (last_was_return_ && machine.cpu().registers().sp > *step_out_sp_) {
            target_reached_ = true;
            stop = true;
        }
        last_was_return_ = decode(bytes_at(machine, pc)).flow == Flow::Return;
    }
    if (watch_interrupts && machine.entered_interrupt()) {
        if (events_on_) {
            record_event(machine, BeamEvent::Irq, 0, 0, pc);
        }
        if (checkpoint_interrupts_) {
            stop |= check(kAccessIrq, pc, machine.peek(pc), where, pc);
        }
    }
    if (restart) {
        return stop;
    }
    const bool back_from_call = target_ && !target_->address && machine.cpu().registers().sp >= *target_->min_sp;
    if (back_from_call || (target_ && target_->address && pc == *target_->address && (!target_->bank || *target_->bank == where.bank) &&
                           (!target_->min_sp || machine.cpu().registers().sp >= *target_->min_sp))) {
        target_reached_ = true;
        stop = true;
    }
    if (memory_flags[pc] & kAccessExec) {
        stop |= check(kAccessExec, pc, machine.peek(pc), where, pc);
    }
    return stop;
}

bool Debugger::on_access(const Machine& machine, Access kind, std::uint16_t address, std::uint8_t value) {
    const BankAddress where = (kind & kUnbankedKinds) ? BankAddress{} : machine.memory().locate(address);
    if (heat_on_ && (kind == kAccessRead || kind == kAccessWrite)) {
        const bool operand = kind == kAccessRead && static_cast<std::uint16_t>(address - instruction_start_) < instruction_length_;
        const HeatMap::Kind heat = operand ? HeatMap::Exec : kind == kAccessRead ? HeatMap::Read : HeatMap::Write;
        heat_.mark(where, heat, static_cast<std::uint32_t>(machine.frame_count() + 1));
        if ((checkpoint_flags_[address] & kind) == 0) {
            return false;
        }
    }
    if (events_on_ && (kind == kAccessIn || kind == kAccessOut)) {
        record_event(machine, kind == kAccessIn ? BeamEvent::In : BeamEvent::Out, static_cast<std::uint8_t>(address), value, machine.pc());
        if ((checkpoint_port_flags_[address & 0xFF] & kind) == 0) {
            return false;
        }
    }
    return check(kind, address, value, where, machine.pc());
}

Stop Debugger::execute(const RunLimits& limits) {
    stop_hits_.clear();
    target_reached_ = false;
    Stop stop;
    stop.run = machine_.run(limits);
    stop.pc = machine_.pc();
    stop.hits = std::move(stop_hits_);
    stop_hits_.clear();
    switch (stop.run.reason) {
    case StopReason::TickBudget:
        stop.why = Why::Budget;
        break;
    case StopReason::InstructionCount:
        stop.why = Why::Step;
        break;
    case StopReason::Debugger:
        stop.why = stop.hits.empty() ? Why::Step : Why::Checkpoint;
        break;
    }
    for (const Hit& hit : stop.hits) {
        const Checkpoint* c = find(hit.checkpoint);
        if (c && c->temporary) {
            remove(hit.checkpoint);
        }
    }
    if (stop.why != Why::Budget) {
        cancel_step();
    }
    return stop;
}

Stop Debugger::run(std::uint64_t max_ticks, bool to_next_frame) {
    return execute({.max_ticks = max_ticks, .to_next_frame = to_next_frame});
}

Stop Debugger::step(std::uint64_t count, std::uint64_t max_ticks) {
    return execute({.max_ticks = max_ticks, .max_instructions = count});
}

bool Debugger::start_step_over() {
    cancel_step();
    if (!machine_.at_boundary() || machine_.halted()) {
        return false;
    }
    const std::uint16_t pc = machine_.pc();
    const std::array<std::uint8_t, 4> bytes = bytes_at(machine_, pc);
    const Decoded d = decode(bytes);
    if (d.flow != Flow::Call && d.flow != Flow::Repeat && d.flow != Flow::Halt) {
        return false;
    }
    const std::uint16_t sp = machine_.cpu().registers().sp;
    target_ = d.flow == Flow::Call ? Target{std::nullopt, std::nullopt, sp} : Target{static_cast<std::uint16_t>(pc + d.length), std::nullopt, sp};
    refresh_flags();
    return true;
}

void Debugger::start_step_out() {
    cancel_step();
    step_out_sp_ = machine_.cpu().registers().sp;
    last_was_return_ = decode(bytes_at(machine_, machine_.pc())).flow == Flow::Return;
    refresh_flags();
}

void Debugger::start_run_to(std::uint16_t address, std::optional<Bank> bank) {
    cancel_step();
    target_ = Target{address, bank, std::nullopt};
    refresh_flags();
}

void Debugger::cancel_step() {
    if (target_ || step_out_sp_) {
        target_.reset();
        step_out_sp_.reset();
        refresh_flags();
    }
}

Stop Debugger::step_over(std::uint64_t max_ticks) {
    if (!start_step_over()) {
        return step(1, max_ticks);
    }
    Stop stop = run(max_ticks);
    cancel_step();
    return stop;
}

Stop Debugger::step_out(std::uint64_t max_ticks) {
    start_step_out();
    Stop stop = run(max_ticks);
    cancel_step();
    return stop;
}

Stop Debugger::run_to(std::uint16_t address, std::optional<Bank> bank, std::uint64_t max_ticks) {
    start_run_to(address, bank);
    Stop stop = run(max_ticks);
    cancel_step();
    return stop;
}

Z80Registers Debugger::registers() const {
    Z80Registers regs = machine_.cpu().registers();
    regs.pc = machine_.pc();
    return regs;
}

bool Debugger::set_registers(const Z80Registers& regs) {
    if (!machine_.at_boundary()) {
        return false;
    }
    const std::uint16_t pc = machine_.pc();
    machine_.cpu().set_registers(regs);
    if (regs.pc != pc) {
        machine_.jump(regs.pc);
    }
    return true;
}

void Debugger::set_trace_capacity(std::size_t entries) {
    trace_.resize(entries);
    refresh_flags();
}

void Debugger::save_state(const std::string& name) { states_.insert_or_assign(name, machine_); }

bool Debugger::load_state(const std::string& name) {
    const auto it = states_.find(name);
    if (it == states_.end()) {
        return false;
    }
    machine_.restore(it->second);
    return true;
}

bool Debugger::delete_state(const std::string& name) { return states_.erase(name) > 0; }

std::vector<std::string> Debugger::state_names() const {
    std::vector<std::string> names;
    for (const auto& entry : states_) {
        names.push_back(entry.first);
    }
    return names;
}

}  // namespace freetvc::debug
