// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/media/tape_signal.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "core/message.hpp"

namespace freetvc {

namespace {

// Half cycles in ticks (tvc-hardware.md §8): "0" 552 µs, "1" 388 µs, leader 470 µs, sync 736 µs; high half first
constexpr std::uint32_t kZeroHalf = 1725;
constexpr std::uint32_t kOneHigh = 1213;
constexpr std::uint32_t kOneLow = 1212;
constexpr std::uint32_t kLeaderHigh = 1469;
constexpr std::uint32_t kLeaderLow = 1469;
constexpr std::uint32_t kSyncHalf = 2300;
constexpr unsigned kHeaderLeader = 10240;
constexpr unsigned kDataLeader = 5120;
constexpr unsigned kTrailer = 5;

constexpr std::uint8_t kHeaderBlock = 0xFF;
constexpr std::uint8_t kDataBlock = 0x00;
constexpr std::uint8_t kBuffered = 0x01;
constexpr std::uint8_t kUnbuffered = 0x11;

void cycle(TapeSignal& s, std::uint32_t high, std::uint32_t low) {
    s.append(true, high);
    s.append(false, low);
}

void put16(std::vector<std::uint8_t>& out, unsigned v) {
    out.push_back(static_cast<std::uint8_t>(v));
    out.push_back(static_cast<std::uint8_t>(v >> 8));
}

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    put16(out, v & 0xFFFF);
    put16(out, v >> 16);
}

std::uint32_t get16(std::span<const std::uint8_t> b, std::size_t at) { return static_cast<std::uint32_t>(b[at] | b[at + 1] << 8); }
std::uint32_t get32(std::span<const std::uint8_t> b, std::size_t at) { return get16(b, at) | get16(b, at + 2) << 16; }

// A sector at the end of `block`; the first sector's CRC starts at 6Ah, a later one's at its sector number
void add_sector(TapeBlock& block, std::uint8_t number, std::span<const std::uint8_t> data, bool last, bool first) {
    const std::size_t crc_from = first ? 1 : block.size();
    block.push_back(number);
    block.push_back(static_cast<std::uint8_t>(data.size() == 256 ? 0 : data.size()));
    block.insert(block.end(), data.begin(), data.end());
    block.push_back(last ? 0xFF : 0x00);
    put16(block, tape_crc(0, std::span<const std::uint8_t>(block).subspan(crc_from)));
}

TapeBlock block_start(std::uint8_t type, bool buffered, bool protect, unsigned sectors) {
    return {0x00, 0x6A, type, buffered ? kBuffered : kUnbuffered, static_cast<std::uint8_t>(protect ? 0xFF : 0x00),
            static_cast<std::uint8_t>(sectors)};
}

}  // namespace

std::uint64_t TapeSignal::length() const {
    std::uint64_t total = 0;
    for (const std::uint32_t r : runs) {
        total += r;
    }
    return total;
}

void TapeSignal::append(bool level, std::uint64_t ticks) {
    if (ticks == 0) {
        return;
    }
    if (runs.empty()) {
        first_level = level;
    } else if ((first_level != ((runs.size() - 1) % 2 == 1)) == level && runs.back() + ticks <= 0xFFFFFFFFu) {
        runs.back() += static_cast<std::uint32_t>(ticks);
        return;
    }
    while (ticks > 0xFFFFFFFFu) {  // a level held longer than 11 minutes: split into runs of the same level
        runs.push_back(0xFFFFFFFFu);
        runs.push_back(0);
        ticks -= 0xFFFFFFFFu;
    }
    runs.push_back(static_cast<std::uint32_t>(ticks));
}

std::uint16_t tape_crc(std::uint16_t crc, std::span<const std::uint8_t> bytes) {
    for (const std::uint8_t byte : bytes) {
        for (int bit = 0; bit < 8; ++bit) {
            const std::uint8_t a = static_cast<std::uint8_t>(((byte >> bit) & 1) << 7 ^ (crc >> 8));
            if (a & 0x80) {
                crc ^= 0x0810;
            }
            crc = static_cast<std::uint16_t>(crc << 1);
            if (a & 0x80) {
                ++crc;
            }
        }
    }
    return crc;
}

