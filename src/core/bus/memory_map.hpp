// SPDX-License-Identifier: GPL-3.0-or-later
// On-board memory, the page decoder (port 02h, port 03h b7-6, the 64k+ video paging register; tvc-hardware.md §3)
// and the cards in the four slots, whose IOMEM it decodes.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

#include "core/bus/multicart.hpp"
#include "core/bus/sddual.hpp"
#include "core/cards/card.hpp"
#include "core/machine/config.hpp"

namespace freetvc {

class ChunkWriter;
class ChunkReader;

// Physical memory a CPU address can reach; CARDn is C000-DFFF of the card in slot n (IOMEM).
enum class Bank : std::uint8_t { Sys, Cart, Ext, U0, U1, U2, U3, Vid0, Vid1, Vid2, Vid3, Card0, Card1, Card2, Card3, Open };

inline constexpr std::size_t kBankCount = 16;

// "SYS", "CART", "EXT", "U0"-"U3", "VID0"-"VID3", "CARD0"-"CARD3", "OPEN".
const char* bank_name(Bank bank);

struct BankAddress {
    Bank bank = Bank::Open;
    std::uint16_t offset = 0;  // from the start of the bank (EXT starts at E000, CARDn at C000)
};

class MemoryMap {
public:
    static constexpr std::size_t kPageSize = 0x4000;
    static constexpr std::size_t kVideoBanks = 4;
    static constexpr std::uint8_t kOpenBus = 0xFF;  // unmapped reads (YATE/EP; tvc-hardware.md §3)

    MemoryMap();
    // Copies rebuild the page table, which points into the copy's own arrays. A copy has no MultiCart flash or SD card
    // images attached; an assignment keeps the ones it had.
    MemoryMap(const MemoryMap& other);
    MemoryMap& operator=(const MemoryMap& other);

    // Copies the ROM images and sets the board; follow with reset().
    void configure(Model model, const RomImages& roms);
    // New cards in the slots, powered on.
    void configure_cards(const std::array<CardConfig, kCardSlots>& cards);
    CardSlots& cards() { return cards_; }
    const CardSlots& cards() const { return cards_; }
    // After a card changed what it serves in place of U2/U3
    void cards_changed() { remap(); }
    VideoOverlay* overlay() const { return overlay_; }
    // Reset values of port 02h (SYS, U1, VID, CART), port 03h and the video paging register; memory is kept.
    void reset();
    void clear_ram();

    // Debugger and loader access: no side effects; ROM and cartridge writes are ignored except in a MultiCart's RAM.
    std::uint8_t read(std::uint16_t address) const;
    void write(std::uint16_t address, std::uint8_t value);
    // CPU bus cycles at machine `tick`: as read()/write(), plus the cartridge's registers.
    std::uint8_t cpu_read(std::uint16_t address, std::uint64_t tick = 0) {
        const Page& page = pages_[address >> 14];
        if (page.kind == Kind::Cart) {
            if (multicart_) {
                return multicart_->read(static_cast<std::uint16_t>(address & 0x3FFF), tick, cart_flash_);
            }
            if (sddual_) {
                return sddual_->read(static_cast<std::uint16_t>(address & 0x3FFF), tick, sd_images_);
            }
            if (cart_type_ == CartType::KiloCart && (address & 0x3FFF) >= 0x3FFC) {
                select_cart_page((address & 0x3FFF) - 0x3FFC);
            }
        }
        return read(address);
    }
    void cpu_write(std::uint16_t address, std::uint8_t value, std::uint64_t tick = 0) {
        const Page& page = pages_[address >> 14];
        if (page.kind == Kind::Cart) {
            if (multicart_) {
                multicart_->write(static_cast<std::uint16_t>(address & 0x3FFF), value, tick, cart_flash_);
            } else if (sddual_) {
                sddual_->write(static_cast<std::uint16_t>(address & 0x3FFF), value, tick, sd_images_);
            } else if (cart_type_ == CartType::MegaCart && (address & 0x3FFF) >= 0x3C00) {
                select_cart_page(value & 0x3F);
            }
            return;
        }
        write(address, value);
    }
    // True where VID is paged in: CPU memory cycles there are clock-stretched (tvc-hardware.md §2).
    bool is_video(std::uint16_t address) const { return pages_[address >> 14].video; }

    void set_page_register(std::uint8_t value);  // port 02h
    void set_iomem_slot(unsigned slot);          // port 03h b7-6
    void set_video_paging(std::uint8_t value);   // ports 0Ch-0Fh; ignored except on the 64k+

