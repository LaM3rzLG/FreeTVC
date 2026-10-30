// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/bus/sd_card.hpp"

#include <algorithm>

#include "core/snapshot/snapshot.hpp"

namespace freetvc {

namespace {

constexpr std::uint8_t kVersion = 1;
constexpr std::size_t kMaxOut = 2 + SdCard::kBlock + 8;
constexpr std::uint8_t kStartBlock = 0xFE;
constexpr std::uint8_t kStartMultiWrite = 0xFC;
constexpr std::uint8_t kStopTran = 0xFD;
// Data response tokens: accepted, CRC error, write error
constexpr std::uint8_t kDataAccepted = 0x05;
constexpr std::uint8_t kDataCrcError = 0x0B;
constexpr std::uint8_t kDataWriteError = 0x0D;
// Data error tokens: error, out of range
constexpr std::uint8_t kTokenError = 0x01;
constexpr std::uint8_t kTokenOutOfRange = 0x08;
// OCR: 2.7-3.6 V, card capacity status (b30), power-up done (b31)
constexpr std::uint32_t kOcrVoltages = 0x00FF8000;
constexpr std::uint32_t kOcrCcs = 0x40000000;
constexpr std::uint32_t kOcrReady = 0x80000000;
constexpr std::uint32_t kHcs = 0x40000000;
// ACMD41 or CMD1 calls until the card leaves the idle state; any count is within the specification ❓
constexpr unsigned kInitPolls = 2;

}  // namespace

std::uint8_t SdCard::crc7(std::span<const std::uint8_t> bytes) {
    std::uint8_t crc = 0;
    for (const std::uint8_t byte : bytes) {
        for (int bit = 7; bit >= 0; --bit) {
            const bool in = ((byte >> bit) & 1) != ((crc >> 6) & 1);
            crc = static_cast<std::uint8_t>((crc << 1) & 0x7F);
            if (in) {
                crc ^= 0x09;
            }
        }
    }
    return crc;
}

std::uint16_t SdCard::crc16(std::span<const std::uint8_t> bytes) {
    std::uint16_t crc = 0;
    for (const std::uint8_t byte : bytes) {
        crc ^= static_cast<std::uint16_t>(byte << 8);
        for (int bit = 0; bit < 8; ++bit) {
            crc = static_cast<std::uint16_t>((crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1);
        }
    }
    return crc;
}

std::array<std::uint8_t, 16> SdCard::csd(const SdCardImage& image) {
    std::array<std::uint8_t, 16> r{};
    const std::uint64_t size = image.size();
    if (high_capacity(image)) {
        // CSD 2.0: capacity (C_SIZE + 1) x 512 KiB, READ_BL_LEN 9, ERASE_BLK_EN, SECTOR_SIZE 7Fh, R2W 2, WRITE_BL_LEN 9
        const std::uint32_t c_size = static_cast<std::uint32_t>(std::min<std::uint64_t>(size >> 19, 0x400000) - 1);
        r = {0x40, 0x0E, 0x00, 0x32, 0x5B, 0x59, 0x00, static_cast<std::uint8_t>((c_size >> 16) & 0x3F),
             static_cast<std::uint8_t>(c_size >> 8), static_cast<std::uint8_t>(c_size), 0x7F, 0x80, 0x0A, 0x40, 0x00, 0x00};
    } else {
        // CSD 1.0: capacity (C_SIZE + 1) x 2^(C_SIZE_MULT + 2) x 2^READ_BL_LEN, READ_BL_LEN 9 up to 1 GiB, else 10
        const unsigned bl = size > (1u << 30) ? 10 : 9;
        const std::uint64_t blocks = size >> bl;
        unsigned mult = 0;
        while (mult < 7 && (blocks >> (mult + 2)) > 4096) {
            ++mult;
        }
        const std::uint32_t c_size = static_cast<std::uint32_t>(std::clamp<std::uint64_t>(blocks >> (mult + 2), 1, 4096) - 1);
        r = {0x00, 0x26, 0x00, 0x32, 0x5B, static_cast<std::uint8_t>(0x50 | bl), static_cast<std::uint8_t>(0x80 | (c_size >> 10)),
             static_cast<std::uint8_t>(c_size >> 2), static_cast<std::uint8_t>(((c_size & 3) << 6) | 0x2E),
             static_cast<std::uint8_t>(0xB8 | (mult >> 1)), static_cast<std::uint8_t>(((mult & 1) << 7) | 0x7F), 0x80,
             static_cast<std::uint8_t>(0x08 | (bl >> 2)), static_cast<std::uint8_t>((bl & 3) << 6), 0x00, 0x00};
    }
    r[15] = static_cast<std::uint8_t>((crc7(std::span(r).first(15)) << 1) | 1);
    return r;
}

std::array<std::uint8_t, 16> SdCard::cid() {
    // No manufacturer of ours: MID 0, OID "FT", name "FTVC ", revision 1.0, serial 1, made 2026-10
    std::array<std::uint8_t, 16> r{0x00, 'F', 'T', 'F', 'T', 'V', 'C', ' ', 0x10, 0x00, 0x00, 0x00, 0x01, 0x01, 0xAA, 0x00};
    r[15] = static_cast<std::uint8_t>((crc7(std::span(r).first(15)) << 1) | 1);
    return r;
}

void SdCard::power_on() { *this = SdCard(); }

void SdCard::select() {
    // A command starts over; what the card was sending is dropped, a block being written too ❓
    cmd_count_ = 0;
    out_.clear();
    out_pos_ = 0;
    reading_ = false;
    if (phase_ == Phase::WriteData) {
        phase_ = writing_many_ ? Phase::WriteToken : Phase::Command;
    }
    in_.clear();
}

std::uint8_t SdCard::transfer(std::uint8_t mosi, SdCardImage* image) {
    if (!image) {
        return 0xFF;
    }
    std::uint8_t miso = 0xFF;
    if (out_pos_ >= out_.size() && reading_) {
        next_read_block(image);
    }
    if (out_pos_ < out_.size()) {
        miso = out_[out_pos_++];
        if (out_pos_ == out_.size()) {
            out_.clear();
            out_pos_ = 0;
        }
    }
    switch (phase_) {
    case Phase::WriteToken:
        if (mosi == (writing_many_ ? kStartMultiWrite : kStartBlock)) {
            phase_ = Phase::WriteData;
            in_.clear();
        } else if (writing_many_ && mosi == kStopTran) {
            // One byte, then busy until the card has programmed
            writing_many_ = false;
            phase_ = Phase::Command;
            out_.insert(out_.end(), {0xFF, 0x00, 0x00});
        }
        break;
    case Phase::WriteData:
        in_.push_back(mosi);
        if (in_.size() == kBlock + 2) {
            end_write_block(image);
        }
        break;
    case Phase::Command:
        if (cmd_count_ == 0 && (mosi & 0xC0) != 0x40) {
            break;  // a command starts with 01b
        }
        cmd_[cmd_count_++] = mosi;
        if (cmd_count_ == cmd_.size()) {
            cmd_count_ = 0;
            command(image);
        }
        break;
    }
    return miso;
}

void SdCard::send_data(std::span<const std::uint8_t> bytes) {
    const std::uint16_t crc = crc16(bytes);
    out_.push_back(0xFF);  // NAC: one byte before the start token
    out_.push_back(kStartBlock);
    out_.insert(out_.end(), bytes.begin(), bytes.end());
    out_.push_back(static_cast<std::uint8_t>(crc >> 8));
    out_.push_back(static_cast<std::uint8_t>(crc));
}

bool SdCard::data_offset(std::uint32_t arg, SdCardImage* image, std::uint64_t& offset) {
    const bool hc = high_capacity(*image);
    offset = hc ? std::uint64_t{arg} * kBlock : arg;
    const std::uint32_t length = hc ? kBlock : block_len_;
    if (offset + length > image->size()) {
        respond(kParameterError);
        return false;
    }
    // READ_BLK_MISALIGN and WRITE_BLK_MISALIGN are 0: a block may not cross a 512-byte boundary
    if (!hc && (offset % kBlock) + length > kBlock) {
        respond(kAddressError);
        return false;
    }
    return true;
}

void SdCard::next_read_block(SdCardImage* image) {
    const std::uint32_t length = high_capacity(*image) ? kBlock : block_len_;
    std::vector<std::uint8_t> block(length);
    if (next_offset_ + length > image->size()) {
        reading_ = false;
        out_.insert(out_.end(), {0xFF, kTokenOutOfRange});
        return;
    }
    if (!image->read(next_offset_, block)) {
        reading_ = false;
        out_.insert(out_.end(), {0xFF, kTokenError});
        return;
    }
    next_offset_ += length;
    send_data(block);
}

void SdCard::end_write_block(SdCardImage* image) {
    const std::span<const std::uint8_t> data = std::span<const std::uint8_t>(in_).first(kBlock);
    const std::uint16_t crc = static_cast<std::uint16_t>((in_[kBlock] << 8) | in_[kBlock + 1]);
    std::uint8_t token = kDataAccepted;
    if (crc_on_ && crc != crc16(data)) {
        token = kDataCrcError;
    } else if (next_offset_ + kBlock > image->size() || image->read_only() || !image->write(next_offset_, data)) {
        token = kDataWriteError;
    } else {
        next_offset_ += kBlock;
    }
    in_.clear();
    // The data response, then busy while the block is programmed
    out_.insert(out_.end(), {token, 0x00, 0x00});
    phase_ = writing_many_ && token == kDataAccepted ? Phase::WriteToken : Phase::Command;
    if (phase_ == Phase::Command) {
        writing_many_ = false;
    }
}

void SdCard::command(SdCardImage* image) {
    const unsigned index = cmd_[0] & 0x3F;
    const std::uint32_t arg = (std::uint32_t{cmd_[1]} << 24) | (std::uint32_t{cmd_[2]} << 16) | (std::uint32_t{cmd_[3]} << 8) | cmd_[4];
    const bool crc_ok = static_cast<std::uint8_t>((crc7(std::span(cmd_).first(5)) << 1) | 1) == cmd_[5];
    const bool app = app_command_;
    app_command_ = false;
    if (mode_ == Mode::SdBus) {
        // Only CMD0 with CS low and a valid CRC moves the card to SPI mode; the rest is SD-bus traffic it ignores
        if (index == 0 && crc_ok) {
            mode_ = Mode::Idle;
            init_polls_ = 0;
            respond(kIdle);
        }
        return;
    }
    if (!crc_ok && (crc_on_ || index == 0 || index == 8)) {
        respond(idle_bit() | kCrcError);
        return;
    }
    if (index != 12) {
        reading_ = false;
        out_.clear();
        out_pos_ = 0;
    }
    const auto init = [&](bool hcs) {
        // An SDHC/SDXC card never leaves idle for a host that does not support it. The HCS that counts is the first
        // call's: the SD Dual firmware sends it only there (D2C7h), and SDHC cards work on it ❓
        if (init_polls_++ == 0) {
            init_hcs_ = hcs;
        }
        if ((!high_capacity(*image) || init_hcs_) && init_polls_ >= kInitPolls) {
            mode_ = Mode::Ready;
        }
        respond(idle_bit());
    };
    if (app) {
        switch (index) {
        case 41:
            init((arg & kHcs) != 0);
            return;
        case 23:  // SET_WR_BLK_ERASE_COUNT: only a hint
            respond(idle_bit());
            return;
        default:
            break;
        }
    }
    // In the idle state the card takes only the commands that initialise it
    const bool idle_command = index == 0 || index == 1 || index == 8 || index == 55 || index == 58 || index == 59;
    if (mode_ == Mode::Idle && !idle_command) {
        respond(kIdle | kIllegal);
        return;
    }
    std::uint64_t offset = 0;
    switch (index) {
    case 0:  // GO_IDLE_STATE
        mode_ = Mode::Idle;
        init_polls_ = 0;
        block_len_ = kBlock;
        crc_on_ = false;
        writing_many_ = false;
        phase_ = Phase::Command;
        respond(kIdle);
        break;
    case 1:  // SEND_OP_COND
        init((arg & kHcs) != 0);
        break;
    case 8:  // SEND_IF_COND: R7 echoes the voltage it accepts (2.7-3.6 V only) and the check pattern
        respond(idle_bit());
        out_.insert(out_.end(), {0x00, 0x00, static_cast<std::uint8_t>(((arg >> 8) & 0x0F) == 1 ? 1 : 0), static_cast<std::uint8_t>(arg)});
        break;
    case 9:  // SEND_CSD
        respond(0);
        send_data(csd(*image));
        break;
    case 10:  // SEND_CID
        respond(0);
        send_data(cid());
        break;
    case 12:  // STOP_TRANSMISSION: a stuff byte, then R1b
        reading_ = false;
        out_.clear();
        out_pos_ = 0;
        out_.insert(out_.end(), {0xFF, 0x00, 0x00});
        break;
    case 13:  // SEND_STATUS: R2
        respond(0);
        out_.push_back(0x00);
        break;
    case 16:  // SET_BLOCKLEN: SDHC/SDXC stay at 512
        if (!high_capacity(*image) && (arg == 0 || arg > kBlock)) {
            respond(kParameterError);
        } else {
            if (!high_capacity(*image)) {
                block_len_ = arg;
            }
            respond(0);
        }
        break;
    case 17:  // READ_SINGLE_BLOCK
    case 18:  // READ_MULTIPLE_BLOCK
        if (data_offset(arg, image, offset)) {
            respond(0);
            next_offset_ = offset;
            reading_ = true;
            next_read_block(image);
            reading_ = reading_ && index == 18;
        }
        break;
    case 24:  // WRITE_BLOCK
    case 25:  // WRITE_MULTIPLE_BLOCK
        if (!high_capacity(*image) && arg % kBlock != 0) {
            respond(kAddressError);
        } else if (data_offset(arg, image, offset)) {
            respond(0);
            next_offset_ = offset;
            writing_many_ = index == 25;
            phase_ = Phase::WriteToken;
        }
        break;
    case 55:  // APP_CMD
        app_command_ = true;
        respond(idle_bit());
        break;
    case 58: {  // READ_OCR: R3; CCS is valid once the card is ready
        const std::uint32_t ocr = kOcrVoltages | (mode_ == Mode::Ready ? kOcrReady | (high_capacity(*image) ? kOcrCcs : 0) : 0);
        respond(idle_bit());
        out_.insert(out_.end(), {static_cast<std::uint8_t>(ocr >> 24), static_cast<std::uint8_t>(ocr >> 16),
                                 static_cast<std::uint8_t>(ocr >> 8), static_cast<std::uint8_t>(ocr)});
        break;
    }
    case 59:  // CRC_ON_OFF
        crc_on_ = (arg & 1) != 0;
        respond(idle_bit());
        break;
    default:
        respond(idle_bit() | kIllegal);
        break;
    }
}

void SdCard::save(ChunkWriter& w) const {
    w.u8(kVersion);
    w.u8(static_cast<std::uint8_t>(mode_));
    w.u8(static_cast<std::uint8_t>(phase_));
    w.bytes(cmd_);
    w.u8(static_cast<std::uint8_t>(cmd_count_));
    w.boolean(app_command_);
    w.boolean(crc_on_);
    w.u32(init_polls_);
    w.boolean(init_hcs_);
    w.u32(block_len_);
    w.boolean(reading_);
    w.boolean(writing_many_);
    w.u64(next_offset_);
    const auto pending = std::span<const std::uint8_t>(out_).subspan(out_pos_);
    w.u32(static_cast<std::uint32_t>(pending.size()));
    w.bytes(pending);
    w.u32(static_cast<std::uint32_t>(in_.size()));
    w.bytes(in_);
}

void SdCard::load(ChunkReader& r) {
    if (r.u8() != kVersion) {
        r.fail();
        return;
    }
    mode_ = static_cast<Mode>(std::min<unsigned>(r.u8(), static_cast<unsigned>(Mode::Ready)));
    phase_ = static_cast<Phase>(std::min<unsigned>(r.u8(), static_cast<unsigned>(Phase::WriteData)));
    r.bytes(cmd_);
    cmd_count_ = std::min<unsigned>(r.u8(), 5);
    app_command_ = r.boolean();
    crc_on_ = r.boolean();
    init_polls_ = r.u32();
    init_hcs_ = r.boolean();
    block_len_ = std::clamp<std::uint32_t>(r.u32(), 1, kBlock);
    reading_ = r.boolean();
    writing_many_ = r.boolean();
    next_offset_ = r.u64();
    const std::uint32_t pending = r.u32();
    out_.assign(std::min<std::size_t>(pending, kMaxOut), 0);
    if (pending > kMaxOut) {
        r.fail();
    }
    r.bytes(out_);
    out_pos_ = 0;
    const std::uint32_t received = r.u32();
    in_.assign(std::min<std::size_t>(received, kBlock + 2), 0);
    if (received > kBlock + 2) {
        r.fail();
    }
    r.bytes(in_);
}

}  // namespace freetvc
