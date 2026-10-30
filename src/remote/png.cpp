// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote/png.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <span>

#include "debug/symbols.hpp"

namespace freetvc::remote {

namespace {

void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>(v >> shift));
    }
}

void chunk(std::vector<std::uint8_t>& out, const char* type, const std::vector<std::uint8_t>& data) {
    put32(out, static_cast<std::uint32_t>(data.size()));
    const std::size_t start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data.begin(), data.end());
    put32(out, debug::crc32(std::span(out).subspan(start)));
}

// Deflate bits go least significant first; Huffman codes most significant first (RFC 1951 3.1.1)
class BitWriter {
public:
    void put(std::uint32_t bits, unsigned count) {
        acc_ |= static_cast<std::uint64_t>(bits) << used_;
        used_ += count;
        while (used_ >= 8) {
            out.push_back(static_cast<std::uint8_t>(acc_));
            acc_ >>= 8;
            used_ -= 8;
        }
    }
    void put_code(std::uint32_t code, unsigned length) {
        std::uint32_t reversed = 0;
        for (unsigned i = 0; i < length; ++i) {
            reversed |= ((code >> i) & 1u) << (length - 1 - i);
        }
        put(reversed, length);
    }
    void flush() {
        if (used_ > 0) {
            out.push_back(static_cast<std::uint8_t>(acc_));
            acc_ = 0;
            used_ = 0;
        }
    }
    std::vector<std::uint8_t> out;

private:
    std::uint64_t acc_ = 0;
    unsigned used_ = 0;
};

constexpr std::array<std::uint16_t, 29> kLengthBase{3,  4,  5,  6,  7,  8,  9,  10, 11,  13,  15,  17,  19,  23, 27,
                                                    31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258};
