// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/snapshot/snapshot.hpp"

#include <algorithm>
#include <bit>
#include <cstring>

#include "core/message.hpp"

namespace freetvc {

namespace {

constexpr char kMagic[4] = {'T', 'V', 'C', 'S'};
constexpr std::size_t kMaxChunk = std::size_t{64} << 20;

void put(std::vector<std::uint8_t>& out, std::uint64_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) {
        out.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
}

std::uint64_t get(std::span<const std::uint8_t> b, std::size_t at, int bytes) {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) {
        v |= std::uint64_t{b[at + static_cast<std::size_t>(i)]} << (8 * i);
    }
    return v;
}

std::array<char, 4> make_tag(std::string_view tag) {
    std::array<char, 4> t{' ', ' ', ' ', ' '};
    std::copy_n(tag.begin(), std::min<std::size_t>(tag.size(), 4), t.begin());
    return t;
}

}  // namespace

std::uint32_t crc32(std::span<const std::uint8_t> bytes) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> t{};
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t c = i;
            for (int k = 0; k < 8; ++k) {
                c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            t[i] = c;
        }
        return t;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (const std::uint8_t b : bytes) {
        crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

void ChunkWriter::u16(std::uint16_t v) { put(data, v, 2); }
void ChunkWriter::u32(std::uint32_t v) { put(data, v, 4); }
void ChunkWriter::u64(std::uint64_t v) { put(data, v, 8); }
void ChunkWriter::f32(float v) { u32(std::bit_cast<std::uint32_t>(v)); }
void ChunkWriter::f64(double v) { u64(std::bit_cast<std::uint64_t>(v)); }

void ChunkWriter::text(std::string_view s) {
    u32(static_cast<std::uint32_t>(s.size()));
    data.insert(data.end(), s.begin(), s.end());
}

void ChunkWriter::packed(std::span<const std::uint8_t> b) {
    // PackBits: n in 0-127 copies n + 1 literal bytes, n in 129-255 repeats the next byte 257 - n times
    u32(static_cast<std::uint32_t>(b.size()));
    std::size_t i = 0;
    while (i < b.size()) {
        std::size_t run = 1;
        while (i + run < b.size() && run < 128 && b[i + run] == b[i]) {
            ++run;
        }
        if (run >= 3) {
            data.push_back(static_cast<std::uint8_t>(257 - run));
            data.push_back(b[i]);
            i += run;
            continue;
        }
        std::size_t lit = 0;
        while (i + lit < b.size() && lit < 128) {
            if (i + lit + 2 < b.size() && b[i + lit] == b[i + lit + 1] && b[i + lit] == b[i + lit + 2]) {
                break;
            }
            ++lit;
        }
        data.push_back(static_cast<std::uint8_t>(lit - 1));
        data.insert(data.end(), b.begin() + static_cast<std::ptrdiff_t>(i), b.begin() + static_cast<std::ptrdiff_t>(i + lit));
        i += lit;
    }
}

std::span<const std::uint8_t> ChunkReader::take(std::size_t n) {
    if (failed_ || p_.size() - pos_ < n) {
        failed_ = true;
        return {};
    }
    const auto s = p_.subspan(pos_, n);
    pos_ += n;
    return s;
}

std::uint8_t ChunkReader::u8() {
    const auto s = take(1);
    return s.empty() ? 0 : s[0];
}

std::uint16_t ChunkReader::u16() {
    const auto s = take(2);
    return s.empty() ? 0 : static_cast<std::uint16_t>(get(s, 0, 2));
}

std::uint32_t ChunkReader::u32() {
    const auto s = take(4);
    return s.empty() ? 0 : static_cast<std::uint32_t>(get(s, 0, 4));
}

std::uint64_t ChunkReader::u64() {
    const auto s = take(8);
    return s.empty() ? 0 : get(s, 0, 8);
}

float ChunkReader::f32() { return std::bit_cast<float>(u32()); }
double ChunkReader::f64() { return std::bit_cast<double>(u64()); }

std::string ChunkReader::text(std::size_t max) {
    const std::uint32_t n = u32();
    if (n > max) {
        failed_ = true;
        return {};
    }
    const auto s = take(n);
    return std::string(s.begin(), s.end());
}

void ChunkReader::bytes(std::span<std::uint8_t> out) {
    const auto s = take(out.size());
    if (!s.empty()) {
        std::copy(s.begin(), s.end(), out.begin());
    }
}

std::vector<std::uint8_t> ChunkReader::packed_vector(std::size_t max) {
    const std::uint32_t size = u32();
    std::vector<std::uint8_t> out;
    if (size > max) {
        failed_ = true;
        return out;
    }
    out.reserve(size);
    while (!failed_ && out.size() < size) {
        const std::uint8_t n = u8();
        if (n < 128) {
            const auto s = take(std::size_t{n} + 1);
            out.insert(out.end(), s.begin(), s.end());
        } else if (n > 128) {
            out.insert(out.end(), 257u - n, u8());
        }
        if (out.size() > size) {
            failed_ = true;
        }
    }
    return out;
}

void ChunkReader::packed(std::span<std::uint8_t> out) {
    const std::vector<std::uint8_t> v = packed_vector(out.size());
    if (v.size() != out.size()) {
        failed_ = true;
        return;
    }
    std::copy(v.begin(), v.end(), out.begin());
}

ChunkWriter& Snapshot::add(std::string_view tag, std::uint16_t version) {
    const std::array<char, 4> t = make_tag(tag);
    std::erase_if(chunks_, [&](const SnapshotChunk& c) { return c.tag == t; });
    chunks_.push_back({t, version, {}});
    return chunks_.back().body;
}

const SnapshotChunk* Snapshot::find(std::string_view tag) const {
    const std::array<char, 4> t = make_tag(tag);
    for (const SnapshotChunk& c : chunks_) {
        if (c.tag == t) {
            return &c;
        }
    }
    return nullptr;
}

std::vector<std::uint8_t> Snapshot::encode() const {
    std::vector<std::uint8_t> out(std::begin(kMagic), std::end(kMagic));
    put(out, kFormatVersion, 2);
    for (const SnapshotChunk& c : chunks_) {
        out.insert(out.end(), c.tag.begin(), c.tag.end());
        put(out, c.version, 2);
        put(out, c.body.data.size(), 4);
        out.insert(out.end(), c.body.data.begin(), c.body.data.end());
    }
    return out;
}

std::optional<Snapshot> Snapshot::decode(std::span<const std::uint8_t> file, std::string& error) {
    if (file.size() < 6 || std::memcmp(file.data(), kMagic, 4) != 0) {
        error = msg("not a freeTVC snapshot (no TVCS at the start)");
        return std::nullopt;
    }
    const auto version = static_cast<std::uint16_t>(get(file, 4, 2));
    if (version != kFormatVersion) {
        error = msg("snapshot format version %u is not supported (this build reads %u)", static_cast<unsigned>(version),
                    static_cast<unsigned>(kFormatVersion));
        return std::nullopt;
    }
    Snapshot s;
    std::size_t pos = 6;
    while (pos < file.size()) {
        if (file.size() - pos < 10) {
            error = msg("the snapshot ends inside a chunk header at offset %zu", pos);
            return std::nullopt;
        }
        SnapshotChunk c;
        std::copy_n(file.begin() + static_cast<std::ptrdiff_t>(pos), 4, c.tag.begin());
        c.version = static_cast<std::uint16_t>(get(file, pos + 4, 2));
        const std::size_t length = get(file, pos + 6, 4);
        pos += 10;
        if (length > kMaxChunk || file.size() - pos < length) {
            error = msg("chunk %s runs past the end of the snapshot", std::string(c.tag.begin(), c.tag.end()).c_str());
            return std::nullopt;
        }
        c.body.data.assign(file.begin() + static_cast<std::ptrdiff_t>(pos), file.begin() + static_cast<std::ptrdiff_t>(pos + length));
        pos += length;
        s.chunks_.push_back(std::move(c));
    }
    return s;
}

}  // namespace freetvc
