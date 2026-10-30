// SPDX-License-Identifier: GPL-3.0-or-later
// The SD Dual without system ROMs (design/cards.md "SD Dual"): the SD card's SPI protocol (CRCs, initialisation,
// CSD and CID, single and multiple block reads and writes), the board's registers and windows, its flash with and
// without the jumper, snapshots and copies, the image builder; usage: sddual_test.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/bus/sd_card.hpp"
#include "core/bus/sddual.hpp"
#include "core/machine/machine.hpp"
#include "core/snapshot/snapshot.hpp"
#include "frontend/common/sd_image.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;
using freetvc::SdCard;
using freetvc::SdDual;

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

class MemoryImage final : public freetvc::SdCardImage {
public:
    explicit MemoryImage(std::uint64_t size, bool sparse = false) : size_(size) {
        if (!sparse) {
            bytes_.assign(size, 0);
            for (std::size_t i = 0; i < bytes_.size(); ++i) {
                bytes_[i] = static_cast<std::uint8_t>(i * 7 + (i >> 9));
            }
        }
    }
    std::uint64_t size() const override { return size_; }
    bool read(std::uint64_t offset, std::span<std::uint8_t> out) override {
        for (std::size_t i = 0; i < out.size(); ++i) {
            out[i] = byte(offset + i);
        }
        return true;
    }
    bool write(std::uint64_t offset, std::span<const std::uint8_t> data) override {
        if (bytes_.empty()) {
            return false;
        }
        std::copy(data.begin(), data.end(), bytes_.begin() + static_cast<std::ptrdiff_t>(offset));
        return true;
    }
    bool read_only() const override { return read_only_; }
    std::uint8_t byte(std::uint64_t at) const { return bytes_.empty() ? static_cast<std::uint8_t>(at >> 9) : bytes_[at]; }
    bool read_only_ = false;

private:
    std::uint64_t size_;
    Bytes bytes_;
};

// A command with its CRC7, then the R1 (the first byte without b7 within eight)
std::uint8_t command(SdCard& card, freetvc::SdCardImage& image, unsigned index, std::uint32_t arg) {
    std::uint8_t c[6] = {static_cast<std::uint8_t>(0x40 | index), static_cast<std::uint8_t>(arg >> 24), static_cast<std::uint8_t>(arg >> 16),
                         static_cast<std::uint8_t>(arg >> 8), static_cast<std::uint8_t>(arg), 0};
    c[5] = static_cast<std::uint8_t>((SdCard::crc7(std::span(c).first(5)) << 1) | 1);
    for (const std::uint8_t b : c) {
        card.transfer(b, &image);
    }
    for (int i = 0; i < 8; ++i) {
        const std::uint8_t r = card.transfer(0xFF, &image);
        if (!(r & 0x80)) {
            return r;
        }
    }
    return 0xFF;
}

// A data block after its start token: the payload, then whether its CRC16 matched
bool read_block(SdCard& card, freetvc::SdCardImage& image, Bytes& out, std::size_t length) {
    int wait = 0;
    std::uint8_t token = 0xFF;
    while ((token = card.transfer(0xFF, &image)) == 0xFF && ++wait < 16) {
    }
    if (token != 0xFE) {
        return false;
    }
    out.resize(length);
    for (std::uint8_t& b : out) {
        b = card.transfer(0xFF, &image);
    }
    const std::uint16_t crc = static_cast<std::uint16_t>((card.transfer(0xFF, &image) << 8) | card.transfer(0xFF, &image));
    return crc == SdCard::crc16(out);
}

void test_crc() {
    const std::uint8_t cmd0[] = {0x40, 0, 0, 0, 0};
    const std::uint8_t cmd8[] = {0x48, 0, 0, 0x01, 0xAA};
    check(((SdCard::crc7(cmd0) << 1) | 1) == 0x95, "CRC7 of CMD0 is 95h");
    check(((SdCard::crc7(cmd8) << 1) | 1) == 0x87, "CRC7 of CMD8 1AAh is 87h");
    const Bytes ff(512, 0xFF);
    check(SdCard::crc16(ff) == 0x7FA1, "CRC16 of 512 bytes FFh is 7FA1h");
}

