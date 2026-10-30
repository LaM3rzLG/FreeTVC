// SPDX-License-Identifier: GPL-3.0-or-later
// TVC256k++ storage and mouse without ROMs: the D6h commands on card RAM, the four drives, the C0h file functions,
// function 15, the mouse registers, and an open file across a snapshot. The pendrive is a FAT image here.
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "core/cards/card256.hpp"
#include "core/cards/card256_storage.hpp"
#include "core/machine/machine.hpp"
#include "core/snapshot/snapshot.hpp"

namespace {

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

using Bytes = std::vector<std::uint8_t>;

constexpr std::uint8_t kIndex = 0x10;  // slot 0
constexpr std::uint8_t kData = 0x11;
constexpr std::uint8_t kAllRam = 0xB0;

freetvc::MachineConfig config() {
    freetvc::MachineConfig c;
    c.model = freetvc::Model::Tvc64kPlus;
    c.roms.sys = {0xF3, 0x76};
    c.cards[0] = {freetvc::CardKind::Card256, Bytes(freetvc::Card256::kRomSize, 0xFF)};
    return c;
}

freetvc::Card256& card(freetvc::Machine& m) {
    return const_cast<freetvc::Card256&>(dynamic_cast<const freetvc::Card256&>(*m.cards().card(0)));
}

void set(freetvc::Machine& m, std::uint8_t index, std::uint8_t value) {
    m.io_write(kIndex, index);
    m.io_write(kData, value);
}

std::uint8_t get(freetvc::Machine& m, std::uint8_t index) {
    m.io_write(kIndex, index);
    return m.io_read(kData);
}

// Card RAM by its offset from C000h
void put(freetvc::Machine& m, unsigned offset, const Bytes& bytes) {
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        card(m).mem_write(static_cast<std::uint16_t>(offset + i), bytes[i]);
    }
}

void put_name(freetvc::Machine& m, unsigned offset, const std::string& s) {
    put(m, offset, Bytes{static_cast<std::uint8_t>(s.size())});
    put(m, offset + 1, Bytes(s.begin(), s.end()));
}

void put32(freetvc::Machine& m, unsigned offset, std::uint32_t v) {
    put(m, offset, {static_cast<std::uint8_t>(v), static_cast<std::uint8_t>(v >> 8), static_cast<std::uint8_t>(v >> 16),
                    static_cast<std::uint8_t>(v >> 24)});
}

std::uint8_t peek(freetvc::Machine& m, unsigned offset) { return card(m).mem_read(static_cast<std::uint16_t>(offset)); }

std::uint32_t peek32(freetvc::Machine& m, unsigned offset) {
    return peek(m, offset) | peek(m, offset + 1) << 8 | peek(m, offset + 2) << 16 | static_cast<std::uint32_t>(peek(m, offset + 3)) << 24;
}

std::string peek_string(freetvc::Machine& m, unsigned offset, unsigned n) {
    std::string s;
    for (unsigned i = 0; i < n; ++i) {
        s += static_cast<char>(peek(m, offset + i));
    }
    return s;
}

std::uint8_t command(freetvc::Machine& m, std::uint8_t code) {
    set(m, 0xD6, code);
    return get(m, 0xD6);
}

// D900h: mode, CAS type, name ([FW] tvc_fatfs_open_file); the handle comes back at DA00h
std::uint8_t open(freetvc::Machine& m, std::uint8_t mode, const std::string& name) {
    put(m, 0x1900, {mode, 0x11});
    put_name(m, 0x1902, name);
    return command(m, 0x01);
}

std::uint8_t read(freetvc::Machine& m, std::uint32_t handle, std::uint16_t n) {
    put32(m, 0x1900, handle);
    put(m, 0x1904, {static_cast<std::uint8_t>(n), static_cast<std::uint8_t>(n >> 8)});
    return command(m, 0x03);
}

std::uint8_t named(freetvc::Machine& m, std::uint8_t code, const std::string& name) {
    put_name(m, 0x1900, name);
    return command(m, code);
}

Bytes cas(const Bytes& payload, std::uint8_t type = 0x11) {
    Bytes b(128, 0);
    b[0] = type;
    b.insert(b.end(), payload.begin(), payload.end());
    return b;
}

void store(freetvc::FatVolume& v, const std::string& path, const Bytes& bytes) {
    std::size_t put_bytes = 0;
    check(v.create(path) == 0 && v.write(path, 0, bytes, put_bytes) == 0 && put_bytes == bytes.size(), "stored " + path);
}

