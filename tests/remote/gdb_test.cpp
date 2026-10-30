// SPDX-License-Identifier: GPL-3.0-or-later
// GDB stub: packets, acks, registers, memory and banks, breakpoints, watchpoints, Ctrl-C, qRcmd, then real TCP.
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/machine/machine.hpp"
#include "debug/debugger.hpp"
#include "remote/gdb.hpp"
#include "remote/gdb_host.hpp"
#include "remote/monitor.hpp"
#include "remote/monitor_hub.hpp"
#include "remote/net.hpp"

namespace {

using freetvc::Machine;
using freetvc::remote::GdbServer;
using freetvc::remote::Monitor;

constexpr std::uint64_t kFrame = 2 * 62800;
int g_failures = 0;

void expect(bool ok, int line, const std::string& what, const std::string& got = {}) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL line %d: %s\n  got: %s\n", line, what.c_str(), got.c_str());
    }
}
#define EXPECT(cond) expect((cond), __LINE__, #cond)
#define EXPECT_EQ(got, want)                                                       \
    do {                                                                           \
        const std::string got_ = (got);                                            \
        expect(got_ == (want), __LINE__, std::string("want ") + (want), got_);     \
    } while (false)

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

// 8000 LD A,5 / INC A / CALL 8010h / LD (9000h),A / JR $ ; 8010 INC A / RET
std::unique_ptr<Machine> program() {
    freetvc::MachineConfig config;
    config.audio_rate = 0;
    auto m = std::make_unique<Machine>(config);
    m->io_write(0x02, 0xB0);
    m->load(0x8000, std::vector<std::uint8_t>{0x3E, 0x05, 0x3C, 0xCD, 0x10, 0x80, 0x32, 0x00, 0x90, 0x18, 0xFE});
    m->load(0x8010, std::vector<std::uint8_t>{0x3C, 0xC9});
    freetvc::Z80Registers r = m->cpu().registers();
    r.sp = 0xA000;
    m->cpu().set_registers(r);
    m->jump(0x8000);
    return m;
}

// The payload of the one packet in `out`, after an optional '+'.
std::string payload(const std::string& out) {
    const std::size_t dollar = out.find('$');
    const std::size_t hash = out.rfind('#');
    if (dollar == std::string::npos || hash == std::string::npos || hash < dollar) {
        return "<no packet: " + out + ">";
    }
    return out.substr(dollar + 1, hash - dollar - 1);
}

std::string ask(GdbServer& gdb, const std::string& packet) { return payload(gdb.feed(GdbServer::frame(packet))); }

// Runs the machine through the monitor until the stub has a stop reply.
std::string run_until_stop(Monitor& mon, GdbServer& gdb, int frames = 20) {
    std::string unused;
    for (int i = 0; i < frames; ++i) {
        if (!mon.active()) {
            mon.run(kFrame, unused);
        }
        const std::string out = gdb.poll();
        if (!out.empty()) {
            return payload(out);
        }
    }
    return "<no stop>";
}

std::string hex(const std::string& text) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    for (const unsigned char c : text) {
        out.push_back(kDigits[c >> 4]);
        out.push_back(kDigits[c & 15]);
    }
    return out;
}

std::string unhex(const std::string& text) {
    std::string out;
    for (std::size_t i = 0; i + 1 < text.size(); i += 2) {
        out.push_back(static_cast<char>(std::stoi(text.substr(i, 2), nullptr, 16)));
    }
    return out;
}