TapeProgram cas_program(const CasFile& cas, std::string_view name) {
    TapeProgram p;
    p.name = tvc_file_name(name);
    p.buffered = cas.buffered;  // the protect byte is unreliable in CAS files (design/media.md) and is left off
    if (cas.buffered) {
        p.data = cas.stream;
    } else {
        const auto header = cas.header();
        const auto block = cas.block();
        p.header.assign(header.begin(), header.end());
        // The program header's length is what the ROM reads; some CAS files carry more bytes after it (MAME cuts too)
        const std::size_t length = header.size() >= 4 ? static_cast<std::size_t>(header[2] | header[3] << 8) : 0;
        p.data.assign(block.begin(), block.begin() + static_cast<std::ptrdiff_t>(length > 0 && length < block.size() ? length : block.size()));
    }
    return p;
}

std::vector<TapeBlock> tape_blocks(const TapeProgram& p) {
    std::vector<TapeBlock> blocks;
    std::vector<std::uint8_t> head;
    const std::string name = p.name.substr(0, 16);
    head.push_back(static_cast<std::uint8_t>(name.size()));
    head.insert(head.end(), name.begin(), name.end());
    if (!p.buffered) {
        head.insert(head.end(), p.header.begin(), p.header.end());
    }
    TapeBlock header = block_start(kHeaderBlock, p.buffered, p.protect, 1);
    add_sector(header, 0, head, false, true);  // MAME and cas2wav: "no last sector" in the header block
    blocks.push_back(std::move(header));

    const std::span<const std::uint8_t> data(p.data);
    if (p.buffered) {
        // ⚑ one 256-byte sector per block, numbered from 1 (tvc-hardware.md §8); checked against the ROM's SAVE
        const std::size_t count = std::max<std::size_t>(1, (data.size() + 255) / 256);
        for (std::size_t i = 0; i < count; ++i) {
            TapeBlock block = block_start(kDataBlock, true, p.protect, 1);
            const std::size_t at = std::min(i * 256, data.size());
            add_sector(block, static_cast<std::uint8_t>(i + 1), data.subspan(at, std::min<std::size_t>(256, data.size() - at)), i + 1 == count, true);
            blocks.push_back(std::move(block));
        }
        return blocks;
    }
    const std::size_t sectors = std::max<std::size_t>(1, (data.size() + 255) / 256);
    TapeBlock block = block_start(kDataBlock, false, p.protect, static_cast<unsigned>(sectors & 0xFF));
    for (std::size_t i = 0; i < sectors; ++i) {
        const std::size_t at = i * 256;
        add_sector(block, static_cast<std::uint8_t>(i + 1), data.subspan(at, std::min<std::size_t>(256, data.size() - at)), i + 1 == sectors,
                   i == 0);
    }
    blocks.push_back(std::move(block));
    return blocks;
}

TapeSignal encode_tape(std::span<const TapeProgram> programs) {
    TapeSignal s;
    for (const TapeProgram& p : programs) {
        const std::vector<TapeBlock> blocks = tape_blocks(p);
        for (std::size_t i = 0; i < blocks.size(); ++i) {
            s.append(false, (i == 0 ? 2 : 1) * kTapeTicksPerSecond);
            for (unsigned n = 0; n < (i == 0 ? kHeaderLeader : kDataLeader); ++n) {
                cycle(s, kLeaderHigh, kLeaderLow);
            }
            cycle(s, kSyncHalf, kSyncHalf);
            for (const std::uint8_t byte : blocks[i]) {
                for (int bit = 0; bit < 8; ++bit) {
                    if ((byte >> bit) & 1) {
                        cycle(s, kOneHigh, kOneLow);
                    } else {
                        cycle(s, kZeroHalf, kZeroHalf);
                    }
                }
            }
            if (i + 1 == blocks.size()) {
                s.program_ends.push_back(s.length());  // the ROM has its last byte here; the trailer follows
            }
            for (unsigned n = 0; n < kTrailer; ++n) {
                cycle(s, kLeaderHigh, kLeaderLow);
            }
        }
        s.program_autorun.push_back(p.autorun());
        s.program_data.push_back(p.data);
        if (s.name.empty()) {
            s.name = p.name;
        }
    }
    s.append(false, kTapeTicksPerSecond);
    return s;
}

