// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/cards/i8251.hpp"

#include <algorithm>

#include "core/snapshot/snapshot.hpp"

namespace freetvc {

void I8251::reset(std::uint64_t) {
    phase_ = Phase::Mode;
    mode_ = 0;
    command_ = 0;
    sync_[0] = sync_[1] = 0;
    clear_sides();
}

void I8251::clear_sides() {
    errors_ = 0;
    tx_full_ = false;
    tx_end_ = kNever;
    rx_end_ = kNever;
    rx_ready_ = false;
}

std::uint64_t I8251::char_clocks(bool receive) const {
    if (!async()) {
        return 0;
    }
    static constexpr std::uint64_t kFactor[4] = {1, 1, 16, 64};
    const std::uint64_t factor = kFactor[mode_ & 3];
    const std::uint64_t bits = 1 + 5 + ((mode_ >> 2) & 3) + ((mode_ >> 4) & 1);  // start, data, parity
    if (receive) {
        // RxRDY comes with the stop bit's sample at its centre; one stop bit is enough (data sheet, "Asynchronous Mode (Receive)")
        return factor * bits + std::max<std::uint64_t>(factor / 2, 1);
    }
    static constexpr std::uint64_t kStopHalves[4] = {2, 2, 3, 4};  // 00 is not allowed: taken as 1 ❓
    return (factor * (2 * bits + kStopHalves[mode_ >> 6]) + 1) / 2;
}

bool I8251::can_send(const SerialLine* line) const {
    return phase_ == Phase::Command && async() && (command_ & kTxEn) && line && line->connected();
}

void I8251::start_send(std::uint64_t at) {
    tx_shift_ = tx_buffer_;
    tx_full_ = false;
    tx_end_ = at + char_clocks(false);
}

std::uint64_t I8251::bit_clocks() const {
    static constexpr std::uint64_t kFactor[4] = {0, 1, 16, 64};
    return kFactor[mode_ & 3];
}

bool I8251::listening(const SerialLine* line) const {
    // The host peer waits for an empty receiver, a courtesy a real one need not show (design/cards.md "Serial card")
    return rx_end_ == kNever && !rx_ready_ && phase_ == Phase::Command && async() && (command_ & kRxEn) && line && line->connected() &&
           (!line->waits_for_rts() || (command_ & kRts));
}

void I8251::try_receive(std::uint64_t at, SerialLine* line) {
    if (!listening(line)) {
        return;
    }
    std::uint8_t byte = 0;
    if (line->take(byte)) {
        rx_shift_ = byte;
        rx_end_ = at + char_clocks(true);
    }
}

void I8251::advance(std::uint64_t edges, SerialLine* line) {
    while (tx_end_ <= edges) {
        const std::uint64_t at = tx_end_;
        tx_end_ = kNever;
        if (line) {
            line->send(static_cast<std::uint8_t>(tx_shift_ & data_mask()));
        }
        if (tx_full_ && can_send(line)) {
            start_send(at);
        }
    }
    if (tx_end_ == kNever && tx_full_ && can_send(line)) {
        start_send(edges);
    }
    while (rx_end_ <= edges) {
        const std::uint64_t at = rx_end_;
        rx_end_ = kNever;
        if (rx_ready_) {
            errors_ |= kOverrun;  // the unread character is written over
        }
        rx_data_ = static_cast<std::uint8_t>(rx_shift_ & data_mask());
        rx_ready_ = true;
        try_receive(at, line);
    }
    try_receive(edges, line);
}

std::uint64_t I8251::next_event() const { return std::min(tx_end_, rx_end_); }

std::uint8_t I8251::read_data() {
    rx_ready_ = false;
    return rx_data_;
}

std::uint8_t I8251::read_status(bool dsr) const {
    std::uint8_t status = errors_;
    if (!tx_full_) {
        status |= 0x01;  // TxRDY: the buffer is empty, whatever TxEN and /CTS say (data sheet, figure 13 note 1)
    }
    if (rx_ready_) {
        status |= 0x02;
    }
    if ((!tx_full_ && tx_end_ == kNever) || !(command_ & kTxEn)) {
        status |= 0x04;  // TxEMPTY, which stays high while the transmitter is disabled
    }
    if (dsr) {
        status |= 0x80;
    }
    return status;
}

void I8251::write_data(std::uint8_t value, std::uint64_t edges, SerialLine* line) {
    tx_buffer_ = value;
    tx_full_ = true;
    if (tx_end_ == kNever && can_send(line)) {
        start_send(edges);
    }
}

void I8251::write_control(std::uint8_t value, std::uint64_t edges, SerialLine* line) {
    switch (phase_) {
    case Phase::Mode:
        mode_ = value;
        // A sync mode takes one (b7 = 1) or two sync characters before its commands
        phase_ = async() ? Phase::Command : (value & 0x80) ? Phase::Sync2 : Phase::Sync1;
        return;
    case Phase::Sync1:
        sync_[0] = value;
        phase_ = Phase::Sync2;
        return;
    case Phase::Sync2:
        sync_[1] = value;
        phase_ = Phase::Command;
        return;
    case Phase::Command:
        break;
    }
    if (value & kInternalReset) {
        reset(edges);
        return;
    }
    command_ = value;
    if (value & kErrorReset) {
        errors_ = 0;
    }
    if (!(value & kRxEn)) {
        // RxEnable off holds RxRDY reset; a character arriving is lost ❓
        rx_ready_ = false;
        rx_end_ = kNever;
    }
    if (tx_end_ == kNever && tx_full_ && can_send(line)) {
        start_send(edges);
    }
    try_receive(edges, line);
}

void I8251::save(ChunkWriter& w) const {
    w.u8(static_cast<std::uint8_t>(phase_));
    w.u8(mode_);
    w.u8(command_);
    w.u8(sync_[0]);
    w.u8(sync_[1]);
    w.u8(errors_);
    w.u8(tx_buffer_);
    w.boolean(tx_full_);
    w.u8(tx_shift_);
    w.u64(tx_end_);
    w.u8(rx_shift_);
    w.u64(rx_end_);
    w.u8(rx_data_);
    w.boolean(rx_ready_);
}

void I8251::load(ChunkReader& r) {
    const std::uint8_t phase = r.u8();
    if (phase > static_cast<std::uint8_t>(Phase::Command)) {
        r.fail();
        return;
    }
    phase_ = static_cast<Phase>(phase);
    mode_ = r.u8();
    command_ = r.u8();
    sync_[0] = r.u8();
    sync_[1] = r.u8();
    errors_ = r.u8();
    tx_buffer_ = r.u8();
    tx_full_ = r.boolean();
    tx_shift_ = r.u8();
    tx_end_ = r.u64();
    rx_shift_ = r.u8();
    rx_end_ = r.u64();
    rx_data_ = r.u8();
    rx_ready_ = r.boolean();
}

}  // namespace freetvc
