// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/machine/machine.hpp"

#include <cstddef>

#include "core/bus/video_stretch.hpp"

namespace freetvc {

namespace {

constexpr float kHighpassHz = 20.0f;  // AC coupling of the sound path ❓ (no component values known)
constexpr std::uint64_t kPrinterAckTicks = 63;  // the printer's /ACK pulse ends 10 µs after /STROBE ❓

}  // namespace

Machine::Machine() : Machine(MachineConfig{}) {}

Machine::Machine(const MachineConfig& config) { configure(config); }

void Machine::configure(const MachineConfig& config) {
    config_ = config;
    memory_.configure(config.model, config.roms);
    memory_.configure_cards(config.cards);
    if (config.sid.model != SidModel::None) {
        sid_.emplace(config.sid);
    } else {
        sid_.reset();
    }
    audio_.configure(config.audio_rate, kHighpassHz, 2 * static_cast<std::size_t>(config.audio_rate));
    power_on();
}

void Machine::restore(const Machine& state) {
    *this = state;
    audio_.discard_ready();
}

void Machine::restart_cpu(std::uint16_t pc, bool jumped) {
    skip_boundary_ = true;
    jumped_ = jumped;
    at_boundary_ = true;
    stop_pending_ = false;
    interrupt_acked_ = false;
    entered_interrupt_ = false;
    instruction_pc_ = pc;
}

void Machine::power_on() {
    pins_ = cpu_.reset();
    restart_cpu(0, false);
    ticks_ = 0;
    last_fetch_ = 0;
    memory_.clear_ram();
    memory_.reset();
    memory_.reset_cart();
    memory_.cards().power_on();
    memory_.cards_changed();
    video_.power_on();
    sound_.power_on();
    if (sid_) {
        sid_->power_on();
    }
    audio_.clear();
    keyboard_.fill(0xFF);
    tape_out_ = false;
    reset_ports();
}

void Machine::reset() {
    pins_ = cpu_.reset();
    restart_cpu(0, false);
    memory_.reset();
    memory_.cards().reset(ticks_);
    memory_.cards_changed();
    if (sid_) {
        sid_->reset(ticks_, audio_);  // WinTVC resets its SID with the machine (design/cards.md "SID card")
    }
    reset_ports();
}

void Machine::reset_ports() {
    // Reset clears the page latches (memory_.reset()), 05h (tone and sound IRQ off), the IRQ flip-flop and the slot enables
    port05_ = 0;
    sound_.set_control(0, ticks_, audio_);
    if (tape_.ptr) {
        tape_.ptr->tape_motor(ticks_, false);
    }
    keyboard_row_ = 0;
    irq_flipflop_ = false;
    slot_irq_enable_ = 0;
    // NRESET clears the printer latch (the printer sees NUL) and sets both flip-flops ([HW] §3.3.4, p. 133)
    printer_data_ = 0;
    printer_strobe_ = true;
    printer_ack_ = true;
    printer_ack_due_ = Sound::kNever;
}

void Machine::update_printer() {
    if (printer_ack_due_ <= ticks_) {
        // The /ACK pulse sets the /STROBE flip-flop and its trailing edge the ACK flip-flop
        printer_strobe_ = true;
        printer_ack_ = true;
        printer_ack_due_ = Sound::kNever;
    }
}

void Machine::load(std::uint16_t address, std::span<const std::uint8_t> bytes) {
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        const auto at = static_cast<std::uint16_t>(address + i);
        const std::span<std::uint8_t> unit = memory_.unit_bytes(at >> 14);
        if (!unit.empty()) {
            unit[at & (MemoryMap::kPageSize - 1)] = bytes[i];
        }
    }
}

void Machine::jump(std::uint16_t pc) {
    pins_ = cpu_.prefetch(pc);
    restart_cpu(pc, true);
}

void Machine::return_to(std::uint16_t pc) {
    pins_ = cpu_.prefetch(pc);
    restart_cpu(pc, false);
}

void Machine::offer_os_call() {
    if (os_.ptr && instruction_pc_ == kOsEntry && !halted()) {
        os_.ptr->on_os_call(*this);
    }
}

void Machine::set_key(unsigned row, unsigned column, bool down) {
    if (row < kMatrixRows && column < 8) {
        const auto bit = static_cast<std::uint8_t>(1u << column);
        keyboard_[row] = down ? static_cast<std::uint8_t>(keyboard_[row] & ~bit) : static_cast<std::uint8_t>(keyboard_[row] | bit);
    }
}

