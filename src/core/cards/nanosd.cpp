// SPDX-License-Identifier: GPL-3.0-or-later
// Written from the NanoSD firmware v0.32 (TVC_NanoCartSD.ino, Vass Sándor, BSD-3-Clause) and the Arduino SD library it
// uses; design/cards.md "NanoSD".
#include "core/cards/nanosd.hpp"

#include <algorithm>
#include <string_view>

#include "core/cards/card256_storage.hpp"
#include "core/snapshot/snapshot.hpp"

namespace freetvc {

namespace {

constexpr std::uint8_t kSaveVersion = 1;

// COMMANDS
constexpr std::uint8_t kOpen = 0x00;
constexpr std::uint8_t kClose = 0x01;
constexpr std::uint8_t kChdir = 0x02;
constexpr std::uint8_t kList = 0x03;
constexpr std::uint8_t kAckList = 0x04;
constexpr std::uint8_t kGetData = 0x05;
constexpr std::uint8_t kFileInfo = 0x06;
constexpr std::uint8_t kGetCdir = 0x07;
constexpr std::uint8_t kBank0 = 0x08;
constexpr std::uint8_t kBank1 = 0x09;
constexpr std::uint8_t kCreate = 0x0A;
constexpr std::uint8_t kPutData = 0x0B;
constexpr std::uint8_t kCloseWrite = 0x0C;
constexpr std::uint8_t kGetParam = 0x0D;
constexpr std::uint8_t kSetParam = 0x0E;
constexpr std::uint8_t kMkdir = 0x0F;
constexpr std::uint8_t kRmdir = 0x10;
constexpr std::uint8_t kDelete = 0x11;
constexpr std::uint8_t kCardDetect = 0x12;  // pushed by the pin-change interrupt
constexpr std::uint8_t kAlive = 0xFE;       // answered without an SD card

// STATUS
constexpr std::uint8_t kOk = 0x00;
constexpr std::uint8_t kOpenOk = 0x01;
constexpr std::uint8_t kNotReadyYet = 0x80;
constexpr std::uint8_t kWaitingForInput = 0x81;
constexpr std::uint8_t kFileNotFound = 0x82;
constexpr std::uint8_t kEndOfFile = 0x83;
constexpr std::uint8_t kEndOfList = 0x84;
constexpr std::uint8_t kCardNotReady = 0x86;
constexpr std::uint8_t kBufferOverrun = 0x88;
constexpr std::uint8_t kBufferUnderrun = 0x89;
constexpr std::uint8_t kCloseOk = 0x8A;
constexpr std::uint8_t kCloseFailed = 0x8B;
constexpr std::uint8_t kChdirOk = 0x8C;
constexpr std::uint8_t kChdirFailed = 0x8D;
constexpr std::uint8_t kBankSelectDone = 0x8E;
constexpr std::uint8_t kCreateOk = 0x8F;
constexpr std::uint8_t kCreateFailed = 0x90;
constexpr std::uint8_t kDataReceived = 0x91;
constexpr std::uint8_t kDataFailed = 0x92;
constexpr std::uint8_t kMkdirOk = 0x93;
constexpr std::uint8_t kMkdirFailed = 0x94;
constexpr std::uint8_t kRmdirOk = 0x95;
constexpr std::uint8_t kRmdirFailed = 0x96;
constexpr std::uint8_t kDeleteOk = 0x97;
constexpr std::uint8_t kDeleteFailed = 0x98;
constexpr std::uint8_t kInvalidParameter = 0x99;
constexpr std::uint8_t kParameterSet = 0x9A;
constexpr std::uint8_t kNoSd = 0xFF;

constexpr std::string_view kNanoId = "NanoSD Arduino fw v0.32";  // the release build has no "-dbg"

using File = NanoSd::File;

bool ends_with(const std::string& s, std::string_view tail) { return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0; }
bool starts_with(const std::string& s, std::string_view head) { return s.compare(0, head.size(), head) == 0; }

// Arduino String::toUpperCase (C locale)
std::string upper(std::string s) {
    for (char& c : s) {
        if (c >= 'a' && c <= 'z') {
            c = static_cast<char>(c - 'a' + 'A');
        }
    }
    return s;
}

// SdFile::make83Name: the blank-filled 11-byte name, or false
bool make83(const std::string& s, std::array<char, 11>& out) {
    out.fill(' ');
    unsigned n = 7;
    unsigned i = 0;
    for (const char ch : s) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == '.') {
            if (n == 10) {
                return false;
            }
            n = 10;
            i = 8;
            continue;
        }
        if (std::string_view("|<>^+=?/[];,*\"\\").find(static_cast<char>(c)) != std::string_view::npos || i > n || c < 0x21 || c > 0x7E) {
            return false;
        }
        out[i++] = static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c);
    }
    return out[0] != ' ';
}

