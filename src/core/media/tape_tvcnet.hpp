// SPDX-License-Identifier: GPL-3.0-or-later
// TVCnet (design/media.md "TVCnet"): a UART on one socket's motor line (a set 05h bit
// pulls it low) and line-coded replies on 59h b5. Above that it runs the TVCnet firmware's own shared code.
#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "tvcnet/modem.hpp"

namespace freetvc {

// How TVCnet is set up; the front ends keep it in freetvc.ini.
struct TvcnetSettings {
    bool plugged = false;
    std::uint8_t socket = 0x80;  // the motor bit of its socket: 80h CH12, 40h CH13
    unsigned baud = 2400;
    unsigned t_us = 250;         // the reply line code's T
    bool network = true;         // the host's network stands for TVCnet's WiFi
};

// "ch12" (05h b7) or "ch13" (05h b6) as the motor bit ("yes" is ch12), else nullopt; and back.
std::optional<std::uint8_t> parse_tvcnet_socket(std::string_view name);
const char* tvcnet_socket_name(std::uint8_t motor_bit);

class TapeTvcnet final : private tvcnet::LineOut, private tvcnet::Log {
public:
    static constexpr std::uint8_t kSocketCh12 = 0x80;  // 05h b7
    static constexpr std::uint8_t kSocketCh13 = 0x40;  // 05h b6
    static constexpr unsigned kDefaultBaud = 2400;
    static constexpr std::size_t kLogLines = 256;

    struct Counters {
        std::uint32_t bytes = 0;           // characters received on the motor line
        std::uint32_t framing_errors = 0;  // a low stop bit
    };

    TapeTvcnet();

    // The far end's network; null answers every network command "no WiFi".
    void set_network(tvcnet::Network* net) { modem_.set_network(net); }
    void set_socket(std::uint8_t motor_bit) { socket_ = motor_bit; }
    std::uint8_t socket() const { return socket_; }
    void set_baud(unsigned baud) { baud_ = baud ? baud : kDefaultBaud; }
    unsigned baud() const { return baud_; }
    void set_line_code(const tvcnet::LineCode& code);
    const tvcnet::LineCode& line_code() const { return modem_.config().line; }
    // Power-on state: the line idle, nothing received or waiting.
    void reset();

    // The tape input level (59h b5) at `tick`, the machine's half-T time.
    bool input(std::uint64_t tick);
    // Port 05h's motor bits at `tick`.
    void motor(std::uint64_t tick, std::uint8_t motors);

    Counters counters() const { return counters_; }
    tvcnet::ModemStats stats() const { return modem_.stats(); }
    // The modem's frame log and the line errors since the last call
    std::vector<std::string> take_log();

private:
    void sync(std::uint64_t tick);
    void receive_until(std::uint64_t tick);
    std::uint64_t sample_tick(unsigned index) const;

    void send(const std::vector<std::uint32_t>& halfperiods, std::uint64_t start_us) override;
    bool sending() const override { return !edges_.empty(); }
    void line(std::string_view text) override;

    tvcnet::Modem modem_;
    std::uint8_t socket_ = kSocketCh12;
    unsigned baud_ = kDefaultBaud;
    std::uint64_t now_ = 0;

    bool line_low_ = false;   // the motor line, TVC -> TVCnet
    bool receiving_ = false;
    std::uint64_t start_ = 0; // the start bit's falling edge
    unsigned bit_ = 0;        // the next sample: 0 start, 1-8 data, 9 stop
    unsigned shift_ = 0;

    bool in_level_ = false;   // the tape input, TVCnet -> TVC
    std::deque<std::uint64_t> edges_;

    Counters counters_;
    std::deque<std::string> log_;
};

}  // namespace freetvc