// The switches have no diodes ([OS] p. 87): a pressed key joins its row and column lines, so the selected row
// reaches every column connected to it through pressed keys, and three corners of a rectangle read as four.
std::uint8_t Machine::keyboard_columns(unsigned row) const {
    if (row >= kKeyboardRows) {
        return 0xFF;
    }
    unsigned rows = 1u << row;
    unsigned columns = 0;
    for (;;) {
        unsigned reached_columns = 0;
        unsigned reached_rows = rows;
        for (unsigned r = 0; r < 10; ++r) {
            const unsigned pressed = ~keyboard_[r] & 0xFFu;
            if (rows & (1u << r)) {
                reached_columns |= pressed;
            }
        }
        for (unsigned r = 0; r < 10; ++r) {
            if (~keyboard_[r] & reached_columns & 0xFFu) {
                reached_rows |= 1u << r;
            }
        }
        if (reached_columns == columns && reached_rows == rows) {
            return static_cast<std::uint8_t>(~columns);
        }
        columns = reached_columns;
        rows = reached_rows;
    }
}

// Port map: tvc-hardware.md §4. Only A0-A7 are decoded and A7 must be 0 for the machine; the 2dfx card takes F8h-F9h,
// the SID card the rest of E0h-FFh.
void Machine::io_write(std::uint8_t port, std::uint8_t value) {
    if (port & 0x80) {
        if ((port & 0xFE) == 0xF8) {
            if (TwoDfxCard* fx = memory_.cards().twodfx()) {
                fx->port_write(port & 1, value);
                return;
            }
        }
        if (sid_ && port >= SidCard::kFirstPort) {
            sid_->write(port & 31, value, ticks_, audio_);
        }
        return;
    }
    switch (port >> 4) {
    case 0x0:
        switch (port) {
        case 0x00:
            video_.set_border(value);
            memory_.cards().border_write(value);
            break;
        case 0x01:
            printer_data_ = value;
            break;
        case 0x02:
            memory_.set_page_register(value);
            break;
        case 0x03:
            memory_.set_iomem_slot(value >> 6);
            keyboard_row_ = value & 0x0F;
            break;
        case 0x04:
            sound_.set_pitch_low(value, ticks_, audio_);
            break;
        case 0x05:
            if (tape_.ptr && ((port05_ ^ value) & 0xC0) != 0) {
                tape_.ptr->tape_motor(ticks_, (value & 0xC0) != 0);
            }
            port05_ = value;
            sound_.set_control(value, ticks_, audio_);
            break;
        case 0x06: {
            video_.set_mode(value);
            sound_.set_amplitude(value, ticks_, audio_);
            update_printer();
            const bool strobe = (value & 0x80) != 0;
            if (printer_strobe_ && !strobe) {
                // /STROBE low clears the ACK flip-flop; without a printer no /ACK ever sets it again
                printer_ack_ = false;
                if (printer_.ptr) {
                    printer_.ptr->printer_byte(ticks_, printer_data_);
                    printer_ack_due_ = ticks_ + kPrinterAckTicks;
                }
            }
            printer_strobe_ = strobe;
            break;
        }
        case 0x07:
            irq_flipflop_ = false;
            break;
        case 0x0C:
        case 0x0D:
        case 0x0E:
        case 0x0F:
            // A2/A3 gating makes 0Ch-0Fh one register ([HW] p.125; MAME/YATE/WinTVC decode only 0Fh)
            memory_.set_video_paging(value);
            break;
        default:
            break;  // 08h-0Bh unused
        }
        break;
    case 0x5:
        if (port < 0x58) {
            tape_out_ = !tape_out_;
            if (tape_.ptr) {
                tape_.ptr->tape_output(ticks_, tape_out_);
            }
        } else {
            // 58h-5Bh (mirrored at 5Ch-5Fh): slot n's /CLRINT strobe; what b7 means is the card's ([HW] port table, pp. 125-126)
            const auto bit = static_cast<std::uint8_t>(1u << (port & 3));
            slot_irq_enable_ = (value & 0x80) ? static_cast<std::uint8_t>(slot_irq_enable_ | bit)
                                              : static_cast<std::uint8_t>(slot_irq_enable_ & ~bit);
            memory_.cards().clear_interrupt(port & 3, value);
        }
        break;
    case 0x6:
        video_.set_palette(port & 3, value);  // 60h-63h, mirrored up to 6Fh
        break;
    case 0x7:
        if (port & 1) {
            video_.crtc().write(value);
        } else {
            video_.crtc().select(value);
        }
        break;
    default:
        // 10h-4Fh: slots 0-3
        memory_.cards().io_write((port >> 4) - 1, port & 15, value, card_bus());
        memory_.cards_changed();
        break;
    }
}

