// SPDX-License-Identifier: GPL-3.0-or-later
// TVC256k++ storage (design/cards.md "Storage"): the pendrive's volume and the media kept outside the machine state.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace freetvc {

// FatFs result codes (FRESULT), as the card reports them
inline constexpr std::uint8_t kFrOk = 0;
inline constexpr std::uint8_t kFrDiskErr = 1;
inline constexpr std::uint8_t kFrNotReady = 3;
inline constexpr std::uint8_t kFrNoFile = 4;
inline constexpr std::uint8_t kFrNoPath = 5;
inline constexpr std::uint8_t kFrInvalidName = 6;
inline constexpr std::uint8_t kFrDenied = 7;
inline constexpr std::uint8_t kFrExist = 8;
inline constexpr std::uint8_t kFrInvalidObject = 9;
inline constexpr std::uint8_t kFrWriteProtected = 10;
inline constexpr std::uint8_t kFrInvalidDrive = 11;
inline constexpr std::uint8_t kFrNotEnabled = 12;
inline constexpr std::uint8_t kFrNoFilesystem = 13;
inline constexpr std::uint8_t kFrTooManyOpenFiles = 18;
inline constexpr std::uint8_t kFrInvalidParameter = 19;

// FatFs attribute bits
inline constexpr std::uint8_t kAmReadOnly = 0x01;
inline constexpr std::uint8_t kAmDir = 0x10;
inline constexpr std::uint8_t kAmArchive = 0x20;

struct FatEntry {
    std::string name;  // the long name as stored
    std::string alt;   // the 8.3 name when it differs from `name`, else empty (FatFs FILINFO altname)
    std::uint8_t attr = 0;
    std::uint64_t size = 0;
    std::uint16_t date = 0;  // FAT date and time
    std::uint16_t time = 0;
};

// A FAT volume addressed by absolute paths ("/" the root, "/DIR/NAME.EXT"); names match without ASCII case. Every call
// returns a FatResult; a missing last component is kFrNoFile, a missing directory on the way kFrNoPath.
class FatVolume {
public:
    virtual ~FatVolume() = default;
    virtual std::uint8_t stat(const std::string& path, FatEntry& entry) = 0;
    // A directory's entries in directory order, without "." and ".."
    virtual std::uint8_t list(const std::string& dir, std::vector<FatEntry>& entries) = 0;
    virtual std::uint8_t read(const std::string& path, std::uint64_t pos, std::span<std::uint8_t> out, std::size_t& got) = 0;
    // Writes at `pos`, growing the file (a gap reads as zeros); `put` is less than asked only when the volume is full
    virtual std::uint8_t write(const std::string& path, std::uint64_t pos, std::span<const std::uint8_t> data, std::size_t& put) = 0;
    // A new empty file, or an existing one truncated
    virtual std::uint8_t create(const std::string& path) = 0;
    virtual std::uint8_t make_dir(const std::string& path) = 0;
    // A file or an empty directory
    virtual std::uint8_t remove(const std::string& path) = 0;
    virtual std::uint8_t rename(const std::string& from, const std::string& to) = 0;
};

// The card's single USB-A port (no hub)
enum class Card256Usb : std::uint8_t { None, Pendrive, Mouse };

// What the card reaches outside the machine state; the front end owns it.
struct Card256Media {
    Card256Usb usb = Card256Usb::None;  // what is plugged in; the card follows it at every access
    FatVolume* pendrive = nullptr;  // the pendrive's folder
    std::shared_ptr<const std::vector<std::uint8_t>> flash;  // the flash drive's littlefs image (15 MiB on the card), or none
    bool flash_valid = false;  // card256_flash_valid(*flash), set with it
};

// Whether `image` mounts as the card's flash drive (littlefs, 4 KiB blocks)
bool card256_flash_valid(std::span<const std::uint8_t> image);

// A flash drive image of `bytes` (a multiple of 4 KiB) holding `files`, paths like "dir/name" (directories made on the way);
// empty when they do not fit
struct Card256FlashFile {
    std::string path;
    std::vector<std::uint8_t> bytes;
};
std::vector<std::uint8_t> card256_flash_image(const std::vector<Card256FlashFile>& files, std::size_t bytes);

// The bytes of a FAT image as FatFs reads and writes them, kept anywhere (memory, a host file)
class FatDisk {
public:
    virtual ~FatDisk() = default;
    virtual bool read(std::uint64_t pos, std::span<std::uint8_t> out) = 0;
    virtual bool write(std::uint64_t pos, std::span<const std::uint8_t> in) = 0;
    virtual std::uint64_t size() = 0;
};
// A new FAT file system filling `disk` (FatFs f_mkfs, FAT12/16/32 by size): one partition behind an MBR, or a super floppy
bool fat_format(FatDisk& disk, bool partitioned);
// A volume over `disk`, which must outlive it; nullptr when it does not mount
std::unique_ptr<FatVolume> fat_disk_volume(FatDisk& disk);
// An empty FAT volume of `bytes` (FatFs f_mkfs), and a volume over such an image held in memory; for DSK images and tests
std::vector<std::uint8_t> fat_image_format(std::size_t bytes);
std::unique_ptr<FatVolume> fat_image_volume(std::vector<std::uint8_t>& image);

// The 8.3 alias FatFs would show for `name` (empty when `name` is already a plain upper-case 8.3 name); `n` numbers it (~n)
std::string fat_short_name(const std::string& name, unsigned n);

}  // namespace freetvc
