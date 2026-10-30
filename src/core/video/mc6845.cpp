// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/video/mc6845.hpp"

namespace freetvc {

namespace {

// Writable bits per register (MC6845 data sheet); R16/R17 (light pen) are read-only.
constexpr std::array<std::uint8_t, 16> kWriteMask{0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0x1F, 0x7F, 0x7F,
                                                  0x03, 0x1F, 0x7F, 0x1F, 0x3F, 0xFF, 0x3F, 0xFF};

}  // namespace

void Mc6845::power_on() {
    regs_.fill(0);
    address_ = 0;
    reset_counters();
}

void Mc6845::reset_counters() {
    h_ = 0;
    row_ = 0;
    ra_ = 0;
    in_adjust_ = false;
    adjust_lines_ = 0;
    ma_ = 0;
    row_start_ = 0;
    next_row_start_ = 0;
    hde_ = true;
    vde_ = true;
    hsync_left_ = 0;
    vsync_left_ = 0;
    field_ = 0;
}

void Mc6845::write(std::uint8_t value) {
    if (address_ < kWriteMask.size()) {
        regs_[address_] = value & kWriteMask[address_];
    }
}

std::uint8_t Mc6845::read() const { return address_ >= 14 && address_ < kRegisters ? regs_[address_] : 0x00; }

Mc6845::Output Mc6845::step() {
    Output out;
    out.ma = ma_;
    out.ra = static_cast<std::uint8_t>(ra_);
    out.display = hde_ && vde_;
    out.hsync = hsync_left_ > 0;
    out.vsync = vsync_left_ > 0;
    out.cursor = out.display && ma_ == cursor_address() && cursor_visible();

    if (hsync_left_ > 0) {
        --hsync_left_;
    }
    if (h_ == regs_[0]) {
        h_ = 0;
        hde_ = true;
        end_of_line();
    } else {
        ++h_;
        ma_ = (ma_ + 1) & 0x3FFF;
    }
    if (h_ == regs_[1]) {
        hde_ = false;
        // The next row starts where this one's displayed part ends, latched on its last raster
        if (!in_adjust_ && ra_ == (regs_[9] & 0x1Fu)) {
            next_row_start_ = ma_;
        }
    }
    if (h_ == regs_[2] && (regs_[3] & 0x0F) != 0) {
        hsync_left_ = regs_[3] & 0x0Fu;
    }
    return out;
}

void Mc6845::end_of_line() {
    if (vsync_left_ > 0) {
        --vsync_left_;
    }
    if (in_adjust_) {
        ra_ = (ra_ + 1) & 0x1F;
        if (++adjust_lines_ >= (regs_[5] & 0x1Fu)) {
            start_frame();
        }
    } else if (ra_ == (regs_[9] & 0x1Fu)) {
        ra_ = 0;
        row_start_ = next_row_start_;
        if (row_ == (regs_[4] & 0x7Fu)) {
            if ((regs_[5] & 0x1F) != 0) {
                in_adjust_ = true;
                adjust_lines_ = 0;
            } else {
                start_frame();
            }
        } else {
            row_ = (row_ + 1) & 0x7F;
            check_row_start();
        }
    } else {
        ra_ = (ra_ + 1) & 0x1F;
    }
    ma_ = row_start_;
}

void Mc6845::start_frame() {
    in_adjust_ = false;
    row_ = 0;
    ra_ = 0;
    row_start_ = start_address();
    next_row_start_ = row_start_;
    ++field_;
    vde_ = true;
    check_row_start();
}

void Mc6845::check_row_start() {
    if (row_ == (regs_[6] & 0x7Fu)) {
        vde_ = false;
    }
    if (row_ == (regs_[7] & 0x7Fu)) {
        vsync_left_ = kVsyncLines;
    }
}

bool Mc6845::cursor_visible() const {
    const unsigned mode = (regs_[10] >> 5) & 3;
    if (mode == 1) {
        return false;
    }
    if (ra_ < (regs_[10] & 0x1Fu) || ra_ > (regs_[11] & 0x1Fu)) {
        return false;
    }
    // Blink modes 10/11: on for half of a 16- or 32-field period
    return mode == 0 || (field_ & (mode == 2 ? 8u : 16u)) == 0;
}

}  // namespace freetvc
