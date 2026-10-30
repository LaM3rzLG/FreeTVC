// SPDX-License-Identifier: GPL-3.0-or-later
// The NanoSD without system ROMs (design/cards.md "NanoSD"): the latches, the Nano's commands as the ROM sends them,
// the SD library's view of a FAT volume, the firmware's quirks, card detect, snapshots.
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "core/cards/card256_storage.hpp"
#include "core/cards/nanosd.hpp"
#include "core/machine/machine.hpp"
#include "core/snapshot/snapshot.hpp"

namespace {

using Bytes = std::vector<std::uint8_t>;

int g_failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what.c_str());
    }
}

constexpr std::uint64_t kTicksPerSecond = 6'250'000;
constexpr std::uint8_t kPort = 0x40;  // slot 3

Bytes banked_rom() {
    Bytes rom(freetvc::NanoSd::kRomSize, 0xFF);
    rom[0xC000] = 'M';
    rom[0xE000] = 0xB1;
    return rom;
}

freetvc::MachineConfig config_with_card() {
    freetvc::MachineConfig config;
    config.cards[freetvc::kNanoSdSlot] = {freetvc::CardKind::NanoSd, banked_rom()};
    return config;
}

void store(freetvc::FatVolume& v, const std::string& path, const Bytes& bytes) {
    std::size_t put = 0;
    v.create(path);
    v.write(path, 0, bytes, put);
}

Bytes load(freetvc::FatVolume& v, const std::string& path) {
    freetvc::FatEntry e;
    if (v.stat(path, e) != freetvc::kFrOk) {
        return {};
    }
    Bytes out(e.size);
    std::size_t got = 0;
    v.read(path, 0, out, got);
    return out;
}

// A CAS file: type byte, 127 header bytes, then the data
Bytes cas(std::uint8_t type, const Bytes& data) {
    Bytes out(128, 0);
    out[0] = type;
    out.insert(out.end(), data.begin(), data.end());
    return out;
}

struct Rig {
    Bytes image = freetvc::fat_image_format(1024 * 1024);
    std::unique_ptr<freetvc::FatVolume> sd = freetvc::fat_image_volume(image);
    freetvc::NanoSdMedia media;
    freetvc::Machine m{config_with_card()};

    explicit Rig(bool with_card = true) {
        media.sd = with_card ? sd.get() : nullptr;
        m.set_nanosd_media(&media);
    }
    std::uint8_t in() { return m.io_read(kPort); }
    void out(std::initializer_list<std::uint8_t> bytes) {
        for (const std::uint8_t b : bytes) {
            m.io_write(kPort, b);
        }
    }
    void out_text(const std::string& s, bool zero = true) {
        for (const char c : s) {
            m.io_write(kPort, static_cast<std::uint8_t>(c));
        }
        if (zero) {
            m.io_write(kPort, 0);
        }
    }
    // The ROM's pattern: drop the first reply, then poll past DATA_NOT_READY_YET (80h) and BUFFER_UNDERRUN (89h)
    std::uint8_t reply() {
        in();
        std::uint8_t v = in();
        for (int i = 0; i < 100 && (v == 0x80 || v == 0x81 || v == 0x89); ++i) {
            v = in();
        }
        return v;
    }
    void wait(double seconds) { m.run({.max_ticks = static_cast<std::uint64_t>(seconds * kTicksPerSecond)}); }
    freetvc::NanoSd& card() { return *dynamic_cast<freetvc::NanoSd*>(m.card(freetvc::kNanoSdSlot)); }
};

void test_latch_and_window() {
    Rig r;
    check((r.m.io_read(0x5A) >> 6) == 3, "port 5Ah reads 11 for slot 3");
    check(r.in() == 0x80, "the first read after power-on gives the status setup() latched");
    // Every port of the slot is the one register: GETCDIR on +7, read on +F
    r.m.io_write(kPort + 7, 0x07);
    r.m.io_read(kPort + 0xF);
    check(r.m.io_read(kPort + 3) == 1 && r.m.io_read(kPort + 9) == '/', "the 16 ports alias (GETCDIR gives 1, '/')");
    check(r.card().mem_read(0) == 'M', "bank 0 is the image's C000h");
    r.out({0x09});  // BANKTO1
    check(r.card().mem_read(0) == 'M', "BANKTO1 waits for the next latched byte");
    check(r.in() != 0 && r.card().mem_read(0) == 0xB1, "the read moves A13: bank 1 is E000h");
    r.out({0x08});
    r.in();
    check(r.card().mem_read(0) == 'M', "BANKTO0 back");
    check(r.in() == 0x8E, "BANK_SELECT_DONE");
}

