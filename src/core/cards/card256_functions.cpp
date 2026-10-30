// SPDX-License-Identifier: GPL-3.0-or-later
// TVC256k++ functions of registers C0h/C6h (design/cards.md "TVC256k++", [FW] tvc-routines.c).
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <numbers>
#include <vector>

#include "core/cards/card256.hpp"

namespace freetvc {

namespace {

constexpr std::uint32_t kFastLimit = 256 * 1024;      // the 16 pages the functions address
constexpr std::uint32_t kPsramLimit = 8192 * 1024;
constexpr std::uint32_t kPsramFlag = 0x800000;

// Parameter sizes from [FW] tvc-routines.c init_routines (2836ff.); 80h+ sizes are computed (getFunctionParamSize)
constexpr std::uint8_t kParamSize[42] = {0, 0, 1, 1, 2, 0x80, 0x80, 0, 1, 0, 2, 7, 9, 9, 9, 0x86, 8, 12, 12, 6, 10,
                                         0, 1, 2, 2, 8, 6, 6, 0x82, 2, 6, 6, 9, 18, 2, 0, 0, 0x87, 13, 7, 3, 14};

// Parameter sizes of the file functions 81h-93h ([FW] init_routines)
constexpr std::uint8_t kFileParamSize[19] = {0x83, 4, 6, 0x84, 0x81, 4, 4, 10, 10, 8, 0, 0x81, 0x81, 0x81, 0x85, 0x81, 4, 0x81, 0};

// The storage functions (card256_storage.cpp): 15, 34-36 and the file functions
bool storage_function(std::uint8_t code) { return code == 15 || (code >= 34 && code <= 36) || (code >= 0x81 && code <= 0x93); }

// The memory functions, the text and drawing functions (card256_graphics.cpp), the storage functions
bool built(std::uint8_t code) {
    if (code <= 10 || code == 16 || (code >= 21 && code <= 33) || (code >= 38 && code <= 40) || storage_function(code)) {
        return true;
    }
    switch (code) {
    case 11:
    case 12:
    case 13:
    case 14:
    case 17:
    case 18:
    case 19:
    case 20:
    case 37:
    case 41:
        return true;
    default:
        return false;
    }
}

// The expression's item codes ([FW] tvc-routines.h OP_OR_PTYPES)
enum MathItem : std::uint8_t {
    kFinish = 0,
    kAdd,
    kSub,
    kMul,
    kDiv,
    kMax,
    kMin,
    kAbs,
    kSqr,
    kSqrt,
    kSin,
    kCos,
    kTan,
    kToInt,
    kToFloat,
    kRound,
    kUint8 = 0x40,
    kInt16,
    kInt32,
    kTvcFloat,
    kFloat,
    kVarRef,
    kConst2Pi = 0x80,
    kConst1000 = 0x89,
};

// Bytes per item for the chain's size count; SQR is missing from the firmware's table ([FW] math_type_sizes)
std::uint8_t math_item_size(std::uint8_t item) {
    if (item <= kRound) {
        return item == kSqr ? 0 : 1;
    }
    switch (item) {
    case kUint8:
        return 2;
    case kInt16:
        return 3;
    case kInt32:
    case kFloat:
        return 5;
    case kTvcFloat:
        return 7;
    case kVarRef:
        return 4;
    default:
        return item >= kConst2Pi && item <= kConst1000 ? 1 : 0;
    }
}

constexpr float kMathConstants[10] = {static_cast<float>(2 * std::numbers::pi), static_cast<float>(std::numbers::pi),
                                      static_cast<float>(std::numbers::pi / 2), 0.001f, 0.01f, 0.1f, 1.0f, 10.0f, 100.0f, 1000.0f};

// Cortex-M33 VCVT: saturating, NaN gives 0
std::int32_t to_int(float v) {
    if (std::isnan(v)) {
        return 0;
    }
    if (v >= 2147483648.0f) {
        return std::numeric_limits<std::int32_t>::max();
    }
    if (v < -2147483648.0f) {
        return std::numeric_limits<std::int32_t>::min();
    }
    return static_cast<std::int32_t>(v);
}

std::int32_t wrap(std::uint32_t v) { return static_cast<std::int32_t>(v); }

struct MathValue {
    std::uint8_t type = kFinish;
    std::int32_t i = 0;
    float f = 0;
};

std::uint8_t reverse_bits(std::uint8_t b) {
    b = static_cast<std::uint8_t>((b & 0xF0) >> 4 | (b & 0x0F) << 4);
    b = static_cast<std::uint8_t>((b & 0xCC) >> 2 | (b & 0x33) << 2);
    return static_cast<std::uint8_t>((b & 0xAA) >> 1 | (b & 0x55) << 1);
}

std::uint8_t swap_nibbles(std::uint8_t b) { return static_cast<std::uint8_t>(b >> 4 | b << 4); }

}  // namespace

std::uint8_t Card256::mem_get(std::uint32_t address) const {
    return (address & kPsramFlag) ? psram_read(address & (kPsramFlag - 1)) : fast_get(address);
}

void Card256::mem_put(std::uint32_t address, std::uint8_t value) {
    if (address & kPsramFlag) {
        psram_write(address & (kPsramFlag - 1), value);
    } else {
        fast_put(address, value);
    }
}

std::uint16_t Card256::param16(std::uint32_t at) const { return static_cast<std::uint16_t>(fast_get(at) | fast_get(at + 1) << 8); }

std::uint32_t Card256::param24(std::uint32_t at) const { return param16(at) | static_cast<std::uint32_t>(fast_get(at + 2)) << 16; }

std::uint32_t Card256::param32(std::uint32_t at) const { return param24(at) | static_cast<std::uint32_t>(fast_get(at + 3)) << 24; }

// memmove inside fast RAM; bytes past its end read 0 and are not written
void Card256::fast_move(std::uint32_t dst, std::uint32_t src, std::uint32_t length) {
    std::vector<std::uint8_t> bytes(length);
    for (std::uint32_t i = 0; i < length; ++i) {
        bytes[i] = fast_get(src + i);
    }
    for (std::uint32_t i = 0; i < length; ++i) {
        fast_put(dst + i, bytes[i]);
    }
}

// The block's fast-RAM offset: C1h b7 picks U2's or U3's page, b6-0 × 128 bytes into it ([FW] getFunctionParamAddress)
void Card256::run_function(std::uint8_t code, const CardBus& bus) {
    const std::uint8_t page = unit_page_[param_start_ >> 7];
    if (page == 0xFF || !built(code)) {
        result_ = 0xFF;
        return;
    }
    result_ = call(code, static_cast<std::uint32_t>(page) * kPageSize + (param_start_ & 0x7F) * 128u, bus);
}

// Code byte and parameters, n times; the failing function's index goes to C2h, its error over the first code byte
void Card256::run_chain(std::uint8_t count, const CardBus& bus) {
    const std::uint8_t page = unit_page_[param_start_ >> 7];
    if (page == 0xFF) {
        result_ = 0xFF;
        return;
    }
    const std::uint32_t start = static_cast<std::uint32_t>(page) * kPageSize + (param_start_ & 0x7F) * 128u;
    std::uint32_t at = start;
    result_ = 0;
    for (unsigned i = 0; i < count; ++i) {
        const std::uint8_t code = fast_get(at++);
        const std::uint32_t size = built(code) ? param_size(code, at) : 0;
        if (!built(code)) {
            result_ = static_cast<std::uint8_t>(0x80 + i + 1);
            break;
        }
        if (const std::uint8_t error = call(code, at, bus)) {
            fast_put(start, error);
            result_ = static_cast<std::uint8_t>(i + 1);
            break;
        }
        at += size;
    }
}

// [FW] getFunctionParamSize (2789ff.); the firmware returns it as a byte
std::uint32_t Card256::param_size(std::uint8_t code, std::uint32_t at) const {
    const std::uint8_t kind = code < sizeof kParamSize                         ? kParamSize[code]
                              : code >= 0x81 && code < 0x81 + sizeof kFileParamSize ? kFileParamSize[code - 0x81]
                                                                                   : 0;
    if ((kind & 0x80) == 0) {
        return kind;
    }
    std::uint32_t size = 1;
    switch (kind) {
    case 0x80:
        while (fast_get(at++) != 0 && size < 0x10000) {
            ++size;
        }
        break;
    case 0x81:
        size = fast_get(at) + 1u;
        break;
    case 0x82:
        size = fast_get(at) * 4u;
        break;
    case 0x83:
        size = fast_get(at + 3) + 3u;
        break;
    case 0x84:
        size = 4 + 2 + param16(at + 4);
        break;
    case 0x85:
        size = fast_get(at) + 1u;
        size += fast_get(at + size) + 1u;
        break;
    case 0x86:
        size = fast_get(at) + 2u;
        break;
    case 0x87:
        for (std::uint32_t p = at; fast_get(p) != kFinish;) {
            const std::uint8_t s = math_item_size(fast_get(p));
            if (s == 0) {
                break;
            }
            size += s;
            p += s;
        }
        size = std::max<std::uint32_t>(size, 5);
        break;
    default:
        break;
    }
    return size & 0xFF;
}

std::uint8_t Card256::call(std::uint8_t code, std::uint32_t at, const CardBus& bus) {
    if (storage_function(code)) {
        return *call_file_function(code, at, bus);
    }
    switch (code) {
    case 11:
        return move_short(at);
    case 12:
        return move_full(at);
    case 13:
        return move_from_slow(at);
    case 14:
        return move_to_slow(at);
    case 17:
        return move_chunks_from_block(at);
    case 18:
        return move_chunks(param24(at), param24(at + 3), param16(at + 6), param16(at + 8), param16(at + 10), param16(at + 10));
    case 19:
        return mirror_sprite_phase(at);
    case 20:
        return zx7(at);
    case 37:
        return evaluate_math(at);
    case 41:
        return move_chunks(param24(at), param24(at + 3), param16(at + 6), param16(at + 8), param16(at + 10), param16(at + 12));
    default:
        return call_graphics(code, at).value_or(0xFF);
    }
}

// 11: page (0-17), destination, source, length, all within the page
std::uint8_t Card256::move_short(std::uint32_t at) {
    const std::uint8_t page = fast_get(at);
    const std::uint32_t dst = param16(at + 1);
    const std::uint32_t src = param16(at + 3);
    const std::uint32_t length = param16(at + 5);
    if (src == dst || length == 0) {
        return 0;
    }
    if (page > 0x11) {
        return 1;
    }
    if (src + length > kPageSize || dst + length > kPageSize) {
        return 2;
    }
    fast_move(page * kPageSize + dst, page * kPageSize + src, length);
    return 0;
}

// 12: destination, source, length (24-bit each) within the 256 KiB
std::uint8_t Card256::move_full(std::uint32_t at) {
    const std::uint32_t dst = param24(at);
    const std::uint32_t src = param24(at + 3);
    const std::uint32_t length = param24(at + 6);
    if (src + length > kFastLimit || dst + length > kFastLimit) {
        return 1;
    }
    fast_move(dst, src, length);
    return 0;
}

// 13: fast RAM destination, PSRAM source, length
std::uint8_t Card256::move_from_slow(std::uint32_t at) {
    const std::uint32_t dst = param24(at);
    const std::uint32_t src = param24(at + 3);
    const std::uint32_t length = param24(at + 6);
    if (src + length > kPsramLimit || dst + length > kFastLimit) {
        return 1;
    }
    for (std::uint32_t i = 0; i < length; ++i) {
        fast_put(dst + i, psram_read(src + i));
    }
    return 0;
}

// 14: PSRAM destination, fast RAM source, length
std::uint8_t Card256::move_to_slow(std::uint32_t at) {
    const std::uint32_t dst = param24(at);
    const std::uint32_t src = param24(at + 3);
    const std::uint32_t length = param24(at + 6);
    if (src + length > kFastLimit || dst + length > kPsramLimit) {
        return 1;
    }
    for (std::uint32_t i = 0; i < length; ++i) {
        psram_write(dst + i, fast_get(src + i));
    }
    return 0;
}

// 17: one continuous source into `count` chunks, the destination stepping by `step`
std::uint8_t Card256::move_chunks_from_block(std::uint32_t at) {
    std::uint32_t dst = param24(at);
    std::uint32_t src = param24(at + 3);
    const std::uint32_t chunk = param16(at + 6);
    const std::uint32_t count = param16(at + 8);
    const std::uint32_t step = param16(at + 10);
    const bool from_psram = (src & kPsramFlag) != 0;
    src &= kPsramFlag - 1;
    if (chunk == 0 || step == 0 || count == 0) {
        return 1;
    }
    if (chunk + step * (count - 1) + dst > kFastLimit) {
        return 2;
    }
    if (src + chunk * count > (from_psram ? kPsramLimit : kFastLimit)) {
        return 3;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        if (from_psram) {
            for (std::uint32_t k = 0; k < chunk; ++k) {
                fast_put(dst + k, psram_read(src + k));
            }
        } else {
            fast_move(dst, src, chunk);
        }
        dst += step;
        src += chunk;
    }
    return 0;
}

// 18 and 41: `count` chunks, source and destination stepping on their own ([FW] memory_move_chunks)
std::uint8_t Card256::move_chunks(std::uint32_t dst, std::uint32_t src, std::uint16_t chunk, std::uint16_t count, std::uint16_t dst_step,
                                  std::uint16_t src_step) {
    const bool from_psram = (src & kPsramFlag) != 0;
    src &= kPsramFlag - 1;
    if (chunk == 0 || (src_step == 0 && dst_step == 0) || count == 0) {
        return 1;
    }
    const std::uint32_t last = count - 1u;
    if (dst + chunk + last * dst_step > kFastLimit) {
        return 2;
    }
    if (src + chunk + last * src_step > (from_psram ? kPsramLimit : kFastLimit)) {
        return 3;
    }
    if (from_psram) {
        for (std::uint32_t i = 0; i < count; ++i) {
            for (std::uint32_t k = 0; k < chunk; ++k) {
                fast_put(dst + k, psram_read(src + k));
            }
            dst += dst_step;
            src += src_step;
        }
    } else if (dst < src + chunk + last * dst_step && dst > src) {
        // the firmware's overlap test (with the destination step on both sides): copy from the last chunk back
        dst += last * dst_step;
        src += last * src_step;
        for (std::uint32_t i = 0; i < count; ++i) {
            fast_move(dst, src, chunk);
            dst -= dst_step;
            src -= src_step;
        }
    } else {
        for (std::uint32_t i = 0; i < count; ++i) {
            fast_move(dst, src, chunk);
            dst += dst_step;
            src += src_step;
        }
    }
    return 0;
}

// 19: phase address (20 bits), count, colour mode (0 = 2 colours, 64-byte phases; else 16, 256-byte), flip (0 = horizontal)
std::uint8_t Card256::mirror_sprite_phase(std::uint32_t at) {
    const std::uint32_t address = param32(at) & 0x000FFFFF;
    const std::uint32_t count = fast_get(at + 3);
    const bool full = fast_get(at + 4) != 0;
    const bool vertical = fast_get(at + 5) != 0;
    if (count == 0) {
        return 0;
    }
    const std::uint32_t phase = full ? 256 : 64;
    const std::uint32_t row = full ? 12 : 3;
    if (address + count * phase - (full ? 3 : 0) > kFastLimit) {
        return 1;
    }
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint32_t base = address + i * phase;
        if (vertical) {
            for (std::uint32_t j = 0; j < 10; ++j) {
                for (std::uint32_t k = 0; k < row; ++k) {
                    const std::uint8_t top = fast_get(base + j * row + k);
                    fast_put(base + j * row + k, fast_get(base + (20 - j) * row + k));
                    fast_put(base + (20 - j) * row + k, top);
                }
            }
        } else if (full) {
            for (std::uint32_t j = 0; j < 21; ++j) {
                const std::uint32_t line = base + j * 12;
                for (std::uint32_t k = 0; k < 6; ++k) {
                    const std::uint8_t left = swap_nibbles(fast_get(line + 11 - k));
                    const std::uint8_t right = swap_nibbles(fast_get(line + k));
                    fast_put(line + k, left);
                    fast_put(line + 11 - k, right);
                }
            }
        } else {
            for (std::uint32_t j = 0; j < 21; ++j) {
                const std::uint32_t line = base + j * 3;
                const std::uint8_t left = reverse_bits(fast_get(line + 2));
                const std::uint8_t right = reverse_bits(fast_get(line));
                fast_put(line + 1, reverse_bits(fast_get(line + 1)));
                fast_put(line, left);
                fast_put(line + 2, right);
            }
        }
    }
    return 0;
}

