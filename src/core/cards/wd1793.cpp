// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/cards/wd1793.hpp"

#include <algorithm>

#include "core/cards/floppy.hpp"
#include "core/snapshot/snapshot.hpp"

namespace freetvc {

namespace {

constexpr std::uint8_t kBusy = 0x01;
constexpr std::uint8_t kIndex = 0x02;  // type I
constexpr std::uint8_t kDrq = 0x02;    // types II and III
constexpr std::uint8_t kTrack0 = 0x04;
constexpr std::uint8_t kLostData = 0x04;
constexpr std::uint8_t kSeekError = 0x10;
constexpr std::uint8_t kNotFound = 0x10;
constexpr std::uint8_t kHeadLoaded = 0x20;
constexpr std::uint8_t kWriteProtect = 0x40;
constexpr std::uint8_t kNotReady = 0x80;

constexpr std::uint64_t kB = Wd1793::kByteTicks;
constexpr std::uint64_t kMs = Wd1793::kTicksPerMs;
constexpr std::uint64_t kStepMs[4] = {6, 12, 20, 30};  // at 1 MHz
constexpr std::uint64_t kSettle = 30 * kMs;            // head settle and the E delay, at 1 MHz
constexpr std::uint64_t kIndexWidth = 4 * kMs;
constexpr unsigned kSectorSize = FloppyGeometry::kSectorSize;

// Track layout (IBM System 34 MFM): gap 4a, sync, index mark, gap 1, then per sector sync, ID, gap 2, sync, data, gap 3
constexpr unsigned kTrackHead = 146;
constexpr unsigned kSectorBlock = 654;
constexpr unsigned kIdEnd = 7;      // after the ID mark: C, H, R, N, two CRC bytes
constexpr unsigned kDataStart = 45;  // after the ID mark: CRC, gap 2, sync, A1 A1 A1 FB

std::uint16_t crc_ccitt(std::uint16_t crc, std::uint8_t byte) {
    crc ^= static_cast<std::uint16_t>(byte << 8);
    for (int i = 0; i < 8; ++i) {
        crc = (crc & 0x8000) ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021) : static_cast<std::uint16_t>(crc << 1);
    }
    return crc;
}

std::uint16_t mark_crc(std::uint8_t mark, const std::uint8_t* bytes, std::size_t n) {
    std::uint16_t crc = 0xFFFF;
    for (int i = 0; i < 3; ++i) {
        crc = crc_ccitt(crc, 0xA1);
    }
    crc = crc_ccitt(crc, mark);
    for (std::size_t i = 0; i < n; ++i) {
        crc = crc_ccitt(crc, bytes[i]);
    }
    return crc;
}

}  // namespace

void Wd1793::reset() {
    const auto cylinders = cylinders_;
    const int drive = drive_;
    const unsigned side = side_;
    const bool motor = motor_;
    *this = Wd1793{};
    // The drives and the card's latch are not the controller's
    cylinders_ = cylinders;
    drive_ = drive;
    side_ = side;
    motor_ = motor;
}

bool Wd1793::ready(FloppyDrives* drives) const { return drives && drive_ >= 0 && drives->disk(static_cast<unsigned>(drive_)); }

bool Wd1793::spinning(FloppyDrives* drives) const { return motor_ && ready(drives); }

bool Wd1793::index_hole(std::uint64_t tick, FloppyDrives* drives) const { return spinning(drives) && tick % kRevolutionTicks < kIndexWidth; }

bool Wd1793::write_protected(FloppyDrives* drives) const {
    return ready(drives) && drives->write_protected(static_cast<unsigned>(drive_));
}

unsigned Wd1793::track_sectors(FloppyDrives* drives) const {
    if (!spinning(drives)) {
        return 0;
    }
    const FloppyGeometry& g = drives->disk(static_cast<unsigned>(drive_))->geometry();
    return cylinders_[static_cast<unsigned>(drive_)] < g.tracks && side_ < g.heads ? g.sectors : 0;
}

unsigned Wd1793::id_mark(unsigned i, unsigned sectors) const {
    const unsigned spacing = std::min(kSectorBlock, (kTrackBytes - kTrackHead - 100) / std::max(1u, sectors));
    return kTrackHead + i * spacing + 15;
}

