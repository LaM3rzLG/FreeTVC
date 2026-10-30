// SPDX-License-Identifier: GPL-3.0-or-later
// DZRP server: every supported command on a hand-assembled RAM program, then a session over a real TCP connection.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "core/machine/machine.hpp"
#include "debug/debugger.hpp"
#include "remote/dzrp.hpp"
#include "remote/dzrp_host.hpp"
#include "remote/monitor.hpp"
#include "remote/monitor_hub.hpp"
#include "remote/net.hpp"

namespace {

using freetvc::Machine;
using freetvc::remote::DzrpServer;
using freetvc::remote::Monitor;
using Bytes = std::vector<std::uint8_t>;

constexpr std::uint64_t kFrame = 2 * 62800;
int g_failures = 0;

void expect(bool ok, int line, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL line %d: %s\n", line, what.c_str());
    }
}
#define EXPECT(cond) expect((cond), __LINE__, #cond)

// 8000 LD A,5 / INC A / CALL 8010h / LD (9000h),A / JR $ ; 8010 INC A / RET
std::unique_ptr<Machine> program() {
    freetvc::MachineConfig config;
    config.audio_rate = 0;
    auto m = std::make_unique<Machine>(config);
    m->io_write(0x02, 0xB0);
    m->load(0x8000, Bytes{0x3E, 0x05, 0x3C, 0xCD, 0x10, 0x80, 0x32, 0x00, 0x90, 0x18, 0xFE});
    m->load(0x8010, Bytes{0x3C, 0xC9});
    freetvc::Z80Registers r = m->cpu().registers();
    r.sp = 0xA000;
    m->cpu().set_registers(r);
    m->jump(0x8000);
    return m;
}