void test_parameters() {
    Rig r;
    check(!r.media.eeprom_valid, "the EEPROM starts without its ID");
    r.out({0x0D, 0x00});
    check(r.reply() == 1 && r.in() == 1, "parameter 0 (menu at boot): byte type, 1");
    check(r.media.eeprom_valid && r.media.menu == 1 && r.media.sort == 0, "the first parameter call writes the ID and the defaults");
    r.out({0x0D, 0x03});
    std::string version;
    check(r.reply() == 3, "parameter 3 is a string");
    for (std::uint8_t c; (c = r.in()) != 0 && version.size() < 40;) {
        version += static_cast<char>(c);
    }
    check(version == "NanoSD Arduino fw v0.32", "the version string, without -dbg: '" + version + "'");
    r.out({0x0D, 0x02});
    check(r.reply() == 1 && r.in() == 1, "parameter 2: the SD card is ready");
    r.out({0x0D, 0x07});
    check(r.reply() == 0x99, "an unknown parameter: INVALID_PARAMETER_ID");
    r.out({0x0E, 0x01, 0x02});
    check(r.reply() == 0x9A && r.media.sort == 2, "SETPARAMETER 1 = 2 (sort by size)");
    // An unknown ID takes no value byte: 07h then runs as GETCDIR
    r.out({0x0E, 0x05, 0x07});
    check(r.reply() == 1 && r.in() == '/', "SETPARAMETER with an unknown ID leaves its value byte as the next command");
}

void test_open_read() {
    Rig r;
    Bytes data(150);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::uint8_t>(i * 7);
    }
    store(*r.sd, "/GAME.CAS", cas(0x11, data));
    store(*r.sd, "/README.TXT", {'h', 'i'});
    r.out({0x00});
    r.out_text("game");
    check(r.reply() == 4, "OPEN: the name's length (the ROM never sees OPEN_OK itself)");
    std::string name;
    for (int i = 0; i < 4; ++i) {
        name += static_cast<char>(r.in());
    }
    std::uint32_t size = 0;
    for (int i = 0; i < 4; ++i) {
        size |= static_cast<std::uint32_t>(r.in()) << (8 * i);
    }
    check(name == "GAME" && size == 150, "OPEN \"game\": GAME.CAS upper-cased and found, 150 bytes after the header");
    Bytes got;
    for (int block = 0; block < 4; ++block) {
        r.out({0x05});
        const std::uint8_t n = r.reply();
        if (n == 0x83) {
            break;
        }
        for (unsigned i = 0; i < n; ++i) {
            got.push_back(r.in());
        }
    }
    check(got == data, "GETDATA: 64 + 64 + 22 bytes, then END_OF_FILE");
    r.out({0x01});
    check(r.reply() == 0x8A, "CLOSE_OK");
    // A plain file keeps its first bytes and loses the last 4 characters of its name
    r.out({0x00});
    r.out_text("/README.TXT");
    check(r.reply() == 6 && r.in() == 'R', "a plain file: name without its last 4 characters");
    for (int i = 0; i < 5; ++i) {
        r.in();
    }
    check(r.in() == 2 && r.in() == 0 && r.in() == 0 && r.in() == 0, "a plain file's whole size");
    r.out({0x00});
    r.out_text("NOPE");
    check(r.reply() == 0x82, "FILE_NOT_FOUND");
    // An empty name walks the directory's .CAS files
    store(*r.sd, "/SECOND.CAS", cas(0x01, {1, 2, 3}));
    r.out({0x00, 0x00});
    check(r.reply() == 4 && r.in() == 'G', "OPEN \"\": the first .CAS file");
    for (int i = 0; i < 7; ++i) {
        r.in();
    }
    r.out({0x00, 0x00});
    check(r.reply() == 6 && r.in() == 'S', "OPEN \"\" again: the next one, README.TXT skipped");
    for (int i = 0; i < 9; ++i) {
        r.in();
    }
    r.out({0x00, 0x00});
    check(r.reply() == 0x82, "OPEN \"\" past the last one: FILE_NOT_FOUND");
}