std::uint64_t Wd1793::next_pass(std::uint64_t t, unsigned byte) const {
    std::uint64_t at = t - t % kRevolutionTicks + byte * kB;
    if (at < t) {
        at += kRevolutionTicks;
    }
    return at;
}

std::uint64_t Wd1793::fifth_index(std::uint64_t t) const { return next_pass(t, 0) + 4 * kRevolutionTicks; }

std::uint8_t Wd1793::status(std::uint64_t tick, FloppyDrives* drives) {
    std::uint8_t s = ready(drives) ? 0 : kNotReady;
    if (type1_) {
        if (write_protected(drives)) {
            s |= kWriteProtect;
        }
        // The head unloads after 15 idle revolutions
        if (head_loaded_ && !busy_ && tick - std::min(tick, head_used_) > 15 * kRevolutionTicks) {
            head_loaded_ = false;
        }
        if (head_loaded_) {
            s |= kHeadLoaded;
        }
        s |= errors_ & 0x18;
        if (drive_ >= 0 && cylinders_[static_cast<unsigned>(drive_)] == 0) {
            s |= kTrack0;
        }
        if (index_hole(tick, drives)) {
            s |= kIndex;
        }
    } else {
        s |= errors_ & 0x7C;
        if (drq_) {
            s |= kDrq;
        }
    }
    return busy_ ? static_cast<std::uint8_t>(s | kBusy) : s;
}

std::uint8_t Wd1793::read(unsigned reg, std::uint64_t tick, FloppyDrives* drives) {
    advance(tick, drives);
    switch (reg & 3) {
    case 0: {
        const std::uint8_t s = status(tick, drives);
        if (!intrq_held_) {
            intrq_ = false;
        }
        return s;
    }
    case 1:
        return track_;
    case 2:
        return sector_;
    default:
        drq_ = false;
        return data_;
    }
}

void Wd1793::write(unsigned reg, std::uint8_t value, std::uint64_t tick, FloppyDrives* drives) {
    advance(tick, drives);
    switch (reg & 3) {
    case 0:
        if ((value & 0xF0) == 0xD0) {
            // Force interrupt: ends any command; idle, the status turns to the type I layout
            if (busy_) {
                busy_ = false;
                phase_ = Phase::None;
                drq_ = false;
            } else {
                type1_ = true;
            }
            command_ = value;
            index_interrupt_ = 0;
            if (value & 0x08) {
                intrq_ = true;
                intrq_held_ = true;
            } else if ((value & 0x04) && spinning(drives)) {
                index_interrupt_ = std::max<std::uint64_t>(1, next_pass(tick + 1, 0));
            } else if ((value & 0x0F) == 0) {
                intrq_ = false;
                intrq_held_ = false;
            }
        } else if (!busy_) {
            start(value, tick, drives);
        }
        break;
    case 1:
        if (!busy_) {
            track_ = value;
        }
        break;
    case 2:
        if (!busy_) {
            sector_ = value;
        }
        break;
    default:
        data_ = value;
        drq_ = false;
        break;
    }
}

bool Wd1793::drq(std::uint64_t tick, FloppyDrives* drives) {
    advance(tick, drives);
    return drq_;
}

bool Wd1793::intrq(std::uint64_t tick, FloppyDrives* drives) {
    advance(tick, drives);
    return intrq_;
}

void Wd1793::select(int drive, unsigned side, bool motor, std::uint64_t tick, FloppyDrives* drives) {
    advance(tick, drives);
    drive_ = drive >= 0 && drive < static_cast<int>(cylinders_.size()) ? drive : -1;
    side_ = side & 1;
    motor_ = motor;
}

void Wd1793::advance(std::uint64_t tick, FloppyDrives* drives) {
    while (phase_ != Phase::None && next_ <= tick) {
        run_event(next_, drives);
    }
    if (index_interrupt_ != 0 && tick >= index_interrupt_) {
        intrq_ = true;
        index_interrupt_ = 0;
    }
}

