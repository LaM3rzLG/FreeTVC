// SPDX-License-Identifier: GPL-3.0-or-later
// The debug API every front end uses: checkpoints, stepping, trace, saved states (design/debug-and-remote.md "Debug core").
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "core/machine/machine.hpp"
#include "debug/profile.hpp"

namespace freetvc::debug {

struct Hit {
    unsigned checkpoint = 0;
    Access access = kAccessExec;
    std::uint16_t address = 0;  // CPU address, or the port for in/out
    std::uint8_t value = 0;     // byte read or written; the opcode for exec
    BankAddress where;          // memory kinds only; OPEN for ports
    std::uint16_t pc = 0;       // start of the instruction that made the access
    std::uint64_t ticks = 0;
};

struct Checkpoint {
    unsigned id = 0;                  // assigned by Debugger::add
    std::uint8_t access = 0;          // Access bits: exec/read/write/irq take CPU addresses, in/out ports, line rows
    std::uint16_t start = 0;          // inclusive range
    std::uint16_t end = 0;
    std::optional<Bank> bank;         // memory kinds: only where the address maps to this bank
    bool offsets = false;             // with a bank: start/end are offsets in it, wherever it is paged
    bool enabled = true;
    bool stop = true;                 // false: hits are only logged
    bool temporary = false;           // deleted after it stops the machine
    std::uint32_t ignore = 0;         // hits still to pass over
    std::uint64_t hits = 0;           // hits counted, ignored ones included
    std::function<bool(const Machine&, const Hit&)> condition;  // empty = always
    std::string condition_text;                                  // the expression behind `condition`, for listings
};

struct TraceEntry {
    std::uint64_t ticks = 0;
    BankAddress where;
    std::array<std::uint8_t, 4> bytes{};
    Z80Registers regs{};      // before the instruction; regs.pc is its address
    bool interrupt = false;   // entered through an interrupt acknowledge
};

enum class Why : std::uint8_t {
    Budget,      // the tick budget ran out
    Step,        // the step count, step over, step out or run-to target was reached
    Checkpoint,  // a checkpoint stopped it; see Stop::hits
};

struct Stop {
    Why why = Why::Budget;
    StopInfo run;
    std::uint16_t pc = 0;      // Machine::pc() after the stop
    std::vector<Hit> hits;     // stopping hits of the last instruction
};

template <typename T>
class Ring {
public:
    void resize(std::size_t capacity) {
        items_.assign(capacity, T{});
        next_ = 0;
        size_ = 0;
    }
    void push(const T& item) {
        if (items_.empty()) {
            return;
        }
        items_[next_] = item;
        next_ = (next_ + 1) % items_.size();
        size_ = std::min(size_ + 1, items_.size());
        ++pushed_;
    }
    std::size_t capacity() const { return items_.size(); }
    // Items pushed since construction, dropped ones included; for readers that want only new items.
    std::uint64_t pushed() const { return pushed_; }
    std::size_t size() const { return size_; }
    void clear() { next_ = size_ = 0; }
    // The i-th kept item, oldest first; i < size().
    const T& at(std::size_t i) const { return items_[(next_ + items_.size() - size_ + i) % items_.size()]; }
    // The newest `count` items, oldest first.
    std::vector<T> last(std::size_t count) const {
        count = std::min(count, size_);
        std::vector<T> out;
        out.reserve(count);
        for (std::size_t i = 0; i < count; ++i) {
            out.push_back(items_[(next_ + items_.size() - count + i) % items_.size()]);
        }
        return out;
    }

private:
    std::vector<T> items_;
    std::size_t next_ = 0;
    std::size_t size_ = 0;
    std::uint64_t pushed_ = 0;
};

// The frame of the last exec / read / write of every physical byte, for heat views (design/ui.md "Bench").
class HeatMap {
public:
    enum Kind : unsigned { Exec, Read, Write };
    static constexpr std::size_t kBankSize = 0x4000;

