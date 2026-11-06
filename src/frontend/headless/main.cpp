// SPDX-License-Identifier: GPL-3.0-or-later
// Headless runner: no window, sound only to a WAV file; boots a preset or loads raw binaries, runs a bounded budget, prints the stop state.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "core/cards/card256.hpp"
#include "core/cards/multichip.hpp"
#include "core/cards/nanosd.hpp"
#include "core/input/key_script.hpp"
#include "core/input/keyboard.hpp"
#include "core/machine/machine.hpp"
#include "core/version.hpp"
#include "debug/debugger.hpp"
#include "frontend/common/host_tape.hpp"
#include "frontend/common/multicart_image.hpp"
#include "frontend/common/sd_image.hpp"
#include "frontend/common/presets.hpp"
#include "frontend/common/rom_fetch.hpp"
#include "frontend/common/settings.hpp"
#include "frontend/common/wav.hpp"
#include "remote/mcp.hpp"
#include "remote/dzrp.hpp"
#include "remote/dzrp_host.hpp"
#include "remote/gdb.hpp"
#include "remote/gdb_host.hpp"
#include "remote/mcp_host.hpp"
#include "remote/monitor.hpp"
#include "remote/monitor_hub.hpp"

namespace {

constexpr int kExitUsage = 2;
constexpr int kExitError = 1;

void print_usage(std::FILE* out) {
    std::fprintf(out,
                 "usage: freetvc-headless [options]\n"
                 "  --preset NAME       machine + ROM set:");
    for (const freetvc::Preset& preset : freetvc::kPresets) {
        std::fprintf(out, " %.*s", static_cast<int>(preset.name.size()), preset.name.data());
    }
    std::fprintf(out,
                 "\n"
                 "  --rom-dir DIR       directory with the preset's ROM files; searched first, then $FREETVC_ROM_DIR,\n"
                 "                      roms/ next to the program and roms/ in the user data folder (files are also\n"
                 "                      recognised by CRC-32 under any name)\n"
                 "  --fetch-roms        download the TVC and card ROMs (data/roms.txt: tvc.homeserver.hu, GitHub, the Internet Archive) with\n"
                 "                      the system's curl, checked by SHA-256, into --rom-dir or roms/ in the user data folder; then exit\n"
                 "  --model NAME        32k, 32k+hbm, 64k or 64k+ (default 64k+, or the preset's)\n"
                 "  --sys FILE          SYS ROM image (16 KiB); --ext FILE: EXT ROM (4 or 8 KiB)\n"
                 "  --cart FILE         cartridge image: a MultiCart v2 image by its firmware's header (its flash is written\n"
                 "                      back to FILE), SD Dual firmware by its header, else up to 16 KiB plain, up to\n"
                 "                      64 KiB KiloCart, up to 1 MiB MegaCart\n"
                 "  --cart-type TYPE    plain, kilocart, megacart, multicart or sddual instead of those rules\n"
                 "  --multicart-build OUT  write a new MultiCart v2 image OUT from the --multicart-add files, then exit\n"
                 "  --multicart-add FILE   a .cas program or a MOPS cartridge for it, in menu order (repeatable)\n"
                 "  --multicart-lang L  its firmware's language: en (default), hu or es (multicart-L.rom, found like the\n"
                 "                      system ROMs)\n"
                 "  --cart-bank N       a plain board's bank switches: the 16 KiB window of a larger image (default 0)\n"
                 "  --sddual            Sebestyén Pál's SD Dual cartridge (two SD sockets, VT-DOS); its firmware sddual.rom\n"
                 "                      is found like the system ROMs (--sddual-gm: sddual-gm.rom, the gamer-mode build)\n"
                 "  --sddual-rom FILE   the SD Dual with this firmware image (up to 64 KiB)\n"
                 "  --sddual-flash-write  the SD Dual's flash write jumper on (its firmware updater can then write)\n"
                 "  --sd FILE           a raw SD card image (.img, whole card) in the microSD socket, written in place; the\n"
                 "                      SD Dual included unless --cart or --snapshot brings the cartridge\n"
                 "  --sd2 FILE          the same for the SD socket (a read-only file sets its write-protect switch)\n"
                 "  --sd-build OUT      write a new SD card image OUT (one FAT partition) holding the --sd-add files and\n"
                 "                      folders, list it, then exit; --sd-size MIB sets its size (default 64)\n"
                 "  --sd-add PATH       a file or a folder (with its contents) for its root (repeatable)\n"
                 "  --sd-list FILE      list the files in an SD card image's FAT volume, then exit\n"
                 "  --disk-card         HBF disk card in slot 0 (its ROM hbf.rom is found like the system ROMs); it\n"
                 "                      takes over LOAD and SAVE, so the host cassette and the tape then serve nothing\n"
                 "  --disk-rom ROM      the disk card's EPROM: 1.1 (hbf.rom, VT-DOS 1.1, the default), 1.2 (hbf12.rom,\n"
                 "                      VT-DOS 1.2), both found like the system ROMs, or a 16 KiB image file (UPM, ...)\n"
                 "  --disk [A-D:]FILE   raw disk image (.dsk/.img, 360 or 720 KiB) into a drive, the card included;\n"
                 "                      repeatable; written sectors go back to FILE (a read-only file is write-protected)\n"
                 "  --gamecard [SLOT]   dikdom's GameCard (SN76489 sound, joysticks 3/4) in SLOT 0-3 (default 1); its EPROM\n"
                 "                      gamecard.rom is found like the system ROMs, without it there is no boot menu\n"
                 "  --gamecard-rom FILE the GameCard with this 64 KiB EPROM image\n"
                 "  --multichip [SLOT]  the Multichip sound card (SN76489, SAA1099, YM3812) in SLOT 0-3 (default 2); its\n"
                 "                      EEPROM multichip.rom is found like the system ROMs, without it the chips still play\n"
                 "  --multichip-rom F   the Multichip card with this 32 KiB EEPROM image\n"
                 "  --sid-card [MODEL]  the SID card at ports E0h-FFh, outside the slots (WinTVC's layout): a MOS 6581\n"
                 "                      (default) or 8580 on reSID\n"
                 "  --sid-clock HZ      its clock (default 985248, a PAL C64's)\n"
                 "  --2dfx [SLOT]       the 2dfx graphics card (ports F8h/F9h) in SLOT 0-3 (default 2); not with the\n"
                 "                      TVC256k++ (both drive /EXTC) or the SID card\n"
                 "  --nanosd [SLOT]     the NanoSD IO card (an SD card through an Arduino Nano) in SLOT 0-3 (default 3); its\n"
                 "                      EEPROM nanosd.rom is found like the system ROMs; it takes over LOAD and SAVE\n"
                 "  --nanosd-rom FILE   the NanoSD with this 64 KiB EEPROM image\n"
                 "  --nanosd-sd DIR     a folder as its SD card (written to), the card included unless --snapshot brings\n"
                 "                      one; without it the socket is empty\n"
                 "  --printer FILE      plug in a printer printing to FILE: raw bytes, or UTF-8 text when FILE ends in .txt\n"
                 "                      (appended; give it twice for both); \"on\" keeps the output in memory only.\n"
                 "                      Without it no printer is plugged in, so an LPRINT waits after its first character\n"
                 "  --serial-card [SLOT] Videoton's RS-232 card (an 8251A) in SLOT 0-3 (default 1)\n"
                 "  --serial PEER       its cable: off (default), on (this program is the peer: --serial-send,\n"
                 "                      --serial-capture, the monitor and MCP), listen[:PORT] (a TCP client on\n"
                 "                      127.0.0.1, default port 6585) or connect:PORT; brings the card\n"
                 "  --serial-flow F     rts (default: the peer sends only while the TVC's RTS is on, the factory cable)\n"
                 "                      or none\n"
                 "  --serial-send FILE  queue FILE's bytes for the TVC to receive (--serial on unless given)\n"
                 "  --serial-capture FILE  append everything the TVC sends to FILE (--serial on unless given)\n"
                 "  --card256 [SLOT]    dikdom's TVC256k++ (fast RAM over U2/U3, PSRAM, registers) in SLOT 0-3 (default 0,\n"
                 "                      else the first free slot); its ROM card256.rom is found like the system ROMs\n"
                 "  --card256-rom FILE  the TVC256k++ with this 12 KiB ROM image\n"
                 "  --card256-usb WHAT  the TVC256k++'s USB port: a folder as the pendrive (drive 0, written to), or\n"
                 "                      mouse (the monitor's card256 mouse moves it)\n"
                 "  --card256-flash F   its flash drive (drive 2): a littlefs image or a folder packed into one; without\n"
                 "                      it card256-flash.bin is found like the system ROMs\n"
                 "  --port02 VALUE      write the page register before starting (B0 = all RAM)\n"
                 "  --load FILE@ADDR    copy a raw binary into RAM U0-U3 at ADDR (repeatable)\n"
                 "  --pc ADDR           start execution at ADDR (default: the reset address 0000)\n"
                 "  --tstates N         T-state budget (default 62800, one frame)\n"
                 "  --frames N          T-state budget of N frames (N x 62800)\n"
                 "  --instructions N    stop after N instructions\n"
                 "  --key KEY[+KEY]@F[:H]  hold keys from frame F for H frames (default 5); KEY = ROW:COL or a name\n"
                 "                      (A, 5, Á, SPACE, RETURN, SHIFT, CTRL, DEL, UP, ...); repeatable\n"
                 "  --joy N:DIR[+DIR]@F[:H]  joystick N (1 = row 8, 2 = row 9, 3/4 = the GameCard's): UP DOWN LEFT RIGHT FIRE ACCEL\n"
                 "  --type TEXT@F       type TEXT from frame F, LOCK off; \\n = RETURN (repeatable)\n"
                 "  --type-timing H:G[:R]  typing: frames held, between keys, after RETURN (default 2:2:25)\n"
                 "  --cas FILE          attach a CAS file as the host cassette: BASIC's LOAD reads it\n"
                 "  --cas-dir DIR       cassette folder: LOAD \"NAME\" reads and SAVE \"NAME\" writes DIR/NAME.CAS\n"
                 "                      (without it, the attached file's folder is read only)\n"
                 "  --autostart FILE    attach FILE, cold start, LOAD and RUN it at full speed; frame numbers and\n"
                 "                      budgets then count from the program's start (.wav and .ttp go through the deck;\n"
                 "                      a .dsk or .img goes into A: with the disk card, and its first CAS file is loaded)\n"
                 "  --tape FILE         put FILE (.wav, .ttp, or .cas played as a tape) in the tape deck and press PLAY\n"
                 "  --autostart-tape FILE  as --autostart, but through the tape deck, .cas included\n"
                 "  --tape-record FILE  record the tape output into FILE (WAV) from the start\n"
                 "  --screenshot FILE   write the last complete frame as a binary PPM (800 x 314)\n"
                 "  --snapshot FILE     start from a TVCS snapshot; its board and ROM set win over --preset (ROMs found\n"
                 "                      by CRC-32), the tape and cassette come back by path; budgets count from there\n"
                 "  --save-snapshot FILE  write a TVCS snapshot at the end of the run\n"
                 "  --wav FILE          write the whole run's sound as a 48 kHz 16-bit mono WAV\n"
                 "  --monitor           text monitor on stdin/stdout, starting in the monitor (help lists the commands)\n"
                 "  --remote-monitor    text monitor on 127.0.0.1:6580 (--monitor-port N for another port, 0 = any)\n"
                 "  --moncommands FILE  run monitor commands from FILE first; then the machine runs unless the script ran it\n"
                 "                      With a monitor the run lasts until 'quit', a stop with nobody left to answer, or\n"
                 "                      an explicit --tstates/--frames budget; --instructions is ignored.\n"
                 "  --mcp-stdio         MCP server on stdin/stdout (logs on stderr); the machine waits for tool calls\n"
                 "                      and the run ends when stdin closes\n"
                 "  --mcp-http          MCP server on http://127.0.0.1:6584/mcp (--mcp-port N for another port)\n"
                 "  --mcp-token TOKEN   bearer token for --mcp-http (default: a random one, printed on stderr)\n"
                 "  --mcp-allow-monitor let MCP clients run text monitor commands (monitor_command)\n"
                 "  --dzrp              DZRP server for DeZog on 127.0.0.1:6582 (--dzrp-port N for another port, 0 = any)\n"
                 "  --dzrp-banks        report the TVC's banks (needs DeZog with dzrp.customMemory, tools/dezog/)\n"
                 "  --gdb               GDB stub on 127.0.0.1:6583 (--gdb-port N for another port, 0 = any)\n"
                 "  --version           print the version\n"
                 "  --help              this text\n"
                 "ADDR and VALUE are hexadecimal (8000, 0x8000 or 8000h); N is decimal.\n");
}

std::optional<std::uint16_t> parse_address(std::string_view text) {
    if (text.size() > 2 && (text.substr(0, 2) == "0x" || text.substr(0, 2) == "0X")) {
        text.remove_prefix(2);
    } else if (!text.empty() && (text.back() == 'h' || text.back() == 'H')) {
        text.remove_suffix(1);
    }
    if (text.empty() || text.size() > 4) {
        return std::nullopt;
    }
    unsigned value = 0;
    for (const char c : text) {
        value <<= 4;
        if (c >= '0' && c <= '9') {
            value |= static_cast<unsigned>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            value |= static_cast<unsigned>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            value |= static_cast<unsigned>(c - 'A' + 10);
        } else {
            return std::nullopt;
        }
    }
    return static_cast<std::uint16_t>(value);
}

std::optional<std::uint64_t> parse_count(std::string_view text) {
    if (text.empty() || text.size() > 18) {
        return std::nullopt;
    }
    std::uint64_t value = 0;
    for (const char c : text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        value = value * 10 + static_cast<std::uint64_t>(c - '0');
    }
    return value;
}

struct LoadItem {
    std::string path;
    std::uint16_t address;
};

constexpr std::uint64_t kKeyHoldFrames = 5;

// "@F" or "@F:HOLD" at the end of a spec: the frame and the hold time
std::optional<std::pair<std::uint64_t, std::uint64_t>> parse_when(std::string_view spec) {
    const auto colon = spec.find(':');
    const auto frame = parse_count(spec.substr(0, colon));
    const auto hold = colon == std::string_view::npos ? std::optional<std::uint64_t>(kKeyHoldFrames)
                                                       : parse_count(spec.substr(colon + 1));
    if (!frame || !hold || *hold == 0) {
        return std::nullopt;
    }
    return std::pair{*frame, *hold};
}

// One matrix key: ROW:COL or a name from core/input/keyboard.hpp
std::optional<freetvc::MatrixKey> parse_matrix_key(std::string_view name) {
    const auto colon = name.find(':');
    if (colon != std::string_view::npos) {
        const auto row = parse_count(name.substr(0, colon));
        const auto column = parse_count(name.substr(colon + 1));
        if (!row || !column || *row >= freetvc::kMatrixRows || *column > 7) {
            return std::nullopt;
        }
        return freetvc::MatrixKey{static_cast<std::uint8_t>(*row), static_cast<std::uint8_t>(*column)};
    }
    return freetvc::find_key(name);
}

// --key KEY[+KEY...]@F[:HOLD]
bool add_chord(std::string_view spec, std::vector<freetvc::KeyEvent>& events) {
    const auto at = spec.rfind('@');
    const auto when = at == std::string_view::npos || at == 0 ? std::nullopt : parse_when(spec.substr(at + 1));
    if (!when) {
        return false;
    }
    std::string_view chord = spec.substr(0, at);
    std::vector<freetvc::MatrixKey> keys;
    while (!chord.empty()) {
        const auto plus = chord.find('+');
        const auto key = parse_matrix_key(chord.substr(0, plus));
        if (!key) {
            return false;
        }
        keys.push_back(*key);
        chord = plus == std::string_view::npos ? std::string_view{} : chord.substr(plus + 1);
    }
    for (const freetvc::MatrixKey key : keys) {
        events.push_back({when->first, key, true});
        events.push_back({when->first + when->second, key, false});
    }
    return !keys.empty();
}

// --joy N:DIR[+DIR...]@F[:HOLD] with DIR = UP DOWN LEFT RIGHT FIRE ACCEL
bool add_joystick(std::string_view spec, std::vector<freetvc::KeyEvent>& events) {
    if (spec.size() < 3 || spec[0] < '1' || spec[0] > '4' || spec[1] != ':') {
        return false;
    }
    const auto joystick = static_cast<unsigned>(spec[0] - '0');
    std::string translated;
    std::string_view lines = spec.substr(2);
    const auto at = lines.rfind('@');
    if (at == std::string_view::npos) {
        return false;
    }
    for (std::string_view rest = lines.substr(0, at); !rest.empty();) {
        const auto plus = rest.find('+');
        const auto key = freetvc::find_key(rest.substr(0, plus));
        if (!key || key->row != 8 || key->column == 0) {
            return false;
        }
        const auto line = freetvc::joystick_key(joystick, static_cast<freetvc::JoyLine>(key->column));
        translated += (translated.empty() ? "" : "+") + std::to_string(line.row) + ":" + std::to_string(line.column);
        rest = plus == std::string_view::npos ? std::string_view{} : rest.substr(plus + 1);
    }
    return add_chord(translated + std::string(lines.substr(at)), events);
}

// --type TEXT@F: "\n" (or a real newline) is RETURN, "\\" a backslash
bool add_text(std::string_view spec, freetvc::TypingTiming timing, std::vector<freetvc::KeyEvent>& events) {
    const auto at = spec.rfind('@');
    const auto frame = at == std::string_view::npos ? std::nullopt : parse_count(spec.substr(at + 1));
    if (!frame) {
        return false;
    }
    std::string text;
    for (std::size_t i = 0; i < at; ++i) {
        if (spec[i] == '\\' && i + 1 < at && (spec[i + 1] == 'n' || spec[i + 1] == '\\')) {
            text.push_back(spec[++i] == 'n' ? '\n' : '\\');
        } else {
            text.push_back(spec[i]);
        }
    }
    const auto chars = freetvc::decode_utf8(text);
    if (!chars) {
        return false;
    }
    const freetvc::TypedText typed = freetvc::type_text(*chars, *frame, timing);
    if (typed.unsupported != std::u32string::npos) {
        std::fprintf(stderr, "freetvc-headless: no TVC key types character %zu of '%.*s'\n", typed.unsupported + 1,
                     static_cast<int>(at), spec.data());
        return false;
    }
    events.insert(events.end(), typed.events.begin(), typed.events.end());
    return true;
}

const char* reason_name(freetvc::StopReason reason) {
    switch (reason) {
    case freetvc::StopReason::TickBudget:
        return "budget";
    case freetvc::StopReason::InstructionCount:
        return "instructions";
    case freetvc::StopReason::Debugger:
        return "debugger";
    }
    return "?";
}

bool write_ppm(const std::string& path, const freetvc::Framebuffer& frame) {
    std::FILE* out = std::fopen(path.c_str(), "wb");
    if (!out) {
        return false;
    }
    std::fprintf(out, "P6\n%d %d\n255\n", freetvc::Framebuffer::kWidth, freetvc::Framebuffer::kHeight);
    std::vector<unsigned char> rgb;
    rgb.reserve(frame.pixels.size() * 3);
    for (const std::uint8_t index : frame.pixels) {
        const std::uint32_t colour = freetvc::kRgbPalette[index < freetvc::Framebuffer::kColours ? index : 0];
        rgb.push_back(static_cast<unsigned char>(colour >> 16));
        rgb.push_back(static_cast<unsigned char>(colour >> 8));
        rgb.push_back(static_cast<unsigned char>(colour));
    }
    const bool ok = std::fwrite(rgb.data(), 1, rgb.size(), out) == rgb.size();
    return std::fclose(out) == 0 && ok;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<LoadItem> loads;
    std::vector<freetvc::KeyEvent> key_events;
    std::vector<std::string_view> texts;
    freetvc::TypingTiming typing;
    std::optional<std::uint16_t> start_pc;
    std::optional<std::uint8_t> port02;
    std::uint64_t tstates = freetvc::kTStatesPerFrame;
    std::uint64_t instructions = 0;
    const freetvc::Preset* preset = nullptr;
    std::optional<freetvc::Model> model;
    std::string rom_dir_option;
    std::string sys_path;
    std::string ext_path;
    std::string cart_path;
    std::string cart_type;
    unsigned cart_bank = 0;
    std::string multicart_build;
    std::vector<std::string> multicart_files;
    std::string multicart_lang;
    bool sddual = false;
    bool sddual_gamer = false;
    std::string sddual_rom_path;
    bool sddual_flash_write = false;
    std::array<std::string, 2> sd_paths;
    std::string sd_build;
    std::vector<std::string> sd_files;
    std::uint64_t sd_mib = 64;
    std::string sd_list;
    bool disk_card = false;
    std::string disk_rom_path;
    std::optional<unsigned> gamecard_slot;
    std::string gamecard_rom_path;
    std::optional<unsigned> multichip_slot;
    std::string multichip_rom_path;
    freetvc::SidCardConfig sid;
    std::optional<unsigned> nanosd_slot;
    std::optional<unsigned> twodfx_slot;
    std::string printer_raw, printer_text;
    bool printer_on = false;
    std::optional<unsigned> serial_slot;
    std::string serial_peer, serial_flow, serial_send, serial_capture;
    std::string nanosd_rom_path;
    std::string nanosd_sd;
    std::optional<unsigned> card256_slot;  // kCardSlots: the default slot
    std::string card256_rom_path;
    std::string card256_usb;
    std::string card256_flash;
    std::vector<std::pair<unsigned, std::string>> disks;
    std::string screenshot_path;
    std::string snapshot_path;
    std::string save_snapshot_path;
    std::string wav_path;
    std::string cas_path;
    std::string cas_dir;
    std::string autostart_path;
    bool autostart_tape = false;
    std::string tape_path;
    std::string tape_record_path;
    bool budget_given = false;
    bool console_monitor = false;
    bool remote_monitor = false;
    std::uint16_t monitor_port = freetvc::remote::kMonitorPort;
    std::string moncommands;
    bool mcp_stdio = false;
    bool mcp_http = false;
    std::uint16_t mcp_port = freetvc::remote::kMcpPort;
    std::string mcp_token;
    bool mcp_allow_monitor = false;
    bool dzrp = false;
    std::uint16_t dzrp_port = freetvc::remote::kDzrpPort;
    auto dzrp_memory = freetvc::remote::DzrpMemory::Zx48k;
    bool gdb = false;
    bool fetch_roms = false;
    std::uint16_t gdb_port = freetvc::remote::kGdbPort;

    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        const bool has_value = i + 1 < argc;
        if (arg == "--help") {
            print_usage(stdout);
            return 0;
        }
        if (arg == "--version") {
            std::printf("freeTVC %s\n", freetvc::kVersion);
            return 0;
        }
        if (arg == "--fetch-roms") {
            fetch_roms = true;
            continue;
        }
        if (arg == "--load" && has_value) {
            const std::string_view spec = argv[++i];
            const auto at = spec.rfind('@');
            const auto address = at == std::string_view::npos ? std::nullopt : parse_address(spec.substr(at + 1));
            if (!address || at == 0) {
                std::fprintf(stderr, "freetvc-headless: --load expects FILE@ADDR, got '%s'\n", argv[i]);
                return kExitUsage;
            }
            loads.push_back({std::string(spec.substr(0, at)), *address});
        } else if ((arg == "--pc" || arg == "--port02") && has_value) {
            const auto value = parse_address(argv[++i]);
            if (!value || (arg == "--port02" && *value > 0xFF)) {
                std::fprintf(stderr, "freetvc-headless: bad value '%s'\n", argv[i]);
                return kExitUsage;
            }
            if (arg == "--pc") {
                start_pc = *value;
            } else {
                port02 = static_cast<std::uint8_t>(*value);
            }
        } else if ((arg == "--tstates" || arg == "--frames" || arg == "--instructions") && has_value) {
            const auto count = parse_count(argv[++i]);
            if (!count) {
                std::fprintf(stderr, "freetvc-headless: bad count '%s'\n", argv[i]);
                return kExitUsage;
            }
            if (arg == "--instructions") {
                instructions = *count;
            } else {
                tstates = arg == "--frames" ? *count * freetvc::kTStatesPerFrame : *count;
                budget_given = true;
            }
        } else if ((arg == "--key" || arg == "--joy") && has_value) {
            ++i;
            if (!(arg == "--key" ? add_chord(argv[i], key_events) : add_joystick(argv[i], key_events))) {
                std::fprintf(stderr, "freetvc-headless: bad %s '%s' (see --help)\n", argv[i - 1], argv[i]);
                return kExitUsage;
            }
        } else if (arg == "--type" && has_value) {
            texts.emplace_back(argv[++i]);
        } else if (arg == "--type-timing" && has_value) {
            std::string_view rest = argv[++i];
            std::uint64_t values[3] = {0, 0, typing.after_return};
            std::size_t count = 0;
            bool ok = true;
            while (ok) {
                const auto colon = rest.find(':');
                const auto value = parse_count(rest.substr(0, colon));
                ok = value.has_value() && *value <= 1000 && count < 3;
                if (ok) {
                    values[count++] = *value;
                }
                if (colon == std::string_view::npos) {
                    break;
                }
                rest.remove_prefix(colon + 1);
            }
            if (!ok || count < 2 || values[0] == 0) {
                std::fprintf(stderr, "freetvc-headless: --type-timing expects HOLD:GAP[:AFTER_RETURN], got '%s'\n", argv[i]);
                return kExitUsage;
            }
            typing = {static_cast<unsigned>(values[0]), static_cast<unsigned>(values[1]), static_cast<unsigned>(values[2])};
        } else if (arg == "--preset" && has_value) {
            preset = freetvc::find_preset(argv[++i]);
            if (!preset) {
                std::fprintf(stderr, "freetvc-headless: unknown preset '%s'\n", argv[i]);
                return kExitUsage;
            }
        } else if (arg == "--model" && has_value) {
            model = freetvc::parse_model(argv[++i]);
            if (!model) {
                std::fprintf(stderr, "freetvc-headless: unknown model '%s'\n", argv[i]);
                return kExitUsage;
            }
        } else if (arg == "--rom-dir" && has_value) {
            rom_dir_option = argv[++i];
        } else if (arg == "--sys" && has_value) {
            sys_path = argv[++i];
        } else if (arg == "--ext" && has_value) {
            ext_path = argv[++i];
        } else if (arg == "--cart" && has_value) {
            cart_path = argv[++i];
        } else if (arg == "--sddual") {
            sddual = true;
        } else if (arg == "--sddual-gm") {
            sddual = sddual_gamer = true;
        } else if (arg == "--sddual-rom" && has_value) {
            sddual = true;
            sddual_rom_path = argv[++i];
        } else if (arg == "--sddual-flash-write") {
            sddual_flash_write = true;
        } else if ((arg == "--sd" || arg == "--sd2") && has_value) {
            sd_paths[arg == "--sd" ? 0 : 1] = argv[++i];
        } else if (arg == "--sd-build" && has_value) {
            sd_build = argv[++i];
        } else if (arg == "--sd-add" && has_value) {
            sd_files.emplace_back(argv[++i]);
        } else if (arg == "--sd-list" && has_value) {
            sd_list = argv[++i];
        } else if (arg == "--sd-size" && has_value) {
            sd_mib = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--cart-type" && has_value) {
            cart_type = argv[++i];
        } else if (arg == "--multicart-build" && has_value) {
            multicart_build = argv[++i];
        } else if (arg == "--multicart-add" && has_value) {
            multicart_files.emplace_back(argv[++i]);
        } else if (arg == "--multicart-lang" && has_value) {
            multicart_lang = argv[++i];
        } else if (arg == "--cart-bank" && has_value) {
            const auto n = parse_count(argv[++i]);
            if (!n || *n > 63) {
                std::fprintf(stderr, "freetvc-headless: bad bank '%s'\n", argv[i]);
                return kExitUsage;
            }
            cart_bank = static_cast<unsigned>(*n);
        } else if (arg == "--gamecard") {
            // an optional slot number follows
            gamecard_slot = freetvc::kGameCardSlot;
            if (has_value && std::string_view(argv[i + 1]).size() == 1 && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '3') {
                gamecard_slot = static_cast<unsigned>(argv[++i][0] - '0');
            }
        } else if (arg == "--gamecard-rom" && has_value) {
            gamecard_rom_path = argv[++i];
            if (!gamecard_slot) {
                gamecard_slot = freetvc::kGameCardSlot;
            }
        } else if (arg == "--multichip") {
            multichip_slot = freetvc::kMultichipSlot;
            if (has_value && std::string_view(argv[i + 1]).size() == 1 && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '3') {
                multichip_slot = static_cast<unsigned>(argv[++i][0] - '0');
            }
        } else if (arg == "--multichip-rom" && has_value) {
            multichip_rom_path = argv[++i];
            if (!multichip_slot) {
                multichip_slot = freetvc::kMultichipSlot;
            }
        } else if (arg == "--sid-card") {
            sid.model = freetvc::SidModel::Mos6581;
            if (has_value && (std::string_view(argv[i + 1]) == "6581" || std::string_view(argv[i + 1]) == "8580")) {
                sid.model = std::string_view(argv[++i]) == "8580" ? freetvc::SidModel::Mos8580 : freetvc::SidModel::Mos6581;
            }
        } else if (arg == "--sid-clock" && has_value) {
            const unsigned long hz = std::strtoul(argv[++i], nullptr, 10);
            if (hz < 100'000 || hz > 4'000'000) {
                std::fprintf(stderr, "freetvc-headless: --sid-clock: 100000 to 4000000 Hz\n");
                return kExitUsage;
            }
            sid.clock_hz = static_cast<std::uint32_t>(hz);
        } else if (arg == "--printer" && has_value) {
            const std::string file = argv[++i];
            printer_on = true;
            if (file != "on") {
                (file.size() > 4 && file.compare(file.size() - 4, 4, ".txt") == 0 ? printer_text : printer_raw) = file;
            }
        } else if (arg == "--serial-card") {
            serial_slot = freetvc::kSerialSlot;
            if (has_value && std::string_view(argv[i + 1]).size() == 1 && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '3') {
                serial_slot = static_cast<unsigned>(argv[++i][0] - '0');
            }
        } else if ((arg == "--serial" || arg == "--serial-flow" || arg == "--serial-send" || arg == "--serial-capture") && has_value) {
            (arg == "--serial" ? serial_peer : arg == "--serial-flow" ? serial_flow : arg == "--serial-send" ? serial_send : serial_capture) = argv[++i];
            if (!serial_slot) {
                serial_slot = freetvc::kSerialSlot;
            }
        } else if (arg == "--2dfx") {
            twodfx_slot = 2u;
            if (has_value && std::string_view(argv[i + 1]).size() == 1 && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '3') {
                twodfx_slot = static_cast<unsigned>(argv[++i][0] - '0');
            }
        } else if (arg == "--nanosd") {
            nanosd_slot = freetvc::kNanoSdSlot;
            if (has_value && std::string_view(argv[i + 1]).size() == 1 && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '3') {
                nanosd_slot = static_cast<unsigned>(argv[++i][0] - '0');
            }
        } else if (arg == "--nanosd-rom" && has_value) {
            nanosd_rom_path = argv[++i];
            if (!nanosd_slot) {
                nanosd_slot = freetvc::kNanoSdSlot;
            }
        } else if (arg == "--nanosd-sd" && has_value) {
            nanosd_sd = argv[++i];
        } else if (arg == "--card256") {
            card256_slot = freetvc::kCardSlots;
            if (has_value && std::string_view(argv[i + 1]).size() == 1 && argv[i + 1][0] >= '0' && argv[i + 1][0] <= '3') {
                card256_slot = static_cast<unsigned>(argv[++i][0] - '0');
            }
        } else if (arg == "--card256-rom" && has_value) {
            card256_rom_path = argv[++i];
            if (!card256_slot) {
                card256_slot = freetvc::kCardSlots;
            }
        } else if ((arg == "--card256-usb" || arg == "--card256-flash") && has_value) {
            (arg == "--card256-usb" ? card256_usb : card256_flash) = argv[++i];
            if (!card256_slot) {
                card256_slot = freetvc::kCardSlots;
            }
        } else if (arg == "--disk-card") {
            disk_card = true;
        } else if (arg == "--disk-rom" && has_value) {
            disk_rom_path = argv[++i];
            disk_card = true;
        } else if (arg == "--disk" && has_value) {
            // [A-D:]FILE; a bare file goes into the first drive not named yet
            std::string value = argv[++i];
            std::optional<unsigned> drive;
            if (value.size() > 2 && value[1] == ':') {
                drive = freetvc::HostTape::parse_drive(std::string_view(value).substr(0, 2));
                if (drive) {
                    value.erase(0, 2);
                }
            }
            if (!drive) {
                drive = static_cast<unsigned>(disks.size());
            }
            if (*drive >= freetvc::FloppyDrives::kDrives) {
                std::fprintf(stderr, "freetvc-headless: --disk: there are drives A: to D: only\n");
                return kExitUsage;
            }
            disks.emplace_back(*drive, value);
            disk_card = true;
        } else if (arg == "--cas" && has_value) {
            cas_path = argv[++i];
        } else if (arg == "--cas-dir" && has_value) {
            cas_dir = argv[++i];
        } else if (arg == "--autostart" && has_value) {
            autostart_path = argv[++i];
        } else if (arg == "--autostart-tape" && has_value) {
            autostart_path = argv[++i];
            autostart_tape = true;
        } else if (arg == "--tape" && has_value) {
            tape_path = argv[++i];
        } else if (arg == "--tape-record" && has_value) {
            tape_record_path = argv[++i];
        } else if (arg == "--screenshot" && has_value) {
            screenshot_path = argv[++i];
        } else if (arg == "--snapshot" && has_value) {
            snapshot_path = argv[++i];
        } else if (arg == "--save-snapshot" && has_value) {
            save_snapshot_path = argv[++i];
        } else if (arg == "--wav" && has_value) {
            wav_path = argv[++i];
        } else if (arg == "--monitor") {
            console_monitor = true;
        } else if (arg == "--remote-monitor") {
            remote_monitor = true;
        } else if (arg == "--monitor-port" && has_value) {
            const auto port = parse_count(argv[++i]);
            if (!port || *port > 0xFFFF) {
                std::fprintf(stderr, "freetvc-headless: bad port '%s'\n", argv[i]);
                return kExitUsage;
            }
            monitor_port = static_cast<std::uint16_t>(*port);
            remote_monitor = true;
        } else if (arg == "--moncommands" && has_value) {
            moncommands = argv[++i];
        } else if (arg == "--mcp-stdio") {
            mcp_stdio = true;
        } else if (arg == "--mcp-http") {
            mcp_http = true;
        } else if (arg == "--mcp-port" && has_value) {
            const auto port = parse_count(argv[++i]);
            if (!port || *port > 0xFFFF) {
                std::fprintf(stderr, "freetvc-headless: bad port '%s'\n", argv[i]);
                return kExitUsage;
            }
            mcp_port = static_cast<std::uint16_t>(*port);
            mcp_http = true;
        } else if (arg == "--mcp-token" && has_value) {
            mcp_token = argv[++i];
        } else if (arg == "--mcp-allow-monitor") {
            mcp_allow_monitor = true;
        } else if (arg == "--dzrp") {
            dzrp = true;
        } else if (arg == "--dzrp-banks") {
            dzrp_memory = freetvc::remote::DzrpMemory::TvcBanks;
            dzrp = true;
        } else if (arg == "--dzrp-port" && has_value) {
            const auto port = parse_count(argv[++i]);
            if (!port || *port > 0xFFFF) {
                std::fprintf(stderr, "freetvc-headless: bad port '%s'\n", argv[i]);
                return kExitUsage;
            }
            dzrp_port = static_cast<std::uint16_t>(*port);
            dzrp = true;
        } else if (arg == "--gdb") {
            gdb = true;
        } else if (arg == "--gdb-port" && has_value) {
            const auto port = parse_count(argv[++i]);
            if (!port || *port > 0xFFFF) {
                std::fprintf(stderr, "freetvc-headless: bad port '%s'\n", argv[i]);
                return kExitUsage;
            }
            gdb_port = static_cast<std::uint16_t>(*port);
            gdb = true;
        } else {
            std::fprintf(stderr, "freetvc-headless: unknown or incomplete option '%s'\n", argv[i]);
            print_usage(stderr);
            return kExitUsage;
        }
    }

