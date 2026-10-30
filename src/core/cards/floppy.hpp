// SPDX-License-Identifier: GPL-3.0-or-later
// Floppy disks and the four drives behind a disk card (design/cards.md "Disks"). Pure data: the front end reads
// and writes the host files; disk contents are not part of the machine state.
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace freetvc {

struct FloppyGeometry {
    unsigned tracks = 80;
    unsigned heads = 2;
    unsigned sectors = 9;  // per track, numbered from 1
    static constexpr unsigned kSectorSize = 512;
    std::size_t bytes() const { return std::size_t{tracks} * heads * sectors * kSectorSize; }
};

// A raw sector image (.dsk, .img): track by track, side 0 then side 1, sectors in order.
class DiskImage {
public:
    static constexpr unsigned kMaxTracks = 84;

    // Geometry from the FAT12 boot sector when it fits the size, else from the size alone; nothing (with `error` set)
    // for a size that is no whole number of 512-byte tracks.
    static std::optional<DiskImage> from_bytes(std::vector<std::uint8_t> bytes, std::string& error);
    // A formatted, empty FAT12 disk (VT-DOS's layout: 720 KiB = 80 × 2 × 9, 360 KiB = 80 × 1 × 9).
    static DiskImage blank(unsigned heads);

    const FloppyGeometry& geometry() const { return geometry_; }
    std::span<const std::uint8_t> bytes() const { return bytes_; }
    // nullptr when the image has no such sector
    const std::uint8_t* sector(unsigned track, unsigned head, unsigned sector) const;
    std::uint8_t* sector(unsigned track, unsigned head, unsigned sector);
    // Write Track on a track past the end adds empty tracks up to it (up to kMaxTracks); false if it cannot.
    bool grow_to(unsigned track);

    // File names in the FAT12 root directory, as "NAME.EXT", in directory order; empty when there is no FAT12 volume.
    std::vector<std::string> root_files() const;
    // A root-directory file's bytes by its "NAME.EXT" (ASCII case ignored), following the FAT12 chain; nothing if absent.
    std::optional<std::vector<std::uint8_t>> root_file(std::string_view name) const;

private:
    FloppyGeometry geometry_;
    std::vector<std::uint8_t> bytes_;
};

// The drives A-D: what is in them, write protection, and which were written since the front end last saved them.
class FloppyDrives {
public:
    static constexpr unsigned kDrives = 4;

    void insert(unsigned drive, DiskImage image, bool write_protected);
    void eject(unsigned drive);
    DiskImage* disk(unsigned drive) { return drive < kDrives && drives_[drive].image ? &*drives_[drive].image : nullptr; }
    const DiskImage* disk(unsigned drive) const { return drive < kDrives && drives_[drive].image ? &*drives_[drive].image : nullptr; }
    bool write_protected(unsigned drive) const { return drive < kDrives && drives_[drive].write_protected; }

    // The card reports each sector it wrote; the front end saves dirty disks and clears the flag.
    void mark_written(unsigned drive);
    bool dirty(unsigned drive) const { return drive < kDrives && drives_[drive].dirty; }
    void clear_dirty(unsigned drive);

    // Machine time (half-T ticks) of the drive's last read or write, for the activity light.
    void touch(unsigned drive, std::uint64_t tick);
    std::uint64_t last_access(unsigned drive) const { return drive < kDrives ? drives_[drive].last_access : 0; }
    // Accessed less than `window` ticks before `now`; a time after `now` (the clock restarted since) does not count.
    bool active(unsigned drive, std::uint64_t now, std::uint64_t window) const;
    bool any_active(std::uint64_t now, std::uint64_t window) const;

private:
    struct Drive {
        std::optional<DiskImage> image;
        bool write_protected = false;
        bool dirty = false;
        std::uint64_t last_access = 0;
    };
    std::array<Drive, kDrives> drives_{};
};

}  // namespace freetvc
