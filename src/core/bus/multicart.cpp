// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/bus/multicart.hpp"

#include "core/snapshot/snapshot.hpp"

namespace freetvc {

namespace {

constexpr std::uint8_t kVersion = 1;

}  // namespace

std::uint8_t MultiCart::pla(std::uint32_t in) {
    std::uint8_t out = 0xFF;
    // A1-A4 straight, A5-A12 through the 74HCT32 in pairs (all zero at C000-C003), A13 flipped by the relocation bit
    std::uint32_t address = in & 0x1FE;
    if (in & kInA13) {
        address |= 0x2000;
    }
    if (in & kInRelocated) {
        address ^= 0x2000;
    }
    address |= 0xC000;
    if (!(in & kInPowerOn)) {
        return static_cast<std::uint8_t>(out & ~kOutRegClr);
    }
    if (!(in & kInResetOnRead)) {
        out &= 0x7F;
    }
    if (in & 1) {
        return out;  // the PLA's A0, grounded on the board
    }
    // The cartridge port has no /WR: a cycle without /BRD is a write
    const bool read = !(in & kInBrd);
    const bool write = !read && !(in & kInWindow);
    if (address == 0xC000) {
        // tvcpla.bin clears on a read while RESENA is high; tvcpla.pas tests input bit 7 there instead ⚠
        if (read && (in & kInResetOnRead)) {
            out &= static_cast<std::uint8_t>(~kOutRegClr);
        }
        if (write) {
            return static_cast<std::uint8_t>((out & ~kOutRegWr) | 0x80);
        }
    }
    if (address == 0xC002 && write) {
        return static_cast<std::uint8_t>(out & ~kOutRegWr & 0x7F);
    }
    if (read) {
        out &= static_cast<std::uint8_t>(~kOutMemRd);
    }
    if (write) {
        out &= static_cast<std::uint8_t>(~kOutMemWr);
    }
    if (in & kInRam) {
        return static_cast<std::uint8_t>(out & ~kOutRamCe);
    }
    return static_cast<std::uint8_t>(out & ~((in & kInChip1) ? kOutRomCe1 : kOutRomCe0));
}

MultiCart::MultiCart() : ram_(kRamSize, 0) { power_on(); }

void MultiCart::power_on() {
    // /RST clears the 74HCT273 through D1 and the 74HCT74, so RESENA comes up high; the reset button reaches neither
    reg_ = 0;
    q_ = false;
    for (JedecFlash& chip : chips_) {
        chip.power_on();
    }
}

std::uint32_t MultiCart::inputs(std::uint16_t offset, bool read) const {
    const auto line = [&](unsigned bit) { return (offset >> bit) & 1u; };
    std::uint32_t in = offset & 0x1Eu;
    in |= (line(9) | line(10)) << 5;
    in |= (line(6) | line(11)) << 6;
    in |= (line(7) | line(8)) << 7;
    in |= (line(5) | line(12)) << 8;
    if (offset & 0x2000) {
        in |= kInA13;
    }
    if (!read) {
        in |= kInBrd;
    }
    if (reg_ & kRelocated) {
        in |= kInRelocated;
    }
    if (reg_ & kRam) {
        in |= kInRam;
    }
    if (reg_ & kChip1) {
        in |= kInChip1;
    }
    if (!q_) {
        in |= kInResetOnRead;
    }
    return in | kInPowerOn;  // the window input stays low: the outputs count only while it is open
}

std::uint32_t MultiCart::flash_address(std::uint16_t offset) const {
    return (static_cast<std::uint32_t>(reg_ & 0x1F) << 14) | (offset & 0x3FFFu);
}

std::size_t MultiCart::ram_address(std::uint16_t offset) const {
    // Q1-Q3 drive the 628128's A14-A16; Q4 goes to its NC pin and Q5 to the jumper, set to VCC for 128 KiB
    return (static_cast<std::size_t>(reg_ & 0x07) << 14) | (offset & 0x3FFFu);
}

std::uint8_t MultiCart::read(std::uint16_t offset, std::uint64_t tick, MultiCartFlash* flash) {
    std::uint8_t out = pla(inputs(offset, true));
    if (!(out & kOutRegClr)) {
        // The clear is asynchronous: the register, and the chip the PLA selects with it, change within the cycle
        reg_ = 0;
        out = pla(inputs(offset, true));
    }
    if (out & kOutMemRd) {
        return 0xFF;
    }
    if (!(out & kOutRamCe)) {
        return ram_[ram_address(offset)];
    }
    const unsigned chip = !(out & kOutRomCe0) ? 0 : !(out & kOutRomCe1) ? 1 : 2;
    if (chip > 1 || !flash || flash->bytes.size() < MultiCartFlash::kSize) {
        return 0xFF;
    }
    const auto bytes = std::span<const std::uint8_t>(flash->bytes).subspan(chip * kSst39sf040.size, kSst39sf040.size);
    return chips_[chip].read(flash_address(offset), tick, bytes);
}

void MultiCart::write(std::uint16_t offset, std::uint8_t value, std::uint64_t tick, MultiCartFlash* flash) {
    const std::uint8_t out = pla(inputs(offset, false));
    if (!(out & kOutRegWr)) {
        // /REGWR's rising edge clocks the 74HCT273 (D0-D7) and the 74HCT74 (D = A1)
        reg_ = value;
        q_ = (offset & 2) != 0;
        return;
    }
    if (out & kOutMemWr) {
        return;
    }
    if (!(out & kOutRamCe)) {
        ram_[ram_address(offset)] = value;
        return;
    }
    const unsigned chip = !(out & kOutRomCe0) ? 0 : !(out & kOutRomCe1) ? 1 : 2;
    if (chip > 1 || !flash || flash->bytes.size() < MultiCartFlash::kSize) {
        return;
    }
    const auto bytes = std::span<std::uint8_t>(flash->bytes).subspan(chip * kSst39sf040.size, kSst39sf040.size);
    if (chips_[chip].write(flash_address(offset), value, tick, bytes)) {
        flash->dirty = true;
        flash->last_write = tick;
    }
}

std::uint8_t MultiCart::peek(std::uint16_t offset, const MultiCartFlash* flash) const {
    if (reg_ & kRam) {
        return ram_[ram_address(offset)];
    }
    if (!flash || flash->bytes.size() < MultiCartFlash::kSize) {
        return 0xFF;
    }
    return flash->bytes[((reg_ & kChip1) ? kSst39sf040.size : 0) + flash_address(offset)];
}

void MultiCart::poke(std::uint16_t offset, std::uint8_t value) {
    if (reg_ & kRam) {
        ram_[ram_address(offset)] = value;
    }
}

std::span<std::uint8_t> MultiCart::window(MultiCartFlash* flash) {
    if (reg_ & kRam) {
        return std::span<std::uint8_t>(ram_).subspan(ram_address(0), 0x4000);
    }
    if (!flash || flash->bytes.size() < MultiCartFlash::kSize) {
        return {};
    }
    const std::size_t base = ((reg_ & kChip1) ? kSst39sf040.size : 0) + flash_address(0);
    return std::span<std::uint8_t>(flash->bytes).subspan(base, 0x4000);
}

void MultiCart::save(ChunkWriter& w) const {
    w.u8(kVersion);
    w.u8(reg_);
    w.boolean(q_);
    w.packed(ram_);
    for (const JedecFlash& chip : chips_) {
        chip.save(w);
    }
}

void MultiCart::load(ChunkReader& r) {
    if (r.u8() != kVersion) {
        r.fail();
        return;
    }
    reg_ = r.u8();
    q_ = r.boolean();
    r.packed(ram_);
    for (JedecFlash& chip : chips_) {
        chip.load(r);
    }
}

}  // namespace freetvc
