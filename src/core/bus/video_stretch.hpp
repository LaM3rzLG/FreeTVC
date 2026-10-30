// SPDX-License-Identifier: GPL-3.0-or-later
// VRAM clock stretch: extra half-T ticks for a CPU memory cycle in VID (tvc-hardware.md §2, [HW] fig. 20-22).
#pragma once

#include <array>
#include <cstdint>

namespace freetvc {

// DEB phase of a machine tick: ticks % 4. Phase 0 = CPU slot start (DEB rising), 2 = CRTC slot start (DEB falling).
// Tick 0 at power-on is a CPU slot start; the real power-on phase is unknown (❓, deterministic either way).
constexpr unsigned deb_phase(std::uint64_t tick) { return static_cast<unsigned>(tick & 3); }

// Indexed by the DEB phase at T1. Memory read/write: the T3 falling edge moves to 160 ns after DEB falls.
inline constexpr std::array<std::uint8_t, 4> kStretchReadWrite{2, 1, 4, 3};
// Opcode fetch (M1): the T3 rising edge moves to 160 ns after DEB falls.
inline constexpr std::array<std::uint8_t, 4> kStretchM1{3, 2, 1, 4};

}  // namespace freetvc