    std::uint8_t page_register() const { return page_register_; }
    std::uint8_t video_paging() const { return video_paging_; }
    unsigned iomem_slot() const { return iomem_slot_; }
    unsigned displayed_bank() const { return (video_paging_ >> 4) & 3; }
    std::span<const std::uint8_t, kPageSize> video_bank(unsigned bank) const {
        return std::span<const std::uint8_t, kPageSize>(vram_.data() + (bank & 3) * kPageSize, kPageSize);
    }

    // RAM unit 0-3 as the CPU reaches it: a card's page in place of U2/U3, empty where the board has none
    std::span<std::uint8_t> unit_bytes(unsigned unit);
    std::span<std::uint8_t> ram() { return ram_; }  // the board's U0-U3 back to back
    std::span<const std::uint8_t> ram() const { return ram_; }
    std::span<std::uint8_t> vram() { return vram_; }  // banks 0-3 back to back
    std::span<const std::uint8_t> vram() const { return vram_; }

    // Snapshot fields: the paging latches and the cartridge page; RAM and VRAM go through ram() and vram().
    void save(ChunkWriter& w) const;
    void load(ChunkReader& r);

    // Cartridge page register; power-on sets 0, the reset button keeps it (the cart has no reset line ❓).
    void reset_cart() {
        if (multicart_) {
            multicart_->power_on();
        }
        if (sddual_) {
            sddual_->power_on();
        }
        select_cart_page(cart_type_ == CartType::Plain ? cart_bank_ : 0);
    }
    void select_cart_page(unsigned page);
    unsigned cart_page() const { return cart_page_; }
    unsigned cart_pages() const { return static_cast<unsigned>(cart_.size() / kPageSize); }
    CartType cart_type() const { return cart_type_; }
    std::span<const std::uint8_t> cart_image() const { return cart_; }
    // The MultiCart's board when one is in, and the flash the front end attached to it (outside the machine state)
    MultiCart* multicart() { return multicart_ ? &*multicart_ : nullptr; }
    const MultiCart* multicart() const { return multicart_ ? &*multicart_ : nullptr; }
    void attach_cart_flash(MultiCartFlash* flash) { cart_flash_ = flash; }
    MultiCartFlash* cart_flash() const { return cart_flash_; }
    // The SD Dual's board when one is in, and the SD card images in its sockets (outside the machine state). A new
    // image, or none, is a card going in or out: the board sees the change.
    SdDual* sddual() { return sddual_ ? &*sddual_ : nullptr; }
    const SdDual* sddual() const { return sddual_ ? &*sddual_ : nullptr; }
    void set_sd_image(unsigned socket, SdCardImage* image);
    SdCardImage* sd_image(unsigned socket) const { return sd_images_[socket & 1]; }

    // What the CPU reaches at `address` with the current paging.
    BankAddress locate(std::uint16_t address) const;
    // The bank's bytes (CARDn: the card's 8 KiB at C000-DFFF); empty where the board or slot has no such memory.
    std::span<std::uint8_t> bank_bytes(Bank bank);
    std::span<const std::uint8_t> bank_bytes(Bank bank) const;

private:
    enum class Kind : std::uint8_t { Open, Rom, Ram, Iomem, Cart };
    struct Page {
        Kind kind = Kind::Open;
        const std::uint8_t* read = nullptr;
        std::uint8_t* write = nullptr;
        bool video = false;
        Bank bank = Bank::Open;
    };

    Page rom(const std::uint8_t* image, Bank bank) const;
    Page cart() const;
    const std::uint8_t* cart_window() const;
    Page ram_unit(unsigned unit);
    Page video(unsigned bank);
    void remap();

    Model model_ = Model::Tvc64kPlus;
    std::uint8_t page_register_ = 0;
    std::uint8_t video_paging_ = 0;
    unsigned iomem_slot_ = 0;
    std::array<Page, 4> pages_{};
    VideoOverlay* overlay_ = nullptr;  // the cards' overlay, refreshed with the page table
    std::array<std::uint8_t, 4 * kPageSize> ram_{};
    std::array<std::uint8_t, kVideoBanks * kPageSize> vram_{};
    std::array<std::uint8_t, kPageSize> sys_{};
    std::vector<std::uint8_t> cart_;  // whole 16 KiB pages; empty = no cartridge
    CartType cart_type_ = CartType::Plain;
    unsigned cart_bank_ = 0;
    unsigned cart_page_ = 0;
    std::optional<MultiCart> multicart_;
    MultiCartFlash* cart_flash_ = nullptr;  // not copied (design/cards.md "MultiCart v2")
    std::optional<SdDual> sddual_;
    SdImages sd_images_{};  // not copied (design/cards.md "SD Dual")
    std::array<std::uint8_t, 0x2000> ext_{};  // E000-FFFF
    CardSlots cards_;
};

}  // namespace freetvc
