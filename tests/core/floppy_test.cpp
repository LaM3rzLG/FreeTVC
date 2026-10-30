// SPDX-License-Identifier: GPL-3.0-or-later
// The disk card without ROMs (design/cards.md): disk images and their geometry, the WD1793's commands and timing
// through the HBF card's ports, the card's IOMEM and ID, and copies and snapshots of a card in the middle of a command.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/cards/floppy.hpp"
#include "core/cards/hbf.hpp"
#include "core/machine/machine.hpp"
#include "core/snapshot/snapshot.hpp"

namespace {

int g_failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
}

using freetvc::Wd1793;

constexpr std::uint64_t kMs = Wd1793::kTicksPerMs;
constexpr std::uint64_t kRev = Wd1793::kRevolutionTicks;

std::vector<std::uint8_t> card_rom() {
    std::vector<std::uint8_t> rom(0x4000);
    for (std::size_t i = 0; i < rom.size(); ++i) {
        rom[i] = static_cast<std::uint8_t>((i >> 12) * 0x40 + (i & 0x3F));
    }
    return rom;
}

// A card, its drives and a clock: every access 50 ticks after the last, like a polling loop
struct Bench {
    freetvc::HbfCard card{card_rom()};
    freetvc::FloppyDrives drives;
    std::uint64_t tick = 1000;

    freetvc::CardBus bus() { return {tick, &drives}; }
    std::uint8_t in(unsigned port) {
        tick += 50;
        return card.io_read(port, bus());
    }
    void out(unsigned port, std::uint8_t v) {
        tick += 50;
        card.io_write(port, v, bus());
    }
    // Polls +4 until INTRQ, reading every DRQ byte; gives up after `limit` ticks
    std::vector<std::uint8_t> read_until_done(std::uint64_t limit = 10 * kRev) {
        std::vector<std::uint8_t> bytes;
        const std::uint64_t end = tick + limit;
        while (tick < end) {
            const std::uint8_t aux = in(4);
            if (aux & 0x80) {
                bytes.push_back(in(3));
            } else if (aux & 0x01) {
                break;
            }
        }
        return bytes;
    }
    // Feeds `bytes` to every DRQ until INTRQ
    void write_until_done(const std::vector<std::uint8_t>& bytes, std::uint64_t limit = 10 * kRev) {
        std::size_t next = 0;
        const std::uint64_t end = tick + limit;
        while (tick < end) {
            const std::uint8_t aux = in(4);
            if (aux & 0x80) {
                out(3, next < bytes.size() ? bytes[next] : 0);
                ++next;
            } else if (aux & 0x01) {
                break;
            }
        }
    }
    void wait_done(std::uint64_t limit = 10 * kRev) {
        const std::uint64_t end = tick + limit;
        while (tick < end && !(in(4) & 0x01)) {
        }
    }
};

