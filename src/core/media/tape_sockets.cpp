// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/media/tape_sockets.hpp"

namespace freetvc {

void TapeSockets::plug(TapeTvcnet* tvcnet) {
    if (tvcnet == tvcnet_) {
        return;
    }
    tvcnet_ = tvcnet;
    if (tvcnet_) {
        tvcnet_->reset();
    }
}

bool TapeSockets::tape_input(std::uint64_t tick) {
    const bool deck = deck_.tape_input(tick);
    return tvcnet_ ? tvcnet_->input(tick) : deck;
}

void TapeSockets::tape_motor(std::uint64_t tick, std::uint8_t motors) {
    deck_.tape_motor(tick, tvcnet_ ? static_cast<std::uint8_t>(motors & ~tvcnet_->socket()) : motors);
    if (tvcnet_) {
        tvcnet_->motor(tick, motors);
    }
}

}  // namespace freetvc
