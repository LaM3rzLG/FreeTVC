// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once

#include <array>
#include <cstdint>
#include <vector>

namespace freetvc {

// Boards by RAM fit-out (tvc-hardware.md §1); the ROM set is chosen separately.
enum class Model {
    Tvc32k,      // HBA: U0, U1 only
    Tvc32kHbm,   // HBA + HBM 32 KiB card: U2, U3, and U3 can map to page 0
    Tvc64k,      // HBA-1: page-0 field 11 gives U0
    Tvc64kPlus,  // HBA-2: 4 video banks, VID in page 1, video paging ports 0Ch-0Fh
};

// Cartridge boards (design/media.md "Cartridges").
enum class CartType : std::uint8_t {
    Plain,     // HBR or a home-built EPROM board: one 16 KiB window, chosen by the board's switches (cart_bank)
    KiloCart,  // 64 KiB: a read at offset 3FFC-3FFF selects page (offset - 3FFC)
    MegaCart,  // 1 MiB: a write at offset 3C00-3FFF selects page (value & 3Fh)
    MultiCart, // Vass Sándor's MultiCart v2: flash and RAM behind a register; the flash is media, not in `cart`
    SdDual,    // Sebestyén Pál's SD Dual: `cart` is its firmware in flash, plus RAM, registers and two SD sockets
};

// ROM images as bytes: the core never reads files. Empty or short images read FFh where missing.
struct RomImages {
    std::vector<std::uint8_t> sys;   // 16 KiB
    std::vector<std::uint8_t> ext;   // 8 KiB (2.x) or 4 KiB (1.x, placed in the upper half, F000-FFFF)
    std::vector<std::uint8_t> cart;  // whole cartridge image (up to 1 MiB for a MegaCart); empty = no cartridge
    CartType cart_type = CartType::Plain;
    unsigned cart_bank = 0;  // Plain: the 16 KiB window of a larger image; SdDual: 1 = the flash write jumper is on
    bool has_cart() const { return !cart.empty() || cart_type == CartType::MultiCart; }
};

// Expansion cards (design/cards.md).
enum class CardKind : std::uint8_t {
    None,
    Hbf,       // HBF floppy interface: WD1793, 4 drives, 16 KiB EPROM in 4 KiB pages, 4 KiB RAM
    GameCard,  // dikdom's GameCard: SN76489A, joysticks 3/4, 64 KiB EPROM in 8 KiB pages
    Card256,   // dikdom's TVC256k++: 256 KiB fast RAM over U2/U3, 8 MiB PSRAM, 12 KiB ROM, registers
    Multichip, // Vass Sándor's Multichip Soundcard: SN76489, SAA1099, YM3812, 32 KiB EEPROM in 8 KiB banks
    NanoSd,    // Vass Sándor's NanoSD IO Card: an Arduino Nano serving an SD card, 64 KiB EEPROM in 8 KiB banks
    Serial,    // Videoton's serial line interface (HBS): an 8251A clocked by TXRXCLK, no ROM
};

struct CardConfig {
    CardKind kind = CardKind::None;
    std::vector<std::uint8_t> rom;  // the card's EPROM image; missing bytes read FFh
};

inline constexpr unsigned kCardSlots = 4;
// The slot WinTVC gives the disk card (design/cards.md "Slot model"); ports 10h-1Fh
inline constexpr unsigned kDiskCardSlot = 0;
// The GameCard's default slot; ports 20h-2Fh
inline constexpr unsigned kGameCardSlot = 1;
// The TVC256k++'s default slot, the manual's upper right-most connector; ports 10h-1Fh
inline constexpr unsigned kCard256Slot = 0;
// The Multichip card's default slot, clear of the others'; ports 30h-3Fh
inline constexpr unsigned kMultichipSlot = 2;
// The NanoSD's default slot, the only one clear of the others'; ports 40h-4Fh
inline constexpr unsigned kNanoSdSlot = 3;
// The serial card's default slot, clear of the disk card's; ports 20h-2Fh, so OUT 33,5 on SYS 1.2
inline constexpr unsigned kSerialSlot = 1;

// The SID card: off, or a MOS 6581 or 8580 at ports E0h-FFh outside the slots
enum class SidModel : std::uint8_t { None, Mos6581, Mos8580 };

struct SidCardConfig {
    SidModel model = SidModel::None;
    std::uint32_t clock_hz = 985'248;  // a PAL C64's, as WinTVC uses ❓ (no card known)
    bool operator==(const SidCardConfig&) const = default;
};

struct MachineConfig {
    Model model = Model::Tvc64kPlus;
    RomImages roms;
    std::array<CardConfig, kCardSlots> cards{};
    SidCardConfig sid;
    bool colour = true;  // rear B/W switch, read at port 59h b6
    unsigned audio_rate = 48000;  // sound output in samples per second; 0 = none
};

}  // namespace freetvc
