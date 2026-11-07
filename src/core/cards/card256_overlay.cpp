// SPDX-License-Identifier: GPL-3.0-or-later
// TVC256k++ overlay (design/cards.md "Overlay"): line timing and composition after [FW] tvc-graphics.c, power-on pictures.
#include <algorithm>
#include <cstdint>

#include "core/cards/card256.hpp"

namespace freetvc {

namespace {

constexpr std::uint16_t kLinesAbove = 21;
constexpr unsigned kLeft = 24;  // line-buffer pixels left of the picture
constexpr unsigned kTransparent = 8;

// The card's IRGB (b3 I, b2 R, b1 G, b0 B) as the framebuffer's IGRB
constexpr std::uint8_t to_igrb(std::uint8_t c) {
    return static_cast<std::uint8_t>((c & 0x09) | ((c & 0x02) << 1) | ((c & 0x04) >> 1));
}

// Power-on sprites: [FW] tvc-graphics.c baloon/rectangle, videoton_sprites.h, tvc-hat.h (dikdom, MIT)
constexpr std::uint8_t kBalloon[] = {0, 127, 0,   1, 255, 192, 3, 255, 224, 3, 231, 224, 7, 217, 240, 7,   223, 240, 7, 217, 240,
                                     3, 231, 224, 3, 255, 224, 3, 255, 224, 2, 255, 160, 1, 127, 64,  1,   62,  64,  0, 156, 128,
                                     0, 156, 128, 0, 73,  0,   0, 73,  0,   0, 62,  0,   0, 62,  0,   0,   62,  0,   0, 28,  0};
constexpr std::uint8_t kRectangle[] = {255, 255, 255, 160, 0, 1, 240, 0, 1, 0,   0, 1, 224, 0,   1,   0,   0, 1, 192, 0, 1,
                                       128, 0,   1,   128, 0, 1, 128, 0, 1, 128, 0, 1, 128, 0,   1,   128, 0, 1, 128, 0, 1,
                                       128, 0,   1,   128, 0, 1, 128, 0, 1, 128, 0, 1, 128, 0,   1,   128, 0,   1,
                                       255, 255, 255};
static_assert(sizeof kBalloon == 63 && sizeof kRectangle == 63);
constexpr std::uint8_t kVideotonSprites[512] = {
    0x00, 0x00, 0x00, 0x3C, 0x00, 0x0C, 0x46, 0x03, 0xFC, 0x43, 0x03, 0x0C, 0x61, 0x86, 0x0C, 0x30, 0xCC, 0x18, 0x30, 0x68, 0x30, 0x18,
    0x38, 0x30, 0x0C, 0x30, 0x60, 0x06, 0x00, 0xC0, 0x06, 0x00, 0x80, 0x03, 0x01, 0x80, 0x01, 0x83, 0x00, 0x01, 0x83, 0x00, 0x00, 0xC6,
    0x00, 0x00, 0xCC, 0x00, 0x00, 0x68, 0x00, 0x00, 0x78, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00,
    0x00, 0x01, 0xF8, 0x00, 0x03, 0x18, 0x00, 0x03, 0x18, 0x00, 0x03, 0x18, 0x00, 0x03, 0x18, 0x00, 0x03, 0x18, 0x00, 0x03, 0x18, 0x00,
    0x03, 0x18, 0x00, 0x01, 0x8C, 0x00, 0x01, 0x8C, 0x00, 0x00, 0xC6, 0x00, 0x00, 0xC6, 0x00, 0x00, 0xC6, 0x00, 0x00, 0xC6, 0x00, 0x00,
    0x7C, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x1F,
    0xF8, 0x00,
    0x18, 0x0E, 0x00, 0x30, 0x03, 0x00, 0x30, 0x01, 0x80, 0x33, 0xC1, 0x80, 0x33, 0x60, 0xC0, 0x33, 0x30, 0xC0, 0x33, 0x18, 0xC0, 0x33,
    0x18, 0xC0, 0x33, 0x30, 0xC0, 0x31, 0xE1, 0x80, 0x31, 0x81, 0x80, 0x30, 0x03, 0x00, 0x30, 0x06, 0x00, 0x30, 0x1C, 0x00, 0x60, 0xF0,
    0x00, 0x7F, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x07, 0xFE, 0x00, 0x7C, 0x07,
    0x80, 0x60, 0x01, 0x80, 0x60, 0x01, 0x80, 0x30, 0x7F, 0x00, 0x33, 0xE0, 0x00, 0x33, 0x00, 0x00, 0x31, 0xC0, 0x00, 0x30, 0x60, 0x00,
    0x30, 0x60, 0x00, 0x31, 0xC0, 0x00, 0x73, 0x00, 0x00, 0x63, 0x80, 0x00, 0x61, 0xFF, 0xC0, 0x60, 0x00, 0x60, 0x60, 0x00, 0x60, 0x60,
    0x00, 0xC0, 0x7F, 0xFF, 0x80, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x01, 0x81, 0xC0, 0x03,
    0x00, 0x60, 0x06, 0x00, 0x30, 0x0C, 0x00, 0x38, 0x18, 0x00, 0x18, 0x18, 0x7E, 0x18, 0x18, 0x63, 0x0C, 0x30, 0xC3, 0x0C, 0x30, 0xC1,
    0x98, 0x30, 0xC1, 0x90, 0x30, 0x7F, 0x90, 0x18, 0x06, 0x30, 0x18, 0x00, 0x30, 0x0E, 0x00, 0x60, 0x03, 0x80, 0xC0, 0x00, 0xF9, 0xC0,
    0x00, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x07, 0x8F, 0xE0, 0x1F, 0xFF, 0xE0,
    0x1F, 0xFF, 0xE0, 0x1F, 0xFF, 0xC0, 0x1F, 0xFE, 0x00, 0x00, 0x7C, 0x00, 0x00, 0x7C, 0x00, 0x00, 0x7C, 0x00, 0x00, 0x7C, 0x00, 0x00,
    0x7C, 0x00, 0x00, 0x7C, 0x00, 0x00, 0x7C, 0x00, 0x00, 0x7C, 0x00, 0x00, 0x78, 0x00, 0x00, 0x78, 0x00, 0x00, 0x78, 0x00, 0x00, 0x30,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x7F, 0x00, 0x01, 0xFF, 0x80, 0x03, 0xFF, 0xE0, 0x07, 0xFF,
    0xE0, 0x07, 0xFF, 0xF0, 0x0F, 0xFF, 0xF0, 0x1F, 0xFF, 0xF0, 0x1F, 0xC1, 0xF8, 0x3F, 0x81, 0xF8, 0x3F, 0x81, 0xF0, 0x3F, 0xC0, 0xF0,
    0x3F, 0xFF, 0xF0, 0x1F, 0xFF, 0xF0, 0x1F, 0xFF, 0xE0, 0x0F, 0xFF, 0xE0, 0x03, 0xFF, 0xC0, 0x00, 0xFF, 0xC0, 0x00, 0x0E, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x78, 0x03, 0xC0, 0xFC, 0x0F, 0xE0, 0xFC, 0x0F, 0xF0, 0xFC, 0x0F,
    0xF8, 0xF8, 0x0F, 0xFC, 0xF8, 0x0F, 0xFF, 0xF8, 0x0F, 0xFF, 0xF8, 0x0F, 0xFF, 0xF8, 0x0F, 0x9F, 0xF8, 0x0F, 0x9F, 0xF8, 0x1F, 0x8F,
    0xF8, 0x1F, 0x8F, 0xF8, 0x1F, 0x8F, 0xF8, 0x1F, 0x87, 0xF8, 0x1F, 0x03, 0xF0, 0x1F, 0x03, 0xF0, 0x0E, 0x00, 0xF0, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x01};
constexpr std::uint8_t kHat[252] = {
    0x88, 0x88, 0x88, 0x88, 0x77, 0x77, 0x77, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x87, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x88,
    0x88, 0x88, 0x88, 0x87, 0x77, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xF7, 0x77, 0x77, 0x88, 0x88, 0x77, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0xF7, 0x77, 0x88, 0x88, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xF7, 0x77, 0x78, 0x88, 0xFF, 0xFF, 0xF4, 0x44, 0x44,
    0x44, 0x44, 0xFF, 0xF7, 0x77, 0x78, 0x88, 0xFF, 0x44, 0x44, 0xCC, 0xCC, 0xC4, 0x44, 0x44, 0x44, 0x77, 0x88, 0x88, 0x84, 0x4C, 0xCC,
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x44, 0x44, 0x88, 0x88, 0x8C, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xC4, 0x48, 0x88, 0x88, 0x8C,
    0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xC4, 0x48, 0x88, 0x88, 0x88, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x44, 0x88, 0x88,
    0x88, 0x88, 0x8C, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xC4, 0x48, 0x88, 0x88, 0x88, 0x88, 0x88, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xC4, 0x48,
    0x77, 0x78, 0x88, 0x88, 0x88, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x44, 0x87, 0x7F, 0xF8, 0x88, 0x88, 0x88, 0x8C, 0xCC, 0xCC, 0xCC, 0xC4,
    0x48, 0x87, 0xFF, 0xF7, 0x88, 0x88, 0x88, 0x88, 0xCC, 0xCC, 0xCC, 0xC4, 0x48, 0x84, 0x4F, 0x78, 0x88, 0x88, 0x88, 0x88, 0x8C, 0xCC,
    0xCC, 0xCC, 0x44, 0x44, 0xCC, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC, 0x88, 0x88, 0x88, 0x88, 0x88,
    0x88, 0x8C, 0xCC, 0xCC, 0xCC, 0xCC, 0xC8, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x8C, 0xCC, 0xCC, 0xC8, 0x88, 0x88, 0x88, 0x88,
    0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88, 0x88};

}  // namespace

// The MCU's start ([FW] TVC-IO-main-gfx.c preinit_core_gfx, tvc-graphics.c init_graphics, tvc-routines.c clear_text_screen)
void Card256::load_pictures() {
    if (pictures_) {
        const std::uint8_t* font = pictures_->data();
        const std::uint8_t* picture = font + kFontSize;
        std::copy_n(font, kFontSize, fast_.begin() + (font_base_ & 0x3F) * 0x800);
        const std::size_t bitmap = (bitmap_base_ & 3u) * 0x8000;
        for (std::size_t row = 0; row < 240; ++row) {
            std::copy_n(picture + (239 - row) * 128, 128, fast_.begin() + static_cast<std::ptrdiff_t>(bitmap + row * 128));
        }
    }
    std::copy(std::begin(kBalloon), std::end(kBalloon), fast_.begin());
    std::copy(std::begin(kRectangle), std::end(kRectangle), fast_.begin() + 64);
    std::copy(std::begin(kVideotonSprites), std::end(kVideotonSprites), fast_.begin() + 128);
    const std::size_t hat = sprite_base_ * kPageSize + 0x40 * 256;
    for (std::size_t row = 0; row < 21; ++row) {
        std::copy_n(kHat + (20 - row) * 12, 12, fast_.begin() + static_cast<std::ptrdiff_t>(hat + row * 12));
    }
    const std::size_t screen = screen_base_ * 0x400u;
    const std::size_t colours = colour_base_ * 0x400u;
    for (unsigned i = 0; i < 1024; ++i) {
        fast_[screen + i] = (i > 256 && i < 512) ? static_cast<std::uint8_t>(i) : 0x20;
        fast_[colours + i] = static_cast<std::uint8_t>(i & 0x0F);
    }
    std::fill_n(fast_.begin() + static_cast<std::ptrdiff_t>(colours), 16, std::uint8_t{0x0F});
    text_colour_ = 0x0F;
    std::fill_n(fast_.begin() + static_cast<std::ptrdiff_t>(screen), 32 * screen_height_ / 8, std::uint8_t{0x20});
    std::fill_n(fast_.begin() + static_cast<std::ptrdiff_t>(colours), 32 * screen_height_ / 8, text_colour_);
    text_x_ = text_y_ = 0;
}

void Card256::overlay_power_on() {
    overlay_state_ = OverlayState::Sync;
    sync_vsyncs_ = 0;
    restart_ = false;
    last_display_ = last_vsync_ = false;
    paper_px_ = 0;
    shown_.fill(kClear);
    next_.fill(kClear);
    held_ = kClear;
    bg_mask_.fill(0);
    irq_ = false;
    pen_colour_ = 0;
    load_pictures();
}

void Card256::video_character(bool display, bool vsync, std::uint8_t /*mode*/, std::uint8_t* px) {
    if (last_vsync_ && !vsync) {
        vsync_end();
    }
    last_vsync_ = vsync;
    if (display && !last_display_) {
        paper_start();
    }
    last_display_ = display;
    if (!display) {
        return;
    }
    if (px) {
        for (unsigned k = 0; k < 4; ++k) {
            // Every overlay pixel in every mode: dropping every other one in 16-colour mode broke the Wolf256 title's
            // checkerboard dither into black lines, as on the 2dfx card ⚑ (to check on the real card)
            const unsigned q = paper_px_ + k;
            const std::uint8_t v = q < 256 ? shown_[q] : held_;
            if (v != kClear) {
                px[2 * k] = px[2 * k + 1] = to_igrb(v);
            }
        }
    }
    paper_px_ += 4;
}

// [FW] draw_screen_cycle: a start (power-on, reset) syncs on one VSYNC and draws from the end of the next; then every VSYNC end starts a frame
void Card256::vsync_end() {
    if (overlay_state_ == OverlayState::Sync) {
        if (++sync_vsyncs_ < 2) {
            return;
        }
    } else if (overlay_state_ != OverlayState::WaitVsync) {
        return;  // the line loop only counts BORDER edges
    }
    overlay_state_ = OverlayState::Frame;
    line_ = kLinesAbove;
    compose_line(next_);
    held_ = next_[0];
}

// The PIO starts the queued line; the core counts it and builds the one after ([FW] tvc-graphics.c:651-667)
void Card256::paper_start() {
    paper_px_ = 0;
    if (overlay_state_ != OverlayState::Frame) {
        shown_.fill(held_);  // no data queued: the pins keep their last value
        return;
    }
    shown_ = next_;
    ++line_;
    if (line_ < max_line_) {
        compose_line(next_);
        held_ = next_[0];
        return;
    }
    held_ = shown_[255];
    if ((sp_sp_collision_ & sp_sp_mask_) != 0 || (sp_bg_collision_ & sp_bg_mask_) != 0) {
        irq_ = true;
    }
    if (restart_) {
        restart_ = false;
        for (Sprite& s : sprites_) {
            s.enabled = 0;
        }
        rebuild_sprite_words();
        overlay_state_ = OverlayState::Sync;
        sync_vsyncs_ = 0;
    } else {
        overlay_state_ = OverlayState::WaitVsync;
    }
}

// [FW] calculate_one_line: back sprites, the picture, front sprites, extended border; quirks kept (design/cards.md "Overlay")
void Card256::compose_line(std::array<std::uint8_t, 256>& out) {
    const std::uint16_t y = line_;
    const auto y_use = static_cast<std::uint16_t>(y - (scroll_y_ & 7));
    if ((scroll_y_ & 0x80) && (y < kLinesAbove + 8 || y > max_line_ - 8)) {
        out.fill(border_ & 0x0F);
        return;
    }
    const unsigned start_x = kLeft + (scroll_x_ & 7);
    const std::uint32_t bitmap = (bitmap_base_ & 3u) * 0x8000;
    const std::uint32_t screen = (screen_base_ & 0x7Fu) * 0x400;
    const std::uint32_t colours = (colour_base_ & 0x7Fu) * 0x400;
    const std::uint32_t font = (font_base_ & 0x3Fu) * 0x800;

    std::array<std::uint8_t, kLineBuffer> buf;
    buf.fill(kClear);
    std::fill_n(bg_mask_.begin() + kLeft, 7, std::uint8_t{0});
    std::array<std::uint16_t, kLineBuffer> hits{};
    std::uint16_t sum = 0;

    draw_sprites(buf, hits, sum, false);
    const int rel = static_cast<int>(y_use) - kLinesAbove;
    const auto row = static_cast<std::uint32_t>(rel / 8);
    const auto mid = static_cast<std::uint8_t>(rel % 8);  // negative under a vertical scroll: the next glyph rows
    switch (video_mode_) {
    case 1:
        for (unsigned c = 0; c < 32; ++c) {
            const std::uint32_t at = screen + row * 32 + c;
            const std::uint8_t glyph = fast_get(font + fast_get(at) * 8u + mid);
            const std::uint8_t colour = fast_get(colours + row * 32 + c) & 0x0F;
            const unsigned p = start_x + 8 * c;
            for (unsigned b = 0; b < 8; ++b) {
                if (glyph & (0x80 >> b)) {
                    buf[p + b] = colour;
                }
            }
            // The firmware stores the mask's 32-bit halves swapped: pixels 4-7 first
            for (unsigned b = 0; b < 4; ++b) {
                bg_mask_[p + b] = (glyph & (0x08 >> b)) ? 0xFF : 0x00;
                bg_mask_[p + 4 + b] = (glyph & (0x80 >> b)) ? 0xFF : 0x00;
            }
        }
        break;
    case 2: {
        const std::uint32_t font16 = font & 0x1E000;
        for (unsigned c = 0; c < 32; ++c) {
            const std::uint32_t src = font16 + fast_get(screen + row * 32 + c) * 32u + mid * 4u;
            for (unsigned k = 0; k < 4; ++k) {
                const std::uint8_t b = fast_get(src + k);
                const unsigned p = start_x + 8 * c + 2 * k;
                const unsigned hi = b >> 4;
                const unsigned lo = b & 0x0F;
                if (hi != kTransparent) {
                    buf[p] = static_cast<std::uint8_t>(hi);
                }
                if (lo != kTransparent) {
                    buf[p + 1] = static_cast<std::uint8_t>(lo);
                }
                bg_mask_[p] = hi != kTransparent ? 0xFF : 0x00;
                bg_mask_[p + 1] = lo != kTransparent ? 0xFF : 0x00;
            }
        }
        break;
    }
    case 3:
        if (y_use >= kLinesAbove) {
            const std::uint32_t src = bitmap + static_cast<std::uint32_t>(y_use - kLinesAbove) * 128;
            for (unsigned k = 0; k < 128; ++k) {
                const std::uint8_t b = fast_get(src + k);
                const unsigned p = start_x + 2 * k;
                const unsigned hi = b >> 4;
                const unsigned lo = b & 0x0F;
                if (hi != kTransparent) {
                    buf[p] = static_cast<std::uint8_t>(hi);
                }
                if (lo != kTransparent) {
                    buf[p + 1] = static_cast<std::uint8_t>(lo);
                }
                // The mask gets the colours, so black never collides and transparent always does
                bg_mask_[p] = static_cast<std::uint8_t>(hi != kTransparent ? hi : 0x10);
                bg_mask_[p + 1] = static_cast<std::uint8_t>(lo != kTransparent ? lo : 0x10);
            }
        }
        break;
    default:
        sp_bg_collision_ = 0;
        break;
    }
    draw_sprites(buf, hits, sum, true);

    if (scroll_x_ & 0x80) {
        for (unsigned i = 0; i < 8; ++i) {
            buf[kLeft + i] = buf[kLeft + 248 + i] = border_ & 0x0F;
            bg_mask_[kLeft + i] = bg_mask_[kLeft + 248 + i] = 0;
        }
    }
    sp_sp_collision_ |= sum;
    if (video_mode_ != 0) {
        sp_bg_collision_ |= background_hits();
    }
    std::copy_n(buf.begin() + kLeft, 256, out.begin());
}

// [FW] calc_one_sprite_line with mark_collision_2c_w / mark_collision_16c; 16-colour sprites mark only their left pixels
void Card256::draw_sprites(std::array<std::uint8_t, kLineBuffer>& buf, std::array<std::uint16_t, kLineBuffer>& hits, std::uint16_t& sum,
                           bool front) const {
    const std::uint32_t base = (sprite_base_ & 7u) * 0x4000;
    for (unsigned i = 0; i < sprites_.size(); ++i) {
        const Sprite& s = sprites_[i];
        if (!s.enabled || (s.foreground != 0) != front) {
            continue;
        }
        const int row = static_cast<int>(line_) - static_cast<int>(s.y);
        if (row < 0 || row > 20 || s.x >= 256 + kLeft) {
            continue;
        }
        const auto bit = static_cast<std::uint16_t>(1u << i);
        if (!s.full_colour) {
            const std::uint32_t data = base + s.phase * 64u + static_cast<std::uint32_t>(row) * 3;
            for (unsigned k = 0; k < 3; ++k) {
                const std::uint8_t d = fast_get(data + k);
                const unsigned p = s.x + 8 * k;
                for (unsigned b = 0; b < 8; ++b) {
                    if (d & (0x80 >> b)) {
                        buf[p + b] = s.colour & 0x0F;
                    }
                }
                for (unsigned n = 0; n < 2; ++n) {
                    const unsigned group = p + 4 * n;
                    const unsigned nibble = n == 0 ? d >> 4 : d & 0x0F;
                    std::uint16_t before = 0;
                    for (unsigned j = 0; j < 4; ++j) {
                        if (nibble & (0x08 >> j)) {
                            before |= hits[group + j];
                        }
                    }
                    if (nibble != 0 && before != 0) {
                        sum |= static_cast<std::uint16_t>(bit | before);
                    }
                    for (unsigned j = 0; j < 4; ++j) {
                        if (nibble & (0x08 >> j)) {
                            hits[group + j] |= bit;
                        }
                    }
                }
            }
        } else {
            const std::uint32_t data = base + s.phase * 256u + static_cast<std::uint32_t>(row) * 12;
            for (unsigned k = 0; k < 12; ++k) {
                const std::uint8_t b = fast_get(data + k);
                const unsigned p = s.x + 2 * k;
                const bool left = (b >> 4) != kTransparent;
                const bool right = (b & 0x0F) != kTransparent;
                if (left) {
                    buf[p] = b >> 4;
                }
                if (right) {
                    buf[p + 1] = b & 0x0F;
                }
                if (left) {
                    const auto before = static_cast<std::uint16_t>(hits[p] | (right ? hits[p + 1] : 0));
                    if (before != 0) {
                        sum |= static_cast<std::uint16_t>(bit | before);
                    }
                    hits[p] |= bit;
                }
            }
        }
    }
}

// [FW] build_background_sprite_collision_bitvector: every enabled sprite on the line against the picture's mask
std::uint16_t Card256::background_hits() const {
    const std::uint32_t base = (sprite_base_ & 7u) * 0x4000;
    const auto mask = [&](unsigned p) -> std::uint8_t { return p < kLineBuffer ? bg_mask_[p] : 0; };  // past the buffer: firmware memory ❓
    std::uint16_t hits = 0;
    for (unsigned i = 0; i < sprites_.size(); ++i) {
        const Sprite& s = sprites_[i];
        if (!s.enabled || s.y > line_ || s.y + 20 < line_) {
            continue;
        }
        const unsigned row = line_ - s.y;
        bool hit = false;
        if (s.full_colour) {
            const std::uint32_t data = base + s.phase * 256u + row * 12;
            for (unsigned k = 0; k < 12 && !hit; ++k) {
                const std::uint8_t b = fast_get(data + k);
                const unsigned p = s.x + 2 * k;
                hit = ((b >> 4) != kTransparent && mask(p) != 0) || ((b & 0x0F) != kTransparent && mask(p + 1) != 0);
            }
        } else {
            const std::uint32_t data = base + s.phase * 64u + row * 3;
            for (unsigned k = 0; k < 3 && !hit; ++k) {
                const std::uint8_t d = fast_get(data + k);
                for (unsigned b = 0; b < 8 && !hit; ++b) {
                    hit = (d & (0x80 >> b)) && mask(s.x + 8 * k + b) != 0;
                }
            }
        }
        if (hit) {
            hits |= static_cast<std::uint16_t>(1u << i);
        }
    }
    return hits;
}

}  // namespace freetvc
