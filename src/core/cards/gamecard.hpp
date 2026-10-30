// SPDX-License-Identifier: GPL-3.0-or-later
// dikdom's TVC-GameCard (tvc-hardware.md §9 "GameCard"; design/cards.md "GameCard"): SN76489A at +0, joysticks 3/4
// at +2/+3, control register at +Fh (b2-0 EPROM page, b3 sound clock), 64 KiB EPROM in 8 KiB pages at C000-DFFF.
#pragma once

#include <array>
#include <cstdint>
#include <span>

#include "core/cards/card.hpp"
#include "core/cards/sn76489.hpp"

namespace freetvc {

class GameCard final : public Card {
public:
    static constexpr std::size_t kRomSize = 0x10000;
    static constexpr std::size_t kPageSize = 0x2000;

    // An empty or short image reads FFh where missing (an empty EPROM socket).
    explicit GameCard(std::span<const std::uint8_t> rom);

    std::unique_ptr<Card> clone() const override { return std::make_unique<GameCard>(*this); }
    CardKind kind() const override { return CardKind::GameCard; }
    unsigned id() const override { return 3; }  // IDA/IDB are not wired: named by its ROM header
    void power_on() override;
    void reset(std::uint64_t tick) override;
    std::uint8_t io_read(unsigned port, const CardBus& bus) override;
    void io_write(unsigned port, std::uint8_t value, const CardBus& bus) override;
    std::uint8_t mem_read(std::uint16_t offset) const override { return window_[offset & 0x1FFF]; }
    void mem_write(std::uint16_t, std::uint8_t) override {}
    std::span<std::uint8_t> window() override { return window_; }
    void save(ChunkWriter& w) const override;
    void load(ChunkReader& r) override;
    void advance_audio(std::uint64_t tick, AudioSynth& out) override { sound_.advance_to(tick, out); }

    unsigned page() const { return page_; }
    const Sn76489& sound() const { return sound_; }

private:
    void select_page(unsigned page);

    std::array<std::uint8_t, kRomSize> rom_{};
    std::array<std::uint8_t, kPageSize> window_{};
    unsigned page_ = 7;
    Sn76489 sound_;
};

}  // namespace freetvc
