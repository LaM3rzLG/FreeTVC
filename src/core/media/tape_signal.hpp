// SPDX-License-Identifier: GPL-3.0-or-later
// Tape signals (design/media.md "Tape deck"): the ROM's tape format (hardware/tvc-hardware.md §8) built from CAS and
// TTP files and encoded as level runs, WAV in and out, and a decoder that finds the blocks again.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/media/cas.hpp"

namespace freetvc {

inline constexpr std::uint64_t kTapeTicksPerSecond = 6'250'000;  // machine ticks (half-T, 160 ns)

// A tape as runs of one level each, in machine ticks.
struct TapeSignal {
    bool first_level = false;           // the level of runs[0]
    std::vector<std::uint32_t> runs;
    // Encoded tapes only: where each program's data block ends, its program header's autorun flag, and its bytes,
    // which a correct load leaves in RAM unchanged.
    std::vector<std::uint64_t> program_ends;
    std::vector<bool> program_autorun;
    std::vector<std::vector<std::uint8_t>> program_data;
    std::string name;                   // the first program's name, when known

    std::uint64_t length() const;
    // Adds a run, merged with the last one when the level is the same.
    void append(bool level, std::uint64_t ticks);
};

// One file as the ROM writes it: name, buffered or not, and for an unbuffered one its 16-byte program header.
struct TapeProgram {
    std::string name;
    bool buffered = false;
    bool protect = false;
    std::vector<std::uint8_t> header;  // unbuffered only
    std::vector<std::uint8_t> data;
    bool autorun() const { return !buffered && header.size() > 4 && header[4] != 0; }
};

// One block's bytes as written to tape, from the leading 00h, 6Ah through the last CRC.
using TapeBlock = std::vector<std::uint8_t>;

// The ROM's CRC over `bytes`, LSB first per byte (tvc-hardware.md §8).
std::uint16_t tape_crc(std::uint16_t crc, std::span<const std::uint8_t> bytes);

TapeProgram cas_program(const CasFile& cas, std::string_view name);
// TTP0 followed by the ROM's byte stream; full sectors of 256 bytes or, as cas2ttp writes them, 257 (the CRC decides).
std::optional<std::vector<TapeProgram>> parse_ttp(std::span<const std::uint8_t> bytes, std::string& error);

// The header block and the data blocks of one program.
std::vector<TapeBlock> tape_blocks(const TapeProgram& program);
// Silence, leader, sync, the bytes LSB first and the trailer for every block, as cas2wav and MAME write them.
TapeSignal encode_tape(std::span<const TapeProgram> programs);

// PCM WAV, 8 or 16 bit, first channel; a comparator with hysteresis (a fraction of full scale) around the mean level.
std::optional<TapeSignal> wav_signal(std::span<const std::uint8_t> bytes, std::string& error, double hysteresis = 0.04);
// A square wave, 16 bit mono.
std::vector<std::uint8_t> signal_wav(const TapeSignal& signal, unsigned rate = 44100);

// The blocks found in a signal by the length of each full cycle, as the ROM's reader classifies them.
std::vector<TapeBlock> decode_tape(const TapeSignal& signal);

}  // namespace freetvc