    for (const std::string_view text : texts) {
        if (!add_text(text, typing, key_events)) {
            std::fprintf(stderr, "freetvc-headless: --type expects TEXT@FRAME, got '%.*s'\n", static_cast<int>(text.size()),
                         text.data());
            return kExitUsage;
        }
    }
    std::stable_sort(key_events.begin(), key_events.end(),
                     [](const freetvc::KeyEvent& a, const freetvc::KeyEvent& b) { return a.frame < b.frame; });

    std::string error;
    if (fetch_roms) {
        // The ROM autofetch: into --rom-dir if given, else roms/ in the user data folder, then exit
        const std::filesystem::path dir = !rom_dir_option.empty() ? std::filesystem::path(rom_dir_option)
                                                                  : freetvc::user_data_directory() / "roms";
        const freetvc::CurlCheck curl = freetvc::find_curl();
        if (!curl.ok()) {
            std::fprintf(stderr, "freetvc-headless: --fetch-roms: %s\n", curl.problem.c_str());
            return kExitError;
        }
        std::fprintf(stderr, "freetvc-headless: using %s (%s)\n", curl.program.string().c_str(), curl.version.c_str());
        const bool ok = freetvc::fetch_roms(curl, dir, error, [](const std::string& line) {
            std::fprintf(stderr, "freetvc-headless: %s\n", line.c_str());
        });
        if (!ok) {
            std::fprintf(stderr, "freetvc-headless: --fetch-roms: %s\n", error.c_str());
            return kExitError;
        }
        std::fprintf(stderr, "freetvc-headless: ROMs in %s\n", dir.string().c_str());
        return 0;
    }
    freetvc::MachineConfig config;
    // ROM search with or without a preset; of the settings file only rom_dir counts, so a GUI session never steers a run
    std::vector<freetvc::RomPlace> rom_search;
    {
        const auto program_dir = freetvc::program_directory(argv[0]);
        const auto user_dir = freetvc::user_data_directory();
        const freetvc::SettingsFile settings = freetvc::load_settings(program_dir, user_dir);
        for (const std::string& line : settings.errors) {
            std::fprintf(stderr, "freetvc-headless: %s\n", line.c_str());
        }
        rom_search = freetvc::rom_places(rom_dir_option, settings.values.rom_dir.value_or(""), program_dir, user_dir);
    }
    if (!multicart_build.empty()) {
        if (!freetvc::write_multicart_image(multicart_build, multicart_files, multicart_lang, rom_search, error)) {
            std::fprintf(stderr, "freetvc-headless: --multicart-build: %s\n", error.c_str());
            return kExitError;
        }
        const auto image = freetvc::read_file(multicart_build, freetvc::MultiCartFlash::kSize, error).value_or(std::vector<std::uint8_t>{});
        for (const freetvc::MultiCartEntry& e : freetvc::multicart_directory(image)) {
            std::fprintf(stderr, "freetvc-headless: %-5s bank %2u block %2u, %3u blocks  %s\n", freetvc::multicart_type_name(e.type),
                         e.bank, e.block, e.blocks, e.name.c_str());
        }
        return 0;
    }
    if (!sd_list.empty()) {
        const std::vector<std::string> paths = freetvc::sd_image_listing(sd_list);
        if (paths.empty()) {
            std::fprintf(stderr, "freetvc-headless: --sd-list: %s has no FAT volume with files\n", sd_list.c_str());
            return kExitError;
        }
        for (const std::string& path : paths) {
            std::printf("%s\n", path.c_str());
        }
        return 0;
    }
    if (!sd_build.empty()) {
        if (!freetvc::write_sd_image(sd_build, sd_mib, sd_files, error)) {
            std::fprintf(stderr, "freetvc-headless: --sd-build: %s\n", error.c_str());
            return kExitError;
        }
        for (const std::string& path : freetvc::sd_image_listing(sd_build)) {
            std::fprintf(stderr, "freetvc-headless: %s\n", path.c_str());
        }
        return 0;
    }
    if (preset) {
        auto loaded = freetvc::find_preset_roms(*preset, rom_search, error);
        if (!loaded) {
            std::fprintf(stderr, "freetvc-headless: preset %.*s: %s\n", static_cast<int>(preset->name.size()),
                         preset->name.data(), error.c_str());
            return kExitError;
        }
        config = std::move(*loaded);
    }
    if (model) {
        config.model = *model;
    }
    for (auto [path, size, image] : {std::tuple{&sys_path, 0x4000u, &config.roms.sys},
                                     std::tuple{&ext_path, 0x2000u, &config.roms.ext}}) {
        if (path->empty()) {
            continue;
        }
        auto bytes = freetvc::read_file(*path, size, error);
        if (!bytes) {
            std::fprintf(stderr, "freetvc-headless: %s\n", error.c_str());
            return kExitError;
        }
        *image = std::move(*bytes);
    }
    freetvc::normalise_ext(config.roms.ext);
    std::vector<std::uint8_t> multicart_flash;
    if (!cart_path.empty() && !freetvc::load_cart(cart_path, cart_type, cart_bank, config.roms, error, &multicart_flash)) {
        std::fprintf(stderr, "freetvc-headless: %s\n", error.c_str());
        return kExitError;
    }
    // --sd alone brings the SD Dual, unless a cartridge or a snapshot brings the machine
    if ((!sd_paths[0].empty() || !sd_paths[1].empty()) && cart_path.empty() && snapshot_path.empty()) {
        sddual = true;
    }
    std::vector<std::uint8_t> sddual_rom;
    if (sddual) {
        if (!cart_path.empty()) {
            std::fprintf(stderr, "freetvc-headless: --sddual: the cartridge port holds %s\n", cart_path.c_str());
            return kExitError;
        }
        auto rom = !sddual_rom_path.empty() ? freetvc::read_file(sddual_rom_path, freetvc::SdDual::kFlashSize, error)
                                            : freetvc::find_card_rom(sddual_gamer ? freetvc::kSdDualGamerRom : freetvc::kSdDualRom,
                                                                     freetvc::SdDual::kFlashSize, rom_search, error);
        if (!rom || !freetvc::is_sddual_firmware(*rom)) {
            std::fprintf(stderr, "freetvc-headless: --sddual: %s\n", rom ? "not SD Dual firmware" : error.c_str());
            return kExitError;
        }
        sddual_rom = *rom;
        config.roms.cart = std::move(*rom);
        config.roms.cart_type = freetvc::CartType::SdDual;
    }
    if (config.roms.cart_type == freetvc::CartType::SdDual) {
        config.roms.cart_bank = sddual_flash_write ? 1 : 0;
    }
    // The disk card goes in before the machine exists, so --load, --pc and --port02 are not undone by a power-on
    if (!autostart_path.empty() && freetvc::HostTape::is_disk_file(autostart_path)) {
        disk_card = true;
    }
    std::vector<std::uint8_t> disk_rom;
    if (disk_card) {
        auto rom = freetvc::disk_card_rom(disk_rom_path, rom_search, error);
        if (!rom) {
            std::fprintf(stderr, "freetvc-headless: %s\n", error.c_str());
            return kExitError;
        }
        disk_rom = std::move(*rom);
        config.cards[freetvc::kDiskCardSlot] = {freetvc::CardKind::Hbf, disk_rom};
    }
    std::vector<std::uint8_t> gamecard_rom;
    if (gamecard_slot) {
        if (config.cards[*gamecard_slot].kind != freetvc::CardKind::None) {
            std::fprintf(stderr, "freetvc-headless: --gamecard: slot %u holds the disk card\n", *gamecard_slot);
            return kExitUsage;
        }
        auto rom = gamecard_rom_path.empty() ? freetvc::find_card_rom(freetvc::kGameCardRom, 0x10000, rom_search, error)
                                             : freetvc::read_file(gamecard_rom_path, 0x10000, error);
        if (rom) {
            gamecard_rom = std::move(*rom);
        } else if (!gamecard_rom_path.empty()) {
            std::fprintf(stderr, "freetvc-headless: %s\n", error.c_str());
            return kExitError;
        } else {
            std::fprintf(stderr, "freetvc-headless: GameCard without its EPROM, so without the boot menu: %s\n",
                         error.substr(0, error.find("; searched")).c_str());
        }
        config.cards[*gamecard_slot] = {freetvc::CardKind::GameCard, gamecard_rom};
    }
    std::vector<std::uint8_t> multichip_rom;
    if (multichip_slot) {
        if (config.cards[*multichip_slot].kind != freetvc::CardKind::None) {
            std::fprintf(stderr, "freetvc-headless: --multichip: slot %u holds the %s\n", *multichip_slot,
                         freetvc::card_kind_name(config.cards[*multichip_slot].kind));
            return kExitUsage;
        }
        auto rom = multichip_rom_path.empty()
                       ? freetvc::find_card_rom(freetvc::kMultichipRom, freetvc::MultichipCard::kRomSize, rom_search, error)
                       : freetvc::read_file(multichip_rom_path, freetvc::MultichipCard::kRomSize, error);
        if (rom) {
            multichip_rom = std::move(*rom);
        } else if (!multichip_rom_path.empty()) {
            std::fprintf(stderr, "freetvc-headless: %s\n", error.c_str());
            return kExitError;
        } else {
            std::fprintf(stderr, "freetvc-headless: Multichip sound card without its EEPROM, so without its start-up screen: %s\n",
                         error.substr(0, error.find("; searched")).c_str());
        }
        config.cards[*multichip_slot] = {freetvc::CardKind::Multichip, multichip_rom};
    }
    config.sid = sid;
    // --nanosd-sd alone brings the card, unless a snapshot brings the machine
    if (!nanosd_sd.empty() && !nanosd_slot && snapshot_path.empty()) {
        nanosd_slot = freetvc::kNanoSdSlot;
    }
    std::vector<std::uint8_t> nanosd_rom;
    if (nanosd_slot) {
        if (config.cards[*nanosd_slot].kind != freetvc::CardKind::None) {
            std::fprintf(stderr, "freetvc-headless: --nanosd: slot %u holds the %s\n", *nanosd_slot,
                         freetvc::card_kind_name(config.cards[*nanosd_slot].kind));
            return kExitUsage;
        }
        auto rom = nanosd_rom_path.empty() ? freetvc::find_card_rom(freetvc::kNanoSdRom, freetvc::NanoSd::kRomSize, rom_search, error)
                                           : freetvc::read_file(nanosd_rom_path, freetvc::NanoSd::kRomSize, error);
        if (rom) {
            nanosd_rom = std::move(*rom);
        } else if (!nanosd_rom_path.empty()) {
            std::fprintf(stderr, "freetvc-headless: %s\n", error.c_str());
            return kExitError;
        } else {
            std::fprintf(stderr, "freetvc-headless: NanoSD without its EEPROM, so the machine never calls the card: %s\n",
                         error.substr(0, error.find("; searched")).c_str());
        }
        config.cards[*nanosd_slot] = {freetvc::CardKind::NanoSd, nanosd_rom};
    }
    if (twodfx_slot) {
        if (config.cards[*twodfx_slot].kind != freetvc::CardKind::None) {
            std::fprintf(stderr, "freetvc-headless: --2dfx: slot %u holds the %s\n", *twodfx_slot,
                         freetvc::card_kind_name(config.cards[*twodfx_slot].kind));
            return kExitUsage;
        }
        if (config.sid.model != freetvc::SidModel::None) {
            std::fprintf(stderr, "freetvc-headless: --2dfx: not with the SID card (both use ports F8h/F9h)\n");
            return kExitUsage;
        }
        if (card256_slot) {
            std::fprintf(stderr, "freetvc-headless: --2dfx: not with the TVC256k++ (both drive EC0-3 and /EXTC)\n");
            return kExitUsage;
        }
        config.cards[*twodfx_slot] = {freetvc::CardKind::TwoDfx, {}};
    }
    if (serial_slot) {
        if (config.cards[*serial_slot].kind != freetvc::CardKind::None) {
            std::fprintf(stderr, "freetvc-headless: --serial-card: slot %u holds the %s\n", *serial_slot,
                         freetvc::card_kind_name(config.cards[*serial_slot].kind));
            return kExitUsage;
        }
        config.cards[*serial_slot] = {freetvc::CardKind::Serial, {}};
    }
    std::vector<std::uint8_t> card256_rom;
    if (card256_slot) {
        if (config.model == freetvc::Model::Tvc32kHbm) {
            std::fprintf(stderr, "freetvc-headless: --card256: not with the HBM RAM card (the card's manual: no other RAM expansion)\n");
            return kExitUsage;
        }
        if (*card256_slot == freetvc::kCardSlots) {
            card256_slot = freetvc::card256_default_slot(config.cards);
        }
        if (*card256_slot == freetvc::kCardSlots || config.cards[*card256_slot].kind != freetvc::CardKind::None) {
            std::fprintf(stderr, "freetvc-headless: --card256: slot %u holds another card\n", std::min(*card256_slot, 3u));
            return kExitUsage;
        }
        auto rom = card256_rom_path.empty() ? freetvc::find_card_rom(freetvc::kCard256Rom, freetvc::Card256::kRomSize, rom_search, error)
                                            : freetvc::read_file(card256_rom_path, freetvc::Card256::kRomSize, error);
        if (rom) {
            card256_rom = std::move(*rom);
        } else if (!card256_rom_path.empty()) {
            std::fprintf(stderr, "freetvc-headless: %s\n", error.c_str());
            return kExitError;
        } else {
            std::fprintf(stderr, "freetvc-headless: TVC256k++ without its ROM, so the machine never calls the card: %s\n",
                         error.substr(0, error.find("; searched")).c_str());
        }
        std::vector<std::string> notes;
        card256_rom = freetvc::card256_image(std::move(card256_rom), rom_search, notes);
        for (const std::string& note : notes) {
            std::fprintf(stderr, "freetvc-headless: %s\n", note.c_str());
        }
        config.cards[*card256_slot] = {freetvc::CardKind::Card256, card256_rom};
    }

