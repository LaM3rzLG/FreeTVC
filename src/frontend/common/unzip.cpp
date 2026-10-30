// SPDX-License-Identifier: GPL-3.0-or-later
// Written from RFC 1951 (DEFLATE) and PKWARE's APPNOTE (ZIP central directory).
#include "frontend/common/unzip.hpp"

#include <array>

#include "core/snapshot/snapshot.hpp"

namespace freetvc {

namespace {

constexpr unsigned kMaxBits = 15;

// Bits LSB first, as DEFLATE packs them
class BitReader {
public:
    explicit BitReader(std::span<const std::uint8_t> data) : data_(data) {}
    bool failed() const { return failed_; }
    unsigned bits(unsigned n) {
        unsigned v = 0;
        for (unsigned i = 0; i < n; ++i) {
            if (pos_ >= data_.size() * 8) {
                failed_ = true;
                return 0;
            }
            v |= ((data_[pos_ / 8] >> (pos_ % 8)) & 1u) << i;
            ++pos_;
        }
        return v;
    }
    void align() { pos_ = (pos_ + 7) / 8 * 8; }
    std::size_t byte_pos() const { return pos_ / 8; }
    void skip_bytes(std::size_t n) { pos_ += n * 8; }
    std::span<const std::uint8_t> data() const { return data_; }

private:
    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
    bool failed_ = false;
};

// A canonical Huffman code: how many codes of each length, and the symbols in code order
struct Huffman {
    std::array<std::uint16_t, kMaxBits + 1> count{};
    std::vector<std::uint16_t> symbol;

    bool build(std::span<const std::uint8_t> lengths) {
        count.fill(0);
        for (const std::uint8_t l : lengths) {
            ++count[l];
        }
        count[0] = 0;
        std::array<std::uint16_t, kMaxBits + 2> offset{};
        for (unsigned l = 1; l <= kMaxBits; ++l) {
            offset[l + 1] = static_cast<std::uint16_t>(offset[l] + count[l]);
        }
        symbol.assign(lengths.size(), 0);
        for (std::size_t s = 0; s < lengths.size(); ++s) {
            if (lengths[s] != 0) {
                symbol[offset[lengths[s]]++] = static_cast<std::uint16_t>(s);
            }
        }
        return true;
    }

