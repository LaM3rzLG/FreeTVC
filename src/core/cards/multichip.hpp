// SPDX-License-Identifier: GPL-3.0-or-later
// Vass Sándor's TVC Multichip Soundcard v1.3, "SndMx" (design/cards.md "Multichip"): SN76489, SAA1099, YM3812.
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "core/cards/card.hpp"
#include "core/cards/saa1099.hpp"
#include "core/cards/sn76489.hpp"
#include "core/cards/ym3812.hpp"

namespace freetvc {

class MultichipCard final : public Card {
public:
    static constexpr std::size_t kRomSize = 0x8000;
    static constexpr std::size_t kBankSize = 0x2000;
    static constexpr std::uint32_t kSnHz = 3'579'545;

    // A short image reads FFh where missing (an empty EEPROM socket reads FFh too).
    explicit MultichipCard(std::span<const std::uint8_t> rom);

    std::unique_ptr<Card> clone() const override { return std::make_unique<MultichipCard>(*this); }
    CardKind kind() const override { return CardKind::Multichip; }
    unsigned id() const override { return 3; }  // IDA/IDB open: named by its ROM header
    void power_on() override;
    void reset(std::uint64_t tick) override;
    std::uint8_t io_read(unsigned port, const CardBus& bus) override;
    void io_write(unsigned port, std::uint8_t value, const CardBus& bus) override;
    std::uint8_t mem_read(std::uint16_t offset) const override { return window_[offset & 0x1FFF]; }
    void mem_write(std::uint16_t, std::uint8_t) override {}
    std::span<std::uint8_t> window() override { return window_; }
    void save(ChunkWriter& w) const override;
    void load(ChunkReader& r) override;
    void advance_audio(std::uint64_t tick, AudioSynth& out) override;
    bool irq() const override { return ym_.irq(); }
    std::uint64_t next_event() const override { return ym_.next_event(); }
    void run_events(const CardBus& bus) override;

    unsigned bank() const { return bank_; }
    const Sn76489& sn() const { return sn_; }
    const Saa1099& saa() const { return saa_; }
    const Ym3812& ym() const { return ym_; }

private:
    void select_bank(unsigned bank);

    std::array<std::uint8_t, kRomSize> rom_{};
    std::array<std::uint8_t, kBankSize> window_{};
    unsigned bank_ = 3;
    Sn76489 sn_{kSnHz};
    Saa1099 saa_;
    Ym3812 ym_;
};

}  // namespace freetvc
