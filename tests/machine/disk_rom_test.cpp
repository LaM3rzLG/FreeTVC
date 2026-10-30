// SPDX-License-Identifier: GPL-3.0-or-later
// The HBF disk card on a real ROM set (design/cards.md): a disk autostart through the card's own driver, a snapshot
// taken in the middle of the load running on exactly like the original, and BASIC's SAVE and LOAD through a blank disk.
// usage: disk_rom_test <rom dir> <preset> <hbf.rom> <Flipper.dsk> <scratch dir>; exit 77 (CTest skip) without the files.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/cards/hbf.hpp"
#include "core/input/key_script.hpp"
#include "core/machine/machine.hpp"
#include "core/media/cas.hpp"
#include "frontend/common/host_tape.hpp"
#include "frontend/common/presets.hpp"

namespace {

constexpr int kExitSkip = 77;
constexpr std::uint64_t kFrameTicks = freetvc::kTStatesPerFrame * freetvc::kTicksPerTState;
int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    }
}

bool contains(std::span<const std::uint8_t> haystack, std::span<const std::uint8_t> needle) {
    return !needle.empty() && std::search(haystack.begin(), haystack.end(), needle.begin(), needle.end()) != haystack.end();
}

bool same_machine(const freetvc::Machine& a, const freetvc::Machine& b) {
    const auto ra = a.cpu().registers();
    const auto rb = b.cpu().registers();
    return a.ticks() == b.ticks() && a.pc() == b.pc() && ra.af == rb.af && ra.bc == rb.bc && ra.de == rb.de && ra.hl == rb.hl &&
           ra.sp == rb.sp && ra.ix == rb.ix && ra.iy == rb.iy && std::equal(a.ram().begin(), a.ram().end(), b.ram().begin()) &&
           std::equal(a.memory().vram().begin(), a.memory().vram().end(), b.memory().vram().begin()) &&
           a.framebuffer().pixels == b.framebuffer().pixels;
}

const freetvc::HbfCard* disk_card(const freetvc::Machine& m) {
    return dynamic_cast<const freetvc::HbfCard*>(m.cards().card(freetvc::kDiskCardSlot));
}

void run_autostart(freetvc::HostTape& tape, freetvc::Machine& m) {
    while (tape.media().busy()) {
        tape.media().run_slice(m);
    }
}

