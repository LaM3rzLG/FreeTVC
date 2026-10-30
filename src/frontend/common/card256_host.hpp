// SPDX-License-Identifier: GPL-3.0-or-later
// The TVC256k++'s host side (design/cards.md "Storage"): the pendrive as a host folder, the flash drive's image and
// what is plugged into the USB port. All of it stays outside the machine state.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "core/cards/card256_storage.hpp"

namespace freetvc {

// The flash drive's size on the card (16 MiB module, the first MiB for the firmware)
inline constexpr std::size_t kCard256FlashBytes = 15 * 1024 * 1024;

// TVC names as bytes in code page 437, the way FatFs reads a pendrive ❓; host names are UTF-8
std::string card256_host_name(const std::string& tvc);
std::string card256_tvc_name(const std::string& host);

// A host folder as a FAT pendrive: names match without case, long names get an 8.3 alias, entries come sorted by name
class PendriveFolder final : public FatVolume {
public:
    explicit PendriveFolder(std::filesystem::path root) : root_(std::move(root)) {}
    const std::filesystem::path& root() const { return root_; }

    std::uint8_t stat(const std::string& path, FatEntry& entry) override;
    std::uint8_t list(const std::string& dir, std::vector<FatEntry>& entries) override;
    std::uint8_t read(const std::string& path, std::uint64_t pos, std::span<std::uint8_t> out, std::size_t& got) override;
    std::uint8_t write(const std::string& path, std::uint64_t pos, std::span<const std::uint8_t> data, std::size_t& put) override;
    std::uint8_t create(const std::string& path) override;
    std::uint8_t make_dir(const std::string& path) override;
    std::uint8_t remove(const std::string& path) override;
    std::uint8_t rename(const std::string& from, const std::string& to) override;

private:
    // The host path of a volume path: kFrOk when it exists, kFrNoFile when only the last name is missing (`host` is
    // where it would go), kFrNoPath when a directory on the way is missing
    std::uint8_t locate(const std::string& path, std::filesystem::path& host) const;

    std::filesystem::path root_;
};

class Card256Host {
public:
    Card256Media& media() { return media_; }
    const Card256Media& media() const { return media_; }

    // The USB port: a folder as the pendrive, the mouse, or nothing; false with `error` set
    bool plug_pendrive(const std::string& dir, std::string& error);
    void plug_mouse();
    void unplug();
    Card256Usb usb() const { return media_.usb; }
    std::filesystem::path pendrive_dir() const { return pendrive_ ? pendrive_->root() : std::filesystem::path(); }

    // The flash drive: a littlefs image file, or a folder packed into one (kCard256FlashBytes); false with `error` set
    bool load_flash(const std::string& path, std::string& error);
    void set_flash(std::vector<std::uint8_t> image, std::string name);
    bool has_flash() const { return media_.flash != nullptr; }
    const std::string& flash_name() const { return flash_name_; }

    // One line: the port and the flash drive
    std::string describe() const;

private:
    std::optional<PendriveFolder> pendrive_;
    Card256Media media_;
    std::string flash_name_;
};

}  // namespace freetvc