    // frame_count() + 1 at the last access of this kind; 0 = none recorded
    std::uint32_t at(BankAddress where, Kind kind) const {
        const std::size_t i = index(where, kind);
        return i < stamps_.size() ? stamps_[i] : 0;
    }
    void mark(BankAddress where, Kind kind, std::uint32_t stamp) {
        const std::size_t i = index(where, kind);
        if (i < stamps_.size()) {
            stamps_[i] = stamp;
        }
    }
    void allocate() { stamps_.resize(kBankCount * kBankSize * 3); }

private:
    static std::size_t index(BankAddress where, Kind kind) {
        if (where.bank == Bank::Open || where.offset >= kBankSize) {
            return ~std::size_t{0};
        }
        return (static_cast<std::size_t>(where.bank) * kBankSize + where.offset) * 3 + kind;
    }

    std::vector<std::uint32_t> stamps_;
};

// A port access or interrupt entry with the beam position it happened at (design/ui.md "Bench", 7c).
struct BeamEvent {
    enum Kind : std::uint8_t { Out, In, Irq };
    Kind kind = Out;
    std::uint8_t port = 0;
    std::uint8_t value = 0;
    std::uint8_t crtc_register = 0;  // the selected CRTC register at the access
    std::uint16_t line = 0;          // Video::beam_line()
    std::uint16_t column = 0;        // Video::beam_column(), in characters
    std::uint16_t pc = 0;            // the instruction that made the access, or the handler's first one
};

// A beam event kept in the I/O log, with its time (design/ui.md "Bench", 7f).
struct IoEvent {
    std::uint64_t ticks = 0;
    std::uint64_t frame = 0;  // Video::frame_count() at the event
    BeamEvent event;
};

// The events of the frame in progress and of the one before it, keyed by Video::frame_count().
class EventLog {
public:
    static constexpr std::size_t kMaxPerFrame = 4096;

    void record(std::uint64_t frame, const BeamEvent& event) {
        if (frame != current_frame_) {
            previous_.swap(current_);
            previous_dropped_ = frame == current_frame_ + 1 ? current_dropped_ : 0;
            if (frame != current_frame_ + 1) {
                previous_.clear();
            }
            previous_frame_ = frame - 1;
            current_.clear();
            current_dropped_ = 0;
            current_frame_ = frame;
        }
        if (current_.size() < kMaxPerFrame) {
            current_.push_back(event);
        } else {
            ++current_dropped_;
        }
    }
    // Events recorded while frame_count() was `frame`; empty if that frame is no longer kept.
    const std::vector<BeamEvent>& frame(std::uint64_t frame) const {
        static const std::vector<BeamEvent> kNone;
        return frame == current_frame_ ? current_ : frame == previous_frame_ ? previous_ : kNone;
    }
    std::size_t dropped(std::uint64_t frame) const {
        return frame == current_frame_ ? current_dropped_ : frame == previous_frame_ ? previous_dropped_ : 0;
    }
    void clear() {
        current_.clear();
        previous_.clear();
        current_frame_ = previous_frame_ = ~std::uint64_t{0};
    }

private:
    std::vector<BeamEvent> current_;
    std::vector<BeamEvent> previous_;
    std::uint64_t current_frame_ = ~std::uint64_t{0};
    std::uint64_t previous_frame_ = ~std::uint64_t{0};
    std::size_t current_dropped_ = 0;
    std::size_t previous_dropped_ = 0;
};

class Debugger final : public DebugHooks {
public:
    static constexpr std::size_t kDefaultHitLog = 1024;
    static constexpr std::size_t kIoLog = 65536;

    // Attaches to the machine; the destructor detaches.
    explicit Debugger(Machine& machine);
    ~Debugger() override;

    Machine& machine() { return machine_; }
    const Machine& machine() const { return machine_; }

    unsigned add(Checkpoint checkpoint);
    bool remove(unsigned id);
    // Replaces the settings of checkpoint `checkpoint.id`; its hit count is kept.
    bool update(const Checkpoint& checkpoint);
    const Checkpoint* find(unsigned id) const;
    const std::vector<Checkpoint>& checkpoints() const { return checkpoints_; }
    void clear_checkpoints();

    // Every call is bounded by max_ticks (half-T). A run never stops on the exec checkpoint it starts at, except
    // at the first instruction after a reset or power-on (break at reset = an exec checkpoint at 0000h).
    // to_next_frame: also end (as a budget stop) at the first boundary in a new frame.
    Stop run(std::uint64_t max_ticks, bool to_next_frame = false);
    // Into `count` instructions; mid-instruction, the first completes the current one.
    Stop step(std::uint64_t count, std::uint64_t max_ticks);
    // CALL and RST run until SP is back at the call's level, wherever that is (after RST 30h's function byte, RST 18h's
    // ops, an inline string); block repeats and HALT run to the next instruction.
    Stop step_over(std::uint64_t max_ticks);
    // Runs until a return leaves the current stack frame.
    Stop step_out(std::uint64_t max_ticks);
    Stop run_to(std::uint16_t address, std::optional<Bank> bank, std::uint64_t max_ticks);