// The 8.3 name the library finds in a directory entry: FatFs's alias, else the name itself
std::string short_name(const FatEntry& e) { return e.alt.empty() ? e.name : e.alt; }

std::array<char, 11> entry83(const FatEntry& e) {
    std::array<char, 11> out;
    out.fill(' ');
    const std::string s = short_name(e);
    const auto dot = s.rfind('.');
    const std::string base = s.substr(0, dot);
    const std::string ext = dot == std::string::npos ? std::string() : s.substr(dot + 1);
    std::copy_n(base.begin(), std::min<std::size_t>(base.size(), 8), out.begin());
    std::copy_n(ext.begin(), std::min<std::size_t>(ext.size(), 3), out.begin() + 8);
    return out;
}

// SdFile::dirName: NAME.EXT from the 11 bytes
std::string dir_name(const std::array<char, 11>& n) {
    std::string out;
    for (unsigned i = 0; i < 11; ++i) {
        if (n[i] == ' ') {
            continue;
        }
        if (i == 8) {
            out += '.';
        }
        out += n[i];
    }
    return out;
}

std::string join(const std::string& dir, const std::string& name) { return dir == "/" ? "/" + name : dir + "/" + name; }

File root_file() {
    File f;
    f.valid = f.dir = true;
    f.path = f.name = "/";
    return f;
}

File file_of(const FatEntry& e, const std::string& path, std::string name) {
    File f;
    f.valid = true;
    f.dir = (e.attr & kAmDir) != 0;
    f.path = path;
    f.name = std::move(name);
    f.size = f.dir ? 0 : e.size;
    return f;
}

// SdFile::open(dir, name): the entry whose 8.3 name matches; `parent` must be a directory
bool find(FatVolume& v, const File& parent, const std::string& name, FatEntry& entry, std::string& path) {
    std::array<char, 11> want;
    if (!parent.valid || !parent.dir || !make83(name, want)) {
        return false;
    }
    std::vector<FatEntry> entries;
    if (v.list(parent.path, entries) != kFrOk) {
        return false;
    }
    for (const FatEntry& e : entries) {
        if (entry83(e) == want) {
            entry = e;
            path = join(parent.path, e.name);
            return true;
        }
    }
    return false;
}

File child(FatVolume& v, const File& parent, const std::string& name) {
    FatEntry e;
    std::string path;
    if (!find(v, parent, name, e, path)) {
        return {};
    }
    return file_of(e, path, dir_name(entry83(e)));
}

// SD.open(path, mode): SDClass::getParentDir, then the last component
File sd_open(FatVolume* v, const std::string& filepath, bool write = false) {
    if (!v) {
        return {};
    }
    File parent = root_file();
    std::string p = filepath;
    while (p.find('/') != std::string::npos) {
        if (p[0] == '/') {
            p.erase(0, 1);
            continue;
        }
        const std::size_t idx = std::min<std::size_t>(p.find('/'), 12);  // "don't let them specify long names"
        parent = child(*v, parent, p.substr(0, idx));
        if (!parent.valid) {
            return {};
        }
        p.erase(0, idx);
    }
    if (p.empty()) {
        parent.name = "/";  // File(parentdir, "/"): the directory itself
        return parent;
    }
    File f;
    std::array<char, 11> n83;
    FatEntry e;
    std::string path;
    const std::string name = p.substr(0, 12);  // File::_name holds 12 characters
    if (find(*v, parent, p, e, path)) {
        f = file_of(e, path, name);
        if (write) {
            // O_WRITE on a directory or a read-only file fails; O_APPEND starts at the end
            if (f.dir || (e.attr & kAmReadOnly)) {
                return {};
            }
            f.write = true;
            f.pos = f.size;
        }
    } else if (write && parent.dir && make83(p, n83)) {
        path = join(parent.path, dir_name(n83));
        if (v->create(path) != kFrOk) {
            return {};
        }
        f.valid = f.write = true;
        f.path = path;
        f.name = name;
    }
    return f;
}