std::optional<std::vector<TapeProgram>> parse_ttp(std::span<const std::uint8_t> b, std::string& error) {
    if (b.size() < 4 || std::memcmp(b.data(), "TTP0", 4) != 0) {
        error = msg("not a TTP file (no TTP0 at the start)");
        return std::nullopt;
    }
    std::vector<TapeProgram> programs;
    std::size_t pos = 4;
    while (pos < b.size()) {
        if (pos + 6 > b.size() || b[pos] != 0x00 || b[pos + 1] != 0x6A) {
            error = msg("no block start (00h 6Ah) at offset %zu", pos);
            return std::nullopt;
        }
        const std::uint8_t type = b[pos + 2];
        const bool buffered = b[pos + 3] == kBuffered;
        const unsigned sectors = b[pos + 5] == 0 ? 256 : b[pos + 5];
        std::size_t p = pos + 6;
        std::vector<std::uint8_t> data;
        for (unsigned s = 0; s < sectors; ++s) {
            if (p + 2 > b.size()) {
                error = msg("the file ends inside a sector at offset %zu", p);
                return std::nullopt;
            }
            const std::size_t from = s == 0 ? pos + 1 : p;
            const unsigned count = b[p + 1];
            bool matched = false;
            for (const std::size_t length : {std::size_t{count == 0 ? 256u : count}, std::size_t{count == 0 ? 257u : 0u}}) {
                const std::size_t eof = p + 2 + length;
                if (length == 0 || eof + 3 > b.size()) {
                    continue;
                }
                if (tape_crc(0, b.subspan(from, eof + 1 - from)) == get16(b, eof + 1)) {
                    data.insert(data.end(), b.begin() + static_cast<std::ptrdiff_t>(p + 2), b.begin() + static_cast<std::ptrdiff_t>(eof));
                    p = eof + 3;
                    matched = true;
                    break;
                }
            }
            if (!matched) {
                error = msg("CRC error in sector %u at offset %zu", s, p);
                return std::nullopt;
            }
        }
        if (type == kHeaderBlock) {
            if (data.empty() || data[0] + 1u > data.size()) {
                error = msg("a header block without a name at offset %zu", pos);
                return std::nullopt;
            }
            TapeProgram program;
            program.buffered = buffered;
            program.name.assign(data.begin() + 1, data.begin() + 1 + data[0]);
            if (!buffered) {
                program.header.assign(data.begin() + 1 + data[0], data.end());
            }
            programs.push_back(std::move(program));
        } else if (programs.empty()) {
            error = msg("a data block before any header block at offset %zu", pos);
            return std::nullopt;
        } else {
            programs.back().data.insert(programs.back().data.end(), data.begin(), data.end());
        }
        pos = p;
    }
    // cas2ttp reads one byte past every full sector; the program header's length says where the program ends
    for (TapeProgram& program : programs) {
        if (!program.buffered && program.header.size() >= 4) {
            const std::size_t length = get16(program.header, 2);
            if (length > 0 && program.data.size() > length) {
                program.data.resize(length);
            }
        }
    }
    if (programs.empty()) {
        error = msg("no blocks after TTP0");
        return std::nullopt;
    }
    return programs;
}

std::optional<TapeSignal> wav_signal(std::span<const std::uint8_t> b, std::string& error, double hysteresis) {
    if (b.size() < 12 || std::memcmp(b.data(), "RIFF", 4) != 0 || std::memcmp(b.data() + 8, "WAVE", 4) != 0) {
        error = msg("not a RIFF WAVE file");
        return std::nullopt;
    }
    unsigned channels = 0;
    unsigned rate = 0;
    unsigned bits = 0;
    std::span<const std::uint8_t> data;
    for (std::size_t pos = 12; pos + 8 <= b.size();) {
        const std::size_t size = get32(b, pos + 4);
        const std::size_t body = pos + 8;
        const std::size_t end = std::min(b.size(), body + size);
        if (std::memcmp(b.data() + pos, "fmt ", 4) == 0 && end - body >= 16) {
            const unsigned format = get16(b, body);
            if (format != 1 && format != 0xFFFE) {
                error = msg("only PCM WAV files are read (format %u)", format);
                return std::nullopt;
            }
            channels = get16(b, body + 2);
            rate = get32(b, body + 4);
            bits = get16(b, body + 14);
        } else if (std::memcmp(b.data() + pos, "data", 4) == 0) {
            data = b.subspan(body, end - body);
        }
        pos = body + size + (size & 1);
    }
    if (channels == 0 || rate == 0 || (bits != 8 && bits != 16) || data.empty()) {
        error = msg("a WAV file needs 8- or 16-bit PCM samples and a data chunk");
        return std::nullopt;
    }
    const std::size_t frame = channels * bits / 8;
    const std::size_t frames = data.size() / frame;
    std::vector<float> samples(frames);
    double sum = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        const std::size_t at = i * frame;
        samples[i] = bits == 8 ? (static_cast<float>(data[at]) - 128.0f) / 128.0f
                               : static_cast<float>(static_cast<std::int16_t>(get16(data, at))) / 32768.0f;
        sum += samples[i];
    }
    const float mean = frames ? static_cast<float>(sum / static_cast<double>(frames)) : 0.0f;
    const auto h = static_cast<float>(hysteresis);
    TapeSignal s;
    const double ticks_per_sample = static_cast<double>(kTapeTicksPerSecond) / rate;
    bool level = !samples.empty() && samples[0] > mean;
    std::uint64_t last_edge = 0;
    std::size_t old_side = 0;  // last sample on the current level's side of the mean
    for (std::size_t i = 0; i < frames; ++i) {
        const bool next = samples[i] > mean + h ? true : samples[i] < mean - h ? false : level;
        if (next != level) {
            // The edge where the mean is crossed, between two samples: whole samples would add up to half a sample of jitter
            const float before = samples[old_side];
            const float after = samples[old_side + 1];
            const double frac = after != before ? std::clamp(static_cast<double>((mean - before) / (after - before)), 0.0, 1.0) : 1.0;
            const auto at = std::llround((static_cast<double>(old_side) + frac) * ticks_per_sample);
            const auto edge = std::max(static_cast<std::uint64_t>(at), last_edge);
            s.append(level, edge - last_edge);
            last_edge = edge;
            level = next;
        }
        if ((samples[i] > mean) == level) {
            old_side = i;
        }
    }
    s.append(level, static_cast<std::uint64_t>(std::llround(static_cast<double>(frames) * ticks_per_sample)) - last_edge);
    return s;
}

