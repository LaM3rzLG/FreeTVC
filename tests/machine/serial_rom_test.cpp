// SPDX-License-Identifier: GPL-3.0-or-later
// The printer port and the serial card through the system ROMs: LPRINT and LLIST, an unplugged printer stopped
// with STOP, LIST #6, and SAVE #6 into LOAD #6 on a second machine; usage: serial_rom_test <rom dir> <preset>.
#include <cstdint>
#include <cstdio>
#include <deque>
#include <string>
#include <vector>

#include "core/cards/i8251.hpp"
#include "core/input/key_script.hpp"
#include "core/input/keyboard.hpp"
#include "core/machine/machine.hpp"
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

// The OS's ASCII copy of the text screen in U0, 24 rows of 64
std::string screen(const freetvc::Machine& m) {
    const auto u0 = m.memory().bank_bytes(freetvc::Bank::U0);
    std::string text;
    for (unsigned row = 0; row < 24; ++row) {
        std::string line(reinterpret_cast<const char*>(u0.data()) + 0x100 + row * 64, 64);
        line.erase(line.find_last_not_of(' ') + 1);
        text += line + "\n";
    }
    return text;
}

struct Printer final : freetvc::PrinterPort {
    std::string bytes;
    void printer_byte(std::uint64_t, std::uint8_t value) override { bytes.push_back(static_cast<char>(value)); }
};

struct Line final : freetvc::SerialLine {
    std::deque<std::uint8_t> incoming;
    std::string sent;
    bool connected() const override { return true; }
    bool waits_for_rts() const override { return true; }
    bool take(std::uint8_t& byte) override {
        if (incoming.empty()) {
            return false;
        }
        byte = incoming.front();
        incoming.pop_front();
        return true;
    }
    void send(std::uint8_t byte) override { sent.push_back(static_cast<char>(byte)); }
};

struct Rig {
    freetvc::Machine m;
    std::vector<freetvc::KeyEvent> keys;
    std::size_t next = 0;

    explicit Rig(const freetvc::MachineConfig& config) : m(config) {}
    void frames(std::uint64_t n) {
        for (std::uint64_t i = 0; i < n; ++i) {
            freetvc::apply_key_events(m, keys, next, m.frame_count());
            m.run({.max_ticks = kFrameTicks, .to_next_frame = true});
        }
    }
    void type(std::u32string_view text) {
        const auto typed = freetvc::type_text(text, m.frame_count());
        keys.insert(keys.end(), typed.events.begin(), typed.events.end());
        frames(typed.end_frame - m.frame_count());
    }
    // SPACE leaves the start-up logo, which waits for a key and takes it; editor input follows about 15 frames later
    void boot() {
        frames(380);
        type(U" ");
        frames(40);
    }
    bool wait_for(const std::string& text, std::uint64_t n) {
        for (std::uint64_t i = 0; i < n && screen(m).find(text) == std::string::npos; ++i) {
            frames(1);
        }
        return screen(m).find(text) != std::string::npos;
    }
};

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: serial_rom_test <rom dir> <preset>\n");
        return 2;
    }
    const freetvc::Preset* preset = freetvc::find_preset(argv[2]);
    if (!preset) {
        std::fprintf(stderr, "unknown preset '%s'\n", argv[2]);
        return 2;
    }
    std::string error;
    auto config = freetvc::load_preset(*preset, argv[1], error);
    if (!config) {
        std::printf("SKIP: %s\n", error.c_str());
        return kExitSkip;
    }
    config->cards[freetvc::kSerialSlot] = {freetvc::CardKind::Serial, {}};

    // Printer: LLIST and LPRINT through PAR_CHOUT, Hungarian letters as the TVC's codes
    {
        Rig r(*config);
        Printer printer;
        r.m.set_printer(&printer);
        r.boot();
        r.type(U"10 PRINT 1\nLLIST\nLPRINT \"ÁRVÍZTŰRŐ\";42\n");
        r.frames(60);
        check(printer.bytes == "  10 PRINT 1\r\n\x80RV\x82ZT\x88R\x85 42 \r\n", "LLIST and LPRINT reach the printer");

        // Unplugged: the first character goes out, the second waits until STOP (CTRL+ESC)
        r.m.set_printer(nullptr);
        r.type(U"LPRINT \"AB\"\n");
        r.frames(100);
        check(r.m.pc() >= 0xC000 && (r.m.io_read(0x59) & 0x80) == 0, "unplugged: LPRINT waits in the ROM with ACK low");
        r.keys.push_back({r.m.frame_count(), *freetvc::find_key("CTRL"), true});
        r.keys.push_back({r.m.frame_count(), *freetvc::find_key("ESC"), true});
        r.keys.push_back({r.m.frame_count() + 10, *freetvc::find_key("CTRL"), false});
        r.keys.push_back({r.m.frame_count() + 10, *freetvc::find_key("ESC"), false});
        check(r.wait_for("STOP", 100), "STOP breaks the wait");
        check(printer.bytes.find("AB") == std::string::npos, "nothing reached the unplugged printer");
    }

    // Serial: LIST #6 out, then SAVE #6 into a second machine's LOAD #6
    std::string saved;
    {
        Rig r(*config);
        Line line;
        r.m.set_serial_line(&line);
        r.boot();
        r.type(U"10 PRINT \"LINK\";6*7\nLIST #6\n");
        r.frames(60);
        check(line.sent == "  10 PRINT \"LINK\";6*7\r\n", "LIST #6 goes out through the 8251A: '" + line.sent + "'");
        line.sent.clear();
        r.type(U"SAVE #6\n");
        r.frames(120);
        saved = line.sent;
        check(saved.size() > 20 && saved.find("LINK") != std::string::npos, "SAVE #6 sends the program");
    }
    {
        Rig r(*config);
        Line line;
        line.incoming.assign(saved.begin(), saved.end());
        r.m.set_serial_line(&line);
        r.boot();
        r.type(U"LOAD #6\n");
        r.frames(150);
        check(line.incoming.empty(), "LOAD #6 took every byte");
        r.type(U"RUN\n");
        check(r.wait_for("LINK 42", 100), "the loaded program runs");
    }

    if (failures != 0) {
        return 1;
    }
    std::printf("serial_rom_test %s: all passed\n", argv[2]);
    return 0;
}