void Wd1793::start(std::uint8_t command, std::uint64_t tick, FloppyDrives* drives) {
    command_ = command;
    intrq_ = false;
    intrq_held_ = false;
    index_interrupt_ = 0;
    errors_ = 0;
    pending_ = 0;
    drq_ = false;
    busy_ = true;
    head_used_ = tick;
    steps_ = 0;
    count_ = 0;
    next_ = tick;
    touch(tick, drives);
    if (!(command & 0x80)) {
        // Type I: restore, seek, step, step in, step out; h loads the head
        type1_ = true;
        head_loaded_ = (command & 0x08) != 0;
        phase_ = Phase::Step;
        if ((command >> 4) == 0) {
            track_ = 0xFF;
            data_ = 0;
        }
        return;
    }
    type1_ = false;
    if (!ready(drives)) {
        finish(tick, 0);
        return;
    }
    head_loaded_ = true;
    const bool writes = (command & 0xE0) == 0xA0 || (command & 0xF0) == 0xF0;
    if (writes && write_protected(drives)) {
        finish(tick, kWriteProtect);
        return;
    }
    next_ = tick + ((command & 0x04) ? kSettle : 0);
    if ((command & 0xE0) == 0xE0) {
        // Read track and write track start at the index hole; write track asks for its first byte at once
        phase_ = Phase::WaitIndex;
        if (command & 0x10) {
            drq_ = true;
        }
        if (spinning(drives)) {
            next_ = next_pass(next_, 0);
        } else {
            next_ = ~std::uint64_t{0};  // no index pulse ever comes; only a force interrupt ends it
        }
        return;
    }
    phase_ = Phase::SearchId;
    deadline_ = fifth_index(next_);
}

void Wd1793::finish(std::uint64_t t, std::uint8_t error_bits) {
    errors_ |= error_bits;
    busy_ = false;
    intrq_ = true;
    phase_ = Phase::None;
    head_used_ = t;
}

void Wd1793::step_once() {
    if (drive_ >= 0) {
        std::uint8_t& c = cylinders_[static_cast<unsigned>(drive_)];
        const int next = std::clamp<int>(c + direction_, 0, static_cast<int>(kCylinders) - 1);
        c = static_cast<std::uint8_t>(next);
    }
}

void Wd1793::search(std::uint64_t t, FloppyDrives* drives) {
    const unsigned sectors = track_sectors(drives);
    const unsigned cylinder = drive_ >= 0 ? cylinders_[static_cast<unsigned>(drive_)] : 0;
    const bool read_address = (command_ & 0xF0) == 0xC0;
    std::uint64_t best = ~std::uint64_t{0};
    for (unsigned i = 0; i < sectors; ++i) {
        if (!read_address) {
            // The ID must carry the track and sector registers, and the S flag's side when C asks to compare
            const bool side_ok = !(command_ & 0x02) || ((command_ >> 3) & 1) == side_;
            if (cylinder != track_ || i + 1 != sector_ || !side_ok) {
                continue;
            }
        }
        const std::uint64_t at = next_pass(t, id_mark(i, sectors));
        if (at < best) {
            best = at;
            found_ = i;
        }
    }
    if (best > deadline_) {
        pending_ = kNotFound;
        phase_ = Phase::Finish;
        next_ = deadline_;
        return;
    }
    DiskImage* disk = drives->disk(static_cast<unsigned>(drive_));
    count_ = 0;
    if (read_address) {
        buffer_ = {static_cast<std::uint8_t>(cylinder), static_cast<std::uint8_t>(side_), static_cast<std::uint8_t>(found_ + 1), 2, 0, 0};
        const std::uint16_t crc = mark_crc(0xFE, buffer_.data(), 4);
        buffer_[4] = static_cast<std::uint8_t>(crc >> 8);
        buffer_[5] = static_cast<std::uint8_t>(crc);
        phase_ = Phase::ReadAddress;
        next_ = best + kB;
    } else if (command_ & 0x20) {
        phase_ = Phase::WriteFirst;
        next_ = best + (kIdEnd + 2) * kB;
    } else {
        const std::uint8_t* bytes = disk->sector(cylinder, side_, found_ + 1);
        buffer_.assign(bytes, bytes + kSectorSize);
        phase_ = Phase::ReadData;
        next_ = best + kDataStart * kB;
    }
}

void Wd1793::touch(std::uint64_t t, FloppyDrives* drives) const {
    if (drives && drive_ >= 0) {
        drives->touch(static_cast<unsigned>(drive_), t);
    }
}