    freetvc::Machine machine(config);
    for (const auto& item : loads) {
        const auto bytes = freetvc::read_file(item.path, 0x10000, error);
        if (!bytes) {
            std::fprintf(stderr, "freetvc-headless: %s\n", error.c_str());
            return kExitError;
        }
        machine.load(item.address, *bytes);
    }
    if (port02) {
        machine.io_write(0x02, *port02);
    }
    if (start_pc) {
        machine.jump(*start_pc);
    }

    constexpr std::uint64_t kFrameTicks = freetvc::kTStatesPerFrame * freetvc::kTicksPerTState;
    freetvc::HostTape tape(machine);
    tape.set_rom_places(rom_search);
    tape.set_prompt_dir(freetvc::user_data_directory() / "prompt");
    if (config.roms.cart_type == freetvc::CartType::MultiCart && !multicart_flash.empty()) {
        tape.set_multicart(cart_path, std::move(multicart_flash));
    }
    if (disk_card) {
        tape.set_disk_rom(disk_rom);
    }
    tape.set_sddual_flash_writable(sddual_flash_write);
    if (!sddual_rom_path.empty()) {
        tape.set_sddual_rom(sddual_rom);
    }
    for (unsigned s = 0; s < sd_paths.size(); ++s) {
        if (!sd_paths[s].empty() && !tape.sd_insert(s, sd_paths[s], error)) {
            std::fprintf(stderr, "freetvc-headless: %s: %s\n", s == 0 ? "--sd" : "--sd2", error.c_str());
            return kExitError;
        }
    }
    if (gamecard_slot) {
        tape.set_gamecard_rom(gamecard_rom);
    }
    tape.set_sid_clock(sid.clock_hz);
    if (multichip_slot) {
        tape.set_multichip_rom(multichip_rom);
    }
    if (nanosd_slot) {
        tape.set_nanosd_rom(nanosd_rom);
    }
    if (printer_on && !tape.printer_plug(printer_raw, printer_text, error)) {
        std::fprintf(stderr, "freetvc-headless: --printer: %s\n", error.c_str());
        return kExitError;
    }
    if (serial_peer.empty() && (!serial_send.empty() || !serial_capture.empty())) {
        serial_peer = "on";
    }
    if (!serial_peer.empty() && !tape.serial_peer(serial_peer, error)) {
        std::fprintf(stderr, "freetvc-headless: --serial: %s\n", error.c_str());
        return kExitError;
    }
    if (!serial_flow.empty()) {
        if (serial_flow != "rts" && serial_flow != "none") {
            std::fprintf(stderr, "freetvc-headless: --serial-flow: rts or none\n");
            return kExitUsage;
        }
        tape.serial_flow(serial_flow == "rts");
    }
    if (!serial_send.empty() && !tape.serial_queue_file(serial_send, error)) {
        std::fprintf(stderr, "freetvc-headless: --serial-send: %s\n", error.c_str());
        return kExitError;
    }
    if (!serial_capture.empty() && !tape.serial_capture(serial_capture, error)) {
        std::fprintf(stderr, "freetvc-headless: --serial-capture: %s\n", error.c_str());
        return kExitError;
    }
    if (!nanosd_sd.empty() && !tape.nanosd_sd(nanosd_sd, error)) {
        std::fprintf(stderr, "freetvc-headless: --nanosd-sd: %s\n", error.c_str());
        return kExitError;
    }
    if (card256_slot) {
        tape.set_card256_rom(card256_rom);
        if (!card256_usb.empty() && !tape.card256_usb(card256_usb, error)) {
            std::fprintf(stderr, "freetvc-headless: --card256-usb: %s\n", error.c_str());
            return kExitError;
        }
        if (!card256_flash.empty() && !tape.card256_flash(card256_flash, error)) {
            std::fprintf(stderr, "freetvc-headless: --card256-flash: %s\n", error.c_str());
            return kExitError;
        }
        (void)tape.card256_rom();  // the flash drive from the ROM search when none was given
    }
    for (const auto& [drive, path] : disks) {
        if (!tape.disk_insert(drive, path, error)) {
            std::fprintf(stderr, "freetvc-headless: --disk: %s\n", error.c_str());
            return kExitError;
        }
    }
    const auto log_media = [&] {
        for (const std::string& line : tape.take_messages()) {
            std::fprintf(stderr, "freetvc-headless: %s\n", line.c_str());
        }
    };
    if (!cas_dir.empty() && !tape.set_folder(cas_dir, error)) {
        std::fprintf(stderr, "freetvc-headless: --cas-dir: %s\n", error.c_str());
        return kExitError;
    }
    if (!cas_path.empty() && !tape.attach(cas_path, error)) {
        std::fprintf(stderr, "freetvc-headless: %s\n", error.c_str());
        return kExitError;
    }
    if ((!tape_path.empty() && !tape.tape_insert(tape_path, error)) ||
        (!tape_record_path.empty() && !tape.tape_record(tape_record_path, error))) {
        std::fprintf(stderr, "freetvc-headless: %s\n", error.c_str());
        return kExitError;
    }
    if (!tape_path.empty()) {
        tape.tape_play();
    }
    if (!snapshot_path.empty()) {
        if (!autostart_path.empty() || !loads.empty() || start_pc || port02) {
            std::fprintf(stderr, "freetvc-headless: --snapshot sets the whole machine; drop --autostart, --load, --pc and --port02\n");
            return kExitUsage;
        }
        if (!tape.snapshot_load(snapshot_path, error)) {
            std::fprintf(stderr, "freetvc-headless: %s\n", error.c_str());
            return kExitError;
        }
        std::fprintf(stderr, "freetvc-headless: snapshot %s loaded (frame %llu)\n", snapshot_path.c_str(),
                     static_cast<unsigned long long>(machine.frame_count()));
    }
    if (!autostart_path.empty()) {
        if (!loads.empty() || start_pc || port02) {
            std::fprintf(stderr, "freetvc-headless: --autostart cold-starts the machine; drop --load, --pc and --port02\n");
            return kExitUsage;
        }
        if (autostart_tape ? !tape.tape_autostart(autostart_path, error) : !tape.autostart(autostart_path, error)) {
            std::fprintf(stderr, "freetvc-headless: %s\n", error.c_str());
            return kExitError;
        }
        std::vector<float> discarded;
        while (tape.media().busy()) {
            tape.media().run_slice(machine);
            discarded.resize(machine.audio_available());
            machine.read_audio(discarded);
        }
        tape.media().service(machine);
        log_media();
        const bool started = tape.media().state() == freetvc::AutostartState::Done;
        std::fprintf(stderr, "freetvc-headless: autostart: %s (frame %llu)\n", tape.media().autostart_result().c_str(),
                     static_cast<unsigned long long>(machine.frame_count()));
        if (!started) {
            return kExitError;
        }
    }
    log_media();
    std::size_t next_event = 0;
    std::vector<float> audio;
    const auto collect_audio = [&] {
        if (!wav_path.empty()) {
            const std::size_t old_size = audio.size();
            audio.resize(old_size + machine.audio_available());
            machine.read_audio(std::span<float>(audio).subspan(old_size));
        }
    };
    const auto write_outputs = [&]() -> bool {
        if (!screenshot_path.empty() && !write_ppm(screenshot_path, machine.framebuffer())) {
            std::fprintf(stderr, "freetvc-headless: cannot write '%s'\n", screenshot_path.c_str());
            return false;
        }
        if (!wav_path.empty() && !freetvc::write_wav(wav_path, audio, machine.audio_rate(), error)) {
            std::fprintf(stderr, "freetvc-headless: %s\n", error.c_str());
            return false;
        }
        if (!save_snapshot_path.empty() && !tape.snapshot_save(save_snapshot_path, error)) {
            std::fprintf(stderr, "freetvc-headless: %s\n", error.c_str());
            return false;
        }
        return true;
    };

