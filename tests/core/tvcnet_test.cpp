// SPDX-License-Identifier: GPL-3.0-or-later
// TVCnet without ROMs (design/media.md "TVCnet"): the UART on a motor line, the replies on
// the tape input, the sockets shared with the deck, a clock restart.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/media/tape_deck.hpp"
#include "core/media/tape_tvcnet.hpp"
#include "core/media/tape_sockets.hpp"
#include "tvcnet/codes.hpp"
#include "tvcnet/frame.hpp"
#include "tvcnet/line_code.hpp"

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

constexpr std::uint64_t kTicksPerSecond = 6'250'000;

// Sends `bytes` as 8N1 on the motor bit `motor`, the bit time stretched by `skew` (1.0 exact)
void send_uart(freetvc::TapePort& port, std::uint64_t& tick, const std::vector<std::uint8_t>& bytes, unsigned baud,
               std::uint8_t motor, double skew = 1.0, bool low_stop = false) {
    const double bit = static_cast<double>(kTicksPerSecond) / baud * skew;
    for (const std::uint8_t byte : bytes) {
        double t = static_cast<double>(tick);
        const auto put = [&](bool high) {
            port.tape_motor(static_cast<std::uint64_t>(t), high ? 0 : motor);
            t += bit;
        };
        put(false);
        for (unsigned i = 0; i < 8; ++i) {
            put((byte >> i) & 1);
        }
        put(!low_stop);
        port.tape_motor(static_cast<std::uint64_t>(t), 0);
        tick = static_cast<std::uint64_t>(t + bit);
    }
}

std::vector<std::uint8_t> frame(std::uint8_t ctrl, const std::string& data = {}) {
    std::vector<std::uint8_t> out;
    tvcnet::build_frame(tvcnet::kSyncFromTvc, ctrl, reinterpret_cast<const std::uint8_t*>(data.data()), data.size(), out);
    return out;
}

struct Reply {
    std::vector<std::uint8_t> bytes;  // after SYNC: LEN, CTRL, data, CHK
    std::uint64_t first_edge = 0;
};

// Samples the tape input every 8 us like a polling TVC until it has been quiet for 20 ms
Reply listen(freetvc::TapePort& port, std::uint64_t& tick) {
    Reply reply;
    std::vector<std::uint32_t> halves;
    bool level = port.tape_input(tick);
    std::uint64_t last = 0, quiet_until = tick + kTicksPerSecond / 2;
    while (tick < quiet_until) {
        tick += 50;
        const bool now = port.tape_input(tick);
        if (now != level) {
            level = now;
            if (last) {
                halves.push_back(static_cast<std::uint32_t>((tick - last) * 4 / 25));
            } else {
                reply.first_edge = tick;
            }
            last = tick;
            quiet_until = tick + kTicksPerSecond / 50;
        }
    }
    reply.bytes = tvcnet::decode_line(tvcnet::LineCode{}, halves, tvcnet::kSyncToTvc);
    return reply;
}

void test_ping_through_sockets() {
    freetvc::TapeDeck deck;
    freetvc::TapeTvcnet tvcnet;
    freetvc::TapeSockets sockets(deck);
    sockets.plug(&tvcnet);
    std::uint64_t tick = 1000;
    send_uart(sockets, tick, frame(tvcnet::cmd::kPing), 2400, 0x80);
    const std::uint64_t sent = tick;
    const Reply reply = listen(sockets, tick);
    check(reply.bytes == std::vector<std::uint8_t>({1, 0, tvcnet::kProtocolVersion, 2}), "PING answers OK with the version");
    // The reply starts 2 ms after the middle of the last stop bit; `sent` is one bit after that bit's end
    const std::uint64_t stop_middle = sent - kTicksPerSecond * 3 / (2 * 2400);
    const std::uint64_t due = stop_middle + kTicksPerSecond / 500;
    check(reply.first_edge >= due && reply.first_edge <= due + 100, "the reply starts after the 2 ms turnaround");
    check(tvcnet.counters().bytes == 4 && tvcnet.counters().framing_errors == 0, "four bytes, no framing error");
}

