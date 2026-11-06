// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/video/video.hpp"

#include <algorithm>
#include <cstddef>

namespace freetvc {

namespace {

// A TV-like beam, not hardware: sync pulses are followed only inside a capture window, otherwise the beam free-runs.
constexpr unsigned kMinLineChars = 90;
constexpr unsigned kMaxLineChars = 104;
constexpr unsigned kMinFrameLines = 250;
constexpr unsigned kMaxFrameLines = 330;
constexpr unsigned kLineChars = Framebuffer::kWidth / 8;

// IGRB from the odd bits b7, b5, b3, b1 (border, left 16-colour pixel)
constexpr std::uint8_t igrb_odd(std::uint8_t v) {
    return static_cast<std::uint8_t>((v >> 7 & 1) << 3 | (v >> 5 & 1) << 2 | (v >> 3 & 1) << 1 | (v >> 1 & 1));
}

// IGRB from the even bits b6, b4, b2, b0 (palette, right 16-colour pixel)
constexpr std::uint8_t igrb_even(std::uint8_t v) {
    return static_cast<std::uint8_t>((v >> 6 & 1) << 3 | (v >> 4 & 1) << 2 | (v >> 2 & 1) << 1 | (v & 1));
}

}  // namespace

void Video::power_on() {
    crtc_.power_on();
    border_ = 0;
    palette_.fill(0);
    mode_ = 0;
    next_slot_ = 2;
    vblank_ = false;
    last_hsync_ = false;
    last_vsync_ = false;
    last_ma9_ = false;
    frame_pending_ = false;
    beam_x_ = 0;
    beam_y_ = 0;
    std::fill(work_.pixels.begin(), work_.pixels.end(), Framebuffer::kBlank);
    std::fill(front_.pixels.begin(), front_.pixels.end(), Framebuffer::kBlank);
    frame_count_ = 0;
}

void Video::set_border(std::uint8_t value) { border_ = igrb_odd(value); }

void Video::set_palette(unsigned index, std::uint8_t value) { palette_[index & 3] = igrb_even(value); }

unsigned Video::advance_to(std::uint64_t ticks, std::span<const std::uint8_t, kBankSize> vram, VideoOverlay* overlay) {
    unsigned pulses = 0;
    while (next_slot_ <= ticks) {
        pulses += step_character(vram, overlay) ? 1u : 0u;
        next_slot_ += 4;
    }
    return pulses;
}

bool Video::step_character(std::span<const std::uint8_t, kBankSize> vram, VideoOverlay* overlay) {
    const Mc6845::Output out = crtc_.step();
    std::uint8_t* paper = nullptr;

    if (out.hsync && !last_hsync_ && beam_x_ >= kMinLineChars) {
        new_line();
    }
    last_hsync_ = out.hsync;
    if (out.vsync && !last_vsync_) {
        vblank_ = true;
        frame_pending_ = true;
    }
    last_vsync_ = out.vsync;
    // The blank flip-flop set by VSYNC is cleared when MA9 goes high (row 72 with the ROM's values; other R12/R13 ❓)
    const bool ma9 = (out.ma & 0x200) != 0;
    if (ma9 && !last_ma9_) {
        vblank_ = false;
    }
    last_ma9_ = ma9;

    if (beam_x_ < kLineChars && beam_y_ < static_cast<unsigned>(Framebuffer::kHeight)) {
        std::uint8_t* px = &work_.pixels[static_cast<std::size_t>(beam_y_) * Framebuffer::kWidth + beam_x_ * 8];
        if (vblank_ || out.hsync) {
            // Horizontal blanking taken as the 6845 HSYNC width; the real pulse comes from a 74LS123 (❓)
            std::fill_n(px, 8, Framebuffer::kBlank);
        } else if (!out.display) {
            std::fill_n(px, 8, border_);
        } else {
            decode(vram[vram_address(out.ma, out.ra)], mode_, palette_, px);
            paper = px;
        }
    }
    if (overlay) {
        overlay->video_character(out.display, out.vsync, mode_, paper);
    }
    if (++beam_x_ >= kMaxLineChars) {
        new_line();
    }
    return out.cursor;
}

void Video::decode(std::uint8_t byte, std::uint8_t mode, const std::array<std::uint8_t, 4>& palette, std::uint8_t* px) {
    switch (mode & 3) {
    case 0:  // 2-colour: bit 7 is the leftmost pixel
        for (unsigned k = 0; k < 8; ++k) {
            px[k] = palette[byte >> (7 - k) & 1];
        }
        break;
    case 1:  // 4-colour: pixel k is C1:C0 = bit 3-k : bit 7-k, two dots wide
        for (unsigned k = 0; k < 4; ++k) {
            const std::uint8_t c = palette[(byte >> (3 - k) & 1) << 1 | (byte >> (7 - k) & 1)];
            px[2 * k] = c;
            px[2 * k + 1] = c;
        }
        break;
    default:  // 16-colour: two direct-colour pixels, four dots wide
        std::fill_n(px, 4, igrb_odd(byte));
        std::fill_n(px + 4, 4, igrb_even(byte));
        break;
    }
}

std::bitset<Video::kBankSize> displayed_vram(const std::array<std::uint8_t, Mc6845::kRegisters>& regs) {
    std::bitset<Video::kBankSize> shown;
    const unsigned start = (regs[12] & 0x3Fu) << 8 | regs[13];
    const unsigned rows = regs[6] & 0x7Fu;
    const unsigned lines = std::min((regs[9] & 0x1Fu) + 1, 4u);  // RA b1-0 only: further lines repeat these
    for (unsigned row = 0; row < rows; ++row) {
        for (unsigned column = 0; column < regs[1]; ++column) {
            const auto ma = static_cast<std::uint16_t>((start + row * regs[1] + column) & 0x3FFF);
            for (unsigned ra = 0; ra < lines; ++ra) {
                shown.set(Video::vram_address(ma, static_cast<std::uint8_t>(ra)));
            }
        }
    }
    return shown;
}

void Video::new_line() {
    beam_x_ = 0;
    if (frame_pending_ ? beam_y_ + 1 >= kMinFrameLines : beam_y_ + 1 >= kMaxFrameLines) {
        front_.pixels = work_.pixels;
        ++frame_count_;
        if (sink_.p) {
            sink_.p->frame_done(front_);
        }
        beam_y_ = 0;
    } else {
        ++beam_y_;
    }
    frame_pending_ = false;
}

}  // namespace freetvc
