// SPDX-License-Identifier: GPL-3.0-or-later
// Text monitor: VICE-style commands over the debug API, one engine for the console, TCP and --moncommands
// (design/debug-and-remote.md "Text monitor"). Not thread-safe: the host thread calls everything.
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/input/key_script.hpp"
#include "core/media/media_control.hpp"
#include "debug/debugger.hpp"
#include "debug/expression.hpp"
#include "debug/source_map.hpp"
#include "debug/symbols.hpp"

namespace freetvc::remote {

// Bytes as one line: printable ASCII as is, \r \n \t \\ and \xNN for the rest
std::string escape_bytes(std::string_view bytes);

class Monitor {
public:
    static constexpr std::size_t kHistory = 65536;  // chis entries kept by default

    // Loads the built-in ROM symbols and turns the instruction history on. The machine runs until something enters.
    explicit Monitor(debug::Debugger& debugger);

    // In the monitor the machine waits, as in VICE.
    bool active() const { return active_; }
    // Completes the instruction in progress, enters the monitor and returns the report; empty if already in it.
    std::string enter(std::string_view why);
    // One command line; returns its output (lines end in '\n'). Commands may leave the monitor (g, x, n, ret, until).
    std::string execute(std::string_view line);
    // Outside the monitor: runs up to max_ticks in frame slices, typing keybuf text; a stop enters the monitor and its
    // report is appended to `report`. Returns the ticks run.
    std::uint64_t run(std::uint64_t max_ticks, std::string& report);
    // Hits of log-only (trace) checkpoints since the last call, one line each, at most `limit` lines.
    std::string take_trace_lines(std::size_t limit = 64);

    // VICE's prompt with the next instruction: "(C:$8003) ".
    std::string prompt() const;
    std::optional<int> quit_code() const { return quit_; }
    // Call after the host reconfigures the machine (new ROMs): reloads the ROM symbols.
    void machine_changed();

    // Typing and key presses while the machine runs (keybuf, MCP); return the frame after the last release.
    // queue_text returns nothing if a character has no key.
    std::optional<std::uint64_t> queue_text(std::u32string_view text);
    std::uint64_t queue_keys(std::span<const MatrixKey> keys, unsigned hold_frames);
    bool typing() const { return next_key_ < keys_.size(); }
    // The stop that last entered the monitor from run(); empty before the first one.
    const std::optional<debug::Stop>& last_stop() const { return last_stop_; }
    // How many stops have entered the monitor from run(); tells a new last_stop() from an old one.
    std::uint64_t stop_count() const { return stop_count_; }

    debug::SymbolTable& symbols() { return symbols_; }
    // Source lines from the debug files `ll` loaded (SLD, sjasmplus listing, SDCC, z88dk)
    const debug::SourceMap& sources() const { return sources_; }
    // "main.c:7  counter = twice(counter);" for a source line; the text as far as the file can be read
    std::string source_line_text(debug::SourceLine line);
    // Labels and source lines from a debug or label file (what `ll` does); nullopt and `error` if it cannot be read.
    std::optional<debug::SourceMap::Loaded> load_debug_file(const std::string& path, std::string& error);
    // One source line on (lstep / lnext): Why::Step when a new line came up, Why::Checkpoint on a stop, Why::Budget
    // after 100 frames without one. The machine must be held; nothing to do without source lines (Budget).
    debug::Stop step_line(bool over);
    debug::Debugger& debugger() { return dbg_; }
    // Program media for attach/detach/autostart; run() services it every slice. nullptr: none.
    void set_media(MediaControl* media) { media_ = media; }
    MediaControl* media() { return media_; }

private:
    struct Args;
    struct Place {
        std::optional<Bank> bank;  // with a bank: `address` is an offset in it
        std::uint16_t address = 0;
    };
    using Handler = std::string (Monitor::*)(Args&);
    struct Command {
        const char* names;  // "z step": first name and aliases
        const char* usage;
        const char* help;
        Handler handler;
    };
    static const std::vector<Command>& commands();

