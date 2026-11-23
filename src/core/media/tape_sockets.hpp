// SPDX-License-Identifier: GPL-3.0-or-later
// The two tape sockets (CH12, CH13): their data lines are shared, each has its own motor bit. The deck sits in
// whichever socket TVCnet leaves free; a plugged-in TVCnet drives the shared input (design/media.md).
#pragma once

#include "core/machine/tape_port.hpp"
#include "core/media/tape_deck.hpp"
#include "core/media/tape_tvcnet.hpp"

namespace freetvc {

class TapeSockets final : public TapePort {
public:
    explicit TapeSockets(TapeDeck& deck) : deck_(deck) {}

    // null unplugs TVCnet
    void plug(TapeTvcnet* tvcnet);
    TapeTvcnet* tvcnet() const { return tvcnet_; }

    bool tape_input(std::uint64_t tick) override;
    void tape_output(std::uint64_t tick, bool level) override { deck_.tape_output(tick, level); }
    void tape_motor(std::uint64_t tick, std::uint8_t motors) override;

private:
    TapeDeck& deck_;
    TapeTvcnet* tvcnet_ = nullptr;
};

}  // namespace freetvc