    if (mcp_stdio && console_monitor) {
        std::fprintf(stderr, "freetvc-headless: --mcp-stdio and --monitor both need stdin/stdout\n");
        return kExitUsage;
    }
    if (console_monitor || remote_monitor || !moncommands.empty() || mcp_stdio || mcp_http || dzrp || gdb) {
        tape.set_rewind(true);  // with a monitor or a remote client, rewind is on
        freetvc::debug::Debugger dbg(machine);
        freetvc::remote::Monitor monitor(dbg);
        monitor.set_media(&tape);
        freetvc::remote::MonitorHub hub(monitor);
        std::unique_ptr<freetvc::remote::McpServer> mcp;
        std::unique_ptr<freetvc::remote::McpHost> mcp_host;
        if (mcp_stdio || mcp_http) {
            hub.set_log(stderr);  // stdout belongs to MCP on stdio; keep the two front ends' logs together
            freetvc::remote::McpOptions options;
            options.machine_name = preset ? std::string(preset->name) : "custom";
            options.allow_monitor_command = mcp_allow_monitor;
            mcp = std::make_unique<freetvc::remote::McpServer>(monitor, options);
            mcp_host = std::make_unique<freetvc::remote::McpHost>(*mcp);
            if (mcp_http) {
                const std::string token = mcp_token.empty() ? freetvc::remote::McpHost::make_token() : mcp_token;
                if (!mcp_host->listen_http(mcp_port, token, error)) {
                    std::fprintf(stderr, "freetvc-headless: %s\n", error.c_str());
                    return kExitError;
                }
                std::fprintf(stderr, "freetvc-headless: MCP on http://127.0.0.1:%u/mcp (Authorization: Bearer %s)\n",
                             static_cast<unsigned>(mcp_host->http_port()), token.c_str());
            }
            if (mcp_stdio) {
                mcp_host->attach_stdio();
            }
            if (moncommands.empty()) {
                monitor.enter("MCP");  // the machine moves only when a tool asks
            }
        }
        if (remote_monitor) {
            if (!hub.listen(monitor_port, error)) {
                std::fprintf(stderr, "freetvc-headless: %s\n", error.c_str());
                return kExitError;
            }
            std::fprintf(stderr, "freetvc-headless: text monitor on 127.0.0.1:%u\n", static_cast<unsigned>(hub.port()));
        }
        std::unique_ptr<freetvc::remote::DzrpServer> dzrp_server;
        std::unique_ptr<freetvc::remote::DzrpHost> dzrp_host;
        if (dzrp) {
            dzrp_server = std::make_unique<freetvc::remote::DzrpServer>(monitor, dzrp_memory);
            dzrp_host = std::make_unique<freetvc::remote::DzrpHost>(*dzrp_server);
            if (!dzrp_host->listen(dzrp_port, error)) {
                std::fprintf(stderr, "freetvc-headless: %s\n", error.c_str());
                return kExitError;
            }
            std::fprintf(stderr, "freetvc-headless: DZRP on 127.0.0.1:%u\n", static_cast<unsigned>(dzrp_host->port()));
        }
        std::unique_ptr<freetvc::remote::GdbServer> gdb_server;
        std::unique_ptr<freetvc::remote::GdbHost> gdb_host;
        if (gdb) {
            gdb_server = std::make_unique<freetvc::remote::GdbServer>(monitor);
            gdb_host = std::make_unique<freetvc::remote::GdbHost>(*gdb_server);
            if (!gdb_host->listen(gdb_port, error)) {
                std::fprintf(stderr, "freetvc-headless: %s\n", error.c_str());
                return kExitError;
            }
            std::fprintf(stderr, "freetvc-headless: GDB stub on 127.0.0.1:%u\n", static_cast<unsigned>(gdb_host->port()));
        }
        if (!moncommands.empty()) {
            std::ifstream in(moncommands);
            if (!in) {
                std::fprintf(stderr, "freetvc-headless: cannot read '%s'\n", moncommands.c_str());
                return kExitError;
            }
            std::vector<std::string> lines;
            for (std::string line; std::getline(in, line);) {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                lines.push_back(line);
            }
            hub.add_script(std::move(lines));
        }
        if (console_monitor) {
            hub.attach_console();
            if (moncommands.empty()) {
                hub.start_in_monitor("start");
            }
        }
        const std::uint64_t budget = budget_given ? tstates * freetvc::kTicksPerTState : std::numeric_limits<std::uint64_t>::max();
        const std::uint64_t first_frame = machine.frame_count();
        std::uint64_t ran = 0;
        for (;;) {
            hub.service();
            log_media();
            if (mcp_host) {
                mcp_host->service();
            }
            if (dzrp_host) {
                dzrp_host->service();
            }
            if (gdb_host) {
                gdb_host->service();
            }
            if (hub.quit_code() || ran >= budget || (mcp_stdio && mcp_host->stdio_closed())) {
                break;
            }
            if (!monitor.active()) {
                freetvc::apply_key_events(machine, key_events, next_event, machine.frame_count() - first_frame);
                ran += hub.run(std::min(budget - ran, kFrameTicks));
                collect_audio();
                continue;
            }
            if (!mcp_host && !dzrp_host && !gdb_host && !hub.interactive() && hub.script_done()) {
                break;  // stopped, and nobody is left to resume the machine
            }
            if (gdb_host) {
                gdb_host->wait(std::chrono::milliseconds(10));
            } else if (dzrp_host) {
                dzrp_host->wait(std::chrono::milliseconds(10));
            } else if (mcp_host) {
                mcp_host->wait(std::chrono::milliseconds(10));
            } else {
                hub.wait(std::chrono::milliseconds(100));
            }
        }
        if (!write_outputs()) {
            return kExitError;
        }
        return hub.quit_code().value_or(0);
    }

