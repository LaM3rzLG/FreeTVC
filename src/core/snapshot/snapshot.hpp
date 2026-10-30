// SPDX-License-Identifier: GPL-3.0-or-later
// TVCS snapshot files (design/media.md "Snapshots"): a magic, a format version, then tagged chunks.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace freetvc {

std::uint32_t crc32(std::span<const std::uint8_t> bytes);

// Little-endian fields of one chunk.
class ChunkWriter {
public:
    void u8(std::uint8_t v) { data.push_back(v); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void u16(std::uint16_t v);
    void u32(std::uint32_t v);
    void u64(std::uint64_t v);
    void f32(float v);
    void f64(double v);
    void text(std::string_view s);  // u32 length, then the bytes
    void bytes(std::span<const std::uint8_t> b) { data.insert(data.end(), b.begin(), b.end()); }
    // u32 length, then PackBits runs
    void packed(std::span<const std::uint8_t> b);

    std::vector<std::uint8_t> data;
};

// Reads fields in the order they were written; a short or malformed chunk sets failed() and reads zeros.
class ChunkReader {
public:
    explicit ChunkReader(std::span<const std::uint8_t> payload) : p_(payload) {}
    std::uint8_t u8();
    bool boolean() { return u8() != 0; }
    std::uint16_t u16();
    std::uint32_t u32();
    std::uint64_t u64();
    float f32();
    double f64();
    std::string text(std::size_t max = 4096);
    void bytes(std::span<std::uint8_t> out);
    // Exactly out.size() bytes after unpacking
    void packed(std::span<std::uint8_t> out);
    std::vector<std::uint8_t> packed_vector(std::size_t max);
    bool failed() const { return failed_; }
    // For a field this version cannot read
    void fail() { failed_ = true; }
    bool at_end() const { return pos_ == p_.size(); }

private:
    std::span<const std::uint8_t> take(std::size_t n);
    std::span<const std::uint8_t> p_;
    std::size_t pos_ = 0;
    bool failed_ = false;
};

struct SnapshotChunk {
    std::array<char, 4> tag{};
    std::uint16_t version = 1;
    ChunkWriter body;
};

class Snapshot {
public:
    static constexpr std::uint16_t kFormatVersion = 1;

    // A new chunk (an existing one with the tag is replaced); the writer stays valid until the next add()
    ChunkWriter& add(std::string_view tag, std::uint16_t version = 1);
    // nullptr when the chunk is missing
    const SnapshotChunk* find(std::string_view tag) const;
    const std::vector<SnapshotChunk>& chunks() const { return chunks_; }

    // "TVCS", u16 format version, then per chunk: tag, u16 version, u32 length, payload
    std::vector<std::uint8_t> encode() const;
    static std::optional<Snapshot> decode(std::span<const std::uint8_t> file, std::string& error);

private:
    std::vector<SnapshotChunk> chunks_;
};

}  // namespace freetvc