std::vector<std::uint8_t> signal_wav(const TapeSignal& signal, unsigned rate) {
    std::vector<std::int16_t> samples;
    std::uint64_t edge = 0;
    bool level = signal.first_level;
    for (const std::uint32_t run : signal.runs) {
        edge += run;
        samples.resize(static_cast<std::size_t>(edge * rate / kTapeTicksPerSecond), level ? std::int16_t{16000} : std::int16_t{-16000});
        level = !level;
    }
    std::vector<std::uint8_t> out;
    const auto bytes = static_cast<std::uint32_t>(samples.size() * 2);
    out.insert(out.end(), {'R', 'I', 'F', 'F'});
    put32(out, 36 + bytes);
    out.insert(out.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(out, 16);
    put16(out, 1);
    put16(out, 1);
    put32(out, rate);
    put32(out, rate * 2);
    put16(out, 2);
    put16(out, 16);
    out.insert(out.end(), {'d', 'a', 't', 'a'});
    put32(out, bytes);
    for (const std::int16_t v : samples) {
        put16(out, static_cast<std::uint16_t>(v));
    }
    return out;
}

std::vector<TapeBlock> decode_tape(const TapeSignal& signal) {
    // Full cycles from one rising edge to the next, in µs: "1" < 429 < leader < 511 < "0" < 644 < sync < 1000
    enum Kind { Noise, One, Leader, Zero, Sync, Gap };
    const auto classify = [](std::uint64_t ticks) {
        const double us = static_cast<double>(ticks) * 1e6 / static_cast<double>(kTapeTicksPerSecond);
        return us < 250 ? Noise : us < 429 ? One : us < 511 ? Leader : us < 644 ? Zero : us < 1000 ? Sync : Gap;
    };
    std::vector<TapeBlock> blocks;
    std::vector<std::uint8_t> bits;
    bool reading = false;
    unsigned leaders = 0;
    const auto end_block = [&] {
        if (reading && bits.size() >= 8) {
            TapeBlock block;
            for (std::size_t i = 0; i + 8 <= bits.size(); i += 8) {
                std::uint8_t byte = 0;
                for (int b = 0; b < 8; ++b) {
                    byte = static_cast<std::uint8_t>(byte | bits[i + static_cast<std::size_t>(b)] << b);
                }
                block.push_back(byte);
            }
            blocks.push_back(std::move(block));
        }
        bits.clear();
        reading = false;
    };
    std::uint64_t time = 0;
    std::uint64_t last_rise = 0;
    bool have_rise = false;
    bool level = signal.first_level;
    for (const std::uint32_t run : signal.runs) {
        time += run;
        level = !level;
        if (!level) {
            continue;  // a falling edge
        }
        if (have_rise) {
            const Kind kind = classify(time - last_rise);
            if (reading && (kind == One || kind == Zero)) {
                bits.push_back(kind == One ? 1 : 0);
            } else {
                end_block();
                if (kind == Sync && leaders >= 64) {
                    reading = true;
                }
                leaders = kind == Leader ? leaders + 1 : 0;
            }
        }
        last_rise = time;
        have_rise = true;
    }
    end_block();
    return blocks;
}

}  // namespace freetvc
