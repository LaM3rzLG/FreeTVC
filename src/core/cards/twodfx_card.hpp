// SPDX-License-Identifier: GPL-3.0-or-later
// Vaczkó Károly's 2dfx graphics card (tvc-hardware.md §9, design/cards.md "2dfx") on the author's portable emulator
// (third_party/twodfx, MIT): commands on port F8h, data on F9h, its picture over the paper through EC0-3 and /EXTC.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>

#include "core/cards/card.hpp"

struct twodfx_emu;

namespace freetvc {

class TwoDfxCard final : public Card, public VideoOverlay {
public:
    TwoDfxCard();

    // Copies share the one device: its 8 MiB resource RAM is not duplicated for rewind points
    std::unique_ptr<Card> clone() const override { return std::make_unique<TwoDfxCard>(*this); }
    CardKind kind() const override { return CardKind::TwoDfx; }
    unsigned id() const override { return 3; }
    void power_on() override;
    void reset(std::uint64_t) override { power_on(); }  // the bus RESET drives the MCU's reset pin (Ep128web README) ⚑
    std::uint8_t io_read(unsigned, const CardBus&) override { return 0xFF; }
    void io_write(unsigned, std::uint8_t, const CardBus&) override {}
    std::uint8_t mem_read(std::uint16_t) const override { return 0xFF; }
    void mem_write(std::uint16_t, std::uint8_t) override {}
    std::span<std::uint8_t> window() override { return {}; }
    // The library's own snapshot (its 8 MiB resource RAM included, PackBits-packed) and the raster position
    void save(ChunkWriter& w) const override;
    void load(ChunkReader& r) override;
    VideoOverlay* overlay() override { return this; }
    bool irq() const override;

    // Ports F8h (0) and F9h (1)
    std::uint8_t port_read(unsigned port) const;
    void port_write(unsigned port, std::uint8_t value);

    void video_character(bool display, bool vsync, std::uint8_t mode, std::uint8_t* px) override;

    std::uint8_t status() const { return port_read(0); }

private:
    static constexpr unsigned kLine = 512;  // 2dfx pixels across the BASIC paper

    void line_start();

    std::shared_ptr<twodfx_emu> dev_;
    bool last_vsync_ = false;
    bool last_display_ = false;
    unsigned chars_ = 0;            // CRTC characters since the end of VSYNC
    unsigned line_ = 0;             // lines since the end of VSYNC
    unsigned line_chars_ = 100;     // characters per line, from the last two paper starts
    unsigned last_paper_char_ = 0;
    unsigned first_paper_line_ = 0;  // this frame's first paper line
    unsigned paper_origin_ = 24;     // the last frame's first paper line: raw line 49 ([2DFX] the BASIC paper)
    bool paper_seen_ = false;
    unsigned paper_px_ = 0;
    std::array<std::uint8_t, kLine> colours_{};
    std::array<std::uint8_t, kLine> coverage_{};
};

}  // namespace freetvc