// checkIdInEeprom, else initEeprom: the ID, menu on, no sort
void init_eeprom(NanoSdMedia& m) {
    if (!m.eeprom_valid) {
        m.eeprom_valid = true;
        m.menu = 1;
        m.sort = 0;
    }
}

// SD.begin: a volume whose root can be read
bool sd_begin(FatVolume* v) {
    std::vector<FatEntry> entries;
    return v && v->list("/", entries) == kFrOk;
}

// walkPath's getNextPathComponent split: a leading '/' skipped, 12 characters at most per component
std::vector<std::string> walk_components(const std::string& path) {
    std::vector<std::string> out;
    std::size_t offset = 0;
    for (;;) {
        if (offset < path.size() && path[offset] == '/') {
            ++offset;
        }
        std::string buffer;
        while (buffer.size() < 12 && offset < path.size() && path[offset] != '/') {
            buffer += path[offset++];
        }
        if (offset < path.size() && path[offset] == '/') {
            ++offset;
        }
        out.push_back(buffer);
        if (offset >= path.size()) {
            return out;
        }
    }
}

enum class Walk { Exists, Mkdir, Rmdir, Remove };

// SD.exists / mkdir / rmdir / remove through walkPath and their callbacks
bool sd_walk(FatVolume* v, const std::string& path, Walk what) {
    if (!v) {
        return false;
    }
    const std::vector<std::string> parts = walk_components(path);
    File parent = root_file();
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const bool last = i + 1 == parts.size();
        File c = child(*v, parent, parts[i]);
        switch (what) {
        case Walk::Exists:
            if (!c.valid) {
                return false;
            }
            break;
        case Walk::Mkdir:
            if (!c.valid) {
                std::array<char, 11> n83;
                if (!parent.dir || !make83(parts[i], n83) || v->make_dir(join(parent.path, dir_name(n83))) != kFrOk) {
                    return false;
                }
                c = child(*v, parent, parts[i]);
            }
            break;
        case Walk::Rmdir:
            if (last) {
                std::vector<FatEntry> entries;
                return c.valid && c.dir && v->list(c.path, entries) == kFrOk && entries.empty() && v->remove(c.path) == kFrOk;
            }
            break;
        case Walk::Remove:
            if (last) {
                FatEntry e;
                return c.valid && !c.dir && v->stat(c.path, e) == kFrOk && !(e.attr & kAmReadOnly) && v->remove(c.path) == kFrOk;
            }
            break;
        }
        if (last) {
            return true;
        }
        if (!c.valid) {
            return false;
        }
        parent = c;
    }
    return true;
}

// File::openNextFile on a directory
File next_file(FatVolume* v, File& dir) {
    std::vector<FatEntry> entries;
    if (!v || !dir.valid || !dir.dir || v->list(dir.path, entries) != kFrOk || dir.next >= entries.size()) {
        return {};
    }
    const FatEntry& e = entries[dir.next++];
    return file_of(e, join(dir.path, e.name), dir_name(entry83(e)));
}

// fillFileInfoBuffer: 12-byte padded name, type, size (a ".CAS" name loses 128 bytes, unchecked)
std::array<std::uint8_t, 17> file_info_bytes(const File& f) {
    std::array<std::uint8_t, 17> data;
    data.fill(0x20);
    std::copy_n(f.name.begin(), std::min<std::size_t>(f.name.size(), 12), data.begin());
    data[12] = f.dir ? 0 : 1;
    std::uint32_t size = f.dir ? 0 : static_cast<std::uint32_t>(f.size);
    if (!f.dir && f.name.size() > 3 && ends_with(f.name, ".CAS")) {
        size -= 128;
    }
    for (unsigned i = 0; i < 4; ++i) {
        data[13 + i] = static_cast<std::uint8_t>(size >> (8 * i));
    }
    return data;
}

void save_file(ChunkWriter& w, const File& f) {
    w.boolean(f.valid);
    w.boolean(f.dir);
    w.boolean(f.write);
    w.text(f.path);
    w.text(f.name);
    w.u64(f.size);
    w.u64(f.pos);
    w.u32(f.next);
}

File load_file(ChunkReader& r) {
    File f;
    f.valid = r.boolean();
    f.dir = r.boolean();
    f.write = r.boolean();
    f.path = r.text();
    f.name = r.text();
    f.size = r.u64();
    f.pos = r.u64();
    f.next = r.u32();
    return f;
}

void save_queue(ChunkWriter& w, const std::deque<std::uint8_t>& q) {
    w.u8(static_cast<std::uint8_t>(q.size()));
    for (const std::uint8_t b : q) {
        w.u8(b);
    }
}

