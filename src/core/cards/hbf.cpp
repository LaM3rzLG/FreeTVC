// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/cards/hbf.hpp"

#include <algorithm>

#include "core/snapshot/snapshot.hpp"

namespace freetvc {

HbfCard::HbfCard(std::span<const std::uint8_t> rom) {
    rom_.fill(0xFF);
    std::copy_n(rom.begin(), std::min(rom.size(), rom_.size()), rom_.begin());
    power_on();
}

void HbfCard::power_on() {
    std::fill(window_.begin() + kPageSize, window_.end(), 0);
    reset(0);
}

void HbfCard::reset(std::uint64_t) {
    // The reset line clears the page register and the drive latch ❓ (no schematic reading yet), and resets the FDC
    select_page(0);
    latch_ = 0;
    fdc_.reset();
    fdc_.select(-1, 0, false, 0, nullptr);
}

void HbfCard::select_page(unsigned page) {
    page_ = page & 3;
    std::copy_n(rom_.begin() + static_cast<std::ptrdiff_t>(page_ * kPageSize), kPageSize, window_.begin());
}

// Ports (A3-A2): +0-3 FDC, +4-7 drive latch / DRQ and INTRQ, +8-11 EPROM page, +12-15 nothing
std::uint8_t HbfCard::io_read(unsigned port, const CardBus& bus) {
    switch ((port >> 2) & 3) {
    case 0:
        return fdc_.read(port & 3, bus.tick, bus.drives);
    case 1: {
        // The card ROM's transfer loops need every other bit 0 (design/cards.md "HBF")
        std::uint8_t v = fdc_.drq(bus.tick, bus.drives) ? 0x80 : 0x00;
        if (fdc_.intrq(bus.tick, bus.drives)) {
            v |= 0x01;
        }
        return v;
    }
    default:
        return 0xFF;
    }
}

void HbfCard::io_write(unsigned port, std::uint8_t value, const CardBus& bus) {
    switch ((port >> 2) & 3) {
    case 0:
        fdc_.write(port & 3, value, bus.tick, bus.drives);
        break;
    case 1: {
        // b3-0 drive select (the lowest set bit wins), b5 DDEN, b6 motor, b7 side
        latch_ = value;
        int drive = -1;
        for (int d = 0; d < 4; ++d) {
            if (value & (1u << d)) {
                drive = d;
                break;
            }
        }
        fdc_.select(drive, value >> 7, (value & 0x40) != 0, bus.tick, bus.drives);
        break;
    }
    case 2:
        select_page(value >> 4);
        break;
    default:
        break;
    }
}

void HbfCard::mem_write(std::uint16_t offset, std::uint8_t value) {
    offset &= 0x1FFF;
    if (offset >= kPageSize) {
        window_[offset] = value;
    }
}

void HbfCard::save(ChunkWriter& w) const {
    w.u8(static_cast<std::uint8_t>(page_));
    w.u8(latch_);
    w.packed(std::span<const std::uint8_t>(window_).subspan(kPageSize));
    fdc_.save(w);
}

void HbfCard::load(ChunkReader& r) {
    select_page(r.u8());
    latch_ = r.u8();
    r.packed(std::span<std::uint8_t>(window_).subspan(kPageSize));
    fdc_.load(r);
}

}  // namespace freetvc
