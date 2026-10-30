// SPDX-License-Identifier: GPL-3.0-or-later
// Program media (design/media.md): the host cassette and the fast autostart, attached to the machine as its
// OS-call handler. Front ends call service() before every run slice and, while busy(), run unpaced with the screen
// hidden in slices that end at a frame start (RunLimits::to_next_frame), so scripted keys land the same way however
// the front end slices time.
#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/input/key_script.hpp"
#include "core/machine/machine.hpp"
#include "core/machine/os_call.hpp"
#include "core/media/host_cassette.hpp"
#include "core/media/tape_deck.hpp"

namespace freetvc {

enum class AutostartState {
    Idle,
    Booting,     // cold boot; the logo's keyboard wait gets SPACE
    WaitPrompt,  // BASIC asks for a line: type LOAD
    Loading,     // LOAD typed, waiting for the cassette's close after the program block
    WaitReady,   // loaded without autorun: RUN at the next prompt
    Running,     // RUN typed
    Done,
    Failed,
};

class Media final : public OsCallHandler {
public:
    // A cold boot spends about 330 frames before the prompt (machine.md "Boot results"); this covers that and a long load.
    static constexpr std::uint64_t kAutostartFrames = 3000;

    // After this many frames without a prompt after the tape's close, a program has started itself.
    static constexpr std::uint64_t kTapeRunFrames = 50;

    HostCassette& cassette() { return cassette_; }
    const HostCassette& cassette() const { return cassette_; }
    // While a tape is in the deck the host cassette serves nothing: the ROM's own tape routines run.
    TapeDeck& deck() { return deck_; }
    const TapeDeck& deck() const { return deck_; }

    // Starts "reset and run the attached cassette": restores the prompt state when one matches the machine's
    // configuration (and reuse is allowed), else powers on. Returns false with nothing attached.
    bool autostart(Machine& machine, bool reuse_prompt_state = true);
    // The same from the tape in the deck: rewinds it, types LOAD and presses PLAY. Returns false with no tape.
    bool autostart_tape(Machine& machine, bool reuse_prompt_state = true);
    // The same from a disk: types LOAD "NAME"; the disk card holds the file. Returns false with an empty name.
    bool autostart_disk(Machine& machine, std::string name, bool reuse_prompt_state = true);
    // The attached cassette's or the tape's program name, for messages.
    std::string program_name() const;
    void cancel();
    // Before every run slice.
    void service(Machine& machine);
    // One autostart slice: service(), then a run to the next frame start (at most two frames of ticks).
    StopInfo run_slice(Machine& machine);
    bool busy() const;
    AutostartState state() const { return state_; }
    // Why the last autostart ended ("running NAME", or the failure).
    const std::string& autostart_result() const { return result_; }
    bool has_prompt_state() const { return prompt_.has_value(); }
    void forget_prompt_state() { prompt_.reset(); }
    // For keeping it on disk: nullptr when there is none
    const Machine* prompt_state() const { return prompt_ ? &*prompt_ : nullptr; }
    void set_prompt_state(const Machine& machine) { prompt_ = machine; }

    void on_os_call(Machine& machine) override;

private:
    void begin(Machine& machine, bool reuse_prompt_state);
    void observe(Machine& machine, std::uint8_t code);
    void type(const Machine& machine, std::u32string_view text);
    void finish(AutostartState state, std::string result);

    HostCassette cassette_;
    TapeDeck deck_;
    bool tape_mode_ = false;           // this autostart loads from the deck
    bool disk_mode_ = false;           // this autostart loads from a disk
    std::string disk_name_;
    std::uint64_t closed_frame_ = 0;   // frame of the close that ended a tape or disk load
    std::uint64_t frame_limit_ = kAutostartFrames;
    AutostartState state_ = AutostartState::Idle;
    std::string result_;
    bool cold_ = false;
    bool capture_prompt_ = false;
    std::optional<Machine> prompt_;  // the machine at the first BASIC prompt after a cold boot
    std::uint64_t start_frame_ = 0;
    std::uint64_t return_frame_ = 0;  // frame of the RETURN that starts LOAD
    std::uint64_t blocks_before_ = 0;
    unsigned logo_presses_ = 0;
    std::vector<KeyEvent> keys_;
    std::size_t next_key_ = 0;
};

}  // namespace freetvc
