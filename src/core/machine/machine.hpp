// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include "core/audio/sound.hpp"
#include "core/audio/synth.hpp"
#include "core/bus/memory_map.hpp"
#include "core/cards/sid_card.hpp"
#include "core/cards/twodfx_card.hpp"
#include "core/input/keyboard.hpp"
#include "core/machine/config.hpp"
#include "core/machine/debug_hooks.hpp"
#include "core/machine/os_call.hpp"
#include "core/machine/printer_port.hpp"
#include "core/machine/tape_port.hpp"
#include "core/video/framebuffer.hpp"
#include "core/video/video.hpp"
#include "core/z80/z80.hpp"

namespace freetvc {

class Snapshot;
class FloppyDrives;
struct Card256Media;
struct NanoSdMedia;

// Machine time is counted in half-T ticks (6.25 MHz, 160 ns), the resolution of the VRAM clock stretch.
inline constexpr std::uint64_t kTicksPerTState = 2;
inline constexpr std::uint64_t kTStatesPerFrame = 62800;

enum class StopReason { TickBudget, InstructionCount, Debugger };

struct RunLimits {
    std::uint64_t max_ticks = 0;         // budget in half-T ticks; a stretched last cycle can overrun it by up to 4
    std::uint64_t max_instructions = 0;  // 0 = no instruction limit
    bool to_next_frame = false;          // also stop at the first boundary in a new frame (reason TickBudget)
};

struct StopInfo {
    StopReason reason = StopReason::TickBudget;
    std::uint64_t ticks_run = 0;
    std::uint64_t instructions_run = 0;
    std::uint16_t pc = 0;  // address of the last opcode fetch; the next instruction when stopped at a boundary
};

// The base TVC: Z80, page decoder, I/O decode, cursor/sound IRQ flip-flop, keyboard matrix and video (tvc-hardware.md §2-§7).
// A copy is a complete machine state without the debugger attachment.
class Machine {
public:
    Machine();
    explicit Machine(const MachineConfig& config);

    // Takes everything from `state` except the debugger attachment and the audio not yet read.
    void restore(const Machine& state);
    // TVCS chunks CONF, CPU, BUS, RAM, VRAM, VID (CRTC included), SND, SIDC (core/snapshot/machine_state.hpp).
    void save_state(Snapshot& out) const;
    // The machine must already have the snapshot's board, ROMs and cartridge (snapshot_matches()); on failure nothing
    // changes and `error` says why. Like restore(), it keeps the attachments and drops the audio not yet read.
    bool load_state(const Snapshot& in, std::string& error);
    // nullptr detaches; the hooks must outlive the attachment.
    void set_debug_hooks(DebugHooks* hooks) { hooks_.ptr = hooks; }
    DebugHooks* debug_hooks() const { return hooks_.ptr; }
    // nullptr detaches; like the debugger attachment it is not part of the machine state.
    void set_os_call_handler(OsCallHandler* handler) { os_.ptr = handler; }
    OsCallHandler* os_call_handler() const { return os_.ptr; }
    // nullptr detaches (59h b5 then reads 0); not part of the machine state either.
    void set_tape_port(TapePort* port) { tape_.ptr = port; }
    TapePort* tape_port() const { return tape_.ptr; }
    // nullptr unplugs the printer (/ACK never comes); not part of the machine state either.
    void set_printer(PrinterPort* printer) { printer_.ptr = printer; }
    PrinterPort* printer() const { return printer_.ptr; }
    // The drives behind a disk card (their disks are not machine state); nullptr detaches (no disk anywhere).
    void set_floppy_drives(FloppyDrives* drives) { drives_.ptr = drives; }
    FloppyDrives* floppy_drives() const { return drives_.ptr; }
    // The TVC256k++'s pendrive and flash image (outside the machine state); nullptr detaches.
    void set_card256_media(Card256Media* media) { card256_media_.ptr = media; }
    Card256Media* card256_media() const { return card256_media_.ptr; }
    // The NanoSD's SD card and the Nano's EEPROM (outside the machine state); nullptr detaches.
    void set_nanosd_media(NanoSdMedia* media) { nanosd_media_.ptr = media; }
    // The MultiCart's flash, kept by the front end like a disk
    void set_multicart_flash(MultiCartFlash* flash) { memory_.attach_cart_flash(flash); }
    MultiCartFlash* multicart_flash() const { return memory_.cart_flash(); }
    // The SD Dual's card images, kept by the front end like disks; null empties the socket
    void set_sd_image(unsigned socket, SdCardImage* image) { memory_.set_sd_image(socket, image); }
    SdCardImage* sd_image(unsigned socket) const { return memory_.sd_image(socket); }
    NanoSdMedia* nanosd_media() const { return nanosd_media_.ptr; }
    // The serial card's cable (outside the machine state); nullptr unplugs it.
    void set_serial_line(SerialLine* line) { serial_line_.ptr = line; }
    // The rear B/W switch (port 59h b6) moves without a power-on, as on the hardware
    void set_colour_switch(bool colour) { config_.colour = colour; }
    SerialLine* serial_line() const { return serial_line_.ptr; }
    // A card's view of the bus now, for describing it
    CardBus card_view() { return card_bus(); }