void test_images() {
    std::string error;
    const freetvc::DiskImage blank2 = freetvc::DiskImage::blank(2);
    check(blank2.geometry().tracks == 80 && blank2.geometry().heads == 2 && blank2.geometry().sectors == 9, "blank 720K geometry");
    check(blank2.bytes().size() == 737280, "blank 720K size");
    auto again = freetvc::DiskImage::from_bytes(std::vector<std::uint8_t>(blank2.bytes().begin(), blank2.bytes().end()), error);
    check(again && again->geometry().heads == 2 && again->geometry().tracks == 80, "720K geometry from its boot sector");
    const freetvc::DiskImage blank1 = freetvc::DiskImage::blank(1);
    auto single = freetvc::DiskImage::from_bytes(std::vector<std::uint8_t>(blank1.bytes().begin(), blank1.bytes().end()), error);
    check(single && single->geometry().heads == 1 && single->geometry().tracks == 80, "360K geometry from its boot sector");
    auto raw360 = freetvc::DiskImage::from_bytes(std::vector<std::uint8_t>(368640, 0), error);
    check(raw360 && raw360->geometry().heads == 1 && raw360->geometry().tracks == 80, "360K without a boot sector: 80 x 1");
    auto raw720 = freetvc::DiskImage::from_bytes(std::vector<std::uint8_t>(737280, 0), error);
    check(raw720 && raw720->geometry().heads == 2 && raw720->geometry().tracks == 80, "720K without a boot sector: 80 x 2");
    check(!freetvc::DiskImage::from_bytes(std::vector<std::uint8_t>(1000, 0), error) && !error.empty(), "odd size refused");
    check(blank2.root_files().empty(), "a blank disk has no files");

    // One file of 1500 bytes in clusters 2 and 3 (1 KiB clusters): the FAT chain is followed
    freetvc::DiskImage disk = freetvc::DiskImage::blank(2);
    std::uint8_t* boot = disk.sector(0, 0, 1);
    check(boot != nullptr, "boot sector reachable");
    const std::size_t fat = 512;
    const std::size_t root = 512 * (1 + 2 * 3);
    const std::size_t data = root + 112 * 32;
    std::vector<std::uint8_t> bytes(disk.bytes().begin(), disk.bytes().end());
    bytes[fat + 3] = 0x03;  // cluster 2 -> 3
    bytes[fat + 4] = 0xF0;  // cluster 3 -> FFFh
    bytes[fat + 5] = 0xFF;
    const char entry[12] = "PROG    CAS";
    std::copy(entry, entry + 11, bytes.begin() + static_cast<std::ptrdiff_t>(root));
    bytes[root + 26] = 2;
    bytes[root + 28] = 1500 & 0xFF;
    bytes[root + 29] = 1500 >> 8;
    for (std::size_t i = 0; i < 1500; ++i) {
        bytes[data + i] = static_cast<std::uint8_t>(i * 7);
    }
    auto withfile = freetvc::DiskImage::from_bytes(bytes, error);
    check(withfile && withfile->root_files() == std::vector<std::string>{"PROG.CAS"}, "root directory listing");
    const auto content = withfile ? withfile->root_file("prog.cas") : std::nullopt;
    bool same = content && content->size() == 1500;
    for (std::size_t i = 0; same && i < 1500; ++i) {
        same = (*content)[i] == static_cast<std::uint8_t>(i * 7);
    }
    check(same, "file read through the FAT chain");
}

freetvc::DiskImage patterned_disk() {
    freetvc::DiskImage disk = freetvc::DiskImage::blank(2);
    for (unsigned t = 1; t < 80; ++t) {
        for (unsigned h = 0; h < 2; ++h) {
            for (unsigned s = 1; s <= 9; ++s) {
                std::uint8_t* p = disk.sector(t, h, s);
                for (unsigned i = 0; i < 512; ++i) {
                    p[i] = static_cast<std::uint8_t>(t * 3 + h * 101 + s * 17 + i);
                }
            }
        }
    }
    return disk;
}

bool sector_matches(const std::vector<std::uint8_t>& bytes, std::size_t at, unsigned t, unsigned h, unsigned s) {
    if (bytes.size() < at + 512) {
        return false;
    }
    for (unsigned i = 0; i < 512; ++i) {
        if (bytes[at + i] != static_cast<std::uint8_t>(t * 3 + h * 101 + s * 17 + i)) {
            return false;
        }
    }
    return true;
}

