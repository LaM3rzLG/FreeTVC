// SPDX-License-Identifier: GPL-3.0-or-later
// WinTVC .tvs snapshots, written from hardware/file-formats.md "WinTVC .tvs": an approximation, not byte-exact.
#pragma once

#include <cstdint>
#include <span>
#include <string>

namespace freetvc {

class Machine;

inline constexpr std::size_t kTvsSize = 0x1842F;

// Powers the machine on with its own board and ROMs, then takes the CPU, RAM, VRAM, port and CRTC state from the file.
// The beam starts at a frame start, and a pending IRQ is not carried over. False with `error` set (machine unchanged).
bool import_tvs(Machine& machine, std::span<const std::uint8_t> file, std::string& error);

}  // namespace freetvc
