// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/cards/twodfx_card.hpp"

#include "twodfx_emu.h"

#include <vector>

#include "core/snapshot/snapshot.hpp"

namespace freetvc {

namespace {

constexpr unsigned kRawPaperX = 126;  // [2DFX] the BASIC paper's first pixel (Ep128web patches/zzzz-2dfx.patch)
constexpr int kRawPaperY = 49;        // and its first line

// 2dfx index b0 I, b1 R, b2 G, b3 B (the bus EC0-3) -> freeTVC's I-G-R-B nibble
constexpr std::uint8_t to_igrb(std::uint8_t c) {
    return static_cast<std::uint8_t>(((c & 1) << 3) | (c & 6) | ((c >> 3) & 1));
}

}  // namespace

TwoDfxCard::TwoDfxCard() : dev_(twodfx_emu_create(TWODFX_EMU_TVC), twodfx_emu_destroy) {}

void TwoDfxCard::power_on() {
    if (dev_) {
        twodfx_emu_power_cycle(dev_.get());
    }
    last_vsync_ = last_display_ = paper_seen_ = false;
    chars_ = line_ = 0;
    coverage_.fill(0);
}

void TwoDfxCard::save(ChunkWriter& w) const {
    std::vector<std::uint8_t> blob(dev_ ? twodfx_emu_snapshot_size() : 0);
    if (!blob.empty() && !twodfx_emu_save_snapshot(dev_.get(), blob.data(), blob.size())) {
        blob.clear();
    }
    w.packed(blob);
    w.boolean(last_vsync_);
    w.boolean(last_display_);
    w.boolean(paper_seen_);
    for (const unsigned v : {chars_, line_, line_chars_, last_paper_char_, first_paper_line_, paper_origin_, paper_px_}) {
        w.u32(v);
    }
}

void TwoDfxCard::load(ChunkReader& r) {
    // A device of its own: machine copies share the old one
    dev_ = std::shared_ptr<twodfx_emu>(twodfx_emu_create(TWODFX_EMU_TVC), twodfx_emu_destroy);
    const std::size_t size = twodfx_emu_snapshot_size();
    std::vector<std::uint8_t> blob(size);
    r.packed(blob);
    if (r.failed() || !dev_ || !twodfx_emu_load_snapshot(dev_.get(), blob.data(), blob.size())) {
        r.fail();
        return;
    }
    last_vsync_ = r.boolean();
    last_display_ = r.boolean();
    paper_seen_ = r.boolean();
    for (unsigned* v : {&chars_, &line_, &line_chars_, &last_paper_char_, &first_paper_line_, &paper_origin_, &paper_px_}) {
        *v = r.u32();
    }
    coverage_.fill(0);
}

bool TwoDfxCard::irq() const { return dev_ && twodfx_emu_irq_asserted(dev_.get()) != 0; }

std::uint8_t TwoDfxCard::port_read(unsigned port) const {
    return port == 0 && dev_ ? twodfx_emu_read_f8(dev_.get()) : 0xFF;
}

void TwoDfxCard::port_write(unsigned port, std::uint8_t value) {
    if (!dev_) {
        return;
    }
    if (port == 0) {
        twodfx_emu_write_f8(dev_.get(), value);
    } else {
        twodfx_emu_write_f9(dev_.get(), value);
    }
}

void TwoDfxCard::video_character(bool display, bool vsync, std::uint8_t mode, std::uint8_t* px) {
    if (!dev_) {
        return;
    }
    if (vsync && !last_vsync_) {
        twodfx_emu_vsync(dev_.get());
    }
    if (!vsync && last_vsync_) {
        chars_ = line_ = 0;
        if (paper_seen_) {
            paper_origin_ = first_paper_line_;
        }
        paper_seen_ = false;
    }
    last_vsync_ = vsync;
    if (!vsync) {
        const unsigned line = ++chars_ / line_chars_;
        if (line != line_) {
            line_ = line;
            const int raw = static_cast<int>(line_) + kRawPaperY - static_cast<int>(paper_origin_);
            if (raw >= 0) {
                twodfx_emu_hsync(dev_.get(), static_cast<std::uint16_t>(raw));
            }
        }
    }
    if (display && !last_display_) {
        const unsigned gap = chars_ - last_paper_char_;
        if (paper_seen_ && gap >= 50 && gap <= 200) {
            line_chars_ = gap;
        }
        last_paper_char_ = chars_;
        if (!paper_seen_) {
            paper_seen_ = true;
            first_paper_line_ = line_;
        }
        line_start();
    }
    last_display_ = display;
    if (!display) {
        return;
    }
    if (px) {
        // [2DFX] every 2nd 2dfx pixel, doubled, in 4- and 16-colour mode (Ep128web zzzz-2dfx.patch; the HBA-2 sheet 9
        // reading gives the same for 16 colours). Every 4th broke the Wolf3D title's checkerboard dither into stripes.
        const unsigned mask = mode == 0 ? ~0u : ~1u;
        for (unsigned k = 0; k < 8; ++k) {
            const unsigned q = (paper_px_ + k) & mask;
            if (q < kLine && coverage_[q]) {
                px[k] = to_igrb(colours_[q]);
            }
        }
    }
    paper_px_ += 8;
}

void TwoDfxCard::line_start() {
    paper_px_ = 0;
    coverage_.fill(0);
    const int raw = static_cast<int>(line_) + kRawPaperY - static_cast<int>(paper_origin_);
    if (raw >= 0) {
        twodfx_emu_overlay_raw_line(dev_.get(), kRawPaperX, static_cast<unsigned>(raw), colours_.data(), coverage_.data(),
                                    kLine);
    }
}

}  // namespace freetvc