// Cold boot, SPACE for the logo, then `text` typed at the prompt; runs to `frames`
void boot_and_type(freetvc::Machine& m, freetvc::HostTape& tape, std::u32string_view text, std::uint64_t frames) {
    m.power_on();
    std::vector<freetvc::KeyEvent> keys = freetvc::type_text(U" ", 380).events;
    const auto typed = freetvc::type_text(text, 450);
    keys.insert(keys.end(), typed.events.begin(), typed.events.end());
    std::size_t next = 0;
    while (m.frame_count() < frames) {
        freetvc::apply_key_events(m, keys, next, m.frame_count());
        m.run({.max_ticks = kFrameTicks, .to_next_frame = true});
        tape.after_slice();
    }
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 6) {
        std::fprintf(stderr, "usage: disk_rom_test <rom dir> <preset> <hbf.rom> <Flipper.dsk> <scratch dir>\n");
        return 2;
    }
    const freetvc::Preset* preset = freetvc::find_preset(argv[2]);
    if (!preset) {
        std::fprintf(stderr, "unknown preset '%s'\n", argv[2]);
        return 2;
    }
    std::string error;
    auto config = freetvc::load_preset(*preset, argv[1], error);
    auto rom = config ? freetvc::read_file(argv[3], 0x4000, error) : std::nullopt;
    auto flipper = rom ? freetvc::read_file(argv[4], 1 << 20, error) : std::nullopt;
    if (!config || !rom || !flipper) {
        std::printf("SKIP: %s\n", error.c_str());
        return kExitSkip;
    }
    config->cards[freetvc::kDiskCardSlot] = {freetvc::CardKind::Hbf, *rom};
    const std::filesystem::path scratch = std::filesystem::path(argv[5]) / argv[2];
    std::filesystem::create_directories(scratch);
    const std::string flipper_path = (scratch / "Flipper.dsk").string();
    std::ofstream(flipper_path, std::ios::binary).write(reinterpret_cast<const char*>(flipper->data()), static_cast<std::streamsize>(flipper->size()));
    const auto image = freetvc::DiskImage::from_bytes(*flipper, error);
    const auto code = image ? image->root_file("FLIPKOD.CAS") : std::nullopt;
    const auto code_cas = code ? freetvc::parse_cas(*code, error) : std::nullopt;
    check(code_cas.has_value(), "FLIPKOD.CAS on the disk");

    // 1. Autostart: LOAD "FLIPPER" through the card; the loader brings in the picture and the code
    {
        freetvc::Machine m(*config);
        freetvc::HostTape tape(m);
        tape.set_disk_rom(*rom);
        check(tape.disk_autostart(flipper_path, error), "disk autostart starts: " + error);
        run_autostart(tape, m);
        check(tape.media().state() == freetvc::AutostartState::Done, "disk autostart: " + tape.media().autostart_result());
        for (int i = 0; i < 100; ++i) {
            m.run({.max_ticks = kFrameTicks});
        }
        if (code_cas) {
            const auto block = code_cas->block();
            check(contains(m.ram(), block.subspan(0, std::min<std::size_t>(block.size(), 2048))), "FLIPKOD's code is in RAM");
        }
    }

    // 2. A snapshot while the FDC reads, loaded into a second machine with the same disk, runs on identically
    {
        freetvc::Machine a(*config);
        freetvc::HostTape ta(a);
        ta.set_disk_rom(*rom);
        check(ta.disk_autostart(flipper_path, error), "second autostart starts");
        while (ta.media().busy() && !(disk_card(a) && disk_card(a)->fdc().busy() && a.frame_count() > 50)) {
            ta.media().run_slice(a);
        }
        // a few hundred instructions on, so the snapshot lands inside a transfer
        a.run({.max_ticks = kFrameTicks, .max_instructions = 333});
        check(disk_card(a) && disk_card(a)->fdc().busy(), "snapshot taken while the FDC is busy");
        const std::vector<std::uint8_t> bytes = ta.snapshot_bytes();
        freetvc::Machine b(*config);
        freetvc::HostTape tb(b);
        tb.set_disk_rom(*rom);
        check(tb.snapshot_load_bytes(bytes, "test", error), "snapshot loads: " + error);
        check(tb.drives().disk(0) != nullptr, "the snapshot brings the disk back by path");
        check(same_machine(a, b), "the loaded machine equals the original");
        for (int i = 0; i < 300; ++i) {
            a.run({.max_ticks = kFrameTicks});
            b.run({.max_ticks = kFrameTicks});
        }
        check(same_machine(a, b), "300 frames later both are still the same");
    }

    // 3. SAVE onto a blank disk, then LOAD it back in a new machine
    {
        const std::string blank_path = (scratch / "blank.dsk").string();
        const freetvc::DiskImage blank = freetvc::DiskImage::blank(2);
        std::ofstream(blank_path, std::ios::binary | std::ios::trunc)
            .write(reinterpret_cast<const char*>(blank.bytes().data()), static_cast<std::streamsize>(blank.bytes().size()));
        {
            freetvc::Machine m(*config);
            freetvc::HostTape tape(m);
            tape.set_disk_rom(*rom);
            check(tape.disk_insert(0, blank_path, error), "blank disk in A:");
            boot_and_type(m, tape, U"10 PRINT 6*7\nSAVE \"T\"\n", 900);
            check(tape.save_disks(), "the written disk is saved");
        }
        const auto saved = freetvc::read_file(blank_path, 1 << 20, error);
        const auto disk = saved ? freetvc::DiskImage::from_bytes(*saved, error) : std::nullopt;
        const auto file = disk ? disk->root_file("T.CAS") : std::nullopt;
        const auto cas = file ? freetvc::parse_cas(*file, error) : std::nullopt;
        check(cas && !cas->buffered && !cas->block().empty(), "SAVE wrote T.CAS on the host file");
        freetvc::Machine m(*config);
        freetvc::HostTape tape(m);
        tape.set_disk_rom(*rom);
        check(tape.disk_insert(0, blank_path, error), "the saved disk in A:");
        boot_and_type(m, tape, U"LOAD \"T\"\n", 900);
        check(cas && contains(m.ram(), cas->block()), "LOAD brought the program back");
    }

    std::printf("%s: %s\n", argv[2], failures == 0 ? "all passed" : "FAILED");
    return failures == 0 ? 0 : 1;
}
