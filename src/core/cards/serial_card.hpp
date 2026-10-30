// SPDX-License-Identifier: GPL-3.0-or-later
// Videoton's serial line interface, HBS 49103 (design/cards.md "Serial card"): an 8251A on the slot's 16 ports
// (A0 = C/D), clocked by the bus TXRXCLK, ID 00, no ROM and no interrupt line.
#pragma once

#include <cstdint>
#include <span>

#include "core/cards/card.hpp"
#include "core/cards/i8251.hpp"

namespace freetvc {

class SerialCard final : public Card {
public:
    SerialCard() { power_on(); }

    std::unique_ptr<Card> clone() const override { return std::make_unique<SerialCard>(*this); }
    CardKind kind() const override { return CardKind::Serial; }
    unsigned id() const override { return 0; }  // TB1 grounds IDA and IDB: "RS232"
    void power_on() override;
    void reset(std::uint64_t tick) override;
    std::uint8_t io_read(unsigned port, const CardBus& bus) override;
    void io_write(unsigned port, std::uint8_t value, const CardBus& bus) override;
    std::uint8_t mem_read(std::uint16_t) const override { return 0xFF; }
    void mem_write(std::uint16_t, std::uint8_t) override {}
    std::span<std::uint8_t> window() override { return {}; }
    void save(ChunkWriter& w) const override;
    void load(ChunkReader& r) override;
    std::uint64_t next_event() const override { return next_event_; }
    void run_events(const CardBus& bus) override;

    const I8251& usart() const { return usart_; }
    // The status as a read would give it now, for describing the card
    std::uint8_t status(const CardBus& bus) const { return usart_.read_status(bus.serial && bus.serial->connected()); }

private:
    void catch_up(const CardBus& bus);
    void schedule(const CardBus& bus);

    I8251 usart_;
    std::uint64_t next_event_ = UINT64_MAX;
};

}  // namespace freetvc