void test_echo_with_skew() {
    for (const double skew : {0.97, 1.03}) {
        freetvc::TapeDeck deck;
        freetvc::TapeTvcnet tvcnet;
        freetvc::TapeSockets sockets(deck);
        sockets.plug(&tvcnet);
        std::uint64_t tick = 500;
        send_uart(sockets, tick, frame(0x80 | tvcnet::cmd::kEcho, "A\x6A\x04Z"), 2400, 0x80, skew);
        const Reply reply = listen(sockets, tick);
        check(reply.bytes == std::vector<std::uint8_t>({4, 0x80, 'A', 0x6A, 0x04, 'Z', 0x8D}), "ECHO comes back with a 3 % slow or fast UART");
    }
}

void test_sockets_and_errors() {
    freetvc::TapeDeck deck;
    deck.insert_blank();
    freetvc::TapeTvcnet tvcnet;
    freetvc::TapeSockets sockets(deck);
    sockets.plug(&tvcnet);
    std::uint64_t tick = 100;
    // The other socket's motor moves the deck and says nothing to TVCnet
    send_uart(sockets, tick, frame(tvcnet::cmd::kPing), 2400, 0x40);
    check(tvcnet.counters().bytes == 0, "CH13's motor line is not TVCnet's");
    sockets.tape_motor(tick, 0x40);
    check(deck.motor(), "the deck follows CH13");
    sockets.tape_motor(tick + 10, 0x80);
    check(!deck.motor(), "TVCnet's CH12 does not move the deck");
    sockets.tape_motor(tick + 20, 0);
    tick += kTicksPerSecond / 10;
    // A low stop bit is a framing error and no byte
    send_uart(sockets, tick, {0xA5}, 2400, 0x80, 1.0, true);
    check(tvcnet.counters().framing_errors == 1 && tvcnet.counters().bytes == 0, "a low stop bit is a framing error");
    // Unplugged: the deck takes both motor bits again
    sockets.plug(nullptr);
    sockets.tape_motor(tick, 0x80);
    check(deck.motor(), "without TVCnet either bit moves the deck");
}

void test_no_network_and_restart() {
    freetvc::TapeDeck deck;
    freetvc::TapeTvcnet tvcnet;
    freetvc::TapeSockets sockets(deck);
    sockets.plug(&tvcnet);
    std::uint64_t tick = 100;
    send_uart(sockets, tick, frame(tvcnet::cmd::kGet, "http://x/"), 2400, 0x80);
    Reply reply = listen(sockets, tick);
    check(reply.bytes.size() == 3 && reply.bytes[1] == tvcnet::status::kErrNoWifi, "GET without a network: no WiFi");
    // A request whose reply is pending, then the clock restarts (power-on, a restored state): the reply is dropped
    std::uint64_t again = tick;
    send_uart(sockets, again, frame(0x80 | tvcnet::cmd::kPing), 2400, 0x80);
    std::uint64_t restarted = 10;
    reply = listen(sockets, restarted);
    check(reply.bytes.empty(), "a clock restart drops the waiting reply");
    // 9600 baud on a TVCnet set to it
    tvcnet.set_baud(9600);
    restarted += 1000;
    send_uart(sockets, restarted, frame(tvcnet::cmd::kPing), 9600, 0x80);
    reply = listen(sockets, restarted);
    check(reply.bytes.size() == 4 && reply.bytes[1] == tvcnet::status::kOk, "PING at 9600 baud");
}

}  // namespace

int main() {
    test_ping_through_sockets();
    test_echo_with_skew();
    test_sockets_and_errors();
    test_no_network_and_restart();
    if (g_failures) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("tvcnet: all checks passed\n");
    return 0;
}