    // Reads one code bit by bit; -1 when none matches
    int decode(BitReader& in) const {
        int code = 0;
        int first = 0;
        int index = 0;
        for (unsigned l = 1; l <= kMaxBits; ++l) {
            code |= static_cast<int>(in.bits(1));
            const int n = count[l];
            if (code - n < first) {
                return symbol[static_cast<std::size_t>(index + (code - first))];
            }
            index += n;
            first = (first + n) << 1;
            code <<= 1;
        }
        return -1;
    }
};

constexpr std::array<std::uint16_t, 29> kLengthBase{3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> kLengthExtra{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, 30> kDistBase{1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                                  193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<std::uint8_t, 30> kDistExtra{0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

bool inflate_codes(BitReader& in, const Huffman& lit, const Huffman& dist, std::vector<std::uint8_t>& out) {
    for (;;) {
        const int sym = lit.decode(in);
        if (sym < 0 || in.failed()) {
            return false;
        }
        if (sym < 256) {
            out.push_back(static_cast<std::uint8_t>(sym));
            continue;
        }
        if (sym == 256) {
            return true;
        }
        const unsigned li = static_cast<unsigned>(sym - 257);
        if (li >= kLengthBase.size()) {
            return false;
        }
        const unsigned length = kLengthBase[li] + in.bits(kLengthExtra[li]);
        const int ds = dist.decode(in);
        if (ds < 0 || static_cast<unsigned>(ds) >= kDistBase.size()) {
            return false;
        }
        const std::size_t distance = kDistBase[static_cast<std::size_t>(ds)] + in.bits(kDistExtra[static_cast<std::size_t>(ds)]);
        if (distance > out.size() || in.failed()) {
            return false;
        }
        for (unsigned k = 0; k < length; ++k) {
            out.push_back(out[out.size() - distance]);
        }
    }
}

bool fixed_block(BitReader& in, std::vector<std::uint8_t>& out) {
    std::array<std::uint8_t, 288> lengths{};
    for (unsigned s = 0; s < 288; ++s) {
        lengths[s] = s < 144 ? 8 : s < 256 ? 9 : s < 280 ? 7 : 8;
    }
    std::array<std::uint8_t, 30> distances{};
    distances.fill(5);
    Huffman lit;
    Huffman dist;
    lit.build(lengths);
    dist.build(distances);
    return inflate_codes(in, lit, dist, out);
}

bool dynamic_block(BitReader& in, std::vector<std::uint8_t>& out) {
    const unsigned nlen = in.bits(5) + 257;
    const unsigned ndist = in.bits(5) + 1;
    const unsigned ncode = in.bits(4) + 4;
    if (nlen > 286 || ndist > 30) {
        return false;
    }
    static constexpr std::array<std::uint8_t, 19> kOrder{16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15};
    std::array<std::uint8_t, 19> code_lengths{};
    for (unsigned i = 0; i < ncode; ++i) {
        code_lengths[kOrder[i]] = static_cast<std::uint8_t>(in.bits(3));
    }
    Huffman lencode;
    lencode.build(code_lengths);
    std::vector<std::uint8_t> lengths(nlen + ndist, 0);
    for (unsigned i = 0; i < nlen + ndist;) {
        const int sym = lencode.decode(in);
        if (sym < 0 || in.failed()) {
            return false;
        }
        if (sym < 16) {
            lengths[i++] = static_cast<std::uint8_t>(sym);
            continue;
        }
        std::uint8_t value = 0;
        unsigned repeat = 0;
        if (sym == 16) {
            if (i == 0) {
                return false;
            }
            value = lengths[i - 1];
            repeat = 3 + in.bits(2);
        } else if (sym == 17) {
            repeat = 3 + in.bits(3);
        } else {
            repeat = 11 + in.bits(7);
        }
        if (i + repeat > nlen + ndist) {
            return false;
        }
        while (repeat-- > 0) {
            lengths[i++] = value;
        }
    }
    if (lengths[256] == 0) {
        return false;  // no end-of-block code
    }
    Huffman lit;
    Huffman dist;
    lit.build(std::span(lengths).first(nlen));
    dist.build(std::span(lengths).subspan(nlen));
    return inflate_codes(in, lit, dist, out);
}

std::uint32_t le32(std::span<const std::uint8_t> b, std::size_t at) {
    return at + 4 <= b.size() ? b[at] | b[at + 1] << 8 | b[at + 2] << 16 | static_cast<std::uint32_t>(b[at + 3]) << 24 : 0;
}

std::uint16_t le16(std::span<const std::uint8_t> b, std::size_t at) {
    return at + 2 <= b.size() ? static_cast<std::uint16_t>(b[at] | b[at + 1] << 8) : 0;
}

}  // namespace

std::optional<std::vector<std::uint8_t>> inflate(std::span<const std::uint8_t> data) {
    BitReader in(data);
    std::vector<std::uint8_t> out;
    for (bool last = false; !last;) {
        last = in.bits(1) != 0;
        const unsigned type = in.bits(2);
        bool ok = false;
        if (type == 0) {
            in.align();
            const std::size_t at = in.byte_pos();
            const unsigned len = le16(data, at);
            if (at + 4 + len > data.size() || static_cast<std::uint16_t>(~len) != le16(data, at + 2)) {
                return std::nullopt;
            }
            out.insert(out.end(), data.begin() + static_cast<std::ptrdiff_t>(at + 4), data.begin() + static_cast<std::ptrdiff_t>(at + 4 + len));
            in.skip_bytes(4 + len);
            ok = true;
        } else if (type == 1) {
            ok = fixed_block(in, out);
        } else if (type == 2) {
            ok = dynamic_block(in, out);
        }
        if (!ok || in.failed()) {
            return std::nullopt;
        }
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> unzip_member(std::span<const std::uint8_t> zip, std::string_view name) {
    // The end-of-central-directory record, searched from the end past a comment of up to 64 KiB
    std::size_t end = std::string_view::npos;
    for (std::size_t at = zip.size() >= 22 ? zip.size() - 22 : 0; zip.size() >= 22; --at) {
        if (le32(zip, at) == 0x06054B50) {
            end = at;
            break;
        }
        if (at == 0 || zip.size() - at > 0x10000 + 22) {
            break;
        }
    }
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    const unsigned entries = le16(zip, end + 10);
    std::size_t at = le32(zip, end + 16);
    for (unsigned i = 0; i < entries && le32(zip, at) == 0x02014B50; ++i) {
        const unsigned method = le16(zip, at + 10);
        const std::uint32_t crc = le32(zip, at + 16);
        const std::size_t packed = le32(zip, at + 20);
        const std::size_t size = le32(zip, at + 24);
        const unsigned name_len = le16(zip, at + 28);
        const std::size_t next = at + 46 + name_len + le16(zip, at + 30) + le16(zip, at + 32);
        const std::size_t local = le32(zip, at + 42);
        if (at + 46 + name_len <= zip.size() &&
            std::string_view(reinterpret_cast<const char*>(zip.data() + at + 46), name_len) == name) {
            const std::size_t data = local + 30 + le16(zip, local + 26) + le16(zip, local + 28);
            if (le32(zip, local) != 0x04034B50 || data + packed > zip.size()) {
                return std::nullopt;
            }
            const std::span<const std::uint8_t> bytes = zip.subspan(data, packed);
            std::optional<std::vector<std::uint8_t>> out;
            if (method == 0) {
                out.emplace(bytes.begin(), bytes.end());
            } else if (method == 8) {
                out = inflate(bytes);
            }
            if (!out || out->size() != size || crc32(*out) != crc) {
                return std::nullopt;
            }
            return out;
        }
        at = next;
    }
    return std::nullopt;
}

}  // namespace freetvc
