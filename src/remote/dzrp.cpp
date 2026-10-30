// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote/dzrp.hpp"

#include <algorithm>
#include <iterator>
#include <span>

#include "core/snapshot/snapshot.hpp"
#include "core/version.hpp"

namespace freetvc::remote {

namespace {

enum Command : std::uint8_t {
    kInit = 1,
    kClose = 2,
    kGetRegisters = 3,
    kSetRegister = 4,
    kContinue = 6,
    kPause = 7,
    kWriteMem = 9,
    kLoopback = 15,
    kReadPort = 20,
    kWritePort = 21,
    kInterruptOnOff = 23,
    kGetSupportedCommands = 24,
    kReadBankMem = 25,
    kWriteBankMem = 26,
    kReadMemBlocks = 28,
    kEnableBreakOnInterrupt = 39,
    kAddBreakpoint = 40,
    kRemoveBreakpoint = 41,
    kAddWatchpoint = 42,
    kRemoveWatchpoint = 43,
    kReadState = 50,
    kWriteState = 51,
};

constexpr Command kSupported[] = {kInit,          kClose,           kGetRegisters,  kSetRegister,
                                  kContinue,      kPause,           kWriteMem,      kLoopback,
                                  kReadPort,      kWritePort,       kInterruptOnOff, kGetSupportedCommands,
                                  kReadBankMem,   kWriteBankMem,    kReadMemBlocks, kEnableBreakOnInterrupt,
                                  kAddBreakpoint, kRemoveBreakpoint, kAddWatchpoint, kRemoveWatchpoint,
                                  kReadState,     kWriteState};

constexpr std::uint8_t kNtfPause = 1;
constexpr std::uint8_t kMachineUnknown = 0;  // DeZog takes dzrp.customMemory
constexpr std::uint8_t kMachineZx48k = 2;

enum Reason : std::uint8_t {
    kNoReason = 0,
    kManual = 1,
    kBreakpoint = 2,
    kWatchRead = 3,
    kWatchWrite = 4,
    kOther = 255,
};

std::uint16_t word(std::span<const std::uint8_t> p, std::size_t at) {
    return at + 1 < p.size() ? static_cast<std::uint16_t>(p[at] | (p[at + 1] << 8)) : 0;
}

void put_word(std::vector<std::uint8_t>& out, std::uint16_t value) {
    out.push_back(static_cast<std::uint8_t>(value));
    out.push_back(static_cast<std::uint8_t>(value >> 8));
}

void put_string(std::vector<std::uint8_t>& out, const std::string& text) {
    out.insert(out.end(), text.begin(), text.end());
    out.push_back(0);
}

std::string hex4(unsigned v) {
    static constexpr char kDigits[] = "0123456789ABCDEF";
    std::string s(4, '0');
    for (int i = 3; i >= 0; --i, v >>= 4) {
        s[static_cast<std::size_t>(i)] = kDigits[v & 15];
    }
    return s;
}

}  // namespace

DzrpServer::DzrpServer(Monitor& monitor, DzrpMemory memory)
    : monitor_(monitor), memory_(memory), dbg_(monitor.debugger()), machine_(monitor.debugger().machine()) {}

DzrpServer::~DzrpServer() {
    for (const unsigned id : breakpoints_) {
        dbg_.remove(id);
    }
    drop_temporaries();
    for (const Watchpoint& w : watchpoints_) {
        dbg_.remove(w.id);
    }
    break_on_interrupt(false);
}

std::string DzrpServer::frame(std::span<const std::uint8_t> body) {
    const std::uint32_t length = static_cast<std::uint32_t>(body.size());
    std::string out;
    out.reserve(4 + body.size());
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((length >> (8 * i)) & 0xFF));
    }
    out.append(body.begin(), body.end());
    return out;
}

