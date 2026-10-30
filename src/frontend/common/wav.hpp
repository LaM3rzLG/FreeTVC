// SPDX-License-Identifier: GPL-3.0-or-later
// WAV output for the front ends: 16-bit PCM mono.
#pragma once

#include <span>
#include <string>

namespace freetvc {

// Writes float samples (clipped to -1..1) as a 16-bit mono WAV; returns false and sets error on failure.
bool write_wav(const std::string& path, std::span<const float> samples, unsigned rate, std::string& error);

}  // namespace freetvc
