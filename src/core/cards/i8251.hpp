// SPDX-License-Identifier: GPL-3.0-or-later
// Intel 8251A USART from its data sheet (205222-002; design/cards.md "Serial card"), asynchronous modes only. Time is
// counted in rising edges of TxC/RxC, which the caller supplies; one clock drives both sides, as on the TVC's card.
#pragma once

#include <cstdint>

namespace freetvc {

class ChunkWriter;
class ChunkReader;

// The far end of the card's cable (outside the machine state, like the drives).
class SerialLine {
public:
    virtual ~SerialLine() = default;
    // The cable is in with a peer: /CTS and /DSR are on
    virtual bool connected() const = 0;
    // The peer sends only while the TVC's RTS is on (the factory cable's handshake)
    virtual bool waits_for_rts() const = 0;
    // The peer's next byte, taken when its start bit begins; false if it has none
    virtual bool take(std::uint8_t& byte) = 0;
    // A character the TVC finished sending
    virtual void send(std::uint8_t byte) = 0;
};

class I8251 {
public:
    static constexpr std::uint64_t kNever = UINT64_MAX;

    // Master reset (the RESET pin): the next control write is a mode instruction
    void reset(std::uint64_t edges);
    // Runs both sides up to `edges`; a null line is an unplugged cable
    void advance(std::uint64_t edges, SerialLine* line);
    // C/D = 0 / 1; the caller advances first
    std::uint8_t read_data();
    std::uint8_t read_status(bool dsr) const;
    void write_data(std::uint8_t value, std::uint64_t edges, SerialLine* line);
    void write_control(std::uint8_t value, std::uint64_t edges, SerialLine* line);
    // The clock count of the next character to finish, or kNever
    std::uint64_t next_event() const;
    // The receiver is idle and would start a character the line offers
    bool listening(const SerialLine* line) const;
    // Clocks per bit; 0 in a sync mode
    std::uint64_t bit_clocks() const;

    bool rts() const { return phase_ == Phase::Command && (command_ & kRts) != 0; }
    bool dtr() const { return phase_ == Phase::Command && (command_ & kDtr) != 0; }
    std::uint8_t mode() const { return mode_; }
    std::uint8_t command() const { return command_; }
    bool expecting_mode() const { return phase_ == Phase::Mode; }
    // Clock edges per character, sent (with every stop bit) or received (one stop bit); 0 in a sync mode
    std::uint64_t char_clocks(bool receive) const;

    void save(ChunkWriter& w) const;
    void load(ChunkReader& r);

private:
    enum class Phase : std::uint8_t { Mode, Sync1, Sync2, Command };
    static constexpr std::uint8_t kTxEn = 0x01, kDtr = 0x02, kRxEn = 0x04, kErrorReset = 0x10, kRts = 0x20, kInternalReset = 0x40;
    static constexpr std::uint8_t kParityError = 0x08, kOverrun = 0x10, kFramingError = 0x20;

    bool async() const { return (mode_ & 3) != 0; }
    std::uint8_t data_mask() const { return static_cast<std::uint8_t>(0xFF >> (3 - ((mode_ >> 2) & 3))); }
    bool can_send(const SerialLine* line) const;
    void start_send(std::uint64_t at);
    void try_receive(std::uint64_t at, SerialLine* line);
    void clear_sides();

    Phase phase_ = Phase::Mode;
    std::uint8_t mode_ = 0;
    std::uint8_t command_ = 0;
    std::uint8_t sync_[2]{};
    std::uint8_t errors_ = 0;  // PE, OE, FE in their status bits
    // Transmitter: the CPU's buffer, then the shift register with its last clock
    std::uint8_t tx_buffer_ = 0;
    bool tx_full_ = false;
    std::uint8_t tx_shift_ = 0;
    std::uint64_t tx_end_ = kNever;
    // Receiver: the character arriving, then the data output register
    std::uint8_t rx_shift_ = 0;
    std::uint64_t rx_end_ = kNever;
    std::uint8_t rx_data_ = 0;
    bool rx_ready_ = false;
};

}  // namespace freetvc