    // The same targets kept across run() calls, for front ends that run in slices; any stop but the budget ends them.
    // start_step_over() returns false (and starts nothing) where step over is a single step.
    bool start_step_over();
    void start_step_out();
    void start_run_to(std::uint16_t address, std::optional<Bank> bank);
    void cancel_step();
    bool stepping() const { return target_.has_value() || step_out_sp_.has_value(); }

    // regs.pc is Machine::pc(), not z80.h's internal PC.
    Z80Registers registers() const;
    // Only at an instruction boundary; a new pc restarts the fetch there.
    bool set_registers(const Z80Registers& regs);

    // 0 turns the instruction trace off.
    void set_trace_capacity(std::size_t entries);
    const Ring<TraceEntry>& trace() const { return trace_; }
    // Every counted hit of a checkpoint that was not ignored, stopping or not.
    const Ring<Hit>& hit_log() const { return hit_log_; }

    // While on, every memory access calls into the debugger and is stamped in heat(); checkpoints are unaffected.
    void set_heat(bool on);
    bool heat_enabled() const { return heat_on_; }
    const HeatMap& heat() const { return heat_; }
    // While on, every port access and interrupt entry is kept in events() with the beam position, and in io_log().
    void set_events(bool on);
    bool events_enabled() const { return events_on_; }
    const EventLog& events() const { return events_; }
    // While on, every instruction is timed into profile(); off keeps the counts, on again resumes them.
    void set_profile(bool on);
    bool profile_enabled() const { return profile_on_; }
    const Profile& profile() const { return profile_; }
    void clear_profile() { profile_.clear(); }
    // The last kIoLog events recorded while events were on; kept when they go off.
    const Ring<IoEvent>& io_log() const { return io_log_; }
    void clear_io_log() { io_log_.clear(); }

    void save_state(const std::string& name);
    bool load_state(const std::string& name);
    bool delete_state(const std::string& name);
    std::vector<std::string> state_names() const;

    bool on_instruction(const Machine& machine, std::uint16_t pc, bool restart) override;
    bool on_access(const Machine& machine, Access kind, std::uint16_t address, std::uint8_t value) override;

private:
    struct Target {
        std::optional<std::uint16_t> address;  // empty: the first instruction back at min_sp (a call stepped over)
        std::optional<Bank> bank;
        std::optional<std::uint16_t> min_sp;  // step over: the stack must be back at the call's level
    };

    Stop execute(const RunLimits& limits);
    void refresh_flags();
    bool check(Access kind, std::uint16_t address, std::uint8_t value, BankAddress where, std::uint16_t pc);
    void record_event(const Machine& machine, BeamEvent::Kind kind, std::uint8_t port, std::uint8_t value, std::uint16_t pc);

    Machine& machine_;
    std::vector<Checkpoint> checkpoints_;
    unsigned next_id_ = 1;

    std::optional<Target> target_;
    std::optional<std::uint16_t> step_out_sp_;
    bool last_was_return_ = false;
    bool target_reached_ = false;
    std::vector<Hit> stop_hits_;

    Ring<TraceEntry> trace_;
    Ring<Hit> hit_log_;

    std::array<std::uint8_t, 0x10000> checkpoint_flags_{};  // memory_flags without the heat bits
    bool heat_on_ = false;
    HeatMap heat_;
    std::uint16_t instruction_start_ = 0;  // reads inside the current instruction count as exec
    std::uint8_t instruction_length_ = 0;
    std::array<std::uint8_t, 0x100> checkpoint_port_flags_{};  // port_flags without the event bits
    bool checkpoint_interrupts_ = false;
    bool events_on_ = false;
    EventLog events_;
    Ring<IoEvent> io_log_;
    bool profile_on_ = false;
    Profile profile_;
    std::map<std::string, Machine> states_;
};

}  // namespace freetvc::debug
