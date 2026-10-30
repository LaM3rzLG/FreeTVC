// SPDX-License-Identifier: GPL-3.0-or-later
// TVC256k++ storage (design/cards.md "Storage"): the D6h commands, the C0h file functions and the four
// drives, written from [FW] TVC-IO-main-sys.c, tvc-fatfs.c, tvc-lfs.c, tvc-lfs-flash.c, tvc-lfs-psram.c and tvc-routines.c
// with their quirks. Files and directories are reopened for every command; the card keeps them open in between.
#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/cards/card256.hpp"
#include "ff.h"
#include "diskio.h"  // after ff.h, which defines its types
#include "lfs.h"

namespace freetvc {

namespace {

constexpr std::uint8_t kFaRead = 0x01;
constexpr std::uint8_t kFaWrite = 0x02;
constexpr std::uint8_t kFaCreateNew = 0x04;
constexpr std::uint8_t kFaCreateAlways = 0x08;
constexpr std::uint8_t kFaOpenAlways = 0x10;
constexpr std::uint8_t kFaSeekEnd = 0x20;
constexpr std::uint8_t kLfsReadOnly = 40;  // [FW] LFS_ERR_READONLY_FS
constexpr std::uint8_t kLfsEndOfFile = 41;
constexpr std::uint8_t kBadHandle = 20;
constexpr std::uint8_t kBadCas = 21;
constexpr std::uint32_t kPsramBytes = 8 * 1024 * 1024;
constexpr std::uint32_t kFastLimit = 256 * 1024;
constexpr std::uint32_t kBlock = 4096;
constexpr unsigned kFilinfoSize = 288;  // FatFs FILINFO with exFAT and 255-character names, as [FW] lays it out at DC00h
constexpr unsigned kSector = 512;
constexpr int kLfsReopen = LFS_O_CREAT | LFS_O_EXCL | LFS_O_TRUNC;
// [FW] TVC_RAM's MCU address is not known; only its low 16 bits matter (function 34) ❓
constexpr std::uint32_t kTvcRamAddress = 0x20020000;

// The handles the Z80 keeps are MCU pointers; any distinct values serve ❓
std::uint32_t fat_file_handle(unsigned i) { return 0x20004000u + i * 0x230u; }
std::uint32_t fat_dir_handle(unsigned i) { return 0x20004900u + i * 0x50u; }
std::uint32_t lfs_file_handle(unsigned i) { return 0x20005000u + i * 0x54u; }
std::uint32_t lfs_dir_handle(unsigned i) { return 0x20005200u + i * 0x40u; }

template <typename F>
int slot_of(std::uint32_t handle, F address) {
    for (unsigned i = 0; i < 4; ++i) {
        if (address(i) == handle) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

bool ends_with_cas(const std::string& name) {
    if (name.size() < 4) {
        return false;
    }
    const std::string tail = name.substr(name.size() - 4);
    return tail.size() == 4 && tail[0] == '.' && std::tolower(static_cast<unsigned char>(tail[1])) == 'c' &&
           std::tolower(static_cast<unsigned char>(tail[2])) == 'a' && std::tolower(static_cast<unsigned char>(tail[3])) == 's';
}

std::string lowered(std::string s) {
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

bool same_ascii(const std::string& a, const std::string& b) { return lowered(a) == lowered(b); }

// A C string's byte, 0 past its end (what a zero-padded firmware buffer holds there)
char char_at(const std::string& s, std::size_t i) { return i < s.size() ? s[i] : '\0'; }

// ---- FatFs's disk: one at a time, while a volume is mounted ----

using DskBacking = FatDisk;

// FatFs's drives, one per mounted image
thread_local std::array<DskBacking*, FF_VOLUMES> t_disks{};

DskBacking* disk(BYTE pdrv) { return pdrv < t_disks.size() ? t_disks[pdrv] : nullptr; }

// A free FatFs drive for `backing`, or -1
int claim_drive(DskBacking* backing) {
    for (std::size_t i = 0; i < t_disks.size(); ++i) {
        if (!t_disks[i]) {
            t_disks[i] = backing;
            return static_cast<int>(i);
        }
    }
    return -1;
}

class MemoryBacking final : public DskBacking {
public:
    explicit MemoryBacking(std::vector<std::uint8_t>& image) : image_(image) {}
    bool read(std::uint64_t pos, std::span<std::uint8_t> out) override {
        if (pos + out.size() > image_.size()) {
            return false;
        }
        std::copy_n(image_.begin() + static_cast<std::ptrdiff_t>(pos), out.size(), out.begin());
        return true;
    }
    bool write(std::uint64_t pos, std::span<const std::uint8_t> in) override {
        if (pos + in.size() > image_.size()) {
            return false;
        }
        std::copy(in.begin(), in.end(), image_.begin() + static_cast<std::ptrdiff_t>(pos));
        return true;
    }
    std::uint64_t size() override { return image_.size(); }

private:
    std::vector<std::uint8_t>& image_;
};

// A DSK image that is a file on the pendrive
class VolumeBacking final : public DskBacking {
public:
    VolumeBacking(FatVolume& volume, std::string path) : volume_(volume), path_(std::move(path)) {
        FatEntry e;
        size_ = volume_.stat(path_, e) == kFrOk ? e.size : 0;
    }
    bool read(std::uint64_t pos, std::span<std::uint8_t> out) override {
        std::size_t got = 0;
        return volume_.read(path_, pos, out, got) == kFrOk && got == out.size();
    }
    bool write(std::uint64_t pos, std::span<const std::uint8_t> in) override {
        std::size_t put = 0;
        return pos + in.size() <= size_ && volume_.write(path_, pos, in, put) == kFrOk && put == in.size();
    }
    std::uint64_t size() override { return size_; }

private:
    FatVolume& volume_;
    std::string path_;
    std::uint64_t size_ = 0;
};

// A DSK image that is a file on the PSRAM or flash drive, open for the command ([FW] dsk_file_lfs)
class LfsBacking final : public DskBacking {
public:
    LfsBacking(lfs_t* fs, const std::string& path) : fs_(fs) { open_ = lfs_file_open(fs_, &file_, path.c_str(), LFS_O_RDWR) == 0; }
    ~LfsBacking() override {
        if (open_) {
            lfs_file_close(fs_, &file_);
        }
    }
    bool ok() const { return open_; }
    bool read(std::uint64_t pos, std::span<std::uint8_t> out) override {
        return lfs_file_seek(fs_, &file_, static_cast<lfs_soff_t>(pos), LFS_SEEK_SET) >= 0 &&
               lfs_file_read(fs_, &file_, out.data(), static_cast<lfs_size_t>(out.size())) == static_cast<lfs_ssize_t>(out.size());
    }
    bool write(std::uint64_t pos, std::span<const std::uint8_t> in) override {
        return lfs_file_seek(fs_, &file_, static_cast<lfs_soff_t>(pos), LFS_SEEK_SET) >= 0 &&
               lfs_file_write(fs_, &file_, in.data(), static_cast<lfs_size_t>(in.size())) == static_cast<lfs_ssize_t>(in.size());
    }
    std::uint64_t size() override { return open_ ? static_cast<std::uint64_t>(std::max<lfs_soff_t>(0, lfs_file_size(fs_, &file_))) : 0; }

private:
    lfs_t* fs_;
    lfs_file_t file_{};
    bool open_ = false;
};

FatEntry entry_of(const FILINFO& fi) {
    return {fi.fname, fi.altname, fi.fattrib, fi.fsize, fi.fdate, fi.ftime};
}

// A FAT image through FatFs (the card's own library), on a FatFs drive of its own while this object lives
class FatImage final : public FatVolume {
public:
    explicit FatImage(std::unique_ptr<DskBacking> backing) : owned_(std::move(backing)), backing_(owned_.get()) {}
    explicit FatImage(DskBacking& backing) : backing_(&backing) {}
    ~FatImage() override {
        if (drive_ >= 0) {
            f_mount(nullptr, prefix_.c_str(), 0);
            t_disks[static_cast<std::size_t>(drive_)] = nullptr;
        }
    }
    FatImage(const FatImage&) = delete;
    FatImage& operator=(const FatImage&) = delete;

    std::uint8_t mount() {
        drive_ = claim_drive(backing_);
        if (drive_ < 0) {
            return kFrNotEnabled;
        }
        prefix_ = std::to_string(drive_) + ":";
        const FRESULT r = f_mount(fs_.get(), prefix_.c_str(), 1);
        if (r != FR_OK) {
            f_mount(nullptr, prefix_.c_str(), 0);
            t_disks[static_cast<std::size_t>(drive_)] = nullptr;
            drive_ = -1;
        }
        return static_cast<std::uint8_t>(r);
    }

    std::uint8_t stat(const std::string& path, FatEntry& entry) override {
        FILINFO fi{};
        const FRESULT r = f_stat(on(path).c_str(), &fi);
        if (r == FR_OK) {
            entry = entry_of(fi);
        }
        return static_cast<std::uint8_t>(r);
    }
    std::uint8_t list(const std::string& dir, std::vector<FatEntry>& entries) override {
        DIR d{};
        FRESULT r = f_opendir(&d, on(dir).c_str());
        FILINFO fi{};
        while (r == FR_OK && (r = f_readdir(&d, &fi)) == FR_OK && fi.fname[0] != 0) {
            entries.push_back(entry_of(fi));
        }
        f_closedir(&d);
        return static_cast<std::uint8_t>(r);
    }
    std::uint8_t read(const std::string& path, std::uint64_t pos, std::span<std::uint8_t> out, std::size_t& got) override {
        FIL f{};
        FRESULT r = f_open(&f, on(path).c_str(), FA_READ);
        UINT n = 0;
        if (r == FR_OK && (r = f_lseek(&f, static_cast<FSIZE_t>(pos))) == FR_OK) {
            r = f_read(&f, out.data(), static_cast<UINT>(out.size()), &n);
        }
        got = n;
        f_close(&f);
        return static_cast<std::uint8_t>(r);
    }
    std::uint8_t write(const std::string& path, std::uint64_t pos, std::span<const std::uint8_t> data, std::size_t& put) override {
        FIL f{};
        FRESULT r = f_open(&f, on(path).c_str(), FA_WRITE | FA_OPEN_EXISTING);
        UINT n = 0;
        if (r == FR_OK && (r = f_lseek(&f, static_cast<FSIZE_t>(pos))) == FR_OK) {
            r = f_write(&f, data.data(), static_cast<UINT>(data.size()), &n);
        }
        put = n;
        const FRESULT c = f_close(&f);
        return static_cast<std::uint8_t>(r != FR_OK ? r : c);
    }
    std::uint8_t create(const std::string& path) override {
        FIL f{};
        const FRESULT r = f_open(&f, on(path).c_str(), FA_WRITE | FA_CREATE_ALWAYS);
        if (r == FR_OK) {
            return static_cast<std::uint8_t>(f_close(&f));
        }
        return static_cast<std::uint8_t>(r);
    }
    std::uint8_t make_dir(const std::string& path) override { return static_cast<std::uint8_t>(f_mkdir(on(path).c_str())); }
    std::uint8_t remove(const std::string& path) override { return static_cast<std::uint8_t>(f_unlink(on(path).c_str())); }
    std::uint8_t rename(const std::string& from, const std::string& to) override {
        return static_cast<std::uint8_t>(f_rename(on(from).c_str(), to.c_str()));
    }

private:
    std::string on(const std::string& path) const { return prefix_ + path; }

    std::unique_ptr<DskBacking> owned_;
    DskBacking* backing_;
    std::unique_ptr<FATFS> fs_ = std::make_unique<FATFS>();
    int drive_ = -1;
    std::string prefix_;
};

// ---- littlefs block devices ----

// The card's littlefs geometry ([FW] tvc-lfs-flash.c cfg, tvc-lfs-psram.c psram_drive_cfg)
struct LfsMount {
    lfs_t fs{};
    lfs_config cfg{};
    alignas(8) std::array<std::uint8_t, 256> read_buffer{};
    alignas(8) std::array<std::uint8_t, 256> prog_buffer{};
    alignas(8) std::array<std::uint8_t, 16> lookahead_buffer{};
    const std::uint8_t* data = nullptr;  // a memory image
    std::uint8_t* writable = nullptr;
    std::size_t size = 0;
    bool mounted = false;
    int error = 0;

    void setup(void* context, lfs_size_t blocks) {
        cfg.context = context;
        cfg.read_size = 256;
        cfg.prog_size = 256;
        cfg.block_size = kBlock;
        cfg.block_count = blocks;
        cfg.block_cycles = 500;
        cfg.cache_size = 256;
        cfg.lookahead_size = 16;
        cfg.read_buffer = read_buffer.data();
        cfg.prog_buffer = prog_buffer.data();
        cfg.lookahead_buffer = lookahead_buffer.data();
    }
};

int memory_read(const lfs_config* c, lfs_block_t block, lfs_off_t off, void* buffer, lfs_size_t size) {
    const auto* m = static_cast<const LfsMount*>(c->context);
    if (block >= c->block_count || off >= c->block_size) {
        return LFS_ERR_INVAL;
    }
    const std::size_t pos = static_cast<std::size_t>(block) * c->block_size + off;
    if (pos + size > m->size) {
        return LFS_ERR_IO;
    }
    std::memcpy(buffer, m->data + pos, size);
    return 0;
}

int memory_prog(const lfs_config* c, lfs_block_t block, lfs_off_t off, const void* buffer, lfs_size_t size) {
    auto* m = static_cast<LfsMount*>(c->context);
    const std::size_t pos = static_cast<std::size_t>(block) * c->block_size + off;
    if (!m->writable || block >= c->block_count || pos + size > m->size) {
        return LFS_ERR_IO;  // the flash image is read-only here; the card would program its flash ❓
    }
    std::memcpy(m->writable + pos, buffer, size);
    return 0;
}

int memory_erase(const lfs_config* c, lfs_block_t block) {
    auto* m = static_cast<LfsMount*>(c->context);
    const std::size_t pos = static_cast<std::size_t>(block) * c->block_size;
    if (!m->writable || block >= c->block_count || pos + c->block_size > m->size) {
        return LFS_ERR_IO;
    }
    std::memset(m->writable + pos, 0xFF, c->block_size);
    return 0;
}

int lfs_no_sync(const lfs_config*) { return 0; }

void memory_device(LfsMount& m, const std::uint8_t* data, std::uint8_t* writable, std::size_t size) {
    m.data = data;
    m.writable = writable;
    m.size = size;
    m.setup(&m, static_cast<lfs_size_t>(size / kBlock));
    m.cfg.read = memory_read;
    m.cfg.prog = memory_prog;
    m.cfg.erase = memory_erase;
    m.cfg.sync = lfs_no_sync;
}

}  // namespace

}  // namespace freetvc

// FatFs's disk I/O: each drive is the backing of one mounted image
extern "C" {

DSTATUS disk_status(BYTE pdrv) { return freetvc::disk(pdrv) ? 0 : STA_NOINIT; }

DSTATUS disk_initialize(BYTE pdrv) { return freetvc::disk(pdrv) ? 0 : STA_NOINIT; }

DRESULT disk_read(BYTE pdrv, BYTE* buff, LBA_t sector, UINT count) {
    freetvc::DskBacking* d = freetvc::disk(pdrv);
    if (!d) {
        return RES_NOTRDY;
    }
    return d->read(static_cast<std::uint64_t>(sector) * freetvc::kSector, {buff, count * std::size_t{freetvc::kSector}}) ? RES_OK : RES_ERROR;
}

DRESULT disk_write(BYTE pdrv, const BYTE* buff, LBA_t sector, UINT count) {
    freetvc::DskBacking* d = freetvc::disk(pdrv);
    if (!d) {
        return RES_NOTRDY;
    }
    return d->write(static_cast<std::uint64_t>(sector) * freetvc::kSector, {buff, count * std::size_t{freetvc::kSector}}) ? RES_OK : RES_ERROR;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void* buff) {
    freetvc::DskBacking* d = freetvc::disk(pdrv);
    if (!d) {
        return RES_NOTRDY;
    }
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *static_cast<LBA_t*>(buff) = static_cast<LBA_t>(d->size() / freetvc::kSector);
        return RES_OK;
    case GET_SECTOR_SIZE:
        *static_cast<WORD*>(buff) = freetvc::kSector;
        return RES_OK;
    case GET_BLOCK_SIZE:
        *static_cast<DWORD*>(buff) = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

}  // extern "C"

namespace freetvc {

// ---- One command's view of the drives ----

class Card256::Session {
public:
    Session(Card256& card, const CardBus& bus) : card_(card), media_(bus.card256) {
        if (media_) {
            flash_ = media_->flash;
        }
    }
    ~Session() {
        dsk_.reset();
        for (auto* m : {&psram_mount_, &flash_mount_}) {
            if (*m && (*m)->mounted) {
                lfs_unmount(&(*m)->fs);
            }
        }
    }
    Session(const Session&) = delete;
    Session& operator=(const Session&) = delete;

    Card256Media* media() const { return media_; }

    // The PSRAM (1) or flash (2) drive, mounted on first use; null with `error` when it does not mount
    lfs_t* lfs(std::uint8_t drive, int& error) {
        std::unique_ptr<LfsMount>& m = drive == 1 ? psram_mount_ : flash_mount_;
        if (!m) {
            m = std::make_unique<LfsMount>();
            if (drive == 1) {
                psram_device(*m, card_.psram_blocks_);
            } else if (flash_) {
                memory_device(*m, flash_->data(), nullptr, flash_->size());
            } else {
                m->error = LFS_ERR_IO;
                error = m->error;
                return nullptr;
            }
            m->error = lfs_mount(&m->fs, &m->cfg);
            m->mounted = m->error == 0;
        }
        error = m->error;
        return m->mounted ? &m->fs : nullptr;
    }

    // [FW] psram_drive_init's format and mount
    int format_psram(std::uint32_t blocks) {
        dsk_.reset();
        if (psram_mount_ && psram_mount_->mounted) {
            lfs_unmount(&psram_mount_->fs);
        }
        psram_mount_ = std::make_unique<LfsMount>();
        psram_device(*psram_mount_, blocks);
        if (blocks < 2) {
            psram_mount_->error = LFS_ERR_INVAL;  // the firmware's littlefs has no guard here ❓
            return psram_mount_->error;
        }
        int r = lfs_format(&psram_mount_->fs, &psram_mount_->cfg);
        if (r >= 0) {
            r = lfs_mount(&psram_mount_->fs, &psram_mount_->cfg);
        }
        psram_mount_->error = r;
        psram_mount_->mounted = r == 0;
        return r;
    }

    void drop_dsk() { dsk_.reset(); }

    // FatFs drive 0 (the pendrive) or 1 (the DSK); null with `error` when FatFs has no volume there
    FatVolume* fat(std::uint8_t volume, std::uint8_t& error) {
        if (volume == 0) {
            if (card_.usb_ == Usb::Pendrive && media_ && media_->pendrive) {
                return media_->pendrive;
            }
            error = kFrNotEnabled;
            return nullptr;
        }
        if (volume != 1) {
            error = kFrInvalidDrive;
            return nullptr;
        }
        if (card_.dsk_type_ == 0) {
            error = kFrNotEnabled;
            return nullptr;
        }
        if (!dsk_) {
            error = mount_dsk();
            if (error != kFrOk) {
                return nullptr;
            }
        }
        return dsk_.get();
    }

    // Mounts the DSK image dsk_type_ and dsk_path_ name
    std::uint8_t mount_dsk() {
        std::unique_ptr<DskBacking> backing;
        if (card_.dsk_type_ == 1) {
            std::uint8_t e = 0;
            FatVolume* usb = fat(0, e);
            if (!usb) {
                return e;
            }
            backing = std::make_unique<VolumeBacking>(*usb, card_.dsk_path_);
        } else {
            int e = 0;
            lfs_t* fs = lfs(static_cast<std::uint8_t>(card_.dsk_type_ - 1), e);
            if (!fs) {
                return kFrDiskErr;
            }
            auto lb = std::make_unique<LfsBacking>(fs, card_.dsk_path_);
            if (!lb->ok()) {
                return kFrDiskErr;
            }
            backing = std::move(lb);
        }
        auto image = std::make_unique<FatImage>(std::move(backing));
        if (const std::uint8_t r = image->mount(); r != kFrOk) {
            return r;
        }
        dsk_ = std::move(image);
        return kFrOk;
    }

    // ---- FatFs's API on its current drive (card.fat_drive_) and the drives' working directories ----

    // Splits FatFs's "N:" drive prefix off `name` (get_ldnumber, two drives)
    std::uint8_t drive_of(std::string& name, std::uint8_t& volume) const {
        volume = card_.fat_drive_;
        std::size_t t = 0;
        while (t < name.size() && static_cast<unsigned char>(name[t]) >= ' ' && name[t] != ':') {
            ++t;
        }
        if (t == name.size() || name[t] != ':') {
            return kFrOk;
        }
        if (t == 1 && (name[0] == '0' || name[0] == '1')) {
            volume = static_cast<std::uint8_t>(name[0] - '0');
            name.erase(0, 2);
            return kFrOk;
        }
        return kFrInvalidDrive;
    }

    // FatFs's path walk (follow_path, create_name with long names and FF_FS_RPATH 2): `no_name` when it ends in the
    // directory itself ("", ".", "..")
    std::uint8_t resolve(std::uint8_t volume, const std::string& name, std::string& path, bool& no_name) const {
        std::vector<std::string> parts;
        std::size_t i = 0;
        const auto separator = [&](std::size_t k) { return k < name.size() && (name[k] == '/' || name[k] == '\\'); };
        if (separator(0)) {
            while (separator(i)) {
                ++i;
            }
        } else {
            const std::string& cwd = card_.fat_cwd_[volume & 1];
            for (std::size_t p = 1; p < cwd.size();) {
                const std::size_t e = cwd.find('/', p);
                parts.push_back(cwd.substr(p, e == std::string::npos ? std::string::npos : e - p));
                p = e == std::string::npos ? cwd.size() : e + 1;
            }
        }
        no_name = true;
        while (i < name.size()) {
            std::string comp;
            while (i < name.size() && !separator(i)) {
                comp += name[i++];
            }
            while (separator(i)) {
                ++i;
            }
            if (comp == "." || comp == "..") {
                if (comp == ".." && !parts.empty()) {
                    parts.pop_back();
                }
                no_name = true;
                continue;
            }
            for (const char ch : comp) {
                const auto c = static_cast<unsigned char>(ch);
                if (c < 0x20 || c == 0x7F || std::strchr("*:<>|\"?", c)) {
                    return kFrInvalidName;
                }
            }
            while (!comp.empty() && (comp.back() == ' ' || comp.back() == '.')) {
                comp.pop_back();
            }
            if (comp.empty() || comp.size() > 255) {
                return kFrInvalidName;
            }
            parts.push_back(std::move(comp));
            no_name = false;
        }
        path.clear();
        for (const std::string& p : parts) {
            path += "/" + p;
        }
        if (path.empty()) {
            path = "/";
        }
        return kFrOk;
    }

    struct Target {
        std::uint8_t volume = 0;
        FatVolume* v = nullptr;
        std::string path;
        bool no_name = false;
    };

    std::uint8_t target(std::string name, Target& t) {
        if (const std::uint8_t r = drive_of(name, t.volume)) {
            return r;
        }
        std::uint8_t error = 0;
        t.v = fat(t.volume, error);
        if (!t.v) {
            return error;
        }
        return resolve(t.volume, name, t.path, t.no_name);
    }

    std::uint8_t f_stat(const std::string& name, FatEntry& e) {
        Target t;
        if (const std::uint8_t r = target(name, t)) {
            return r;
        }
        return t.no_name ? kFrInvalidName : t.v->stat(t.path, e);
    }

    std::uint8_t f_open(FatFile& f, const std::string& name, std::uint8_t mode) {
        mode &= kFaRead | kFaWrite | kFaCreateAlways | kFaCreateNew | kFaOpenAlways | kFaSeekEnd;
        Target t;
        if (const std::uint8_t r = target(name, t)) {
            return r;
        }
        FatEntry e;
        std::uint8_t r = t.no_name ? kFrInvalidName : t.v->stat(t.path, e);
        bool created = false;
        if (mode & (kFaCreateAlways | kFaOpenAlways | kFaCreateNew)) {
            if (r == kFrNoFile) {
                r = t.v->create(t.path);
                created = true;
            } else if (r == kFrOk) {
                if (e.attr & (kAmReadOnly | kAmDir)) {
                    r = kFrDenied;
                } else if (mode & kFaCreateNew) {
                    r = kFrExist;
                }
            }
            if (r == kFrOk && (mode & kFaCreateAlways) && !created) {
                r = t.v->create(t.path);
            }
        } else if (r == kFrOk) {
            if (e.attr & kAmDir) {
                r = kFrNoFile;
            } else if ((mode & kFaWrite) && (e.attr & kAmReadOnly)) {
                r = kFrDenied;
            }
        }
        if (r != kFrOk) {
            return r;
        }
        f.open = true;
        f.volume = t.volume;
        f.path = t.path;
        f.pos = 0;
        f.mode = mode;
        if (mode & kFaSeekEnd) {
            f.pos = size_of(f);
        }
        return kFrOk;
    }

    FatVolume* volume_of(const FatFile& f) {
        std::uint8_t e = 0;
        return f.open ? fat(f.volume, e) : nullptr;
    }

    std::uint64_t size_of(FatFile& f) {
        FatEntry e;
        FatVolume* v = volume_of(f);
        return v && v->stat(f.path, e) == kFrOk ? e.size : 0;
    }

    std::uint8_t f_read(FatFile& f, std::span<std::uint8_t> out, std::size_t& got) {
        got = 0;
        FatVolume* v = volume_of(f);
        if (!v) {
            return kFrInvalidObject;
        }
        if (!(f.mode & kFaRead)) {
            return kFrDenied;
        }
        const std::uint64_t size = size_of(f);
        const std::uint64_t n = f.pos < size ? std::min<std::uint64_t>(out.size(), size - f.pos) : 0;
        if (n == 0) {
            return kFrOk;
        }
        const std::uint8_t r = v->read(f.path, f.pos, out.first(static_cast<std::size_t>(n)), got);
        f.pos += got;
        return r;
    }

    std::uint8_t f_write(FatFile& f, std::span<const std::uint8_t> data, std::size_t& put) {
        put = 0;
        FatVolume* v = volume_of(f);
        if (!v) {
            return kFrInvalidObject;
        }
        if (!(f.mode & kFaWrite)) {
            return kFrDenied;
        }
        const std::uint8_t r = v->write(f.path, f.pos, data, put);
        f.pos += put;
        return r;
    }

    std::uint8_t f_lseek(FatFile& f, std::uint64_t pos) {
        FatVolume* v = volume_of(f);
        if (!v) {
            return kFrInvalidObject;
        }
        const std::uint64_t size = size_of(f);
        if (pos > size) {
            if (!(f.mode & kFaWrite)) {
                pos = size;
            } else {
                const std::vector<std::uint8_t> gap(static_cast<std::size_t>(pos - size), 0);
                std::size_t put = 0;
                if (const std::uint8_t r = v->write(f.path, size, gap, put); r != kFrOk) {
                    return r;
                }
            }
        }
        f.pos = pos;
        return kFrOk;
    }

    std::uint8_t f_close(FatFile& f) {
        if (!volume_of(f)) {
            return kFrInvalidObject;
        }
        f.open = false;
        return kFrOk;
    }

    std::uint8_t f_opendir(FatDir& d, const std::string& name) {
        Target t;
        if (const std::uint8_t r = target(name, t)) {
            return r;
        }
        if (!t.no_name) {
            FatEntry e;
            std::uint8_t r = t.v->stat(t.path, e);
            if (r == kFrOk && !(e.attr & kAmDir)) {
                r = kFrNoPath;
            }
            if (r != kFrOk) {
                return r == kFrNoFile ? kFrNoPath : r;
            }
        }
        d.open = true;
        d.volume = t.volume;
        d.path = t.path;
        d.index = 0;
        return kFrOk;
    }

    // `end` at the end of the directory (FatFs: an empty name)
    std::uint8_t f_readdir(FatDir& d, FatEntry& e, bool& end) {
        std::uint8_t error = 0;
        FatVolume* v = d.open ? fat(d.volume, error) : nullptr;
        if (!v) {
            return kFrInvalidObject;
        }
        std::vector<FatEntry> entries;
        if (const std::uint8_t r = v->list(d.path, entries); r != kFrOk) {
            return r;
        }
        end = d.index >= entries.size();
        if (!end) {
            e = entries[d.index++];
        }
        return kFrOk;
    }

    std::uint8_t f_getcwd(std::string& out) {
        std::uint8_t error = 0;
        if (!fat(card_.fat_drive_, error)) {
            return error;
        }
        out = std::to_string(card_.fat_drive_) + ":" + card_.fat_cwd_[card_.fat_drive_ & 1];
        return kFrOk;
    }

    // The path with every name as the volume stores it
    std::string canonical(FatVolume* v, const std::string& path) {
        std::string out;
        for (std::size_t p = 1; p < path.size();) {
            const std::size_t e = path.find('/', p);
            const std::size_t end = e == std::string::npos ? path.size() : e;
            FatEntry entry;
            const std::string prefix = path.substr(0, end);
            out += "/" + (v->stat(prefix, entry) == kFrOk ? entry.name : path.substr(p, end - p));
            p = end + 1;
        }
        return out.empty() ? "/" : out;
    }

    std::uint8_t f_chdir(const std::string& name) {
        Target t;
        if (const std::uint8_t r = target(name, t)) {
            return r;
        }
        if (!t.no_name) {
            FatEntry e;
            std::uint8_t r = t.v->stat(t.path, e);
            if (r == kFrOk && !(e.attr & kAmDir)) {
                r = kFrNoPath;
            }
            if (r != kFrOk) {
                return r == kFrNoFile ? kFrNoPath : r;
            }
        }
        card_.fat_cwd_[t.volume & 1] = canonical(t.v, t.path);
        return kFrOk;
    }

    std::uint8_t f_mkdir(const std::string& name) {
        Target t;
        if (const std::uint8_t r = target(name, t)) {
            return r;
        }
        if (t.no_name) {
            return kFrExist;
        }
        FatEntry e;
        const std::uint8_t r = t.v->stat(t.path, e);
        if (r == kFrOk) {
            return kFrExist;
        }
        return r == kFrNoFile ? t.v->make_dir(t.path) : r;
    }

    std::uint8_t f_unlink(const std::string& name) {
        Target t;
        if (const std::uint8_t r = target(name, t)) {
            return r;
        }
        if (t.no_name) {
            return kFrInvalidName;
        }
        FatEntry e;
        if (const std::uint8_t r = t.v->stat(t.path, e); r != kFrOk) {
            return r;
        }
        if (e.attr & kAmReadOnly) {
            return kFrDenied;
        }
        if (e.attr & kAmDir) {
            std::vector<FatEntry> entries;
            if (same_ascii(t.path, card_.fat_cwd_[t.volume & 1]) || t.v->list(t.path, entries) != kFrOk || !entries.empty()) {
                return kFrDenied;
            }
        }
        return t.v->remove(t.path);
    }

    std::uint8_t f_rename(const std::string& from, std::string to) {
        Target t;
        if (const std::uint8_t r = target(from, t)) {
            return r;
        }
        if (t.no_name) {
            return kFrInvalidName;
        }
        FatEntry e;
        if (const std::uint8_t r = t.v->stat(t.path, e); r != kFrOk) {
            return r;
        }
        std::uint8_t ignored = 0;
        drive_of(to, ignored);
        std::string path;
        bool no_name = false;
        if (const std::uint8_t r = resolve(t.volume, to, path, no_name)) {
            return r;
        }
        if (no_name) {
            return kFrExist;
        }
        const std::uint8_t r = t.v->stat(path, e);
        if (r == kFrOk && !same_ascii(path, t.path)) {
            return kFrExist;
        }
        if (r != kFrOk && r != kFrNoFile) {
            return r;
        }
        return t.v->rename(t.path, path);
    }

private:
    static int psram_read_block(const lfs_config* c, lfs_block_t block, lfs_off_t off, void* buffer, lfs_size_t size) {
        if (block >= c->block_count || off >= c->block_size) {
            return LFS_ERR_INVAL;
        }
        static_cast<Session*>(c->context)->psram_get(block * kBlock + off, static_cast<std::uint8_t*>(buffer), size);
        return 0;
    }
    static int psram_prog_block(const lfs_config* c, lfs_block_t block, lfs_off_t off, const void* buffer, lfs_size_t size) {
        if (block >= c->block_count || off >= c->block_size) {
            return LFS_ERR_INVAL;
        }
        static_cast<Session*>(c->context)->psram_put(block * kBlock + off, static_cast<const std::uint8_t*>(buffer), size);
        return 0;
    }
    static int psram_erase_block(const lfs_config* c, lfs_block_t block) {
        static_cast<Session*>(c->context)->card_.psram_fill(block * kBlock, kBlock, 0xFF);
        return 0;
    }

    void psram_device(LfsMount& m, std::uint32_t blocks) {
        m.setup(this, blocks);
        m.cfg.read = psram_read_block;
        m.cfg.prog = psram_prog_block;
        m.cfg.erase = psram_erase_block;
        m.cfg.sync = lfs_no_sync;
    }

    void psram_get(std::uint32_t pos, std::uint8_t* out, std::size_t n) {
        while (n > 0 && pos < kPsramSize) {
            const std::uint32_t off = pos % kPageSize;
            const std::size_t chunk = std::min<std::size_t>(n, kPageSize - off);
            if (const auto& p = card_.psram_[pos / kPageSize]) {
                std::memcpy(out, p->data() + off, chunk);
            } else {
                std::memset(out, 0, chunk);
            }
            out += chunk;
            pos += static_cast<std::uint32_t>(chunk);
            n -= chunk;
        }
        std::memset(out, 0, n);
    }

    void psram_put(std::uint32_t pos, const std::uint8_t* in, std::size_t n) {
        while (n > 0 && pos < kPsramSize) {
            const std::uint32_t off = pos % kPageSize;
            const std::size_t chunk = std::min<std::size_t>(n, kPageSize - off);
            std::memcpy(card_.psram_page_for_write(pos / kPageSize) + off, in, chunk);
            in += chunk;
            pos += static_cast<std::uint32_t>(chunk);
            n -= chunk;
        }
    }

    Card256& card_;
    Card256Media* media_;
    std::shared_ptr<const std::vector<std::uint8_t>> flash_;
    std::unique_ptr<LfsMount> psram_mount_;
    std::unique_ptr<LfsMount> flash_mount_;
    std::unique_ptr<FatImage> dsk_;
};

// ---- Card RAM ----

std::uint32_t Card256::rom32(unsigned offset) const {
    return rom(offset) | rom(offset + 1) << 8 | rom(offset + 2) << 16 | static_cast<std::uint32_t>(rom(offset + 3)) << 24;
}

void Card256::set_rom32(unsigned offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) {
        set_rom(offset + i, static_cast<std::uint8_t>(value >> (8 * i)));
    }
}

// A C string in card RAM, at most `length` bytes
std::string Card256::rom_string(unsigned offset, unsigned length) const {
    std::string s;
    for (unsigned i = 0; i < length && offset + i < iomem_.size() && iomem_[offset + i] != 0; ++i) {
        s += static_cast<char>(iomem_[offset + i]);
    }
    return s;
}

void Card256::put_rom_string(unsigned offset, const std::string& text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        set_rom(offset + static_cast<unsigned>(i), static_cast<std::uint8_t>(text[i]));
    }
    set_rom(offset + static_cast<unsigned>(text.size()), 0);
}

void Card256::lower_rom(unsigned offset) {
    for (unsigned i = 0; i < 256 && rom(offset + i) != 0; ++i) {
        set_rom(offset + i, static_cast<std::uint8_t>(std::tolower(rom(offset + i))));
    }
}

void Card256::psram_fill(std::uint32_t pos, std::uint32_t n, std::uint8_t value) {
    for (std::uint32_t end = std::min(pos + n, static_cast<std::uint32_t>(kPsramSize)); pos < end;) {
        const std::uint32_t off = pos % kPageSize;
        const std::uint32_t chunk = std::min(end - pos, static_cast<std::uint32_t>(kPageSize) - off);
        std::memset(psram_page_for_write(pos / kPageSize) + off, value, chunk);
        pos += chunk;
    }
}

// FatFs FILINFO at DC00h ([FW] tvc_fatfs_read_dir reads into card RAM)
void Card256::put_filinfo(const FatEntry& e) {
    for (unsigned i = 0; i < 8; ++i) {
        set_rom(0x1C00 + i, static_cast<std::uint8_t>(e.size >> (8 * i)));
    }
    set_rom(0x1C08, static_cast<std::uint8_t>(e.date));
    set_rom(0x1C09, static_cast<std::uint8_t>(e.date >> 8));
    set_rom(0x1C0A, static_cast<std::uint8_t>(e.time));
    set_rom(0x1C0B, static_cast<std::uint8_t>(e.time >> 8));
    set_rom(0x1C0C, e.attr);
    put_rom_string(0x1C0D, e.alt.substr(0, 12));
    put_rom_string(0x1C1A, e.name.substr(0, 255));
}

// ---- The D6h commands ([FW] TVC-IO-main-sys.c process_queue_cycle, REG_USB_MSC_CMD) ----

std::uint8_t Card256::storage_command(std::uint8_t code, const CardBus& bus) {
    Session s(*this, bus);
    return storage_dispatch(s, code);
}

// Status: 00h done, result | 80h failed, FFh no such command
std::uint8_t Card256::storage_dispatch(Session& s, std::uint8_t code) {
    const std::uint8_t drive = rom(0x1810);
    const auto by_drive = [&](auto fat, auto lfs) -> std::uint8_t {
        switch (drive) {
        case 0:
            return fat(std::uint8_t{0});
        case 1:
        case 2:
            return lfs(drive);
        case 3:
            return fat(std::uint8_t{1});
        default:
            return 0xFF;
        }
    };
    // The flash drive's wrappers check that it mounted, except where they refuse at once ([FW] tvc-lfs-flash.c)
    const auto ready = [&](std::uint8_t d) { return d == 1 ? psram_drive_ : flash_ok_; };
    const auto lfs_op = [&](auto op) {
        return [&, op](std::uint8_t d) -> std::uint8_t { return ready(d) ? op(d) : kFrNotReady; };
    };
    const auto flash_refuses = [&](auto op, std::uint8_t refusal) {
        return [&, op, refusal](std::uint8_t d) -> std::uint8_t {
            if (d == 2) {
                return refusal;
            }
            return ready(d) ? op(d) : kFrNotReady;
        };
    };
    std::uint8_t r = 0xFF;
    switch (code) {
    case 0x01:
        r = by_drive([&](std::uint8_t v) { return fat_open(s, v); }, lfs_op([&](std::uint8_t d) { return lfs_open(s, d); }));
        break;
    case 0x02:
        r = by_drive([&](std::uint8_t) { return fat_close(s); }, lfs_op([&](std::uint8_t) { return lfs_close(s); }));
        break;
    case 0x03:
        r = by_drive([&](std::uint8_t) { return fat_read(s); }, lfs_op([&](std::uint8_t) { return lfs_read(s); }));
        break;
    case 0x04:
        r = by_drive([&](std::uint8_t) { return fat_write(s); }, flash_refuses([&](std::uint8_t) { return lfs_write(s); }, kFrWriteProtected));
        break;
    case 0x05:
        r = by_drive([&](std::uint8_t v) { return fat_open_dir(s, v); }, lfs_op([&](std::uint8_t d) { return lfs_open_dir(s, d); }));
        break;
    case 0x06:
        r = by_drive([&](std::uint8_t) { return fat_close_dir(s); }, lfs_op([&](std::uint8_t) { return lfs_close_dir(s); }));
        break;
    case 0x07:
        r = by_drive([&](std::uint8_t) { return fat_read_dir(s); }, lfs_op([&](std::uint8_t) { return lfs_read_dir(s); }));
        break;
    case 0x08:
        r = by_drive([&](std::uint8_t) { return fat_write_source(s); },
                     flash_refuses([&](std::uint8_t) { return lfs_write_source(s); }, kLfsReadOnly));
        break;
    case 0x09:
        r = by_drive([&](std::uint8_t) { return fat_read_dest(s); }, lfs_op([&](std::uint8_t) { return lfs_read_dest(s); }));
        break;
    case 0x0A:
        r = by_drive([&](std::uint8_t) { return fat_seek(s); }, lfs_op([&](std::uint8_t) { return lfs_seek(s); }));
        break;
    case 0x0B:
        r = by_drive([&](std::uint8_t v) { return fat_getcwd(s, v); }, lfs_op([&](std::uint8_t d) { return lfs_getcwd(d); }));
        break;
    case 0x0C:
        r = by_drive([&](std::uint8_t v) { return fat_chdir(s, v); }, lfs_op([&](std::uint8_t d) { return lfs_chdir(s, d); }));
        break;
    case 0x0D:
        r = by_drive([&](std::uint8_t v) { return fat_mkdir(s, v); },
                     flash_refuses([&](std::uint8_t d) { return lfs_mkdir(s, d); }, kLfsReadOnly));
        break;
    case 0x0E:
        r = by_drive([&](std::uint8_t v) { return fat_delete(s, v); },
                     flash_refuses([&](std::uint8_t d) { return lfs_delete(s, d); }, kLfsReadOnly));
        break;
    case 0x0F:
        // Neither littlefs drive renames ([FW] tvc_lfs_psram_rename, tvc_lfs_flash_rename)
        r = by_drive([&](std::uint8_t v) { return fat_rename(s, v); },
                     flash_refuses([&](std::uint8_t) { return kLfsReadOnly; }, kLfsReadOnly));
        break;
    case 0x10:
        r = by_drive([&](std::uint8_t v) { return fat_stat(s, v); }, lfs_op([&](std::uint8_t d) { return lfs_stat(s, d); }));
        break;
    case 0x11:
        r = by_drive([&](std::uint8_t) { return fat_sync(s); }, lfs_op([&](std::uint8_t d) {
            return d == 1 ? lfs_sync(s) : std::uint8_t{0};
        }));
        break;
    case 0x12:
        switch (drive) {
        case 0:
            r = fat_mount_dsk(s);
            break;
        case 1:
        case 2:
            r = ready(drive) ? lfs_mount_dsk(s, drive) : kFrNotReady;
            break;
        default:
            r = 0x80;
            break;
        }
        break;
    case 0x13:
        switch (dsk_type_) {
        case 1:
            r = fat_unmount_dsk(s);
            break;
        case 2:
        case 3:
            r = ready(static_cast<std::uint8_t>(dsk_type_ - 1)) ? lfs_unmount_dsk(s) : kFrNotReady;
            break;
        default:
            r = 0x80;
            break;
        }
        break;
    default:
        return 0xFF;
    }
    return r == 0 ? 0 : static_cast<std::uint8_t>(r | 0x80);
}

// ---- The FatFs layer ([FW] tvc-fatfs.c) ----

std::uint8_t Card256::fat_open(Session& s, std::uint8_t volume) {
    const std::uint8_t mode = rom(0x1900);
    const std::uint8_t type = rom(0x1901);
    const std::uint8_t len = rom(0x1902);
    set_rom(0x1903 + len, 0);
    if (mode & ~0x3F) {
        return kFrInvalidParameter;
    }
    fat_drive_ = volume;
    std::string name;
    std::uint64_t size = 0;
    bool cas = false;
    FatEntry e;
    const auto free_dir = [&] { return std::any_of(fat_dirs_.begin(), fat_dirs_.end(), [](const FatDir& d) { return !d.used; }); };
    if (mode & kFaRead) {
        if (len == 0) {
            // The first .cas file of the working directory, by its short name when it has one
            FatDir d;
            if (!free_dir() || s.f_opendir(d, "./") != kFrOk) {
                return kFrNoFile;
            }
            for (bool end = false;;) {
                if (s.f_readdir(d, e, end) != kFrOk) {
                    break;
                }
                if (end) {
                    return kFrNoFile;
                }
                if (!e.alt.empty() && ends_with_cas(e.alt)) {
                    name = e.alt;
                } else if (ends_with_cas(e.name)) {
                    name = e.name;
                } else {
                    continue;
                }
                size = e.size;
                cas = true;
                break;
            }
        } else {
            name = rom_string(0x1903, 256);
            if (len > 1 && char_at(name, len - 1) == '*') {
                // A prefix: the first file (not directory) whose lower-case name starts with it
                name = lowered(name.substr(0, len - 1u));
                FatDir d;
                if (!free_dir() || s.f_opendir(d, "./") != kFrOk) {
                    return kFrNoFile;
                }
                for (bool end = false;;) {
                    if (s.f_readdir(d, e, end) != kFrOk) {
                        break;
                    }
                    if (end) {
                        return kFrNoFile;
                    }
                    const std::string low = lowered(e.name);
                    if (!(e.attr & kAmDir) && low.starts_with(name)) {
                        name = low.substr(0, 255);
                        break;
                    }
                }
            }
            std::uint8_t r = s.f_stat(name, e);
            if ((r != kFrOk || (e.attr & kAmDir)) && name.size() < 252) {
                name += ".cas";
                r = s.f_stat(name, e);
                if (r != kFrOk) {
                    return r;
                }
                cas = true;
            } else if (ends_with_cas(name)) {
                cas = true;
            }
            if (r != kFrOk) {
                return r;
            }
            size = e.size;
        }
    } else {
        if (len == 0) {
            return kFrInvalidName;
        }
        if (type != 0x01 && type != 0x11) {
            return kFrInvalidParameter;
        }
        if (rom(0x1904) == ':') {
            return kFrInvalidParameter;
        }
        name = rom_string(0x1903, 256);
        if (!ends_with_cas(name) && (len < 4 || char_at(name, len - 4u) != '.') && name.size() < 252) {
            name += ".cas";
        }
        cas = ends_with_cas(name);
    }
    const auto slot = std::find_if(fat_files_.begin(), fat_files_.end(), [](const FatFile& f) { return !f.used; });
    if (slot == fat_files_.end()) {
        return kFrTooManyOpenFiles;
    }
    FatFile& f = *slot;
    f = FatFile{};
    f.used = true;
    std::uint8_t r = s.f_open(f, name, mode);
    if (r != kFrOk) {
        f = FatFile{};
        return r;
    }
    if (cas) {
        if (mode & kFaRead) {
            std::array<std::uint8_t, 1> first{};
            std::size_t got = 0;
            r = s.f_read(f, first, got);
            if (first[0] != 0x01 && first[0] != 0x11) {
                s.f_close(f);
                f = FatFile{};
                return kBadCas;
            }
            s.f_lseek(f, 0x80);
            size -= 0x80;
        } else if (mode & (kFaCreateNew | kFaCreateAlways)) {
            // The CAS header: the type byte and 127 zeros
            std::size_t put = 0;
            const std::array<std::uint8_t, 1> t{type};
            r = s.f_write(f, t, put);
            const std::array<std::uint8_t, 1> zero{0};
            for (int i = 1; r == kFrOk && i < 128; ++i) {
                r = s.f_write(f, zero, put);
            }
        }
    }
    set_rom32(0x1A00, fat_file_handle(static_cast<unsigned>(slot - fat_files_.begin())));
    set_rom32(0x1A04, static_cast<std::uint32_t>(size));
    set_rom(0x1A08, static_cast<std::uint8_t>(name.size()));
    for (unsigned i = 0; i < 255; ++i) {  // strncpy pads with zeros
        set_rom(0x1A09 + i, static_cast<std::uint8_t>(char_at(name, i)));
    }
    set_rom(0x1B08, 0);
    return r;
}

std::uint8_t Card256::fat_close(Session& s) {
    const std::uint32_t handle = rom32(0x1900);
    if (handle == 0) {
        for (FatFile& f : fat_files_) {
            f = FatFile{};
        }
        return kFrOk;
    }
    const int i = slot_of(handle, fat_file_handle);
    if (i < 0) {
        return kBadHandle;
    }
    const std::uint8_t r = s.f_close(fat_files_[static_cast<unsigned>(i)]);
    fat_files_[static_cast<unsigned>(i)] = FatFile{};
    return r;
}

std::uint8_t Card256::fat_read(Session& s) {
    const int i = slot_of(rom32(0x1900), fat_file_handle);
    if (i < 0) {
        return kBadHandle;
    }
    const std::uint16_t n = static_cast<std::uint16_t>(rom(0x1904) | rom(0x1905) << 8);
    if (n == 0 || n > 6 * 256 - 2) {
        return kFrInvalidParameter;
    }
    std::vector<std::uint8_t> buffer(n);
    std::size_t got = 0;
    const std::uint8_t r = s.f_read(fat_files_[static_cast<unsigned>(i)], buffer, got);
    for (std::size_t k = 0; k < got; ++k) {
        set_rom(0x1A02 + static_cast<unsigned>(k), buffer[k]);
    }
    if (r == kFrOk) {
        set_rom(0x1A00, static_cast<std::uint8_t>(got));
        set_rom(0x1A01, static_cast<std::uint8_t>(got >> 8));
    }
    return r;
}

std::uint8_t Card256::fat_read_dest(Session& s) {
    const int i = slot_of(rom32(0x1900), fat_file_handle);
    if (i < 0) {
        return kBadHandle;
    }
    const std::uint32_t n = rom32(0x1904) & 0xFFFFFF;
    std::uint32_t pos = rom32(0x1907) & 0xFFFFFF;
    const bool slow = pos & 0x800000;
    if (slow) {
        pos &= 0x7FFFFF;
        if (kPsramBytes < pos + n) {
            return kFrInvalidParameter;
        }
    } else if (n == 0 || n + pos > kFastLimit) {
        return kFrInvalidParameter;
    }
    std::vector<std::uint8_t> buffer(n);
    std::size_t got = 0;
    const std::uint8_t r = s.f_read(fat_files_[static_cast<unsigned>(i)], buffer, got);
    for (std::size_t k = 0; k < got; ++k) {
        if (slow) {
            psram_write(pos + static_cast<std::uint32_t>(k), buffer[k]);
        } else {
            fast_put(pos + static_cast<std::uint32_t>(k), buffer[k]);
        }
    }
    if (r == kFrOk) {
        set_rom32(0x1A00, static_cast<std::uint32_t>(got));
    }
    return r;
}

std::uint8_t Card256::fat_write(Session& s) {
    const int i = slot_of(rom32(0x1900), fat_file_handle);
    if (i < 0) {
        return kBadHandle;
    }
    const std::uint16_t n = static_cast<std::uint16_t>(rom(0x1904) | rom(0x1905) << 8);
    if (n == 0 || n + 0x1906u > 0x2000) {
        return kFrInvalidParameter;
    }
    std::vector<std::uint8_t> buffer(n);
    for (unsigned k = 0; k < n; ++k) {
        buffer[k] = rom(0x1906 + k);
    }
    std::size_t put = 0;
    const std::uint8_t r = s.f_write(fat_files_[static_cast<unsigned>(i)], buffer, put);
    set_rom(0x1A00, static_cast<std::uint8_t>(put));
    set_rom(0x1A01, static_cast<std::uint8_t>(put >> 8));
    return r;
}

std::uint8_t Card256::fat_write_source(Session& s) {
    const int i = slot_of(rom32(0x1900), fat_file_handle);
    if (i < 0) {
        return kBadHandle;
    }
    const std::uint32_t n = rom32(0x1904) & 0xFFFFFF;
    std::uint32_t pos = rom32(0x1907) & 0xFFFFFF;
    const bool slow = pos & 0x800000;
    if (slow) {
        pos &= 0x7FFFFF;
        if (kPsramBytes < pos + n) {
            return kFrInvalidParameter;
        }
    } else if (n == 0 || n + pos > kFastLimit) {
        return kFrInvalidParameter;
    }
    std::vector<std::uint8_t> buffer(n);
    for (std::uint32_t k = 0; k < n; ++k) {
        buffer[k] = slow ? psram_read(pos + k) : fast_get(pos + k);
    }
    std::size_t put = 0;
    const std::uint8_t r = s.f_write(fat_files_[static_cast<unsigned>(i)], buffer, put);
    set_rom32(0x1A00, static_cast<std::uint32_t>(put));
    return r;
}

std::uint8_t Card256::fat_open_dir(Session& s, std::uint8_t volume) {
    const std::uint8_t len = rom(0x1900);
    if (len == 0) {
        return kFrInvalidParameter;
    }
    set_rom(0x1901 + len, 0);
    const auto slot = std::find_if(fat_dirs_.begin(), fat_dirs_.end(), [](const FatDir& d) { return !d.used; });
    if (slot == fat_dirs_.end()) {
        return kFrTooManyOpenFiles;
    }
    fat_drive_ = volume;
    FatDir d;
    d.used = true;
    const std::uint8_t r = s.f_opendir(d, rom_string(0x1901, 255));
    if (r == kFrOk) {
        *slot = d;
        set_rom32(0x1A00, fat_dir_handle(static_cast<unsigned>(slot - fat_dirs_.begin())));
    }
    return r;
}

std::uint8_t Card256::fat_close_dir(Session&) {
    const int i = slot_of(rom32(0x1900), fat_dir_handle);
    if (i < 0) {
        return kBadHandle;
    }
    FatDir& d = fat_dirs_[static_cast<unsigned>(i)];
    const std::uint8_t r = d.open ? kFrOk : kFrInvalidObject;
    d = FatDir{};
    return r;
}

std::uint8_t Card256::fat_read_dir(Session& s) {
    const int i = slot_of(rom32(0x1900), fat_dir_handle);
    if (i < 0) {
        return kBadHandle;
    }
    for (unsigned k = 0; k < kFilinfoSize; ++k) {
        set_rom(0x1C00 + k, 0);
    }
    FatEntry e;
    bool end = false;
    const std::uint8_t r = s.f_readdir(fat_dirs_[static_cast<unsigned>(i)], e, end);
    if (r != kFrOk) {
        return r;
    }
    if (end) {
        e = FatEntry{};
    } else {
        put_filinfo(e);
    }
    const std::string name = e.name.substr(0, 255);
    set_rom(0x1A00, static_cast<std::uint8_t>(name.size()));
    for (std::size_t k = 0; k < name.size(); ++k) {
        set_rom(0x1A01 + static_cast<unsigned>(k), static_cast<std::uint8_t>(name[k]));
    }
    set_rom(0x1B00, e.attr);
    set_rom32(0x1B01, static_cast<std::uint32_t>(std::min<std::uint64_t>(e.size, 0xFFFFFFFF)));
    if (name.size() > 12) {
        const std::string alt = e.alt.substr(0, 12);
        for (std::size_t k = 0; k < alt.size(); ++k) {
            set_rom(0x1B06 + static_cast<unsigned>(k), static_cast<std::uint8_t>(alt[k]));
        }
        set_rom(0x1B05, static_cast<std::uint8_t>(alt.size()));
    }
    return kFrOk;
}

std::uint8_t Card256::fat_seek(Session& s) {
    const int i = slot_of(rom32(0x1900), fat_file_handle);
    const std::uint32_t pos = rom32(0x1904);
    if (i < 0) {
        return kBadHandle;
    }
    return s.f_lseek(fat_files_[static_cast<unsigned>(i)], pos);
}

std::uint8_t Card256::fat_getcwd(Session& s, std::uint8_t volume) {
    fat_drive_ = volume;
    std::string cwd;
    const std::uint8_t r = s.f_getcwd(cwd);
    if (r != kFrOk) {
        return r;
    }
    if (cwd.size() >= 255) {
        return 17;  // FR_NOT_ENOUGH_CORE: the 255-byte buffer
    }
    // Written as "N:/path", then the drive moved off with memmove: the old tail stays behind it
    put_rom_string(0x1A01, cwd);
    unsigned len = static_cast<unsigned>(cwd.size());
    if (cwd.size() > 1 && cwd[1] == ':') {
        len -= 2;
        for (unsigned k = 0; k < len; ++k) {
            set_rom(0x1A01 + k, static_cast<std::uint8_t>(cwd[k + 2]));
        }
    }
    set_rom(0x1A00, static_cast<std::uint8_t>(len));
    return kFrOk;
}

std::uint8_t Card256::fat_chdir(Session& s, std::uint8_t volume) {
    const std::uint8_t len = rom(0x1900);
    if (len == 0) {
        return kFrInvalidParameter;
    }
    if (rom(0x1901) == ':') {
        return kFrInvalidParameter;
    }
    fat_drive_ = volume;
    set_rom(0x1901 + len, 0);
    if (len > 1 && rom(0x1901 + len - 1u) == '*') {
        // A prefix: the first directory whose lower-case name starts with it; card RAM keeps the edits
        set_rom(0x1901 + len - 1u, 0);
        lower_rom(0x1901);
        const std::string prefix = rom_string(0x1901, 256);
        FatDir d;
        if (std::none_of(fat_dirs_.begin(), fat_dirs_.end(), [](const FatDir& x) { return !x.used; }) ||
            s.f_opendir(d, "./") != kFrOk) {
            return kFrNoPath;
        }
        FatEntry e;
        for (bool end = false;;) {
            if (s.f_readdir(d, e, end) != kFrOk) {
                return kFrNoPath;
            }
            if (end) {
                return kFrNoPath;
            }
            const std::string low = lowered(e.name);
            if ((e.attr & kAmDir) && low.starts_with(prefix)) {
                for (unsigned i = 0; i < 255; ++i) {
                    set_rom(0x1901 + i, static_cast<std::uint8_t>(char_at(low, i)));
                }
                set_rom(0x1901 + 255, 0);
                break;
            }
        }
    }
    return s.f_chdir(rom_string(0x1901, 256));
}

std::uint8_t Card256::fat_mkdir(Session& s, std::uint8_t volume) {
    const std::uint8_t len = rom(0x1900);
    if (len == 0) {
        return kFrInvalidParameter;
    }
    fat_drive_ = volume;
    set_rom(0x1901 + len, 0);
    return s.f_mkdir(rom_string(0x1901, 256));
}

std::uint8_t Card256::fat_delete(Session& s, std::uint8_t volume) {
    const std::uint8_t len = rom(0x1900);
    if (len == 0) {
        return kFrInvalidParameter;
    }
    fat_drive_ = volume;
    set_rom(0x1901 + len, 0);
    return s.f_unlink(rom_string(0x1901, 256));
}

std::uint8_t Card256::fat_rename(Session& s, std::uint8_t volume) {
    const std::uint8_t len1 = rom(0x1900);
    const std::uint8_t len2 = rom(0x1901 + len1);
    if (len1 == 0 || len2 == 0) {
        return kFrInvalidParameter;
    }
    fat_drive_ = volume;
    return s.f_rename(rom_string(0x1901, len1), rom_string(0x1902 + len1, len2));
}

std::uint8_t Card256::fat_stat(Session& s, std::uint8_t volume) {
    const std::uint8_t len = rom(0x1900);
    if (len == 0) {
        return kFrInvalidParameter;
    }
    fat_drive_ = volume;
    set_rom(0x1901 + len, 0);
    FatEntry e;
    const std::uint8_t r = s.f_stat(rom_string(0x1901, 256), e);
    if (r == kFrOk) {
        const std::string name = e.name.substr(0, 255);
        set_rom(0x1A00, static_cast<std::uint8_t>(name.size()));
        for (std::size_t k = 0; k < name.size(); ++k) {
            set_rom(0x1A01 + static_cast<unsigned>(k), static_cast<std::uint8_t>(name[k]));
        }
        set_rom(0x1B00, e.attr);
        set_rom32(0x1B01, static_cast<std::uint32_t>(std::min<std::uint64_t>(e.size, 0xFFFFFFFF)));
        if (name.size() > 12) {
            const std::string alt = e.alt.substr(0, 12);
            set_rom(0x1B05, static_cast<std::uint8_t>(alt.size()));
            for (std::size_t k = 0; k < alt.size(); ++k) {
                set_rom(0x1B06 + static_cast<unsigned>(k), static_cast<std::uint8_t>(alt[k]));
            }
        } else {
            set_rom(0x1B05, 0);
        }
    }
    return r;
}

std::uint8_t Card256::fat_sync(Session& s) {
    const int i = slot_of(rom32(0x1900), fat_file_handle);
    if (i < 0) {
        return kBadHandle;
    }
    return s.volume_of(fat_files_[static_cast<unsigned>(i)]) ? kFrOk : kFrInvalidObject;
}

// A DSK image on the pendrive ([FW] tvc_fatfs_mount_dsk), opened on FatFs's current drive
std::uint8_t Card256::fat_mount_dsk(Session& s) {
    const std::uint8_t len = rom(0x1900);
    set_rom(0x1901 + len, 0);
    if (dsk_type_ != 0) {
        return kFrNotReady;
    }
    FatFile image;
    std::uint8_t r = s.f_open(image, rom_string(0x1901, 256), kFaRead | kFaWrite);
    if (r == kFrOk) {
        dsk_type_ = 1;
        dsk_path_ = image.path;
        fat_cwd_[1] = "/";
        r = s.mount_dsk();
        if (r != kFrOk) {
            dsk_type_ = 0;
            dsk_path_.clear();
            fat_drive_ = 0;
        }
    }
    set_rom(0x1825, r == kFrOk);
    return r;
}

std::uint8_t Card256::fat_unmount_dsk(Session& s) {
    if (dsk_type_ != 1) {
        return kFrInvalidObject;
    }
    fat_drive_ = 0;
    s.drop_dsk();
    dsk_unmounted();
    if (rom(0x1810) == 3) {
        set_rom(0x1810, 0);
    }
    return kFrOk;
}

// The DSK's FatFs handles lose their volume ([FW] f_unmount)
void Card256::dsk_unmounted() {
    for (FatFile& f : fat_files_) {
        if (f.volume == 1) {
            f.open = false;
        }
    }
    for (FatDir& d : fat_dirs_) {
        if (d.volume == 1) {
            d.open = false;
        }
    }
    dsk_type_ = 0;
    dsk_path_.clear();
    set_rom(0x1825, 0);
}

// ---- The littlefs layer ([FW] tvc-lfs.c) ----

namespace {

// [FW] reopens nothing: its files stay open; ours are opened for each command at the handle's place
template <typename F>
int with_file(lfs_t* fs, std::string_view path, int flags, std::uint32_t& pos, F use) {
    lfs_file_t file{};
    const std::string p(path);
    int r = lfs_file_open(fs, &file, p.c_str(), flags);
    if (r < 0) {
        return r;
    }
    lfs_file_seek(fs, &file, static_cast<lfs_soff_t>(pos), LFS_SEEK_SET);
    r = use(file);
    pos = static_cast<std::uint32_t>(std::max<lfs_soff_t>(0, lfs_file_tell(fs, &file)));
    lfs_file_close(fs, &file);
    return r;
}

}  // namespace

std::uint8_t Card256::lfs_open(Session& s, std::uint8_t drive) {
    const std::uint8_t mode = rom(0x1900);
    const std::uint8_t type = rom(0x1901);
    std::uint8_t len = rom(0x1902);
    if (!(mode & kFaRead)) {
        return kLfsReadOnly;  // both drives open read-only ([FW] tvc_lfs_*_file_open pass readOnly = true)
    }
    set_rom(0x1903 + len, 0);
    lower_rom(0x1903);
    const std::string wd = lfs_cwd(drive);
    int flags = 0;
    if (mode & kFaRead) {
        flags |= LFS_O_RDONLY;
    }
    if (mode & kFaWrite) {
        flags |= LFS_O_WRONLY;
    }
    if (mode & kFaCreateAlways) {
        flags |= LFS_O_CREAT | LFS_O_TRUNC;
    } else if (mode & kFaCreateNew) {
        flags |= LFS_O_CREAT | LFS_O_EXCL;
    } else if (mode & kFaOpenAlways) {
        flags |= LFS_O_CREAT;
    }
    int err = 0;
    lfs_t* fs = s.lfs(drive, err);
    if (!fs) {
        return static_cast<std::uint8_t>(err);
    }
    bool cas = false;
    std::uint32_t size = 0;
    std::string name = rom_string(0x1903, 256);
    lfs_info info{};
    if (len == 0) {
        bool found = false;
        lfs_dir_t dir{};
        if (lfs_dir_open(fs, &dir, wd.c_str()) == 0) {
            while (lfs_dir_read(fs, &dir, &info) > 0) {
                if (info.type == LFS_TYPE_REG && ends_with_cas(info.name)) {
                    if (std::strlen(info.name) + wd.size() >= 256) {
                        lfs_dir_close(fs, &dir);
                        return kFrInvalidName;
                    }
                    temp_path_ = wd + info.name;
                    name = info.name;
                    put_rom_string(0x1903, name);
                    cas = true;
                    size = info.size;
                    found = true;
                    break;
                }
            }
            lfs_dir_close(fs, &dir);
        }
        if (!found) {
            return kFrNoFile;
        }
    } else {
        if (char_at(name, 0) == '/') {
            int i = std::min<int>(len, static_cast<int>(name.size())) - 1;
            while (i >= 0 && name[static_cast<std::size_t>(i)] != '/') {
                --i;
            }
            temp_path_ = name.substr(0, static_cast<std::size_t>(i + 1));
            name = name.substr(static_cast<std::size_t>(i + 1));
            put_rom_string(0x1903, name);
        } else {
            temp_path_ = wd;
        }
        if (name.size() + wd.size() >= 252) {
            return kFrInvalidName;
        }
        len = static_cast<std::uint8_t>(name.size());
        if (len > 1 && name[len - 1u] == '*') {
            lfs_dir_t dir{};
            int res = lfs_dir_open(fs, &dir, temp_path_.c_str());
            name.pop_back();
            set_rom(0x1903 + len - 1u, 0);
            if (res == 0) {
                while ((res = lfs_dir_read(fs, &dir, &info)) > 0) {
                    if (info.type == LFS_TYPE_REG && std::string_view(info.name).starts_with(name)) {
                        name = std::string(info.name).substr(0, 255);
                        for (unsigned i = 0; i < 255; ++i) {
                            set_rom(0x1903 + i, static_cast<std::uint8_t>(char_at(name, i)));
                        }
                        set_rom(0x1903 + 255, 0);
                        break;
                    }
                }
                lfs_dir_close(fs, &dir);
            }
            if (res == 0) {
                return kFrNoFile;
            }
        }
        temp_path_ += name;
        if (ends_with_cas(name)) {
            const auto r = static_cast<std::uint8_t>(::lfs_stat(fs, temp_path_.c_str(), &info));
            if (r != 0) {
                return r;
            }
            size = info.size;
            cas = true;
        } else if (temp_path_.size() < 252) {
            temp_path_ += ".cas";
            if (::lfs_stat(fs, temp_path_.c_str(), &info) < 0) {
                temp_path_.resize(temp_path_.size() - 4);
                if (::lfs_stat(fs, temp_path_.c_str(), &info) >= 0) {
                    size = info.size;
                }
                cas = false;
            } else {
                size = info.size;
                cas = true;
                name += ".cas";
                put_rom_string(0x1903, name);
            }
        }
    }
    const auto slot = std::find_if(lfs_files_.begin(), lfs_files_.end(), [](const LfsFile& f) { return !f.used; });
    if (slot == lfs_files_.end()) {
        return kFrTooManyOpenFiles;
    }
    LfsFile& h = *slot;
    h = LfsFile{};
    h.used = true;
    lfs_file_t file{};
    int res = lfs_file_open(fs, &file, temp_path_.c_str(), flags);
    if (res == 0) {
        h.open = true;
        h.drive = drive;
        h.path = temp_path_;
        h.flags = flags & ~kLfsReopen;
        if (cas) {
            std::uint8_t first = type;  // an empty file leaves the variable as it was
            lfs_file_read(fs, &file, &first, 1);
            if (first != 0x01 && first != 0x11) {
                lfs_file_close(fs, &file);
                h.open = false;  // [FW] keeps the slot taken: a handle leaks
                return kBadCas;
            }
            lfs_file_seek(fs, &file, 0x80, LFS_SEEK_SET);
            h.pos = 0x80;
            size -= 0x80;
        }
        lfs_file_close(fs, &file);
    } else {
        h = LfsFile{};
        if (res == LFS_ERR_EXIST) {
            res = kFrExist;
        }
    }
    const bool ok = res == 0;
    set_rom32(0x1A00, ok ? lfs_file_handle(static_cast<unsigned>(slot - lfs_files_.begin())) : 0);
    set_rom32(0x1A04, ok ? size : 0);
    set_rom(0x1A08, ok ? static_cast<std::uint8_t>(name.size()) : 0);
    if (ok) {
        put_rom_string(0x1A09, name);
    }
    return static_cast<std::uint8_t>(res);
}

std::uint8_t Card256::lfs_close(Session&) {
    const int i = slot_of(rom32(0x1900), lfs_file_handle);
    if (i < 0 || !lfs_files_[static_cast<unsigned>(i)].used) {
        return kFrInvalidObject;
    }
    lfs_files_[static_cast<unsigned>(i)] = LfsFile{};
    return 0;
}

std::uint8_t Card256::lfs_read(Session& s) {
    const int i = slot_of(rom32(0x1900), lfs_file_handle);
    if (i < 0 || !lfs_files_[static_cast<unsigned>(i)].used) {
        return kFrInvalidObject;
    }
    LfsFile& h = lfs_files_[static_cast<unsigned>(i)];
    const std::uint16_t n = static_cast<std::uint16_t>(rom(0x1904) | rom(0x1905) << 8);
    if (n == 0 || n > 6 * 256 - 2) {
        return kFrInvalidParameter;
    }
    int err = 0;
    lfs_t* fs = s.lfs(h.drive, err);
    std::vector<std::uint8_t> buffer(n);
    const int got = !fs ? err : !h.open ? LFS_ERR_BADF : with_file(fs, h.path, h.flags, h.pos, [&](lfs_file_t& f) {
        return static_cast<int>(lfs_file_read(fs, &f, buffer.data(), n));
    });
    const std::uint16_t count = got > 0 ? static_cast<std::uint16_t>(got) : 0;
    for (unsigned k = 0; k < count; ++k) {
        set_rom(0x1A02 + k, buffer[k]);
    }
    set_rom(0x1A00, static_cast<std::uint8_t>(count));
    set_rom(0x1A01, static_cast<std::uint8_t>(count >> 8));
    return got > 0 ? 0 : got == 0 ? kLfsEndOfFile : static_cast<std::uint8_t>(got);
}

std::uint8_t Card256::lfs_write(Session& s) {
    const int i = slot_of(rom32(0x1900), lfs_file_handle);
    if (i < 0 || !lfs_files_[static_cast<unsigned>(i)].used) {
        return kFrInvalidObject;
    }
    LfsFile& h = lfs_files_[static_cast<unsigned>(i)];
    const std::uint16_t n = static_cast<std::uint16_t>(rom(0x1904) | rom(0x1905) << 8);
    if (n == 0 || n > 6 * 256 - 6) {
        return kFrInvalidParameter;
    }
    std::vector<std::uint8_t> buffer(n);
    for (unsigned k = 0; k < n; ++k) {
        buffer[k] = rom(0x1906 + k);
    }
    int err = 0;
    lfs_t* fs = s.lfs(h.drive, err);
    const int put = !fs ? err : !h.open ? LFS_ERR_BADF : with_file(fs, h.path, h.flags, h.pos, [&](lfs_file_t& f) {
        return static_cast<int>(lfs_file_write(fs, &f, buffer.data(), n));
    });
    if (put >= 0) {
        set_rom(0x1A00, static_cast<std::uint8_t>(put));
        set_rom(0x1A01, static_cast<std::uint8_t>(put >> 8));
        return 0;
    }
    return static_cast<std::uint8_t>(put);
}

std::uint8_t Card256::lfs_open_dir(Session& s, std::uint8_t drive) {
    const std::uint8_t len = rom(0x1900);
    if (len == 0) {
        return kFrInvalidParameter;
    }
    set_rom(0x1901 + len, 0);
    const auto slot = std::find_if(lfs_dirs_.begin(), lfs_dirs_.end(), [](const LfsDir& d) { return !d.used; });
    if (slot == lfs_dirs_.end()) {
        return kFrTooManyOpenFiles;
    }
    std::string dir = rom_string(0x1901, 256);
    if (rom(0x1901 + len - 1u) != '/') {
        dir += "/";
        put_rom_string(0x1901, dir);
    }
    if (dir == "./") {
        dir = lfs_cwd(drive);
        put_rom_string(0x1901, dir);
    }
    int err = 0;
    lfs_t* fs = s.lfs(drive, err);
    int res = err;
    if (fs) {
        lfs_dir_t d{};
        res = lfs_dir_open(fs, &d, dir.c_str());
        if (res == 0) {
            lfs_dir_close(fs, &d);
        }
    }
    if (res == 0) {
        *slot = LfsDir{true, drive, dir, 0};
        set_rom32(0x1A00, lfs_dir_handle(static_cast<unsigned>(slot - lfs_dirs_.begin())));
    }
    return static_cast<std::uint8_t>(res);
}

std::uint8_t Card256::lfs_close_dir(Session&) {
    const int i = slot_of(rom32(0x1900), lfs_dir_handle);
    if (i < 0 || !lfs_dirs_[static_cast<unsigned>(i)].used) {
        return kBadHandle;
    }
    lfs_dirs_[static_cast<unsigned>(i)] = LfsDir{};
    return 0;
}

std::uint8_t Card256::lfs_read_dir(Session& s) {
    const int i = slot_of(rom32(0x1900), lfs_dir_handle);
    if (i < 0 || !lfs_dirs_[static_cast<unsigned>(i)].used) {
        return kBadHandle;
    }
    LfsDir& d = lfs_dirs_[static_cast<unsigned>(i)];
    int err = 0;
    lfs_t* fs = s.lfs(d.drive, err);
    if (!fs) {
        return static_cast<std::uint8_t>(err);
    }
    lfs_dir_t dir{};
    lfs_info info{};
    int res = lfs_dir_open(fs, &dir, d.path.c_str());
    if (res == 0) {
        for (std::uint32_t k = 0; k < d.index && res >= 0; ++k) {
            res = lfs_dir_read(fs, &dir, &info);
        }
        if (res >= 0) {
            do {
                info = lfs_info{};
                res = lfs_dir_read(fs, &dir, &info);
                d.index += res > 0;
            } while (res > 0 && (std::strcmp(info.name, ".") == 0 || std::strcmp(info.name, "..") == 0));
        }
        lfs_dir_close(fs, &dir);
    }
    // The lfs_info lands at DC00h: type, size at +4, name at +8
    for (unsigned k = 0; k < 8 + LFS_NAME_MAX + 1; ++k) {
        set_rom(0x1C00 + k, 0);
    }
    if (res > 0) {
        set_rom(0x1C00, info.type);
        set_rom32(0x1C04, info.size);
        put_rom_string(0x1C08, info.name);
        const std::string name = info.name;
        const std::size_t len = name.size();
        set_rom(0x1A00, static_cast<std::uint8_t>(len));
        put_rom_string(0x1A01, name);
        set_rom(0x1AFF, 0);
        set_rom(0x1B00, info.type == LFS_TYPE_DIR ? 0x10 : 0x01);
        set_rom32(0x1B01, info.size);
        if (len > 12) {
            int pos = static_cast<int>(len) - 1;
            while (pos >= 0 && name[static_cast<std::size_t>(pos)] != '.') {
                --pos;
            }
            if (pos < static_cast<int>(len) - 4) {
                for (unsigned k = 0; k < 12; ++k) {
                    set_rom(0x1B06 + k, static_cast<std::uint8_t>(name[k]));
                }
            } else {
                for (unsigned k = 0; k < 7; ++k) {
                    set_rom(0x1B06 + k, static_cast<std::uint8_t>(name[k]));
                }
                set_rom(0x1B06 + 7, '~');
                set_rom(0x1B06 + 8, '.');
                const std::size_t ext = std::min<std::size_t>(3, len - static_cast<std::size_t>(pos) - 1);
                for (std::size_t k = 0; k < ext; ++k) {
                    set_rom(0x1B06 + 9 + static_cast<unsigned>(k), static_cast<std::uint8_t>(name[static_cast<std::size_t>(pos) + 1 + k]));
                }
            }
            set_rom(0x1B05, 12);
        } else {
            set_rom(0x1B05, 0);
        }
        return 0;
    }
    if (res == 0) {
        set_rom(0x1A00, 0);
    }
    return static_cast<std::uint8_t>(res);
}

std::uint8_t Card256::lfs_seek(Session&) {
    const int i = slot_of(rom32(0x1900), lfs_file_handle);
    const std::uint32_t pos = rom32(0x1904);
    if (i < 0 || !lfs_files_[static_cast<unsigned>(i)].used) {
        return kBadHandle;
    }
    if (pos > 0x7FFFFFFF) {
        return static_cast<std::uint8_t>(LFS_ERR_INVAL);  // past littlefs's file_max
    }
    lfs_files_[static_cast<unsigned>(i)].pos = pos;
    return 0;
}

std::uint8_t Card256::lfs_read_dest(Session& s) {
    const int i = slot_of(rom32(0x1900), lfs_file_handle);
    if (i < 0 || !lfs_files_[static_cast<unsigned>(i)].used) {
        return kBadHandle;
    }
    LfsFile& h = lfs_files_[static_cast<unsigned>(i)];
    const std::uint32_t n = rom32(0x1904) & 0xFFFFFF;
    std::uint32_t pos = rom32(0x1907) & 0xFFFFFF;
    const bool slow = pos & 0x800000;
    if (slow) {
        pos &= 0x7FFFFF;
        if (kPsramBytes < pos + n) {
            return kFrInvalidParameter;
        }
    } else if (n == 0 || n + pos > kFastLimit) {
        return kFrInvalidParameter;
    }
    int err = 0;
    lfs_t* fs = s.lfs(h.drive, err);
    std::vector<std::uint8_t> buffer(n);
    const int got = !fs ? err : !h.open ? LFS_ERR_BADF : with_file(fs, h.path, h.flags, h.pos, [&](lfs_file_t& f) {
        return static_cast<int>(lfs_file_read(fs, &f, buffer.data(), n));
    });
    for (int k = 0; k < got; ++k) {
        if (slow) {
            psram_write(pos + static_cast<std::uint32_t>(k), buffer[static_cast<std::size_t>(k)]);
        } else {
            fast_put(pos + static_cast<std::uint32_t>(k), buffer[static_cast<std::size_t>(k)]);
        }
    }
    if (got >= 0 && static_cast<std::uint32_t>(got) == n) {
        set_rom32(0x1A00, n);
        return 0;
    }
    set_rom32(0x1A00, 0);
    return got == 0 ? kLfsEndOfFile : static_cast<std::uint8_t>(got);  // a short read reports its length ([FW] quirk)
}

std::uint8_t Card256::lfs_write_source(Session& s) {
    const int i = slot_of(rom32(0x1900), lfs_file_handle);
    if (i < 0 || !lfs_files_[static_cast<unsigned>(i)].used) {
        return kBadHandle;
    }
    LfsFile& h = lfs_files_[static_cast<unsigned>(i)];
    const std::uint32_t n = rom32(0x1904) & 0xFFFFFF;
    std::uint32_t pos = rom32(0x1907) & 0xFFFFFF;
    const bool slow = pos & 0x800000;
    if (slow) {
        pos &= 0x7FFFFF;
        if (kPsramBytes < pos + n) {
            return kFrInvalidParameter;
        }
    } else if (n == 0 || n + pos > kFastLimit) {
        return kFrInvalidParameter;
    }
    std::vector<std::uint8_t> buffer(n);
    for (std::uint32_t k = 0; k < n; ++k) {
        buffer[k] = slow ? psram_read(pos + k) : fast_get(pos + k);
    }
    int err = 0;
    lfs_t* fs = s.lfs(h.drive, err);
    const int put = !fs ? err : !h.open ? LFS_ERR_BADF : with_file(fs, h.path, h.flags, h.pos, [&](lfs_file_t& f) {
        return static_cast<int>(lfs_file_write(fs, &f, buffer.data(), n));
    });
    if (put >= 0 && static_cast<std::uint32_t>(put) == n) {
        set_rom32(0x1A00, n);
        return 0;
    }
    set_rom32(0x1A00, 0);
    return static_cast<std::uint8_t>(put);
}

std::uint8_t Card256::lfs_getcwd(std::uint8_t drive) {
    const std::string& wd = lfs_cwd(drive);
    set_rom(0x1A00, static_cast<std::uint8_t>(wd.size()));
    for (std::size_t k = 0; k < wd.size(); ++k) {
        set_rom(0x1A01 + static_cast<unsigned>(k), static_cast<std::uint8_t>(wd[k]));
    }
    return 0;
}

std::uint8_t Card256::lfs_chdir(Session& s, std::uint8_t drive) {
    std::uint8_t len = rom(0x1900);
    if (len == 0) {
        return kFrInvalidParameter;
    }
    set_rom(0x1901 + len, 0);
    lower_rom(0x1901);
    std::string& wd = lfs_cwd(drive);
    int err = 0;
    lfs_t* fs = s.lfs(drive, err);
    if (!fs) {
        return static_cast<std::uint8_t>(err);
    }
    std::string path = rom_string(0x1901, 256);
    if (len > 1 && char_at(path, len - 1u) == '*') {
        // A prefix: the first directory starting with it; card RAM keeps the full name
        lfs_dir_t dir{};
        const bool opened = lfs_dir_open(fs, &dir, wd.c_str()) == 0;
        set_rom(0x1901 + len - 1u, 0);
        path = rom_string(0x1901, 256);
        int res = 0;
        lfs_info info{};
        while (opened && (res = lfs_dir_read(fs, &dir, &info)) > 0) {
            if (info.type == LFS_TYPE_DIR && std::string_view(info.name).starts_with(path)) {
                const std::string found = std::string(info.name).substr(0, 255);
                for (unsigned i = 0; i < 255; ++i) {
                    set_rom(0x1901 + i, static_cast<std::uint8_t>(char_at(found, i)));
                }
                set_rom(0x1901 + 255, 0);
                path = found;
                break;
            }
        }
        if (opened) {
            lfs_dir_close(fs, &dir);
        }
        if (res == 0) {
            return kFrNoPath;
        }
    }
    temp_path_ = wd;
    if (char_at(path, 0) == '/') {
        temp_path_ = path.substr(0, 255);
    } else if (char_at(path, 0) == '.' && char_at(path, 1) == '/') {
        if (wd.size() + path.size() - 2 >= 256) {
            return kFrInvalidName;
        }
        temp_path_ += path.substr(2);
    } else {
        // The byte at the original length: a trailing '/' is refused ([FW] quirk)
        if (rom(0x1901 + len - 1u) != '/' && len < 255) {
            path += "/";
            put_rom_string(0x1901, path);
            ++len;
        } else {
            return kFrInvalidName;
        }
        if (wd.size() + path.size() >= 256) {
            return kFrInvalidName;
        }
        temp_path_ += path;
    }
    for (std::size_t p; (p = temp_path_.find("//")) != std::string::npos;) {
        temp_path_.erase(p, 1);
    }
    for (std::size_t p; (p = temp_path_.find("..")) != std::string::npos;) {
        if (p == 1 || p < 2) {
            temp_path_.erase(p, 2);
        } else {
            std::size_t prev = p - 2;
            while (prev != 0 && temp_path_[prev] != '/') {
                --prev;
            }
            temp_path_.erase(prev, p + 2 - prev);
        }
    }
    lfs_info info{};
    if (::lfs_stat(fs, temp_path_.c_str(), &info) < 0 || info.type != LFS_TYPE_DIR) {
        return kFrNoPath;
    }
    if (temp_path_.size() < 254 && (temp_path_.empty() || temp_path_.back() != '/')) {
        temp_path_ += '/';
    }
    wd = temp_path_.substr(0, 255);
    return 0;
}

std::uint8_t Card256::lfs_mkdir(Session& s, std::uint8_t drive) {
    const std::uint8_t len = rom(0x1900);
    if (len == 0) {
        return kFrInvalidParameter;
    }
    set_rom(0x1901 + len, 0);
    if (temp_path_.size() + rom_string(0x1901, 256).size() >= 256) {
        return kFrInvalidName;  // measured against the last path ([FW] stale temp_path)
    }
    lower_rom(0x1901);
    temp_path_ = lfs_cwd(drive) + rom_string(0x1901, 256);
    int err = 0;
    lfs_t* fs = s.lfs(drive, err);
    return static_cast<std::uint8_t>(fs ? ::lfs_mkdir(fs, temp_path_.c_str()) : err);
}

std::uint8_t Card256::lfs_delete(Session& s, std::uint8_t drive) {
    const std::uint8_t len = rom(0x1900);
    if (len == 0) {
        return kFrInvalidParameter;
    }
    set_rom(0x1901 + len, 0);
    if (temp_path_.size() + rom_string(0x1901, 256).size() >= 256) {
        return kFrInvalidName;
    }
    lower_rom(0x1901);
    temp_path_ = lfs_cwd(drive) + rom_string(0x1901, 256);
    int err = 0;
    lfs_t* fs = s.lfs(drive, err);
    return static_cast<std::uint8_t>(fs ? lfs_remove(fs, temp_path_.c_str()) : err);
}

std::uint8_t Card256::lfs_stat(Session& s, std::uint8_t drive) {
    const std::uint8_t len = rom(0x1900);
    if (len == 0) {
        return kFrInvalidParameter;
    }
    set_rom(0x1901 + len, 0);
    if (temp_path_.size() + rom_string(0x1901, 256).size() >= 256) {
        return kFrInvalidName;
    }
    lower_rom(0x1901);
    temp_path_ = lfs_cwd(drive) + rom_string(0x1901, 256);
    int err = 0;
    lfs_t* fs = s.lfs(drive, err);
    if (!fs) {
        return static_cast<std::uint8_t>(err);
    }
    lfs_info info{};
    const int res = ::lfs_stat(fs, temp_path_.c_str(), &info);
    if (res == 0) {
        const std::string name = info.name;
        const std::size_t l = name.size();
        set_rom(0x1A00, static_cast<std::uint8_t>(l));
        for (std::size_t k = 0; k < l; ++k) {
            set_rom(0x1A01 + static_cast<unsigned>(k), static_cast<std::uint8_t>(name[k]));
        }
        set_rom(0x1B00, info.type == LFS_TYPE_DIR ? 0x10 : 0x01);
        set_rom32(0x1B01, info.type == LFS_TYPE_REG ? info.size : 0);
        if (l > 12) {
            set_rom(0x1B05, 12);
            int pos = static_cast<int>(l) - 1;
            while (pos >= 0 && name[static_cast<std::size_t>(pos)] != '.') {
                --pos;
            }
            if (pos < static_cast<int>(l) - 4) {
                for (unsigned k = 0; k < 11; ++k) {
                    set_rom(0x1B06 + k, static_cast<std::uint8_t>(name[k]));
                }
                set_rom(0x1B06 + 11, '~');
            } else {
                for (unsigned k = 0; k < 7; ++k) {
                    set_rom(0x1B06 + k, static_cast<std::uint8_t>(name[k]));
                }
                set_rom(0x1B06 + 7, '~');
                set_rom(0x1B06 + 8, '.');
                const std::size_t ext = std::min<std::size_t>(3, l - static_cast<std::size_t>(pos) - 1);
                for (std::size_t k = 0; k < ext; ++k) {
                    set_rom(0x1B06 + 9 + static_cast<unsigned>(k), static_cast<std::uint8_t>(name[static_cast<std::size_t>(pos) + 1 + k]));
                }
            }
        } else {
            set_rom(0x1B05, 0);
        }
    }
    return static_cast<std::uint8_t>(res);
}

std::uint8_t Card256::lfs_sync(Session&) {
    const int i = slot_of(rom32(0x1900), lfs_file_handle);
    if (i < 0 || !lfs_files_[static_cast<unsigned>(i)].used) {
        return kBadHandle;
    }
    return 0;
}

// A DSK image on the PSRAM or flash drive, named from the drive's root ([FW] tvc_lfs_mount_dsk)
std::uint8_t Card256::lfs_mount_dsk(Session& s, std::uint8_t drive) {
    const std::uint8_t len = rom(0x1900);
    set_rom(0x1901 + len, 0);
    if (dsk_type_ != 0) {
        return kFrNotReady;
    }
    const std::string name = rom_string(0x1901, 256);
    int err = 0;
    lfs_t* fs = s.lfs(drive, err);
    lfs_file_t file{};
    if (!fs || lfs_file_open(fs, &file, name.c_str(), LFS_O_RDWR) != 0) {
        return kFrNoFile;
    }
    lfs_file_close(fs, &file);
    dsk_type_ = drive == 1 ? 2 : 3;
    dsk_path_ = name;
    fat_cwd_[1] = "/";
    const std::uint8_t r = s.mount_dsk();
    if (r != kFrOk) {
        dsk_type_ = 0;
        dsk_path_.clear();
        fat_drive_ = 0;
    }
    set_rom(0x1825, r == kFrOk);
    return r;
}

std::uint8_t Card256::lfs_unmount_dsk(Session& s) {
    if (dsk_type_ != 2 && dsk_type_ != 3) {
        return kFrInvalidObject;
    }
    const std::uint8_t type = dsk_type_;
    fat_drive_ = 0;
    s.drop_dsk();
    dsk_unmounted();
    if (rom(0x1810) == 3) {
        set_rom(0x1810, static_cast<std::uint8_t>(type - 1));  // the drive the image was on
    }
    return kFrOk;
}

// ---- The PSRAM drive's functions ([FW] tvc-lfs-psram.c psram_drive_init, tvc-routines.c 15, 34-36) ----

int Card256::psram_drive_init(Session& s, std::uint32_t blocks) {
    free_psram_start_ = 0;
    psram_drive_ = false;
    if (static_cast<std::uint64_t>(blocks) * kBlock > kPsramBytes) {
        return -1;  // D824h is left as it was
    }
    psram_fill(0, blocks * kBlock, 0xFF);
    psram_blocks_ = blocks;
    const int r = s.format_psram(blocks);
    if (r >= 0) {
        free_psram_start_ = 0xFFFFFF;  // [FW] psram_size() * 1024 * 1024 wraps to 0 in 32 bits, so this branch always
    }
    psram_drive_ = r >= 0;
    set_rom(0x1824, r >= 0);
    return r;
}

// 15: copies the pendrive folder's files into a new PSRAM drive, flat and in lower case, optionally showing progress
std::uint8_t Card256::copy_dir_to_psram(Session& s, std::uint32_t at) {
    if (usb_ != Usb::Pendrive || !s.media() || !s.media()->pendrive) {
        return 129;
    }
    if (rom(0x1810) != 0) {
        return 130;
    }
    const std::uint8_t folder_len = fast_get(at);
    const bool show = fast_get(at + folder_len + 1u) != 0;
    std::string folder = "./";
    if (folder_len != 0) {
        folder.clear();
        for (unsigned k = 0; k < folder_len; ++k) {
            folder += static_cast<char>(fast_get(at + 1 + k));
        }
        folder = std::string(folder.c_str());
    }
    fast_put(at, 0);
    const std::uint8_t mode_before = video_mode_;
    if (show) {
        text_colour_ = 15;
        call_graphics(0x00, at);
        video_mode_ = 1;
        print_ascii_text("Caching dir: ");
    }
    FatDir dir;
    std::uint8_t r = s.f_opendir(dir, folder);
    if (show) {
        print_ascii_text(folder);
        print_ascii_text("\r\n");
    }
    const auto show_hex = [&](std::uint8_t b) {
        print_hex_digit(b >> 4);
        print_hex_digit(b & 0x0F);
    };
    std::uint64_t sum = 0;
    std::uint8_t files = 0;
    FatEntry e;
    while (r == kFrOk) {
        bool end = false;
        r = s.f_readdir(dir, e, end);
        if (r != kFrOk) {
            if (show) {
                print_ascii_text("\nread direntry failed\r\n");
            }
            return r;
        }
        if (end) {
            if (show) {
                print_ascii_text("\rScanning directory done\r\n");
            }
            break;
        }
        if (e.attr & kAmDir) {
            continue;
        }
        ++files;
        sum += e.size;
        if (show) {
            text_x_ = 0;
            show_hex(files);
            print_code(' ');
            for (int shift = 24; shift >= 0; shift -= 8) {
                show_hex(static_cast<std::uint8_t>(sum >> shift));
            }
        }
    }
    const std::uint64_t mapped = std::min<std::uint64_t>(std::max<std::uint64_t>(512 * 1024, 2 * sum), kPsramBytes);
    if (mapped < sum) {
        if (show) {
            print_ascii_text("Not enough PSRAM space..\r\n");
        }
        return 131;
    }
    if (show) {
        print_ascii_text("Formatting PSRAM... ");
    }
    const auto result = static_cast<std::uint8_t>(psram_drive_init(s, static_cast<std::uint32_t>((mapped - 1) / kBlock + 1)));
    if (result != 0) {
        if (show) {
            print_ascii_text("FAILEDs\r\n");
            show_hex(result);
        }
        return 132;
    }
    if (show) {
        print_ascii_text("OK\r\n");
    }
    dir = FatDir{};
    r = s.f_opendir(dir, folder);
    if (r != kFrOk) {
        if (show) {
            print_ascii_text("2nd opendir failed");
        }
        return r;
    }
    if (show) {
        print_ascii_text("Copying files...\r\n");
    }
    int err = 0;
    lfs_t* fs = s.lfs(1, err);
    int res = kFrOk;
    std::size_t previous = 0;
    while (res == kFrOk) {
        bool end = false;
        res = s.f_readdir(dir, e, end);
        if (res != kFrOk) {
            if (show) {
                print_ascii_text("\r\nread direntry failed\r\n");
            }
            return static_cast<std::uint8_t>(res);
        }
        if (end) {
            if (show) {
                print_ascii_text("\rCaching directory done. \r\n");
                video_mode_ = mode_before;
            }
            set_rom(0x1810, 1);
            set_rom(0x1824, 1);
            break;
        }
        if (e.attr & kAmDir) {
            continue;
        }
        if (show) {
            text_x_ = 0;
            print_ascii_text(e.name);
            const std::uint8_t x = text_x_;
            for (std::size_t k = e.name.size(); k < previous; ++k) {
                print_code(' ');
            }
            if (e.name.size() < previous) {
                text_x_ = x;
            }
        }
        previous = e.name.size();
        std::string source = folder;
        if (source.empty() || source.back() != '/') {
            source += "/";
        }
        source += e.name;
        FatFile in;
        res = s.f_open(in, source, kFaRead);
        if (res != kFrOk) {
            if (show) {
                print_ascii_text(" failed to open");
            }
            continue;  // the loop then ends: res is no longer FR_OK
        }
        lfs_file_t out{};
        const std::string target = lowered(e.name);
        res = fs ? lfs_file_open(fs, &out, target.c_str(), LFS_O_CREAT | LFS_O_WRONLY) : err;
        if (res < 0) {
            if (show) {
                print_ascii_text(" failed to create");
            }
            continue;
        }
        std::uint64_t remaining = e.size;
        std::vector<std::uint8_t> buffer(4096);
        while (remaining > 0) {
            const std::size_t want = static_cast<std::size_t>(std::min<std::uint64_t>(buffer.size(), remaining));
            std::size_t got = 0;
            res = s.f_read(in, std::span(buffer).first(want), got);
            if (res != kFrOk) {
                if (show) {
                    print_ascii_text(" failed to read");
                }
                break;
            }
            res = lfs_file_write(fs, &out, buffer.data(), static_cast<lfs_size_t>(got));
            if (res < 0) {
                if (show) {
                    print_ascii_text(" failed to write");
                }
                break;
            }
            res = kFrOk;
            remaining -= got;
            if (got == 0) {
                break;  // the file shrank on the host while copying
            }
        }
        lfs_file_close(fs, &out);
        fast_put(at, static_cast<std::uint8_t>(fast_get(at) + 1));
    }
    return static_cast<std::uint8_t>(res);
}

// 34: the block count is read from the parameter block's MCU address, not from the block ([FW] quirk)
std::uint8_t Card256::create_psram_drive(Session& s, std::uint32_t at) {
    const auto blocks = static_cast<std::uint16_t>(kTvcRamAddress + at);
    const int r = psram_drive_init(s, blocks);
    return r >= 0 ? 0 : static_cast<std::uint8_t>(r);
}

// 36: the firmware's length wraps to nearly 4 GiB; the fill stops at the PSRAM's end here ⚑
std::uint8_t Card256::delete_psram_drive() {
    if (psram_drive_) {
        psram_fill(0, kPsramBytes, 0xFF);
        psram_drive_ = false;
        free_psram_start_ = 0;
        set_rom(0x1824, 0);
    }
    return 0;
}

// ---- The C0h file functions ([FW] tvc-routines.c tvcfunc_*): the parameter block in, the D6h layer, results out ----

std::optional<std::uint8_t> Card256::call_file_function(std::uint8_t code, std::uint32_t at, const CardBus& bus) {
    Session s(*this, bus);
    const std::uint8_t drive = rom(0x1810);
    const auto copy_in = [&](unsigned to, std::uint32_t from, unsigned n) {
        for (unsigned k = 0; k < n; ++k) {
            set_rom(to + k, fast_get(from + k));
        }
    };
    const auto copy_out = [&](std::uint32_t to, unsigned from, unsigned n) {
        for (unsigned k = 0; k < n; ++k) {
            fast_put(to + k, rom(from + k));
        }
    };
    const auto handle_in = [&] { set_rom32(0x1900, param32(at)); };
    const auto lfs_ready = [&](std::uint8_t d) { return d == 1 ? psram_drive_ : flash_ok_; };
    // The four drives' function, or `fallback` for a drive select above 3
    const auto run = [&](auto fat, auto lfs, std::uint8_t fallback) -> std::uint8_t {
        switch (drive) {
        case 0:
            return fat(std::uint8_t{0});
        case 1:
        case 2:
            return lfs(drive);
        case 3:
            return fat(std::uint8_t{1});
        default:
            return fallback;
        }
    };
    const auto lfs_op = [&](auto op) {
        return [&, op](std::uint8_t d) -> std::uint8_t { return lfs_ready(d) ? op(d) : kFrNotReady; };
    };
    std::uint8_t r = 0;
    switch (code) {
    case 15:
        return copy_dir_to_psram(s, at);
    case 34:
        return create_psram_drive(s, at);
    case 35:
        return 0;  // the address goes to a local copy: the Z80 sees nothing ([FW] get_first_usable_psram_pos)
    case 36:
        return delete_psram_drive();
    case 0x81: {
        set_rom(0x1900, fast_get(at));
        set_rom(0x1901, fast_get(at + 1));
        const std::uint8_t len = fast_get(at + 2);
        if (len == 0) {
            return kFrInvalidParameter;
        }
        copy_in(0x1902, at + 2, len + 1u);
        r = run([&](std::uint8_t v) { return fat_open(s, v); }, lfs_op([&](std::uint8_t d) { return lfs_open(s, d); }), 4);
        fast_put(at, r);
        if (r == kFrOk) {
            copy_out(at, 0x1A00, 8);
            const std::uint8_t found = rom(0x1A08);
            fast_put(at + 8, found);
            copy_out(at + 9, 0x1A09, found);
        }
        return r;
    }
    case 0x82:
        handle_in();
        return run([&](std::uint8_t) { return fat_close(s); }, lfs_op([&](std::uint8_t) { return lfs_close(s); }), 4);
    case 0x83:
        handle_in();
        set_rom32(0x1904, param16(at + 4));
        r = run([&](std::uint8_t) { return fat_read(s); }, lfs_op([&](std::uint8_t) { return lfs_read(s); }), 3);
        if (r == kFrOk) {
            const unsigned n = rom(0x1A00) | rom(0x1A01) << 8;
            copy_out(at + 4, 0x1A00, 2);
            copy_out(at + 6, 0x1A02, n);
        }
        return r;
    case 0x84: {
        // The data pointer goes to D906h and the block is overwritten from DA00h: what is written is card RAM ([FW] quirk)
        handle_in();
        const std::uint16_t n = param16(at + 4);
        set_rom(0x1904, static_cast<std::uint8_t>(n));
        set_rom(0x1905, static_cast<std::uint8_t>(n >> 8));
        set_rom32(0x1906, kTvcRamAddress + at + 6);
        if (n == 0 || n + 0x1A00u > 0x2000) {
            return kFrInvalidParameter;
        }
        copy_out(at + 6, 0x1A00, n);
        r = run([&](std::uint8_t) { return fat_write(s); },
                [&](std::uint8_t d) -> std::uint8_t { return d == 2 ? kFrWriteProtected : lfs_ready(d) ? lfs_write(s) : kFrNotReady; }, 4);
        if (r == kFrOk) {
            copy_out(at + 4, 0x1A00, 2);
        }
        return r;
    }
    case 0x85: {
        const std::uint8_t len = fast_get(at);
        if (len == 0) {
            return kFrInvalidParameter;
        }
        copy_in(0x1900, at, len + 1u);
        r = run([&](std::uint8_t v) { return fat_open_dir(s, v); }, lfs_op([&](std::uint8_t d) { return lfs_open_dir(s, d); }), 4);
        if (r == kFrOk) {
            copy_out(at, 0x1A00, 4);
        }
        return r;
    }
    case 0x86:
        handle_in();
        return run([&](std::uint8_t) { return fat_close_dir(s); }, lfs_op([&](std::uint8_t) { return lfs_close_dir(s); }), 4);
    case 0x87:
        handle_in();
        r = run([&](std::uint8_t) { return fat_read_dir(s); }, lfs_op([&](std::uint8_t) { return lfs_read_dir(s); }), 4);
        if (r == kFrOk) {
            copy_out(at + 4, 0x1A00, kFilinfoSize + 1);
        }
        return r;
    case 0x88:
    case 0x89:
        handle_in();
        set_rom32(0x1904, param32(at + 4) & 0xFFFFFF);
        set_rom32(0x1907, param32(at + 7) & 0xFFFFFF);
        if (code == 0x88) {
            r = run([&](std::uint8_t) { return fat_write_source(s); },
                    [&](std::uint8_t d) -> std::uint8_t { return d == 2 ? kLfsReadOnly : lfs_ready(d) ? lfs_write_source(s) : kFrNotReady; }, 4);
        } else {
            r = run([&](std::uint8_t) { return fat_read_dest(s); }, lfs_op([&](std::uint8_t) { return lfs_read_dest(s); }), 4);
        }
        if (r == kFrOk) {
            copy_out(at + 4, 0x1A00, 3);
        }
        return r;
    case 0x8A:
        handle_in();
        set_rom32(0x1904, param32(at + 4));
        return run([&](std::uint8_t) { return fat_seek(s); }, lfs_op([&](std::uint8_t) { return lfs_seek(s); }), 4);
    case 0x8B:
        r = run([&](std::uint8_t v) { return fat_getcwd(s, v); }, lfs_op([&](std::uint8_t d) { return lfs_getcwd(d); }), 4);
        if (r == kFrOk) {
            copy_out(at, 0x1A00, rom(0x1A00) + 1u);
        }
        return r;
    case 0x8C:
    case 0x8D: {
        const std::uint8_t len = fast_get(at);
        if (len == 0) {
            return kFrInvalidParameter;
        }
        copy_in(0x1900, at, len + 1u);
        if (code == 0x8C) {
            return run([&](std::uint8_t v) { return fat_chdir(s, v); }, lfs_op([&](std::uint8_t d) { return lfs_chdir(s, d); }), 4);
        }
        return run([&](std::uint8_t v) { return fat_mkdir(s, v); },
                   [&](std::uint8_t d) -> std::uint8_t { return d == 2 ? kLfsReadOnly : lfs_ready(d) ? lfs_mkdir(s, d) : kFrNotReady; }, 4);
    }
    case 0x8E: {
        const std::uint8_t len = fast_get(at);
        set_rom(0x1900, len);
        if (len == 0) {
            return kFrInvalidParameter;
        }
        copy_in(0x1901, at + 1, len + 1u);
        return run([&](std::uint8_t v) { return fat_delete(s, v); },
                   [&](std::uint8_t d) -> std::uint8_t { return d == 2 ? kLfsReadOnly : lfs_ready(d) ? lfs_delete(s, d) : kFrNotReady; }, 3);
    }
    case 0x8F: {
        const std::uint8_t len1 = fast_get(at);
        const std::uint8_t len2 = fast_get(at + len1 + 1u);
        copy_in(0x1900, at, len1 + len2 + 2u);
        return run([&](std::uint8_t v) { return fat_rename(s, v); },
                   [&](std::uint8_t d) -> std::uint8_t { return d == 2 ? kLfsReadOnly : lfs_ready(d) ? kLfsReadOnly : kFrNotReady; }, 4);
    }
    case 0x90: {
        const std::uint8_t len = fast_get(at);
        set_rom(0x1900, len);
        if (len != 0) {
            copy_in(0x1901, at + 1, len);
        }
        r = run([&](std::uint8_t v) { return fat_stat(s, v); }, lfs_op([&](std::uint8_t d) { return lfs_stat(s, d); }), 3);
        if (r == kFrOk) {
            copy_out(at, 0x1A00, kFilinfoSize);
        }
        return r;
    }
    case 0x91:
        handle_in();
        return run([&](std::uint8_t) { return fat_sync(s); }, lfs_op([&](std::uint8_t d) { return d == 1 ? lfs_sync(s) : std::uint8_t{0}; }), 3);
    case 0x92: {
        const std::uint8_t len = fast_get(at);
        if (len == 0) {
            return kFrInvalidParameter;
        }
        copy_in(0x1900, at, len + 1u);
        switch (drive) {
        case 0:
            return fat_mount_dsk(s);
        case 1:
        case 2:
            return lfs_ready(drive) ? lfs_mount_dsk(s, drive) : kFrNotReady;
        default:
            return 1;
        }
    }
    case 0x93:
        switch (dsk_type_) {
        case 1:
            r = fat_unmount_dsk(s);
            set_rom(0x1810, 0);
            return r;
        case 2:
        case 3: {
            const std::uint8_t type = dsk_type_;
            r = lfs_ready(static_cast<std::uint8_t>(type - 1)) ? lfs_unmount_dsk(s) : kFrNotReady;
            set_rom(0x1810, static_cast<std::uint8_t>(type - 1));
            return r;
        }
        default:
            return 1;
        }
    default:
        return std::nullopt;
    }
}

// ---- The USB port and the mouse ----

void Card256::set_usb(Usb device) {
    if (device == usb_) {
        return;
    }
    if (usb_ == Usb::Pendrive) {
        // FatFs drops the volume ([FW] tuh_msc_umount_cb): its handles go stale
        for (FatFile& f : fat_files_) {
            if (f.volume == 0) {
                f.open = false;
            }
        }
        for (FatDir& d : fat_dirs_) {
            if (d.volume == 0) {
                d.open = false;
            }
        }
    }
    if (usb_ == Usb::Mouse) {
        mouse_buttons_ = 0;
    }
    usb_ = device;
}

// [FW] tvc-usb.c process_mouse_report: the sum since the last read, times the speed (3.5 fixed point), clamped
void Card256::mouse_report(int dx, int dy, std::uint8_t buttons) {
    if (usb_ != Usb::Mouse) {
        return;
    }
    mouse_buttons_ = static_cast<std::uint8_t>((buttons & 1 ? 0x01 : 0) | (buttons & 4 ? 0x02 : 0) | (buttons & 2 ? 0x04 : 0));
    const double speed = static_cast<double>(mouse_speed_ >> 5) + static_cast<double>(mouse_speed_ & 0x1F) / 32;
    const auto scaled = [&](std::int32_t& sum, bool& restart, int delta) {
        if (restart) {
            sum = 0;
            restart = false;
        }
        sum += std::clamp(delta, -127, 127);
        const long v = std::clamp(std::lround(sum * speed), -254L, 255L);
        return static_cast<std::uint8_t>(v & 0xFF);
    };
    mouse_dx_ = scaled(mouse_dx_sum_, mouse_dx_read_, dx);
    mouse_dy_ = scaled(mouse_dy_sum_, mouse_dy_read_, dy);
}

// ---- Image helpers ----

bool card256_flash_valid(std::span<const std::uint8_t> image) {
    if (image.size() < 2 * kBlock) {
        return false;
    }
    auto m = std::make_unique<LfsMount>();
    memory_device(*m, image.data(), nullptr, image.size());
    if (lfs_mount(&m->fs, &m->cfg) != 0) {
        return false;
    }
    lfs_unmount(&m->fs);
    return true;
}

std::vector<std::uint8_t> card256_flash_image(const std::vector<Card256FlashFile>& files, std::size_t bytes) {
    std::vector<std::uint8_t> image(bytes / kBlock * kBlock, 0xFF);
    auto m = std::make_unique<LfsMount>();
    memory_device(*m, image.data(), image.data(), image.size());
    if (image.size() < 2 * kBlock || lfs_format(&m->fs, &m->cfg) != 0 || lfs_mount(&m->fs, &m->cfg) != 0) {
        return {};
    }
    bool ok = true;
    for (const Card256FlashFile& f : files) {
        for (std::size_t p = f.path.find('/'); p != std::string::npos; p = f.path.find('/', p + 1)) {
            const int r = lfs_mkdir(&m->fs, f.path.substr(0, p).c_str());
            ok = ok && (r == 0 || r == LFS_ERR_EXIST);
        }
        lfs_file_t file{};
        if (lfs_file_open(&m->fs, &file, f.path.c_str(), LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC) != 0) {
            ok = false;
            continue;
        }
        ok = ok && lfs_file_write(&m->fs, &file, f.bytes.data(), static_cast<lfs_size_t>(f.bytes.size())) ==
                       static_cast<lfs_ssize_t>(f.bytes.size());
        ok = lfs_file_close(&m->fs, &file) == 0 && ok;
    }
    lfs_unmount(&m->fs);
    return ok ? image : std::vector<std::uint8_t>{};
}

std::vector<std::uint8_t> fat_image_format(std::size_t bytes) {
    std::vector<std::uint8_t> image(bytes / kSector * kSector, 0);
    MemoryBacking backing(image);
    const int drive = claim_drive(&backing);
    if (drive < 0) {
        return {};
    }
    std::vector<std::uint8_t> work(FF_MAX_SS * 4);
    const MKFS_PARM opt{FM_FAT | FM_SFD, 2, 0, 0, 0};
    const FRESULT r = f_mkfs((std::to_string(drive) + ":").c_str(), &opt, work.data(), static_cast<UINT>(work.size()));
    t_disks[static_cast<std::size_t>(drive)] = nullptr;
    return r == FR_OK ? image : std::vector<std::uint8_t>{};
}

bool fat_format(FatDisk& disk, bool partitioned) {
    const int drive = claim_drive(&disk);
    if (drive < 0) {
        return false;
    }
    std::vector<std::uint8_t> work(FF_MAX_SS * 8);
    const MKFS_PARM opt{static_cast<BYTE>(FM_FAT | FM_FAT32 | (partitioned ? 0 : FM_SFD)), 2, 0, 0, 0};
    const FRESULT r = f_mkfs((std::to_string(drive) + ":").c_str(), &opt, work.data(), static_cast<UINT>(work.size()));
    t_disks[static_cast<std::size_t>(drive)] = nullptr;
    return r == FR_OK;
}

std::unique_ptr<FatVolume> fat_disk_volume(FatDisk& disk) {
    auto volume = std::make_unique<FatImage>(disk);
    if (volume->mount() != kFrOk) {
        return nullptr;
    }
    return volume;
}

std::unique_ptr<FatVolume> fat_image_volume(std::vector<std::uint8_t>& image) {
    auto volume = std::make_unique<FatImage>(std::make_unique<MemoryBacking>(image));
    if (volume->mount() != kFrOk) {
        return nullptr;
    }
    return volume;
}

std::string fat_short_name(const std::string& name, unsigned n) {
    const auto upper = [](std::string s) {
        for (char& c : s) {
            c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
        }
        return s;
    };
    const auto legal = [](char ch) {
        const auto c = static_cast<unsigned char>(ch);
        return c > 0x20 && !std::strchr("\"*+,./:;<=>?[\\]|", c);
    };
    const std::size_t dot = name.rfind('.');
    const std::string base = dot == std::string::npos || dot == 0 ? name : name.substr(0, dot);
    const std::string ext = dot == std::string::npos || dot == 0 ? "" : name.substr(dot + 1);
    const bool fits = !base.empty() && base.size() <= 8 && ext.size() <= 3 && std::all_of(base.begin(), base.end(), legal) &&
                      std::all_of(ext.begin(), ext.end(), legal);
    if (fits) {
        const std::string u = upper(name);
        return u == name ? std::string{} : u;
    }
    std::string b;
    for (const char c : upper(base)) {
        if (c != ' ' && c != '.') {
            b += legal(c) ? c : '_';
        }
    }
    std::string e;
    for (const char c : upper(ext)) {
        if (c != ' ' && e.size() < 3) {
            e += legal(c) ? c : '_';
        }
    }
    const std::string tail = "~" + std::to_string(n);
    b = b.substr(0, 8 - std::min<std::size_t>(tail.size(), 7)) + tail;
    return e.empty() ? b : b + "." + e;
}

}  // namespace freetvc