std::deque<std::uint8_t> load_queue(ChunkReader& r) {
    std::deque<std::uint8_t> q(std::min<std::size_t>(r.u8(), NanoSd::kQueueSize));
    for (std::uint8_t& b : q) {
        b = r.u8();
    }
    return q;
}

}  // namespace

NanoSd::NanoSd(std::span<const std::uint8_t> rom) {
    rom_.fill(0xFF);
    std::copy_n(rom.begin(), std::min(rom.size(), rom_.size()), rom_.begin());
    power_on();
}

void NanoSd::select_bank(unsigned bank) {
    bank_ = bank & 1;
    std::copy_n(rom_.begin() + static_cast<std::ptrdiff_t>(0xC000 + bank_ * kBankSize), kBankSize, window_.begin());
}

// The Nano boots; own_media_ keeps its EEPROM
void NanoSd::power_on() {
    bank_store_ = 0;
    select_bank(0);
    booted_ = false;
    latch_ = 0xFF;
    status_ = kWaitingForInput;
    hung_ = false;
    in_.clear();
    out_.clear();
    pin_ = false;
    pcint_on_ = true;
    sd_ready_ = false;
    debounce_until_ = 0;
    pin_changed_ = 0;
    in_command_ = false;
    cmd_ = 0;
    phase_ = 0;
    arg_.clear();
    number_ = 0;
    data_.clear();
    cur_dir_ = "/";
    dir_file_ = {};
    file_ = {};
    list_dir_ = {};
    file_open_ = false;
    counter_ = 0;
    save_type_ = 0;
}

NanoSdMedia& NanoSd::media(const CardBus& bus) { return bus.nanosd ? *bus.nanosd : own_media_; }

void NanoSd::setup(const CardBus& bus) {
    booted_ = true;
    FatVolume* sd = media(bus).sd;
    pin_ = sd != nullptr;
    sd_ready_ = sd_begin(sd);
    if (sd_ready_) {
        status_ = kNotReadyYet;
        dir_file_ = sd_open(sd, "/");
    }
    if (!sd_ready_ || !dir_file_.valid) {
        status_ = kCardNotReady;
    }
    latch_ = status_;  // initTVC_IN_register
    select_bank(0);
}

void NanoSd::sense_card(const CardBus& bus) {
    const NanoSdMedia& m = media(bus);
    const bool present = m.sd != nullptr;
    if (present != pin_) {
        pin_ = present;
        if (pcint_on_) {
            // The Nano has been idle since the last access, so the event ran when the pin moved
            pin_changed_ = std::min(m.sd_changed, bus.tick);
            push_input(kCardDetect);
            pcint_on_ = false;  // PCICR = 0 until the handler is done
        }
    }
}

void NanoSd::push_input(std::uint8_t value) {
    if (in_.size() >= kQueueSize) {
        status_ = kBufferOverrun;
    } else {
        in_.push_back(value);
    }
}

std::uint8_t NanoSd::io_read(unsigned, const CardBus& bus) {
    if (!booted_) {
        setup(bus);
    }
    sense_card(bus);
    run(bus);
    const std::uint8_t value = latch_;
    // INT0 after the read: the next byte, with the bank line on the same port write
    select_bank(bank_store_);
    if (status_ & 0x80) {
        latch_ = status_;
    } else if (!out_.empty()) {
        latch_ = out_.front();
        out_.pop_front();
    } else {
        latch_ = kBufferUnderrun;
    }
    return value;
}

void NanoSd::io_write(unsigned, std::uint8_t value, const CardBus& bus) {
    if (!booted_) {
        setup(bus);
    }
    sense_card(bus);
    run(bus);
    push_input(value);
    run(bus);
}

// A hung main loop never gets past its push, so it sets nothing more; the interrupts still run
void NanoSd::set_status(std::uint8_t value) {
    if (!hung_) {
        status_ = value;
    }
}

bool NanoSd::pop(std::uint8_t& value) {
    if (in_.empty()) {
        return false;
    }
    value = in_.front();
    in_.pop_front();
    return true;
}

bool NanoSd::collect_string() {
    std::uint8_t c;
    while (pop(c)) {
        if (c == 0) {
            return true;
        }
        arg_ += static_cast<char>(c);
    }
    return false;
}

