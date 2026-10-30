// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/media/media.hpp"

#include <algorithm>

#include "core/message.hpp"

namespace freetvc {

namespace {

constexpr std::uint8_t kEditorChin = 0xA1;  // [OS] ED CHIN; ED BKIN is A2h
constexpr std::uint8_t kEditorBkin = 0xA2;
constexpr std::uint8_t kCasClose = 0xD4;
constexpr std::uint16_t kInTable = 0x0B00;   // device per class for input (rom.md §4)
constexpr std::uint16_t kOutTable = 0x0B08;  // and for output

bool same_config(const MachineConfig& a, const MachineConfig& b) {
    for (unsigned i = 0; i < kCardSlots; ++i) {
        if (a.cards[i].kind != b.cards[i].kind || a.cards[i].rom != b.cards[i].rom) {
            return false;
        }
    }
    return a.model == b.model && a.colour == b.colour && a.audio_rate == b.audio_rate && a.roms.sys == b.roms.sys &&
           a.roms.ext == b.roms.ext && a.roms.cart == b.roms.cart && a.roms.cart_type == b.roms.cart_type &&
           a.roms.cart_bank == b.roms.cart_bank;
}

// The OS's routing (rom.md §4 step 5): a card byte takes the whole class; otherwise the table byte picks the device
// for functions 0-2 only, and functions 3-15 run on the class's own device.
bool routed_to_cassette(const Machine& m, std::uint8_t code) {
    const unsigned cls = (code >> 4) & 7;
    const unsigned function = code & 0x0F;
    if (cls == 7) {
        return false;
    }
    const std::uint8_t device = m.ram()[((code & 0x80) ? kInTable : kOutTable) + cls];
    if (device & 0x80) {
        return false;
    }
    return function <= 2 ? device == 5 : cls == 5;
}

std::string state_name(AutostartState s) {
    switch (s) {
    case AutostartState::Booting:
        return msg("booting");
    case AutostartState::WaitPrompt:
        return msg("waiting for the BASIC prompt");
    case AutostartState::Loading:
        return msg("loading");
    case AutostartState::WaitReady:
        return msg("waiting for READY");
    case AutostartState::Running:
        return msg("typing RUN");
    default:
        return msg("idle");
    }
}

}  // namespace

bool Media::autostart(Machine& machine, bool reuse_prompt_state) {
    if (!cassette_.attached()) {
        return false;
    }
    tape_mode_ = false;
    disk_mode_ = false;
    frame_limit_ = kAutostartFrames;
    begin(machine, reuse_prompt_state);
    return true;
}

bool Media::autostart_disk(Machine& machine, std::string name, bool reuse_prompt_state) {
    if (name.empty()) {
        return false;
    }
    tape_mode_ = false;
    disk_mode_ = true;
    disk_name_ = std::move(name);
    frame_limit_ = kAutostartFrames;
    begin(machine, reuse_prompt_state);
    return true;
}

bool Media::autostart_tape(Machine& machine, bool reuse_prompt_state) {
    if (!deck_.loaded()) {
        return false;
    }
    tape_mode_ = true;
    disk_mode_ = false;
    deck_.stop();
    deck_.rewind();
    frame_limit_ = kAutostartFrames + deck_.length() / (kTStatesPerFrame * kTicksPerTState);
    begin(machine, reuse_prompt_state);
    return true;
}

std::string Media::program_name() const {
    if (disk_mode_) {
        return disk_name_;
    }
    if (tape_mode_ || !cassette_.attached()) {
        return deck_.signal().name.empty() ? msg("the tape") : deck_.signal().name;
    }
    return cassette_.attached_name();
}

void Media::begin(Machine& machine, bool reuse_prompt_state) {
    keys_.clear();
    next_key_ = 0;
    logo_presses_ = 0;
    result_.clear();
    capture_prompt_ = reuse_prompt_state;
    if (prompt_ && !same_config(prompt_->config(), machine.config())) {
        prompt_.reset();
    }
    if (reuse_prompt_state && prompt_) {
        machine.restore(*prompt_);
        cold_ = false;
        state_ = AutostartState::WaitPrompt;
    } else {
        machine.power_on();
        cold_ = true;
        state_ = AutostartState::Booting;
    }
    start_frame_ = machine.frame_count();
}

void Media::cancel() {
    if (busy()) {
        state_ = AutostartState::Idle;
        result_ = msg("cancelled");
    }
}

bool Media::busy() const {
    return state_ != AutostartState::Idle && state_ != AutostartState::Done && state_ != AutostartState::Failed;
}

void Media::service(Machine& machine) {
    if (!busy()) {
        // Releases left over from an ended or cancelled autostart
        for (; next_key_ < keys_.size(); ++next_key_) {
            if (!keys_[next_key_].down) {
                machine.set_key(keys_[next_key_].key.row, keys_[next_key_].key.column, false);
            }
        }
        return;
    }
    apply_key_events(machine, keys_, next_key_, machine.frame_count());
    if (state_ == AutostartState::Running && next_key_ == keys_.size()) {
        finish(AutostartState::Done, msg("running %s", program_name().c_str()));
    } else if (state_ == AutostartState::WaitReady && (tape_mode_ || disk_mode_) && machine.frame_count() - closed_frame_ > kTapeRunFrames) {
        finish(AutostartState::Done, msg("running %s", program_name().c_str()));  // no prompt came: the program started itself
    } else if (machine.frame_count() - start_frame_ > frame_limit_) {
        finish(AutostartState::Failed, msg("timed out %s", state_name(state_).c_str()));
    }
}

StopInfo Media::run_slice(Machine& machine) {
    service(machine);
    return machine.run({.max_ticks = 2 * kTStatesPerFrame * kTicksPerTState, .to_next_frame = true});
}

void Media::type(const Machine& machine, std::u32string_view text) {
    std::uint64_t start = machine.frame_count() + 1;
    if (!keys_.empty()) {
        start = std::max(start, keys_.back().frame + 2);
    }
    const TypedText typed = type_text(text, start);
    for (const KeyEvent& e : typed.events) {
        if (e.down && e.key.row == kKeyReturn.row && e.key.column == kKeyReturn.column) {
            return_frame_ = e.frame;
        }
    }
    keys_.insert(keys_.end(), typed.events.begin(), typed.events.end());
}

void Media::finish(AutostartState state, std::string result) {
    state_ = state;
    result_ = std::move(result);
}

void Media::observe(Machine& machine, std::uint8_t code) {
    const bool keyboard_in = (code & 0xF0) == 0x90 && (code & 0x0F) != 0;
    const bool editor_in = code == kEditorChin || code == kEditorBkin;
    const std::uint64_t frame = machine.frame_count();
    switch (state_) {
    case AutostartState::Booting:
    case AutostartState::WaitPrompt:
        if (editor_in) {
            if (cold_ && capture_prompt_ && !prompt_) {
                prompt_ = machine;  // at the 0030h boundary of BASIC's first line input
            }
            if (disk_mode_) {
                std::u32string line = U"LOAD \"";
                line.append(disk_name_.begin(), disk_name_.end());
                type(machine, line + U"\"\n");
            } else {
                // A disk card holds the cassette class: give it back, as POKE 2821,5: POKE 2829,5 does (design/cards.md)
                if (machine.peek(kInTable + 5) & 0x80) {
                    machine.poke(kInTable + 5, 0x05);
                    machine.poke(kOutTable + 5, 0x05);
                }
                type(machine, U"LOAD\n");
            }
            blocks_before_ = cassette_.blocks_read();
            if (tape_mode_) {
                deck_.play();
            }
            state_ = AutostartState::Loading;
        } else if (keyboard_in && next_key_ == keys_.size() && logo_presses_ < 3) {
            // The start-up logo waits for a key through the keyboard class
            ++logo_presses_;
            keys_.push_back({frame + 1, kKeySpace, true});
            keys_.push_back({frame + 3, kKeySpace, false});
            state_ = AutostartState::WaitPrompt;
        }
        break;
    case AutostartState::Loading:
        if (editor_in && frame > return_frame_ + 1) {
            finish(AutostartState::Failed, msg("LOAD stopped with an error; BASIC is back at the prompt"));
        }
        break;
    case AutostartState::WaitReady:
        if (editor_in) {
            type(machine, U"RUN\n");
            state_ = AutostartState::Running;
        }
        break;
    default:
        break;
    }
}

void Media::on_os_call(Machine& machine) {
    // Only the OS's own entry (JP INFUNC), as written by every SYS ROM, carries a function byte
    if (machine.peek(kOsEntry) != 0xC3 || machine.peek(kOsEntry + 1) != 0x23 || machine.peek(kOsEntry + 2) != 0x0B) {
        return;
    }
    const std::uint16_t sp = machine.cpu().registers().sp;
    const auto ret = static_cast<std::uint16_t>(machine.peek(sp) | machine.peek(static_cast<std::uint16_t>(sp + 1)) << 8);
    const std::uint8_t code = machine.peek(ret);
    if (busy()) {
        observe(machine, code);
    }
    if (disk_mode_ && busy()) {
        // The disk card serves the file; its close ends the load, then RUN or the program's own start
        if (state_ == AutostartState::Loading && code == kCasClose) {
            state_ = AutostartState::WaitReady;
            closed_frame_ = machine.frame_count();
        }
        return;
    }
    if (deck_.loaded()) {
        // The ROM reads the tape itself; its close ends the load (design/media.md "With the host cassette and autostart")
        if (tape_mode_ && state_ == AutostartState::Loading && code == kCasClose && routed_to_cassette(machine, code)) {
            const TapeSignal& tape = deck_.signal();
            const auto ram = machine.ram();
            const bool in_ram = tape.program_data.empty() || tape.program_data.front().empty() ||
                                std::search(ram.begin(), ram.end(), tape.program_data.front().begin(), tape.program_data.front().end()) != ram.end();
            if (!tape.program_ends.empty() && deck_.position() < tape.program_ends.front()) {
                finish(AutostartState::Failed, msg("LOAD closed the tape before the end of the program (a read error?)"));
            } else if (!in_ram) {
                finish(AutostartState::Failed, msg("LOAD ended, but the program is not in memory as it is on the tape (a read error?)"));
            } else if (!tape.program_autorun.empty() && tape.program_autorun.front()) {
                finish(AutostartState::Done, msg("running %s", program_name().c_str()));
            } else {
                state_ = AutostartState::WaitReady;
                closed_frame_ = machine.frame_count();
            }
        }
        return;
    }
    if (!routed_to_cassette(machine, code) || !cassette_.call(machine, code)) {
        return;
    }
    if (code == kCasClose && state_ == AutostartState::Loading) {
        const auto& file = cassette_.attached_file();
        if (cassette_.blocks_read() == blocks_before_) {
            finish(AutostartState::Failed, msg("LOAD closed the file without reading the program (Bad file?)"));
        } else if (file && file->autorun()) {
            finish(AutostartState::Done, msg("running %s", program_name().c_str()));
        } else {
            state_ = AutostartState::WaitReady;
        }
    }
}

}  // namespace freetvc