Bytes load(freetvc::FatVolume& v, const std::string& path) {
    freetvc::FatEntry e;
    if (v.stat(path, e) != 0) {
        return {};
    }
    Bytes b(static_cast<std::size_t>(e.size));
    std::size_t got = 0;
    v.read(path, 0, b, got);
    return b;
}

const Bytes kHello = {'H', 'E', 'L', 'L', 'O', ' ', 'T', 'V', 'C', '!'};

struct Rig {
    Bytes usb = freetvc::fat_image_format(1024 * 1024);
    std::unique_ptr<freetvc::FatVolume> pendrive = freetvc::fat_image_volume(usb);
    freetvc::Card256Media media;
    freetvc::Machine m{config()};

    Rig() {
        store(*pendrive, "/HELLO.CAS", cas(kHello));
        media.pendrive = pendrive.get();
        media.usb = freetvc::Card256Usb::Pendrive;
        m.set_card256_media(&media);
        m.io_write(0x02, kAllRam);
        set(m, 0xC5, 0);
    }
};

void test_pendrive() {
    Rig r;
    freetvc::Machine& m = r.m;
    check(r.pendrive != nullptr, "the FAT image mounts");
    check(get(m, 0xD0) == 0x02, "D0h: a pendrive");
    // A bare LOAD: the first .cas file
    check(open(m, 0x01, "") == 0, "open the first .cas file");
    const std::uint32_t h = peek32(m, 0x1A00);
    check(h != 0 && peek32(m, 0x1A04) == kHello.size(), "a handle and the size without the CAS header");
    check(peek(m, 0x1A08) == 9 && peek_string(m, 0x1A09, 9) == "HELLO.CAS", "the name found");
    check(read(m, h, 4) == 0 && peek(m, 0x1A00) == 4 && peek_string(m, 0x1A02, 4) == "HELL", "read 4 bytes after the header");
    check(read(m, h, 100) == 0 && peek(m, 0x1A00) == 6, "the rest");
    check(read(m, h, 100) == 0 && peek(m, 0x1A00) == 0, "at the end: OK with 0 bytes");
    check(read(m, h, 0) == 0x93, "reading 0 bytes: invalid parameter");
    put32(m, 0x1900, h);
    check(command(m, 0x02) == 0, "close");
    check(read(m, h, 1) == 0x89, "a closed handle: invalid object");
    check(read(m, 0x12345678, 1) == 0x94, "no such handle: 20");
    // A name without the extension, and a prefix
    check(open(m, 0x01, "hello") == 0 && peek_string(m, 0x1A09, 10) == std::string("hello.cas") + '\0', ".cas added");
    put32(m, 0x1900, 0);
    check(command(m, 0x02) == 0, "handle 0 closes every file");
    check(open(m, 0x01, "HEL*") == 0 && peek_string(m, 0x1A09, 9) == "hello.cas", "a prefix finds the file, in lower case");
    check(open(m, 0x01, "nothing") == 0x84, "a missing file: no file");
    put32(m, 0x1900, 0);
    command(m, 0x02);
    // SAVE "NEW": a new .cas with the header
    check(open(m, 0x06, "NEW") == 0, "create NEW");
    const std::uint32_t w = peek32(m, 0x1A00);
    put32(m, 0x1900, w);
    put(m, 0x1904, {3, 0, 'a', 'b', 'c'});
    check(command(m, 0x04) == 0 && peek(m, 0x1A00) == 3, "write 3 bytes");
    put32(m, 0x1900, w);
    check(command(m, 0x02) == 0, "close the new file");
    const Bytes saved = load(*r.pendrive, "/NEW.cas");
    check(saved == cas({'a', 'b', 'c'}), "NEW.cas holds the header and the bytes");
    check(open(m, 0x06, "NEW") == 0x88, "creating it again: exists");
    put(m, 0x1900, {0x02, 0x22, 3, 'B', 'A', 'D'});
    check(command(m, 0x01) == 0x93, "a CAS type other than 01h/11h: invalid parameter");
    // Directories
    check(named(m, 0x0D, "SUB") == 0, "mkdir");
    check(named(m, 0x0D, "SUB") == 0x88, "mkdir again: exists");
    check(named(m, 0x0C, "sub") == 0, "chdir, any case");
    check(command(m, 0x0B) == 0 && peek(m, 0x1A00) == 4 && peek_string(m, 0x1A01, 6) == "/SUBUB", "getcwd, the old tail left behind");
    check(named(m, 0x0C, "..") == 0 && command(m, 0x0B) == 0 && peek(m, 0x1A00) == 1, "back to the root");
    check(named(m, 0x0C, "s*") == 0 && peek_string(m, 0x1901, 4) == std::string("sub") + '\0', "chdir by a prefix");
    check(named(m, 0x0C, "/") == 0, "chdir to the root");
    check(named(m, 0x0C, "HELLO.CAS") == 0x85, "chdir to a file: no path");
    check(named(m, 0x05, ".") == 0, "opendir");
    const std::uint32_t d = peek32(m, 0x1A00);
    std::vector<std::string> names;
    for (int i = 0; i < 5; ++i) {
        put32(m, 0x1900, d);
        if (command(m, 0x07) != 0 || peek(m, 0x1A00) == 0) {
            break;
        }
        names.push_back(peek_string(m, 0x1A01, peek(m, 0x1A00)));
    }
    check(names == std::vector<std::string>{"HELLO.CAS", "NEW.cas", "SUB"}, "the root in directory order");
    put32(m, 0x1900, d);
    check(command(m, 0x06) == 0, "closedir");
    check(named(m, 0x10, "NEW.CAS") == 0 && peek(m, 0x1B00) == 0x20 && peek32(m, 0x1B01) == 131, "stat");
    put_name(m, 0x1900, "NEW.cas");
    put_name(m, 0x1900 + 8, "OLD.cas");
    check(command(m, 0x0F) == 0 && load(*r.pendrive, "/OLD.cas").size() == 131, "rename");
    check(named(m, 0x0E, "OLD.cas") == 0 && load(*r.pendrive, "/OLD.cas").empty(), "delete");
    check(command(m, 0x40) == 0xFF && command(m, 0x00) == 0xFF, "unknown commands: FFh");
    r.media.usb = freetvc::Card256Usb::None;
    check(get(m, 0xD0) == 0 && open(m, 0x01, "X") == 0x8C, "no pendrive: no volume (8Ch)");
    check(open(m, 0x01, "") == 0x84, "no pendrive, a bare LOAD: no file (the directory scan fails first)");
}

