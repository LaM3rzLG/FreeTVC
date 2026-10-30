// SPDX-License-Identifier: GPL-3.0-or-later
// Text monitor: commands on a hand-assembled RAM program, a script through the hub, and a real TCP client.
// usage: monitor_test <scratch dir>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "core/machine/machine.hpp"
#include "debug/debugger.hpp"
#include "remote/monitor.hpp"
#include "remote/monitor_hub.hpp"
#include "remote/net.hpp"

namespace {

using freetvc::Machine;
using freetvc::remote::Monitor;
using freetvc::remote::MonitorHub;

constexpr std::uint64_t kFrame = 2 * 62800;
int g_failures = 0;

bool contains(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }

void expect(bool ok, int line, const std::string& what, const std::string& output = {}) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL line %d: %s\n%s", line, what.c_str(), output.c_str());
    }
}

#define EXPECT_IN(output, part)                                                    \
    do {                                                                           \
        const std::string out_ = (output);                                         \
        expect(contains(out_, part), __LINE__, std::string("missing '") + (part) + "'", out_); \
    } while (false)

// 8000 LD A,5 / INC A / CALL 8010h / NOP / JR $ ; 8010 INC A / RET
std::unique_ptr<Machine> program() {
    freetvc::MachineConfig config;
    config.audio_rate = 0;
    auto m = std::make_unique<Machine>(config);
    m->io_write(0x02, 0xB0);
    m->load(0x8000, std::vector<std::uint8_t>{0x3E, 0x05, 0x3C, 0xCD, 0x10, 0x80, 0x00, 0x18, 0xFE});
    m->load(0x8010, std::vector<std::uint8_t>{0x3C, 0xC9});
    freetvc::Z80Registers r = m->cpu().registers();
    r.sp = 0xA000;
    m->cpu().set_registers(r);
    m->jump(0x8000);
    return m;
}

std::string run_until_stop(Monitor& mon, int frames = 10) {
    std::string report;
    for (int i = 0; i < frames && !mon.active(); ++i) {
        mon.run(kFrame, report);
    }
    return report;
}

