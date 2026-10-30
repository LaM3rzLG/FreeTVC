// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/bus/jedec_flash.hpp"

#include <algorithm>

#include "core/audio/synth.hpp"
#include "core/snapshot/snapshot.hpp"

namespace freetvc {

namespace {

std::uint64_t ticks(std::uint32_t us) { return AudioSynth::kTickRate * us / 1'000'000; }

}  // namespace

void JedecFlash::power_on() {
    step_ = Step::Ready;
    id_mode_ = false;
    busy_until_ = 0;
    final_dq7_ = 0;
    toggle_ = false;
}

std::uint8_t JedecFlash::read(std::uint32_t address, std::uint64_t tick, std::span<const std::uint8_t> bytes) {
    if (busy(tick)) {
        // Data# polling on DQ7, the toggle bit on DQ6; the other outputs are not valid ❓ (read as 0)
        toggle_ = !toggle_;
        return static_cast<std::uint8_t>((~final_dq7_ & 0x80) | (toggle_ ? 0x40 : 0));
    }
    if (id_mode_) {
        // Table 3-1 reads them with AMS-A1 = 0; elsewhere A0 alone is assumed ❓
        return (address & 1) ? spec_->device_id : spec_->maker_id;
    }
    return address < bytes.size() ? bytes[address] : 0xFF;
}

bool JedecFlash::write(std::uint32_t address, std::uint8_t value, std::uint64_t tick, std::span<std::uint8_t> bytes) {
    if (busy(tick)) {
        return false;  // commands during an internal operation are ignored (§3.2-3.4)
    }
    // An invalid cycle aborts to read mode (§3.10); it may itself start a new sequence
    const auto restart = [&] {
        if (id_mode_ && value == 0xF0) {
            id_mode_ = false;
        }
        step_ = at(address, spec_->unlock1) && value == 0xAA ? Step::Unlock1 : Step::Ready;
    };
    const auto start = [&](std::uint64_t ticks, std::uint8_t dq7) {
        busy_until_ = tick + ticks;
        final_dq7_ = dq7;
        toggle_ = false;
    };
    switch (step_) {
    case Step::Ready:
        restart();
        return false;
    case Step::Unlock1:
        if (at(address, spec_->unlock2) && value == 0x55) {
            step_ = Step::Unlock2;
        } else {
            restart();
        }
        return false;
    case Step::Unlock2:
        step_ = Step::Ready;
        if (!at(address, spec_->unlock1)) {
            restart();
        } else if (value == 0xA0) {
            step_ = Step::Program;
        } else if (value == 0x80) {
            step_ = Step::Erase;
        } else if (value == 0x90) {
            id_mode_ = true;
        } else if (value == 0xF0) {
            id_mode_ = false;
        } else {
            restart();
        }
        return false;
    case Step::Program: {
        step_ = Step::Ready;
        if (address >= bytes.size()) {
            return false;
        }
        // Programming only clears bits; on an unerased byte the cells keep their zeros ❓
        const std::uint8_t old = bytes[address];
        bytes[address] = static_cast<std::uint8_t>(old & value);
        start(ticks(spec_->program_us), bytes[address]);
        return bytes[address] != old;
    }
    case Step::Erase:
        if (at(address, spec_->unlock1) && value == 0xAA) {
            step_ = Step::EraseUnlock1;
        } else {
            restart();
        }
        return false;
    case Step::EraseUnlock1:
        if (at(address, spec_->unlock2) && value == 0x55) {
            step_ = Step::EraseUnlock2;
        } else {
            restart();
        }
        return false;
    case Step::EraseUnlock2: {
        step_ = Step::Ready;
        std::size_t from = 0;
        std::size_t count = 0;
        if (value == 0x30) {
                        from = address & ~(spec_->sector_size - 1);
            count = spec_->sector_size;
            start(ticks(spec_->sector_erase_us), 0xFF);
        } else if (value == 0x10 && at(address, spec_->unlock1)) {
            count = bytes.size();
            start(ticks(spec_->chip_erase_us), 0xFF);
        } else {
            restart();
            return false;
        }
        if (from >= bytes.size()) {
            return false;
        }
        const auto first = bytes.begin() + static_cast<std::ptrdiff_t>(from);
        const auto last = first + static_cast<std::ptrdiff_t>(std::min(count, bytes.size() - from));
        const bool changed = std::any_of(first, last, [](std::uint8_t b) { return b != 0xFF; });
        std::fill(first, last, std::uint8_t{0xFF});
        return changed;
    }
    }
    return false;
}

void JedecFlash::save(ChunkWriter& w) const {
    w.u8(static_cast<std::uint8_t>(step_));
    w.boolean(id_mode_);
    w.u64(busy_until_);
    w.u8(final_dq7_);
    w.boolean(toggle_);
}

void JedecFlash::load(ChunkReader& r) {
    step_ = static_cast<Step>(std::min<unsigned>(r.u8(), static_cast<unsigned>(Step::EraseUnlock2)));
    id_mode_ = r.boolean();
    busy_until_ = r.u64();
    final_dq7_ = r.u8();
    toggle_ = r.boolean();
}

}  // namespace freetvc