std::string DzrpServer::handle(std::span<const std::uint8_t> message) {
    if (message.size() < 2) {
        return {};
    }
    bool paused = false;
    std::vector<std::uint8_t> body{static_cast<std::uint8_t>(message[0] & 0x0F)};
    const std::vector<std::uint8_t> payload = command(message[1], message.subspan(2), paused);
    body.insert(body.end(), payload.begin(), payload.end());
    std::string out = frame(body);
    if (paused) {
        out += frame(pause_notification());  // after the PAUSE response (spec)
    }
    return out;
}

std::vector<std::uint8_t> DzrpServer::command(std::uint8_t id, std::span<const std::uint8_t> p, bool& paused) {
    std::vector<std::uint8_t> out;
    switch (id) {
    case kInit:
        end_session();
        monitor_.enter("DeZog");
        out = {0, kProtocolVersion[0], kProtocolVersion[1], kProtocolVersion[2],
               memory_ == DzrpMemory::TvcBanks ? kMachineUnknown : kMachineZx48k};
        put_string(out, std::string("freeTVC ") + freetvc::kVersion);
        break;
    case kClose:
        end_session();
        break;
    case kGetRegisters:
        out = registers();
        break;
    case kSetRegister:
        if (p.size() >= 3) {
            set_register(p[0], word(p, 1));
        }
        break;
    case kContinue:
        continue_program(p);
        break;
    case kPause:
        if (running_ && !monitor_.active()) {
            monitor_.enter("DeZog pause");
            paused = true;
        }
        break;
    case kWriteMem:
        if (p.size() >= 3) {
            const std::uint16_t address = word(p, 1);
            for (std::size_t i = 3; i < p.size(); ++i) {
                machine_.poke(static_cast<std::uint16_t>(address + i - 3), p[i]);
            }
        }
        break;
    case kLoopback:
        out.assign(p.begin(), p.end());
        break;
    case kReadPort:
        out.push_back(machine_.io_read(static_cast<std::uint8_t>(word(p, 0))));
        break;
    case kWritePort:
        if (p.size() >= 3) {
            machine_.io_write(static_cast<std::uint8_t>(word(p, 0)), p[2]);
        }
        break;
    case kInterruptOnOff:
        if (!p.empty()) {
            Z80Registers r = dbg_.registers();
            r.iff1 = r.iff2 = p[0] != 0;
            dbg_.set_registers(r);
        }
        break;
    case kReadState: {
        // A TVCS snapshot of the machine; DeZog stores it as it is
        Snapshot snapshot;
        machine_.save_state(snapshot);
        out = snapshot.encode();
        break;
    }
    case kWriteState: {
        // No error field in the reply: a snapshot from another board or ROM set leaves the machine as it was
        std::string error;
        if (auto snapshot = Snapshot::decode(p, error)) {
            dbg_.cancel_step();
            machine_.load_state(*snapshot, error);
        }
        break;
    }
    case kGetSupportedCommands:
        out.assign(7, 0);
        for (const Command c : kSupported) {
            out[c / 8] = static_cast<std::uint8_t>(out[c / 8] | (1u << (c % 8)));
        }
        break;
    case kReadBankMem: {
        const std::uint16_t offset = word(p, 1);
        const std::uint16_t size = word(p, 3);
        const std::uint8_t bank = p.empty() ? 0 : p[0];
        for (std::uint32_t i = 0; i < size; ++i) {
            out.push_back(bank_read(bank, offset + i));
        }
        break;
    }
    case kWriteBankMem:
        if (p.size() >= 3) {
            const std::uint16_t offset = word(p, 1);
            for (std::size_t i = 3; i < p.size(); ++i) {
                bank_write(p[0], offset + static_cast<std::uint32_t>(i - 3), p[i]);
            }
        }
        break;
    case kReadMemBlocks:
        for (std::size_t at = 4; at + 4 <= p.size(); at += 4) {
            const std::uint16_t address = word(p, at);
            const std::uint16_t size = word(p, at + 2);
            for (std::uint32_t i = 0; i < size; ++i) {
                out.push_back(machine_.peek(static_cast<std::uint16_t>(address + i)));
            }
        }
        break;
    case kEnableBreakOnInterrupt:
        break_on_interrupt(!p.empty() && p[0] != 0);
        break;
    case kAddBreakpoint:
        // The condition is DeZog's: it checks it after the stop
        put_word(out, add_breakpoint(word(p, 0), p.size() > 2 ? p[2] : 0));
        break;
    case kRemoveBreakpoint: {
        const unsigned bp = word(p, 0);
        if (breakpoints_.erase(bp) > 0) {
            dbg_.remove(bp);
        }
        break;
    }
    case kAddWatchpoint:
        out.push_back(p.size() >= 6 && add_watchpoint(word(p, 0), p[2], word(p, 3), p[5]) ? 0 : 1);
        break;
    case kRemoveWatchpoint:
        if (p.size() >= 6) {
            remove_watchpoint(word(p, 0), p[2], word(p, 3), p[5]);
        }
        break;
    default:
        break;  // not in GET_SUPPORTED_COMMANDS, so DeZog does not send it
    }
    return out;
}