void test_flash() {
    Rig r;
    freetvc::Machine& m = r.m;
    auto flash = std::make_shared<Bytes>(freetvc::card256_flash_image(
        {{"launcher.cas", cas({1, 2, 3})}, {"games/pac.cas", cas({4, 5})}, {"readme.txt", {'h', 'i'}}}, 256 * 1024));
    check(!flash->empty(), "a flash image is built");
    r.media.flash = flash;
    r.media.flash_valid = freetvc::card256_flash_valid(*flash);
    check(r.media.flash_valid && get(m, 0xC9) == 1, "C9h: the flash drive mounts");
    set(m, 0xC5, 2);
    check(open(m, 0x01, "") == 0 && peek_string(m, 0x1A09, 12) == "launcher.cas" && peek32(m, 0x1A04) == 3, "the first .cas");
    const std::uint32_t h = peek32(m, 0x1A00);
    check(read(m, h, 10) == 0 && peek(m, 0x1A00) == 3 && peek(m, 0x1A04) == 3, "read it");
    check(read(m, h, 10) == 0xA9, "at the end: 41 (littlefs layer)");
    check(command(m, 0x04) == 0x8A, "write: write protected");
    check(named(m, 0x0D, "x") == 0xA8 && named(m, 0x0E, "x") == 0xA8, "mkdir and delete: read-only (40)");
    check(open(m, 0x06, "NEW") == 0xA8, "an open without read: read-only (40)");
    check(open(m, 0x01, "LAU*") == 0 && peek_string(m, 0x1A09, 12) == "launcher.cas", "a prefix, lower-cased");
    check(named(m, 0x0C, "games/") == 0x86, "chdir with a trailing slash: invalid name ([FW] quirk)");
    check(named(m, 0x0C, "GAMES") == 0 && command(m, 0x0B) == 0 && peek_string(m, 0x1A01, 7) == "/games/", "chdir, getcwd");
    check(open(m, 0x01, "pac") == 0 && peek_string(m, 0x1A09, 7) == "pac.cas", ".cas added");
    check(open(m, 0x01, "/readme.txt") == 0 && peek32(m, 0x1A04) == 2, "an absolute path, not a CAS file");
    check(open(m, 0x01, "/x") == 0x92, "a fifth open: too many files (4 handles)");
    check(named(m, 0x05, "./") == 0, "opendir ./");
    const std::uint32_t d = peek32(m, 0x1A00);
    put32(m, 0x1900, d);
    check(command(m, 0x07) == 0 && peek_string(m, 0x1A01, 7) == "pac.cas" && peek(m, 0x1B00) == 0x01, "readdir skips . and ..");
    put32(m, 0x1900, d);
    check(command(m, 0x07) == 0 && peek(m, 0x1A00) == 0, "the end");
    r.media.flash_valid = false;
    check(get(m, 0xC9) == 0 && command(m, 0x0B) == 0x83, "no flash image: not ready");
}