void test_fdc() {
    Bench b;
    b.drives.insert(0, patterned_disk(), false);
    b.drives.insert(2, patterned_disk(), true);

    // Not ready: no drive selected
    b.out(0, 0x80);
    check((b.in(0) & 0x81) == 0x80, "read sector with no drive: not ready, not busy");

    // Drive A, motor on, side 0; restore at 6 ms a step lands on track 0 at once
    b.out(4, 0x41);
    b.out(0, 0x08);
    b.wait_done();
    std::uint8_t s = b.in(0);
    check((s & 0x01) == 0 && (s & 0x04) && (s & 0x20), "restore: idle, track 0, head loaded");

    // Seek to 5 with verify: 5 steps of 6 ms, 30 ms settle, then an ID
    const std::uint64_t start = b.tick;
    b.out(1, 0);
    b.out(3, 5);
    b.out(0, 0x14);
    b.wait_done();
    s = b.in(0);
    check(b.card.fdc().cylinder(0) == 5 && b.in(1) == 5, "seek: head and track register at 5");
    check((s & 0x18) == 0, "seek: verified");
    check(b.tick - start >= 5 * 6 * kMs + 30 * kMs, "seek: takes the step and settle time");
    check(b.tick - start <= 5 * 6 * kMs + 30 * kMs + kRev + kMs, "seek: verify within a revolution");

    // A seek out while the head already sits on track 0 stops there with the track register at 0
    {
        Bench t;
        t.drives.insert(0, patterned_disk(), false);
        t.out(4, 0x41);
        t.out(1, 7);
        t.out(3, 2);
        t.out(0, 0x10);
        t.wait_done();
        check(t.in(1) == 0 && t.card.fdc().cylinder(0) == 0, "seek out at TR00: track register 0, no step");
    }

    // Activity (drive light, fast disk): every step counts, idle does not, nor a time after a restarted clock
    {
        Bench t;
        t.drives.insert(0, patterned_disk(), false);
        t.out(4, 0x41);
        t.out(3, 40);
        t.out(0, 0x13);  // seek to 40 at 30 ms a step: 1.2 s without a byte read
        for (int i = 0; i < 10; ++i) {
            t.tick += 100 * kMs;
            (void)t.in(0);
        }
        check((t.in(0) & 0x01) && t.drives.active(0, t.tick, 31 * kMs), "a long seek keeps the drive active between steps");
        t.wait_done();
        t.tick += kRev * 2;
        (void)t.in(0);
        check(!t.drives.active(0, t.tick, kRev + kMs) && !t.drives.any_active(t.tick, kRev + kMs), "idle drives are not active");
        check(!t.drives.active(0, 1000, kRev), "a time after `now` is not activity");
    }

    // Read sector 3 on side 0
    b.out(2, 3);
    b.out(0, 0x80);
    std::vector<std::uint8_t> bytes = b.read_until_done();
    s = b.in(0);
    check(bytes.size() == 512 && sector_matches(bytes, 0, 5, 0, 3), "read sector 5/0/3");
    check((s & 0x7D) == 0, "read sector: no error");

    // Side 1 through the latch, side compare off
    b.out(4, 0xC1);
    b.out(0, 0x80);
    bytes = b.read_until_done();
    check(sector_matches(bytes, 0, 5, 1, 3), "read sector on side 1");
    // Side compare on with S = 0 while the head reads side 1: not found after five index pulses
    const std::uint64_t rnf_start = b.tick;
    b.out(0, 0x82);
    bytes = b.read_until_done();
    s = b.in(0);
    check(bytes.empty() && (s & 0x10), "side compare: record not found");
    check(b.tick - rnf_start >= 4 * kRev, "record not found after the index pulses");
    b.out(4, 0x41);

    // Multiple records from sector 8: 8 and 9, then not found
    b.out(2, 8);
    b.out(0, 0x90);
    bytes = b.read_until_done(20 * kRev);
    check(bytes.size() == 1024 && sector_matches(bytes, 0, 5, 0, 8) && sector_matches(bytes, 512, 5, 0, 9), "multiple-record read");
    check(b.in(2) == 10 && (b.in(0) & 0x10), "multiple-record read ends with sector 10 not found");

    // Lost data: nobody reads the data register
    b.out(2, 1);
    b.out(0, 0x80);
    b.wait_done();
    check((b.in(0) & 0x04) != 0, "read sector unserved: lost data");

    // Write sector 2, then read it back; the drive is marked written
    std::vector<std::uint8_t> pattern(512);
    for (unsigned i = 0; i < 512; ++i) {
        pattern[i] = static_cast<std::uint8_t>(0xA5 ^ i);
    }
    b.out(2, 2);
    b.out(0, 0xA0);
    b.write_until_done(pattern);
    check((b.in(0) & 0x7C) == 0, "write sector: no error");
    check(b.drives.dirty(0), "write sector marks the drive written");
    check(std::equal(pattern.begin(), pattern.end(), b.drives.disk(0)->sector(5, 0, 2)), "write sector reaches the image");
    b.out(0, 0x80);
    check(b.read_until_done() == pattern, "written sector reads back");

    // Read address: C H R N and a correct CRC; the sector register gets the track
    b.out(0, 0xC0);
    bytes = b.read_until_done();
    check(bytes.size() == 6 && bytes[0] == 5 && bytes[1] == 0 && bytes[2] >= 1 && bytes[2] <= 9 && bytes[3] == 2, "read address");
    check(b.in(2) == 5, "read address puts the track into the sector register");
    if (bytes.size() == 6) {
        std::uint16_t crc = 0xFFFF;
        for (const std::uint8_t v : {std::uint8_t{0xA1}, std::uint8_t{0xA1}, std::uint8_t{0xA1}, std::uint8_t{0xFE}, bytes[0], bytes[1], bytes[2], bytes[3]}) {
            crc ^= static_cast<std::uint16_t>(v << 8);
            for (int i = 0; i < 8; ++i) {
                crc = (crc & 0x8000) ? static_cast<std::uint16_t>((crc << 1) ^ 0x1021) : static_cast<std::uint16_t>(crc << 1);
            }
        }
        check(bytes[4] == (crc >> 8) && bytes[5] == (crc & 0xFF), "read address CRC");
    }

    // Read track: a whole revolution with the ID of sector 1
    b.out(0, 0xE0);
    bytes = b.read_until_done();
    const std::uint8_t id1[] = {0xA1, 0xA1, 0xA1, 0xFE, 5, 0, 1, 2};
    check(bytes.size() == Wd1793::kTrackBytes && std::search(bytes.begin(), bytes.end(), std::begin(id1), std::end(id1)) != bytes.end(),
          "read track");

    // Write track (format) on track 6 side 1: nine 512-byte sectors of F0h + R
    b.out(3, 6);
    b.out(0, 0x10);
    b.wait_done();
    b.out(4, 0xC1);
    std::vector<std::uint8_t> track;
    const auto put = [&](std::uint8_t v, unsigned n) { track.insert(track.end(), n, v); };
    put(0x4E, 80);
    put(0x00, 12);
    put(0xF6, 3);
    put(0xFC, 1);
    put(0x4E, 50);
    for (unsigned r = 1; r <= 9; ++r) {
        put(0x00, 12);
        put(0xF5, 3);
        put(0xFE, 1);
        track.insert(track.end(), {6, 1, static_cast<std::uint8_t>(r), 2});
        put(0xF7, 1);
        put(0x4E, 22);
        put(0x00, 12);
        put(0xF5, 3);
        put(0xFB, 1);
        put(static_cast<std::uint8_t>(0xF0 + r), 512);
        put(0xF7, 1);
        put(0x4E, 54);
    }
    put(0x4E, 6250);
    b.out(0, 0xF0);
    b.write_until_done(track);
    check((b.in(0) & 0x44) == 0, "write track: no error");
    const std::uint8_t* formatted = b.drives.disk(0)->sector(6, 1, 4);
    check(formatted && std::all_of(formatted, formatted + 512, [](std::uint8_t v) { return v == 0xF4; }), "write track formats the sectors");
    b.out(4, 0x41);

    // Force interrupt: D0 stops a command without INTRQ, D8 raises it until the next command
    b.out(0, 0x80);
    b.out(0, 0xD0);
    check((b.in(0) & 0x01) == 0 && (b.in(4) & 0x01) == 0, "D0: stopped, no INTRQ");
    b.out(0, 0xD8);
    check((b.in(4) & 0x01) != 0, "D8: INTRQ");
    (void)b.in(0);
    check((b.in(4) & 0x01) != 0, "D8: INTRQ survives a status read");
    b.out(0, 0xD0);
    check((b.in(4) & 0x01) == 0, "D0 after D8 clears INTRQ");

    // Write-protected disk in C:
    b.out(4, 0x44);
    b.out(0, 0xA0);
    b.wait_done();
    check((b.in(0) & 0x40) != 0 && !b.drives.dirty(2), "write to a protected disk refused");
}