std::vector<std::uint8_t> DzrpServer::registers() const {
    const Z80Registers r = dbg_.registers();
    std::vector<std::uint8_t> out;
    for (const std::uint16_t v : {r.pc, r.sp, r.af, r.bc, r.de, r.hl, r.ix, r.iy, r.af2, r.bc2, r.de2, r.hl2}) {
        put_word(out, v);
    }
    out.insert(out.end(), {r.r, r.i, r.im, 0});
    if (memory_ == DzrpMemory::TvcBanks) {
        out.push_back(static_cast<std::uint8_t>(std::size(kTvcSlots)));
        for (const std::uint16_t start : kTvcSlots) {
            out.push_back(static_cast<std::uint8_t>(machine_.memory().locate(start).bank));
        }
    } else {
        out.insert(out.end(), {2, 0, 1});  // two slots, banks 0 and 1
    }
    return out;
}

void DzrpServer::set_register(std::uint8_t number, std::uint16_t value) {
    Z80Registers r = dbg_.registers();
    const auto low = [&](std::uint16_t& pair) { pair = static_cast<std::uint16_t>((pair & 0xFF00) | (value & 0xFF)); };
    const auto high = [&](std::uint16_t& pair) { pair = static_cast<std::uint16_t>((pair & 0x00FF) | (value << 8)); };
    switch (number) {
    case 0: r.pc = value; break;
    case 1: r.sp = value; break;
    case 2: r.af = value; break;
    case 3: r.bc = value; break;
    case 4: r.de = value; break;
    case 5: r.hl = value; break;
    case 6: r.ix = value; break;
    case 7: r.iy = value; break;
    case 8: r.af2 = value; break;
    case 9: r.bc2 = value; break;
    case 10: r.de2 = value; break;
    case 11: r.hl2 = value; break;
    case 13: r.im = static_cast<std::uint8_t>(value & 3); break;
    case 14: low(r.af); break;
    case 15: high(r.af); break;
    case 16: low(r.bc); break;
    case 17: high(r.bc); break;
    case 18: low(r.de); break;
    case 19: high(r.de); break;
    case 20: low(r.hl); break;
    case 21: high(r.hl); break;
    case 22: low(r.ix); break;
    case 23: high(r.ix); break;
    case 24: low(r.iy); break;
    case 25: high(r.iy); break;
    case 26: low(r.af2); break;
    case 27: high(r.af2); break;
    case 28: low(r.bc2); break;
    case 29: high(r.bc2); break;
    case 30: low(r.de2); break;
    case 31: high(r.de2); break;
    case 32: low(r.hl2); break;
    case 33: high(r.hl2); break;
    case 34: r.r = static_cast<std::uint8_t>(value); break;
    case 35: r.i = static_cast<std::uint8_t>(value); break;
    default: return;
    }
    dbg_.set_registers(r);  // refused between instructions; DeZog writes registers only while paused
}

