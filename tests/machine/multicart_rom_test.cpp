// SPDX-License-Identifier: GPL-3.0-or-later
// The MultiCart v2 firmware 2.1.14 on real ROMs: its menu with the chips' IDs and the RAM size, a cart started from
// it, the reset-on-read latch both ways, SAVE through EXT 0 into the flash, the saved program run, a delete;
// usage: multicart_rom_test <rom dir> <preset> <multicart_v2.1.14en.bin>.
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/bus/multicart.hpp"
#include "core/input/key_script.hpp"
#include "core/machine/machine.hpp"
#include "frontend/common/multicart_image.hpp"
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

// The OS's copy of the text screen in U0 (tvc-ram.sym): 24 rows of 64, or of 80 in the firmware's 80-column mode
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

std::string screens(const freetvc::Machine& m) { return screen(m, 64) + screen(m, 80); }

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
    bool wait_for(const std::string& text, std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            if (screens(m).find(text) != std::string::npos) {
                return true;
            }
            frame();
        }
        return screens(m).find(text) != std::string::npos;
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
};

// The cart's mark in U1: LD HL,1234h; LD (5000h),HL; JR $
std::uint16_t mark(const freetvc::Machine& m) {
    const auto u1 = m.memory().bank_bytes(freetvc::Bank::U1);
    return static_cast<std::uint16_t>(u1[0x1000] | (u1[0x1001] << 8));
}