    // Run in frame-sized slices so key presses land at their frame
    const std::uint64_t budget = tstates * freetvc::kTicksPerTState;
    freetvc::StopInfo stop;
    for (std::uint64_t frame = 0; stop.ticks_run + freetvc::kTicksPerTState <= budget; ++frame) {
        freetvc::apply_key_events(machine, key_events, next_event, frame);
        tape.media().service(machine);
        const std::uint64_t slice_end = std::min(budget, (frame + 1) * kFrameTicks);
        const std::uint64_t left = instructions == 0 ? 0 : instructions - stop.instructions_run;
        const freetvc::StopInfo slice = machine.run(
            {.max_ticks = slice_end > stop.ticks_run ? slice_end - stop.ticks_run : 0, .max_instructions = left});
        stop.ticks_run += slice.ticks_run;
        stop.instructions_run += slice.instructions_run;
        stop.pc = slice.pc;
        stop.reason = slice.reason;
        collect_audio();
        tape.after_slice();
        log_media();
        if (slice.reason == freetvc::StopReason::InstructionCount) {
            break;
        }
    }
    const freetvc::Z80Registers r = machine.cpu().registers();
    std::printf("stop %s  tstates %llu  instructions %llu  pc %04X  frames %llu\n", reason_name(stop.reason),
                static_cast<unsigned long long>(stop.ticks_run / freetvc::kTicksPerTState),
                static_cast<unsigned long long>(stop.instructions_run), stop.pc,
                static_cast<unsigned long long>(machine.frame_count()));
    std::printf("af %04X bc %04X de %04X hl %04X ix %04X iy %04X sp %04X wz %04X\n", r.af, r.bc, r.de, r.hl, r.ix,
                r.iy, r.sp, r.wz);
    std::printf("af' %04X bc' %04X de' %04X hl' %04X i %02X r %02X im %u iff1 %u iff2 %u\n", r.af2, r.bc2, r.de2,
                r.hl2, r.i, r.r, r.im, r.iff1 ? 1u : 0u, r.iff2 ? 1u : 0u);
    return write_outputs() ? 0 : kExitError;
}