constexpr std::array<std::uint8_t, 29> kLengthExtra{0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
constexpr std::array<std::uint16_t, 30> kDistanceBase{1,   2,   3,   4,   5,   7,    9,    13,   17,   25,   33,   49,   65,    97,    129,
                                                      193, 257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
constexpr std::array<std::uint8_t, 30> kDistanceExtra{0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};

// A literal/length symbol in the fixed Huffman code (RFC 1951 3.2.6)
void symbol(BitWriter& w, unsigned v) {
    if (v < 144) {
        w.put_code(0x30 + v, 8);
    } else if (v < 256) {
        w.put_code(0x190 + (v - 144), 9);
    } else if (v < 280) {
        w.put_code(v - 256, 7);
    } else {
        w.put_code(0xC0 + (v - 280), 8);
    }
}

void match(BitWriter& w, unsigned length, unsigned distance) {
    unsigned l = static_cast<unsigned>(kLengthBase.size()) - 1;
    while (kLengthBase[l] > length) {
        --l;
    }
    symbol(w, 257 + l);
    w.put(length - kLengthBase[l], kLengthExtra[l]);
    unsigned d = static_cast<unsigned>(kDistanceBase.size()) - 1;
    while (kDistanceBase[d] > distance) {
        --d;
    }
    w.put_code(d, 5);
    w.put(distance - kDistanceBase[d], kDistanceExtra[d]);
}

// One final block with the fixed codes: greedy LZ77 over hash chains (TVC pictures are long runs and repeated rows)
std::vector<std::uint8_t> deflate(std::span<const std::uint8_t> data) {
    constexpr std::size_t kWindow = 32768;
    constexpr unsigned kMinMatch = 3;
    constexpr std::size_t kMaxMatch = 258;
    constexpr unsigned kMaxChain = 64;
    constexpr unsigned kHashBits = 15;
    const std::size_t n = data.size();
    std::vector<std::int64_t> head(std::size_t{1} << kHashBits, -1);
    std::vector<std::int64_t> previous(kWindow, -1);
    const auto hash = [&](std::size_t p) {
        return (static_cast<std::size_t>(data[p]) << 10 ^ static_cast<std::size_t>(data[p + 1]) << 5 ^ data[p + 2]) & ((1u << kHashBits) - 1);
    };
    const auto insert = [&](std::size_t p) {
        if (p + 2 < n) {
            const std::size_t h = hash(p);
            previous[p % kWindow] = head[h];
            head[h] = static_cast<std::int64_t>(p);
        }
    };
    BitWriter w;
    w.put(1, 1);  // BFINAL
    w.put(1, 2);  // BTYPE 01: fixed codes
    std::size_t i = 0;
    while (i < n) {
        std::size_t best = 0;
        std::size_t distance = 0;
        if (i + kMinMatch <= n) {
            const std::size_t limit = std::min(kMaxMatch, n - i);
            std::int64_t candidate = head[hash(i)];
            for (unsigned chain = 0; chain < kMaxChain && candidate >= 0; ++chain) {
                const auto c = static_cast<std::size_t>(candidate);
                if (c >= i || i - c > kWindow) {
                    break;  // a slot reused by a newer position, or out of reach
                }
                std::size_t length = 0;
                while (length < limit && data[c + length] == data[i + length]) {
                    ++length;
                }
                if (length > best) {
                    best = length;
                    distance = i - c;
                    if (length == limit) {
                        break;
                    }
                }
                const std::int64_t next = previous[c % kWindow];
                if (next >= candidate) {
                    break;
                }
                candidate = next;
            }
        }
        if (best >= kMinMatch) {
            match(w, static_cast<unsigned>(best), static_cast<unsigned>(distance));
            for (std::size_t k = 0; k < best; ++k) {
                insert(i + k);
            }
            i += best;
        } else {
            symbol(w, data[i]);
            insert(i);
            ++i;
        }
    }
    symbol(w, 256);
    w.flush();
    return std::move(w.out);
}

}  // namespace

std::vector<std::uint8_t> encode_png(const Framebuffer& frame, ScreenArea area) {
    area.x = std::clamp(area.x, 0, Framebuffer::kWidth);
    area.y = std::clamp(area.y, 0, Framebuffer::kHeight);
    area.width = std::clamp(area.width, 0, Framebuffer::kWidth - area.x);
    area.height = std::clamp(area.height, 0, Framebuffer::kHeight - area.y);

    // Scanlines of palette indexes with filter type 0
    std::vector<std::uint8_t> raw;
    raw.reserve(static_cast<std::size_t>(area.height) * (1 + static_cast<std::size_t>(area.width)));
    for (int y = 0; y < area.height; ++y) {
        raw.push_back(0);
        for (int x = 0; x < area.width; ++x) {
            const std::size_t at = static_cast<std::size_t>(area.y + y) * Framebuffer::kWidth + static_cast<std::size_t>(area.x + x);
            const std::uint8_t index = frame.pixels[at];
            raw.push_back(index < Framebuffer::kColours ? index : 0);
        }
    }

    // zlib stream (RFC 1950): header, one deflate block, Adler-32
    std::vector<std::uint8_t> z{0x78, 0x01};
    const std::vector<std::uint8_t> compressed = deflate(raw);
    z.insert(z.end(), compressed.begin(), compressed.end());
    std::uint32_t a = 1;
    std::uint32_t b = 0;
    for (const std::uint8_t byte : raw) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    put32(z, b << 16 | a);

    std::vector<std::uint8_t> png{0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    std::vector<std::uint8_t> header;
    put32(header, static_cast<std::uint32_t>(area.width));
    put32(header, static_cast<std::uint32_t>(area.height));
    header.insert(header.end(), {8, 3, 0, 0, 0});  // 8-bit palette indexes, deflate, no filter choice, no interlace
    std::vector<std::uint8_t> palette;
    for (const std::uint32_t rgb : kRgbPalette) {
        palette.insert(palette.end(), {static_cast<std::uint8_t>(rgb >> 16), static_cast<std::uint8_t>(rgb >> 8), static_cast<std::uint8_t>(rgb)});
    }
    chunk(png, "IHDR", header);
    chunk(png, "PLTE", palette);
    chunk(png, "IDAT", z);
    chunk(png, "IEND", {});
    return png;
}

bool write_png(const std::string& path, const Framebuffer& frame, ScreenArea area, std::string& error) {
    const std::vector<std::uint8_t> png = encode_png(frame, area);
    std::FILE* f = std::fopen(path.c_str(), "wb");
    if (!f) {
        error = "cannot write '" + path + "'";
        return false;
    }
    const bool ok = std::fwrite(png.data(), 1, png.size(), f) == png.size();
    std::fclose(f);
    if (!ok) {
        error = "cannot write '" + path + "'";
    }
    return ok;
}

}  // namespace freetvc::remote
