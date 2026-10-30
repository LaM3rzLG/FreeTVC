// SPDX-License-Identifier: GPL-3.0-or-later
// The HBF floppy interface (Videoton 49111; tvc-hardware.md §9, design/cards.md "HBF").
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "core/cards/card.hpp"
#include "core/cards/wd1793.hpp"

namespace freetvc {

class HbfCard final : public Card {
public:
    static constexpr std::size_t kRomSize = 0x4000;
    static constexpr std::size_t kPageSize = 0x1000;

    explicit HbfCard(std::span<const std::uint8_t> rom);

    std::unique_ptr<Card> clone() const override { return std::make_unique<HbfCard>(*this); }
    CardKind kind() const override { return CardKind::Hbf; }
    unsigned id() const override { return 2; }
    void power_on() override;
    void reset(std::uint64_t tick) override;
    std::uint8_t io_read(unsigned port, const CardBus& bus) override;
    void io_write(unsigned port, std::uint8_t value, const CardBus& bus) override;
    std::uint8_t mem_read(std::uint16_t offset) const override { return window_[offset & 0x1FFF]; }
    void mem_write(std::uint16_t offset, std::uint8_t value) override;
    std::span<std::uint8_t> window() override { return window_; }
    void save(ChunkWriter& w) const override;
    void load(ChunkReader& r) override;

    const Wd1793& fdc() const { return fdc_; }
    std::uint8_t latch() const { return latch_; }
    unsigned page() const { return page_; }

private:
    void select_page(unsigned page);

    std::array<std::uint8_t, kRomSize> rom_{};
    std::array<std::uint8_t, 0x2000> window_{};  // the selected EPROM page, then the 4 KiB RAM
    unsigned page_ = 0;
    std::uint8_t latch_ = 0;
    Wd1793 fdc_;
};

}  // namespace freetvc
