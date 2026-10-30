// SPDX-License-Identifier: GPL-3.0-or-later
// Expansion cards and the four slots (tvc-hardware.md §9, design/cards.md "Slot model"): slot n has ports 10h + 10h·n to
// 1Fh + 10h·n, the IOMEM window C000-DFFF when port 03h b7-6 = n, and a 2-bit ID at port 5Ah.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <span>

#include "core/machine/config.hpp"
#include "core/video/video.hpp"

namespace freetvc {

class AudioSynth;
class Sound;
class SerialLine;
class ChunkWriter;
class ChunkReader;
class FloppyDrives;
struct Card256Media;
struct NanoSdMedia;

// What a card sees of the machine at an access.
struct CardBus {
    std::uint64_t tick = 0;  // half-T machine time
    FloppyDrives* drives = nullptr;
    AudioSynth* audio = nullptr;              // the bus SOUND line
    const std::uint8_t* keyboard = nullptr;   // the matrix rows, active low; rows 10-11 are joysticks 3 and 4
    Card256Media* card256 = nullptr;          // the TVC256k++'s pendrive and flash image
    NanoSdMedia* nanosd = nullptr;            // the NanoSD's SD card and the Nano's EEPROM
    const Sound* sound = nullptr;             // the tone generator, whose ÷2 stage is the bus TXRXCLK
    SerialLine* serial = nullptr;             // the serial card's cable
};

class Card {
public:
    virtual ~Card() = default;
    virtual std::unique_ptr<Card> clone() const = 0;
    virtual CardKind kind() const = 0;
    // Port 5Ah, two bits: 00 serial, 01 (unnamed), 10 floppy, 11 none or a card named by its own ROM header
    virtual unsigned id() const = 0;
    virtual void power_on() = 0;
    // The reset button, at the machine's `tick`
    virtual void reset(std::uint64_t tick) = 0;
    // The slot's 16 ports, 0-15
    virtual std::uint8_t io_read(unsigned port, const CardBus& bus) = 0;
    virtual void io_write(unsigned port, std::uint8_t value, const CardBus& bus) = 0;
    // IOMEM, offset from C000h; reads have no side effects
    virtual std::uint8_t mem_read(std::uint16_t offset) const = 0;
    virtual void mem_write(std::uint16_t offset, std::uint8_t value) = 0;
    // The 8 KiB the CPU sees at C000-DFFF, for debuggers
    virtual std::span<std::uint8_t> window() = 0;
    virtual void save(ChunkWriter& w) const = 0;
    virtual void load(ChunkReader& r) = 0;
    // A card with sound brings it up to `tick` before the machine's audio is read out
    virtual void advance_audio(std::uint64_t tick, AudioSynth& out) {
        (void)tick;
        (void)out;
    }
    // The 16 KiB a card serves in place of RAM unit 2 or 3 (it asserts /PHANTOM), or nullptr
    virtual std::uint8_t* phantom_page(unsigned unit) {
        (void)unit;
        return nullptr;
    }
    // Port 00h writes, which every card can see on the bus
    virtual void border_write(std::uint8_t value) { (void)value; }
    // The card that drives /EXTC and EC0-3, or nullptr
    virtual VideoOverlay* overlay() { return nullptr; }
    // The slot's /INT line, true while pulled low
    virtual bool irq() const { return false; }
    // A write to 58h + slot: the slot's /CLRINT strobe ([HW] port table, pp. 125-126)
    virtual void clear_interrupt(std::uint8_t value) { (void)value; }
    // The tick of the card's next timed event (a chip timer that can pull /INT), or UINT64_MAX
    virtual std::uint64_t next_event() const { return UINT64_MAX; }
    // Called once the machine reaches next_event(): the card catches up to bus.tick
    virtual void run_events(const CardBus& bus) { (void)bus; }
};

// "none", "hbf", "gamecard", "card256", "multichip", "nanosd", "serial"
const char* card_kind_name(CardKind kind);

// The four slots; a copy clones the cards.
class CardSlots {
public:
    CardSlots() = default;
    CardSlots(const CardSlots& other);
    CardSlots& operator=(const CardSlots& other);
    CardSlots(CardSlots&&) = default;
    CardSlots& operator=(CardSlots&&) = default;

    void configure(const std::array<CardConfig, kCardSlots>& configs);
    Card* card(unsigned slot) { return slot < kCardSlots ? cards_[slot].get() : nullptr; }
    const Card* card(unsigned slot) const { return slot < kCardSlots ? cards_[slot].get() : nullptr; }

    // Port 5Ah: slot n in bits 2n+1..2n, 11 for an empty slot
    std::uint8_t ids() const;
    void power_on();
    void reset(std::uint64_t tick);
    // Empty slots read FFh and ignore writes
    std::uint8_t io_read(unsigned slot, unsigned port, const CardBus& bus);
    void io_write(unsigned slot, unsigned port, std::uint8_t value, const CardBus& bus);
    std::uint8_t mem_read(unsigned slot, std::uint16_t offset) const;
    void mem_write(unsigned slot, std::uint16_t offset, std::uint8_t value);
    void advance_audio(std::uint64_t tick, AudioSynth& out);
    // The first card that serves RAM unit 2 or 3, else nullptr
    std::uint8_t* phantom_page(unsigned unit);
    void border_write(std::uint8_t value);
    // The first card with an overlay; two would fight over the lines (the manual forbids it)
    VideoOverlay* overlay();
    // Bit n set while slot n pulls /INT low
    std::uint8_t irq_lines() const;
    void clear_interrupt(unsigned slot, std::uint8_t value);
    // The earliest card event, kept up to date by every call that can move one
    std::uint64_t next_event() const { return next_event_; }
    void run_events(const CardBus& bus);
    // After a snapshot loads the cards' state
    void refresh_events();

private:
    std::array<std::unique_ptr<Card>, kCardSlots> cards_{};
    std::uint64_t next_event_ = UINT64_MAX;
};

}  // namespace freetvc
