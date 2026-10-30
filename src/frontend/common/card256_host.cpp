// SPDX-License-Identifier: GPL-3.0-or-later
#include "frontend/common/card256_host.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <memory>
#include <system_error>

#include "core/message.hpp"
#include "ff.h"

namespace freetvc {

namespace fs = std::filesystem;

namespace {

constexpr WORD kCodePage = 437;

void put_utf8(std::string& out, std::uint32_t c) {
    if (c < 0x80) {
        out += static_cast<char>(c);
    } else if (c < 0x800) {
        out += static_cast<char>(0xC0 | c >> 6);
        out += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        out += static_cast<char>(0xE0 | c >> 12);
        out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        out += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// UTF-8 code points; a broken sequence gives U+FFFD
std::vector<std::uint32_t> code_points(const std::string& s) {
    std::vector<std::uint32_t> out;
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        const unsigned n = b < 0x80 ? 1 : (b & 0xE0) == 0xC0 ? 2 : (b & 0xF0) == 0xE0 ? 3 : (b & 0xF8) == 0xF0 ? 4 : 0;
        if (n == 0 || i + n > s.size()) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        std::uint32_t c = n == 1 ? b : b & (0x7F >> n);
        for (unsigned k = 1; k < n; ++k) {
            c = c << 6 | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        out.push_back(c);
        i += n;
    }
    return out;
}

// What FatFs compares: each character's upper case
std::vector<std::uint32_t> folded(const std::string& tvc) {
    std::vector<std::uint32_t> out;
    for (const char ch : tvc) {
        const auto b = static_cast<unsigned char>(ch);
        out.push_back(ff_wtoupper(b < 0x80 ? b : ff_oem2uni(b, kCodePage)));
    }
    return out;
}

std::string host_string(const fs::path& p) {
    const auto u8 = p.u8string();
    return std::string(u8.begin(), u8.end());
}

fs::path host_path(const std::string& utf8) { return fs::path(std::u8string(utf8.begin(), utf8.end())); }

// FAT date and time of a file's last change (UTC)
void fat_time(const fs::file_time_type& t, std::uint16_t& date, std::uint16_t& time) {
    using namespace std::chrono;
    const auto sys = time_point_cast<seconds>(file_clock::to_sys(t));
    const auto day = floor<days>(sys);
    const year_month_day ymd(day);
    const hh_mm_ss hms(sys - day);
    const int year = std::clamp(static_cast<int>(ymd.year()), 1980, 2107);
    date = static_cast<std::uint16_t>((year - 1980) << 9 | static_cast<unsigned>(ymd.month()) << 5 | static_cast<unsigned>(ymd.day()));
    time = static_cast<std::uint16_t>(hms.hours().count() << 11 | hms.minutes().count() << 5 | hms.seconds().count() / 2);
}

std::vector<std::string> components(const std::string& path) {
    std::vector<std::string> out;
    for (std::size_t p = 1; p < path.size();) {
        const std::size_t e = path.find('/', p);
        out.push_back(path.substr(p, e == std::string::npos ? std::string::npos : e - p));
        p = e == std::string::npos ? path.size() : e + 1;
    }
    return out;
}

FatEntry entry_of(const fs::directory_entry& e) {
    std::error_code ec;
    FatEntry out;
    out.name = card256_tvc_name(host_string(e.path().filename()));
    out.alt = fat_short_name(out.name, 1);
    const bool dir = e.is_directory(ec);
    out.attr = dir ? kAmDir : kAmArchive;
    if ((fs::status(e.path(), ec).permissions() & fs::perms::owner_write) == fs::perms::none) {
        out.attr |= kAmReadOnly;
    }
    out.size = dir ? 0 : e.file_size(ec);
    fat_time(e.last_write_time(ec), out.date, out.time);
    return out;
}

}  // namespace

std::string card256_host_name(const std::string& tvc) {
    std::string out;
    for (const char ch : tvc) {
        const auto b = static_cast<unsigned char>(ch);
        put_utf8(out, b < 0x80 ? b : ff_oem2uni(b, kCodePage));
    }
    return out;
}

std::string card256_tvc_name(const std::string& host) {
    std::string out;
    for (const std::uint32_t c : code_points(host)) {
        const WCHAR oem = c < 0x80 ? static_cast<WCHAR>(c) : ff_uni2oem(c, kCodePage);
        out += static_cast<char>(oem == 0 || oem > 0xFF ? '?' : oem);
    }
    return out;
}

std::uint8_t PendriveFolder::locate(const std::string& path, fs::path& host) const {
    std::error_code ec;
    if (!fs::is_directory(root_, ec)) {
        return kFrNotReady;
    }
    host = root_;
    const std::vector<std::string> parts = components(path);
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const std::vector<std::uint32_t> want = folded(parts[i]);
        std::vector<fs::path> matches;
        for (const fs::directory_entry& e : fs::directory_iterator(host, ec)) {
            if (folded(card256_tvc_name(host_string(e.path().filename()))) == want) {
                matches.push_back(e.path());
            }
        }
        if (matches.empty()) {
            if (i + 1 < parts.size()) {
                return kFrNoPath;
            }
            host /= host_path(card256_host_name(parts[i]));
            return kFrNoFile;
        }
        std::sort(matches.begin(), matches.end());  // the same pick when names differ only in case
        host = matches.front();
        if (i + 1 < parts.size() && !fs::is_directory(host, ec)) {
            return kFrNoPath;
        }
    }
    return kFrOk;
}

std::uint8_t PendriveFolder::stat(const std::string& path, FatEntry& entry) {
    fs::path host;
    const std::uint8_t r = locate(path, host);
    if (r == kFrOk) {
        entry = entry_of(fs::directory_entry(host));
    }
    return r;
}

std::uint8_t PendriveFolder::list(const std::string& dir, std::vector<FatEntry>& entries) {
    fs::path host;
    std::uint8_t r = locate(dir, host);
    std::error_code ec;
    if (r == kFrOk && !fs::is_directory(host, ec)) {
        r = kFrNoPath;
    }
    if (r != kFrOk) {
        return r == kFrNoFile ? kFrNoPath : r;
    }
    for (const fs::directory_entry& e : fs::directory_iterator(host, ec)) {
        entries.push_back(entry_of(e));
    }
    std::sort(entries.begin(), entries.end(), [](const FatEntry& a, const FatEntry& b) { return folded(a.name) < folded(b.name); });
    return ec ? kFrDiskErr : kFrOk;
}

std::uint8_t PendriveFolder::read(const std::string& path, std::uint64_t pos, std::span<std::uint8_t> out, std::size_t& got) {
    got = 0;
    fs::path host;
    if (const std::uint8_t r = locate(path, host); r != kFrOk) {
        return r;
    }
    std::ifstream in(host, std::ios::binary);
    in.seekg(static_cast<std::streamoff>(pos));
    if (!in) {
        return kFrDiskErr;
    }
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
    got = static_cast<std::size_t>(in.gcount());
    return kFrOk;
}

std::uint8_t PendriveFolder::write(const std::string& path, std::uint64_t pos, std::span<const std::uint8_t> data, std::size_t& put) {
    put = 0;
    fs::path host;
    if (const std::uint8_t r = locate(path, host); r != kFrOk) {
        return r;
    }
    std::fstream f(host, std::ios::binary | std::ios::in | std::ios::out);
    if (!f) {
        return kFrDenied;
    }
    f.seekp(static_cast<std::streamoff>(pos));
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    if (!f) {
        return kFrDiskErr;
    }
    put = data.size();
    return kFrOk;
}

std::uint8_t PendriveFolder::create(const std::string& path) {
    fs::path host;
    const std::uint8_t r = locate(path, host);
    std::error_code ec;
    if (r == kFrNoPath || r == kFrNotReady || (r == kFrOk && fs::is_directory(host, ec))) {
        return r == kFrOk ? kFrDenied : r;
    }
    std::ofstream out(host, std::ios::binary | std::ios::trunc);
    return out ? kFrOk : kFrDenied;
}

std::uint8_t PendriveFolder::make_dir(const std::string& path) {
    fs::path host;
    const std::uint8_t r = locate(path, host);
    if (r != kFrNoFile) {
        return r == kFrOk ? kFrExist : r;
    }
    std::error_code ec;
    return fs::create_directory(host, ec) ? kFrOk : kFrDenied;
}

std::uint8_t PendriveFolder::remove(const std::string& path) {
    fs::path host;
    if (const std::uint8_t r = locate(path, host); r != kFrOk) {
        return r;
    }
    std::error_code ec;
    return fs::remove(host, ec) ? kFrOk : kFrDenied;
}

std::uint8_t PendriveFolder::rename(const std::string& from, const std::string& to) {
    fs::path old_host;
    if (const std::uint8_t r = locate(from, old_host); r != kFrOk) {
        return r;
    }
    fs::path new_host;
    const std::uint8_t r = locate(to, new_host);
    std::error_code ec;
    if (r == kFrOk && !fs::equivalent(old_host, new_host, ec)) {
        return kFrExist;
    }
    if (r == kFrOk) {
        new_host = new_host.parent_path() / host_path(card256_host_name(components(to).back()));
    } else if (r != kFrNoFile) {
        return r;
    }
    fs::rename(old_host, new_host, ec);
    return ec ? kFrDenied : kFrOk;
}

bool Card256Host::plug_pendrive(const std::string& dir, std::string& error) {
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        error = msg("'%s' is not a folder", dir.c_str());
        return false;
    }
    pendrive_.emplace(fs::absolute(dir, ec));
    media_.pendrive = &*pendrive_;
    media_.usb = Card256Usb::Pendrive;
    return true;
}

void Card256Host::plug_mouse() {
    media_.usb = Card256Usb::Mouse;
    media_.pendrive = nullptr;
}

void Card256Host::unplug() {
    media_.usb = Card256Usb::None;
    media_.pendrive = nullptr;
}

bool Card256Host::load_flash(const std::string& path, std::string& error) {
    std::error_code ec;
    const fs::path p = host_path(path);
    if (fs::is_directory(p, ec)) {
        // A folder packs into an image: names in lower case, as the card's littlefs-python tool wants them
        std::vector<Card256FlashFile> files;
        std::uintmax_t total = 0;
        for (const fs::directory_entry& e : fs::recursive_directory_iterator(p, ec)) {
            if (!e.is_regular_file(ec)) {
                continue;
            }
            std::string name;
            for (const char c : card256_tvc_name(host_string(fs::relative(e.path(), p, ec).generic_u8string()))) {
                name += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            }
            total += e.file_size(ec);
            if (total > kCard256FlashBytes) {
                error = msg("'%s' holds more than the flash drive's 15 MiB", path.c_str());
                return false;
            }
            std::ifstream in(e.path(), std::ios::binary);
            files.push_back({name, std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in), {})});
        }
        std::vector<std::uint8_t> image = card256_flash_image(files, kCard256FlashBytes);
        if (image.empty()) {
            error = msg("'%s' does not fit the flash drive", path.c_str());
            return false;
        }
        set_flash(std::move(image), host_string(p.filename()) + "/");
        return true;
    }
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        error = msg("cannot read '%s'", path.c_str());
        return false;
    }
    std::vector<std::uint8_t> image(std::istreambuf_iterator<char>(in), {});
    if (!card256_flash_valid(image)) {
        error = msg("'%s' is not a littlefs image with 4 KiB blocks", path.c_str());
        return false;
    }
    set_flash(std::move(image), host_string(p.filename()));
    return true;
}

void Card256Host::set_flash(std::vector<std::uint8_t> image, std::string name) {
    media_.flash_valid = card256_flash_valid(image);
    media_.flash = std::make_shared<const std::vector<std::uint8_t>>(std::move(image));
    flash_name_ = std::move(name);
}

std::string Card256Host::describe() const {
    std::string text = "USB port: ";
    switch (media_.usb) {
    case Card256Usb::Pendrive:
        text += "pendrive " + host_string(pendrive_dir());
        break;
    case Card256Usb::Mouse:
        text += "mouse";
        break;
    default:
        text += "empty";
        break;
    }
    text += "; flash drive: " + (media_.flash ? flash_name_ + (media_.flash_valid ? "" : " (does not mount)") : std::string("none"));
    return text;
}

}  // namespace freetvc