void NanoSd::push(std::uint8_t value) {
    if (hung_ || out_.size() >= kQueueSize) {
        hung_ = true;  // CircularQueue::push spins while the status keeps every read from draining it
        return;
    }
    out_.push_back(value);
}

void NanoSd::push_block(std::span<const std::uint8_t> data) {
    if (hung_ || out_.size() + data.size() > kQueueSize) {
        hung_ = true;
        return;
    }
    out_.insert(out_.end(), data.begin(), data.end());
}

void NanoSd::run(const CardBus& bus) {
    while (!hung_) {
        if (!in_command_) {
            if (in_.empty()) {
                return;
            }
            cmd_ = in_.front();
            in_.pop_front();
            in_command_ = true;
            phase_ = 0;
            arg_.clear();
            set_status(kNotReadyYet);
            if (!sd_ready_ && cmd_ != kGetParam && cmd_ != kCardDetect) {
                if (cmd_ == kAlive) {
                    push(75);
                    set_status(kOk);
                } else {
                    set_status(kNoSd);
                    in_.clear();
                }
                in_command_ = false;
                continue;
            }
        }
        if (!step(bus)) {
            return;
        }
        in_command_ = false;
    }
}

std::string NanoSd::file_path(std::string name) const {
    if (starts_with(name, "./")) {
        name = name.substr(2);
    }
    if (!starts_with(name, "/")) {
        name = cur_dir_ + name;
    }
    if (!ends_with(name, "/")) {
        name += '/';
    }
    // PathStack: up to 8 components, "../" drops the last one
    std::vector<std::pair<std::size_t, std::size_t>> stack;
    for (std::size_t start = 0; start < name.size();) {
        const std::size_t pos = name.find('/', start);
        if (name.compare(start, pos + 1 - start, "../") != 0) {
            if (stack.size() < 8) {
                stack.emplace_back(start, pos + 1);
            }
        } else if (!stack.empty()) {
            stack.pop_back();
        }
        start = pos + 1;
    }
    std::string out;
    for (const auto& [b, e] : stack) {
        out += name.substr(b, e - b);
    }
    return out;
}

bool NanoSd::step(const CardBus& bus) {
    NanoSdMedia& m = media(bus);
    std::uint8_t b;
    switch (cmd_) {
    case kOpen:
        if (phase_ == 0) {
            set_status(kWaitingForInput);
            if (file_open_) {
                file_.valid = false;
            }
            phase_ = 1;
        }
        if (!collect_string()) {
            return false;
        }
        finish_open(bus);
        return true;
    case kClose:
        if (file_open_) {
            file_.valid = false;
            file_open_ = false;
        }
        counter_ = 0;
        out_.clear();
        set_status(kCloseOk);
        return true;
    case kGetData:
        get_data(bus);
        return true;
    case kList:
        if (phase_ == 0) {
            list_dir_ = sd_open(m.sd, cur_dir_);
            list_dir_.next = 0;
            phase_ = 1;
        }
        return list_next(bus);
    case kChdir:
        if (phase_ == 0) {
            set_status(kWaitingForInput);
            phase_ = 1;
        }
        if (!collect_string()) {
            return false;
        }
        set_status(kNotReadyYet);
        {
            const std::string path = file_path(arg_);
            const File dir = sd_open(m.sd, path);
            if (dir.valid && dir.dir) {
                dir_file_ = dir;
                dir_file_.next = 0;
                cur_dir_ = path;
                set_status(kChdirOk);
            } else {
                set_status(kChdirFailed);
            }
        }
        return true;
    case kFileInfo:
        if (phase_ == 0) {
            set_status(kWaitingForInput);
            phase_ = 1;
        }
        if (!collect_string()) {
            return false;
        }
        file_info(bus);
        return true;
    case kGetCdir: {
        const std::size_t len = std::min<std::size_t>({cur_dir_.size(), 127, kQueueSize - 1});
        push(static_cast<std::uint8_t>(len));
        for (std::size_t i = 0; i < len; ++i) {
            push(static_cast<std::uint8_t>(cur_dir_[i]));
        }
        set_status(kOk);
        return true;
    }
    case kBank0:
    case kBank1:
        bank_store_ = cmd_ == kBank1 ? 1 : 0;
        set_status(kBankSelectDone);
        return true;
    case kCreate:
        if (phase_ == 0) {
            if (!pop(number_)) {
                return false;
            }
            phase_ = 1;
        }
        if (phase_ == 1) {
            while (arg_.size() < number_ && arg_.size() < 63) {
                if (!pop(b)) {
                    return false;
                }
                arg_ += static_cast<char>(b);
            }
            phase_ = 2;
        }
        if (!pop(save_type_)) {
            return false;
        }
        create(bus);
        return true;
    case kPutData:
        if (phase_ == 0) {
            if (!pop(number_)) {
                return false;
            }
            data_.clear();
            phase_ = 1;
        }
        while (data_.size() < number_) {
            if (!pop(b)) {
                return false;
            }
            data_.push_back(b);
        }
        put_data(bus);
        return true;
    case kCloseWrite:
        close_write(bus);
        return true;
    case kGetParam:
        if (phase_ == 0) {
            init_eeprom(m);
            phase_ = 1;
        }
        if (!pop(b)) {
            return false;
        }
        number_ = b;
        get_parameter(bus);
        return true;
    case kSetParam:
        if (phase_ == 0) {
            init_eeprom(m);
            phase_ = 1;
        }
        if (phase_ == 1) {
            if (!pop(number_)) {
                return false;
            }
            if (number_ > 1) {
                set_status(kInvalidParameter);  // no value byte taken
                return true;
            }
            phase_ = 2;
        }
        if (!pop(b)) {
            return false;
        }
        (number_ == 0 ? m.menu : m.sort) = b;
        set_status(kParameterSet);
        return true;
    case kMkdir:
    case kRmdir:
    case kDelete: {
        if (!collect_string()) {
            return false;
        }
        const std::string path = file_path(arg_);
        if (cmd_ == kMkdir) {
            set_status(!sd_walk(m.sd, path, Walk::Exists) && sd_walk(m.sd, path, Walk::Mkdir) ? kMkdirOk : kMkdirFailed);
        } else if (cmd_ == kRmdir) {
            set_status(sd_walk(m.sd, path, Walk::Exists) && sd_walk(m.sd, path, Walk::Rmdir) ? kRmdirOk : kRmdirFailed);
        } else {
            set_status(sd_walk(m.sd, path, Walk::Exists) && sd_walk(m.sd, path, Walk::Remove) ? kDeleteOk : kDeleteFailed);
        }
        return true;
    }
    case kCardDetect:
        if (phase_ == 0) {
            debounce_until_ = pin_changed_ + kDebounceTicks;
            phase_ = 1;
        }
        if (bus.tick < debounce_until_) {
            return false;
        }
        card_changed(bus);
        return true;
    default:
        return true;  // unknown: the status stays DATA_NOT_READY_YET
    }
}

