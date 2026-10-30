// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/common/wav.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "core/message.hpp"

namespace freetvc {

namespace {

void put16(std::vector<unsigned char>& out, unsigned value) {
    out.push_back(static_cast<unsigned char>(value));
    out.push_back(static_cast<unsigned char>(value >> 8));
}

void put32(std::vector<unsigned char>& out, std::uint32_t value) {
    put16(out, value & 0xFFFF);
    put16(out, value >> 16);
}

}  // namespace

bool write_wav(const std::string& path, std::span<const float> samples, unsigned rate, std::string& error) {
    const auto data_size = static_cast<std::uint32_t>(samples.size() * 2);
    std::vector<unsigned char> bytes;
    bytes.reserve(44 + data_size);
    bytes.insert(bytes.end(), {'R', 'I', 'F', 'F'});
    put32(bytes, 36 + data_size);
    bytes.insert(bytes.end(), {'W', 'A', 'V', 'E', 'f', 'm', 't', ' '});
    put32(bytes, 16);
    put16(bytes, 1);  // PCM
    put16(bytes, 1);  // mono
    put32(bytes, rate);
    put32(bytes, rate * 2);
    put16(bytes, 2);
    put16(bytes, 16);
    bytes.insert(bytes.end(), {'d', 'a', 't', 'a'});
    put32(bytes, data_size);
    for (const float sample : samples) {
        const long value = std::lround(std::clamp(sample, -1.0f, 1.0f) * 32767.0f);
        put16(bytes, static_cast<unsigned>(value) & 0xFFFF);
    }
    std::FILE* out = std::fopen(path.c_str(), "wb");
    if (!out) {
        error = msg("cannot write '%s'", path.c_str());
        return false;
    }
    const bool ok = std::fwrite(bytes.data(), 1, bytes.size(), out) == bytes.size();
    if (std::fclose(out) != 0 || !ok) {
        error = msg("cannot write '%s'", path.c_str());
        return false;
    }
    return true;
}

}  // namespace freetvc