void packets() {
    auto m = program();
    freetvc::debug::Debugger dbg(*m);
    Monitor mon(dbg);
    GdbServer gdb(mon);
    gdb.begin_session();
    EXPECT(mon.active());

    // Framing and acks
    EXPECT_EQ(GdbServer::frame("OK"), "$OK#9a");
    std::string out = gdb.feed(GdbServer::frame("qSupported:multiprocess+;swbreak+;hwbreak+"));
    EXPECT(out.size() > 1 && out[0] == '+');
    EXPECT(contains(payload(out), "qXfer:features:read+") && contains(payload(out), "QStartNoAckMode+"));
    const std::string supported = out.substr(1);
    EXPECT_EQ(gdb.feed("$g#00"), "-");     // bad checksum
    EXPECT_EQ(gdb.feed("-"), supported);   // a '-' from the client resends the last reply
    out = gdb.feed("$?#3");
    EXPECT(out.empty());  // incomplete
    out = gdb.feed("f");
    EXPECT(out.size() > 2 && out[0] == '+' && payload(out).rfind("T05", 0) == 0);

    // target.xml in pieces
    std::string xml;
    for (std::size_t offset = 0;; offset += 0x80) {
        char request[64];
        std::snprintf(request, sizeof request, "qXfer:features:read:target.xml:%zx,80", offset);
        const std::string part = ask(gdb, request);
        if (part.empty() || (part[0] != 'm' && part[0] != 'l')) {
            break;
        }
        xml += part.substr(1);
        if (part[0] == 'l') {
            break;
        }
    }
    EXPECT(xml == GdbServer::target_xml());
    EXPECT(contains(xml, "<architecture>z80</architecture>") && contains(xml, "org.gnu.gdb.z80.cpu"));
    EXPECT(contains(xml, "name=\"pc\" bitsize=\"16\" type=\"code_ptr\" regnum=\"11\""));

    // Registers: af bc de hl af' bc' de' hl' ix iy sp pc ir, 16 bit little endian
    std::string g = ask(gdb, "g");
    EXPECT(g.size() == 13 * 4);
    EXPECT_EQ(g.substr(40, 8), "00a00080");  // SP A000h, PC 8000h
    EXPECT_EQ(ask(gdb, "Pb=1080"), "OK");
    EXPECT_EQ(ask(gdb, "pb"), "1080");
    EXPECT(dbg.registers().pc == 0x8010);
    EXPECT_EQ(ask(gdb, "P3=3412"), "OK");
    EXPECT(dbg.registers().hl == 0x1234);
    EXPECT_EQ(ask(gdb, "Pb=0080"), "OK");
    EXPECT_EQ(ask(gdb, "p20"), "E00");

    // Memory: the CPU view, and banks at (bank+1) << 16 (U2 = 5)
    EXPECT_EQ(ask(gdb, "m8000,3"), "3e053c");
    EXPECT_EQ(ask(gdb, "M9000,2:aabb"), "OK");
    EXPECT(m->peek(0x9000) == 0xAA && m->peek(0x9001) == 0xBB);
    EXPECT_EQ(ask(gdb, "m60000,3"), "3e053c");
    EXPECT_EQ(ask(gdb, "M80010,1:99"), "OK");  // VID0 (7) offset 0010h
    EXPECT(m->bank_bytes(freetvc::Bank::Vid0)[0x10] == 0x99);
    m->bank_bytes(freetvc::Bank::Ext)[0x1FFF] = 0x5A;
    EXPECT_EQ(ask(gdb, "m31fff,2"), "5aff");  // EXT (2) is 8 K: its last byte, then open bus

    // A breakpoint: continue answers only with '+', the stop reply comes after the run
    EXPECT_EQ(ask(gdb, "Z0,8010,1"), "OK");
    out = gdb.feed(GdbServer::frame("c"));
    EXPECT_EQ(out, "+");
    EXPECT(!mon.active());
    std::string stop = run_until_stop(mon, gdb);
    EXPECT(stop.rfind("T05swbreak:;", 0) == 0 && contains(stop, "0b:1080;"));
    EXPECT_EQ(ask(gdb, "z0,8010,1"), "OK");
    EXPECT(dbg.checkpoints().empty());

    // Step: one instruction, INC A at 8010h
    stop = ask(gdb, "s");
    EXPECT(stop.rfind("T05", 0) == 0 && contains(stop, "0b:1180;"));

    // A write watchpoint stops after the store, with the address
    EXPECT_EQ(ask(gdb, "Z2,9000,1"), "OK");
    gdb.feed(GdbServer::frame("c"));
    stop = run_until_stop(mon, gdb);
    EXPECT(stop.rfind("T05watch:9000;", 0) == 0 && contains(stop, "0b:0980;"));
    EXPECT_EQ(ask(gdb, "z2,9000,1"), "OK");

    // A bank-offset breakpoint: U2 (6 << 16) offset 0009h = 8009h, the JR $
    EXPECT_EQ(ask(gdb, "Pb=0080"), "OK");
    EXPECT_EQ(ask(gdb, "Z0,60009,1"), "OK");
    gdb.feed(GdbServer::frame("c"));
    stop = run_until_stop(mon, gdb);
    EXPECT(contains(stop, "0b:0980;"));
    EXPECT_EQ(ask(gdb, "z0,60009,1"), "OK");

    // Ctrl-C while running: SIGINT
    gdb.feed(GdbServer::frame("c"));
    std::string unused;
    mon.run(kFrame, unused);
    out = gdb.feed("\x03");
    EXPECT(payload(out).rfind("T02", 0) == 0);
    EXPECT(mon.active());

    // qRcmd runs a monitor command and returns its text
    const std::string reply = unhex(ask(gdb, "qRcmd," + hex("r")));
    EXPECT(contains(reply, "PC") || contains(reply, "pc"));
    // A run command in qRcmd leaves the machine held for GDB
    unhex(ask(gdb, "qRcmd," + hex("x")));
    EXPECT(mon.active());

    // No-ack mode: the OK is still acked, later packets are not
    out = gdb.feed(GdbServer::frame("QStartNoAckMode"));
    EXPECT_EQ(out, "+$OK#9a");
    out = gdb.feed(GdbServer::frame("pb"));
    EXPECT(!out.empty() && out[0] == '$');

    // Detach: checkpoints gone, the machine runs on
    ask(gdb, "Z0,8010,1");
    EXPECT_EQ(ask(gdb, "D"), "OK");
    EXPECT(dbg.checkpoints().empty() && !mon.active());
}