    // Execution
    std::string cmd_go(Args& a);
    std::string cmd_exit(Args& a);
    std::string cmd_step(Args& a);
    std::string cmd_line_step(Args& a);
    std::string cmd_line_next(Args& a);
    std::string line_steps(std::uint64_t count, bool over);
    std::string cmd_next(Args& a);
    std::string cmd_return(Args& a);
    std::string cmd_until(Args& a);
    std::string cmd_reset(Args& a);
    std::string cmd_quit(Args& a);
    // State
    std::string cmd_registers(Args& a);
    std::string cmd_memory(Args& a);
    std::string cmd_disassemble(Args& a);
    std::string cmd_write(Args& a);
    std::string cmd_fill(Args& a);
    std::string cmd_hunt(Args& a);
    std::string cmd_transfer(Args& a);
    std::string cmd_compare(Args& a);
    std::string cmd_bank(Args& a);
    std::string cmd_io(Args& a);
    std::string cmd_stopwatch(Args& a);
    std::string cmd_history(Args& a);
    std::string cmd_profile(Args& a);
    std::string cmd_snapshot(Args& a);
    // Checkpoints
    std::string cmd_break(Args& a);
    std::string cmd_watch(Args& a);
    std::string cmd_trace(Args& a);
    std::string cmd_delete(Args& a);
    std::string cmd_enable(Args& a);
    std::string cmd_disable(Args& a);
    std::string cmd_condition(Args& a);
    std::string cmd_ignore(Args& a);
    // Symbols, files, input
    std::string cmd_load_labels(Args& a);
    std::string cmd_add_label(Args& a);
    std::string cmd_delete_label(Args& a);
    std::string cmd_show_labels(Args& a);
    std::string cmd_clear_labels(Args& a);
    std::string cmd_source(Args& a);
    std::string cmd_bload(Args& a);
    std::string cmd_bsave(Args& a);
    std::string cmd_screenshot(Args& a);
    std::string cmd_keybuf(Args& a);
    std::string cmd_attach(Args& a);
    std::string cmd_detach(Args& a);
    std::string cmd_autostart(Args& a);
    std::string cmd_tape(Args& a);
    std::string cmd_cart(Args& a);
    std::string cmd_disk(Args& a);
    std::string cmd_gamecard(Args& a);
    std::string cmd_multichip(Args& a);
    std::string cmd_twodfx(Args& a);
    std::string cmd_sid(Args& a);
    std::string cmd_nanosd(Args& a);
    std::string cmd_printer(Args& a);
    std::string cmd_serial(Args& a);
    std::string cmd_sddual(Args& a);
    std::string cmd_card256(Args& a);
    // "on [slot] | off" for a card that needs only its image (the GameCard, the Multichip card)
    std::string switch_card(Args& a, const char* usage, unsigned default_slot, bool (MediaControl::*place_card)(bool, unsigned, std::string&),
                            std::string (MediaControl::*describe_card)() const);
    std::string cmd_dump(Args& a);
    std::string cmd_undump(Args& a);
    std::string cmd_rewind(Args& a);
    std::string cmd_radix(Args& a);
    std::string cmd_help(Args& a);

    std::string checkpoint_command(Args& a, std::uint8_t default_access, bool stop, const char* name);
    std::string describe(const debug::Checkpoint& c) const;
    std::string hit_line(const debug::Hit& hit, const char* verb) const;
    std::string stop_report(const debug::Stop& stop);
    // "label:" line(s) and ".C:8003  3C           INC A" for the instruction at `address`; returns its length
    std::string listing_line(std::uint16_t address, std::uint8_t* length = nullptr) const;
    std::string next_steps(std::uint64_t count);
    std::string leave();

    debug::ParseOptions parse_options() const;
    std::int64_t value(std::string_view text) const;
    Place place(std::string_view text) const;
    // Banked places stop with an error at the bank's end instead of reading open bus or dropping writes
    std::uint8_t read(const Place& p, unsigned offset) const;
    std::string bank_end(Bank bank) const;
    void write(const Place& p, unsigned offset, std::uint8_t value);

    debug::Debugger& dbg_;
    Machine& machine_;
    debug::SymbolTable symbols_;
    debug::SourceMap sources_;
    std::map<std::string, std::vector<std::string>> source_files_;  // read for source_line_text; cleared by ll
    bool active_ = false;
    unsigned radix_ = 16;
    std::optional<int> quit_;
    std::uint64_t pending_next_ = 0;  // `n count`: steps over still to do after the current one
    std::optional<Place> memory_cursor_;
    std::optional<std::uint16_t> listing_cursor_;
    std::uint64_t stopwatch_ = 0;
    std::uint64_t trace_seen_ = 0;
    std::vector<KeyEvent> keys_;
    std::size_t next_key_ = 0;
    std::optional<debug::Stop> last_stop_;
    std::uint64_t stop_count_ = 0;
    MediaControl* media_ = nullptr;
};

}  // namespace freetvc::remote
