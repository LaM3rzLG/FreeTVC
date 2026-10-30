// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/cards/floppy.hpp"

#include <algorithm>
#include <iterator>

#include "core/message.hpp"

namespace freetvc {

namespace {

constexpr unsigned kSector = FloppyGeometry::kSectorSize;

unsigned le16(std::span<const std::uint8_t> b, std::size_t at) { return b[at] | static_cast<unsigned>(b[at + 1]) << 8; }

// The BIOS parameter block, if it describes this many bytes of 512-byte sectors on at most two sides
std::optional<FloppyGeometry> bpb_geometry(std::span<const std::uint8_t> b) {
    if (b.size() < kSector || le16(b, 11) != kSector) {
        return std::nullopt;
    }
    const unsigned spt = le16(b, 24);
    const unsigned heads = le16(b, 26);
    const unsigned total = le16(b, 19);
    if (spt == 0 || spt > 11 || heads == 0 || heads > 2 || total == 0 || total % (spt * heads) != 0 ||
        std::size_t{total} * kSector != b.size()) {
        return std::nullopt;
    }
    const unsigned tracks = total / (spt * heads);
    if (tracks > DiskImage::kMaxTracks) {
        return std::nullopt;
    }
    return FloppyGeometry{tracks, heads, spt};
}

}  // namespace

std::optional<DiskImage> DiskImage::from_bytes(std::vector<std::uint8_t> bytes, std::string& error) {
    std::optional<FloppyGeometry> g = bpb_geometry(bytes);
    if (!g) {
        // Without a usable boot sector: 9 sectors a track, one side up to 84 tracks (VT-DOS's 360 KiB), else two
        const std::size_t track = std::size_t{9} * kSector;
        const std::size_t tracks = bytes.size() / track;
        if (bytes.empty() || bytes.size() % track != 0 || (tracks > kMaxTracks && (tracks % 2 != 0 || tracks > 2 * kMaxTracks))) {
            error = msg("%zu bytes is no disk of 9 × 512-byte sectors a track, up to 84 tracks a side", bytes.size());
            return std::nullopt;
        }
        g = tracks <= kMaxTracks ? FloppyGeometry{static_cast<unsigned>(tracks), 1, 9}
                                 : FloppyGeometry{static_cast<unsigned>(tracks / 2), 2, 9};
    }
    DiskImage image;
    image.geometry_ = *g;
    image.bytes_ = std::move(bytes);
    return image;
}

DiskImage DiskImage::blank(unsigned heads) {
    DiskImage image;
    image.geometry_ = FloppyGeometry{80, heads == 1 ? 1u : 2u, 9};
    image.bytes_.assign(image.geometry_.bytes(), 0);
    // The boot sector and FATs of the sample disks (DiskMgr's 720K and 360K layouts, design/cards.md "Disks")
    const unsigned total = 80 * image.geometry_.heads * 9;
    const unsigned fat_sectors = heads == 1 ? 2 : 3;
    const std::uint8_t media = heads == 1 ? 0xF8 : 0xF9;
    std::uint8_t* b = image.bytes_.data();
    const std::uint8_t head[] = {0xEB, 0x3C, 0x90, 'F', 'R', 'E', 'E', 'T', 'V', 'C', ' '};
    std::copy(std::begin(head), std::end(head), b);
    const auto put16 = [&](std::size_t at, unsigned v) {
        b[at] = static_cast<std::uint8_t>(v);
        b[at + 1] = static_cast<std::uint8_t>(v >> 8);
    };
    put16(11, kSector);
    b[13] = 2;  // sectors per cluster
    put16(14, 1);
    b[16] = 2;
    put16(17, 112);
    put16(19, total);
    b[21] = media;
    put16(22, fat_sectors);
    put16(24, 9);
    put16(26, image.geometry_.heads);
    b[510] = 0x55;
    b[511] = 0xAA;
    for (unsigned fat = 0; fat < 2; ++fat) {
        std::uint8_t* f = b + (1 + fat * fat_sectors) * kSector;
        f[0] = media;
        f[1] = 0xFF;
        f[2] = 0xFF;
    }
    return image;
}

const std::uint8_t* DiskImage::sector(unsigned track, unsigned head, unsigned sector) const {
    if (track >= geometry_.tracks || head >= geometry_.heads || sector == 0 || sector > geometry_.sectors) {
        return nullptr;
    }
    return bytes_.data() + ((std::size_t{track} * geometry_.heads + head) * geometry_.sectors + sector - 1) * kSector;
}

std::uint8_t* DiskImage::sector(unsigned track, unsigned head, unsigned sector) {
    return const_cast<std::uint8_t*>(static_cast<const DiskImage*>(this)->sector(track, head, sector));
}

bool DiskImage::grow_to(unsigned track) {
    if (track >= kMaxTracks) {
        return false;
    }
    if (track >= geometry_.tracks) {
        geometry_.tracks = track + 1;
        bytes_.resize(geometry_.bytes(), 0);
    }
    return true;
}

namespace {

// Root directory entries of a FAT12 volume: the entry's name as "NAME.EXT" and its bytes
template <typename Visit>
void for_each_root_entry(std::span<const std::uint8_t> b, Visit visit) {
    const std::size_t root = (le16(b, 14) + std::size_t{b[16]} * le16(b, 22)) * kSector;
    const std::size_t entries = le16(b, 17);
    for (std::size_t i = 0; i < entries && root + (i + 1) * 32 <= b.size(); ++i) {
        const std::uint8_t* e = b.data() + root + i * 32;
        if (e[0] == 0) {
            break;
        }
        // deleted entries, volume labels, directories and long-name parts are left out
        if (e[0] == 0xE5 || (e[11] & 0x18) != 0) {
            continue;
        }
        std::string name(reinterpret_cast<const char*>(e), 8);
        std::string ext(reinterpret_cast<const char*>(e + 8), 3);
        name.erase(name.find_last_not_of(' ') + 1);
        ext.erase(ext.find_last_not_of(' ') + 1);
        if (!visit(ext.empty() ? name : name + "." + ext, e)) {
            return;
        }
    }
}

}  // namespace

std::vector<std::string> DiskImage::root_files() const {
    std::vector<std::string> names;
    if (bpb_geometry(bytes_)) {
        for_each_root_entry(bytes_, [&](std::string name, const std::uint8_t*) {
            names.push_back(std::move(name));
            return true;
        });
    }
    return names;
}

std::optional<std::vector<std::uint8_t>> DiskImage::root_file(std::string_view wanted) const {
    if (!bpb_geometry(bytes_)) {
        return std::nullopt;
    }
    const std::span<const std::uint8_t> b = bytes_;
    const auto upper = [](char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 32) : c; };
    std::optional<std::vector<std::uint8_t>> out;
    for_each_root_entry(b, [&](const std::string& name, const std::uint8_t* e) {
        if (name.size() != wanted.size() || !std::equal(name.begin(), name.end(), wanted.begin(), [&](char x, char y) { return upper(x) == upper(y); })) {
            return true;
        }
        const std::size_t fat = le16(b, 14) * std::size_t{kSector};
        const std::size_t data = fat + (std::size_t{b[16]} * le16(b, 22) + le16(b, 17) * 32 / kSector) * kSector;
        const std::size_t cluster_bytes = std::size_t{b[13]} * kSector;
        std::size_t left = e[28] | e[29] << 8 | static_cast<std::size_t>(e[30]) << 16 | static_cast<std::size_t>(e[31]) << 24;
        unsigned cluster = e[26] | e[27] << 8;
        out.emplace();
        for (unsigned guard = 0; left > 0 && cluster >= 2 && cluster < 0xFF0 && guard < 4096; ++guard) {
            const std::size_t at = data + (cluster - 2) * cluster_bytes;
            if (at + cluster_bytes > b.size()) {
                break;
            }
            const std::size_t n = std::min(left, cluster_bytes);
            out->insert(out->end(), b.begin() + static_cast<std::ptrdiff_t>(at), b.begin() + static_cast<std::ptrdiff_t>(at + n));
            left -= n;
            const std::size_t entry = fat + cluster * 3 / 2;
            const unsigned pair = b[entry] | b[entry + 1] << 8;
            cluster = (cluster & 1) ? pair >> 4 : pair & 0xFFF;
        }
        return false;
    });
    return out;
}