void test_copies_and_snapshots() {
    // A read in progress, copied and snapshotted: all three go on alike
    Bench a;
    a.drives.insert(0, patterned_disk(), false);
    a.out(4, 0x41);
    a.out(3, 9);
    a.out(0, 0x10);
    a.wait_done();
    a.out(2, 4);
    a.out(0, 0x80);
    std::vector<std::uint8_t> first;
    while (first.size() < 100) {
        if (a.in(4) & 0x80) {
            first.push_back(a.in(3));
        }
    }
    freetvc::ChunkWriter w;
    a.card.save(w);
    Bench b;
    b.drives.insert(0, patterned_disk(), false);
    b.tick = a.tick;
    freetvc::ChunkReader r(w.data);
    b.card.load(r);
    check(!r.failed() && r.at_end(), "card state reads back whole");
    Bench c;
    c.drives.insert(0, patterned_disk(), false);
    c.tick = a.tick;
    const std::unique_ptr<freetvc::Card> clone = a.card.clone();
    const auto rest_a = a.read_until_done();
    const auto rest_b = b.read_until_done();
    std::vector<std::uint8_t> rest_c;
    for (const std::uint64_t end = c.tick + 10 * kRev; c.tick < end;) {
        c.tick += 50;
        const std::uint8_t aux = clone->io_read(4, c.bus());
        if (aux & 0x80) {
            c.tick += 50;
            rest_c.push_back(clone->io_read(3, c.bus()));
        } else if (aux & 0x01) {
            break;
        }
    }
    check(rest_a.size() == 412 && rest_a == rest_b && rest_a == rest_c, "a copy and a snapshot finish the read alike");
    first.insert(first.end(), rest_a.begin(), rest_a.end());
    check(sector_matches(first, 0, 9, 0, 4), "the split read gives the sector");
}

