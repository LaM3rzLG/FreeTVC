// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/bus/sddual.hpp"

#include <algorithm>

#include "core/snapshot/snapshot.hpp"

namespace freetvc {

namespace {

constexpr std::uint8_t kVersion = 1;

}  // namespace

SdDual::SdDual(std::span<const std::uint8_t> firmware, bool writable)
    : writable_(writable), flash_(kFlashSize, 0xFF), ram_(kRamSize, 0) {
    std::copy_n(firmware.begin(), std::min(firmware.size(), flash_.size()), flash_.begin());
    power_on();
}

void SdDual::power_on() {
    // No reset line reaches the cartridge port; every flash page starts by selecting page 0, so none is needed
    page_ = 0;
    control_ = 0;
    fast_ = false;
    changed_ = true;
    miso_ = 0xFF;
    std::fill(ram_.begin(), ram_.end(), std::uint8_t{0});
    chip_.power_on();
    for (SdCard& card : cards_) {
        card.power_on();
    }
}

void SdDual::card_changed(unsigned socket) {
    cards_[socket & 1].power_on();
    changed_ = true;
}

std::uint8_t SdDual::status(const SdImages& images) const {
    std::uint8_t s = 0;
    // Only the SD socket has a write-protect switch; the firmware checks it for socket 1 alone
    if (!images[1] || images[1]->read_only()) {
        s |= kStatusProtect;
    }
    if (!images[0] && !images[1]) {
        s |= kStatusNoCard;
    }
    if (changed_) {
        s |= kStatusChanged;
    }
    return s;  // b4-0 ❓ (read as 0)
}

void SdDual::transfer(std::uint8_t mosi, const SdImages& images) {
    // MISO floats high; a selected card drives it
    std::uint8_t in = 0xFF;
    for (unsigned s = 0; s < cards_.size(); ++s) {
        if (control_ & (s == 0 ? kSelect0 : kSelect1)) {
            in &= cards_[s].transfer(mosi, images[s]);
        }
    }
    miso_ = in;
}

std::uint8_t SdDual::io_read(std::uint16_t offset, const SdImages& images) {
    if (fast_) {
        // Fast mode: any register read gives the byte in and clocks the next one with MOSI high
        const std::uint8_t value = miso_;
        transfer(0xFF, images);
        return value;
    }
    switch (offset & 3) {
    case 0:
        return miso_;
    case 1:
        return status(images);
    default:
        return 0x00;  // +2 and +3 are write-only ❓
    }
}

void SdDual::io_write(std::uint16_t offset, std::uint8_t value, const SdImages& images) {
    switch (offset & 3) {
    case 0:
        transfer(value, images);
        break;
    case 1:
        for (unsigned s = 0; s < cards_.size(); ++s) {
            const std::uint8_t bit = s == 0 ? kSelect0 : kSelect1;
            if ((value & bit) && !(control_ & bit)) {
                cards_[s].select();
            }
        }
        control_ = value;
        if (value & kClearChanged) {
            changed_ = false;
        }
        break;
    case 2:
        page_ = value;
        break;
    default:
        fast_ = (value & kFast) != 0;
        break;
    }
}

std::uint8_t SdDual::read(std::uint16_t offset, std::uint64_t tick, const SdImages& images) {
    offset &= 0x3FFF;
    if (!(offset & 0x2000)) {
        return chip_.read(static_cast<std::uint32_t>(page() * kPage + offset), tick, flash_);
    }
    if (offset < kIo) {
        return ram_[offset & (kRamSize - 1)];
    }
    return io_read(offset, images);
}

void SdDual::write(std::uint16_t offset, std::uint8_t value, std::uint64_t tick, const SdImages& images) {
    offset &= 0x3FFF;
    if (!(offset & 0x2000)) {
        // Without the jumper the flash's WE# stays high
        if (writable_) {
            chip_.write(static_cast<std::uint32_t>(page() * kPage + offset), value, tick, flash_);
        }
    } else if (offset < kIo) {
        ram_[offset & (kRamSize - 1)] = value;
    } else {
        io_write(offset, value, images);
    }
}

std::uint8_t SdDual::peek(std::uint16_t offset) const {
    offset &= 0x3FFF;
    if (!(offset & 0x2000)) {
        return flash_[page() * kPage + offset];
    }
    return offset < kIo ? ram_[offset & (kRamSize - 1)] : 0xFF;
}

void SdDual::poke(std::uint16_t offset, std::uint8_t value) {
    offset &= 0x3FFF;
    if ((offset & 0x2000) && offset < kIo) {
        ram_[offset & (kRamSize - 1)] = value;
    }
}

void SdDual::save(ChunkWriter& w) const {
    w.u8(kVersion);
    w.boolean(writable_);
    w.packed(flash_);
    w.packed(ram_);
    chip_.save(w);
    w.u8(page_);
    w.u8(control_);
    w.boolean(fast_);
    w.boolean(changed_);
    w.u8(miso_);
    for (const SdCard& card : cards_) {
        card.save(w);
    }
}

void SdDual::load(ChunkReader& r) {
    if (r.u8() != kVersion) {
        r.fail();
        return;
    }
    writable_ = r.boolean();
    r.packed(flash_);
    r.packed(ram_);
    chip_.load(r);
    page_ = r.u8();
    control_ = r.u8();
    fast_ = r.boolean();
    changed_ = r.boolean();
    miso_ = r.u8();
    for (SdCard& card : cards_) {
        card.load(r);
    }
}

}  // namespace freetvc
