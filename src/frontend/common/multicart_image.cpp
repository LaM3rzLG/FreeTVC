// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/common/multicart_image.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <filesystem>
#include <fstream>

#include "core/message.hpp"
#include "frontend/common/presets.hpp"

namespace freetvc {

namespace {

constexpr std::size_t kChip = 0x80000;
constexpr std::size_t kImage = 2 * kChip;
constexpr std::size_t kBank = 0x4000;
constexpr std::size_t kBlock = 0x100;
constexpr std::size_t kBlockData = 254;  // a CAS block: next bank, next block, 254 bytes
constexpr std::size_t kBlocksPerChip = kChip / kBlock;
constexpr std::size_t kEntry = 20;  // type, bank, block, blocks, 16-character name
constexpr std::size_t kFirmwareCopy = 0x3000;  // MC21RM's setFirmware: 3 × 4 KiB, then the first entry

// The TVC's codes for the Hungarian letters the builder maps (unit1.pas ReplaceSpecToTVCChars)
constexpr std::array<std::string_view, 18> kAccents{"Á", "É", "Í", "Ó", "Ö", "Ő", "Ú", "Ü", "Ű",
                                                    "á", "é", "í", "ó", "ö", "ő", "ú", "ü", "ű"};

std::uint8_t accent_code(std::size_t i) { return static_cast<std::uint8_t>(i < 9 ? 0x80 + i : 0x90 + (i - 9)); }

// 16 TVC characters for a UTF-8 name: the accents mapped, other non-ASCII characters as '?'
std::array<std::uint8_t, 16> tvc_name(std::string_view utf8) {
    std::array<std::uint8_t, 16> out;
    out.fill(' ');
    std::size_t n = 0;
    for (std::size_t i = 0; i < utf8.size() && n < out.size();) {
        const auto c = static_cast<unsigned char>(utf8[i]);
        if (c < 0x80) {
            out[n++] = c;
            ++i;
            continue;
        }
        std::size_t len = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
        len = std::min(len, utf8.size() - i);
        const std::string_view cp = utf8.substr(i, len);
        const auto it = std::find(kAccents.begin(), kAccents.end(), cp);
        out[n++] = it == kAccents.end() ? std::uint8_t{'?'} : accent_code(static_cast<std::size_t>(it - kAccents.begin()));
        i += len;
    }
    return out;
}

std::string utf8_name(std::span<const std::uint8_t> tvc) {
    std::string out;
    for (const std::uint8_t c : tvc) {
        if (c >= 0x80 && c <= 0x88) {
            out += kAccents[c - 0x80];
        } else if (c >= 0x90 && c <= 0x98) {
            out += kAccents[9 + c - 0x90];
        } else if (c >= 0x20 && c < 0x7F) {
            out += static_cast<char>(c);
        } else {
            out += '?';
        }
    }
    while (!out.empty() && out.back() == ' ') {
        out.pop_back();
    }
    return out;
}

std::size_t bam_offset(std::span<const std::uint8_t> firmware) {
    return static_cast<std::size_t>(firmware[8] | (firmware[9] << 8)) - 0xC000;
}

}  // namespace

const char* multicart_type_name(std::uint8_t type) {
    switch (type) {
    case MultiCartEntry::Basic:
        return "BASIC";
    case MultiCartEntry::Cart:
        return "CART";
    case MultiCartEntry::Cas:
        return "CAS";
    case MultiCartEntry::Bin:
        return "BIN";
    case MultiCartEntry::Dat:
        return "DAT";
    default:
        return "?";
    }
}

bool is_multicart_firmware(std::span<const std::uint8_t> image) {
    if (image.size() < 10 || !std::equal(image.begin(), image.begin() + 4, "MOPS") || image[4] != 0x18 || image[6] < 0x21) {
        return false;
    }
    const unsigned bam = image[8] | (image[9] << 8);
    return bam >= 0xC000 + kFirmwareCopy && bam + 0x200 + kEntry <= 0x10000;
}

bool is_multicart_image(std::span<const std::uint8_t> image) {
    return image.size() > 4 * kBank && image.size() <= kImage && is_multicart_firmware(image);
}

std::vector<MultiCartEntry> multicart_directory(std::span<const std::uint8_t> image) {
    std::vector<MultiCartEntry> out;
    if (!is_multicart_firmware(image) || image.size() < kBank) {
        return out;
    }
    // Entries after the first until a type byte of 0 (the firmware's F_INIT_ENTRIES)
    for (std::size_t p = bam_offset(image) + 0x200 + kEntry; p + kEntry <= kBank && image[p] != 0; p += kEntry) {
        MultiCartEntry e;
        e.type = image[p];
        e.bank = image[p + 1];
        e.block = image[p + 2];
        e.blocks = image[p + 3];
        e.name = utf8_name(image.subspan(p + 4, 16));
        out.push_back(std::move(e));
    }
    return out;
}

std::optional<std::vector<std::uint8_t>> build_multicart_image(std::span<const std::uint8_t> firmware,
                                                               const std::vector<MultiCartFile>& files, std::string& error) {
    if (!is_multicart_firmware(firmware) || firmware.size() < bam_offset(firmware) + 0x200 + kEntry) {
        error = msg("not a MultiCart v2.1+ firmware");
        return std::nullopt;
    }
    // MC21RM starts from zeroed chips; the firmware erases a sector before it writes one
    std::vector<std::uint8_t> rom(kImage, 0);
    const std::size_t bam = bam_offset(firmware);
    std::copy_n(firmware.begin(), kFirmwareCopy, rom.begin());
    std::copy_n(firmware.begin() + static_cast<std::ptrdiff_t>(bam + 0x200), kEntry, rom.begin() + static_cast<std::ptrdiff_t>(bam + 0x200));
    rom[bam + 0x200] = MultiCartEntry::Basic;

    std::vector<bool> used(2 * kBlocksPerChip, false);
    std::fill_n(used.begin(), kBank / kBlock, true);  // the firmware's bank
    const auto free_blocks = [&] { return static_cast<std::size_t>(std::count(used.begin(), used.end(), false)); };

    std::size_t entry = bam + 0x200 + kEntry;
    for (const MultiCartFile& file : files) {
        if (entry + kEntry > kBank) {
            error = msg("%s: the directory is full", file.name.c_str());
            return std::nullopt;
        }
        std::uint8_t* e = rom.data() + entry;
        const bool mops = file.bytes.size() >= 4 && std::equal(file.bytes.begin(), file.bytes.begin() + 4, "MOPS");
        if (mops) {
            // A whole bank whose blocks are all free, chip 0 first (unit1.pas allocateCart)
            const std::size_t size = std::min(file.bytes.size(), kBank);
            const std::size_t blocks = (size + kBlock - 1) / kBlock;
            std::size_t bank = 0;
            for (; bank < 64; ++bank) {
                const auto first = used.begin() + static_cast<std::ptrdiff_t>(bank * (kBank / kBlock));
                if (std::none_of(first, first + static_cast<std::ptrdiff_t>(blocks), [](bool b) { return b; })) {
                    std::fill_n(first, blocks, true);
                    break;
                }
            }
            if (bank == 64) {
                error = msg("%s: no free 16 KiB bank", file.name.c_str());
                return std::nullopt;
            }
            std::copy_n(file.bytes.begin(), size, rom.begin() + static_cast<std::ptrdiff_t>(bank * kBank));
            e[0] = MultiCartEntry::Cart;
            e[1] = static_cast<std::uint8_t>(bank);
            e[3] = static_cast<std::uint8_t>(blocks);
        } else if (file.cas) {
            if (file.bytes.size() <= 0x90) {
                error = msg("%s: too short for a CAS file", file.name.c_str());
                return std::nullopt;
            }
            // The payload length at 82h-83h, or the rest of the file as a BIN entry (unit1.pas addImageToROM)
            const std::size_t length = file.bytes[0x82] | (file.bytes[0x83] << 8);
            const bool bin = length == 0 || file.bytes.size() < length;
            const std::size_t count = std::min(bin ? file.bytes.size() : length, file.bytes.size() - 0x90);
            const std::size_t blocks = (count + kBlockData - 1) / kBlockData;
            if (count == 0 || blocks > free_blocks() || blocks > 255) {
                error = msg(count == 0 ? N_("%s: no program in the CAS file") : N_("%s: does not fit"), file.name.c_str());
                return std::nullopt;
            }
            // Each next free block, chip 0 first (allocateAndCopyCAS); the last one holds FFh and its byte count
            std::size_t prev = SIZE_MAX;
            for (std::size_t done = 0; done < count;) {
                const std::size_t pos = static_cast<std::size_t>(std::find(used.begin(), used.end(), false) - used.begin());
                used[pos] = true;
                const auto bank = static_cast<std::uint8_t>(pos / (kBank / kBlock));
                const auto block = static_cast<std::uint8_t>(pos % (kBank / kBlock));
                if (prev == SIZE_MAX) {
                    e[1] = bank;
                    e[2] = block;
                } else {
                    rom[prev * kBlock] = bank;
                    rom[prev * kBlock + 1] = block;
                }
                const std::size_t n = std::min(kBlockData, count - done);
                std::copy_n(file.bytes.begin() + static_cast<std::ptrdiff_t>(0x90 + done), n,
                            rom.begin() + static_cast<std::ptrdiff_t>(pos * kBlock + 2));
                done += n;
                prev = pos;
            }
            rom[prev * kBlock] = 0xFF;
            rom[prev * kBlock + 1] = static_cast<std::uint8_t>((count - 1) % kBlockData + 1);
            e[0] = bin ? MultiCartEntry::Bin : MultiCartEntry::Cas;
            e[3] = static_cast<std::uint8_t>(blocks);
        } else {
            error = msg("%s: neither a CAS file nor a MOPS cartridge", file.name.c_str());
            return std::nullopt;
        }
        const auto name = tvc_name(file.name);
        std::copy(name.begin(), name.end(), e + 4);
        entry += kEntry;
    }

    // The BAM: chip 0's 2048 blocks, then chip 1's, one bit each, the lowest block in bit 0 (insertBAM)
    for (std::size_t i = 0; i < used.size(); ++i) {
        if (used[i]) {
            rom[bam + i / 8] |= static_cast<std::uint8_t>(1u << (i % 8));
        }
    }
    return rom;
}

std::optional<MultiCartFile> read_multicart_file(const std::string& path, std::string& error) {
    auto bytes = read_file(path, kBank * 4, error);
    if (!bytes) {
        return std::nullopt;
    }
    const std::filesystem::path p(path);
    std::string ext = p.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    MultiCartFile file;
    file.name = p.stem().string();
    file.bytes = std::move(*bytes);
    file.cas = ext == ".cas";
    return file;
}

bool write_multicart_image(const std::string& out, const std::vector<std::string>& files, const std::string& language,
                           const std::vector<RomPlace>& places, std::string& error) {
    const std::string_view name = language == "hu" ? kMultiCartFirmwareHu
                                  : language == "es" ? kMultiCartFirmwareEs
                                  : language == "en" || language.empty() ? kMultiCartFirmwareEn
                                                                         : std::string_view();
    if (name.empty()) {
        error = msg("unknown MultiCart firmware language '%s' (en, hu, es)", language.c_str());
        return false;
    }
    std::string why;
    const auto firmware = find_card_rom(name, kBank, places, why);
    if (!firmware) {
        error = msg("no MultiCart firmware: %s", why.substr(0, why.find("; searched")).c_str());
        return false;
    }
    std::vector<MultiCartFile> list;
    for (const std::string& path : files) {
        auto file = read_multicart_file(path, error);
        if (!file) {
            return false;
        }
        list.push_back(std::move(*file));
    }
    const auto image = build_multicart_image(*firmware, list, error);
    if (!image) {
        return false;
    }
    const std::string temp = out + ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        file.write(reinterpret_cast<const char*>(image->data()), static_cast<std::streamsize>(image->size()));
        if (!file) {
            error = msg("cannot write %s", temp.c_str());
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, out, ec);
    if (ec) {
        error = msg("cannot write %s: %s", out.c_str(), ec.message().c_str());
        return false;
    }
    return true;
}

}  // namespace freetvc
