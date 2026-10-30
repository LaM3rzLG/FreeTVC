// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/common/sd_image.hpp"

#include <algorithm>
#include <memory>
#include <system_error>

#include "core/cards/card256_storage.hpp"
#include "core/message.hpp"

namespace freetvc {

namespace fs = std::filesystem;

namespace {

constexpr std::size_t kCopyChunk = 64 * 1024;

// FatFs's disk over a card image
class ImageDisk final : public FatDisk {
public:
    explicit ImageDisk(SdCardImage& image) : image_(image) {}
    bool read(std::uint64_t pos, std::span<std::uint8_t> out) override { return image_.read(pos, out); }
    bool write(std::uint64_t pos, std::span<const std::uint8_t> in) override { return image_.write(pos, in); }
    std::uint64_t size() override { return image_.size(); }

private:
    SdCardImage& image_;
};

bool copy_file(FatVolume& volume, const fs::path& from, const std::string& to, std::string& error) {
    std::ifstream in(from, std::ios::binary);
    if (!in) {
        error = msg("cannot read %s", from.string().c_str());
        return false;
    }
    if (volume.create(to) != kFrOk) {
        error = msg("cannot create %s in the image", to.c_str());
        return false;
    }
    std::vector<std::uint8_t> chunk(kCopyChunk);
    std::uint64_t pos = 0;
    while (in) {
        in.read(reinterpret_cast<char*>(chunk.data()), static_cast<std::streamsize>(chunk.size()));
        const auto got = static_cast<std::size_t>(in.gcount());
        if (got == 0) {
            break;
        }
        std::size_t put = 0;
        if (volume.write(to, pos, std::span(chunk).first(got), put) != kFrOk || put != got) {
            error = msg("the image is full at %s", from.string().c_str());
            return false;
        }
        pos += got;
    }
    return true;
}

bool copy_tree(FatVolume& volume, const fs::path& from, const std::string& to, std::string& error) {
    std::error_code ec;
    if (!fs::is_directory(from, ec)) {
        return copy_file(volume, from, to, error);
    }
    if (volume.make_dir(to) != kFrOk) {
        error = msg("cannot make %s in the image", to.c_str());
        return false;
    }
    std::vector<fs::path> entries;
    for (const fs::directory_entry& e : fs::directory_iterator(from, ec)) {
        entries.push_back(e.path());
    }
    std::sort(entries.begin(), entries.end());
    for (const fs::path& e : entries) {
        if (!copy_tree(volume, e, to + "/" + e.filename().string(), error)) {
            return false;
        }
    }
    return true;
}

void list_tree(FatVolume& volume, const std::string& dir, std::vector<std::string>& out) {
    std::vector<FatEntry> entries;
    if (volume.list(dir, entries) != kFrOk) {
        return;
    }
    for (const FatEntry& e : entries) {
        const std::string path = (dir == "/" ? std::string() : dir) + "/" + e.name;
        if (e.attr & kAmDir) {
            out.push_back(path + "/");
            list_tree(volume, path, out);
        } else {
            out.push_back(path);
        }
    }
}

}  // namespace

bool SdImageFile::open(const std::string& path, std::string& error) {
    close();
    std::error_code ec;
    const std::uint64_t size = fs::file_size(path, ec);
    if (ec) {
        error = msg("cannot open %s: %s", path.c_str(), ec.message().c_str());
        return false;
    }
    if (size % SdCard::kBlock != 0 || size < kMinSize || size > kMaxSize) {
        error = msg("%s: an SD card image is whole 512-byte blocks, 64 KiB to 2 TiB (%llu bytes)", path.c_str(), static_cast<unsigned long long>(size));
        return false;
    }
    file_.open(path, std::ios::binary | std::ios::in | std::ios::out);
    read_only_ = !file_.is_open();
    if (read_only_) {
        file_.clear();
        file_.open(path, std::ios::binary | std::ios::in);
    }
    if (!file_.is_open()) {
        error = msg("cannot open %s", path.c_str());
        return false;
    }
    path_ = path;
    size_ = size;
    failed_ = false;
    return true;
}

void SdImageFile::close() {
    if (file_.is_open()) {
        file_.close();
    }
    file_.clear();
    path_.clear();
    size_ = 0;
    read_only_ = false;
    failed_ = false;
}

bool SdImageFile::read(std::uint64_t offset, std::span<std::uint8_t> out) {
    if (!file_.is_open() || offset + out.size() > size_) {
        return false;
    }
    file_.clear();
    file_.seekg(static_cast<std::streamoff>(offset));
    file_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    const bool ok = static_cast<std::size_t>(file_.gcount()) == out.size();
    failed_ = failed_ || !ok;
    return ok;
}

bool SdImageFile::write(std::uint64_t offset, std::span<const std::uint8_t> data) {
    if (!file_.is_open() || read_only_ || offset + data.size() > size_) {
        return false;
    }
    file_.clear();
    file_.seekp(static_cast<std::streamoff>(offset));
    file_.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    file_.flush();
    const bool ok = static_cast<bool>(file_);
    failed_ = failed_ || !ok;
    return ok;
}

bool is_sddual_firmware(std::span<const std::uint8_t> image) {
    static constexpr std::uint8_t kHeader[] = {'M', 'O', 'P', 'S', 0xAF, 0x32, 0x02, 0xFC};
    return image.size() >= sizeof kHeader && image.size() <= 0x10000 && std::equal(std::begin(kHeader), std::end(kHeader), image.begin());
}

bool write_sd_image(const std::string& out, std::uint64_t mib, const std::vector<std::string>& files, std::string& error) {
    if (mib == 0 || mib > (SdImageFile::kMaxSize >> 20)) {
        error = msg("an SD card image is 1 MiB to 2 TiB");
        return false;
    }
    const fs::path temp = out + ".tmp";
    std::error_code ec;
    {
        std::ofstream create(temp, std::ios::binary | std::ios::trunc);
        if (!create) {
            error = msg("cannot write %s", temp.string().c_str());
            return false;
        }
    }
    fs::resize_file(temp, mib << 20, ec);
    const auto fail = [&](std::string why) {
        error = std::move(why);
        std::error_code ignored;
        fs::remove(temp, ignored);
        return false;
    };
    if (ec) {
        return fail(msg("cannot write %s: %s", temp.string().c_str(), ec.message().c_str()));
    }
    // The image is closed before the temporary file goes (Windows cannot remove an open file)
    const auto build = [&]() -> std::string {
        SdImageFile image;
        std::string why;
        if (!image.open(temp.string(), why)) {
            return why;
        }
        ImageDisk disk(image);
        if (!fat_format(disk, true)) {
            return msg("FatFs could not format %llu MiB", static_cast<unsigned long long>(mib));
        }
        std::unique_ptr<FatVolume> volume = fat_disk_volume(disk);
        if (!volume) {
            return msg("the new FAT volume does not mount");
        }
        for (const std::string& f : files) {
            if (!copy_tree(*volume, f, "/" + fs::path(f).filename().string(), why)) {
                return why;
            }
        }
        volume.reset();
        return image.failed() ? msg("cannot write %s", temp.string().c_str()) : std::string();
    };
    if (std::string why = build(); !why.empty()) {
        return fail(std::move(why));
    }
    fs::rename(temp, out, ec);
    if (ec) {
        return fail(msg("cannot write %s: %s", out.c_str(), ec.message().c_str()));
    }
    return true;
}

std::vector<std::string> sd_image_listing(const std::string& path) {
    std::vector<std::string> out;
    SdImageFile image;
    std::string error;
    if (!image.open(path, error)) {
        return out;
    }
    ImageDisk disk(image);
    if (std::unique_ptr<FatVolume> volume = fat_disk_volume(disk)) {
        list_tree(*volume, "/", out);
    }
    return out;
}

}  // namespace freetvc