    // Takes a new board and ROM set, then powers on.
    void configure(const MachineConfig& config);
    const MachineConfig& config() const { return config_; }
    // Cold start: CPU power-on state, RAM and VRAM cleared, every register reset, clock at zero.
    void power_on();
    // Reset button: CPU and port latches reset, memory kept (6845 registers kept ❓).
    void reset();

    // Copies bytes into RAM U0-U3 (64 KiB back to back), bypassing the page decoder; wraps at 64 KiB.
    void load(std::uint16_t address, std::span<const std::uint8_t> bytes);
    // Continues execution with an opcode fetch at pc.
    void jump(std::uint16_t pc);
    // As jump(), for a call the host performed: the debugger sees the instruction at pc as an ordinary one.
    void return_to(std::uint16_t pc);
    StopInfo run(const RunLimits& limits);

    // CPU-side bus access without timing, for tests, loaders and debuggers.
    std::uint8_t peek(std::uint16_t address) const { return memory_.read(address); }
    void poke(std::uint16_t address, std::uint8_t value) { memory_.write(address, value); }
    void io_write(std::uint8_t port, std::uint8_t value);
    std::uint8_t io_read(std::uint8_t port);

    // Matrix rows 0-9 (joysticks 1/2 on 8, 9; tvc-hardware.md §7) and the GameCard's joysticks 3/4 on 10, 11 (keyboard.hpp).
    void set_key(unsigned row, unsigned column, bool down);
    bool key_down(unsigned row, unsigned column) const {
        return row < kMatrixRows && column < 8 && !(keyboard_[row] & (1u << column));
    }

    std::uint64_t ticks() const { return ticks_; }
    // Start of the instruction in progress; at a boundary, the next instruction.
    std::uint16_t pc() const { return instruction_pc_; }
    // True between instructions: after a run stopped by an instruction count or the debugger, after jump() and resets.
    bool at_boundary() const { return at_boundary_; }
    bool halted() const { return (pins_ & Z80::kHALT) != 0; }
    // True if an interrupt was acknowledged on the way to the current boundary.
    bool entered_interrupt() const { return entered_interrupt_; }
    Z80& cpu() { return cpu_; }
    const Z80& cpu() const { return cpu_; }
    std::span<std::uint8_t> ram() { return memory_.ram(); }
    std::span<const std::uint8_t> ram() const { return memory_.ram(); }
    const MemoryMap& memory() const { return memory_; }
    const CardSlots& cards() const { return memory_.cards(); }
    // A card's own controls from the front end (the TVC256k++'s mouse); nothing that changes the memory map
    Card* card(unsigned slot) { return memory_.cards().card(slot); }
    // Debugger access to whole banks, ROMs included; no timing, no side effects.
    std::span<std::uint8_t> bank_bytes(Bank bank) { return memory_.bank_bytes(bank); }
    const Video& video() const { return video_; }
    const Framebuffer& framebuffer() const { return video_.frame(); }
    std::uint64_t frame_count() const { return video_.frame_count(); }
    bool irq_pending() const { return irq_flipflop_; }
    const Sound& sound() const { return sound_; }
    // The printer port's flip-flops (design/machine.md "Printer port"): /STROBE high, ACK set = ready
    bool printer_strobe() const { return printer_strobe_; }
    bool printer_ack() const { return printer_ack_; }
    std::uint8_t printer_data() const { return printer_data_; }
    // The SID card, or nullptr when it is out
    const SidCard* sid_card() const { return sid_ ? &*sid_ : nullptr; }