std::uint8_t Machine::io_read(std::uint8_t port) {
    if (port & 0x80) {
        if ((port & 0xFE) == 0xF8) {
            if (const TwoDfxCard* fx = memory_.cards().twodfx()) {
                return fx->port_read(port & 1);
            }
        }
        if (sid_ && port >= SidCard::kFirstPort) {
            return sid_->read(port & 31, ticks_, audio_);
        }
        return MemoryMap::kOpenBus;
    }
    switch (port >> 4) {
    case 0x5:
        if (port < 0x58) {
            tape_out_ = !tape_out_;  // reads toggle the tape flip-flop too ([HW] fig. 8, the gate circuit)
            if (tape_.ptr) {
                tape_.ptr->tape_output(ticks_, tape_out_);
            }
            return MemoryMap::kOpenBus;
        }
        switch (port & 3) {
        case 0:
            return keyboard_columns(keyboard_row_);
        case 1: {
            // b3-0 the slots' /INT lines (0 = pending), b4 cursor/sound IRQ (0 = pending), b5 tape in (idle 0), b6 colour switch, b7 printer ACK
            update_printer();
            std::uint8_t value = static_cast<std::uint8_t>(~memory_.cards().irq_lines() & 0x0F);
            if (printer_ack_) {
                value |= 0x80;
            }
            if (!irq_flipflop_) {
                value |= 0x10;
            }
            if (config_.colour) {
                value |= 0x40;
            }
            if (tape_.ptr && tape_.ptr->tape_input(ticks_)) {
                value |= 0x20;
            }
            return value;
        }
        case 2:
            return memory_.cards().ids();  // card IDs, 2 bits per slot: 11 = empty
        default:
            sound_.instart(ticks_, audio_);
            return MemoryMap::kOpenBus;  // 5Bh INSTART: data undriven ❓, WinTVC gives 00h
        }
    case 0x7:
        return (port & 1) ? video_.crtc().read() : MemoryMap::kOpenBus;
    case 0x1:
    case 0x2:
    case 0x3:
    case 0x4:
        return memory_.cards().io_read((port >> 4) - 1, port & 15, card_bus());
    default:
        return MemoryMap::kOpenBus;  // write-only ports
    }
}

unsigned Machine::stretch(std::uint64_t start) const {
    if ((pins_ & (Z80::kMREQ | Z80::kRFSH)) != Z80::kMREQ || !(pins_ & (Z80::kRD | Z80::kWR)) ||
        !memory_.is_video(Z80::address(pins_))) {
        return 0;
    }
    // z80.h raises M1 in its T1 and MR/MW in T2, so the DEB phase is taken one T earlier for MR/MW
    if (pins_ & Z80::kM1) {
        return kStretchM1[deb_phase(start)];
    }
    return kStretchReadWrite[deb_phase(start - kTicksPerTState)];
}

void Machine::advance_devices() {
    if (video_.advance_to(ticks_, memory_.video_bank(memory_.displayed_bank()), memory_.overlay()) > 0) {
        irq_flipflop_ = true;
    }
    if (sound_.advance_to(ticks_, audio_) > 0) {
        irq_flipflop_ = true;
    }
    if (ticks_ >= memory_.cards().next_event()) {
        memory_.cards().run_events(card_bus());
    }
    DebugHooks* const hooks = hooks_.ptr;
    if (hooks && hooks->watch_lines && video_.beam_line() != watched_line_) {
        watched_line_ = video_.beam_line();
        if (hooks->line_flags[watched_line_ % hooks->line_flags.size()]) {
            stop_pending_ |= hooks->on_access(*this, kAccessLine, static_cast<std::uint16_t>(watched_line_), 0);
        }
    }
}