Bytes command(std::uint8_t seq, std::uint8_t id, const Bytes& payload = {}) {
    Bytes out{seq, id};
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// Splits framed output into message bodies (sequence number first).
std::vector<Bytes> messages(const std::string& framed) {
    std::vector<Bytes> out;
    std::size_t at = 0;
    while (at + 4 <= framed.size()) {
        const auto b = [&](std::size_t i) { return static_cast<std::uint8_t>(framed[at + i]); };
        const std::size_t length = b(0) | (b(1) << 8) | (b(2) << 16) | (static_cast<std::size_t>(b(3)) << 24);
        if (at + 4 + length > framed.size()) {
            break;  // not all there yet
        }
        out.emplace_back(framed.begin() + static_cast<std::ptrdiff_t>(at + 4),
                         framed.begin() + static_cast<std::ptrdiff_t>(at + 4 + length));
        at += 4 + length;
    }
    return out;
}

std::uint16_t word(const Bytes& b, std::size_t at) { return static_cast<std::uint16_t>(b[at] | (b[at + 1] << 8)); }

// One response with this sequence number.
Bytes reply(DzrpServer& dzrp, std::uint8_t seq, std::uint8_t id, const Bytes& payload = {}) {
    const std::vector<Bytes> m = messages(dzrp.handle(command(seq, id, payload)));
    expect(m.size() == 1 && !m[0].empty() && m[0][0] == seq, __LINE__, "one response, same sequence number");
    return m.empty() ? Bytes{} : m[0];
}

// Runs the machine through the monitor until the server has a notification.
std::optional<Bytes> run_until_pause(Monitor& mon, DzrpServer& dzrp, int frames = 20) {
    std::string unused;
    for (int i = 0; i < frames; ++i) {
        if (!mon.active()) {
            mon.run(kFrame, unused);
        }
        const std::vector<Bytes> m = messages(dzrp.poll());
        if (!m.empty()) {
            return m[0];
        }
    }
    return std::nullopt;
}

void check_pause(const std::optional<Bytes>& n, std::uint8_t reason, std::uint16_t address, int line) {
    expect(n.has_value() && n->size() >= 7, line, "a NTF_PAUSE");
    if (!n || n->size() < 7) {
        return;
    }
    expect((*n)[0] == 0 && (*n)[1] == 1, line, "sequence 0, NTF_PAUSE");
    expect((*n)[2] == reason, line, "reason " + std::to_string(reason) + ", got " + std::to_string((*n)[2]));
    expect(word(*n, 3) == address, line, "address");
    expect((*n)[5] == (address < 0x4000 ? 1 : 2), line, "ZX48K bank+1");
    expect(n->back() == 0, line, "reason string ends in 0");
}

void commands() {
    auto m = program();
    freetvc::debug::Debugger dbg(*m);
    Monitor mon(dbg);
    DzrpServer dzrp(mon);

    // INIT: version 2.2.0, ZX48K, program name; the machine is held
    Bytes r = reply(dzrp, 1, 1, Bytes{2, 2, 0, 'D', 'e', 'Z', 'o', 'g', 0});
    EXPECT(r.size() > 6 && r[1] == 0 && r[2] == 2 && r[3] == 2 && r[4] == 0 && r[5] == 2);
    EXPECT(std::string(r.begin() + 6, r.end() - 1).rfind("freeTVC ", 0) == 0 && r.back() == 0);
    EXPECT(mon.active());

    r = reply(dzrp, 2, 24);
    const auto supported = [&](unsigned id) { return id / 8 + 1 < r.size() && (r[id / 8 + 1] >> (id % 8)) & 1; };
    EXPECT(supported(1) && supported(24) && supported(28) && supported(40) && supported(43) && supported(50) && supported(51));
    EXPECT(!supported(10) && !supported(22));

    // READ_STATE gives a TVCS snapshot; WRITE_STATE brings back HL after a change
    const Bytes state = reply(dzrp, 2, 50);
    EXPECT(state.size() > 5 && state[1] == 'T' && state[2] == 'V' && state[3] == 'C' && state[4] == 'S');
    const std::uint16_t hl = dbg.registers().hl;
    reply(dzrp, 2, 4, Bytes{5, static_cast<std::uint8_t>(~hl & 0xFF), static_cast<std::uint8_t>(~hl >> 8)});
    EXPECT(dbg.registers().hl != hl);
    reply(dzrp, 2, 51, Bytes(state.begin() + 1, state.end()));
    EXPECT(dbg.registers().hl == hl);

    // Registers: PC SP ... then R I IM reserved, two slots with banks 0 and 1
    r = reply(dzrp, 3, 3);
    EXPECT(r.size() == 32);
    EXPECT(word(r, 1) == 0x8000 && word(r, 3) == 0xA000);
    EXPECT(r[29] == 2 && r[30] == 0 && r[31] == 1);

    reply(dzrp, 4, 4, Bytes{5, 0x34, 0x12});  // HL
    reply(dzrp, 5, 4, Bytes{17, 0x77, 0x00});  // B
    r = reply(dzrp, 6, 3);
    EXPECT(word(r, 11) == 0x1234 && (word(r, 7) >> 8) == 0x77);

    // Memory: blocks, 64 K writes, ZX48K banks (bank 1 starts at 4000h)
    r = reply(dzrp, 7, 28, Bytes{6, 0, 0, 0, 0x00, 0x80, 3, 0, 0x10, 0x80, 2, 0});
    EXPECT(r == (Bytes{7, 0x3E, 0x05, 0x3C, 0x3C, 0xC9}));
    reply(dzrp, 8, 9, Bytes{0, 0x00, 0x90, 0xAA, 0xBB});
    EXPECT(m->peek(0x9000) == 0xAA && m->peek(0x9001) == 0xBB);
    r = reply(dzrp, 9, 25, Bytes{1, 0x00, 0x50, 2, 0});
    EXPECT(r == (Bytes{9, 0xAA, 0xBB}));
    reply(dzrp, 10, 26, Bytes{1, 0x02, 0x50, 0xCC});
    EXPECT(m->peek(0x9002) == 0xCC);
    r = reply(dzrp, 11, 25, Bytes{0, 0xFF, 0x3F, 2, 0});
    EXPECT(r.size() == 3 && r[2] == 0xFF);  // past the end of bank 0
    r = reply(dzrp, 12, 15, Bytes{1, 2, 3});
    EXPECT(r == (Bytes{12, 1, 2, 3}));

    // Ports: the border colour (port 00h, I-G-R-B- in b7, b5, b3, b1)
    reply(dzrp, 13, 21, Bytes{0x00, 0x00, 0xAA});
    EXPECT(m->video().border() == 0x0F);

    // A breakpoint at the subroutine
    r = reply(dzrp, 14, 40, Bytes{0x10, 0x80, 2, 0});
    EXPECT(r.size() == 3 && word(r, 1) != 0);
    const std::uint16_t bp = word(r, 1);
    reply(dzrp, 15, 6, Bytes(11, 0));
    EXPECT(!mon.active());
    check_pause(run_until_pause(mon, dzrp), 2, 0x8010, __LINE__);
    EXPECT(dbg.registers().pc == 0x8010 && (dbg.registers().af >> 8) == 6);

    // Step over the RET's caller: DeZog's temporary breakpoint after the CALL
    reply(dzrp, 1, 6, Bytes{1, 0x06, 0x80, 0, 0, 0, 0, 0, 0, 0, 0});
    check_pause(run_until_pause(mon, dzrp), 0, 0x8006, __LINE__);
    EXPECT(dbg.checkpoints().size() == 1);  // the temporary one is gone

    // A write watchpoint on 9000h stops after the store
    r = reply(dzrp, 2, 42, Bytes{0x00, 0x90, 2, 1, 0, 2});
    EXPECT(r.size() == 2 && r[1] == 0);
    reply(dzrp, 3, 6, Bytes(11, 0));
    check_pause(run_until_pause(mon, dzrp), 4, 0x9000, __LINE__);
    EXPECT(m->peek(0x9000) == 7 && dbg.registers().pc == 0x8009);
    reply(dzrp, 4, 43, Bytes{0x00, 0x90, 2, 1, 0, 2});
    reply(dzrp, 5, 41, Bytes{static_cast<std::uint8_t>(bp), static_cast<std::uint8_t>(bp >> 8)});
    EXPECT(dbg.checkpoints().empty());

    // PAUSE while running: the response, then NTF_PAUSE (manual) in the same write
    reply(dzrp, 6, 6, Bytes(11, 0));
    std::string unused;
    mon.run(kFrame, unused);
    const std::vector<Bytes> paused = messages(dzrp.handle(command(7, 7)));
    EXPECT(paused.size() == 2 && paused[0] == Bytes{7});
    check_pause(paused.size() == 2 ? std::optional<Bytes>(paused[1]) : std::nullopt, 1, 0x8009, __LINE__);
    EXPECT(mon.active());
    EXPECT(messages(dzrp.handle(command(8, 7))).size() == 1);  // not running: no notification

    // A checkpoint DeZog does not know about is "other", with a text
    mon.execute("break 8002");
    reply(dzrp, 9, 4, Bytes{0, 0x00, 0x80});
    reply(dzrp, 10, 6, Bytes(11, 0));
    const std::optional<Bytes> other = run_until_pause(mon, dzrp);
    check_pause(other, 255, 0x8002, __LINE__);
    EXPECT(other && std::string(other->begin() + 6, other->end()).find("checkpoint") != std::string::npos);
    mon.execute("delete");

    // Break on interrupt: the sound IRQ, with interrupts on through DZRP
    reply(dzrp, 11, 39, Bytes{1});
    reply(dzrp, 12, 23, Bytes{1});
    m->io_write(0x04, 0x00);
    m->io_write(0x05, 0x2F);  // pitch F00h, sound IRQ on
    m->io_read(0x5B);         // INSTART
    reply(dzrp, 13, 4, Bytes{13, 1, 0});  // IM 1
    reply(dzrp, 14, 6, Bytes(11, 0));
    const std::optional<Bytes> irq = run_until_pause(mon, dzrp, 60);
    check_pause(irq, 255, 0x0038, __LINE__);
    reply(dzrp, 15, 39, Bytes{0});
    EXPECT(dbg.checkpoints().empty());

    // CLOSE drops the session's checkpoints and lets the machine run on
    reply(dzrp, 1, 40, Bytes{0x02, 0x80, 2, 0});
    reply(dzrp, 2, 2);
    EXPECT(dbg.checkpoints().empty() && !mon.active());
}

// With TVC banks: machine type 0, five slots with Bank numbers, bank-qualified breakpoints, bank memory by offset
void tvc_banks() {
    auto m = program();  // port 02h B0: U0, U1, U2, U3
    freetvc::debug::Debugger dbg(*m);
    Monitor mon(dbg);
    DzrpServer dzrp(mon, freetvc::remote::DzrpMemory::TvcBanks);
    using freetvc::Bank;
    const auto n = [](Bank b) { return static_cast<std::uint8_t>(b); };

    Bytes r = reply(dzrp, 1, 1, Bytes{2, 2, 0, 't', 0});
    EXPECT(r.size() > 6 && r[5] == 0);
    r = reply(dzrp, 2, 3);
    EXPECT(r.size() == 35 && r[29] == 5);
    EXPECT(r.size() == 35 && r[30] == n(Bank::U0) && r[31] == n(Bank::U1) && r[32] == n(Bank::U2) &&
           r[33] == n(Bank::U3) && r[34] == n(Bank::U3));

    // Page 2 shows VID0, page 3 the IOMEM split (CARD0 below E000h, EXT above)
    m->io_write(0x02, 0xD0);
    r = reply(dzrp, 3, 3);
    EXPECT(r.size() == 35 && r[32] == n(Bank::Vid0) && r[33] == n(Bank::Card0) && r[34] == n(Bank::Ext));
    m->io_write(0x02, 0xB0);

    // Bank memory by offset: U2 holds the program at offset 0
    r = reply(dzrp, 4, 25, Bytes{n(Bank::U2), 0x00, 0x00, 3, 0});
    EXPECT(r == (Bytes{4, 0x3E, 0x05, 0x3C}));
    reply(dzrp, 5, 26, Bytes{n(Bank::Vid0), 0x10, 0x00, 0x99});
    EXPECT(m->bank_bytes(Bank::Vid0)[0x10] == 0x99);
    r = reply(dzrp, 6, 25, Bytes{n(Bank::Ext), 0x00, 0x20, 1, 0});
    EXPECT(r.size() == 2 && r[1] == 0xFF);  // EXT is 8 K

    // A breakpoint in VID0 at 8010h never fires while U2 is there; the one in U2 does, and the stop carries U2
    reply(dzrp, 7, 40, Bytes{0x10, 0x80, static_cast<std::uint8_t>(n(Bank::Vid0) + 1), 0});
    reply(dzrp, 8, 40, Bytes{0x10, 0x80, static_cast<std::uint8_t>(n(Bank::U2) + 1), 0});
    reply(dzrp, 9, 6, Bytes(11, 0));
    const std::optional<Bytes> stop = run_until_pause(mon, dzrp);
    EXPECT(stop && stop->size() >= 7 && (*stop)[2] == 2 && word(*stop, 3) == 0x8010 && (*stop)[5] == n(Bank::U2) + 1);
    EXPECT(dbg.find(1) && dbg.find(1)->hits == 0);

    // A watchpoint qualified with U2 is removed by the same address, bank, size and access
    reply(dzrp, 10, 42, Bytes{0x00, 0x90, static_cast<std::uint8_t>(n(Bank::U2) + 1), 1, 0, 2});
    reply(dzrp, 11, 43, Bytes{0x00, 0x90, static_cast<std::uint8_t>(n(Bank::U2) + 1), 1, 0, 2});
    EXPECT(dbg.checkpoints().size() == 2);
}

void tcp_session() {
    auto m = program();
    freetvc::debug::Debugger dbg(*m);
    Monitor mon(dbg);
    freetvc::remote::MonitorHub hub(mon);
    DzrpServer dzrp(mon);
    auto host_owner = std::make_unique<freetvc::remote::DzrpHost>(dzrp);
    freetvc::remote::DzrpHost& host = *host_owner;
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
        const auto send = [&](std::uint8_t seq, std::uint8_t id, const Bytes& payload) {
            std::string f;
            for (int i = 0; i < 4; ++i) {
                f.push_back(static_cast<char>((payload.size() >> (8 * i)) & 0xFF));
            }
            f.push_back(static_cast<char>(seq));
            f.push_back(static_cast<char>(id));
            f.append(payload.begin(), payload.end());
            c.send_all(f);
        };
        // Reads until `count` messages have arrived
        const auto read_messages = [&](std::size_t count) {
            while (messages(received).size() < count) {
                char buf[256];
                const long n = c.receive(buf, sizeof buf);
                if (n <= 0) {
                    return;
                }
                received.append(buf, static_cast<std::size_t>(n));
            }
        };
        // The program ran before the client came, so it starts it again, as DeZog's execAddress does
        send(1, 1, Bytes{2, 2, 0, 't', 0});
        read_messages(1);
        send(2, 4, Bytes{0, 0x00, 0x80});
        read_messages(2);
        send(3, 40, Bytes{0x10, 0x80, 2, 0});
        read_messages(3);
        send(4, 6, Bytes(11, 0));
        read_messages(5);  // the CONTINUE response and the NTF_PAUSE of the breakpoint
        send(5, 3, {});
        read_messages(6);
        send(6, 2, {});
        read_messages(7);
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

    const std::vector<Bytes> got = messages(received);
    EXPECT(got.size() == 7);
    if (got.size() == 7) {
        EXPECT(got[0].size() > 6 && got[0][0] == 1 && got[0][5] == 2);
        EXPECT(got[1] == Bytes{2});
        EXPECT(got[2].size() == 3 && got[2][0] == 3);
        EXPECT(got[3] == Bytes{4});
        check_pause(got[4], 2, 0x8010, __LINE__);
        EXPECT(got[5].size() == 32 && word(got[5], 1) == 0x8010);
        EXPECT(got[6] == Bytes{6});
    } else {
        std::printf("received %zu bytes\n", received.size());
    }
    EXPECT(dbg.checkpoints().empty() && !mon.active());
}

}  // namespace

int main() {
    commands();
    tvc_banks();
    tcp_session();
    if (g_failures == 0) {
        std::printf("dzrp_test: all passed\n");
        return 0;
    }
    std::printf("dzrp_test: %d failure(s)\n", g_failures);
    return 1;
}