void NanoSd::finish_open(const CardBus& bus) {
    FatVolume* sd = media(bus).sd;
    std::string name = arg_;
    set_status(kNotReadyYet);
    if (name.empty()) {
        // The first or next .CAS file of the current directory
        for (;;) {
            file_ = next_file(sd, dir_file_);
            if (!file_.valid || (!file_.dir && ends_with(file_.name, ".CAS"))) {
                break;
            }
        }
    } else {
        if (!starts_with(name, "/")) {
            name = cur_dir_ + name;
        }
        name = upper(name);
        if (!sd_walk(sd, name, Walk::Exists) && !ends_with(name, ".CAS")) {
            name += ".CAS";
        }
        file_ = sd_open(sd, name);
    }
    if (!file_.valid) {
        set_status(kFileNotFound);
        file_open_ = false;
        return;
    }
    std::string shown = upper(file_.name);
    // openFile.read(): the first byte, -1 for a directory or an empty file ❓ (a directory reads its raw entries on the card)
    std::uint8_t type = 0xFF;
    std::size_t got = 0;
    if (!file_.dir) {
        sd->read(file_.path, 0, std::span<std::uint8_t>(&type, 1), got);
        if (got == 0) {
            type = 0xFF;
        }
    }
    const bool cas = ends_with(shown, ".CAS") && (type == 0x01 || type == 0x11);
    if (shown.size() >= 4) {
        shown.resize(shown.size() - 4);  // String::substring(0, length - 4); shorter names stay whole
    }
    push(static_cast<std::uint8_t>(shown.size()));
    for (const char c : shown) {
        push(static_cast<std::uint8_t>(c));
    }
    counter_ = static_cast<std::int32_t>(file_.size);
    if (cas) {
        counter_ -= 0x80;
        file_.pos = 0x80;
    } else {
        file_.pos = 0;
    }
    for (unsigned i = 0; i < 4; ++i) {
        push(static_cast<std::uint8_t>(static_cast<std::uint32_t>(counter_) >> (8 * i)));
    }
    set_status(kOpenOk);
    file_open_ = true;
}