void commands(const std::filesystem::path& dir) {
    auto m = program();
    freetvc::debug::Debugger dbg(*m);
    Monitor mon(dbg);
    EXPECT_IN(mon.enter("test"), ".C:8000  3E 05        LD A,05h");
    expect(mon.active(), __LINE__, "active after enter");
    EXPECT_IN(mon.prompt(), "(C:$8000) ");
    EXPECT_IN(mon.execute("r"), ".;8000 ");
    EXPECT_IN(mon.execute("z"), ".C:8002  3C");
    EXPECT_IN(mon.execute("z"), ".C:8003  CD 10 80     CALL 8010h");
    // n over a CALL leaves the monitor; the stop comes from run()
    expect(mon.execute("n").empty() && !mon.active(), __LINE__, "n over a call runs");
    EXPECT_IN(run_until_stop(mon), ".C:8006  00");
    EXPECT_IN(mon.execute("n 2"), ".C:8007");
    expect(mon.active(), __LINE__, "n 2 without calls stays in the monitor");

    mon.execute("r a=42, hl=1234, f=1");
    const freetvc::Z80Registers r = dbg.registers();
    expect(r.af == 0x4201 && r.hl == 0x1234, __LINE__, "r assignments");
    mon.execute("> 9000 11 22 33");
    EXPECT_IN(mon.execute("m 9000 9002"), ">C:9000  11 22 33");
    EXPECT_IN(mon.execute("m U2:1000 1002"), ">U2:1000  11 22 33");
    // A bank's end: no open-bus bytes shown, no write dropped in silence, nothing written by a refused line
    EXPECT_IN(mon.execute("m U2:4000"), "U2 has 16384 bytes: offsets 0000-3FFF");
    EXPECT_IN(mon.execute("m U2:3FFC"), ">U2:3FFC  ");
    EXPECT_IN(mon.execute("> U2:3FFF 12 34"), "error: U2 has 16384 bytes");
    EXPECT_IN(mon.execute("m U2:3FFF 3FFF"), ">U2:3FFF  00");
    EXPECT_IN(mon.execute("f 9000 9003 AA"), "$AA or 0AA");  // Hex that starts with a letter needs $ or 0
    mon.execute("f 9000 9003 0AA");
    EXPECT_IN(mon.execute("m 9000 9003"), "AA AA AA AA");
    const std::string hunt = mon.execute("h 8000 8010 $CD 10 80");
    EXPECT_IN(hunt, "8003\n");
    EXPECT_IN(hunt, "1 found");
    mon.execute("t 9000 9003 9100");
    EXPECT_IN(mon.execute("c 9000 9003 9100"), "0 differences");
    EXPECT_IN(mon.execute("bank"), "page 2  8000-BFFF  U2");
    EXPECT_IN(mon.execute("io"), "CRTC R0-R17:");

    // Checkpoints
    EXPECT_IN(mon.execute("break exec 8010"), "#1  exec       8010  stop  hits 0");
    EXPECT_IN(mon.execute("break"), "#1  exec");
    mon.execute("g 8000");
    expect(!mon.active(), __LINE__, "g leaves");
    EXPECT_IN(run_until_stop(mon), "#1 (Stop on exec 8010)  U2:0010");
    EXPECT_IN(mon.execute("ret"), "");
    EXPECT_IN(run_until_stop(mon), ".C:8006");
    EXPECT_IN(mon.execute("condition 1 if A == 99"), "if A == 99");
    mon.execute("g 8000");
    std::string report;
    mon.run(2 * kFrame, report);
    expect(!mon.active() && report.empty(), __LINE__, "condition false: no stop", report);
    mon.enter("again");
    EXPECT_IN(mon.execute("disable 1"), "(disabled)");
    EXPECT_IN(mon.execute("break U2:0010"), "#2  exec       U2:0010");
    mon.execute("g 8000");
    EXPECT_IN(run_until_stop(mon), "#2 (Stop on exec 8010)");
    EXPECT_IN(mon.execute("delete"), "deleted 2 checkpoint(s)");
    EXPECT_IN(mon.execute("trace exec 8007"), "trace");
    mon.execute("x");
    mon.run(kFrame, report);
    const std::string traced = mon.take_trace_lines();
    EXPECT_IN(traced, "#3 (Trace exec 8007)");
    EXPECT_IN(traced, "more trace hits)");  // a frame of JR $ is far more than 64 lines
    mon.enter("again");
    mon.execute("delete");
    EXPECT_IN(mon.execute("watch 9000"), "#4  load|store");
    mon.execute("x");
    EXPECT_IN(mon.execute("until 8010"), "");  // run_to while already running: replaced by the next command
    mon.enter("again");
    mon.execute("delete");
    mon.execute("g 8000");
    mon.enter("again");
    mon.execute("until 8010");
    EXPECT_IN(run_until_stop(mon), ".C:8010");

    // States, labels, files
    EXPECT_IN(mon.execute("snap save here"), "saved 'here'");
    mon.execute("z");
    EXPECT_IN(mon.execute("snap load here"), ".C:8010");
    EXPECT_IN(mon.execute("snap list"), "here");
    mon.execute("al 8010 sub");
    EXPECT_IN(mon.execute("d 8003 8003"), "CALL sub");
    EXPECT_IN(mon.execute("shl sub"), "sub                  = 8010");
    mon.execute("dl sub");
    EXPECT_IN(mon.execute("d 8003 8003"), "CALL 8010h");
    {
        std::ofstream labels(dir / "prog.sym");
        labels << "start: EQU 0x00008000\nloop = 0x8007\nbad line\n";
    }
    const std::string ll = mon.execute("ll \"" + (dir / "prog.sym").string() + "\"");
    EXPECT_IN(ll, "loaded 2 labels");
    EXPECT_IN(ll, "line 3:");
    EXPECT_IN(mon.execute("d start 8000"), "start:\n.C:8000");
    EXPECT_IN(mon.execute("cl"), "cleared 2 labels");
    const std::string bin = (dir / "code.bin").string();
    EXPECT_IN(mon.execute("bsave \"" + bin + "\" 8000 8008"), "saved 9 bytes");
    EXPECT_IN(mon.execute("bload \"" + bin + "\" U2:1000"), "loaded 9 bytes at U2:1000");
    EXPECT_IN(mon.execute("c 8000 8008 9000"), "0 differences");
    const std::string png = (dir / "shot.png").string();
    EXPECT_IN(mon.execute("scrsh \"" + png + "\""), "(650 x 287)");
    std::ifstream shot(png, std::ios::binary);
    const std::vector<char> bytes((std::istreambuf_iterator<char>(shot)), std::istreambuf_iterator<char>());
    expect(bytes.size() > 33 && std::string(bytes.begin() + 1, bytes.begin() + 4) == "PNG" &&
               static_cast<unsigned char>(bytes[18]) == 0x02 && static_cast<unsigned char>(bytes[19]) == 0x8A &&
               static_cast<unsigned char>(bytes[22]) == 0x01 && static_cast<unsigned char>(bytes[23]) == 0x1F,
           __LINE__, "PNG signature and 650 x 287 header");

    // Typing, radix, help, errors, reset, quit
    EXPECT_IN(mon.execute("keybuf \"print \\\"x\\\"\\n\""), "typing 10 characters");  // escapes: \" and \n
    EXPECT_IN(mon.execute("ll \"C:\\tmp\\no.sym\""), "cannot read 'C:\\tmp\\no.sym'");  // file names stay raw
    mon.execute("x");
    bool pressed = false;
    for (int f = 0; f < 6 && !pressed; ++f) {
        mon.run(kFrame / 2, report);
        for (unsigned row = 0; row < 10; ++row) {
            for (unsigned col = 0; col < 8; ++col) {
                pressed |= m->key_down(row, col);
            }
        }
    }
    expect(pressed, __LINE__, "keybuf presses a key while running");
    mon.enter("again");
    EXPECT_IN(mon.execute("radix d"), "radix decimal");
    mon.execute("> $9000 10");
    EXPECT_IN(mon.execute("m 0x9000 0x9000"), ">C:9000  0A");  // decimal now, so plain addresses are decimal too
    EXPECT_IN(mon.execute("m 9000 9000"), ">C:2328");
    mon.execute("radix h");
    EXPECT_IN(mon.execute("help"), "break [kind ...]");
    EXPECT_IN(mon.execute("help z"), "z [count]");
    EXPECT_IN(mon.execute("foo"), "error: unknown command 'foo'");
    EXPECT_IN(mon.execute("m zz"), "error: 'zz': unknown name");
    EXPECT_IN(mon.execute("keybuf \"unterminated"), "error: unterminated string");
    EXPECT_IN(mon.execute("break line 300"), "error: rows are");
    EXPECT_IN(mon.execute("break line #300"), "line");
    EXPECT_IN(mon.execute("sw"), "since the reset");
    EXPECT_IN(mon.execute("chis 2"), ".C:");
    EXPECT_IN(mon.execute("reset"), "(reset)\n.C:0000");
    mon.execute("quit 5");
    expect(mon.quit_code() == 5, __LINE__, "quit code");
}