void test_machine() {
    freetvc::MachineConfig config;
    check(freetvc::Machine(config).io_read(0x5A) == 0xFF, "port 5Ah: no cards");
    config.cards[0] = {freetvc::CardKind::Hbf, card_rom()};
    freetvc::Machine m(config);
    check(m.io_read(0x5A) == 0xFE, "port 5Ah: floppy card in slot 0");
    m.io_write(0x02, 0xC0);  // page 3 = IOMEM
    m.io_write(0x03, 0x00);  // slot 0
    check(m.peek(0xC000) == 0x00 && m.peek(0xC03F) == 0x3F, "IOMEM: EPROM page 0");
    m.io_write(0x18, 0x20);  // page register at slot + 8, b5-4
    check(m.peek(0xC001) == 0x81, "IOMEM: EPROM page 2");
    m.poke(0xD123, 0x5A);
    m.poke(0xC010, 0x77);
    check(m.peek(0xD123) == 0x5A && m.peek(0xC010) == 0x90, "IOMEM: RAM at D000, EPROM read-only");
    check(m.bank_bytes(freetvc::Bank::Card0).size() == 0x2000 && m.bank_bytes(freetvc::Bank::Card0)[0x1123] == 0x5A, "CARD0 bank view");
    freetvc::Machine copy(m);
    m.poke(0xD123, 0x11);
    check(copy.peek(0xD123) == 0x5A, "a copied machine has its own card");
    m.io_write(0x03, 0x40);  // slot 1 is empty
    check(m.peek(0xC000) == 0xFF && m.io_read(0x20) == 0xFF, "an empty slot is open bus");
    m.io_write(0x03, 0x00);
    m.reset();
    m.io_write(0x02, 0xC0);
    check(m.peek(0xC001) == 0x01 && m.peek(0xD123) == 0x11, "reset: page 0, RAM kept");

    freetvc::Snapshot snap;
    m.save_state(snap);
    freetvc::Machine plain{freetvc::MachineConfig{}};
    std::string error;
    check(!plain.load_state(snap, error), "a card snapshot needs the card");
    freetvc::Machine other(config);
    check(other.load_state(snap, error) && other.peek(0xD123) == 0x11, "a card snapshot loads into a machine with the card");
}

}  // namespace

int main() {
    test_images();
    test_fdc();
    test_copies_and_snapshots();
    test_machine();
    if (g_failures == 0) {
        std::printf("floppy_test: all passed\n");
        return 0;
    }
    std::printf("floppy_test: %d failures\n", g_failures);
    return 1;
}