void test_sdsc() {
    MemoryImage image(64u << 20);
    SdCard card;
    check(card.transfer(0x40, &image) == 0xFF && !card.spi_mode(), "before CMD0 the card is in SD mode");
    card.power_on();
    card.select();
    // A CMD0 with a wrong CRC is SD-bus noise
    for (const std::uint8_t b : {0x40, 0, 0, 0, 0, 0x01}) {
        card.transfer(static_cast<std::uint8_t>(b), &image);
    }
    check(card.transfer(0xFF, &image) == 0xFF && !card.spi_mode(), "a CMD0 with a bad CRC does not reach SPI mode");
    check(command(card, image, 0, 0) == 0x01 && card.spi_mode(), "CMD0: idle, SPI mode");
    check(command(card, image, 17, 0) == (SdCard::kIdle | SdCard::kIllegal), "CMD17 in idle: illegal");
    check(command(card, image, 8, 0x1AA) == 0x01, "CMD8: R7");
    std::uint8_t r7[4];
    for (std::uint8_t& b : r7) {
        b = card.transfer(0xFF, &image);
    }
    check(r7[2] == 0x01 && r7[3] == 0xAA, "CMD8 echoes the voltage and check pattern");
    command(card, image, 55, 0);
    check(command(card, image, 41, 0x40000000) == 0x01, "the first ACMD41 is still idle");
    command(card, image, 55, 0);
    check(command(card, image, 41, 0) == 0x00 && card.ready(), "the second ACMD41: ready");
    check(command(card, image, 58, 0) == 0x00, "CMD58");
    std::uint8_t ocr[4];
    for (std::uint8_t& b : ocr) {
        b = card.transfer(0xFF, &image);
    }
    check(ocr[0] == 0x80 && ocr[1] == 0xFF && ocr[2] == 0x80, "OCR: powered up, SDSC, 2.7-3.6 V");

    // CSD 1.0 for 64 MiB: (C_SIZE + 1) * 2^(C_SIZE_MULT + 2) * 2^READ_BL_LEN
    check(command(card, image, 9, 0) == 0x00, "CMD9");
    Bytes csd;
    check(read_block(card, image, csd, 16), "the CSD's CRC16");
    const unsigned bl = csd[5] & 0x0F;
    const unsigned c_size = ((csd[6] & 3u) << 10) | (csd[7] << 2) | (csd[8] >> 6);
    const unsigned mult = ((csd[9] & 3u) << 1) | (csd[10] >> 7);
    check((csd[0] >> 6) == 0 && (std::uint64_t{c_size} + 1) * (1u << (mult + 2)) * (1u << bl) == (64u << 20), "CSD 1.0 capacity 64 MiB");
    check(((SdCard::crc7(std::span(csd).first(15)) << 1) | 1) == csd[15], "the CSD's CRC7");
    check(command(card, image, 10, 0) == 0x00 && read_block(card, image, csd, 16), "CMD10: the CID");

    // Byte addresses: block 3 is at 600h
    Bytes block;
    check(command(card, image, 17, 3 * 512) == 0x00 && read_block(card, image, block, 512), "CMD17 at 600h");
    check(block[0] == image.byte(3 * 512) && block[511] == image.byte(3 * 512 + 511), "the block's bytes");
    check(command(card, image, 17, 3 * 512 + 1) == SdCard::kAddressError, "a misaligned 512-byte read");
    check(command(card, image, 17, 64u << 20) == SdCard::kParameterError, "a read past the end");
    check(command(card, image, 16, 16) == 0x00 && command(card, image, 17, 0x210) == 0x00 && read_block(card, image, block, 16) &&
              block[0] == image.byte(0x210),
          "CMD16 16: a partial block");
    command(card, image, 16, 512);

    // READ_MULTIPLE_BLOCK until CMD12 (a stuff byte, then R1b)
    check(command(card, image, 18, 10 * 512) == 0x00, "CMD18");
    bool all = true;
    for (int i = 0; i < 3; ++i) {
        all = read_block(card, image, block, 512) && block[5] == image.byte((10 + i) * 512 + 5) && all;
    }
    check(all, "three blocks in a row");
    check(command(card, image, 12, 0) == 0x00, "CMD12 stops it");

    // WRITE_BLOCK: token, data, CRC, then the data response and busy
    check(command(card, image, 24, 5 * 512) == 0x00, "CMD24");
    card.transfer(0xFF, &image);
    card.transfer(0xFE, &image);
    for (int i = 0; i < 512; ++i) {
        card.transfer(static_cast<std::uint8_t>(i ^ 0x5A), &image);
    }
    card.transfer(0xFF, &image);
    card.transfer(0xFF, &image);
    check((card.transfer(0xFF, &image) & 0x1F) == 0x05, "the block is accepted");
    check(card.transfer(0xFF, &image) == 0x00, "then busy");
    check(image.byte(5 * 512) == 0x5A && image.byte(5 * 512 + 511) == static_cast<std::uint8_t>(511 ^ 0x5A), "the block is in the image");

    // WRITE_MULTIPLE_BLOCK: FCh per block, FDh to stop
    check(command(card, image, 25, 20 * 512) == 0x00, "CMD25");
    for (int b = 0; b < 2; ++b) {
        card.transfer(0xFF, &image);
        card.transfer(0xFC, &image);
        for (int i = 0; i < 512; ++i) {
            card.transfer(static_cast<std::uint8_t>(b + 1), &image);
        }
        card.transfer(0xFF, &image);
        card.transfer(0xFF, &image);
        check((card.transfer(0xFF, &image) & 0x1F) == 0x05, "multi-block write accepted");
        while (card.transfer(0xFF, &image) == 0x00) {
        }
    }
    card.transfer(0xFD, &image);
    check(image.byte(20 * 512) == 1 && image.byte(21 * 512 + 100) == 2 && image.byte(22 * 512) != 3, "two blocks written");

    // A read-only image refuses the block
    image.read_only_ = true;
    command(card, image, 24, 6 * 512);
    card.transfer(0xFE, &image);
    for (int i = 0; i < 514; ++i) {
        card.transfer(0x00, &image);
    }
    check((card.transfer(0xFF, &image) & 0x1F) == 0x0D, "a read-only image gives a write error");
}

