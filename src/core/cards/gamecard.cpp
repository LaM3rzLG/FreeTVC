// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/cards/gamecard.hpp"

#include <algorithm>

#include "core/snapshot/snapshot.hpp"

namespace freetvc {

GameCard::GameCard(std::span<const std::uint8_t> rom) {
    rom_.fill(0xFF);
    std::copy_n(rom.begin(), std::min(rom.size(), rom_.size()), rom_.begin());
    power_on();
}

void GameCard::power_on() {
    sound_.power_on();
    reset(0);
}

// The reset line loads xxxx0111 into the control register: page 7 (the boot menu), sound clock stopped (TVC-LOGIC.PLD)
void GameCard::reset(std::uint64_t) {
    select_page(7);
    sound_.stop_clock();
}

void GameCard::select_page(unsigned page) {
    page_ = page & 7;
    std::copy_n(rom_.begin() + static_cast<std::ptrdiff_t>(page_ * kPageSize), kPageSize, window_.begin());
}

// Reads at +2/+3 drive D0-D4 from joystick 3/4 (TVC-JOY.PLD); D5-D7 and every other port are not driven
std::uint8_t GameCard::io_read(unsigned port, const CardBus& bus) {
    if ((port == 2 || port == 3) && bus.keyboard) {
        return static_cast<std::uint8_t>(0xE0 | (bus.keyboard[port == 2 ? 10 : 11] & 0x1F));
    }
    return 0xFF;
}

void GameCard::io_write(unsigned port, std::uint8_t value, const CardBus& bus) {
    if (port == 0 && bus.audio) {
        sound_.write(value, bus.tick, *bus.audio);
    } else if (port == 0x0F) {
        select_page(value);
        // b3 = 1 runs the clock: BattleField and RasterRunner write it before they play (design/cards.md "GameCard")
        if (bus.audio) {
            sound_.set_clock((value & 0x08) != 0, bus.tick, *bus.audio);
        }
    }
}

void GameCard::save(ChunkWriter& w) const {
    w.u8(static_cast<std::uint8_t>(page_));
    sound_.save(w);
}

void GameCard::load(ChunkReader& r) {
    select_page(r.u8());
    sound_.load(r);
}

}  // namespace freetvc
