// SPDX-License-Identifier: GPL-3.0-or-later
// DZRP 2.2 server for DeZog's generic "dzrp" remote (design/debug-and-remote.md "DZRP server"). By default the
// TVC is reported as a ZX48K (slot 0 = 0000-3FFF, slot 1 = 4000-FFFF, a flat view of what is paged in); with TVC banks
// it is machine type 0 with five slots that carry the Bank numbers (tools/dezog/).
// Not thread-safe: the host thread calls everything between run slices.
#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "remote/monitor.hpp"

namespace freetvc::remote {

inline constexpr std::uint16_t kDzrpPort = 6582;

enum class DzrpMemory {
    Zx48k,     // any DeZog with the dzrp remote
    TvcBanks,  // needs DeZog with dzrp.customMemory (tools/dezog/dezog-dzrp-custom-memory.patch)
};

class DzrpServer {
public:
    static constexpr std::uint8_t kProtocolVersion[3] = {2, 2, 0};
    // Slot start addresses with TVC banks: pages 0-2, then page 3 split where IOMEM splits it.
    static constexpr std::uint16_t kTvcSlots[5] = {0x0000, 0x4000, 0x8000, 0xC000, 0xE000};

    explicit DzrpServer(Monitor& monitor, DzrpMemory memory = DzrpMemory::Zx48k);
    ~DzrpServer();
    DzrpServer(const DzrpServer&) = delete;
    DzrpServer& operator=(const DzrpServer&) = delete;

    // One command without its length field (sequence number, command id, payload); returns the framed bytes to
    // send: the response, then a notification if the command stopped a running program.
    std::string handle(std::span<const std::uint8_t> message);
    // Host thread, after every run slice: NTF_PAUSE once the program a CONTINUE started has stopped.
    std::string poll();
    // CMD_CLOSE or a lost client: removes this session's checkpoints and lets a machine it holds run on.
    void end_session();

    // A frame: little-endian length of the rest, then `body` (sequence number first).
    static std::string frame(std::span<const std::uint8_t> body);

private:
    struct Watchpoint {
        unsigned id = 0;
        std::uint16_t address = 0;
        std::uint8_t long_bank = 0;  // as sent: bank+1, 0 = any
        std::uint16_t size = 0;
        std::uint8_t access = 0;  // DZRP bits: 1 read, 2 write
    };

    std::vector<std::uint8_t> command(std::uint8_t id, std::span<const std::uint8_t> payload, bool& paused);
    std::vector<std::uint8_t> registers() const;
    void set_register(std::uint8_t number, std::uint16_t value);
    void continue_program(std::span<const std::uint8_t> payload);
    std::vector<std::uint8_t> pause_notification();
    void drop_temporaries();
    std::uint16_t add_breakpoint(std::uint16_t address, std::uint8_t long_bank);
    bool add_watchpoint(std::uint16_t address, std::uint8_t long_bank, std::uint16_t size, std::uint8_t access);
    void remove_watchpoint(std::uint16_t address, std::uint8_t long_bank, std::uint16_t size, std::uint8_t access);
    void break_on_interrupt(bool on);
    // The bank of a long address (bank+1) with TVC banks; nothing for 0 or with the ZX48K view.
    std::optional<Bank> checkpoint_bank(std::uint8_t long_bank) const;
    // bank+1 for NTF_PAUSE: the ZX48K slot, or the TVC bank the address was in.
    std::uint8_t long_bank_of(std::uint16_t address, std::optional<Bank> bank) const;
    // ZX48K banks: 0 = 0000-3FFF, 1 = 4000-FFFF of the CPU's view; TVC banks: Bank numbers, offsets in the bank.
    std::uint8_t bank_read(std::uint8_t bank, std::uint32_t offset) const;
    void bank_write(std::uint8_t bank, std::uint32_t offset, std::uint8_t value);

    Monitor& monitor_;
    DzrpMemory memory_;
    debug::Debugger& dbg_;
    Machine& machine_;
    bool running_ = false;  // a CONTINUE is waiting for its NTF_PAUSE
    std::uint64_t stops_at_continue_ = 0;
    std::set<unsigned> breakpoints_;
    std::set<unsigned> temporaries_;
    std::vector<Watchpoint> watchpoints_;
    unsigned interrupt_checkpoint_ = 0;
};

}  // namespace freetvc::remote
