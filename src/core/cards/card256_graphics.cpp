// SPDX-License-Identifier: GPL-3.0-or-later
// TVC256k++ text and drawing functions of registers C0h/C6h (design/cards.md "Overlay", [FW] tvc-routines.c).
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "core/cards/card256.hpp"

namespace freetvc {

namespace {

constexpr std::uint32_t kPsramFlag = 0x800000;
constexpr unsigned kTransparent = 8;
constexpr unsigned kFloodStack = 1024;  // [FW] Point stack[1024]: a fuller stack ends the fill with error 2

// The MCU's 32-bit arithmetic, wrapping where C on the Cortex-M33 does
std::int32_t wrap32(std::int64_t v) { return static_cast<std::int32_t>(static_cast<std::uint32_t>(v)); }

// [FW] tvc-routines.c accents[]: TVC codes 90h-98h (small) and 80h-88h (capital) as screen codes
constexpr std::uint8_t kAccents[18] = {0xF0, 0xFC, 0xE1, 0xF6, 0xF4, 0xE5, 0xF5, 0xEE, 0xFB,
                                       0xEB, 0xF1, 0xE2, 0xF9, 0xF7, 0xE3, 0xF8, 0xF3, 0xF2};

}  // namespace

std::int16_t Card256::param_s16(std::uint32_t at) const { return static_cast<std::int16_t>(param16(at)); }

// [FW] setFunctionBitmapBaseAddr: C7h, or A4h while C7h is FFh
void Card256::select_draw_bitmap() {
    draw_base_ = (function_bitmap_base_ == 0xFF ? (bitmap_base_ & 3u) : (function_bitmap_base_ & 3u)) * 0x8000;
}

// [FW] set_dotc_impl / get_dot_color_impl take the coordinates as bytes
void Card256::set_dot(std::uint8_t x, std::uint8_t y, std::uint8_t colour) {
    const std::uint32_t at = draw_base_ + y * 128u + (x >> 1);
    const std::uint8_t b = fast_get(at);
    fast_put(at, (x & 1) ? static_cast<std::uint8_t>((b & 0xF0) | colour) : static_cast<std::uint8_t>((b & 0x0F) | (colour << 4)));
}

std::uint8_t Card256::get_dot(std::uint8_t x, std::uint8_t y) const {
    const std::uint8_t b = fast_get(draw_base_ + y * 128u + (x >> 1));
    return (x & 1) ? b & 0x0F : b >> 4;
}

// [FW] draw_line_impl (Bresenham), clipped per pixel to 256 x the screen height
void Card256::draw_line(std::int16_t x1, std::int16_t y1, std::int16_t x2, std::int16_t y2, std::uint8_t colour) {
    int x = x1;
    int y = y1;
    const int dx = std::abs(x2 - x1);
    const int dy = std::abs(y2 - y1);
    const int sx = x1 < x2 ? 1 : -1;
    const int sy = y1 < y2 ? 1 : -1;
    int err = dx - dy;
    for (;;) {
        if (x >= 0 && x < 256 && y >= 0 && y < screen_height_) {
            set_dot(static_cast<std::uint8_t>(x), static_cast<std::uint8_t>(y), colour);
        }
        if (x == x2 && y == y2) {
            break;
        }
        const int err2 = err * 2;
        if (err2 > -dy) {
            err -= dy;
            x = static_cast<std::int16_t>(x + sx);
        }
        if (err2 < dx) {
            err += dx;
            y = static_cast<std::int16_t>(y + sy);
        }
    }
}

// [FW] draw_horizontal_line_impl: odd left and even right ends by pixel, the bytes between in one fill
void Card256::draw_hline(std::int16_t x1, std::int16_t x2, std::int16_t y, std::uint8_t colour) {
    if (x1 % 2) {
        set_dot(static_cast<std::uint8_t>(x1), static_cast<std::uint8_t>(y), colour);
        ++x1;
    }
    if (x2 % 2 == 0) {
        set_dot(static_cast<std::uint8_t>(x2), static_cast<std::uint8_t>(y), colour);
        --x2;
    }
    if (x2 > x1) {
        const auto at = static_cast<std::uint32_t>(static_cast<std::int32_t>(draw_base_) + y * 128 + (x1 >> 1));
        for (int i = 0; i < (x2 - x1 + 1) >> 1; ++i) {
            fast_put(at + static_cast<std::uint32_t>(i), static_cast<std::uint8_t>((colour << 4) + colour));
        }
    }
}

// [FW] draw_horizontal_line_2c_impl: the two colours' nibbles alternate along the line
void Card256::draw_hline_2c(std::int16_t x1, std::int16_t x2, std::int16_t y, std::uint8_t two) {
    if (x1 > x2) {
        std::swap(x1, x2);
    }
    if (x1 % 2) {
        set_dot(static_cast<std::uint8_t>(x1), static_cast<std::uint8_t>(y), two & 0x0F);
        ++x1;
    }
    if (x2 % 2 == 0) {
        set_dot(static_cast<std::uint8_t>(x2), static_cast<std::uint8_t>(y), two >> 4);
        --x2;
    }
    const auto at = static_cast<std::uint32_t>(static_cast<std::int32_t>(draw_base_) + y * 128 + (x1 >> 1));
    for (int i = 0; i < (x2 - x1 + 1) >> 1; ++i) {
        fast_put(at + static_cast<std::uint32_t>(i), two);
    }
}

// [FW] fix_rect_params: off-limits rectangles become 0,0,0,0 (which fill_box_2c still draws as one dot)
void Card256::fix_rect(std::uint32_t at, std::int16_t& x, std::int16_t& y, std::uint8_t& w, std::uint8_t& h) const {
    x = param_s16(at);
    y = param_s16(at + 2);
    w = fast_get(at + 4);
    h = fast_get(at + 5);
    if (x < -255 || y < -255 || x > 255 || y > screen_height_ || w == 0 || h == 0) {
        x = y = 0;
        w = h = 0;
        return;
    }
    if (x < 0) {
        w = static_cast<std::uint8_t>(w + x);
        x = 0;
    }
    if (x + w > 255) {
        w = static_cast<std::uint8_t>(255 - x);
    }
    if (y < 0) {
        h = static_cast<std::uint8_t>(h + y);
        y = 0;
    }
    if (y + h >= screen_height_) {
        h = static_cast<std::uint8_t>(screen_height_ - y - 1);
    }
}

// 1Ah: [FW] draw_rectangle
std::uint8_t Card256::draw_rectangle(std::uint32_t at) {
    const std::int16_t x = param_s16(at);
    const std::int16_t y = param_s16(at + 2);
    const std::uint8_t w = fast_get(at + 4);
    const std::uint8_t h = fast_get(at + 5);
    select_draw_bitmap();
    if (y >= 0 && y < screen_height_) {
        draw_hline(std::max<std::int16_t>(0, x), static_cast<std::int16_t>(std::min(x + w, 255)), y, pen_colour_);
    }
    draw_line(static_cast<std::int16_t>(x + w), y, static_cast<std::int16_t>(x + w), static_cast<std::int16_t>(y + h), pen_colour_);
    if (y + h >= 0 && y + h < screen_height_) {
        draw_hline(std::max<std::int16_t>(0, x), static_cast<std::int16_t>(std::min(x + w, 255)), static_cast<std::int16_t>(y + h), pen_colour_);
    }
    draw_line(x, static_cast<std::int16_t>(y + h), x, y, pen_colour_);
    return 0;
}

// 1Dh and 28h: [FW] scanline_flood_fill_impl; it also looks at (and may fill) the line under the screen height
std::uint8_t Card256::flood_fill(std::uint8_t start_x, std::uint8_t start_y, std::uint8_t colour, bool two_colours) {
    select_draw_bitmap();
    const std::uint8_t even = colour;
    const auto odd = static_cast<std::uint8_t>((colour << 4) | (colour >> 4));
    const std::uint8_t target = get_dot(start_x, start_y);
    if (target == (colour & 0x0F)) {
        return 0;
    }
    std::vector<std::pair<std::uint8_t, std::uint8_t>> stack{{start_x, start_y}};
    while (!stack.empty()) {
        const auto [px, py] = stack.back();
        stack.pop_back();
        const int y = py;
        int left = px;
        while (left >= 0 && get_dot(static_cast<std::uint8_t>(left), py) == target) {
            --left;
        }
        ++left;
        int right = px;
        while (right < 256 && get_dot(static_cast<std::uint8_t>(right), py) == target) {
            ++right;
        }
        --right;
        if (right >= left) {
            if (two_colours) {
                draw_hline_2c(static_cast<std::int16_t>(left), static_cast<std::int16_t>(right), static_cast<std::int16_t>(y), (y & 1) ? odd : even);
            } else {
                draw_hline(static_cast<std::int16_t>(left), static_cast<std::int16_t>(right), static_cast<std::int16_t>(y), colour);
            }
        }
        for (int ny : {y - 1, y + 1}) {
            if ((ny < y && y <= 0) || (ny > y && y >= screen_height_)) {
                continue;
            }
            bool in_segment = false;
            for (int i = left; i <= right; ++i) {
                if (get_dot(static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(ny)) == target) {
                    if (!in_segment) {
                        if (stack.size() >= kFloodStack) {
                            return 2;
                        }
                        stack.emplace_back(static_cast<std::uint8_t>(i), static_cast<std::uint8_t>(ny));
                        in_segment = true;
                    }
                } else {
                    in_segment = false;
                }
            }
        }
    }
    return 0;
}

// 1Eh and 1Fh: [FW] ellipse (midpoint, two phases), outline or filled by lines
std::uint8_t Card256::ellipse(std::uint32_t at, bool fill) {
    const std::int16_t xc = param_s16(at);
    const std::int16_t yc = param_s16(at + 2);
    const std::uint8_t a = fast_get(at + 4);
    const std::uint8_t b = fast_get(at + 5);
    select_draw_bitmap();
    std::uint8_t x = 0;
    std::int32_t y = b;
    const std::uint32_t a2 = static_cast<std::uint32_t>(a) * a;
    const std::uint32_t b2 = static_cast<std::uint32_t>(b) * b;
    const std::uint32_t two_a2 = 2 * a2;
    const std::uint32_t two_b2 = 2 * b2;
    auto dx = static_cast<std::int32_t>(two_b2 * x);
    auto dy = static_cast<std::int32_t>(two_a2 * static_cast<std::uint32_t>(y));

    const auto plot = [&](std::uint8_t px, std::uint8_t py) {
        if (fill) {
            if (xc - px > 256 || xc + px < 0 || yc - py > screen_height_ || yc + py < 0) {
                return;
            }
            const auto left = static_cast<std::int16_t>(xc - px > 0 ? xc - px : 0);
            const auto right = static_cast<std::int16_t>(xc + px < 256 ? xc + px : 255);
            const auto top = static_cast<std::int16_t>(yc >= py ? yc - py : 0);
            const auto bottom = static_cast<std::int16_t>(yc + py < screen_height_ ? yc + py : screen_height_ - 1);
            draw_hline(left, right, top, pen_colour_);
            draw_hline(left, right, bottom, pen_colour_);
            return;
        }
        for (const int xx : {xc + px, xc - px}) {
            for (const int yy : {yc + py, yc - py}) {
                if (xx >= 0 && xx < 256 && yy >= 0 && yy < screen_height_) {
                    set_dot(static_cast<std::uint8_t>(xx), static_cast<std::uint8_t>(yy), pen_colour_);
                }
            }
        }
    };

    auto p = wrap32(static_cast<std::int64_t>(b2) - static_cast<std::int64_t>(a2 * b) + a2 / 4);
    while (dx < dy) {
        plot(x, static_cast<std::uint8_t>(y));
        ++x;
        dx = wrap32(static_cast<std::int64_t>(dx) + two_b2);
        if (p < 0) {
            p = wrap32(static_cast<std::int64_t>(p) + b2 + dx);
        } else {
            --y;
            dy = wrap32(static_cast<std::int64_t>(dy) - two_a2);
            p = wrap32(static_cast<std::int64_t>(p) + b2 + dx - dy);
        }
    }
    // The firmware's phase-2 start in its unsigned 32-bit arithmetic
    p = static_cast<std::int32_t>(b2 * (static_cast<std::uint32_t>(x) * x + x) + b2 / 4 +
                                  a2 * static_cast<std::uint32_t>(y - 1) * static_cast<std::uint32_t>(y - 1) - a2 * b2);
    while (y >= 0) {
        plot(x, static_cast<std::uint8_t>(y));
        --y;
        dy = wrap32(static_cast<std::int64_t>(dy) - two_a2);
        if (p > 0) {
            p = wrap32(static_cast<std::int64_t>(p) + a2 - dy);
        } else {
            ++x;
            dx = wrap32(static_cast<std::int64_t>(dx) + two_b2);
            p = wrap32(static_cast<std::int64_t>(p) + a2 - dy + dx);
        }
    }
    return 0;
}

// 20h: [FW] copy_image_block_fast; colour 8 leaves the bitmap's pixel
std::uint8_t Card256::copy_image_block(std::uint32_t at) {
    const std::int16_t dest_x = param_s16(at);
    const std::int16_t dest_y = param_s16(at + 2);
    const std::uint8_t width = fast_get(at + 4);
    const std::uint8_t height = fast_get(at + 5);
    std::uint32_t source = param24(at + 6);
    if (width == 0 || height == 0) {
        return 0;
    }
    const int x0 = std::max<int>(0, dest_x);
    if (dest_x + width < 0 || dest_x > 255) {
        return 0;
    }
    int y = std::max<int>(0, dest_y);
    if (dest_y + height < 0 || dest_y >= screen_height_) {
        return 0;
    }
    const bool psram = (source & kPsramFlag) != 0;
    source &= kPsramFlag - 1;
    const auto src = [&](std::uint32_t a) { return psram ? psram_read(a) : fast_get(a); };
    // A byte's pixels where the source is not colour 8
    const auto put = [&](std::uint32_t t, std::uint8_t s) {
        std::uint8_t mask = 0xFF;
        if ((s & 0x0F) == kTransparent) {
            mask &= 0xF0;
        }
        if ((s & 0xF0) == kTransparent << 4) {
            mask &= 0x0F;
        }
        fast_put(t, static_cast<std::uint8_t>((fast_get(t) & ~mask) | (s & mask)));
    };
    select_draw_bitmap();
    const auto line_bytes = static_cast<std::uint8_t>(((width - 1) >> 1) + 1);
    const int skipped_lines = -std::min<int>(0, dest_y);
    const int skipped = -std::min<int>(0, dest_x);
    std::uint32_t s0 = source + static_cast<std::uint32_t>(skipped_lines) * line_bytes + static_cast<std::uint32_t>(skipped >> 1);
    std::uint32_t t0 = draw_base_ + static_cast<std::uint32_t>(y) * 128 + static_cast<std::uint32_t>(x0 >> 1);
    for (; y < dest_y + height && y < screen_height_; ++y, s0 += line_bytes, t0 += 128) {
        int x = x0;
        std::uint32_t s = s0;
        std::uint32_t t = t0;
        std::uint8_t data = 0;
        if ((x & 1) == (skipped & 1)) {
            if (x & 1) {
                put(t, static_cast<std::uint8_t>((src(s) & 0x0F) | 0x80));
                ++s;
                ++t;
                ++x;
            }
            for (; x < dest_x + width && x < 256; x += 2, ++s, ++t) {
                put(t, src(s));
            }
        } else {
            if (x & 1) {
                data = static_cast<std::uint8_t>((src(s) >> 4) | 0x80);
            }
            for (; x < 256 && x < dest_x + width; ++x) {
                if (x & 1) {
                    put(t, data);
                    ++t;
                } else {
                    data = static_cast<std::uint8_t>(src(s) << 4);
                    ++s;
                    data = static_cast<std::uint8_t>(data | (src(s) >> 4));
                }
            }
            if (x & 1) {
                put(t, static_cast<std::uint8_t>((data & 0xF0) | 0x08));
            }
        }
    }
    return 0;
}

// 21h: [FW] copy_sub_image; outside the source the default colour, colour 8 skipped when b7-4 of it are set
std::uint8_t Card256::copy_sub_image(std::uint32_t at) {
    const std::int16_t dst_x = param_s16(at);
    const std::int16_t dst_y = param_s16(at + 2);
    const std::uint8_t d_width = fast_get(at + 4);
    const std::uint8_t d_height = fast_get(at + 5);
    const std::int16_t src_x = param_s16(at + 6);
    const std::int16_t src_y = param_s16(at + 8);
    const std::uint16_t s_width = param16(at + 10);
    const std::uint16_t s_height = param16(at + 12);
    std::uint32_t source = param24(at + 14);
    const std::uint8_t def_colour = fast_get(at + 17) & 0x0F;
    const bool skip_clear = (fast_get(at + 17) & 0xF0) != 0;
    if (s_width == 0 || s_height == 0 || d_width == 0 || d_height == 0) {
        return 0;
    }
    select_draw_bitmap();
    const bool psram = (source & kPsramFlag) != 0;
    source &= kPsramFlag - 1;
    const auto line_bytes = static_cast<std::uint8_t>(((s_width - 1) >> 1) + 1);  // a byte in the firmware too
    int sy = src_y;
    std::uint32_t offset = 0;
    for (int dy = 0; dy < d_height; ++dy, ++sy) {
        if (dst_y + dy < 0) {
            continue;
        }
        if (dst_y + dy >= screen_height_) {
            break;
        }
        int sx = src_x;
        if (sy >= 0 && sy < s_height) {
            offset = source + static_cast<std::uint32_t>(sy) * line_bytes;
        }
        for (int dx = 0; dx < d_width; ++dx, ++sx) {
            if (dst_x + dx < 0) {
                continue;
            }
            if (dst_x + dx > 255) {
                break;
            }
            std::uint8_t colour = def_colour;
            if (sy >= 0 && sy < s_height && sx >= 0 && sx < s_width) {
                const std::uint32_t a = offset + static_cast<std::uint32_t>(sx >> 1);
                const std::uint8_t b = psram ? psram_read(a) : fast_get(a);
                colour = (sx & 1) ? b & 0x0F : b >> 4;
            }
            if (!(skip_clear && colour == kTransparent)) {
                set_dot(static_cast<std::uint8_t>(dst_x + dx), static_cast<std::uint8_t>(dst_y + dy), colour);
            }
        }
    }
    return 0;
}

// 26h: [FW] draw_filled_triangle; counter-clockwise only, checkerboard fill, edges in the pen colour unless it is 8
std::uint8_t Card256::fill_triangle(std::uint32_t at) {
    std::int16_t x1 = param_s16(at);
    std::int16_t y1 = param_s16(at + 2);
    std::int16_t x2 = param_s16(at + 4);
    std::int16_t y2 = param_s16(at + 6);
    std::int16_t x3 = param_s16(at + 8);
    std::int16_t y3 = param_s16(at + 10);
    if (wrap32(static_cast<std::int64_t>(x2 - x1) * (y3 - y1) - static_cast<std::int64_t>(y2 - y1) * (x3 - x1)) > 0) {
        return 0;
    }
    const std::uint8_t two = fast_get(at + 12);
    select_draw_bitmap();
    if (y1 > y2) {
        std::swap(y1, y2);
        std::swap(x1, x2);
    }
    if (y1 > y3) {
        std::swap(y1, y3);
        std::swap(x1, x3);
    }
    if (y2 > y3) {
        std::swap(y2, y3);
        std::swap(x2, x3);
    }
    if (y3 < 0 || y1 >= 240 || y1 == y3) {
        return 0;
    }
    const auto clamp_x = [](std::int32_t x) { return static_cast<std::int16_t>(x < 0 ? 0 : x >= 256 ? 255 : x); };
    const std::int32_t total = y3 - y1;
    const std::int32_t dx_total = wrap32(static_cast<std::int64_t>(x3 - x1) * 65536) / total;
    const auto even = two;
    const auto odd = static_cast<std::uint8_t>((two >> 4) | (two << 4));
    std::int32_t segment = y2 - y1;
    if (segment > 0 && y1 < 240 && y2 > 0) {
        const std::int32_t dx_short = wrap32(static_cast<std::int64_t>(x2 - x1) * 65536) / segment;
        std::int32_t x_long = wrap32(static_cast<std::int64_t>(x1) * 65536);
        std::int32_t x_short = x_long;
        int start = y1;
        int end = y2;
        if (start < 0) {
            x_long = wrap32(x_long + static_cast<std::int64_t>(dx_total) * -start);
            x_short = wrap32(x_short + static_cast<std::int64_t>(dx_short) * -start);
            start = 0;
        }
        end = std::min(end, 240);
        for (int y = start; y < end; ++y) {
            draw_hline_2c(clamp_x(x_long >> 16), clamp_x(x_short >> 16), static_cast<std::int16_t>(y), y % 2 == 0 ? even : odd);
            x_long = wrap32(static_cast<std::int64_t>(x_long) + dx_total);
            x_short = wrap32(static_cast<std::int64_t>(x_short) + dx_short);
        }
    }
    segment = y3 - y2;
    if (segment > 0 && y2 < 240 && y3 > 0) {
        const std::int32_t dx_short = wrap32(static_cast<std::int64_t>(x3 - x2) * 65536) / segment;
        std::int32_t x_long = wrap32(static_cast<std::int64_t>(x1) * 65536 + static_cast<std::int64_t>(dx_total) * (y2 - y1));
        std::int32_t x_short = wrap32(static_cast<std::int64_t>(x2) * 65536);
        int start = y2;
        int end = y3;
        if (start < 0) {
            x_long = wrap32(x_long + static_cast<std::int64_t>(dx_total) * -start);
            x_short = wrap32(x_short + static_cast<std::int64_t>(dx_short) * -start);
            start = 0;
        }
        if (end >= 240) {
            end = 239;
        }
        for (int y = start; y <= end; ++y) {
            draw_hline_2c(clamp_x(x_long >> 16), clamp_x(x_short >> 16), static_cast<std::int16_t>(y), y % 2 == 0 ? even : odd);
            x_long = wrap32(static_cast<std::int64_t>(x_long) + dx_total);
            x_short = wrap32(static_cast<std::int64_t>(x_short) + dx_short);
        }
    }
    if (pen_colour_ != kTransparent) {
        draw_line(x1, y1, x2, y2, pen_colour_);
        draw_line(x2, y2, x3, y3, pen_colour_);
        draw_line(x3, y3, x1, y1, pen_colour_);
    }
    return 0;
}

// Text: [FW] print_char_scr_code and scrollScreenUp use A1h/A2h masked to 3Fh
void Card256::scroll_text_up() {
    const std::uint32_t screen = (screen_base_ & 0x3Fu) * 0x400;
    const std::uint32_t colours = (colour_base_ & 0x3Fu) * 0x400;
    const std::uint32_t rows = screen_height_ / 8;
    fast_move(screen, screen + 32, 32 * (rows - 1));
    fast_move(colours, colours + 32, 32 * (rows - 1));
    for (unsigned i = 0; i < 32; ++i) {
        fast_put(screen + 32 * (rows - 1) + i, 0x20);
        fast_put(colours + 32 * (rows - 1) + i, text_colour_);
    }
}

void Card256::print_code(std::uint8_t code) {
    const std::uint32_t cell = text_y_ * 32u + text_x_;
    fast_put((screen_base_ & 0x3Fu) * 0x400 + cell, code);
    fast_put((colour_base_ & 0x3Fu) * 0x400 + cell, text_colour_);
    if (++text_x_ >= 32) {
        text_x_ = 0;
        if (++text_y_ >= screen_height_ / 8) {
            scroll_text_up();
            --text_y_;
        }
    }
}

void Card256::print_hex_digit(unsigned digit) {
    print_code(static_cast<std::uint8_t>(digit < 10 ? '0' + digit : 1 + (digit - 10)));  // screen code 1 is 'A'
}

// 06h: [FW] print_string_ascii; control codes move the cursor, F0h-FFh set the colour
void Card256::print_ascii(std::uint32_t at) {
    for (std::uint8_t c; (c = fast_get(at)) != 0; ++at) {
        print_ascii_char(c);
    }
}

void Card256::print_ascii_text(const std::string& text) {
    for (const char c : text) {
        if (c == '\0') {
            break;
        }
        print_ascii_char(static_cast<std::uint8_t>(c));
    }
}

void Card256::print_ascii_char(std::uint8_t c) {
    {
        const unsigned rows = screen_height_ / 8;
        if (c >= 'A' && c <= ']') {
            print_code(static_cast<std::uint8_t>(c - 'A' + 1));
        } else if (c >= 'a' && c <= 'z') {
            print_code(static_cast<std::uint8_t>(c - 'a' + 0x81));
        } else if (c >= 32 && c <= 64) {
            print_code(c);
        } else if (c == '_') {
            print_code(0x52);
        } else if (c == '^') {
            print_code(0x1E);
        } else if (c == '~') {
            print_code(0x1F);
        } else if (c == 0x0A) {
            if (++text_y_ >= rows) {
                scroll_text_up();
                --text_y_;
            }
        } else if (c == 0x0D) {
            text_x_ = 0;
        } else if (c == 0x04) {
            if (++text_x_ > 31) {
                text_x_ = 0;
                if (++text_y_ >= rows) {
                    --text_y_;
                    text_x_ = 31;
                }
            }
        } else if (c == 0x13) {
            if (--text_x_ == 0xFF) {
                text_x_ = 31;
                if (--text_y_ == 0xFF) {
                    text_y_ = 0;
                    text_x_ = 0;
                }
            }
        } else if (c == 0x05) {
            if (--text_y_ == 0xFF) {
                text_y_ = 0;
            }
        } else if (c == 0x18) {
            if (++text_y_ >= rows) {
                --text_y_;
            }
        } else if (c >= 0x90 && c <= 0x98) {
            print_code(kAccents[c - 0x90]);
        } else if (c >= 0x80 && c <= 0x88) {
            print_code(kAccents[c - 0x80 + 9]);
        } else if (c >= 0xA0 && c < 0xE0) {
            print_code(static_cast<std::uint8_t>(c - 0xA0 + 0x40));
        } else if (c >= 0xF0) {
            text_colour_ = c - 0xF0;
        } else {
            print_code('?');
        }
    }
}

// The text and drawing functions; nullopt for any other code
std::optional<std::uint8_t> Card256::call_graphics(std::uint8_t code, std::uint32_t at) {
    const std::uint32_t rows = screen_height_ / 8;
    switch (code) {
    case 0x00:  // clear_text_screen: A1h/A2h unmasked here
        for (std::uint32_t i = 0; i < 32 * rows; ++i) {
            fast_put(screen_base_ * 0x400u + i, 0x20);
            fast_put(colour_base_ * 0x400u + i, text_colour_);
        }
        text_x_ = text_y_ = 0;
        return 0;
    case 0x01:  // clear_bitmap_screen: all colour 8
        select_draw_bitmap();
        for (std::uint32_t i = 0; i < 128u * screen_height_; ++i) {
            fast_put(draw_base_ + i, 0x88);
        }
        return 0;
    case 0x02:
        print_code(fast_get(at));
        return 0;
    case 0x03:
        print_hex_digit(fast_get(at) >> 4);
        print_hex_digit(fast_get(at) & 0x0F);
        return 0;
    case 0x04:
        for (int shift = 12; shift >= 0; shift -= 4) {
            print_hex_digit((param16(at) >> shift) & 0x0F);
        }
        return 0;
    case 0x05:
        for (std::uint8_t c; (c = fast_get(at)) != 0; ++at) {
            print_code(c);
        }
        return 0;
    case 0x06:
        print_ascii(at);
        return 0;
    case 0x07:
        fast_put(at, text_colour_);
        return 0;
    case 0x08:
        text_colour_ = fast_get(at) & 0x0F;
        return 0;
    case 0x09:
        fast_put(at, text_x_);
        fast_put(at + 1, text_y_);
        return 0;
    case 0x0A:
        if (const std::uint8_t x = fast_get(at); x != 0xFF) {
            text_x_ = std::min<std::uint8_t>(x, 31);
        }
        if (const std::uint8_t y = fast_get(at + 1); y != 0xFF) {
            text_y_ = std::min<std::uint8_t>(y, 29);
        }
        return 0;
    case 0x10: {  // replace_pixel_color, nibble by nibble over a fast-RAM range
        const std::uint32_t start = param24(at);
        const std::uint32_t length = param24(at + 3);
        const std::uint8_t from = fast_get(at + 6) & 0x0F;
        const std::uint8_t to = fast_get(at + 7) & 0x0F;
        if (start + length > 256 * 1024) {
            return 1;
        }
        for (std::uint32_t i = 0; i < length; ++i) {
            std::uint8_t b = fast_get(start + i);
            if ((b & 0x0F) == from) {
                b = static_cast<std::uint8_t>((b & 0xF0) | to);
            }
            if ((b >> 4) == from) {
                b = static_cast<std::uint8_t>((b & 0x0F) | (to << 4));
            }
            fast_put(start + i, b);
        }
        return 0;
    }
    case 0x15:
        fast_put(at, pen_colour_);
        return 0;
    case 0x16:
        pen_colour_ = fast_get(at) & 0x0F;
        return 0;
    case 0x17:
    case 0x18: {
        const std::uint8_t x = fast_get(at);
        const std::uint8_t y = fast_get(at + 1);
        if (y >= screen_height_) {
            return 1;
        }
        select_draw_bitmap();
        if (code == 0x17) {
            fast_put(at, get_dot(x, y));
        } else {
            set_dot(x, y, pen_colour_);
        }
        return 0;
    }
    case 0x19:
        select_draw_bitmap();
        draw_line(param_s16(at), param_s16(at + 2), param_s16(at + 4), param_s16(at + 6), pen_colour_);
        return 0;
    case 0x1A:
        return draw_rectangle(at);
    case 0x1B:
    case 0x27: {  // fill_rectangle (its last line left out) and fill_box_2c (with it, plus the outline)
        std::int16_t x = 0;
        std::int16_t y = 0;
        std::uint8_t w = 0;
        std::uint8_t h = 0;
        fix_rect(at, x, y, w, h);
        select_draw_bitmap();
        if (code == 0x1B) {
            for (std::uint8_t i = static_cast<std::uint8_t>(y); i < y + h; ++i) {
                draw_hline(x, static_cast<std::int16_t>(x + w), i, pen_colour_);
            }
            return 0;
        }
        const std::uint8_t two = fast_get(at + 6);
        const auto odd = static_cast<std::uint8_t>((two >> 4) | (two << 4));
        for (std::uint8_t i = static_cast<std::uint8_t>(y); i <= y + h; ++i) {
            draw_hline_2c(x, static_cast<std::int16_t>(x + w), i, i % 2 == 0 ? two : odd);
        }
        if (pen_colour_ != kTransparent) {
            draw_rectangle(at);
        }
        return 0;
    }
    case 0x1C: {
        const std::uint8_t points = fast_get(at);
        if (points < 2) {
            return 1;
        }
        select_draw_bitmap();
        for (unsigned i = 0; i + 1 < points; ++i) {
            const std::uint32_t p = at + 1 + i * 4;
            draw_line(param_s16(p), param_s16(p + 2), param_s16(p + 4), param_s16(p + 6), pen_colour_);
        }
        return 0;
    }
    case 0x1D:
    case 0x28:
        if (fast_get(at + 1) >= screen_height_) {
            return 1;
        }
        return code == 0x1D ? flood_fill(fast_get(at), fast_get(at + 1), pen_colour_, false)
                            : flood_fill(fast_get(at), fast_get(at + 1), fast_get(at + 2), true);
    case 0x1E:
    case 0x1F:
        return ellipse(at, code == 0x1F);
    case 0x20:
        return copy_image_block(at);
    case 0x21:
        return copy_sub_image(at);
    case 0x26:
        return fill_triangle(at);
    default:
        return std::nullopt;
    }
}

}  // namespace freetvc