// 20: ZX7 (written from the format) from source to destination, b23 = PSRAM; the size goes to parameter bytes 6-9
std::uint8_t Card256::zx7(std::uint32_t at) {
    const std::uint32_t dst = param24(at);
    std::uint32_t src = param24(at + 3);
    const std::uint32_t dst_space = (dst & kPsramFlag) ? kPsramFlag + kPsramLimit : static_cast<std::uint32_t>(fast_.size());
    std::uint32_t written = 0;
    std::uint32_t consumed = 0;
    unsigned mask = 0;
    std::uint8_t bits = 0;
    const auto byte = [&]() {
        ++consumed;
        return mem_get(src++);
    };
    const auto bit = [&]() {
        mask >>= 1;
        if (mask == 0) {
            mask = 0x80;
            bits = byte();
        }
        return (bits & mask) != 0;
    };
    const auto put = [&](std::uint8_t v) {
        mem_put(dst + written, v);
        ++written;
    };
    // A stream that never ends would run the MCU off its memory; stop once both sides are past it
    const auto runaway = [&]() { return consumed > kPsramLimit || dst + written >= dst_space; };
    put(byte());
    while (!runaway()) {
        if (!bit()) {
            put(byte());
            continue;
        }
        int zeros = 0;
        while (!bit()) {
            ++zeros;
            if (zeros > 15) {
                break;
            }
        }
        if (zeros > 15) {
            break;
        }
        std::uint32_t length = 1;
        for (int i = 0; i < zeros; ++i) {
            length = length << 1 | (bit() ? 1u : 0u);
        }
        ++length;
        std::uint32_t offset = byte();
        if (offset >= 128) {
            unsigned high = 0;
            for (int i = 0; i < 4; ++i) {
                high = high << 1 | (bit() ? 1u : 0u);
            }
            offset = ((offset & 127) | high << 7) + 128;
        }
        ++offset;
        for (std::uint32_t i = 0; i < length; ++i) {
            // before the destination's start the MCU read whatever lay there; here it reads 0
            put(written >= offset ? mem_get(dst + written - offset) : std::uint8_t{0});
        }
    }
    for (unsigned i = 0; i < 4; ++i) {
        fast_put(at + 6 + i, static_cast<std::uint8_t>(written >> (8 * i)));
    }
    return 0;
}