void script_through_hub(const std::filesystem::path& dir) {
    auto m = program();
    freetvc::debug::Debugger dbg(*m);
    Monitor mon(dbg);
    MonitorHub hub(mon);
    const std::string log_path = (dir / "script.log").string();
    std::FILE* log = std::fopen(log_path.c_str(), "w");
    hub.set_log(log);
    hub.add_script({"break exec 8010", "g", "r", "quit 2"});
    for (int i = 0; i < 100 && !hub.quit_code(); ++i) {
        hub.service();
        hub.run(kFrame);
    }
    std::fclose(log);
    std::ifstream in(log_path);
    const std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    expect(hub.quit_code() == 2, __LINE__, "script quit code", text);
    EXPECT_IN(text, "(C:$8000) break exec 8010\n#1  exec");
    EXPECT_IN(text, "#1 (Stop on exec 8010)");
    EXPECT_IN(text, "(C:$8010) r\n");
    expect(!contains(text, ") (C:$"), __LINE__, "no doubled prompt", text);

    // A script that never runs the machine lets it go at the end
    auto m2 = program();
    freetvc::debug::Debugger dbg2(*m2);
    Monitor mon2(dbg2);
    MonitorHub hub2(mon2);
    hub2.set_log(nullptr);
    hub2.add_script({"break exec 8010"});
    hub2.service();
    expect(!mon2.active(), __LINE__, "machine runs after the script");
    hub2.run(kFrame);
    expect(mon2.active() && m2->pc() == 0x8010, __LINE__, "and stops at the breakpoint");
}

