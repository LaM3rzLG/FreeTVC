// SPDX-License-Identifier: GPL-3.0-or-later
// The printer port and the serial card without system ROMs (design/machine.md "Printer port", design/cards.md
// "Serial card"): flip-flops, the 8251A's timing on TXRXCLK, the handshake, copies and snapshots.
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "core/cards/serial_card.hpp"
#include "core/machine/machine.hpp"
#include "core/snapshot/snapshot.hpp"

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

constexpr std::uint8_t kData = 0x20, kControl = 0x21;  // slot 1
// The ROM's BAUD table (EXT 2.x E403h): 110, 150, 300, 600, 1200, 2400, 4800, 9600, 19 200 baud
constexpr std::uint16_t kBaudPitch[9] = {0xC88, 0xD75, 0xEBA, 0xF5D, 0xFAF, 0xFD7, 0xFEC, 0xFF6, 0xFFB};

struct Printer final : freetvc::PrinterPort {
    std::vector<std::pair<std::uint64_t, std::uint8_t>> bytes;
    void printer_byte(std::uint64_t tick, std::uint8_t value) override { bytes.emplace_back(tick, value); }
};

struct Line final : freetvc::SerialLine {
    bool plugged = true;
    bool rts_flow = true;
    std::deque<std::uint8_t> incoming;
    std::string sent;
    bool connected() const override { return plugged; }
    bool waits_for_rts() const override { return rts_flow; }
    bool take(std::uint8_t& byte) override {
        if (incoming.empty()) {
            return false;
        }
        byte = incoming.front();
        incoming.pop_front();
        return true;
    }
    void send(std::uint8_t byte) override { sent.push_back(static_cast<char>(byte)); }
};

// All RAM, a JR $ at 0000h: the CPU spins while the test drives the ports
freetvc::Machine spinning(bool serial_card) {
    freetvc::MachineConfig config;
    if (serial_card) {
        config.cards[freetvc::kSerialSlot] = {freetvc::CardKind::Serial, {}};
    }
    freetvc::Machine m(config);
    m.io_write(0x02, 0xB0);
    const std::uint8_t loop[] = {0x18, 0xFE};
    m.load(0x0000, loop);
    m.jump(0x0000);
    return m;
}

void run_ticks(freetvc::Machine& m, std::uint64_t ticks) {
    const std::uint64_t end = m.ticks() + ticks;
    while (m.ticks() < end) {
        if (m.run({.max_ticks = end - m.ticks()}).ticks_run == 0) {
            break;
        }
    }
}

bool ack(freetvc::Machine& m) { return (m.io_read(0x59) & 0x80) != 0; }

void strobe(freetvc::Machine& m, std::uint8_t value) {
    m.io_write(0x01, value);
    m.io_write(0x06, 0x00);
    m.io_write(0x06, 0x80);
}

