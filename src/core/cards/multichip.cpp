// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/cards/multichip.hpp"

#include <algorithm>

#include "core/snapshot/snapshot.hpp"

namespace freetvc {

namespace {

constexpr std::uint8_t kSaveVersion = 1;
// Levels per volt at the chips (❓): the YM3014B's full scale, VDD / 4 = 1.25 V, is 0.53 (−3 dB, set by listening)
constexpr float kPerVolt = 0.53f / 1.25f;
constexpr float kYmScale = kPerVolt * 1.25f / 32768.0f;
// The schematic's mixers: 22k inputs for the SN and the YM, 12k for each SAA side, 22k feedback; the TV gets both sides' mean
constexpr float kSaaGain = 22.0f / 12.0f / 2.0f;
// A full SAA channel sinks about Iref (data sheet: 250 uA) through the card's 1k loads
constexpr float kSaaScale = kPerVolt * 0.25f * kSaaGain / Saa1099::kChannelFull;

}  // namespace

MultichipCard::MultichipCard(std::span<const std::uint8_t> rom) {
    rom_.fill(0xFF);
    std::copy_n(rom.begin(), std::min(rom.size(), rom_.size()), rom_.begin());
    saa_.set_scale(kSaaScale);
    ym_.set_scale(kYmScale);
    power_on();
}

void MultichipCard::power_on() {
    sn_.power_on();
    sn_.mute_all();
    sn_.start_clock(0);
    saa_.power_on();
    ym_.power_on();
    select_bank(3);
}

// /RESET presets the bank latch to 11 (74HCT74 /PRE) and drives the YM's /IC; the SN and the SAA have no reset pin
void MultichipCard::reset(std::uint64_t tick) {
    select_bank(3);
    ym_.reset(tick);
}

void MultichipCard::select_bank(unsigned bank) {
    bank_ = bank & 3;
    std::copy_n(rom_.begin() + static_cast<std::ptrdiff_t>(bank_ * kBankSize), kBankSize, window_.begin());
}

// A 74HCT139 decodes A2-A1 (A3 is ignored): SN, SAA, YM, bank latch; only the YM drives the data bus on reads
std::uint8_t MultichipCard::io_read(unsigned port, const CardBus& bus) {
    if ((port & 6) == 4 && bus.audio) {
        return ym_.read(port & 1, bus.tick, *bus.audio);
    }
    return 0xFF;
}

void MultichipCard::io_write(unsigned port, std::uint8_t value, const CardBus& bus) {
    if (!bus.audio) {
        if ((port & 6) == 6) {
            select_bank(value);
        }
        return;
    }
    switch (port & 7) {
    case 0:
    case 1:
        sn_.write(value, bus.tick, *bus.audio);
        break;
    case 2:
        saa_.write_data(value, bus.tick, *bus.audio);
        break;
    case 3:
        saa_.write_address(value, bus.tick, *bus.audio);
        break;
    case 4:
    case 5:
        ym_.write(port & 1, value, bus.tick, *bus.audio);
        break;
    default:
        select_bank(value);
        break;
    }
}

void MultichipCard::advance_audio(std::uint64_t tick, AudioSynth& out) {
    sn_.advance_to(tick, out);
    saa_.advance_to(tick, out);
    ym_.advance_to(tick, out);
}

void MultichipCard::run_events(const CardBus& bus) {
    if (bus.audio) {
        ym_.advance_to(bus.tick, *bus.audio);
    }
}

void MultichipCard::save(ChunkWriter& w) const {
    w.u8(kSaveVersion);
    w.u8(static_cast<std::uint8_t>(bank_));
    sn_.save(w);
    sn_.save_phase(w);
    saa_.save(w);
    ym_.save(w);
}

void MultichipCard::load(ChunkReader& r) {
    const std::uint8_t version = r.u8();
    if (version < 1 || version > kSaveVersion) {
        r.fail();  // a layout this version does not know
        return;
    }
    select_bank(r.u8());
    sn_.load(r);
    sn_.load_phase(r);
    saa_.load(r);
    ym_.load(r);
}

}  // namespace freetvc