void test_list_and_info() {
    Rig r;
    store(*r.sd, "/GAME.CAS", cas(0x01, Bytes(200)));
    r.sd->make_dir("/GAMES");
    store(*r.sd, "/LongFileName.cas", cas(0x01, Bytes(10)));
    std::vector<std::string> names;
    r.out({0x03});
    for (int n = 0; n < 10; ++n) {
        const std::uint8_t v = r.reply();
        if (v != 0) {
            check(v == 0x84, "LIST ends with END_OF_LIST");
            break;
        }
        std::string e;
        for (int i = 0; i < 17; ++i) {
            e += static_cast<char>(r.in());
        }
        names.push_back(e);
        r.out({0x04});
    }
    check(names.size() == 3, "LIST: three entries");
    bool alias = false;
    for (const std::string& e : names) {
        if (e.compare(0, 12, "GAME.CAS    ") == 0) {
            check(e[12] == 1 && static_cast<std::uint8_t>(e[13]) == 200 && e[14] == 0, "GAME.CAS: a file of 200 bytes (header off)");
        } else if (e.compare(0, 12, "GAMES       ") == 0) {
            check(e[12] == 0 && e[13] == 0, "GAMES: a directory");
        } else {
            alias = e.compare(0, 9, "LONGFI~1.") == 0;
        }
    }
    check(alias, "a long name shows as its 8.3 alias");
    // A byte other than ACK_LIST ends the list and is eaten
    r.out({0x03});
    check(r.reply() == 0, "LIST again");
    r.out({0x07});
    check(r.reply() == 0x84 && r.card().output_size() == 0, "GETCDIR during LIST ends it and is consumed");
    // FINFO: as spelt, and one byte short
    store(*r.sd, "/BIG.CAS", cas(0x01, Bytes(70000)));
    r.out({0x06});
    r.out_text("BIG.CAS");
    std::string info;
    check(r.reply() == 0, "FINFO: OK");
    for (int i = 0; i < 16; ++i) {
        info += static_cast<char>(r.in());
    }
    check(info.compare(0, 12, "BIG.CAS     ") == 0 && static_cast<std::uint8_t>(info[13]) == 0x70 && static_cast<std::uint8_t>(info[14]) == 0x11 &&
              static_cast<std::uint8_t>(info[15]) == 0x01 && r.card().output_size() == 0,
          "FINFO: 70000 = 011170h, but the size's top byte never goes out");
    r.out({0x06});
    r.out_text("big.cas");
    r.reply();
    for (int i = 0; i < 13; ++i) {
        r.in();
    }
    check(r.in() == 0xF0, "FINFO \"big.cas\": the name as spelt, so the 128 header bytes are counted (70128 = 0111F0h)");
}

void test_write() {
    Rig r;
    const auto save = [&](const std::string& name, std::uint8_t type, const Bytes& data) {
        r.out({0x0A, static_cast<std::uint8_t>(name.size())});
        r.out_text(name, false);
        r.out({type});
        const std::uint8_t created = r.reply();
        r.out({0x0B, static_cast<std::uint8_t>(data.size())});
        r.in();
        for (const std::uint8_t b : data) {
            r.out({b});
        }
        const std::uint8_t put = r.reply();
        r.out({0x0C});
        return std::make_pair(created == 0x8F && put == 0x91, r.reply() == 0x8A);
    };
    const auto [ok, closed] = save("hello", 0x01, {1, 2, 3});
    check(ok && closed, "CREATE, PUTDATA, CLOSEWRITE");
    const Bytes file = load(*r.sd, "/HELLO.CAS");
    check(file.size() == 256 && file[0] == 0x01 && file[1] == 0 && file[128] == 1 && file[131] == 0,
          "\"hello\" becomes HELLO.CAS: type 01h, zero header, data padded to 128");
    save("hello", 0x11, {9});
    check(load(*r.sd, "/HELLO.CAS").size() == 256 + 129, "CREATE onto an existing name appends (FILE_WRITE is O_APPEND) ⚠");
    save("data.txt", 0x01, {'x'});
    check(load(*r.sd, "/DATA.TXT") == Bytes{'x'}, "a name not ending in .CAS writes a plain file");
    save("game.cas", 0x11, {'y'});
    check(load(*r.sd, "/GAME.CAS") == Bytes{'y'}, "\".cas\" in lower case is not \".CAS\": no header");
    save("A.B", 0x11, {'z'});
    check(load(*r.sd, "/A.B").size() == 129, "\"A.B\": -1 == 3 - 4 in 16 bits, so the type stays");
    r.out({0x0B, 1, 0x55});
    check(r.reply() == 0x92, "PUTDATA without a file: DATA_FAILED_TO_RECEIVE");
}