void Wd1793::run_event(std::uint64_t t, FloppyDrives* drives) {
    // Every step, byte and command end counts as activity (the drive light, fast disk, the idle save)
    touch(t, drives);
    switch (phase_) {
    case Phase::Step: {
        const unsigned kind = command_ >> 4;
        const std::uint64_t rate = kStepMs[command_ & 3] * kMs;
        bool done = false;
        if (kind == 0) {
            // Restore: step out until track 0 is seen, at most 255 pulses
            if (drive_ >= 0 && cylinders_[static_cast<unsigned>(drive_)] == 0) {
                track_ = 0;
                done = true;
            } else if (steps_ >= 255) {
                finish(t, kSeekError);
                return;
            } else {
                direction_ = -1;
            }
        } else if (kind == 1) {
            if (track_ == data_) {
                done = true;
            } else {
                direction_ = data_ > track_ ? 1 : -1;
                track_ = static_cast<std::uint8_t>(track_ + direction_);
            }
        } else if (steps_ > 0) {
            done = true;
        } else {
            if (kind >= 4) {
                direction_ = kind < 6 ? 1 : -1;
            }
            if (command_ & 0x10) {
                track_ = static_cast<std::uint8_t>(track_ + direction_);
            }
        }
        // Stepping out with TR00 active: the track register becomes 0 and no pulse is issued (FD179X data sheet p. 9)
        if (!done && kind != 0 && direction_ < 0 && drive_ >= 0 && cylinders_[static_cast<unsigned>(drive_)] == 0) {
            track_ = 0;
            done = true;
        }
        if (!done) {
            step_once();
            ++steps_;
            next_ = t + rate;
            return;
        }
        if (command_ & 0x04) {
            phase_ = Phase::Settle;
            next_ = t + kSettle;
        } else {
            finish(t, 0);
        }
        return;
    }
    case Phase::Settle: {
        // Verify: an ID on this track must carry the track register within five index pulses
        head_loaded_ = true;
        const unsigned sectors = track_sectors(drives);
        deadline_ = fifth_index(t);
        if (sectors > 0 && cylinders_[static_cast<unsigned>(drive_)] == track_) {
            std::uint64_t best = ~std::uint64_t{0};
            for (unsigned i = 0; i < sectors; ++i) {
                best = std::min(best, next_pass(t, id_mark(i, sectors) + kIdEnd));
            }
            pending_ = 0;
            next_ = best;
        } else {
            pending_ = kSeekError;
            next_ = deadline_;
        }
        phase_ = Phase::Finish;
        return;
    }
    case Phase::SearchId:
        search(t, drives);
        return;
    case Phase::ReadData:
        if (drq_) {
            errors_ |= kLostData;
        }
        data_ = buffer_[count_++];
        drq_ = true;
        if (count_ < kSectorSize) {
            next_ = t + kB;
        } else {
            phase_ = Phase::Finish;
            next_ = t + 3 * kB;
        }
        return;
    case Phase::WriteFirst:
        if (count_ == 0) {
            drq_ = true;
            count_ = 1;
            next_ = t + 20 * kB;
            return;
        }
        if (drq_) {
            drq_ = false;
            finish(t, kLostData);
            return;
        }
        buffer_.assign(kSectorSize, 0);
        buffer_[0] = data_;
        drq_ = true;
        count_ = 1;
        phase_ = Phase::WriteData;
        next_ = t + 17 * kB;
        return;
    case Phase::WriteData:
        if (drq_) {
            errors_ |= kLostData;
            buffer_[count_++] = 0;
        } else {
            buffer_[count_++] = data_;
        }
        if (count_ < kSectorSize) {
            drq_ = true;
            next_ = t + kB;
            return;
        }
        drq_ = false;
        if (DiskImage* disk = ready(drives) ? drives->disk(static_cast<unsigned>(drive_)) : nullptr) {
            if (std::uint8_t* bytes = disk->sector(cylinders_[static_cast<unsigned>(drive_)], side_, found_ + 1)) {
                std::copy(buffer_.begin(), buffer_.end(), bytes);
                drives->mark_written(static_cast<unsigned>(drive_));
            }
        }
        phase_ = Phase::Finish;
        next_ = t + 3 * kB;
        return;
    case Phase::ReadAddress:
        if (drq_) {
            errors_ |= kLostData;
        }
        data_ = buffer_[count_++];
        drq_ = true;
        if (count_ < 6) {
            next_ = t + kB;
        } else {
            sector_ = buffer_[0];
            phase_ = Phase::Finish;
            next_ = t + kB;
        }
        return;
    case Phase::WaitIndex:
        count_ = 0;
        if (command_ & 0x10) {
            if (drq_) {
                drq_ = false;
                finish(t, kLostData);
                return;
            }
            buffer_.assign(kTrackBytes, 0x4E);
            buffer_[count_++] = data_;
            drq_ = true;
            phase_ = Phase::WriteTrack;
        } else {
            build_track(drives);
            phase_ = Phase::ReadTrack;
            next_ = t;
            return;
        }
        next_ = t + kB;
        return;
    case Phase::ReadTrack:
        if (drq_) {
            errors_ |= kLostData;
        }
        data_ = buffer_[count_++];
        drq_ = true;
        if (count_ < kTrackBytes) {
            next_ = t + kB;
        } else {
            pending_ = 0;
            phase_ = Phase::Finish;
            next_ = t + kB;
        }
        return;
    case Phase::WriteTrack:
        if (drq_) {
            errors_ |= kLostData;
            buffer_[count_++] = 0;
        } else {
            buffer_[count_++] = data_;
        }
        if (count_ < kTrackBytes) {
            drq_ = true;
            next_ = t + kB;
            return;
        }
        drq_ = false;
        write_track_data(drives);
        pending_ = 0;
        phase_ = Phase::Finish;
        next_ = t;
        return;
    case Phase::Finish:
        // A multiple-record read or write goes on with the next sector until one is not found
        if (!type1_ && (command_ & 0xD0) == 0x90 && pending_ == 0 && !(errors_ & kLostData)) {
            sector_ = static_cast<std::uint8_t>(sector_ + 1);
            phase_ = Phase::SearchId;
            deadline_ = fifth_index(t);
            next_ = t;
            return;
        }
        finish(t, pending_);
        return;
    case Phase::None:
        return;
    }
}

