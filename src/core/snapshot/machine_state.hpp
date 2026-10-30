// SPDX-License-Identifier: GPL-3.0-or-later
// What a TVCS snapshot needs before Machine::load_state(): the board, the ROMs by CRC-32, the cartridge and the cards.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "core/machine/config.hpp"
#include "core/snapshot/snapshot.hpp"

namespace freetvc {

struct SnapshotNeeds {
    Model model = Model::Tvc64kPlus;
    bool colour = true;
    unsigned audio_rate = 0;
    std::uint32_t sys_crc = 0;
    std::uint32_t ext_crc = 0;
    std::vector<std::uint8_t> cart;  // the image itself: cartridges are embedded
    CartType cart_type = CartType::Plain;
    unsigned cart_bank = 0;
    std::array<CardConfig, kCardSlots> cards{};  // from the SLOT chunk; card ROMs are embedded like the cartridge
    SidCardConfig sid;  // from the SIDC chunk; none without it
};

// From the CONF chunk; nullopt with `error` set when it is missing or damaged.
std::optional<SnapshotNeeds> snapshot_needs(const Snapshot& snapshot, std::string& error);
// True when `config` has the snapshot's board, colour switch, ROM images, cartridge and cards (the audio rate may differ).
bool snapshot_matches(const MachineConfig& config, const SnapshotNeeds& needs);
// `config` with the snapshot's board, colour switch, cartridge and cards; the system ROMs stay as they are.
MachineConfig apply_needs(MachineConfig config, const SnapshotNeeds& needs);

}  // namespace freetvc
