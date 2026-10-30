// SPDX-License-Identifier: GPL-3.0-or-later
// GDB Remote Serial Protocol stub (design/debug-and-remote.md "GDB stub"): GDB's org.gnu.gdb.z80.cpu registers in
// MAME's wire order (PC = 0Bh), so GNU gdb and DeZog's gdb remote both work. Addresses from 10000h up reach a bank:
// (bank+1) << 16 | offset, as DeZog's long addresses. Not thread-safe: the host thread calls everything.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "remote/monitor.hpp"

namespace freetvc::remote {

inline constexpr std::uint16_t kGdbPort = 6583;

class GdbServer {
public:
    explicit GdbServer(Monitor& monitor);
    ~GdbServer();
    GdbServer(const GdbServer&) = delete;
    GdbServer& operator=(const GdbServer&) = delete;

    // A client connected: holds the machine, acknowledgements on.
    void begin_session();
    // Bytes from the client (packets, acks, Ctrl-C); returns the bytes to send back.
    std::string feed(std::string_view bytes);
    // Host thread, after every run slice: the stop reply once a continued program has stopped.
    std::string poll();
    // Detach, kill or a lost client: removes this session's checkpoints and lets a machine it holds run on.
    void end_session();

    static const std::string& target_xml();
    // $payload#checksum
    static std::string frame(std::string_view payload);

private:
    struct Point {
        char type = '0';  // Z packet type: 0/1 breakpoint, 2 write, 3 read, 4 access watchpoint
        std::uint32_t address = 0;
        std::uint32_t length = 0;
        bool operator<(const Point& o) const {
            return type != o.type ? type < o.type : address != o.address ? address < o.address : length < o.length;
        }
    };

    std::string reply(std::string_view payload);
    // One packet; `answer` false when the reply comes later (continue) or never (kill).
    std::string handle(std::string_view packet, bool& answer);
    std::string query(std::string_view packet);
    std::string registers() const;
    std::string read_register(unsigned number) const;
    bool write_register(unsigned number, std::uint16_t value);
    std::string read_memory(std::string_view args) const;
    std::string write_memory(std::string_view args);
    std::string insert_point(std::string_view args);
    std::string remove_point(std::string_view args);
    std::string monitor_command(std::string_view hex);
    std::string stop_reply();
    std::string stop_reply(std::uint8_t signal, std::string_view reason) const;
    // "watch:addr" (rwatch, awatch) for a watchpoint, "swbreak:" for a breakpoint if the client asked for it.
    std::string stop_reason(const debug::Stop& stop) const;
    void resume(std::string_view address);

    std::uint8_t read_byte(std::uint32_t address) const;
    void write_byte(std::uint32_t address, std::uint8_t value);

    Monitor& monitor_;
    debug::Debugger& dbg_;
    Machine& machine_;
    bool ack_ = true;
    bool swbreak_ = false;  // the client said swbreak+ in qSupported
    bool running_ = false;  // a continue is waiting for its stop reply
    std::uint64_t stops_at_continue_ = 0;
    std::string pending_;   // bytes of an incomplete packet
    std::string last_sent_; // for a '-' (resend)
    std::map<Point, unsigned> points_;  // Z packets -> checkpoint ids
};

}  // namespace freetvc::remote