void NanoSd::get_data(const CardBus& bus) {
    // toRead = min(remainingBytes, 64) as a byte; a CAS file shorter than its header leaves a negative count ❓
    const std::size_t to_read = counter_ > 0 ? static_cast<std::size_t>(std::min<std::int32_t>(counter_, 64)) : 0;
    std::array<std::uint8_t, 65> buffer{};
    std::size_t got = 0;
    FatVolume* sd = media(bus).sd;
    if (to_read > 0 && file_.valid && !file_.dir && sd &&
        sd->read(file_.path, file_.pos, std::span<std::uint8_t>(buffer.data() + 1, to_read), got) == kFrOk) {
        file_.pos += got;
    }
    if (got > 0) {
        buffer[0] = static_cast<std::uint8_t>(got);
        push_block(std::span<const std::uint8_t>(buffer.data(), got + 1));
        counter_ -= static_cast<std::int32_t>(got);
        set_status(kOk);
    } else {
        set_status(kEndOfFile);
    }
}

bool NanoSd::list_next(const CardBus& bus) {
    FatVolume* sd = media(bus).sd;
    for (;;) {
        if (phase_ == 1) {
            const File entry = next_file(sd, list_dir_);
            if (!entry.valid) {
                set_status(kEndOfList);
                return true;
            }
            std::array<std::uint8_t, 18> data;
            data[0] = kOk;
            const auto info = file_info_bytes(entry);
            std::copy(info.begin(), info.end(), data.begin() + 1);
            push_block(data);
            if (hung_) {
                return false;
            }
            set_status(kOk);
            phase_ = 2;
        }
        std::uint8_t ack;
        if (!pop(ack)) {
            return false;
        }
        set_status(kNotReadyYet);
        if (ack != kAckList) {
            out_.clear();  // READ_OUT_OF_SYNC, then END_OF_LIST
            set_status(kEndOfList);
            return true;
        }
        phase_ = 1;
    }
}

void NanoSd::file_info(const CardBus& bus) {
    std::string name = arg_;
    if (!starts_with(name, "/")) {
        name = cur_dir_ + name;
    }
    set_status(kNotReadyYet);
    const File f = sd_open(media(bus).sd, name);
    if (!f.valid) {
        set_status(kFileNotFound);
        return;
    }
    // buffer[17] gets OK and 17 bytes, but only 17 go out: the size's top byte is lost
    std::array<std::uint8_t, 17> data;
    data[0] = kOk;
    const auto info = file_info_bytes(f);
    std::copy_n(info.begin(), 16, data.begin() + 1);
    push_block(data);
    set_status(kOk);
}

void NanoSd::create(const CardBus& bus) {
    std::string name = arg_.empty() ? std::string("NONAME.CAS") : arg_;
    if (name.rfind('.') == std::string::npos) {
        name += ".CAS";
    } else {
        // lastIndexOf(".CAS") != length() - 4 in the AVR's 16-bit ints: -1 equals 3 - 4, so "A.B" keeps its type
        const std::size_t cas = name.rfind(".CAS");
        const auto index = static_cast<std::uint16_t>(cas == std::string::npos ? -1 : static_cast<int>(cas));
        if (index != static_cast<std::uint16_t>(name.size() - 4)) {
            save_type_ = 0;
        }
    }
    if (!starts_with(name, "/")) {
        name = cur_dir_ + name;
    }
    file_.valid = false;
    FatVolume* sd = media(bus).sd;
    // FILE_WRITE is O_CREAT | O_APPEND in this SD library: an existing file is appended to ⚠
    file_ = sd_open(sd, name, true);
    if (!file_.valid) {
        set_status(kCreateFailed);
        return;
    }
    if (save_type_ == 0x01 || save_type_ == 0x11) {
        std::array<std::uint8_t, 128> header{};
        header[0] = save_type_;
        std::size_t put = 0;
        sd->write(file_.path, file_.pos, header, put);
        file_.pos += put;
    }
    counter_ = 0;
    set_status(kCreateOk);
}

void NanoSd::put_data(const CardBus& bus) {
    bool ok = false;
    FatVolume* sd = media(bus).sd;
    if (file_.valid && file_.write && sd) {
        std::size_t put = 0;
        ok = sd->write(file_.path, file_.pos, data_, put) == kFrOk && put == data_.size();
        file_.pos += put;
    }
    counter_ += number_;
    set_status(ok || data_.empty() ? kDataReceived : kDataFailed);
}