void test_sdhc() {
    MemoryImage image(3ull << 30, true);
    SdCard card;
    check(SdCard::high_capacity(image), "3 GiB is SDHC");
    // Without HCS on the first ACMD41 the card stays idle; the HCS that counts is the first call's
    card.power_on();
    card.select();
    command(card, image, 0, 0);
    bool idle = true;
    for (int i = 0; i < 5; ++i) {
        command(card, image, 55, 0);
        idle = command(card, image, 41, 0) == 0x01 && idle;
    }
    check(idle, "SDHC without HCS never gets ready");
    card.power_on();
    card.select();
    command(card, image, 0, 0);
    command(card, image, 55, 0);
    command(card, image, 41, 0x40000000);
    command(card, image, 55, 0);
    check(command(card, image, 41, 0) == 0x00, "HCS on the first ACMD41 only (the SD Dual firmware's loop)");
    command(card, image, 58, 0);
    check(card.transfer(0xFF, &image) == 0xC0, "OCR: CCS set");
    Bytes csd;
    command(card, image, 9, 0);
    read_block(card, image, csd, 16);
    const std::uint32_t c_size = ((csd[7] & 0x3Fu) << 16) | (csd[8] << 8) | csd[9];
    check((csd[0] >> 6) == 1 && (std::uint64_t{c_size} + 1) * 512 * 1024 == (3ull << 30), "CSD 2.0 capacity 3 GiB");
    // Block addresses
    Bytes block;
    check(command(card, image, 17, 5000) == 0x00 && read_block(card, image, block, 512) && block[0] == image.byte(5000ull * 512),
          "CMD17 by block number");
    check(command(card, image, 16, 16) == 0x00 && command(card, image, 17, 1) == 0x00 && read_block(card, image, block, 512),
          "SDHC keeps 512-byte blocks after CMD16");
}

