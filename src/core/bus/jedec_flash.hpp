// SPDX-License-Identifier: GPL-3.0-or-later
// Byte-wide flash chips with the JEDEC command set (unlock, program, sector and chip erase, ID mode): the SST39SF040
// from its data sheet DS20005022F (design/cards.md "MultiCart v2") and the SD Dual's chip (design/cards.md "SD Dual").
// The bytes stay outside; the chip keeps only its command state.
#pragma once

#include <cstdint>
#include <span>

namespace freetvc {

class ChunkWriter;
class ChunkReader;

struct FlashSpec {
    std::uint32_t size = 0;
    std::uint32_t sector_size = 0;
    std::uint32_t unlock1 = 0;  // the AAh cycle's address
    std::uint32_t unlock2 = 0;  // the 55h cycle's address
    std::uint32_t command_mask = 0;  // the address lines a command cycle compares
    std::uint8_t maker_id = 0;
    std::uint8_t device_id = 0;
    std::uint32_t program_us = 0;
    std::uint32_t sector_erase_us = 0;
    std::uint32_t chip_erase_us = 0;
};

// The data sheet's figures: A14-A0 compared (Table 4-2 note 1), 4 KiB sectors, typical times 14 µs, 18 ms, 70 ms
inline constexpr FlashSpec kSst39sf040{0x80000, 0x1000, 0x5555, 0x2AAA, 0x7FFF, 0xBF, 0xB7, 14, 18'000, 70'000};

class JedecFlash {
public:
    explicit JedecFlash(const FlashSpec& spec) : spec_(&spec) {}

    const FlashSpec& spec() const { return *spec_; }
    void power_on();
    // CE# and OE# low: the array byte, the ID in ID mode, or the status bits while it programs or erases
    std::uint8_t read(std::uint32_t address, std::uint64_t tick, std::span<const std::uint8_t> bytes);
    // CE# and WE# low; true when the array changed
    bool write(std::uint32_t address, std::uint8_t value, std::uint64_t tick, std::span<std::uint8_t> bytes);

    bool busy(std::uint64_t tick) const { return tick < busy_until_; }
    bool id_mode() const { return id_mode_; }
    void save(ChunkWriter& w) const;
    void load(ChunkReader& r);

private:
    enum class Step : std::uint8_t { Ready, Unlock1, Unlock2, Program, Erase, EraseUnlock1, EraseUnlock2 };

    bool at(std::uint32_t address, std::uint32_t command) const { return (address & spec_->command_mask) == command; }

    const FlashSpec* spec_;
    Step step_ = Step::Ready;
    bool id_mode_ = false;
    std::uint64_t busy_until_ = 0;
    std::uint8_t final_dq7_ = 0;  // what DQ7 reads once the operation ends (Data# polling gives its complement)
    bool toggle_ = false;         // DQ6: high on the first status read
};

}  // namespace freetvc