void FloppyDrives::insert(unsigned drive, DiskImage image, bool write_protected) {
    if (drive < kDrives) {
        drives_[drive].image = std::move(image);
        drives_[drive].write_protected = write_protected;
        drives_[drive].dirty = false;
    }
}

void FloppyDrives::eject(unsigned drive) {
    if (drive < kDrives) {
        drives_[drive].image.reset();
        drives_[drive].dirty = false;
    }
}

void FloppyDrives::mark_written(unsigned drive) {
    if (drive < kDrives) {
        drives_[drive].dirty = true;
    }
}

void FloppyDrives::clear_dirty(unsigned drive) {
    if (drive < kDrives) {
        drives_[drive].dirty = false;
    }
}

void FloppyDrives::touch(unsigned drive, std::uint64_t tick) {
    if (drive < kDrives) {
        drives_[drive].last_access = tick;
    }
}

bool FloppyDrives::active(unsigned drive, std::uint64_t now, std::uint64_t window) const {
    if (drive >= kDrives) {
        return false;
    }
    const std::uint64_t last = drives_[drive].last_access;
    return last != 0 && last <= now && now - last < window;
}

bool FloppyDrives::any_active(std::uint64_t now, std::uint64_t window) const {
    for (unsigned d = 0; d < kDrives; ++d) {
        if (active(d, now, window)) {
            return true;
        }
    }
    return false;
}

}  // namespace freetvc