unsigned used_blocks(const std::vector<std::uint8_t>& flash) {
    unsigned n = 0;
    for (std::size_t i = 0x3000; i < 0x3200; ++i) {
        for (unsigned b = 0; b < 8; ++b) {
            n += (flash[i] >> b) & 1u;
        }
    }
    return n;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: multicart_rom_test <rom dir> <preset> <multicart_v2.1.14en.bin>\n");
        return 2;
    }
    const freetvc::Preset* preset = freetvc::find_preset(argv[2]);
    if (!preset) {
        std::fprintf(stderr, "unknown preset '%s'\n", argv[2]);
        return 2;
    }
    std::string error;
    auto config = freetvc::load_preset(*preset, argv[1], error);
    auto firmware = config ? freetvc::read_file(argv[3], 0x4000, error) : std::nullopt;
    if (!config || !firmware) {
        std::printf("SKIP: %s\n", error.c_str());
        return kExitSkip;
    }
    std::vector<std::uint8_t> cart(0x1000, 0xFF);
    const std::uint8_t code[] = {'M', 'O', 'P', 'S', 0x21, 0x34, 0x12, 0x22, 0x00, 0x50, 0x18, 0xFE};
    std::copy(std::begin(code), std::end(code), cart.begin());
    auto image = freetvc::build_multicart_image(*firmware, {{"MARK", cart, false}}, error);
    if (!image) {
        std::printf("FAIL: the image: %s\n", error.c_str());
        return 1;
    }
    freetvc::MultiCartFlash flash;
    flash.bytes = std::move(*image);
    config->roms.cart_type = freetvc::CartType::MultiCart;
    freetvc::Machine m(*config);
    m.set_multicart_flash(&flash);
    const freetvc::MultiCart& board = *m.memory().multicart();
    Rig r{m, {}, 0};

    check(r.wait_for("MultiCart ROM selection tool v2.1.14e", 1500), "the firmware's menu");
    check(r.wait_for("Flash detected: 512kB,512kB", 50), "both SST39SF040 found by their IDs");
    check(r.wait_for("RAM detected: 128kB", 50), "128 KiB RAM found");
    check(r.wait_for("MARK", 50) && r.wait_for("CART", 10), "the cart is listed");
    const std::string menu = screens(m);

    // A cart from the menu; the reset button brings the menu back (RESENA: the ROM's MOPS read clears the register)
    r.press("Y");
    r.press(freetvc::kKeyReturn);
    r.frames(100);
    check(mark(m) == 0x1234, "the cart runs from bank 1");
    check(board.reg() == 1 && board.reset_on_read(), "register: bank 1, reset on read");
    m.poke(0x5000, 0);
    m.poke(0x5001, 0);
    m.reset();
    r.frames(600);  // the reset keeps RAM, so the old menu text is still there: let the ROM boot first
    check(r.wait_for("MultiCart ROM selection tool", 900) && mark(m) == 0 && board.reg() == 0,
          "reset: the menu again (mark " + std::to_string(mark(m)) + ", register " + std::to_string(board.reg()) + ")");

    // SPACE: no reset on read, so the reset button starts the cart again
    r.press(freetvc::kKeySpace);
    check(r.wait_for("Reset MultiCart on TVC reset: NO", 50), "SPACE switches the reset off");
    r.press("Y");
    r.press(freetvc::kKeyReturn);
    r.frames(100);
    check(mark(m) == 0x1234 && board.reg() == 1 && !board.reset_on_read(), "the cart runs, written through C002h");
    m.poke(0x5000, 0);
    m.poke(0x5001, 0);
    m.reset();
    r.frames(400);
    check(mark(m) == 0x1234 && board.reg() == 1, "reset: the cart again");

    // SAVE from BASIC through EXT 0 into the flash
    m.power_on();
    const unsigned used = used_blocks(flash.bytes);
    check(r.wait_for("MultiCart ROM selection tool", 1500), "power-on: the menu");
    r.frames(150);  // the list and its key loop come after the header
    r.press(freetvc::kKeyReturn);
    for (int i = 0; i < 3 && !r.wait_for("bytes free", 300); ++i) {
        r.press(freetvc::kKeySpace);  // the start-up logo, where the ROM has one
    }
    check(r.wait_for("bytes free", 300), "EXIT to BASIC");
    r.type(U"10 PRINT 6*7\nEXT 0\n");
    check(r.wait_for("What do you want to do?", 300), "EXT 0 is the firmware's save menu");
    r.type(U"1\n");
    check(r.wait_for("Enter file", 300), "it asks for a name");
    r.type(U"HELLO\n");
    check(r.wait_for("SST39SF0x0 found", 300), "the save finds the SST chips");
    r.frames(200);
    auto dir = freetvc::multicart_directory(flash.bytes);
    check(flash.dirty && dir.size() == 2 && dir[1].name == "HELLO" && dir[1].type == freetvc::MultiCartEntry::Cas,
          "HELLO is in the flash's directory");
    check(used_blocks(flash.bytes) == used + 1, "one block allocated");

    // The saved program from the menu
    m.power_on();
    check(r.wait_for("HELLO", 1500), "power-on: HELLO listed");
    r.frames(50);
    r.press("Y");
    r.press("Y");
    r.press(freetvc::kKeyReturn);
    if (config->roms.sys.size() > 0x1A0C && config->roms.sys[0x1A0C] == 0x5B) {
        // SYS 1.2: CONFIGURE_EXT0 returns with Z set, so JP NZ at CA4Eh falls into the 2.x exit, which clears the program
        for (int i = 0; i < 3 && !r.wait_for("bytes free", 300); ++i) {
            r.press(freetvc::kKeySpace);
        }
        check(r.wait_for("bytes free", 300) && screens(m).find(" 42\n") == std::string::npos,
              "SYS 1.2: BASIC comes up without the program (firmware 2.1.14, cards.md)");
    } else {
        check(r.wait_for(" 42\n", 600), "HELLO loads and runs");
    }

    // Delete it: the firmware's BAM update clears bit d-1 for block d, so this block (bit 0 of its byte) stays allocated
    m.power_on();
    check(r.wait_for("HELLO", 1500), "power-on: HELLO still listed");
    r.frames(50);
    r.press("Y");
    r.press("Y");
    r.press(freetvc::kKeyDel);
    check(r.wait_for("Y/N", 100), "delete asks");
    r.press("Y");
    check(r.wait_for("Done.", 600), "the delete writes the flash");
    dir = freetvc::multicart_directory(flash.bytes);
    check(dir.size() == 1 && dir[0].name == "MARK", "HELLO is gone from the directory");
    check(used_blocks(flash.bytes) == used + 1, "its block stays allocated (the firmware's off-by-one, cards.md)");

    if (failures != 0) {
        std::printf("menu:\n%sscreen at the end:\n%s", menu.c_str(), screens(m).c_str());
        return 1;
    }
    std::printf("multicart_rom_test %s: all passed\n", argv[2]);
    return 0;
}