void test_directories() {
    Rig r;
    const auto path_cmd = [&](std::uint8_t cmd, const std::string& path) {
        r.out({cmd});
        r.out_text(path);
        return r.reply();
    };
    const auto cwd = [&] {
        r.out({0x07});
        const std::uint8_t n = r.reply();
        std::string s;
        for (unsigned i = 0; i < n && i < 100; ++i) {
            s += static_cast<char>(r.in());
        }
        return s;
    };
    check(path_cmd(0x0F, "A/B") == 0x93, "MKDIR A/B makes both levels");
    check(path_cmd(0x0F, "A") == 0x94, "MKDIR of an existing directory fails");
    check(path_cmd(0x02, "A/B") == 0x8C && cwd() == "/A/B/", "CHDIR A/B");
    check(path_cmd(0x02, "../..") == 0x8C && cwd() == "/", "CHDIR ../.. back to the root");
    check(path_cmd(0x02, "/../") == 0x8C && cwd().empty(), "CHDIR /../ empties the path, which still works as the root");
    check(path_cmd(0x02, "A") == 0x8C && cwd() == "A/", "a relative path then has no leading slash");
    check(path_cmd(0x02, "/NOWHERE") == 0x8D, "CHDIR to a missing directory fails");
    store(*r.sd, "/A/F.CAS", cas(0x01, {1}));
    check(path_cmd(0x10, "/A") == 0x96, "RMDIR of a directory that is not empty fails");
    check(path_cmd(0x11, "/A/B") == 0x98, "DELETE of a directory fails");
    check(path_cmd(0x11, "/A/F.CAS") == 0x97, "DELETE a file");
    check(path_cmd(0x10, "/A/B") == 0x95 && path_cmd(0x10, "/A") == 0x95, "RMDIR the empty ones");
    check(path_cmd(0x02, "/A") == 0x8D, "and they are gone");
}

void test_card_detect() {
    Rig r(false);
    check(r.in() == 0x86, "power-on without a card: CARD_NOT_READY");
    r.out({0x07});
    check(r.reply() == 0xFF, "a command without a card: STATUS_NO_SD");
    r.out({0xFE});
    check(r.reply() == 75, "command FEh without a card answers 75");
    r.out({0x0D, 0x02});
    check(r.reply() == 1 && r.in() == 0, "parameter 2 without a card: 0");
    const auto plug = [&](bool in) {
        r.media.sd = in ? r.sd.get() : nullptr;
        r.media.sd_changed = r.m.ticks();
    };
    plug(true);
    r.wait(0.1);
    check(r.reply() == 0x80, "inserted: the Nano debounces with DATA_NOT_READY_YET");
    r.wait(0.25);
    r.out({0x0D, 0x02});
    check(r.reply() == 1 && r.in() == 1, "after ~300 ms the card is ready");
    plug(false);
    r.wait(0.4);
    check(r.reply() == 0x86 && !r.card().sd_ready(), "taken out: CARD_NOT_READY");
    // While the Nano debounces, writes pile up in its 80-byte input queue
    plug(true);
    r.in();
    for (int i = 0; i < 81; ++i) {
        r.out({0x07});
    }
    r.in();  // the status latched by the read before the writes
    check(r.card().input_size() == 80 && r.in() == 0x88, "the 81st byte: BUFFER_OVERRUN");
}

void test_hang_and_power() {
    Rig r;
    store(*r.sd, "/BIG.TXT", Bytes(1000, 0xAA));
    r.out({0x00});
    r.out_text("/BIG.TXT");
    r.reply();
    r.out({0x05, 0x05});
    check(r.card().hung(), "two GETDATA blocks without reading: the output queue is full, the Nano hangs");
    r.in();  // the byte latched before
    check(r.in() == 0x80 && r.in() == 0x80, "a hung Nano reads DATA_NOT_READY_YET for ever");
    r.m.reset();
    check(r.card().hung(), "the reset button does not reach the Nano");
    r.m.power_on();
    check(!r.card().hung() && r.in() == 0x80, "power-on restarts it");
}

void test_snapshot_and_copy() {
    Rig r;
    Bytes data(300);
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<std::uint8_t>(i ^ 0x5A);
    }
    store(*r.sd, "/S.CAS", cas(0x01, data));
    r.out({0x00});
    r.out_text("S");
    r.reply();
    for (int i = 0; i < 5; ++i) {
        r.in();
    }
    r.out({0x05});
    r.reply();
    for (int i = 0; i < 10; ++i) {
        r.in();
    }
    freetvc::Snapshot snap;
    r.m.save_state(snap);
    freetvc::Machine copy = r.m;
    copy.set_nanosd_media(&r.media);
    freetvc::Machine restored(config_with_card());
    restored.set_nanosd_media(&r.media);
    std::string error;
    check(restored.load_state(snap, error), "snapshot loads: " + error);
    Bytes a, b, c;
    for (int i = 0; i < 54; ++i) {
        a.push_back(r.m.io_read(kPort));
        b.push_back(copy.io_read(kPort));
        c.push_back(restored.io_read(kPort));
    }
    check(a == b && a == c && a[0] == data[10], "a copy and a snapshot go on mid-block with the same bytes");
}

}  // namespace

int main() {
    test_latch_and_window();
    test_parameters();
    test_open_read();
    test_list_and_info();
    test_write();
    test_directories();
    test_card_detect();
    test_hang_and_power();
    test_snapshot_and_copy();
    if (g_failures) {
        std::printf("nanosd_test: %d failures\n", g_failures);
        return 1;
    }
    std::printf("nanosd_test: ok\n");
    return 0;
}