void DzrpServer::continue_program(std::span<const std::uint8_t> p) {
    drop_temporaries();
    // Breakpoints 1 and 2 are DeZog's step targets; the alternate step commands are never sent (spec, CMD_CONTINUE)
    for (const std::size_t at : {std::size_t{0}, std::size_t{3}}) {
        if (at + 2 < p.size() && p[at] != 0) {
            debug::Checkpoint c;
            c.access = kAccessExec;
            c.start = c.end = word(p, at + 1);
            c.temporary = true;
            temporaries_.insert(dbg_.add(std::move(c)));
        }
    }
    running_ = true;
    stops_at_continue_ = monitor_.stop_count();
    if (monitor_.active()) {
        monitor_.execute("x");
    }
}

std::string DzrpServer::poll() {
    if (!running_ || !monitor_.active()) {
        return {};
    }
    return frame(pause_notification());
}

std::vector<std::uint8_t> DzrpServer::pause_notification() {
    running_ = false;
    std::uint8_t reason = kManual;
    std::uint16_t address = machine_.pc();
    std::optional<Bank> bank;
    std::string text;
    const std::optional<debug::Stop>& stop = monitor_.last_stop();
    if (monitor_.stop_count() != stops_at_continue_ && stop) {
        address = stop->pc;
        reason = kNoReason;
        if (stop->why == debug::Why::Checkpoint && !stop->hits.empty()) {
            const auto temporary = std::find_if(stop->hits.begin(), stop->hits.end(),
                                                [&](const debug::Hit& h) { return temporaries_.count(h.checkpoint) > 0; });
            const debug::Hit& hit = temporary != stop->hits.end() ? *temporary : stop->hits.front();
            address = hit.address;
            bank = hit.where.bank;
            const auto watch = std::find_if(watchpoints_.begin(), watchpoints_.end(),
                                            [&](const Watchpoint& w) { return w.id == hit.checkpoint; });
            if (temporary != stop->hits.end()) {
                reason = kNoReason;
            } else if (breakpoints_.count(hit.checkpoint) > 0) {
                reason = kBreakpoint;
            } else if (watch != watchpoints_.end()) {
                reason = hit.access == kAccessWrite ? kWatchWrite : kWatchRead;
            } else if (hit.checkpoint == interrupt_checkpoint_) {
                reason = kOther;
                text = "interrupt, handler at " + hex4(hit.address) + "h";
            } else {
                // Not DeZog's: it would look for its own breakpoint there and continue
                reason = kOther;
                text = "freeTVC checkpoint #" + std::to_string(hit.checkpoint) + " at " + hex4(hit.address) + "h";
            }
        }
    }
    drop_temporaries();
    std::vector<std::uint8_t> out{0, kNtfPause, reason};
    put_word(out, address);
    out.push_back(long_bank_of(address, bank));
    put_string(out, text);
    return out;
}

std::optional<Bank> DzrpServer::checkpoint_bank(std::uint8_t long_bank) const {
    if (memory_ != DzrpMemory::TvcBanks || long_bank == 0 || long_bank > kBankCount) {
        return std::nullopt;
    }
    return static_cast<Bank>(long_bank - 1);
}

std::uint8_t DzrpServer::long_bank_of(std::uint16_t address, std::optional<Bank> bank) const {
    if (memory_ != DzrpMemory::TvcBanks) {
        return address < 0x4000 ? 1 : 2;
    }
    return static_cast<std::uint8_t>(static_cast<unsigned>(bank.value_or(machine_.memory().locate(address).bank)) + 1);
}

void DzrpServer::drop_temporaries() {
    for (const unsigned id : temporaries_) {
        dbg_.remove(id);  // the one that stopped is gone already
    }
    temporaries_.clear();
}

void DzrpServer::end_session() {
    for (const unsigned id : breakpoints_) {
        dbg_.remove(id);
    }
    breakpoints_.clear();
    for (const Watchpoint& w : watchpoints_) {
        dbg_.remove(w.id);
    }
    watchpoints_.clear();
    drop_temporaries();
    break_on_interrupt(false);
    running_ = false;
    if (monitor_.active()) {
        monitor_.execute("x");
    }
}

