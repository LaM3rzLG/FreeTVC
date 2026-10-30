// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/bus/memory_map.hpp"

#include <algorithm>
#include <vector>

namespace freetvc {

namespace {

template <std::size_t N>
void copy_image(std::array<std::uint8_t, N>& dst, const std::vector<std::uint8_t>& src, std::size_t offset) {
    dst.fill(MemoryMap::kOpenBus);
    const std::size_t count = std::min(src.size(), N - std::min(offset, N));
    std::copy_n(src.begin(), count, dst.begin() + static_cast<std::ptrdiff_t>(offset));
}

constexpr std::array<const char*, kBankCount> kBankNames{
    "SYS", "CART", "EXT", "U0", "U1", "U2", "U3", "VID0", "VID1", "VID2", "VID3", "CARD0", "CARD1", "CARD2", "CARD3", "OPEN"};

}  // namespace

const char* bank_name(Bank bank) { return kBankNames[static_cast<std::size_t>(bank) % kBankCount]; }

MemoryMap::MemoryMap() {
    configure(Model::Tvc64kPlus, {});
    reset();
}

MemoryMap::MemoryMap(const MemoryMap& other)
    : model_(other.model_),
      page_register_(other.page_register_),
      video_paging_(other.video_paging_),
      iomem_slot_(other.iomem_slot_),
      ram_(other.ram_),
      vram_(other.vram_),
      sys_(other.sys_),
      cart_(other.cart_),
      cart_type_(other.cart_type_),
      cart_bank_(other.cart_bank_),
      cart_page_(other.cart_page_),
      multicart_(other.multicart_),
      sddual_(other.sddual_),
      ext_(other.ext_),
      cards_(other.cards_) {
    remap();
}

MemoryMap& MemoryMap::operator=(const MemoryMap& other) {
    if (this != &other) {
        model_ = other.model_;
        page_register_ = other.page_register_;
        video_paging_ = other.video_paging_;
        iomem_slot_ = other.iomem_slot_;
        ram_ = other.ram_;
        vram_ = other.vram_;
        sys_ = other.sys_;
        cart_ = other.cart_;
        cart_type_ = other.cart_type_;
        cart_bank_ = other.cart_bank_;
        cart_page_ = other.cart_page_;
        multicart_ = other.multicart_;
        sddual_ = other.sddual_;
        ext_ = other.ext_;
        cards_ = other.cards_;
        remap();
    }
    return *this;
}

BankAddress MemoryMap::locate(std::uint16_t address) const {
    const Page& page = pages_[address >> 14];
    if (page.kind == Kind::Iomem) {
        if (address >= 0xE000) {
            return {Bank::Ext, static_cast<std::uint16_t>(address - 0xE000)};
        }
        return {static_cast<Bank>(static_cast<unsigned>(Bank::Card0) + iomem_slot_), static_cast<std::uint16_t>(address - 0xC000)};
    }
    return {page.bank, static_cast<std::uint16_t>(address & (kPageSize - 1))};
}

std::span<std::uint8_t> MemoryMap::bank_bytes(Bank bank) {
    const auto index = static_cast<unsigned>(bank);
    switch (bank) {
    case Bank::Sys:
        return sys_;
    case Bank::Cart:
        if (multicart_) {
            return multicart_->window(cart_flash_);
        }
        if (sddual_) {
            return sddual_->window();
        }
        if (cart_window() == nullptr) {
            return {};
        }
        return std::span<std::uint8_t>(cart_).subspan(cart_page_ * kPageSize, kPageSize);
    case Bank::Ext:
        return ext_;
    case Bank::U0:
    case Bank::U1:
    case Bank::U2:
    case Bank::U3:
        return unit_bytes(index - static_cast<unsigned>(Bank::U0));
    case Bank::Vid0:
    case Bank::Vid1:
    case Bank::Vid2:
    case Bank::Vid3: {
        const unsigned vid = index - static_cast<unsigned>(Bank::Vid0);
        if (vid > 0 && model_ != Model::Tvc64kPlus) {
            return {};
        }
        return std::span<std::uint8_t>(vram_).subspan(vid * kPageSize, kPageSize);
    }
    case Bank::Card0:
    case Bank::Card1:
    case Bank::Card2:
    case Bank::Card3: {
        Card* card = cards_.card(index - static_cast<unsigned>(Bank::Card0));
        return card ? card->window() : std::span<std::uint8_t>{};
    }
    default:
        return {};  // OPEN has no bytes
    }
}

std::span<const std::uint8_t> MemoryMap::bank_bytes(Bank bank) const { return const_cast<MemoryMap*>(this)->bank_bytes(bank); }

void MemoryMap::configure(Model model, const RomImages& roms) {
    model_ = model;
    copy_image(sys_, roms.sys, 0);
    cart_type_ = roms.cart_type;
    cart_bank_ = roms.cart_bank;
    // whole 16 KiB pages, FFh past the image's end (empty EPROM sockets on HBR read FFh)
    cart_.assign((roms.cart.size() + kPageSize - 1) / kPageSize * kPageSize, kOpenBus);
    std::copy(roms.cart.begin(), roms.cart.end(), cart_.begin());
    cart_page_ = cart_type_ == CartType::Plain ? cart_bank_ : 0;
    if (cart_type_ == CartType::MultiCart) {
        multicart_.emplace();
    } else {
        multicart_.reset();
    }
    // The SD Dual's firmware lives in its flash; the plain page table never sees it
    if (cart_type_ == CartType::SdDual) {
        sddual_.emplace(roms.cart, (cart_bank_ & 1) != 0);
        cart_.clear();
    } else {
        sddual_.reset();
    }
    // EXT 1.x is a 4 KiB ROM in the upper half of the 8 KiB socket; E000-EFFF reads FFh (rom.md §1)
    copy_image(ext_, roms.ext, roms.ext.size() <= 0x1000 ? 0x1000 : 0);
    remap();
}

void MemoryMap::configure_cards(const std::array<CardConfig, kCardSlots>& cards) {
    cards_.configure(cards);
    remap();
}

void MemoryMap::reset() {
    page_register_ = 0;
    video_paging_ = 0;
    iomem_slot_ = 0;
    remap();
}

void MemoryMap::clear_ram() {
    ram_.fill(0);
    vram_.fill(0);
}

std::uint8_t MemoryMap::read(std::uint16_t address) const {
    const Page& page = pages_[address >> 14];
    if (page.kind == Kind::Iomem) {
        // C000-DFFF belongs to the card in the selected slot (an empty slot is open bus); E000-FFFF is the EXT ROM
        return address >= 0xE000 ? ext_[address - 0xE000] : cards_.mem_read(iomem_slot_, static_cast<std::uint16_t>(address - 0xC000));
    }
    if (page.read) {
        return page.read[address & (kPageSize - 1)];
    }
    if (page.kind == Kind::Cart && sddual_) {
        return sddual_->peek(address & (kPageSize - 1));
    }
    return page.kind == Kind::Cart && multicart_ ? multicart_->peek(address & (kPageSize - 1), cart_flash_) : kOpenBus;
}

void MemoryMap::write(std::uint16_t address, std::uint8_t value) {
    const Page& page = pages_[address >> 14];
    if (page.kind == Kind::Iomem && address < 0xE000) {
        cards_.mem_write(iomem_slot_, static_cast<std::uint16_t>(address - 0xC000), value);
    } else if (page.write) {
        page.write[address & (kPageSize - 1)] = value;
    } else if (page.kind == Kind::Cart && multicart_) {
        multicart_->poke(address & (kPageSize - 1), value);
    } else if (page.kind == Kind::Cart && sddual_) {
        sddual_->poke(address & (kPageSize - 1), value);
    }
}

void MemoryMap::set_page_register(std::uint8_t value) {
    page_register_ = value;
    remap();
}

void MemoryMap::set_sd_image(unsigned socket, SdCardImage* image) {
    socket &= 1;
    if (sd_images_[socket] != image) {
        sd_images_[socket] = image;
        if (sddual_) {
            sddual_->card_changed(socket);
        }
    }
}

void MemoryMap::set_iomem_slot(unsigned slot) { iomem_slot_ = slot & 3; }

void MemoryMap::set_video_paging(std::uint8_t value) {
    if (model_ == Model::Tvc64kPlus) {
        video_paging_ = value & 0x3F;
        remap();
    }
}

MemoryMap::Page MemoryMap::rom(const std::uint8_t* image, Bank bank) const { return {Kind::Rom, image, nullptr, false, bank}; }

const std::uint8_t* MemoryMap::cart_window() const {
    // a page past the image's end reads FFh, like an unpopulated MegaCart page
    return cart_page_ < cart_pages() ? cart_.data() + cart_page_ * kPageSize : nullptr;
}

MemoryMap::Page MemoryMap::cart() const { return {Kind::Cart, cart_window(), nullptr, false, Bank::Cart}; }

void MemoryMap::select_cart_page(unsigned page) {
    cart_page_ = page;
    remap();
}

std::span<std::uint8_t> MemoryMap::unit_bytes(unsigned unit) {
    // A card asserting /PHANTOM answers in place of U2/U3 (design/cards.md "TVC256k++")
    if (unit >= 2 && unit < 4) {
        if (std::uint8_t* page = cards_.phantom_page(unit)) {
            return {page, kPageSize};
        }
    }
    // A bare 32k board has only U0 and U1; U2/U3 come with the HBM card (tvc-hardware.md §1)
    if (unit >= 4 || (unit >= 2 && model_ == Model::Tvc32k)) {
        return {};
    }
    return std::span<std::uint8_t>(ram_).subspan(unit * kPageSize, kPageSize);
}

MemoryMap::Page MemoryMap::ram_unit(unsigned unit) {
    const std::span<std::uint8_t> bytes = unit_bytes(unit);
    if (bytes.empty()) {
        return {};
    }
    return {Kind::Ram, bytes.data(), bytes.data(), false, static_cast<Bank>(static_cast<unsigned>(Bank::U0) + unit)};
}

MemoryMap::Page MemoryMap::video(unsigned bank) {
    std::uint8_t* base = vram_.data() + (bank & 3) * kPageSize;
    return {Kind::Ram, base, base, true, static_cast<Bank>(static_cast<unsigned>(Bank::Vid0) + (bank & 3))};
}

void MemoryMap::remap() {
    const bool plus = model_ == Model::Tvc64kPlus;
    const std::uint8_t p = page_register_;
    overlay_ = cards_.overlay();

    switch ((p >> 3) & 3) {
    case 0:
        pages_[0] = rom(sys_.data(), Bank::Sys);
        break;
    case 1:
        pages_[0] = cart();
        break;
    case 2:
        pages_[0] = ram_unit(0);
        break;
    default:
        // U3 on the 64k+ and with the HBM card; the unmodified 64k board decodes 11 as U0 (tvc-hardware.md §1, §3)
        pages_[0] = ram_unit(model_ == Model::Tvc64k ? 0 : 3);
        break;
    }

    // Page 1 can hold VID only on the 64k+ (02h b2); the bank comes from video paging b1-0
    pages_[1] = plus && (p & 0x04) ? video(video_paging_ & 3) : ram_unit(1);

    // Page 2: VID (bank from video paging b3-2 on the 64k+) or U2
    pages_[2] = (p & 0x20) ? ram_unit(2) : video(plus ? (video_paging_ >> 2) & 3 : 0);

    switch (p >> 6) {
    case 0:
        pages_[3] = cart();
        break;
    case 1:
        pages_[3] = rom(sys_.data(), Bank::Sys);
        break;
    case 2:
        pages_[3] = ram_unit(3);
        break;
    default:
        pages_[3] = {Kind::Iomem, nullptr, nullptr, false};
        break;
    }
}

}  // namespace freetvc
