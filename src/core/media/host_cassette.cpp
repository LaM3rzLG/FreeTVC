// SPDX-License-Identifier: GPL-3.0-or-later
#include "core/media/host_cassette.hpp"

#include <algorithm>

#include "core/machine/machine.hpp"
#include "core/message.hpp"

namespace freetvc {

namespace {

// Cassette workspace shared by EXT 1.x and 2.x (hardware/rom.md §12); U0 RAM
constexpr std::uint16_t kBuffer = 0x0B6B;
constexpr std::uint16_t kProtect = 0x0B6D;
constexpr std::uint16_t kEof = 0x0B6E;
constexpr std::uint16_t kMuddle = 0x0B6F;
constexpr std::uint16_t kReadType = 0x0BF3;  // 0 = no read file, else its type byte (01h / 11h)
constexpr std::uint16_t kReadName = 0x0BF4;
constexpr std::uint16_t kWriteType = 0x0D14;
constexpr std::uint16_t kWriteName = 0x0D15;

constexpr std::uint8_t kErrProtect = 0xE6;
constexpr std::uint8_t kErrInternal = 0xE7;
constexpr std::uint8_t kErrVerify = 0xE8;
constexpr std::uint8_t kErrNotOpen = 0xE9;
constexpr std::uint8_t kErrOpen = 0xEB;
constexpr std::uint8_t kErrEof = 0xEC;

// [NANOSD] the card serves a file that is not .CAS byte for byte (the 2dfx Wolf3D reads its raw .bin data this way).
// A .CAS file keeps its header even with an odd type byte (seen in the wild). `found` has no extension for a .CAS match.
bool has_cas_header(std::string_view found) {
    const std::size_t dot = found.rfind('.');
    if (dot != std::string_view::npos) {
        std::string ext(found.substr(dot));
        for (char& c : ext) {
            c = static_cast<char>(c >= 'a' && c <= 'z' ? c - 32 : c);
        }
        if (ext != ".CAS") {
            return false;
        }
    }
    return true;
}

// The OS runs every function with U0, U1, U2 and SYS paged in (port 02h = 70h, rom.md §4)
class OsPaging {
public:
    explicit OsPaging(Machine& m) : m_(m), saved_(m.memory().page_register()) { m.io_write(0x02, 0x70); }
    ~OsPaging() { m_.io_write(0x02, saved_); }
    OsPaging(const OsPaging&) = delete;
    OsPaging& operator=(const OsPaging&) = delete;

private:
    Machine& m_;
    std::uint8_t saved_;
};

// The flags of the OS exit path's OR A (C3EFh)
std::uint8_t or_flags(std::uint8_t a) {
    std::uint8_t f = a & 0xA8;  // S, 5, 3
    if (a == 0) {
        f |= 0x40;
    }
    unsigned bits = a;
    bits ^= bits >> 4;
    bits ^= bits >> 2;
    bits ^= bits >> 1;
    if (!(bits & 1)) {
        f |= 0x04;
    }
    return f;
}

std::string read_name(const Machine& m, std::uint16_t at) {
    std::string name;
    const std::uint8_t length = m.peek(at);
    for (unsigned i = 1; i <= length; ++i) {
        name.push_back(static_cast<char>(m.peek(static_cast<std::uint16_t>(at + i))));
    }
    return tvc_file_name(name);
}

void store_name(std::span<std::uint8_t> ram, std::uint16_t at, std::string_view name) {
    ram[at] = static_cast<std::uint8_t>(name.size());
    std::copy(name.begin(), name.end(), ram.begin() + at + 1);
}

// The OS's error exit (SYS 2.2 C3F2h → EXT 2.x E1F4h, EXT 1.x F1E6h): outside the IRQ service the calling class gets
// its default device back, unless a card holds it; the defaults are EXT E259h/E261h (1.x: FB0Eh/FB16h), equal in both.
void reset_assignment(std::span<std::uint8_t> ram, std::uint8_t code) {
    static constexpr std::uint8_t kDefaultIn[8] = {0xFF, 0x01, 0x02, 0xFF, 0xFF, 0x05, 0x06, 0xFF};
    static constexpr std::uint8_t kDefaultOut[8] = {0x00, 0xFF, 0x02, 0xFF, 0x04, 0x05, 0x06, 0xFF};
    const unsigned cls = (code >> 4) & 7;
    if (ram[0x0B20] == 0xFF || cls == 7) {
        return;  // INT_FLAG set: called from the IRQ service
    }
    const bool in = (code & 0x80) != 0;
    const std::uint16_t table = in ? 0x0B00 : 0x0B08;
    const std::uint8_t device = ram[table + cls];
    if (device == 0x06) {
        ram[table + 7] = ram[0x0B1C];  // EXT E22Ch
    }
    if (!(device & 0x80)) {
        ram[table + cls] = in ? kDefaultIn[cls] : kDefaultOut[cls];
    }
}

void close_workspace(std::span<std::uint8_t> ram, std::uint16_t type) {
    ram[type] = 0;
    ram[kMuddle] = 0;
    ram[kMuddle + 1] = 0;
}

}  // namespace

void HostCassette::attach(std::string name, CasFile file) {
    attached_name_ = tvc_file_name(name);
    attached_ = std::move(file);
}

void HostCassette::detach() {
    attached_.reset();
    attached_name_.clear();
}

std::vector<std::string> HostCassette::take_messages() {
    std::vector<std::string> out;
    out.swap(messages_);
    return out;
}

bool HostCassette::call(Machine& m, std::uint8_t code) {
    const unsigned function = code & 0x0F;
    const bool in = (code & 0x80) != 0;
    if (!active() || function == 0 || function > 5 || (!in && function == 5)) {
        return false;
    }
    const std::span<std::uint8_t> ram = m.ram();
    if (ram[kReadType] == 0) {
        read_.reset();  // closed by a reset or by the ROM
    }
    if (ram[kWriteType] == 0) {
        write_.reset();
    }
    // A file the ROM opened from a real tape stays the ROM's
    if (function != 3 && ((in && ram[kReadType] != 0 && !read_) || (!in && ram[kWriteType] != 0 && !write_))) {
        return false;
    }

    Z80Registers r = m.cpu().registers();
    const std::uint16_t ret = static_cast<std::uint16_t>(m.peek(r.sp) | m.peek(static_cast<std::uint16_t>(r.sp + 1)) << 8);
    std::uint16_t de = r.de;
    std::uint16_t bc = r.bc;
    std::uint8_t a = 0;
    {
        OsPaging paging(m);
        // A class assigned to the cassette (functions 1-2) reaches the cassette's own function
        switch (static_cast<std::uint8_t>((code & 0x8F) | 0x50)) {
        case 0xD1:  // CAS CHIN
            if (!read_) {
                a = kErrNotOpen;
            } else if (read_->file.buffered) {
                const auto& s = read_->file.stream;
                if (read_->pos >= s.size()) {
                    a = kErrEof;
                } else {
                    bc = static_cast<std::uint16_t>((bc & 0xFF00) | s[read_->pos++]);
                    if (read_->pos == s.size()) {
                        ram[kEof] = 0xFF;
                    }
                }
            } else if (read_->pos >= read_->file.header().size()) {
                a = kErrInternal;  // more than the header's 16 bytes
            } else {
                bc = static_cast<std::uint16_t>((bc & 0xFF00) | read_->file.header()[read_->pos++]);
            }
            break;
        case 0xD2:  // CAS BKIN: DE = destination, BC = count; returns BC = bytes not read
            if (!read_) {
                a = kErrNotOpen;
            } else if (read_->file.buffered) {
                const auto& s = read_->file.stream;
                for (; bc != 0 && read_->pos < s.size(); --bc) {
                    m.poke(de++, s[read_->pos++]);
                }
                if (read_->pos == s.size()) {
                    ram[kEof] = 0xFF;
                }
                a = bc != 0 ? kErrEof : 0;
            } else if (read_->block_done) {
                a = kErrEof;
            } else {
                const std::span<const std::uint8_t> block = read_->file.block();
                const std::size_t n = std::min<std::size_t>(bc, block.size());
                for (std::size_t i = 0; i < n; ++i) {
                    m.poke(de++, block[i]);
                }
                bc = static_cast<std::uint16_t>(bc - n);
                a = bc != 0 ? kErrInternal : 0;  // asked for more than the block holds
                blocks_read_ += a == 0 ? 1 : 0;
                read_->block_done = true;
                ram[kEof] = 0xFF;
            }
            break;
        case 0xD3:  // CAS OPEN: DE = name; returns DE = the found file's name
            a = open_read(m, de).value_or(0);
            break;
        case 0xD4:  // CAS CLOSE (read)
            read_.reset();
            close_workspace(ram, kReadType);
            break;
        case 0xD5:  // CAS VERIFY: DE = memory, BC = length
            if (!read_) {
                a = kErrNotOpen;
            } else if (read_->file.buffered) {
                const auto& s = read_->file.stream;
                for (; bc != 0 && read_->pos < s.size(); --bc, ++de) {
                    if (m.peek(de) != s[read_->pos++]) {
                        a = kErrVerify;  // DE and BC point at the differing byte
                        break;
                    }
                }
                if (a == 0 && bc != 0) {
                    a = kErrEof;
                }
            } else if (read_->block_done) {
                a = kErrEof;
            } else {
                const std::span<const std::uint8_t> block = read_->file.block();
                if (bc != block.size()) {
                    a = kErrVerify;
                } else {
                    for (std::size_t i = 0; i < block.size(); ++i) {
                        if (m.peek(static_cast<std::uint16_t>(de + i)) != block[i]) {
                            a = kErrVerify;
                            break;
                        }
                    }
                }
                read_->block_done = true;
                ram[kEof] = 0xFF;
            }
            break;
        case 0x51:  // CAS CHOUT: C = character
            if (!write_) {
                a = kErrNotOpen;
            } else if (write_->buffered) {
                write_->stream.push_back(static_cast<std::uint8_t>(bc));
            } else if (write_->block_done) {
                a = kErrEof;
            } else if (write_->header_bytes >= kCasProgramHeaderSize) {
                a = kErrInternal;  // an unbuffered header holds 16 bytes
            } else {
                write_->stream.push_back(static_cast<std::uint8_t>(bc));
                ++write_->header_bytes;
            }
            break;
        case 0x52:  // CAS BKOUT: DE = source, BC = count
            if (!write_) {
                a = kErrNotOpen;
            } else if (!write_->buffered && write_->block_done) {
                a = kErrEof;
            } else {
                if (!write_->buffered) {
                    write_->stream.resize(kCasProgramHeaderSize, 0);
                }
                for (; bc != 0; --bc) {
                    write_->stream.push_back(m.peek(de++));
                }
                if (!write_->buffered) {
                    write_->block_done = true;
                    a = finish_write(m);  // an unbuffered block write also closes the file (EXT E6FCh)
                }
            }
            break;
        case 0x53:  // CAS CRTE: DE = name; returns DE = the converted name
            a = create(m, de).value_or(0);
            break;
        case 0x54:  // CAS CLOSE (write); closing a closed file is no error
            if (write_) {
                a = finish_write(m);
            } else {
                close_workspace(ram, kWriteType);
            }
            break;
        default:
            break;
        }
    }

    if (a != 0) {
        reset_assignment(ram, code);
    }
    r.af = static_cast<std::uint16_t>(a << 8 | or_flags(a));
    r.bc = bc;
    r.de = de;
    r.sp = static_cast<std::uint16_t>(r.sp + 2);
    m.cpu().set_registers(r);
    m.return_to(static_cast<std::uint16_t>(ret + 1));
    return true;
}

std::optional<std::uint8_t> HostCassette::open_read(Machine& m, std::uint16_t& de) {
    const std::span<std::uint8_t> ram = m.ram();
    if (ram[kReadType] != 0) {
        return kErrOpen;
    }
    const std::string name = read_name(m, de);
    std::optional<CasFile> file;
    std::string found;
    if (attached_ && (name.empty() || name == attached_name_)) {
        file = attached_;
        found = attached_name_;
    } else if (folder_) {
        if (const auto bytes = folder_->read(name, found); bytes && !has_cas_header(found)) {
            file = CasFile{};
            file->buffered = true;
            file->stream = *bytes;
        } else if (bytes) {
            std::string error;
            file = parse_cas(*bytes, error);
            if (!file) {
                messages_.push_back(msg("OPEN \"%s\": %s", name.c_str(), error.c_str()));
                return kErrInternal;
            }
            for (const std::string& w : file->warnings) {
                messages_.push_back(msg("OPEN \"%s\": %s", name.c_str(), w.c_str()));
            }
        }
    }
    if (!file && attached_) {
        file = attached_;  // a one-program cassette answers any name
        found = attached_name_;
    }
    if (!file) {
        messages_.push_back(msg("OPEN \"%s\": no such file", name.c_str()));
        return kErrEof;
    }
    found = tvc_file_name(found);
    messages_.push_back(msg("OPEN \"%s\": %s", name.c_str(), (found.empty() ? msg("(no name)") : found).c_str()));
    ram[kReadType] = file->buffered ? 0x01 : 0x11;
    ram[kBuffer] = static_cast<std::uint8_t>(ram[kReadType] - 0x11);  // EXT E4B8h: 0 unbuffered, F0h buffered
    ram[kEof] = 0;
    store_name(ram, kReadName, found);
    read_ = ReadFile{std::move(*file)};
    de = kReadName;
    return std::nullopt;
}

std::optional<std::uint8_t> HostCassette::create(Machine& m, std::uint16_t& de) {
    const std::span<std::uint8_t> ram = m.ram();
    if (ram[kWriteType] != 0) {
        return kErrOpen;
    }
    const std::string name = read_name(m, de);
    if (!folder_ || !folder_->writable()) {
        messages_.push_back(msg("SAVE \"%s\": no cassette folder to write to (--cas-dir)", name.c_str()));
        return kErrProtect;
    }
    WriteFile file;
    file.name = name;
    file.buffered = ram[kBuffer] != 0;
    file.protect = ram[kProtect] != 0;
    ram[kProtect] = 0;
    ram[kWriteType] = file.buffered ? 0x01 : 0x11;
    store_name(ram, kWriteName, name);
    write_ = std::move(file);
    de = kWriteName;
    return std::nullopt;
}

std::uint8_t HostCassette::finish_write(Machine& m) {
    WriteFile& file = *write_;
    if (!file.buffered) {
        file.stream.resize(std::max(file.stream.size(), kCasProgramHeaderSize), 0);
    }
    const std::vector<std::uint8_t> cas = build_cas(file.buffered, file.protect, file.stream);
    std::string error;
    std::uint8_t a = 0;
    if (folder_ && folder_->write(file.name, cas, error)) {
        messages_.push_back(msg("SAVE \"%s\": %zu bytes", file.name.c_str(), cas.size()));
    } else {
        messages_.push_back(msg("SAVE \"%s\": %s", file.name.c_str(), error.c_str()));
        a = kErrProtect;
    }
    write_.reset();
    close_workspace(m.ram(), kWriteType);
    return a;
}

}  // namespace freetvc
