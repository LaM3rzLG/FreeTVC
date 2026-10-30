// SPDX-License-Identifier: GPL-3.0-or-later
// The SD Dual firmware 2022.11.09 on real ROMs: SAVE from BASIC onto a FAT card our builder made, LOAD and RUN it
// after a power-on, the VT-DOS CLI's DIR on both sockets with an SDHC card in the second; with the author's files
// (optional) the firmware updater with and without the flash jumper and a VT-DOS boot from the card;
// usage: sddual_rom_test <rom dir> <preset> <sddual.rom> [<sd_dual_2022_11_09_gm dir> <system_f_v2 dir>].
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "core/bus/sddual.hpp"
#include "core/input/key_script.hpp"
#include "core/machine/machine.hpp"
#include "frontend/common/presets.hpp"
#include "frontend/common/sd_image.hpp"

namespace {

namespace fs = std::filesystem;

constexpr int kExitSkip = 77;
constexpr std::uint64_t kFrameTicks = freetvc::kTStatesPerFrame * freetvc::kTicksPerTState;
int failures = 0;

void check(bool ok, const std::string& what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what.c_str());
        ++failures;
    }
}

// The OS's copy of the text screen in U0 (tvc-ram.sym): 24 rows of 64, or of 80 in VT-DOS's 80-column mode
std::string screen(const freetvc::Machine& m, unsigned width) {
    const auto u0 = m.memory().bank_bytes(freetvc::Bank::U0);
    std::string text;
    for (unsigned row = 0; row < 24; ++row) {
        std::string line(reinterpret_cast<const char*>(u0.data()) + 0x100 + row * width, width);
        line.erase(line.find_last_not_of(' ') + 1);
        text += line + "\n";
    }
    return text;
}

std::string screen(const freetvc::Machine& m) { return screen(m, 64) + screen(m, 80); }

struct Rig {
    freetvc::Machine& m;
    std::vector<freetvc::KeyEvent> keys;
    std::size_t next = 0;

    void frame() {
        freetvc::apply_key_events(m, keys, next, m.frame_count());
        m.run({.max_ticks = kFrameTicks, .to_next_frame = true});
    }
    void frames(int n) {
        for (int i = 0; i < n; ++i) {
            frame();
        }
    }
    bool wait_for(const std::string& text, int n) {
        for (int i = 0; i < n; ++i) {
            if (screen(m).find(text) != std::string::npos) {
                return true;
            }
            frame();
        }
        return screen(m).find(text) != std::string::npos;
    }
    void press(freetvc::MatrixKey key) {
        const std::uint64_t f = m.frame_count();
        keys.push_back({f, key, true});
        keys.push_back({f + 4, key, false});
        frames(8);
    }
    void press(const char* name) { press(*freetvc::find_key(name)); }
    void type(std::u32string_view text) {
        const auto typed = freetvc::type_text(text, m.frame_count());
        keys.insert(keys.end(), typed.events.begin(), typed.events.end());
        while (m.frame_count() < typed.end_frame) {
            frame();
        }
    }
    // Power on to the BASIC prompt: the start-up logo takes the first key
    bool basic() {
        m.power_on();
        frames(300);
        for (int i = 0; i < 4 && !wait_for("bytes free", 100); ++i) {
            press(freetvc::kKeySpace);
        }
        return wait_for("bytes free", 100);
    }
};