// 37: a postfix expression in place; the result (type byte + value) overwrites its start ([FW] evaluate_math_expression)
std::uint8_t Card256::evaluate_math(std::uint32_t at) {
    std::array<MathValue, 32> stack{};
    unsigned depth = 0;
    std::uint32_t cursor = at;

    // One item at `p` into `v`; the bytes it took, or -1
    const auto parse = [&](std::uint32_t p, MathValue& v, auto& self) -> int {
        const std::uint8_t item = mem_get(p);
        if (item <= kRound) {
            v = {item, 0, 0};
            return item == kFinish ? 0 : 1;
        }
        switch (item) {
        case kUint8:
            v = {kUint8, mem_get(p + 1), 0};
            return 2;
        case kInt16:
            v = {kInt16, static_cast<std::int16_t>(mem_get(p + 1) | mem_get(p + 2) << 8), 0};
            return 3;
        case kInt32:
            v = {kInt32, wrap(static_cast<std::uint32_t>(mem_get(p + 1)) | static_cast<std::uint32_t>(mem_get(p + 2)) << 8 |
                              static_cast<std::uint32_t>(mem_get(p + 3)) << 16 | static_cast<std::uint32_t>(mem_get(p + 4)) << 24),
                 0};
            return 5;
        case kTvcFloat: {
            // five BCD bytes, low digit first, then sign and excess-40h decimal exponent
            double d = 0.0;
            for (std::uint32_t i = 1; i <= 5; ++i) {
                const std::uint8_t b = mem_get(p + i);
                d = d / 10.0 + (b & 0x0F);
                d = d / 10.0 + ((b & 0xF0) >> 4);
            }
            const std::uint8_t last = mem_get(p + 6);
            if (last & 0x80) {
                d = -d;
            }
            const int power = static_cast<std::int8_t>((last & 0x7F) - 0x40);
            for (int i = 0; i < std::abs(power); ++i) {
                d = power > 0 ? d * 10.0 : d / 10.0;
            }
            v = {kFloat, 0, static_cast<float>(d)};
            return 7;
        }
        case kFloat: {
            const std::uint32_t raw = static_cast<std::uint32_t>(mem_get(p + 1)) | static_cast<std::uint32_t>(mem_get(p + 2)) << 8 |
                                      static_cast<std::uint32_t>(mem_get(p + 3)) << 16 | static_cast<std::uint32_t>(mem_get(p + 4)) << 24;
            v = {kFloat, 0, std::bit_cast<float>(raw)};
            return 5;
        }
        case kVarRef: {
            const std::uint32_t target = static_cast<std::uint32_t>(mem_get(p + 1)) | static_cast<std::uint32_t>(mem_get(p + 2)) << 8 |
                                         static_cast<std::uint32_t>(mem_get(p + 3)) << 16;
            const std::uint32_t where = (target & kPsramFlag) ? target : target & 0x1FFFFF;
            const std::uint8_t type = mem_get(where);
            if (type < kUint8 || type > kFloat) {
                return -1;
            }
            self(where, v, self);
            return 4;
        }
        default:
            if (item >= kConst2Pi && item <= kConst1000) {
                v = {kFloat, 0, kMathConstants[item - kConst2Pi]};
                return 1;
            }
            return -1;
        }
    };

    const auto as_float = [](const MathValue& v) { return v.type == kFloat ? v.f : static_cast<float>(v.i); };
    // Integer results take the wider operand type; the value itself is not cut to it
    const auto int_type = [](const MathValue& a, const MathValue& b) {
        if (a.type == kInt32 || b.type == kInt32) {
            return std::uint8_t{kInt32};
        }
        if (a.type == kInt16 || b.type == kInt16) {
            return std::uint8_t{kInt16};
        }
        return a.type;
    };

    int error = 0;
    for (;;) {
        if (depth >= stack.size()) {
            error = -1;  // the MCU's 32-entry stack would overflow into its other variables
            break;
        }
        MathValue& next = stack[depth];
        const int taken = parse(cursor, next, parse);
        if (taken < 0) {
            error = -1;
            break;
        }
        cursor += static_cast<std::uint32_t>(taken);
        const std::uint8_t type = next.type;
        if (type == kFinish) {
            break;
        }
        if (type >= kAbs && type <= kRound) {
            if (depth < 1) {
                error = -2;
                break;
            }
            MathValue& a = stack[depth - 1];
            switch (type) {
            case kAbs:
                if (a.type == kFloat) {
                    a.f = a.f >= 0 ? a.f : -a.f;
                } else {
                    a.i = a.i < 0 ? wrap(0u - static_cast<std::uint32_t>(a.i)) : a.i;
                }
                break;
            case kSqr:
                if (a.type == kFloat) {
                    a.f = a.f * a.f;
                } else {
                    a.i = wrap(static_cast<std::uint32_t>(a.i) * static_cast<std::uint32_t>(a.i));
                }
                break;
            case kSqrt:
                a.f = std::sqrt(as_float(a));
                a.type = kFloat;
                break;
            case kSin:
            case kCos:
            case kTan:
                if (a.type != kFloat) {
                    error = type == kSin ? -3 : type == kCos ? -4 : -5;
                    break;
                }
                a.f = type == kSin ? std::sin(a.f) : type == kCos ? std::cos(a.f) : std::tan(a.f);
                break;
            case kToInt:
                if (a.type != kFloat) {
                    error = -6;
                    break;
                }
                a.i = to_int(a.f);
                a.type = kInt32;
                break;
            case kToFloat:
                if (a.type == kFloat) {
                    error = -7;
                    break;
                }
                a.f = static_cast<float>(a.i);
                a.type = kFloat;
                break;
            default:  // kRound: a no-op on integers
                if (a.type == kFloat) {
                    a.i = to_int(std::round(a.f));
                    a.type = kInt32;
                }
                break;
            }
            if (error) {
                break;
            }
        } else if (type < kAbs) {
            if (depth < 2) {
                error = -9;
                break;
            }
            MathValue& a = stack[depth - 2];
            const MathValue b = stack[depth - 1];
            --depth;
            if (a.type == kFloat || b.type == kFloat) {
                const float x = as_float(a);
                const float y = as_float(b);
                switch (type) {
                case kAdd:
                    a.f = x + y;
                    break;
                case kSub:
                    a.f = x - y;
                    break;
                case kMul:
                    a.f = x * y;
                    break;
                case kDiv:
                    a.f = x / y;
                    break;
                case kMin:
                    a.f = std::fmin(x, y);
                    break;
                default:
                    a.f = std::fmax(x, y);
                    break;
                }
                a.type = kFloat;
            } else {
                const auto x = static_cast<std::uint32_t>(a.i);
                const auto y = static_cast<std::uint32_t>(b.i);
                switch (type) {
                case kAdd:
                    a.i = wrap(x + y);
                    break;
                case kSub:
                    a.i = wrap(x - y);
                    break;
                case kMul:
                    a.i = wrap(x * y);
                    break;
                case kDiv:
                    // SDIV: by zero gives 0, INT_MIN / -1 gives INT_MIN
                    a.i = b.i == 0 ? 0 : (a.i == std::numeric_limits<std::int32_t>::min() && b.i == -1) ? a.i : a.i / b.i;
                    break;
                case kMin:
                    a.i = wrap(std::min(x, y));  // compared unsigned, as the firmware's MIN macro on uint32_t
                    break;
                default:
                    a.i = wrap(std::max(x, y));
                    break;
                }
                a.type = int_type(a, b);
            }
        } else {
            ++depth;  // a value or a constant
        }
    }
    if (error < 0 || depth < 1) {
        return static_cast<std::uint8_t>(-error);
    }
    // The type of the top entry with the value of the bottom one, as the firmware writes it
    const std::uint8_t type = stack[depth - 1].type;
    const MathValue& bottom = stack[0];
    fast_put(at, type);
    switch (type) {
    case kUint8:
        fast_put(at + 1, static_cast<std::uint8_t>(bottom.i));
        break;
    case kInt16:
        fast_put(at + 1, static_cast<std::uint8_t>(bottom.i));
        fast_put(at + 2, static_cast<std::uint8_t>(bottom.i >> 8));
        break;
    case kInt32:
    case kFloat: {
        const std::uint32_t raw = type == kFloat ? std::bit_cast<std::uint32_t>(bottom.f) : static_cast<std::uint32_t>(bottom.i);
        for (unsigned i = 0; i < 4; ++i) {
            fast_put(at + 1 + i, static_cast<std::uint8_t>(raw >> (8 * i)));
        }
        break;
    }
    default:
        break;
    }
    return 0;
}

}  // namespace freetvc
