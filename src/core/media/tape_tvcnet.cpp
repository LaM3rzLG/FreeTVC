// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/media/tape_tvcnet.hpp"

#include <algorithm>
#include <cstdio>

namespace freetvc {

namespace {

constexpr std::uint64_t kTicksPerSecond = 6'250'000;  // half-T at 3.125 MHz

std::uint64_t to_us(std::uint64_t tick) { return tick * 4 / 25; }
std::uint64_t to_ticks(std::uint64_t us) { return us * 25 / 4; }

}  // namespace

std::optional<std::uint8_t> parse_tvcnet_socket(std::string_view name) {
    if (name == "ch12" || name == "yes") {
        return std::uint8_t{0x80};
    }
    if (name == "ch13") {
        return std::uint8_t{0x40};
    }
    return std::nullopt;
}

const char* tvcnet_socket_name(std::uint8_t motor_bit) { return motor_bit == 0x40 ? "ch13" : "ch12"; }

TapeTvcnet::TapeTvcnet() : modem_(*this, nullptr, {}, this) {}

void TapeTvcnet::set_line_code(const tvcnet::LineCode& code) {
    tvcnet::ModemConfig config = modem_.config();
    config.line = code;
    modem_.set_config(config);
}

void TapeTvcnet::reset() {
    modem_.reset();
    now_ = 0;
    line_low_ = false;
    receiving_ = false;
    in_level_ = false;
    edges_.clear();
}

std::uint64_t TapeTvcnet::sample_tick(unsigned index) const {
    return start_ + (2 * index + 1) * kTicksPerSecond / (2 * baud_);
}

void TapeTvcnet::receive_until(std::uint64_t tick) {
    // The line has kept line_low_ since the last change, which is not after any sample checked here
    while (receiving_ && sample_tick(bit_) < tick) {
        const bool high = !line_low_;
        if (bit_ == 0) {
            if (high) {
                receiving_ = false;  // a glitch, not a start bit
            }
        } else if (bit_ <= 8) {
            shift_ |= (high ? 1u : 0u) << (bit_ - 1);
        } else {
            receiving_ = false;
            if (high) {
                ++counters_.bytes;
                modem_.on_byte(static_cast<std::uint8_t>(shift_), to_us(sample_tick(bit_)));
            } else {
                ++counters_.framing_errors;
                line("line: framing error (low stop bit)");
            }
        }
        ++bit_;
    }
}

void TapeTvcnet::sync(std::uint64_t tick) {
    if (tick < now_) {
        reset();  // the machine's clock restarted: power-on or a restored state
    }
    now_ = tick;
    receive_until(tick + 1);
    modem_.poll(to_us(tick));
    while (!edges_.empty() && edges_.front() <= tick) {
        in_level_ = !in_level_;
        edges_.pop_front();
    }
}

bool TapeTvcnet::input(std::uint64_t tick) {
    sync(tick);
    return in_level_;
}

void TapeTvcnet::motor(std::uint64_t tick, std::uint8_t motors) {
    if (tick < now_) {
        reset();
    }
    now_ = tick;
    receive_until(tick);
    const bool low = (motors & socket_) != 0;
    if (low && !line_low_ && !receiving_) {
        receiving_ = true;
        start_ = tick;
        bit_ = 0;
        shift_ = 0;
    }
    line_low_ = low;
    sync(tick);
}

void TapeTvcnet::send(const std::vector<std::uint32_t>& halfperiods, std::uint64_t start_us) {
    const std::uint64_t start = std::max(to_ticks(start_us), now_ + 1);
    std::uint64_t us = 0;
    edges_.push_back(start);
    for (const std::uint32_t half : halfperiods) {
        us += half;
        edges_.push_back(start + to_ticks(us));
    }
}

void TapeTvcnet::line(std::string_view text) {
    if (log_.size() == kLogLines) {
        log_.pop_front();
    }
    log_.emplace_back(text);
}

std::vector<std::string> TapeTvcnet::take_log() {
    std::vector<std::string> lines(log_.begin(), log_.end());
    log_.clear();
    return lines;
}

}  // namespace freetvc