bool has(const std::vector<std::string>& listing, const std::string& path) {
    return std::find(listing.begin(), listing.end(), path) != listing.end();
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4 && argc != 6) {
        std::fprintf(stderr, "usage: sddual_rom_test <rom dir> <preset> <sddual.rom> [<updater dir> <system dir>]\n");
        return 2;
    }
    const freetvc::Preset* preset = freetvc::find_preset(argv[2]);
    if (!preset) {
        std::fprintf(stderr, "unknown preset '%s'\n", argv[2]);
        return 2;
    }
    std::string error;
    auto config = freetvc::load_preset(*preset, argv[1], error);
    auto firmware = config ? freetvc::read_file(argv[3], freetvc::SdDual::kFlashSize, error) : std::nullopt;
    if (!config || !firmware) {
        std::printf("SKIP: %s\n", error.c_str());
        return kExitSkip;
    }
    const fs::path dir = fs::temp_directory_path() / ("freetvc-sddual-rom-" + std::string(argv[2]));
    fs::remove_all(dir);
    fs::create_directories(dir);
    std::ofstream(dir / "README.TXT") << "freeTVC";
    const std::string card0 = (dir / "micro.img").string();
    const std::string card1 = (dir / "sdhc.img").string();
    check(freetvc::write_sd_image(card0, 32, {(dir / "README.TXT").string()}, error), "the FAT16 card: " + error);
    // 3 GiB, sparse: an SDHC card (CSD 2.0, block addresses) with FAT32
    check(freetvc::write_sd_image(card1, 3072, {(dir / "README.TXT").string()}, error), "the SDHC card: " + error);

    config->roms.cart = *firmware;
    config->roms.cart_type = freetvc::CartType::SdDual;
    freetvc::Machine m(*config);
    freetvc::SdImageFile micro;
    check(micro.open(card0, error), "open the FAT16 card: " + error);
    m.set_sd_image(0, &micro);
    Rig r{m, {}, 0};

    // SAVE from BASIC goes to the card through the firmware's cassette-class handler
    check(r.basic(), "BASIC");
    r.type(U"10 PRINT 6*7\nSAVE \"HELLO\"\n");
    r.frames(200);
    check(has(freetvc::sd_image_listing(card0), "/HELLO.CAS"), "SAVE wrote HELLO.CAS into the FAT volume");
    check(r.basic(), "BASIC after a power-on");
    r.type(U"LOAD \"HELLO\"\nRUN\n");
    check(r.wait_for(" 42\n", 400), "LOAD and RUN it from the card");

    // The VT-DOS CLI on both sockets: A: the microSD card, B: the SDHC card
    freetvc::SdImageFile sdhc;
    check(sdhc.open(card1, error) && freetvc::SdCard::high_capacity(sdhc), "open the SDHC card: " + error);
    m.set_sd_image(1, &sdhc);
    check(r.basic(), "BASIC with two cards");
    r.type(U"EXT 2\n");
    check(r.wait_for("VT-DOS CLI 1.2", 300), "EXT 2: the VT-DOS CLI");
    r.type(U"DIR\n");
    const bool listed = r.wait_for("HELLO    CAS", 300) && r.wait_for("README   TXT", 10);
    check(listed, "DIR of A: lists the card's files");
    if (!listed) {
        std::printf("%s", screen(m, 64).c_str());
    }
    r.wait_for("free", 300);
    r.frames(50);
    r.type(U"B:\n");
    r.frames(50);
    r.type(U"DIR\n");
    check(r.wait_for("Directory of B:", 300) && r.wait_for("1 file", 100), "DIR of B: reads the SDHC card");
    // FAT32: the firmware never reads FSInfo's free count and shows 0K (design/cards.md "SD Dual")
    check(screen(m).find("0K free") != std::string::npos, "FAT32 free space shows 0K (the firmware's limit)");
    m.set_sd_image(1, nullptr);

    if (argc == 6) {
        // The author's firmware updater, flashing the gamer-mode build, from the card's root
        const fs::path upd = dir / "FW";
        fs::create_directories(upd);
        fs::copy_file(fs::path(argv[4]) / "SDFLASH.CAS", upd / "SDFLASH.CAS");
        fs::copy_file(fs::path(argv[4]) / "vtdos_cr.bin", upd / "vtdos_cr.bin");
        const std::string card2 = (dir / "update.img").string();
        check(freetvc::write_sd_image(card2, 16, {(upd / "SDFLASH.CAS").string(), (upd / "vtdos_cr.bin").string()}, error),
              "the updater card: " + error);
        const auto gm = freetvc::read_file((upd / "vtdos_cr.bin").string(), freetvc::SdDual::kFlashSize, error).value_or(std::vector<std::uint8_t>{});
        freetvc::SdImageFile updater;
        for (const bool jumper : {false, true}) {
            config->roms.cart_bank = jumper ? 1 : 0;
            m.configure(*config);
            check(updater.open(card2, error), "open the updater card: " + error);
            m.set_sd_image(0, &updater);
            check(r.basic(), "BASIC for the updater");
            r.type(U"LOAD \"SDFLASH\"\nRUN\n");
            check(r.wait_for("Firmware friss", 300), "the updater's menu");
            r.type(U"1");
            const bool done = r.wait_for("Sikeres friss", 3000);
            const auto flash = m.memory().sddual()->flash();
            if (jumper) {
                check(done && std::equal(gm.begin(), gm.end(), flash.begin()), "with the jumper: flashed and verified");
                bool stubs = true;
                for (unsigned p = 5; p < 8; ++p) {
                    stubs = stubs && std::equal(flash.begin(), flash.begin() + 8, flash.begin() + p * freetvc::SdDual::kPage);
                }
                check(stubs, "pages 5-7 get the MOPS page-0 stub");
            } else {
                check(!done && screen(m).find("Hiba") != std::string::npos && std::equal(firmware->begin(), firmware->end(), flash.begin()),
                      "without the jumper the updater fails and the flash is unchanged");
            }
            m.set_sd_image(0, nullptr);
            updater.close();
        }
        config->roms.cart_bank = 0;
        m.configure(*config);

        // VT-DOS from the card with the author's system files: COMMAND.COM runs AUTOEXEC.BAT
        const fs::path sys(argv[5]);
        const std::string card3 = (dir / "system.img").string();
        check(freetvc::write_sd_image(card3, 16, {(sys / "AUTOEXEC.BAT").string(), (sys / "BDOS.SYS").string(), (sys / "COMMAND.COM").string()},
                                      error),
              "the system card: " + error);
        freetvc::SdImageFile system;
        check(system.open(card3, error), "open the system card: " + error);
        m.set_sd_image(0, &system);
        check(r.basic(), "BASIC for VT-DOS");
        r.type(U"EXT 2\n");
        r.wait_for("VT-DOS CLI", 300);
        r.type(U"DOS\n");
        check(r.wait_for("Enter VT-DOS", 100), "DOS asks");
        r.type(U"Y");
        r.frames(400);
        // COMMAND.COM, read from the card, is in RAM and has printed its banner (VT-DOS then draws its own console)
        const auto ram = m.memory().ram();
        const auto command = freetvc::read_file((sys / "COMMAND.COM").string(), 0x10000, error).value_or(std::vector<std::uint8_t>{});
        const bool loaded = command.size() >= 64 && std::search(ram.begin(), ram.end(), command.begin(), command.begin() + 64) != ram.end();
        check(loaded && screen(m).find("VT-DOS COMMAND") != std::string::npos, "VT-DOS boots COMMAND.COM from the card");
        m.set_sd_image(0, nullptr);
    }

    m.set_sd_image(0, nullptr);
    m.set_sd_image(1, nullptr);
    micro.close();
    sdhc.close();
    fs::remove_all(dir);
    if (failures != 0) {
        std::printf("screen at the end:\n%s", screen(m).c_str());
        return 1;
    }
    std::printf("sddual_rom_test %s: all passed\n", argv[2]);
    return 0;
}
