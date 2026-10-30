// SPDX-License-Identifier: GPL-3.0-or-later
// Sebestyén Pál's SD Dual cartridge (design/cards.md "SD Dual"): a Xilinx CPLD, a flash chip seen in 8 KiB pages
// at C000-DFFF, an 8 KiB SRAM at E000-FBFF, the CPLD's registers at FC00-FFFF and two SD sockets on one SPI bus
// (socket 0 microSD, socket 1 SD with a write-protect switch). The cards' images are media outside the machine state.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "core/bus/jedec_flash.hpp"
#include "core/bus/sd_card.hpp"

namespace freetvc {

class ChunkWriter;
class ChunkReader;

// The two sockets' images, served by the front end; null = an empty socket
using SdImages = std::array<SdCardImage*, 2>;

// The flash chip: AMD-style commands at AAAh/555h (a x16 part in byte mode); the firmware updater erases the whole
// 64 KiB with one sector erase ❓ the part, its sectors, ID and times (typical values of that family)
inline constexpr FlashSpec kSdDualFlash{0x10000, 0x10000, 0xAAA, 0x555, 0xFFF, 0xFF, 0xFF, 9, 700'000, 700'000};

class SdDual {
public:
    static constexpr std::size_t kFlashSize = kSdDualFlash.size;
    static constexpr std::size_t kPage = 0x2000;
    static constexpr std::size_t kRamSize = 0x2000;
    static constexpr std::uint16_t kIo = 0x3C00;  // window offset of the registers
    // Status register (read +1): set = write protected (socket 1), no card (both empty), card changed
    static constexpr std::uint8_t kStatusProtect = 0x80;
    static constexpr std::uint8_t kStatusNoCard = 0x40;
    static constexpr std::uint8_t kStatusChanged = 0x20;
    // Control register (write +1): chip selects, and clear the change latch
    static constexpr std::uint8_t kSelect0 = 0x80;
    static constexpr std::uint8_t kSelect1 = 0x40;
    static constexpr std::uint8_t kClearChanged = 0x20;
    static constexpr std::uint8_t kFast = 0x80;  // write +3: every register read clocks the next byte

    // The flash holds `firmware` with FFh past it; `writable` is the flash write jumper
    SdDual(std::span<const std::uint8_t> firmware, bool writable);
    void power_on();
    // CPU cycles at `offset` in the 16 KiB window (A13-A0), at machine `tick`
    std::uint8_t read(std::uint16_t offset, std::uint64_t tick, const SdImages& images);
    void write(std::uint16_t offset, std::uint8_t value, std::uint64_t tick, const SdImages& images);
    // Debugger access: no side effects; writes reach the RAM only
    std::uint8_t peek(std::uint16_t offset) const;
    void poke(std::uint16_t offset, std::uint8_t value);
    // The flash page at C000-DFFF
    std::span<std::uint8_t> window() { return std::span<std::uint8_t>(flash_).subspan(page() * kPage, kPage); }
    // A socket's image came or went: that card powers up from nothing and the change latch is set
    void card_changed(unsigned socket);

    unsigned page() const { return page_ >> 5; }
    std::uint8_t page_register() const { return page_; }
    std::uint8_t control() const { return control_; }
    bool fast() const { return fast_; }
    bool changed() const { return changed_; }
    bool writable() const { return writable_; }
    std::uint8_t status(const SdImages& images) const;
    const SdCard& card(unsigned socket) const { return cards_[socket & 1]; }
    std::span<const std::uint8_t> flash() const { return flash_; }
    std::span<const std::uint8_t> ram() const { return ram_; }
    void save(ChunkWriter& w) const;
    void load(ChunkReader& r);

private:
    // One byte out on MOSI to the selected cards; the CPLD latches what comes back on MISO
    void transfer(std::uint8_t mosi, const SdImages& images);
    std::uint8_t io_read(std::uint16_t offset, const SdImages& images);
    void io_write(std::uint16_t offset, std::uint8_t value, const SdImages& images);

    bool writable_;
    std::vector<std::uint8_t> flash_;
    std::vector<std::uint8_t> ram_;
    JedecFlash chip_{kSdDualFlash};
    std::uint8_t page_ = 0;     // +2: b7-5 the flash page, the rest ignored
    std::uint8_t control_ = 0;  // +1 as last written
    bool fast_ = false;
    bool changed_ = true;
    std::uint8_t miso_ = 0xFF;  // the byte the last transfer brought in
    std::array<SdCard, 2> cards_{};
};

}  // namespace freetvc
