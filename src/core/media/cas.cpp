// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/media/cas.hpp"

#include <algorithm>

#include "core/message.hpp"

namespace freetvc {

std::span<const std::uint8_t> CasFile::header() const {
    return std::span<const std::uint8_t>(stream).first(std::min(stream.size(), kCasProgramHeaderSize));
}

std::span<const std::uint8_t> CasFile::block() const {
    return stream.size() > kCasProgramHeaderSize ? std::span<const std::uint8_t>(stream).subspan(kCasProgramHeaderSize)
                                                 : std::span<const std::uint8_t>();
}

std::optional<CasFile> parse_cas(std::span<const std::uint8_t> bytes, std::string& error) {
    if (bytes.size() < kCasHeaderSize) {
        error = msg("shorter than the 128-byte CAS header");
        return std::nullopt;
    }
    CasFile cas;
    const std::uint8_t type = bytes[0];
    cas.buffered = type == 0x01;
    if (type != 0x01 && type != 0x11) {
        cas.warnings.push_back(msg("type byte %u is neither 01h nor 11h; read as unbuffered", static_cast<unsigned>(type)));
    }
    cas.protect = bytes[1] != 0;
    const std::size_t records = bytes[2] | (bytes[3] << 8);
    if (records * 128 + bytes[4] != bytes.size() || bytes[4] > 127) {
        cas.warnings.push_back(msg("record count does not match the file size (ignored)"));
    }
    if (!cas.buffered) {
        if (bytes.size() < kCasHeaderSize + kCasProgramHeaderSize) {
            error = msg("unbuffered file without its 16-byte program header");
            return std::nullopt;
        }
        const std::size_t length = bytes[0x82] | (bytes[0x83] << 8);
        const std::size_t present = bytes.size() - kCasHeaderSize - kCasProgramHeaderSize;
        if (length != present) {
            cas.warnings.push_back(msg("program header gives %zu bytes, the file holds %zu", length, present));
        }
    }
    cas.stream.assign(bytes.begin() + kCasHeaderSize, bytes.end());
    return cas;
}

std::vector<std::uint8_t> build_cas(bool buffered, bool protect, std::span<const std::uint8_t> stream) {
    std::vector<std::uint8_t> file(kCasHeaderSize, 0);
    const std::size_t size = kCasHeaderSize + stream.size();
    file[0] = buffered ? 0x01 : 0x11;
    file[1] = protect ? 0xFF : 0x00;
    file[2] = static_cast<std::uint8_t>(size / 128);
    file[3] = static_cast<std::uint8_t>(size / 128 >> 8);
    file[4] = static_cast<std::uint8_t>(size % 128);
    file.insert(file.end(), stream.begin(), stream.end());
    return file;
}

std::string tvc_file_name(std::string_view name) {
    std::string out(name.substr(0, 16));
    for (char& c : out) {
        const auto b = static_cast<std::uint8_t>(c);
        if (b >= 'a' && b <= 'z') {
            c = static_cast<char>(b & 0xDF);
        } else if (b >= 0x90 && b <= 0x98) {
            c = static_cast<char>(b - 0x10);
        }
    }
    return out;
}

}  // namespace freetvc