void test_psram_drive() {
    Rig r;
    freetvc::Machine& m = r.m;
    store(*r.pendrive, "/TWO.CAS", cas({9, 8, 7}));
    // Function 15: the root's two files into a new PSRAM drive
    set(m, 0xC1, 0x00);
    m.poke(0x8000, 0);
    m.poke(0x8001, 0);
    set(m, 0xC0, 15);
    check(get(m, 0xC2) == 0 && m.memory().read(0x8000) == 2, "function 15 copies two files");
    check(get(m, 0xC8) == 1 && get(m, 0xC5) == 1 && card(m).psram_drive(), "the PSRAM drive is valid and selected");
    check(open(m, 0x01, "two") == 0 && peek32(m, 0x1A04) == 3, "open a copied file, names in lower case");
    check(read(m, peek32(m, 0x1A00), 3) == 0 && peek(m, 0x1A02) == 9, "read it from PSRAM");
    check(open(m, 0x06, "SAVED") == 0xA8, "SAVE to the PSRAM drive fails: opened read-only ([FW] quirk)");
    check(named(m, 0x0D, "dir") == 0 && named(m, 0x0C, "dir") == 0, "mkdir and chdir on the PSRAM drive");
    check(named(m, 0x0F, "x") == 0xA8, "rename refused (40)");
    // Function 36 wipes it
    set(m, 0xC0, 36);
    check(get(m, 0xC2) == 0 && get(m, 0xC8) == 0 && card(m).psram_read(0x7FFFFF) == 0xFF, "function 36 deletes the drive");
    check(open(m, 0x01, "two") == 0x83, "no PSRAM drive: not ready");
}

void test_dsk() {
    Rig r;
    freetvc::Machine& m = r.m;
    Bytes dsk = freetvc::fat_image_format(360 * 1024);
    {
        auto v = freetvc::fat_image_volume(dsk);
        check(v != nullptr, "a 360 KiB FAT image");
        if (v) {
            store(*v, "/GAME.CAS", cas({0x55, 0x66}));
        }
    }
    store(*r.pendrive, "/DISK.IMG", dsk);
    check(named(m, 0x12, "disk.img") == 0 && get(m, 0xCA) == 1 && card(m).dsk_kind() == 1, "mount a DSK on the pendrive");
    check(named(m, 0x12, "disk.img") == 0x83, "a second mount: not ready");
    set(m, 0xC5, 3);
    check(open(m, 0x01, "GAME") == 0 && read(m, peek32(m, 0x1A00), 2) == 0 && peek(m, 0x1A03) == 0x66, "read a file in the DSK");
    check(command(m, 0x0B) == 0 && peek(m, 0x1A00) == 1 && peek(m, 0x1A01) == '/', "the DSK's working directory");
    check(open(m, 0x06, "OUT") == 0, "SAVE into the DSK");
    put32(m, 0x1900, peek32(m, 0x1A00));
    put(m, 0x1904, {1, 0, 0x77});
    check(command(m, 0x04) == 0, "write into the DSK");
    put32(m, 0x1900, 0);
    command(m, 0x02);
    check(command(m, 0x13) == 0 && get(m, 0xCA) == 0 && get(m, 0xC5) == 0, "unmount: back to the pendrive");
    const Bytes back = load(*r.pendrive, "/DISK.IMG");
    auto v = freetvc::fat_image_volume(const_cast<Bytes&>(back));
    check(v && load(*v, "/OUT.cas") == cas({0x77}), "the write reached the image on the pendrive");
    check(command(m, 0x13) == 0x80, "unmount with nothing mounted: 80h");
    set(m, 0xC5, 3);
    check(command(m, 0x12) == 0x80, "mount with the DSK drive selected: 80h");
}