void tcp_session() {
    auto m = program();
    freetvc::debug::Debugger dbg(*m);
    Monitor mon(dbg);
    freetvc::remote::MonitorHub hub(mon);
    GdbServer gdb(mon);
    auto host_owner = std::make_unique<freetvc::remote::GdbHost>(gdb);
    freetvc::remote::GdbHost& host = *host_owner;
    std::string error;
    EXPECT(host.listen(0, error));
    const std::uint16_t port = host.port();

    std::string received;
    std::atomic<bool> done{false};
    std::thread client([&] {
        std::string e;
        freetvc::remote::Connection c = freetvc::remote::Connection::connect_loopback(port, e);
        if (!c.open()) {
            received = "connect failed: " + e;
            done = true;
            return;
        }
        // Reads until `count` packets have arrived
        const auto read_packets = [&](std::size_t count) {
            for (;;) {
                std::size_t packets = 0;
                for (std::size_t at = received.find('#'); at != std::string::npos; at = received.find('#', at + 1)) {
                    ++packets;
                }
                if (packets >= count) {
                    return;
                }
                char buf[512];
                const long n = c.receive(buf, sizeof buf);
                if (n <= 0) {
                    return;
                }
                received.append(buf, static_cast<std::size_t>(n));
            }
        };
        c.send_all(GdbServer::frame("?"));
        read_packets(1);
        c.send_all("+" + GdbServer::frame("pb=1080"));  // not a P: nothing happens
        c.send_all(GdbServer::frame("Pb=0080"));
        c.send_all(GdbServer::frame("Z0,8010,1"));
        read_packets(4);
        c.send_all(GdbServer::frame("c"));
        read_packets(5);  // the stop reply of the breakpoint
        c.send_all(GdbServer::frame("D"));
        read_packets(6);
        done = true;
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!done && std::chrono::steady_clock::now() < deadline) {
        hub.service();
        host.service();
        if (!mon.active()) {
            hub.run(kFrame / 8);
        } else {
            host.wait(std::chrono::milliseconds(5));
        }
    }
    if (done) {
        host.service();
    } else {
        std::printf("FAIL: the TCP session timed out\n");
        ++g_failures;
        host_owner.reset();  // closes the client's connection, so its thread ends
    }
    client.join();

    EXPECT(received.rfind("+$T05", 0) == 0);
    EXPECT(contains(received, "T05swbreak:;") || contains(received, "0b:1080;"));
    EXPECT(contains(received, "$OK#9a"));
    EXPECT(dbg.checkpoints().empty() && !mon.active());
}

}  // namespace

int main() {
    packets();
    tcp_session();
    if (g_failures == 0) {
        std::printf("gdb_test: all passed\n");
        return 0;
    }
    std::printf("gdb_test: %d failure(s)\n", g_failures);
    return 1;
}