void test_printer() {
    freetvc::Machine m = spinning(false);
    check(ack(m), "after power-on the ACK flip-flop is set");
    strobe(m, 'A');
    check(!ack(m), "/STROBE clears the ACK flip-flop");
    run_ticks(m, 100'000);
    check(!ack(m), "unplugged: no /ACK ever comes");
    m.reset();
    check(ack(m) && m.printer_data() == 0 && m.printer_strobe(), "reset sets both flip-flops and clears the latch");

    Printer printer;
    m.set_printer(&printer);
    strobe(m, 'B');
    check(printer.bytes.size() == 1 && printer.bytes[0].second == 'B', "the printer takes the byte on /STROBE's falling edge");
    check(!ack(m), "busy right after the strobe");
    run_ticks(m, 40);
    check(!ack(m), "still busy before 10 µs");
    run_ticks(m, 40);
    check(ack(m), "ready 10 µs after the strobe");

    // /STROBE left low: the printer's /ACK ends it by itself
    m.io_write(0x01, 'C');
    m.io_write(0x06, 0x00);
    run_ticks(m, 100);
    check(ack(m) && m.printer_strobe(), "the /ACK pulse sets the /STROBE flip-flop again");

    // A snapshot between the strobe and the /ACK keeps the pending pulse
    strobe(m, 'D');
    freetvc::Snapshot snap;
    m.save_state(snap);
    freetvc::Machine other = spinning(false);
    std::string error;
    check(other.load_state(snap, error), "snapshot with a pending /ACK loads: " + error);
    check(!ack(other), "the loaded machine is busy");
    run_ticks(other, 80);
    check(ack(other), "and ready on time");
}

void program(freetvc::Machine& m, std::uint16_t pitch, std::uint8_t mode, std::uint8_t command) {
    m.io_write(0x04, static_cast<std::uint8_t>(pitch));
    m.io_write(0x05, static_cast<std::uint8_t>(0x10 | pitch >> 8));
    for (const std::uint8_t b : {0x00, 0x00, 0x00, 0x40}) {
        m.io_write(kControl, b);
    }
    m.io_write(kControl, mode);
    m.io_write(kControl, command);
}

// Ticks until the line has `count` bytes, in steps of `step`; 0 if it takes longer than `limit`
std::uint64_t ticks_until_sent(freetvc::Machine& m, Line& line, std::size_t count, std::uint64_t step, std::uint64_t limit) {
    const std::uint64_t start = m.ticks();
    while (line.sent.size() < count) {
        if (m.ticks() - start > limit) {
            return 0;
        }
        run_ticks(m, step);
    }
    return m.ticks() - start;
}

void test_serial() {
    freetvc::Machine m = spinning(true);
    check((m.io_read(0x5A) & 0x0C) == 0, "port 5Ah: slot 1 reads ID 00 (RS232)");
    const auto* card = dynamic_cast<const freetvc::SerialCard*>(m.cards().card(freetvc::kSerialSlot));
    check(card && card->usart().expecting_mode(), "after power-on the 8251A waits for its mode");

    // Unplugged: /CTS and /DSR are off, so nothing leaves
    program(m, 0xFAF, 0xEE, 0x05);
    check((m.io_read(kControl) & 0x80) == 0, "unplugged: DSR off");
    m.io_write(kData, 'x');
    run_ticks(m, 200'000);
    check((m.io_read(kControl) & 0x05) == 0, "unplugged: the byte stays in the buffer (TxRDY and TxEMPTY low)");

    Line line;
    m.set_serial_line(&line);
    check((m.io_read(kControl) & 0x80) != 0, "plugged in: DSR on");
    // The ROM's mode EEh: ×16, 8 bits, no parity, 2 stop bits: 176 clocks a character, a clock every 4·(4096 − P) ticks
    for (unsigned i = 0; i < 9; ++i) {
        program(m, kBaudPitch[i], 0xEE, 0x05);
        line.sent.clear();
        const std::uint64_t per_char = 176ull * 4 * (0x1000 - kBaudPitch[i]);
        m.io_write(kData, static_cast<std::uint8_t>('0' + i));
        const std::uint64_t took = ticks_until_sent(m, line, 1, 4 * (0x1000 - kBaudPitch[i]), 2 * per_char);
        check(took + 2 * 4 * (0x1000 - kBaudPitch[i]) >= per_char && took <= per_char + 2 * 4 * (0x1000 - kBaudPitch[i]),
              "pitch " + std::to_string(kBaudPitch[i]) + ": a character takes 176 clocks (" + std::to_string(took) + " of " +
                  std::to_string(per_char) + " ticks)");
        check(line.sent == std::string(1, static_cast<char>('0' + i)), "pitch " + std::to_string(kBaudPitch[i]) + ": the byte arrives");
    }

    // Two bytes back to back at 1200 baud: the second waits in the buffer
    program(m, 0xFAF, 0xEE, 0x05);
    line.sent.clear();
    m.io_write(kData, 'A');
    check((m.io_read(kControl) & 0x05) == 0x01, "the first byte goes to the shift register at once: TxRDY high, TxEMPTY low");
    m.io_write(kData, 'B');
    check((m.io_read(kControl) & 0x01) == 0, "the second waits: TxRDY low");
    const std::uint64_t two = ticks_until_sent(m, line, 2, 324, 3 * 176 * 324);
    check(line.sent == "AB" && two >= 2 * 176 * 324 - 2 * 324 && two <= 2 * 176 * 324 + 2 * 324, "two characters in 352 clocks");
    check((m.io_read(kControl) & 0x05) == 0x05, "then TxRDY and TxEMPTY");

    // 7 bits: the top bit is not sent
    program(m, 0xFAF, 0xEA, 0x05);
    line.sent.clear();
    m.io_write(kData, 0xC1);
    ticks_until_sent(m, line, 1, 324, 200 * 324);
    check(line.sent == "A", "7 data bits send the low seven");

    // Receiving: the peer waits for the TVC's RTS (the factory cable)
    program(m, 0xFAF, 0xEE, 0x05);
    line.incoming = {'X', 'Y'};
    run_ticks(m, 200 * 324);
    check((m.io_read(kControl) & 0x02) == 0 && line.incoming.size() == 2, "without RTS the peer sends nothing");
    m.io_write(kControl, 0x25);
    run_ticks(m, 150 * 324);
    check((m.io_read(kControl) & 0x02) == 0, "a character takes 152 clocks to arrive (one stop bit)");
    run_ticks(m, 4 * 324);
    check((m.io_read(kControl) & 0x02) != 0 && line.incoming.size() == 1, "RxRDY after 152 clocks");
    run_ticks(m, 400 * 324);
    check(line.incoming.size() == 1, "the peer waits while the receiver holds an unread character");
    check(m.io_read(kData) == 'X' && (m.io_read(kControl) & 0x02) == 0, "reading takes it and clears RxRDY");
    run_ticks(m, 160 * 324);
    check(m.io_read(kData) == 'Y' && (m.io_read(kControl) & 0x38) == 0, "the next one follows, no errors");

    // flow none: the peer sends without RTS
    line.rts_flow = false;
    m.io_write(kControl, 0x05);
    line.incoming = {'Z'};
    run_ticks(m, 180 * 324);  // the card looks at the line once a bit, then 152 clocks
    check((m.io_read(kControl) & 0x02) != 0 && m.io_read(kData) == 'Z', "flow none: a byte arrives without RTS");
    line.rts_flow = true;

    // The ROM's reset sequence from a sync mode: two sync characters, then a command that resets
    m.io_write(kControl, 0x40);
    m.io_write(kControl, 0x00);  // sync mode, two sync characters
    check(!card->usart().expecting_mode(), "a sync mode waits for its sync characters");
    for (const std::uint8_t b : {0x00, 0x00, 0x00, 0x40}) {
        m.io_write(kControl, b);
    }
    check(card->usart().expecting_mode(), "00h 00h 00h 40h ends in the mode from a sync mode too");

    // A copy and a snapshot taken mid-character finish it at the same time
    program(m, 0xFAF, 0xEE, 0x05);
    line.sent.clear();
    m.io_write(kData, 'Q');
    run_ticks(m, 60 * 324);
    freetvc::Machine copy(m);
    Line copy_line;
    copy.set_serial_line(&copy_line);
    freetvc::Snapshot snap;
    m.save_state(snap);
    freetvc::Machine loaded = spinning(true);
    Line loaded_line;
    loaded.set_serial_line(&loaded_line);
    std::string error;
    check(loaded.load_state(snap, error), "snapshot mid-character loads: " + error);
    const std::uint64_t a = ticks_until_sent(m, line, 1, 64, 200 * 324);
    const std::uint64_t b = ticks_until_sent(copy, copy_line, 1, 64, 200 * 324);
    const std::uint64_t c = ticks_until_sent(loaded, loaded_line, 1, 64, 200 * 324);
    check(a != 0 && a == b && a == c && copy_line.sent == "Q" && loaded_line.sent == "Q", "the copy and the snapshot send Q with the original");
    freetvc::Snapshot after_m, after_loaded;
    m.save_state(after_m);
    loaded.save_state(after_loaded);
    check(after_m.encode() == after_loaded.encode(), "and their states stay byte-identical");

    // The reset button resets the chip
    m.reset();
    check(card->usart().expecting_mode(), "reset: the 8251A waits for its mode again");
}

}  // namespace

int main() {
    test_printer();
    test_serial();
    if (g_failures == 0) {
        std::printf("serial_test: all checks passed\n");
    }
    return g_failures == 0 ? 0 : 1;
}