void test_file_functions() {
    Rig r;
    freetvc::Machine& m = r.m;
    set(m, 0xC1, 0x00);
    const std::string name = "HELLO.CAS";
    Bytes block = {0x01, 0x11, static_cast<std::uint8_t>(name.size())};
    block.insert(block.end(), name.begin(), name.end());
    for (std::size_t i = 0; i < block.size(); ++i) {
        m.poke(static_cast<std::uint16_t>(0x8000 + i), block[i]);
    }
    set(m, 0xC0, 0x81);
    check(get(m, 0xC2) == 0 && m.memory().read(0x8004) == kHello.size() && m.memory().read(0x8008) == 9, "81h opens");
    const std::uint8_t h0 = m.memory().read(0x8000);
    m.poke(0x8004, 3);
    m.poke(0x8005, 0);
    set(m, 0xC0, 0x83);
    check(get(m, 0xC2) == 0 && m.memory().read(0x8004) == 3 && m.memory().read(0x8006) == 'H' && m.memory().read(0x8008) == 'L',
          "83h reads into the block");
    check(m.memory().read(0x8000) == h0, "the handle stays at the block's start");
    set(m, 0xC0, 0x8B);
    check(get(m, 0xC2) == 0 && m.memory().read(0x8000) == 1 && m.memory().read(0x8001) == '/', "8Bh getcwd");
    m.poke(0x8000, 0);
    set(m, 0xC0, 0x92);
    check(get(m, 0xC2) == 0x13, "92h with an empty name: invalid parameter");
}

void test_mouse() {
    freetvc::Machine m(config());
    card(m).set_usb(freetvc::Card256::Usb::Mouse);
    check(get(m, 0xD0) == 0x01 && get(m, 0xD5) == 0x10, "D0h: a mouse; speed 10h (0.5)");
    card(m).mouse_report(10, -5, 0x01);
    check(get(m, 0xD1) == 0x01, "left button");
    check(get(m, 0xD2) == 5 && get(m, 0xD2) == 0, "dX 10 x 0.5, cleared by the read");
    check(get(m, 0xD3) == 0xFD, "dY -5 x 0.5 rounds to -3");
    card(m).mouse_report(4, 0, 0x06);
    check(get(m, 0xD1) == 0x06 && get(m, 0xD2) == 2, "right and middle buttons; a new sum after the read");
    card(m).mouse_report(1, 0, 0);
    card(m).mouse_report(1, 0, 0);
    check(get(m, 0xD2) == 1, "unread reports add up: (1 + 1) x 0.5");
    set(m, 0xD5, 0x7F);
    card(m).mouse_report(127, -127, 0);
    check(get(m, 0xD2) == 0xFF && get(m, 0xD3) == 0x02, "clamped to 255 and -254");
    check(get(m, 0xD4) == 0, "no wheel");
    card(m).set_usb(freetvc::Card256::Usb::None);
    card(m).mouse_report(10, 10, 1);
    check(get(m, 0xD0) == 0 && get(m, 0xD1) == 0 && get(m, 0xD2) == 0, "unplugged: no reports");
}

void test_snapshot() {
    Rig r;
    freetvc::Machine& m = r.m;
    check(open(m, 0x01, "") == 0, "open");
    const std::uint32_t h = peek32(m, 0x1A00);
    check(read(m, h, 2) == 0, "read 2 bytes");
    freetvc::Snapshot snap;
    m.save_state(snap);
    freetvc::Machine b(config());
    std::string error;
    check(b.load_state(snap, error), "snapshot loads: " + error);
    b.set_card256_media(&r.media);
    check(get(b, 0xD0) == 0x02 && read(b, h, 2) == 0 && peek_string(b, 0x1A02, 2) == "LL", "the file reads on after the load");
    freetvc::Machine c = m;
    c.set_card256_media(&r.media);
    check(read(c, h, 2) == 0 && peek_string(c, 0x1A02, 2) == "LL", "and in a copy of the machine");
    check(read(m, h, 2) == 0 && peek_string(m, 0x1A02, 2) == "LL", "the original keeps its own place");
}

void test_short_names() {
    check(freetvc::fat_short_name("HELLO.CAS", 1).empty(), "an upper-case 8.3 name has no alias");
    check(freetvc::fat_short_name("hello.cas", 1) == "HELLO.CAS", "a lower-case 8.3 name: its upper case");
    check(freetvc::fat_short_name("A long name.cas", 1) == "ALONGN~1.CAS", "a long name: ~1");
    check(freetvc::fat_short_name("x.y.z", 2) == "XY~2.Z", "dots inside");
}

}  // namespace

int main() {
    test_pendrive();
    test_flash();
    test_psram_drive();
    test_dsk();
    test_file_functions();
    test_mouse();
    test_snapshot();
    test_short_names();
    if (g_failures == 0) {
        std::printf("card256_storage_test: all passed\n");
        return 0;
    }
    std::printf("card256_storage_test: %d failure(s)\n", g_failures);
    return 1;
}