void Wd1793::build_track(FloppyDrives* drives) {
    buffer_.assign(kTrackBytes, 0x4E);
    const unsigned sectors = track_sectors(drives);
    std::size_t at = 80;
    const auto put = [&](std::uint8_t b, unsigned n) {
        for (unsigned i = 0; i < n && at < buffer_.size(); ++i) {
            buffer_[at++] = b;
        }
    };
    put(0x00, 12);
    put(0xC2, 3);
    put(0xFC, 1);
    if (sectors == 0) {
        return;
    }
    const unsigned cylinder = cylinders_[static_cast<unsigned>(drive_)];
    const DiskImage* disk = drives->disk(static_cast<unsigned>(drive_));
    for (unsigned i = 0; i < sectors; ++i) {
        at = id_mark(i, sectors) - 15;
        put(0x00, 12);
        put(0xA1, 3);
        put(0xFE, 1);
        const std::uint8_t id[4] = {static_cast<std::uint8_t>(cylinder), static_cast<std::uint8_t>(side_), static_cast<std::uint8_t>(i + 1), 2};
        for (std::uint8_t b : id) {
            put(b, 1);
        }
        const std::uint16_t id_crc = mark_crc(0xFE, id, 4);
        put(static_cast<std::uint8_t>(id_crc >> 8), 1);
        put(static_cast<std::uint8_t>(id_crc), 1);
        put(0x4E, 22);
        put(0x00, 12);
        put(0xA1, 3);
        put(0xFB, 1);
        const std::uint8_t* data = disk->sector(cylinder, side_, i + 1);
        for (unsigned k = 0; k < kSectorSize; ++k) {
            put(data[k], 1);
        }
        const std::uint16_t data_crc = mark_crc(0xFB, data, kSectorSize);
        put(static_cast<std::uint8_t>(data_crc >> 8), 1);
        put(static_cast<std::uint8_t>(data_crc), 1);
    }
}