StopInfo Machine::run(const RunLimits& limits) {
    StopInfo info;
    DebugHooks* const hooks = hooks_.ptr;
    const std::uint64_t first_frame = video_.frame_count();
    // A run resumed at 0030h after a debugger stop there still owes the handler its look
    if (at_boundary_) {
        const std::uint16_t before = instruction_pc_;
        offer_os_call();
        if (instruction_pc_ != before) {
            ++info.instructions_run;  // the performed call counts as the instruction at 0030h
            if (limits.max_instructions != 0 && info.instructions_run >= limits.max_instructions) {
                info.reason = StopReason::InstructionCount;
                info.pc = instruction_pc_;
                return info;
            }
        }
    }
    while (info.ticks_run + kTicksPerTState <= limits.max_ticks) {
        // The cursor/sound flip-flop and the slots' /INT lines through J2 (HBA-2 sheets 7 and 10)
        pins_ = (irq_flipflop_ || memory_.cards().irq_lines() != 0) ? (pins_ | Z80::kINT) : (pins_ & ~Z80::kINT);
        const std::uint64_t start = ticks_;
        pins_ = cpu_.tick(pins_);
        const std::uint64_t length = kTicksPerTState + stretch(start);
        ticks_ += length;
        info.ticks_run += length;
        at_boundary_ = false;
        // Devices catch up first, so a VRAM write lands after the characters already fetched
        advance_devices();

        if (pins_ & Z80::kMREQ) {
            const std::uint16_t addr = Z80::address(pins_);
            if (pins_ & Z80::kRD) {
                const std::uint8_t value = memory_.cpu_read(addr, ticks_);
                pins_ = Z80::with_data(pins_, value);
                if (pins_ & Z80::kM1) {
                    last_fetch_ = addr;
                } else if (hooks && (hooks->memory_flags[addr] & kAccessRead)) {
                    stop_pending_ |= hooks->on_access(*this, kAccessRead, addr, value);
                }
            } else if (pins_ & Z80::kWR) {
                memory_.cpu_write(addr, Z80::data(pins_), ticks_);
                if (hooks && (hooks->memory_flags[addr] & kAccessWrite)) {
                    stop_pending_ |= hooks->on_access(*this, kAccessWrite, addr, Z80::data(pins_));
                }
            }
        } else if (pins_ & Z80::kIORQ) {
            const auto port = static_cast<std::uint8_t>(Z80::address(pins_));
            if (pins_ & Z80::kM1) {
                pins_ = Z80::with_data(pins_, 0xFF);  // INTA: bus not driven ([EP] assumes FFh ❓)
                interrupt_acked_ = true;
            } else if (pins_ & Z80::kRD) {
                pins_ = Z80::with_data(pins_, io_read(port));
                if (hooks && (hooks->port_flags[port] & kAccessIn)) {
                    stop_pending_ |= hooks->on_access(*this, kAccessIn, port, Z80::data(pins_));
                }
            } else if (pins_ & Z80::kWR) {
                io_write(port, Z80::data(pins_));
                if (hooks && (hooks->port_flags[port] & kAccessOut)) {
                    stop_pending_ |= hooks->on_access(*this, kAccessOut, port, Z80::data(pins_));
                }
            }
        }

        if (cpu_.instruction_done()) {
            const bool restart = skip_boundary_;
            at_boundary_ = true;
            instruction_pc_ = last_fetch_;
            entered_interrupt_ = interrupt_acked_;
            interrupt_acked_ = false;
            if (skip_boundary_) {
                skip_boundary_ = false;
            } else {
                ++info.instructions_run;
            }
            bool stop = stop_pending_;
            stop_pending_ = false;
            // While halted the CPU refetches at HALT+1 every 4 T; those fetches start no instruction
            if (hooks && !halted() &&
                (hooks->every_instruction || (hooks->memory_flags[instruction_pc_] & kAccessExec) ||
                 (entered_interrupt_ && hooks->watch_interrupts))) {
                stop |= hooks->on_instruction(*this, instruction_pc_, restart && jumped_);
            }
            if (stop) {
                info.reason = StopReason::Debugger;
                break;
            }
            offer_os_call();
            if (limits.to_next_frame && video_.frame_count() != first_frame) {
                break;
            }
            if (!restart && limits.max_instructions != 0 && info.instructions_run >= limits.max_instructions) {
                info.reason = StopReason::InstructionCount;
                break;
            }
        }
    }
    memory_.cards().advance_audio(ticks_, audio_);
    if (sid_) {
        sid_->advance_to(ticks_, audio_);
    }
    audio_.end_at(ticks_);
    info.pc = last_fetch_;
    return info;
}

}  // namespace freetvc
