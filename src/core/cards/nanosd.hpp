// SPDX-License-Identifier: GPL-3.0-or-later
// Vass Sándor's NanoSD IO Card (design/cards.md "NanoSD"): two latches to an Arduino Nano that serves an SD card,
// and a 64 KiB EEPROM whose 8 KiB at C000h + 2000h * bank the Nano's A13 line selects.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "core/cards/card.hpp"

namespace freetvc {

class FatVolume;

// What the card reaches outside the machine state; the front end owns it.
struct NanoSdMedia {
    FatVolume* sd = nullptr;  // the SD card's folder; nullptr is an empty socket
    std::uint64_t sd_changed = 0;  // machine time of the last insert or eject, when the card-detect pin moved
    bool skip_menu = false;  // an autostart: parameter 0 reads 0 (no menu at boot), the EEPROM untouched (a host shortcut)
    // The Nano's EEPROM: its ID string written (checkIdInEeprom), then menu at boot and sort order
    bool eeprom_valid = false;
    std::uint8_t menu = 1;
    std::uint8_t sort = 0;
};

class NanoSd final : public Card {
public:
    static constexpr std::size_t kRomSize = 0x10000;
    static constexpr std::size_t kBankSize = 0x2000;
    static constexpr std::size_t kQueueSize = 80;  // INPUTDATABUFFER_SIZE, OUTPUTDATABUFFER_SIZE ⚑
    static constexpr std::uint64_t kDebounceTicks = 1'875'000;  // the card-detect handler's ~300 ms loop

    // A short image reads FFh where missing (an erased EEPROM).
    explicit NanoSd(std::span<const std::uint8_t> rom);

    std::unique_ptr<Card> clone() const override { return std::make_unique<NanoSd>(*this); }
    CardKind kind() const override { return CardKind::NanoSd; }
    unsigned id() const override { return 3; }  // IDA/IDB not connected: named by its ROM header
    void power_on() override;
    void reset(std::uint64_t) override {}  // RESET is not wired: the Nano keeps running
    std::uint8_t io_read(unsigned port, const CardBus& bus) override;
    void io_write(unsigned port, std::uint8_t value, const CardBus& bus) override;
    std::uint8_t mem_read(std::uint16_t offset) const override { return window_[offset & 0x1FFF]; }
    void mem_write(std::uint16_t, std::uint8_t) override {}
    std::span<std::uint8_t> window() override { return window_; }
    void save(ChunkWriter& w) const override;
    void load(ChunkReader& r) override;

    // For the monitor
    unsigned bank() const { return bank_; }
    bool booted() const { return booted_; }  // setup() runs at the first access after power-on
    std::uint8_t status() const { return status_; }
    std::uint8_t latch() const { return latch_; }
    bool sd_ready() const { return sd_ready_; }
    bool hung() const { return hung_; }
    std::size_t input_size() const { return in_.size(); }
    std::size_t output_size() const { return out_.size(); }
    const std::string& current_dir() const { return cur_dir_; }
    // The open file's 8.3 path, or empty
    std::string open_file() const { return file_.valid ? file_.path : std::string(); }

    // An SD library File: a file or directory of the card, with the volume path of its real names
    struct File {
        bool valid = false;
        bool dir = false;
        bool write = false;
        std::string path;  // as the volume names it
        std::string name;  // File::name(): the 8.3 name, or the last component as given
        std::uint64_t size = 0;
        std::uint64_t pos = 0;
        std::uint32_t next = 0;  // a directory's openNextFile() position
    };

private:
    void select_bank(unsigned bank);
    // The firmware's setup(), on the first access after power-on
    void setup(const CardBus& bus);
    NanoSdMedia& media(const CardBus& bus);
    // The card-detect pin and its pin-change interrupt
    void sense_card(const CardBus& bus);
    void push_input(std::uint8_t value);
    // The main loop up to `bus.tick`: commands run as far as their input allows
    void run(const CardBus& bus);
    // One step of the current command; false while it waits for input or time
    bool step(const CardBus& bus);
    void set_status(std::uint8_t value);
    bool pop(std::uint8_t& value);
    // A string up to 00h into arg_; false while it is still arriving
    bool collect_string();
    void push(std::uint8_t value);
    void push_block(std::span<const std::uint8_t> data);
    void card_changed(const CardBus& bus);
    std::string file_path(std::string name) const;

    void finish_open(const CardBus& bus);
    void get_data(const CardBus& bus);
    bool list_next(const CardBus& bus);
    void file_info(const CardBus& bus);
    void create(const CardBus& bus);
    void put_data(const CardBus& bus);
    void close_write(const CardBus& bus);
    void get_parameter(const CardBus& bus);

    std::array<std::uint8_t, kRomSize> rom_{};
    std::array<std::uint8_t, kBankSize> window_{};
    unsigned bank_ = 0;        // the A13 pin
    unsigned bank_store_ = 0;  // memoryBankMapStore: reaches the pin with the next latched byte
    NanoSdMedia own_media_;    // when the machine has none attached (tests)

    bool booted_ = false;
    std::uint8_t latch_ = 0xFF;
    std::uint8_t status_ = 0x81;
    bool hung_ = false;  // blocked on a full output queue, until power-on
    std::deque<std::uint8_t> in_;
    std::deque<std::uint8_t> out_;
    bool pin_ = false;       // a card in the socket, as the pin-change interrupt last saw it
    bool pcint_on_ = true;   // the pin-change interrupt enabled
    bool sd_ready_ = false;  // sdInitialized
    std::uint64_t debounce_until_ = 0;
    std::uint64_t pin_changed_ = 0;  // when the pin-change interrupt queued the event

    bool in_command_ = false;
    std::uint8_t cmd_ = 0;
    unsigned phase_ = 0;
    std::string arg_;
    std::uint8_t number_ = 0;
    std::vector<std::uint8_t> data_;

    std::string cur_dir_ = "/";
    File dir_file_;   // currentDirFile
    File file_;       // openFile
    File list_dir_;   // LIST's cDir
    bool file_open_ = false;  // fileIsOpen
    std::int32_t counter_ = 0;  // fileSizeCounter: remainingBytes and bytesWritten in one long
    std::uint8_t save_type_ = 0;
};

}  // namespace freetvc
