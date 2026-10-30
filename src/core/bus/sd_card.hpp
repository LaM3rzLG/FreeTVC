// SPDX-License-Identifier: GPL-3.0-or-later
// An SD memory card in SPI mode (design/cards.md "SD Dual"), our own from the SD Physical Layer Simplified
// Specification's SPI chapter: commands, R1/R1b/R2/R3/R7 responses, data tokens, CSD and CID, CRC7 and CRC16.
// Up to 2 GiB it is an SDSC card (CSD 1.0, byte addresses), above that SDHC/SDXC (CSD 2.0, block addresses).
// The card's bytes are an image the front end serves, outside the machine state like a disk.
#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace freetvc {

class ChunkWriter;
class ChunkReader;

// A whole card's contents (MBR partitions or a super floppy), served by the front end
class SdCardImage {
public:
    virtual ~SdCardImage() = default;
    // Bytes, a multiple of 512
    virtual std::uint64_t size() const = 0;
    virtual bool read(std::uint64_t offset, std::span<std::uint8_t> out) = 0;
    virtual bool write(std::uint64_t offset, std::span<const std::uint8_t> data) = 0;
    virtual bool read_only() const = 0;
};

class SdCard {
public:
    static constexpr std::uint32_t kBlock = 512;
    static constexpr std::uint64_t kSdscLimit = 2ull << 30;  // larger images are SDHC/SDXC

    // R1 bits
    static constexpr std::uint8_t kIdle = 0x01;
    static constexpr std::uint8_t kIllegal = 0x04;
    static constexpr std::uint8_t kCrcError = 0x08;
    static constexpr std::uint8_t kAddressError = 0x20;
    static constexpr std::uint8_t kParameterError = 0x40;

    // Power from the socket: back to SD mode with nothing pending (a card going in, or the machine powering on)
    void power_on();
    // One byte each way with CS low; `image` null is an empty socket (the bus floats high)
    std::uint8_t transfer(std::uint8_t mosi, SdCardImage* image);
    // CS goes low: a command starts over
    void select();

    bool spi_mode() const { return mode_ != Mode::SdBus; }
    bool ready() const { return mode_ == Mode::Ready; }
    static bool high_capacity(const SdCardImage& image) { return image.size() > kSdscLimit; }
    // The registers for `image` (the CSD and the CID with their CRC7 in the last byte)
    static std::array<std::uint8_t, 16> csd(const SdCardImage& image);
    static std::array<std::uint8_t, 16> cid();
    static std::uint8_t crc7(std::span<const std::uint8_t> bytes);
    static std::uint16_t crc16(std::span<const std::uint8_t> bytes);

    void save(ChunkWriter& w) const;
    void load(ChunkReader& r);

private:
    enum class Mode : std::uint8_t { SdBus, Idle, Ready };
    enum class Phase : std::uint8_t { Command, WriteToken, WriteData };

    void command(SdCardImage* image);
    void respond(std::uint8_t r1) { out_.push_back(r1); }
    void send_data(std::span<const std::uint8_t> bytes);
    // The next block of a READ_MULTIPLE_BLOCK, or its error token
    void next_read_block(SdCardImage* image);
    void end_write_block(SdCardImage* image);
    std::uint8_t idle_bit() const { return mode_ == Mode::Ready ? 0 : kIdle; }
    // A command's argument as a byte offset, or false with R1 sent
    bool data_offset(std::uint32_t arg, SdCardImage* image, std::uint64_t& offset);

    Mode mode_ = Mode::SdBus;
    Phase phase_ = Phase::Command;
    std::array<std::uint8_t, 6> cmd_{};
    unsigned cmd_count_ = 0;
    bool app_command_ = false;  // the last command was CMD55
    bool crc_on_ = false;       // CMD59; CMD0 and CMD8 are always checked
    unsigned init_polls_ = 0;   // ACMD41/CMD1 calls since CMD0
    bool init_hcs_ = false;     // HCS of the call that started the initialisation
    std::uint32_t block_len_ = kBlock;
    bool reading_ = false;      // READ_MULTIPLE_BLOCK streaming
    bool writing_many_ = false; // WRITE_MULTIPLE_BLOCK in progress
    std::uint64_t next_offset_ = 0;
    std::vector<std::uint8_t> out_;  // MISO bytes still to send, first at out_[out_pos_]
    std::size_t out_pos_ = 0;
    std::vector<std::uint8_t> in_;   // a data block being received, with its CRC
};

}  // namespace freetvc