// The board through its window offsets
void test_board() {
    Bytes firmware(0xA000, 0x00);
    for (unsigned p = 0; p < 5; ++p) {
        firmware[p * SdDual::kPage] = static_cast<std::uint8_t>(0x10 + p);
    }
    SdDual sd(firmware, false);
    MemoryImage image(1u << 20);
    freetvc::SdImages none{};
    freetvc::SdImages micro{&image, nullptr};
    check(sd.read(0x0000, 0, none) == 0x10, "page 0 at C000h");
    sd.write(0x3C02, 0x7F, 0, none);  // b7-5 = 3, the rest ignored
    check(sd.page() == 3 && sd.read(0x0000, 0, none) == 0x13, "+2 b7-5 select the 8 KiB page");
    sd.write(0x3C02, 0xE0, 0, none);
    check(sd.read(0x0000, 0, none) == 0xFF, "page 7: erased flash past the firmware");
    sd.write(0x2000, 0x12, 0, none);
    sd.write(0x3BFF, 0x34, 0, none);
    check(sd.read(0x2000, 0, none) == 0x12 && sd.read(0x3BFF, 0, none) == 0x34, "RAM at E000h-FBFFh");
    check(sd.peek(0x2000) == 0x12 && sd.peek(0x3C01) == 0xFF, "peek: RAM, no registers");

    // Status: protect (socket 1 empty or read-only), no card (both empty), changed until cleared
    check(sd.read(0x3C01, 0, none) == 0xE0, "status with no cards: protect, no card, changed");
    sd.write(0x3C01, SdDual::kClearChanged, 0, micro);
    check(sd.read(0x3C01, 0, micro) == 0x80, "a microSD card: present, the SD socket's switch reads protected");
    check(sd.read(0x3C05, 0, micro) == 0x80 && sd.read(0x3FFD, 0, micro) == 0x80, "A1-A0 only: the registers repeat");
    sd.card_changed(0);
    check(sd.read(0x3C01, 0, micro) & SdDual::kStatusChanged, "a card going in sets the change latch");
    MemoryImage protect(1u << 20);
    protect.read_only_ = true;
    freetvc::SdImages sd1{nullptr, &protect};
    check((sd.read(0x3C01, 0, sd1) & 0xC0) == 0x80, "a read-only SD card: protected");

    // SPI through +0 with CS0, the byte in readable from +0
    sd.write(0x3C01, SdDual::kSelect0, 0, micro);
    for (const std::uint8_t b : {0x40, 0, 0, 0, 0, 0x95}) {
        sd.write(0x3C00, b, 0, micro);
    }
    sd.write(0x3C00, 0xFF, 0, micro);
    check(sd.read(0x3C00, 0, micro) == 0x01 && sd.read(0x3C00, 0, micro) == 0x01, "R1 of CMD0; a plain read does not clock");
    check(sd.card(0).spi_mode() && !sd.card(1).spi_mode(), "only the selected card heard it");
    // Fast mode: every register read gives the byte in and clocks FFh
    sd.write(0x3C03, SdDual::kFast, 0, micro);
    check(sd.read(0x3D17, 0, micro) == 0x01 && sd.read(0x3C00, 0, micro) == 0xFF, "fast mode clocks on every read");
    sd.write(0x3C03, 0x00, 0, micro);
    sd.write(0x3C01, 0x00, 0, micro);
    sd.write(0x3C00, 0x00, 0, micro);
    check(sd.read(0x3C00, 0, micro) == 0xFF, "nothing selected: MISO high");

    // The flash: AMD commands at AAAh/555h, only with the jumper
    sd.write(0x3C02, 0x00, 0, none);
    const auto program = [&](SdDual& board, std::uint16_t at, std::uint8_t value) {
        board.write(0x0AAA, 0xAA, 0, none);
        board.write(0x0555, 0x55, 0, none);
        board.write(0x0AAA, 0xA0, 0, none);
        board.write(at, value, 0, none);
    };
    program(sd, 0x0001, 0x00);
    check(sd.read(0x0001, 1'000'000, none) == 0x00 && sd.flash()[1] == 0x00, "without the jumper the flash keeps its bytes");
    SdDual open(firmware, true);
    program(open, 0x1000, 0x5A);
    check((open.read(0x1000, 1, none) & 0x80) == 0x80, "while programming: DQ7 is the complement");
    check(open.read(0x1000, 1'000'000, none) == 0x00, "programming only clears bits (00h stays)");
    // Commands while it programs are ignored; the erase comes after
    constexpr std::uint64_t t = 1'000'000;
    open.write(0x3C02, 0x80, t, none);
    open.write(0x0AAA, 0xAA, t, none);
    open.write(0x0555, 0x55, t, none);
    open.write(0x0AAA, 0x80, t, none);
    open.write(0x0AAA, 0xAA, t, none);
    open.write(0x0555, 0x55, t, none);
    open.write(0x0000, 0x30, t, none);
    const std::uint8_t s1 = open.read(0x0000, t + 10, none);
    const std::uint8_t s2 = open.read(0x0000, t + 11, none);
    check(((s1 ^ s2) & 0x40) && !(s1 & 0x80), "erasing: DQ6 toggles, DQ7 low");
    check(std::all_of(open.flash().begin(), open.flash().end(), [](std::uint8_t b) { return b == 0xFF; }),
          "one sector erase clears the whole 64 KiB");
    open.write(0x3C02, 0x00, 0, none);
    constexpr std::uint64_t later = 20'000'000;
    open.write(0x0AAA, 0xAA, later, none);
    open.write(0x0555, 0x55, later, none);
    open.write(0x0AAA, 0xA0, later, none);
    open.write(0x0000, 0x4D, later, none);
    check(open.read(0x0000, later + 1'000'000, none) == 0x4D, "programmed after the erase");
}