void Wd1793::write_track_data(FloppyDrives* drives) {
    DiskImage* disk = ready(drives) ? drives->disk(static_cast<unsigned>(drive_)) : nullptr;
    if (!disk || side_ >= disk->geometry().heads) {
        return;  // a side the image does not have keeps nothing ❓
    }
    const unsigned cylinder = cylinders_[static_cast<unsigned>(drive_)];
    if (cylinder >= disk->geometry().tracks && !disk->grow_to(cylinder)) {
        return;
    }
    // F5 writes A1 and F6 writes C2 with missing clocks, F7 the CRC; an FE mark starts an ID, F8-FB a data field
    bool have_id = false;
    std::uint8_t id[4] = {};
    for (std::size_t i = 0; i < buffer_.size();) {
        const std::uint8_t b = buffer_[i];
        if (b == 0xFE && i + 4 < buffer_.size()) {
            std::copy_n(buffer_.begin() + static_cast<std::ptrdiff_t>(i + 1), 4, id);
            have_id = true;
            i += 5;
        } else if (b >= 0xF8 && b <= 0xFB && have_id) {
            const std::size_t length = std::size_t{128} << (id[3] & 3);
            if (length == kSectorSize) {
                if (std::uint8_t* bytes = disk->sector(cylinder, side_, id[2])) {
                    const std::size_t n = std::min(length, buffer_.size() - i - 1);
                    std::copy_n(buffer_.begin() + static_cast<std::ptrdiff_t>(i + 1), n, bytes);
                }
            }
            have_id = false;
            i += 1 + length;
        } else {
            ++i;
        }
    }
    drives->mark_written(static_cast<unsigned>(drive_));
}

void Wd1793::save(ChunkWriter& w) const {
    w.u8(command_);
    w.u8(track_);
    w.u8(sector_);
    w.u8(data_);
    w.u8(errors_);
    w.boolean(type1_);
    w.boolean(busy_);
    w.boolean(drq_);
    w.boolean(intrq_);
    w.boolean(intrq_held_);
    w.u64(index_interrupt_);
    w.boolean(head_loaded_);
    w.u64(head_used_);
    w.u8(static_cast<std::uint8_t>(direction_));
    w.u8(static_cast<std::uint8_t>(drive_ + 1));
    w.u8(static_cast<std::uint8_t>(side_));
    w.boolean(motor_);
    w.bytes(cylinders_);
    w.u8(static_cast<std::uint8_t>(phase_));
    w.u64(next_);
    w.u64(deadline_);
    w.u32(steps_);
    w.u32(count_);
    w.u32(found_);
    w.u8(pending_);
    w.packed(buffer_);
}

void Wd1793::load(ChunkReader& r) {
    command_ = r.u8();
    track_ = r.u8();
    sector_ = r.u8();
    data_ = r.u8();
    errors_ = r.u8();
    type1_ = r.boolean();
    busy_ = r.boolean();
    drq_ = r.boolean();
    intrq_ = r.boolean();
    intrq_held_ = r.boolean();
    index_interrupt_ = r.u64();
    head_loaded_ = r.boolean();
    head_used_ = r.u64();
    direction_ = static_cast<std::int8_t>(r.u8());
    drive_ = static_cast<int>(r.u8() & 7) - 1;
    if (drive_ > 3) {
        drive_ = -1;
    }
    side_ = r.u8() & 1u;
    motor_ = r.boolean();
    r.bytes(cylinders_);
    const std::uint8_t phase = r.u8();
    phase_ = phase <= static_cast<std::uint8_t>(Phase::Finish) ? static_cast<Phase>(phase) : Phase::None;
    next_ = r.u64();
    deadline_ = r.u64();
    steps_ = r.u32();
    count_ = r.u32();
    found_ = r.u32();
    pending_ = r.u8();
    buffer_ = r.packed_vector(kTrackBytes);
    // An event reading past the buffer would be a damaged snapshot
    if ((phase_ == Phase::ReadData || phase_ == Phase::WriteData) && buffer_.size() < kSectorSize) {
        phase_ = Phase::None;
        busy_ = false;
    }
    if ((phase_ == Phase::ReadTrack || phase_ == Phase::WriteTrack) && buffer_.size() < kTrackBytes) {
        phase_ = Phase::None;
        busy_ = false;
    }
}

}  // namespace freetvc