    // Mono float samples at config.audio_rate up to the end of the last run(), oldest first; at most 2 s are kept.
    unsigned audio_rate() const { return audio_.rate(); }
    std::size_t audio_available() const { return audio_.available(); }
    std::size_t read_audio(std::span<float> out) { return audio_.read(out); }

private:
    // A copied or assigned machine keeps its own attachment.
    template <typename T>
    struct Attachment {
        T* ptr = nullptr;
        Attachment() = default;
        Attachment(const Attachment&) {}
        Attachment& operator=(const Attachment&) { return *this; }
    };

    void offer_os_call();
    CardBus card_bus() {
        return CardBus{ticks_, drives_.ptr, &audio_, keyboard_.data(), card256_media_.ptr, nanosd_media_.ptr, &sound_, serial_line_.ptr};
    }

    void restart_cpu(std::uint16_t pc, bool jumped);
    void reset_ports();
    unsigned stretch(std::uint64_t start) const;
    void advance_devices();
    // Applies a /ACK pulse that has ended by now
    void update_printer();
    std::uint8_t keyboard_columns(unsigned row) const;

    Attachment<DebugHooks> hooks_;
    Attachment<OsCallHandler> os_;
    Attachment<TapePort> tape_;
    Attachment<PrinterPort> printer_;
    Attachment<FloppyDrives> drives_;
    Attachment<Card256Media> card256_media_;
    Attachment<NanoSdMedia> nanosd_media_;
    Attachment<SerialLine> serial_line_;
    MachineConfig config_;
    Z80 cpu_;
    Z80::Pins pins_ = 0;
    std::uint64_t ticks_ = 0;
    std::uint16_t last_fetch_ = 0;
    std::uint16_t instruction_pc_ = 0;
    bool skip_boundary_ = true;  // the first fetch after a (re)start ends no instruction
    bool jumped_ = false;        // that restart came from jump(), not a reset
    unsigned watched_line_ = ~0u;
    bool at_boundary_ = true;
    bool stop_pending_ = false;  // a watched access asked to stop at the next boundary
    bool interrupt_acked_ = false;
    bool entered_interrupt_ = false;

    MemoryMap memory_;
    Video video_;
    Sound sound_;
    std::optional<SidCard> sid_;
    AudioSynth audio_;
    bool irq_flipflop_ = false;  // cursor and sound share one flip-flop; any write to 07h clears it
    std::uint8_t keyboard_row_ = 0;
    std::array<std::uint8_t, 16> keyboard_{};  // active low; rows 10-15 stay FFh
    std::uint8_t port05_ = 0;  // b7-6 tape motors; the sound bits live in sound_
    std::uint8_t printer_data_ = 0;
    bool printer_strobe_ = true;  // the /STROBE flip-flop (06h b7)
    bool printer_ack_ = true;     // the ACK flip-flop (59h b7)
    std::uint64_t printer_ack_due_ = Sound::kNever;  // the end of the attached printer's /ACK pulse
    bool tape_out_ = false;
    std::uint8_t slot_irq_enable_ = 0;
};

}  // namespace freetvc