void NanoSd::close_write(const CardBus& bus) {
    if (!file_.valid) {
        set_status(kCloseFailed);
        return;
    }
    FatVolume* sd = media(bus).sd;
    if ((save_type_ == 0x01 || save_type_ == 0x11) && file_.write && sd) {
        const unsigned missing = 128 - static_cast<std::uint8_t>(counter_ % 128);
        if (save_type_ == 0x01 && missing != 128) {
            const std::vector<std::uint8_t> zeros(missing, 0);
            std::size_t put = 0;
            sd->write(file_.path, file_.pos, zeros, put);
            file_.pos += put;
        }
    }
    file_.valid = false;
    set_status(kCloseOk);
}

void NanoSd::get_parameter(const CardBus& bus) {
    const NanoSdMedia& m = media(bus);
    switch (number_) {
    case 0:
        push(1);
        push(m.skip_menu ? 0 : m.menu);
        break;
    case 1:
        push(1);
        push(m.sort);
        break;
    case 2:
        push(1);
        push(sd_ready_ ? 1 : 0);
        break;
    case 3:
        push(3);
        for (const char c : kNanoId) {
            push(static_cast<std::uint8_t>(c));
        }
        push(0);
        break;
    default:
        set_status(kInvalidParameter);
        return;
    }
    set_status(kOk);
}

void NanoSd::card_changed(const CardBus& bus) {
    pcint_on_ = true;  // a change during the debounce is cleared with PCIFR, the pin is read now
    FatVolume* sd = media(bus).sd;
    if (!sd) {
        set_status(kCardNotReady);
        sd_ready_ = false;
        dir_file_.valid = false;
        if (file_open_) {
            file_.valid = false;
        }
    } else if (!sd_ready_) {
        if (sd_begin(sd)) {
            dir_file_ = sd_open(sd, cur_dir_);
            if (!dir_file_.valid) {
                dir_file_ = sd_open(sd, "/");
                cur_dir_ = "/";
            }
            if (dir_file_.valid) {
                dir_file_.next = 0;
                sd_ready_ = true;
                set_status(kNotReadyYet);
            }
        }
    }
}

void NanoSd::save(ChunkWriter& w) const {
    w.u8(kSaveVersion);
    w.u8(static_cast<std::uint8_t>(bank_));
    w.u8(static_cast<std::uint8_t>(bank_store_));
    w.boolean(booted_);
    w.u8(latch_);
    w.u8(status_);
    w.boolean(hung_);
    save_queue(w, in_);
    save_queue(w, out_);
    w.boolean(pin_);
    w.boolean(pcint_on_);
    w.boolean(sd_ready_);
    w.u64(debounce_until_);
    w.u64(pin_changed_);
    w.boolean(in_command_);
    w.u8(cmd_);
    w.u8(static_cast<std::uint8_t>(phase_));
    w.text(arg_);
    w.u8(number_);
    w.u8(static_cast<std::uint8_t>(data_.size()));
    w.bytes(data_);
    w.text(cur_dir_);
    save_file(w, dir_file_);
    save_file(w, file_);
    save_file(w, list_dir_);
    w.boolean(file_open_);
    w.u32(static_cast<std::uint32_t>(counter_));
    w.u8(save_type_);
}

void NanoSd::load(ChunkReader& r) {
    if (r.u8() != kSaveVersion) {
        r.fail();
        return;
    }
    const unsigned bank = r.u8();
    bank_store_ = r.u8() & 1;
    select_bank(bank);
    booted_ = r.boolean();
    latch_ = r.u8();
    status_ = r.u8();
    hung_ = r.boolean();
    in_ = load_queue(r);
    out_ = load_queue(r);
    pin_ = r.boolean();
    pcint_on_ = r.boolean();
    sd_ready_ = r.boolean();
    debounce_until_ = r.u64();
    pin_changed_ = r.u64();
    in_command_ = r.boolean();
    cmd_ = r.u8();
    phase_ = r.u8();
    arg_ = r.text();
    number_ = r.u8();
    data_.resize(r.u8());
    r.bytes(data_);
    cur_dir_ = r.text();
    dir_file_ = load_file(r);
    file_ = load_file(r);
    list_dir_ = load_file(r);
    file_open_ = r.boolean();
    counter_ = static_cast<std::int32_t>(r.u32());
    save_type_ = r.u8();
}

}  // namespace freetvc
