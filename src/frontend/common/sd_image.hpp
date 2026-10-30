// SPDX-License-Identifier: GPL-3.0-or-later
// SD card images for the SD Dual (design/cards.md "SD Dual"): a host file holding a whole card (MBR partitions or
// a super floppy), read and written in place; and new FAT images built from host files with the vendored FatFs.
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

#include "core/bus/sd_card.hpp"

namespace freetvc {

class SdImageFile final : public SdCardImage {
public:
    static constexpr std::uint64_t kMinSize = 64 * 1024;
    static constexpr std::uint64_t kMaxSize = 2ull << 40;  // 2 TiB, the most CSD 2.0 describes

    // A file of whole 512-byte blocks, 64 KiB to 2 TiB; read-only when it cannot be written. False with `error` set.
    bool open(const std::string& path, std::string& error);
    void close();
    bool is_open() const { return file_.is_open(); }
    const std::filesystem::path& path() const { return path_; }
    // False once a read or write failed (the card reports it; the front end says so once)
    bool failed() const { return failed_; }

    std::uint64_t size() const override { return size_; }
    bool read(std::uint64_t offset, std::span<std::uint8_t> out) override;
    bool write(std::uint64_t offset, std::span<const std::uint8_t> data) override;
    bool read_only() const override { return read_only_; }

private:
    std::fstream file_;
    std::filesystem::path path_;
    std::uint64_t size_ = 0;
    bool read_only_ = false;
    bool failed_ = false;
};

// The SD Dual's firmware: MOPS, then XOR A and LD (FC02h),A (every flash page starts so), at most 64 KiB
bool is_sddual_firmware(std::span<const std::uint8_t> image);

// A new card image at `out` of `mib` MiB (1 to 2 TiB): one FAT partition behind an MBR (FAT16 up to 128 MiB, FAT32
// above, as FatFs chooses), with the host files and folders (with their contents) copied into its root in order.
// False with `error` set; `out` is not left half-written.
bool write_sd_image(const std::string& out, std::uint64_t mib, const std::vector<std::string>& files, std::string& error);
// The paths in the image's FAT volume ("/NAME.EXT", a directory with a trailing '/'), depth first; empty when it does
// not mount
std::vector<std::string> sd_image_listing(const std::string& path);

}  // namespace freetvc