void local_client() {
    auto m = program();
    freetvc::debug::Debugger dbg(*m);
    Monitor mon(dbg);
    MonitorHub hub(mon);
    hub.set_log(nullptr);
    const int local = hub.attach_local();
    // A line while the machine runs enters the monitor first, as for a TCP client
    hub.submit_local(local, "break exec 8010");
    hub.service();
    std::string out = hub.take_local_output();
    EXPECT_IN(out, "(stopped by the monitor)");
    EXPECT_IN(out, "#1  exec");
    expect(hub.take_local_output().empty(), __LINE__, "output is taken once");
    // Stop reports from the run reach the local client too
    hub.submit_local(local, "x");
    hub.service();
    for (int i = 0; i < 10 && !mon.active(); ++i) {
        hub.run(kFrame);
    }
    out = hub.take_local_output();
    EXPECT_IN(out, "#1 (Stop on exec 8010)");
    EXPECT_IN(out, "(C:$8010) ");
}

void tcp_client() {
    auto m = program();
    freetvc::debug::Debugger dbg(*m);
    Monitor mon(dbg);
    auto hub_owner = std::make_unique<MonitorHub>(mon);
    MonitorHub& hub = *hub_owner;
    hub.set_log(nullptr);
    std::string error;
    expect(hub.listen(0, error), __LINE__, "listen: " + error);
    const std::uint16_t port = hub.port();
    std::string received;
    std::thread client([&] {
        std::string e;
        freetvc::remote::Connection c = freetvc::remote::Connection::connect_loopback(port, e);
        if (!c.open()) {
            received = "connect failed: " + e;
            return;
        }
        const auto read_prompt = [&](int prompts) {
            char buf[1024];
            int seen = 0;
            while (seen < prompts) {
                const long n = c.receive(buf, sizeof buf);
                if (n <= 0) {
                    return;
                }
                const std::string chunk(buf, static_cast<std::size_t>(n));
                received += chunk;
                for (std::size_t p = chunk.find("(C:$"); p != std::string::npos; p = chunk.find("(C:$", p + 1)) {
                    ++seen;
                }
            }
        };
        c.send_all("r\r\n");  // CR LF from telnet-like clients
        read_prompt(1);
        c.send_all("break exec 8010\ng 8000\n");
        read_prompt(2);  // the break command's prompt, then the stop's
        c.send_all("quit 0\n");
        char buf[64];
        c.receive(buf, sizeof buf);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
    while (!hub.quit_code() && std::chrono::steady_clock::now() < deadline) {
        hub.service();
        if (!mon.active()) {
            hub.run(kFrame / 8);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        } else {
            hub.wait(std::chrono::milliseconds(5));
        }
    }
    const bool quit = hub.quit_code() == 0;
    hub_owner.reset();  // closes the server side, so a client still waiting returns
    client.join();
    expect(quit, __LINE__, "quit over TCP", received);
    EXPECT_IN(received, "(stopped by the monitor)");
    EXPECT_IN(received, "  PC   AF   BC");
    EXPECT_IN(received, "#1 (Stop on exec 8010)");
    std::printf("TCP client on 127.0.0.1:%u: %zu bytes received\n", static_cast<unsigned>(port), received.size());
}

}  // namespace

int main(int argc, char** argv) {
    const std::filesystem::path dir = argc > 1 ? argv[1] : std::filesystem::temp_directory_path() / "freetvc_monitor_test";
    std::filesystem::create_directories(dir);
    commands(dir);
    script_through_hub(dir);
    local_client();
    tcp_client();
    if (g_failures == 0) {
        std::printf("monitor_test: all checks passed\n");
        return 0;
    }
    std::printf("monitor_test: %d check(s) failed\n", g_failures);
    return 1;
}