void test_machine() {
    freetvc::MachineConfig config;
    config.roms.cart.assign(0x2000, 0x00);
    config.roms.cart[0] = 'M';
    config.roms.cart_type = freetvc::CartType::SdDual;
    freetvc::Machine m(config);
    MemoryImage image(1u << 20);
    m.set_sd_image(0, &image);
    check(m.memory().sddual() && m.sd_image(0) == &image, "the machine has the board and the image");
    m.io_write(0x02, 0x00);  // page 3 = CART
    m.poke(0xE123, 0x77);
    check(m.memory().read(0xE123) == 0x77 && m.memory().read(0xC000) == 'M', "CART at C000h: flash, RAM");
    freetvc::Snapshot snap;
    m.save_state(snap);
    check(snap.find("SDDC") != nullptr, "a snapshot has the SDDC chunk");
    freetvc::Machine copy = m;
    check(copy.sd_image(0) == nullptr && copy.memory().read(0xE123) == 0x77, "a copy has the board but no images");
    m.poke(0xE123, 0x00);
    std::string error;
    check(m.load_state(snap, error) && m.memory().read(0xE123) == 0x77 && m.sd_image(0) == &image,
          "loading keeps the images: " + error);
}

void test_builder() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "freetvc-sddual-test";
    fs::remove_all(dir);
    fs::create_directories(dir / "src" / "GAMES");
    std::ofstream(dir / "src" / "A.TXT") << "hello";
    std::ofstream(dir / "src" / "GAMES" / "long name.cas") << "x";
    std::string error;
    const std::string out = (dir / "card.img").string();
    check(freetvc::write_sd_image(out, 8, {(dir / "src" / "A.TXT").string(), (dir / "src" / "GAMES").string()}, error),
          "build an 8 MiB image: " + error);
    const auto listing = freetvc::sd_image_listing(out);
    check(listing.size() == 3 && listing[0] == "/A.TXT" && listing[1] == "/GAMES/" && listing[2] == "/GAMES/long name.cas",
          "the image lists its files");
    Bytes mbr(512);
    std::ifstream(out, std::ios::binary).read(reinterpret_cast<char*>(mbr.data()), 512);
    check(mbr[510] == 0x55 && mbr[511] == 0xAA && mbr[0x1BE + 4] != 0, "an MBR with one partition");
    check(fs::file_size(out) == 8u << 20 && !fs::exists(out + ".tmp"), "8 MiB, no temporary file left");
    freetvc::SdImageFile file;
    check(file.open(out, error) && !file.read_only() && file.size() == 8u << 20, "the image opens for writing");
    file.close();
    check(!freetvc::write_sd_image((dir / "full.img").string(), 1, {(dir / "src").string(), out}, error) &&
              !fs::exists(dir / "full.img"),
          "an image too small fails and leaves nothing");
    fs::remove_all(dir);
}

}  // namespace

int main() {
    test_crc();
    test_sdsc();
    test_sdhc();
    test_board();
    test_machine();
    test_builder();
    if (g_failures != 0) {
        std::printf("%d failure(s)\n", g_failures);
        return 1;
    }
    std::printf("sddual_test: all passed\n");
    return 0;
}