std::uint16_t DzrpServer::add_breakpoint(std::uint16_t address, std::uint8_t long_bank) {
    debug::Checkpoint c;
    c.access = kAccessExec;
    c.start = c.end = address;
    c.bank = checkpoint_bank(long_bank);  // the CPU address, only while that bank is paged there
    const unsigned id = dbg_.add(std::move(c));
    if (id > 0xFFFF) {
        dbg_.remove(id);
        return 0;
    }
    breakpoints_.insert(id);
    return static_cast<std::uint16_t>(id);
}

bool DzrpServer::add_watchpoint(std::uint16_t address, std::uint8_t long_bank, std::uint16_t size,
                                std::uint8_t access) {
    if (size == 0 || (access & 3) == 0) {
        return false;
    }
    debug::Checkpoint c;
    c.access = static_cast<std::uint8_t>(((access & 1) ? kAccessRead : 0) | ((access & 2) ? kAccessWrite : 0));
    c.start = address;
    c.end = static_cast<std::uint16_t>(std::min<unsigned>(0xFFFF, address + size - 1u));
    c.bank = checkpoint_bank(long_bank);
    watchpoints_.push_back({dbg_.add(std::move(c)), address, long_bank, size, access});
    return true;
}

void DzrpServer::remove_watchpoint(std::uint16_t address, std::uint8_t long_bank, std::uint16_t size,
                                   std::uint8_t access) {
    const auto it = std::find_if(watchpoints_.begin(), watchpoints_.end(), [&](const Watchpoint& w) {
        return w.address == address && w.long_bank == long_bank && w.size == size && w.access == access;
    });
    if (it != watchpoints_.end()) {
        dbg_.remove(it->id);
        watchpoints_.erase(it);
    }
}

void DzrpServer::break_on_interrupt(bool on) {
    if (!on) {
        if (interrupt_checkpoint_ != 0) {
            dbg_.remove(interrupt_checkpoint_);
            interrupt_checkpoint_ = 0;
        }
        return;
    }
    if (interrupt_checkpoint_ == 0) {
        debug::Checkpoint c;
        c.access = kAccessIrq;
        c.start = 0;
        c.end = 0xFFFF;
        interrupt_checkpoint_ = dbg_.add(std::move(c));
    }
}

std::uint8_t DzrpServer::bank_read(std::uint8_t bank, std::uint32_t offset) const {
    if (memory_ == DzrpMemory::TvcBanks) {
        if (bank >= kBankCount) {
            return MemoryMap::kOpenBus;
        }
        const std::span<const std::uint8_t> bytes = machine_.memory().bank_bytes(static_cast<Bank>(bank));
        return offset < bytes.size() ? bytes[offset] : MemoryMap::kOpenBus;
    }
    const std::uint32_t base = bank == 0 ? 0x0000 : 0x4000;
    const std::uint32_t size = bank == 0 ? 0x4000 : 0xC000;
    return bank <= 1 && offset < size ? machine_.peek(static_cast<std::uint16_t>(base + offset)) : MemoryMap::kOpenBus;
}

void DzrpServer::bank_write(std::uint8_t bank, std::uint32_t offset, std::uint8_t value) {
    if (memory_ == DzrpMemory::TvcBanks) {
        if (bank < kBankCount) {
            const std::span<std::uint8_t> bytes = machine_.bank_bytes(static_cast<Bank>(bank));
            if (offset < bytes.size()) {
                bytes[offset] = value;  // ROM images too: DeZog marks ROM banks itself
            }
        }
        return;
    }
    const std::uint32_t base = bank == 0 ? 0x0000 : 0x4000;
    const std::uint32_t size = bank == 0 ? 0x4000 : 0xC000;
    if (bank <= 1 && offset < size) {
        machine_.poke(static_cast<std::uint16_t>(base + offset), value);
    }
}

}  // namespace freetvc::remote
