// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote/monitor.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <utility>

#include "core/input/keyboard.hpp"
#include "debug/disassembler.hpp"
#include "remote/png.hpp"

namespace freetvc::remote {

namespace {

constexpr std::uint64_t kFrameTicks = kTStatesPerFrame * kTicksPerTState;
constexpr std::uint64_t kStepBudget = 100 * kFrameTicks;  // z / n without a call: plenty, but bounded
constexpr std::size_t kDefaultMemory = 0x80;
constexpr int kDefaultListing = 16;

struct CommandError {
    std::string message;
};

[[noreturn]] void fail(std::string message) { throw CommandError{std::move(message)}; }

std::string hex2(unsigned v) {
    char buf[4];
    std::snprintf(buf, sizeof buf, "%02X", v & 0xFF);
    return buf;
}

std::string hex4(unsigned v) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%04X", v & 0xFFFF);
    return buf;
}

std::string lower(std::string_view s) {
    std::string out(s);
    for (char& c : out) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return out;
}

std::string trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
    }
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
        s.remove_suffix(1);
    }
    return std::string(s);
}

std::string pad(std::string s, std::size_t width) {
    if (s.size() < width) {
        s.append(width - s.size(), ' ');
    }
    return s;
}

struct Kind {
    const char* name;
    std::uint8_t access;
};

constexpr std::array<Kind, 7> kKinds{{
    {"exec", kAccessExec}, {"load", kAccessRead}, {"store", kAccessWrite}, {"in", kAccessIn},
    {"out", kAccessOut}, {"irq", kAccessIrq}, {"line", kAccessLine},
}};

std::string kind_names(std::uint8_t access) {
    std::string out;
    for (const Kind& k : kKinds) {
        if (access & k.access) {
            out += (out.empty() ? "" : "|") + std::string(k.name);
        }
    }
    return out;
}

unsigned checkpoint_id(const std::string& word) {
    unsigned id = 0;
    for (const char c : word) {
        if (!std::isdigit(static_cast<unsigned char>(c)) || id > 100000) {
            fail("checkpoint numbers are decimal, got '" + word + "'");
        }
        id = id * 10 + static_cast<unsigned>(c - '0');
    }
    return id;
}

std::optional<std::vector<std::uint8_t>> read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return std::nullopt;
    }
    return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

}  // namespace

struct Monitor::Args {
    std::vector<std::string> words;
    std::vector<bool> quoted;
    std::string condition;  // after "if"
    bool has_condition = false;

    std::size_t size() const { return words.size(); }
    const std::string& operator[](std::size_t i) const { return words[i]; }
    std::string quoted_word(std::size_t i, const char* what) const {
        if (i >= words.size()) {
            fail(std::string("missing ") + what);
        }
        return words[i];
    }
};

namespace {

// keybuf escapes: \n and \r (RETURN), \t, \" , \\ and \xNN; for bytes (`keyboard` false) \r is 0Dh and \n 0Ah
std::string unescape(std::string_view text, bool keyboard = true) {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (text[i] != '\\' || i + 1 >= text.size()) {
            out.push_back(text[i]);
            continue;
        }
        const char e = text[++i];
        if (e == 'n' || e == 'r') {
            out.push_back(keyboard || e == 'n' ? '\n' : '\r');
        } else if (e == 't') {
            out.push_back('\t');
        } else if (e == 'x' && i + 2 < text.size() && std::isxdigit(static_cast<unsigned char>(text[i + 1])) &&
                   std::isxdigit(static_cast<unsigned char>(text[i + 2]))) {
            const auto nibble = [](char c) {
                return std::isdigit(static_cast<unsigned char>(c)) ? c - '0' : std::toupper(static_cast<unsigned char>(c)) - 'A' + 10;
            };
            out.push_back(static_cast<char>(nibble(text[i + 1]) << 4 | nibble(text[i + 2])));
            i += 2;
        } else {
            out.push_back(e);
        }
    }
    return out;
}

// Words split at blanks; "quoted strings" are kept raw (Windows paths), \" inside them does not end them;
// everything after a bare `if` is the condition
void tokenize(std::string_view text, std::vector<std::string>& words, std::vector<bool>& quoted, std::string& condition,
              bool& has_condition) {
    std::size_t i = 0;
    while (i < text.size()) {
        while (i < text.size() && std::isspace(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
        if (i >= text.size()) {
            break;
        }
        if (text[i] == '"') {
            std::string word;
            ++i;
            while (i < text.size() && text[i] != '"') {
                if (text[i] == '\\' && i + 1 < text.size() && text[i + 1] == '"') {
                    word.push_back('\\');
                    ++i;
                }
                word.push_back(text[i]);
                ++i;
            }
            if (i >= text.size()) {
                fail("unterminated string");
            }
            ++i;
            words.push_back(std::move(word));
            quoted.push_back(true);
            continue;
        }
        const std::size_t start = i;
        while (i < text.size() && !std::isspace(static_cast<unsigned char>(text[i]))) {
            ++i;
        }
        const std::string_view word = text.substr(start, i - start);
        if (lower(word) == "if") {
            condition = trim(text.substr(i));
            has_condition = true;
            return;
        }
        words.emplace_back(word);
        quoted.push_back(false);
    }
}

}  // namespace

std::string escape_bytes(std::string_view bytes) {
    std::string out;
    for (const char c : bytes) {
        const auto b = static_cast<unsigned char>(c);
        if (b == '\r') {
            out += "\\r";
        } else if (b == '\n') {
            out += "\\n";
        } else if (b == '\t') {
            out += "\\t";
        } else if (b == '\\') {
            out += "\\\\";
        } else if (b >= 0x20 && b < 0x7F) {
            out += c;
        } else {
            char hex[8];
            std::snprintf(hex, sizeof hex, "\\x%02X", b);
            out += hex;
        }
    }
    return out;
}

const std::vector<Monitor::Command>& Monitor::commands() {
    static const std::vector<Command> table{
        {"g goto", "g [address]", "leave the monitor and run, from `address` if given", &Monitor::cmd_go},
        {"x exit", "x", "leave the monitor and run", &Monitor::cmd_exit},
        {"z step", "z [count]", "step into `count` instructions (1)", &Monitor::cmd_step},
        {"n next", "n [count]", "step over CALL, RST (and RST 30h's function byte), block repeats and HALT", &Monitor::cmd_next},
        {"lstep", "lstep [count]", "step into source lines: run until the PC is on another line (ll a debug file first)", &Monitor::cmd_line_step},
        {"lnext", "lnext [count]", "step over source lines: as lstep, with calls run through", &Monitor::cmd_line_next},
        {"ret return", "ret", "run until a return leaves the current stack frame", &Monitor::cmd_return},
        {"until un", "until address", "run to `address`", &Monitor::cmd_until},
        {"reset", "reset [hard]", "reset button; `hard`: power cycle (RAM cleared)", &Monitor::cmd_reset},
        {"quit", "quit [code]", "quit the emulator with exit code `code` (0)", &Monitor::cmd_quit},
        {"r registers", "r [reg=value, ...]", "show or set registers (A ... HL' IX IY SP PC WZ I R IM IFF1 IFF2)", &Monitor::cmd_registers},
        {"m mem", "m [[BANK:]start [end]]", "hex dump; BANK:offset reads a bank wherever it is paged", &Monitor::cmd_memory},
        {"d disass", "d [start [end]]", "disassemble (CPU addresses)", &Monitor::cmd_disassemble},
        {">", "> [BANK:]address byte ...", "write bytes", &Monitor::cmd_write},
        {"f fill", "f start end byte ...", "fill with a byte pattern", &Monitor::cmd_fill},
        {"h hunt", "h start end byte ...", "find a byte sequence", &Monitor::cmd_hunt},
        {"t transfer", "t start end destination", "copy memory", &Monitor::cmd_transfer},
        {"c compare", "c start end other", "compare memory", &Monitor::cmd_compare},
        {"bank", "bank", "show the paging: what each page maps", &Monitor::cmd_bank},
        {"io", "io", "show the I/O state: video, CRTC, sound, IRQ", &Monitor::cmd_io},
        {"sw stopwatch", "sw [reset]", "T-states since the last stopwatch reset", &Monitor::cmd_stopwatch},
        {"chis", "chis [count|on|off]", "instruction history (the last `count`, 16)", &Monitor::cmd_history},
        {"profile prof", "profile on|off|clear|[count]", "time every instruction and routine; no argument or a count lists the top routines (16)",
         &Monitor::cmd_profile},
        {"snap", "snap save|load|delete name | snap list", "machine states kept in memory", &Monitor::cmd_snapshot},
        {"break bk", "break [kind ...] [[BANK:]start [end]] [if cond]", "stop on exec (default) / load / store / in / out / irq / line; no arguments: list", &Monitor::cmd_break},
        {"watch w", "watch [load|store] [BANK:]start [end] [if cond]", "stop on memory access (load and store by default)", &Monitor::cmd_watch},
        {"trace tr", "trace [kind ...] [BANK:]start [end] [if cond]", "report accesses without stopping (exec by default)", &Monitor::cmd_trace},
        {"delete del", "delete [n ...]", "delete checkpoints (all without arguments)", &Monitor::cmd_delete},
        {"enable en", "enable n", "enable a checkpoint", &Monitor::cmd_enable},
        {"disable dis", "disable n", "disable a checkpoint", &Monitor::cmd_disable},
        {"condition cond", "condition n [if] [expression]", "set or clear a checkpoint's condition", &Monitor::cmd_condition},
        {"ignore", "ignore n [count]", "pass over the next `count` hits (1)", &Monitor::cmd_ignore},
        {"ll", "ll \"file\"",
         "load labels and source lines: name = value / EQU, sjasmplus --sym, --sld and --lst, SDCC .noi .cdb .map, z88dk .map",
         &Monitor::cmd_load_labels},
        {"src", "src [address]", "the source line of an address (the PC)", &Monitor::cmd_source},
        {"al", "al address name", "add a label", &Monitor::cmd_add_label},
        {"dl", "dl name", "delete a label", &Monitor::cmd_delete_label},
        {"shl", "shl [filter]", "show labels (names containing `filter`)", &Monitor::cmd_show_labels},
        {"cl", "cl", "clear the labels you loaded or added (the ROM symbols stay)", &Monitor::cmd_clear_labels},
        {"bload bl", "bload \"file\" [BANK:]address", "load a binary file into memory", &Monitor::cmd_bload},
        {"bsave bs", "bsave \"file\" [BANK:]start end", "save memory to a binary file", &Monitor::cmd_bsave},
        {"scrsh", "scrsh \"file.png\" [full]", "screenshot of the TV area (or the full raster) as PNG", &Monitor::cmd_screenshot},
        {"keybuf", "keybuf \"text\"", "type text while the machine runs (\\n = RETURN)", &Monitor::cmd_keybuf},
        {"attach", "attach [\"file.cas\"]", "attach a CAS file as the host cassette; no argument: show the media", &Monitor::cmd_attach},
        {"detach", "detach", "remove the attached cassette", &Monitor::cmd_detach},
        {"autostart", "autostart \"file.cas\"", "attach, cold start and run the program (fast), then leave the monitor", &Monitor::cmd_autostart},
        {"tape", "tape [insert|autostart|record \"file\" | play|stop|rewind|eject]",
         "the tape deck: a .wav, .ttp or .cas (played as a tape); record writes a WAV; no argument: show the deck", &Monitor::cmd_tape},
        {"dump", "dump \"file\"", "save a snapshot (TVCS) of the machine, with the tape and cassette by path", &Monitor::cmd_dump},
        {"undump", "undump \"file\"", "load a snapshot; the board and ROM set follow it (ROMs found by CRC-32)", &Monitor::cmd_undump},
        {"rewind", "rewind [seconds]", "go back in machine time (default 1 s; asking again goes further back); no "
                   "argument with nothing kept: show the state", &Monitor::cmd_rewind},
        {"cart", "cart [\"file\" [auto|plain|kilocart|megacart|multicart|sddual [bank]] | remove | build \"out\" en|hu|es \"file\"...]",
         "insert or remove a cartridge and power on; a MultiCart v2 image goes by its firmware's header, others by size, "
         "unless a type is given (a MultiCart's flash is written back to its file); build writes a new MultiCart image from "
         ".cas programs and MOPS carts in menu order, with the firmware in that language, without inserting it; no "
         "argument: show it", &Monitor::cmd_cart},
        {"disk", "disk [insert \"file\" [A-D] | eject [A-D] | autostart \"file\" | card on|off]",
         "the HBF disk card and drives A-D: insert a .dsk/.img (the card goes in, powering on, when it is out), eject "
         "(written disks are saved first), autostart from A:, or switch the card (powers on); no argument: show them", &Monitor::cmd_disk},
        {"gamecard", "gamecard [on [slot] | off]",
         "dikdom's GameCard (SN76489, joysticks 3/4) in a slot (default 1) or out; powers on; no argument: show it", &Monitor::cmd_gamecard},
        {"multichip", "multichip [on [slot] | off]",
         "the Multichip sound card (SN76489, SAA1099, YM3812) in a slot (default 2) or out; powers on; no argument: show it",
         &Monitor::cmd_multichip},
        {"2dfx", "2dfx [on [slot] | off]",
         "the 2dfx graphics card (ports F8h/F9h, not beside the TVC256k++) in a slot (default 2, else the first free) or out; "
         "powers on; no argument: show it and its status",
         &Monitor::cmd_twodfx},
        {"sid", "sid [6581 | 8580 | off]",
         "the SID card at ports E0h-FFh, outside the slots, with a MOS 6581 or 8580, or out; powers on; no argument: "
         "show it", &Monitor::cmd_sid},
        {"nanosd", "nanosd [on [slot] | off | sd \"folder\"|none]",
         "the NanoSD IO card (an SD card through an Arduino Nano) in a slot (default 3) or out (powers on); a folder as its SD card "
         "or an empty socket (the Nano notices); no argument: show it and the Nano's state", &Monitor::cmd_nanosd},
        {"printer", "printer [on | off | \"file\" [\"file\"] | read]",
         "the printer on the Centronics port: plug one in printing to files (raw bytes, or UTF-8 text for a .txt), on "
         "keeps its output in memory only, off unplugs it (then an LPRINT waits after its first character until STOP); read "
         "shows what it printed since the last read; no argument: show it", &Monitor::cmd_printer},
        {"serial", "serial [on [slot] | off | line off|on|listen[:port]|connect:port | flow rts|none | send \"text\" | file \"file\" | capture \"file\"|none | read]",
         "Videoton's RS-232 card (an 8251A) in a slot (default 1) or out (powers on); its cable: unplugged, the host as the peer, "
         "or a TCP peer on 127.0.0.1; flow rts: the peer sends only while the TVC's RTS is on; send queues bytes for the TVC "
         "(\\r \\n \\t \\xNN), file queues a file's bytes, capture appends what the TVC sends to a file, read shows what it "
         "sent since the last read; no argument: show the card and the line", &Monitor::cmd_serial},
        {"sddual", "sddual [on [gm] | off | insert 0|1 \"file\" | eject 0|1 | build \"out\" mib \"path\"...]",
         "Sebestyén Pál's SD Dual cartridge in the cartridge port (gm: its gamer-mode firmware) or out (powers on); a raw SD "
         "card image into socket 0 (microSD) or 1 (SD) or out of it (the cartridge notices); build writes a new FAT card image "
         "of mib MiB from host files and folders without inserting it; no argument: show it and both sockets", &Monitor::cmd_sddual},
        {"card256", "card256 [on [slot] | off | usb \"folder\"|mouse|none | flash \"file\" | mouse dx dy [buttons]]",
         "dikdom's TVC256k++ (fast RAM over U2/U3, PSRAM) in a slot (default 0, else the first free) or out (powers on); its USB "
         "port (a folder as the pendrive, the mouse, or nothing), its flash drive (a littlefs image or a folder), a mouse report "
         "(y down; buttons 1 left, 2 right, 4 middle, decimal); no argument: show it",
         &Monitor::cmd_card256},
        {"radix", "radix [h|d]", "default number base for input (hex)", &Monitor::cmd_radix},
        {"help ?", "help [command]", "this list, or one command", &Monitor::cmd_help},
    };
    return table;
}

Monitor::Monitor(debug::Debugger& debugger) : dbg_(debugger), machine_(debugger.machine()) {
    debug::load_rom_symbols(symbols_, machine_);
    if (dbg_.trace().capacity() == 0) {
        dbg_.set_trace_capacity(kHistory);
    }
    trace_seen_ = dbg_.hit_log().pushed();
    stopwatch_ = machine_.ticks();
}

void Monitor::machine_changed() {
    std::set<std::string> rom_sources;
    for (const debug::Symbol& s : symbols_.symbols()) {
        if (s.source.rfind("rom:", 0) == 0) {
            rom_sources.insert(s.source);
        }
    }
    for (const std::string& source : rom_sources) {
        symbols_.remove_source(source);
    }
    debug::load_rom_symbols(symbols_, machine_);
    keys_.clear();
    next_key_ = 0;
    stopwatch_ = machine_.ticks();
}

std::string Monitor::prompt() const { return "(C:$" + hex4(machine_.pc()) + ") "; }

debug::ParseOptions Monitor::parse_options() const {
    debug::ParseOptions options;
    options.radix = radix_;
    options.symbols = &symbols_;
    return options;
}

std::int64_t Monitor::value(std::string_view text) const {
    const auto [e, error] = debug::Expression::parse(text, parse_options());
    if (!e) {
        fail("'" + std::string(text) + "': " + error.message);
    }
    return e->evaluate({machine_});
}

Monitor::Place Monitor::place(std::string_view text) const {
    const auto [spec, error] = debug::parse_address(text, parse_options());
    if (!spec) {
        fail("'" + std::string(text) + "': " + error.message);
    }
    Place p;
    p.bank = spec->bank;
    p.address = static_cast<std::uint16_t>(spec->value.evaluate({machine_}));
    if (p.bank && machine_.memory().bank_bytes(*p.bank).empty()) {
        fail(std::string("this machine has no ") + bank_name(*p.bank));
    }
    if (p.bank && p.address >= machine_.memory().bank_bytes(*p.bank).size()) {
        fail(bank_end(*p.bank));
    }
    return p;
}

std::string Monitor::bank_end(Bank bank) const {
    const std::size_t size = machine_.memory().bank_bytes(bank).size();
    return std::string(bank_name(bank)) + " has " + std::to_string(size) + " bytes: offsets 0000-" + hex4(static_cast<unsigned>(size - 1));
}

std::uint8_t Monitor::read(const Place& p, unsigned offset) const {
    if (!p.bank) {
        return machine_.peek(static_cast<std::uint16_t>(p.address + offset));
    }
    const std::span<const std::uint8_t> bytes = machine_.memory().bank_bytes(*p.bank);
    const std::size_t at = static_cast<std::size_t>(p.address) + offset;
    if (at >= bytes.size()) {
        fail(bank_end(*p.bank));
    }
    return bytes[at];
}

void Monitor::write(const Place& p, unsigned offset, std::uint8_t value) {
    if (!p.bank) {
        machine_.poke(static_cast<std::uint16_t>(p.address + offset), value);
        return;
    }
    const std::span<std::uint8_t> bytes = machine_.bank_bytes(*p.bank);
    const std::size_t at = static_cast<std::size_t>(p.address) + offset;
    if (at >= bytes.size()) {
        fail(bank_end(*p.bank));
    }
    bytes[at] = value;
}

std::string Monitor::listing_line(std::uint16_t address, std::uint8_t* length) const {
    const debug::DisasmOptions options = debug::tvc_disasm_options(symbols_, machine_);
    const debug::Disassembly d = debug::disassemble(machine_, address, options);
    std::string out;
    if (const debug::Symbol* s = symbols_.at(address, machine_)) {
        out += s->name + ":\n";
    }
    std::string bytes;
    for (unsigned i = 0; i < d.length && i < 4; ++i) {
        bytes += hex2(machine_.peek(static_cast<std::uint16_t>(address + i))) + " ";
    }
    out += ".C:" + hex4(address) + "  " + pad(bytes, 13) + (d.comment.empty() ? d.text : pad(d.text, 24) + " ; " + d.comment);
    out += "\n";
    if (length) {
        *length = d.length;
    }
    return out;
}

std::string Monitor::hit_line(const debug::Hit& hit, const char* verb) const {
    std::string what;
    switch (hit.access) {
    case kAccessExec:
        what = "exec " + hex4(hit.address);
        break;
    case kAccessRead:
    case kAccessWrite:
        what = std::string(hit.access == kAccessRead ? "load " : "store ") + hex4(hit.address) + " = " + hex2(hit.value) +
               ", from " + hex4(hit.pc);
        break;
    case kAccessIn:
    case kAccessOut:
        what = std::string(hit.access == kAccessIn ? "in " : "out ") + hex2(hit.address) + " = " + hex2(hit.value) +
               ", from " + hex4(hit.pc);
        break;
    case kAccessIrq:
        what = "irq " + hex4(hit.address);
        break;
    default:
        what = "line #" + std::to_string(hit.address) + ", column #" + std::to_string(machine_.video().beam_column());
        break;
    }
    std::string out = "#" + std::to_string(hit.checkpoint) + " (" + verb + " " + what + ")";
    if (hit.where.bank != Bank::Open) {
        out += "  " + std::string(bank_name(hit.where.bank)) + ":" + hex4(hit.where.offset);
    }
    return out;
}

std::string Monitor::stop_report(const debug::Stop& stop) {
    std::string out;
    for (const debug::Hit& hit : stop.hits) {
        out += hit_line(hit, "Stop on") + "\n";
    }
    listing_cursor_.reset();
    return out + listing_line(machine_.pc());
}

std::string Monitor::describe(const debug::Checkpoint& c) const {
    const bool rows = (c.access & kAccessLine) != 0;
    const auto number = [&](unsigned v) { return rows ? "#" + std::to_string(v) : hex4(v); };
    std::string out = "#" + std::to_string(c.id) + "  " + pad(kind_names(c.access), 10) + " ";
    if (c.bank) {
        out += std::string(bank_name(*c.bank)) + ":";
    }
    out += number(c.start);
    if (c.end != c.start) {
        out += "-" + number(c.end);
    }
    if (c.bank && !c.offsets) {
        out += " in " + std::string(bank_name(*c.bank));
    }
    out += c.stop ? "  stop" : "  trace";
    out += "  hits " + std::to_string(c.hits);
    if (c.ignore > 0) {
        out += "  ignore " + std::to_string(c.ignore);
    }
    if (!c.condition_text.empty()) {
        out += "  if " + c.condition_text;
    }
    if (c.temporary) {
        out += "  temporary";
    }
    if (!c.enabled) {
        out += "  (disabled)";
    }
    return out + "\n";
}

std::string Monitor::enter(std::string_view why) {
    if (active_) {
        return {};
    }
    dbg_.cancel_step();
    pending_next_ = 0;
    std::string out = "(" + std::string(why) + ")\n";
    if (!machine_.at_boundary()) {
        const debug::Stop stop = dbg_.step(1, kStepBudget);
        for (const debug::Hit& hit : stop.hits) {
            out += hit_line(hit, "Stop on") + "\n";
        }
    }
    active_ = true;
    listing_cursor_.reset();
    return out + listing_line(machine_.pc());
}

std::string Monitor::leave() {
    active_ = false;
    return {};
}

std::string Monitor::execute(std::string_view line) {
    std::string text = trim(line);
    if (text.empty() || text[0] == ';') {
        return {};  // `;` starts a comment line in scripts
    }
    try {
        std::string name;
        std::string_view rest;
        if (text[0] == '>') {
            name = ">";
            rest = std::string_view(text).substr(1);
        } else {
            const std::size_t space = text.find_first_of(" \t");
            name = lower(std::string_view(text).substr(0, space));
            rest = space == std::string::npos ? std::string_view{} : std::string_view(text).substr(space);
        }
        for (const Command& c : commands()) {
            std::string_view names = c.names;
            while (!names.empty()) {
                const std::size_t sp = names.find(' ');
                const std::string_view alias = names.substr(0, sp);
                names = sp == std::string_view::npos ? std::string_view{} : names.substr(sp + 1);
                if (alias == name) {
                    Args args;
                    tokenize(rest, args.words, args.quoted, args.condition, args.has_condition);
                    return (this->*c.handler)(args);
                }
            }
        }
        return "error: unknown command '" + name + "' (help lists them)\n";
    } catch (const CommandError& e) {
        return "error: " + e.message + "\n";
    }
}

std::uint64_t Monitor::run(std::uint64_t max_ticks, std::string& report) {
    std::uint64_t ran = 0;
    while (!active_ && ran < max_ticks) {
        apply_key_events(machine_, keys_, next_key_, machine_.frame_count());
        if (next_key_ >= keys_.size()) {
            keys_.clear();
            next_key_ = 0;
        }
        bool frame_slices = false;
        if (media_) {
            media_->media().service(machine_);
            frame_slices = media_->media().busy();
        }
        const debug::Stop stop = dbg_.run(std::min(max_ticks - ran, frame_slices ? 2 * kFrameTicks : kFrameTicks), frame_slices);
        ran += stop.run.ticks_run;
        if (media_) {
            media_->after_slice();
        }
        if (stop.why == debug::Why::Budget) {
            if (stop.run.ticks_run == 0) {
                break;
            }
            continue;
        }
        if (stop.why == debug::Why::Step && pending_next_ > 0) {
            const std::uint64_t left = pending_next_;
            pending_next_ = 0;
            active_ = true;
            const std::string out = next_steps(left);
            if (active_) {
                report += out;
            }
            continue;
        }
        active_ = true;
        last_stop_ = stop;
        ++stop_count_;
        report += stop_report(stop);
    }
    return ran;
}

std::string Monitor::take_trace_lines(std::size_t limit) {
    const std::uint64_t pushed = dbg_.hit_log().pushed();
    const std::size_t fresh = static_cast<std::size_t>(std::min<std::uint64_t>(pushed - trace_seen_, dbg_.hit_log().size()));
    trace_seen_ = pushed;
    std::string out;
    std::size_t lines = 0;
    std::size_t skipped = 0;
    for (const debug::Hit& hit : dbg_.hit_log().last(fresh)) {
        const debug::Checkpoint* c = dbg_.find(hit.checkpoint);
        if (!c || c->stop) {
            continue;
        }
        if (lines < limit) {
            out += hit_line(hit, "Trace") + "\n";
            ++lines;
        } else {
            ++skipped;
        }
    }
    if (skipped > 0) {
        out += "(" + std::to_string(skipped) + " more trace hits)\n";
    }
    return out;
}

// --- execution ---

std::string Monitor::cmd_go(Args& a) {
    if (a.size() > 0) {
        Z80Registers r = dbg_.registers();
        r.pc = static_cast<std::uint16_t>(value(a[0]));
        if (!dbg_.set_registers(r)) {
            fail("not at an instruction boundary");
        }
    }
    return leave();
}

std::string Monitor::cmd_exit(Args&) { return leave(); }

std::string Monitor::cmd_step(Args& a) {
    const std::int64_t count = a.size() > 0 ? value(a[0]) : 1;
    if (count < 1) {
        fail("count must be at least 1");
    }
    const debug::Stop stop = dbg_.step(static_cast<std::uint64_t>(count), kStepBudget);
    std::string out = stop_report(stop);
    if (stop.why == debug::Why::Budget) {
        out = "(stopped after 100 frames)\n" + out;
    }
    return out;
}

std::string Monitor::next_steps(std::uint64_t count) {
    while (count > 0) {
        if (dbg_.start_step_over()) {
            pending_next_ = count - 1;
            return leave();
        }
        const debug::Stop stop = dbg_.step(1, kStepBudget);
        --count;
        if (stop.why == debug::Why::Checkpoint) {
            return stop_report(stop);
        }
    }
    return stop_report({});
}

std::string Monitor::cmd_next(Args& a) {
    const std::int64_t count = a.size() > 0 ? value(a[0]) : 1;
    if (count < 1) {
        fail("count must be at least 1");
    }
    return next_steps(static_cast<std::uint64_t>(count));
}

std::string Monitor::source_line_text(debug::SourceLine line) {
    if (line.file >= sources_.files().size()) {
        return {};
    }
    const std::string& path = sources_.files()[line.file];
    auto [it, inserted] = source_files_.try_emplace(path);
    if (inserted) {
        std::ifstream in(path);
        for (std::string text; std::getline(in, text);) {
            if (!text.empty() && text.back() == '\r') {
                text.pop_back();
            }
            it->second.push_back(text);
        }
    }
    std::string out = std::filesystem::path(path).filename().string() + ":" + std::to_string(line.line);
    if (line.line >= 1 && line.line <= it->second.size()) {
        out += "  " + trim(it->second[line.line - 1]);
    }
    return out;
}

debug::Stop Monitor::step_line(bool over) {
    if (sources_.empty()) {
        return {};
    }
    const std::uint64_t start_ticks = machine_.ticks();
    const std::optional<debug::SourceLine> from = sources_.at(machine_.pc());
    while (true) {
        // Code without source lines (the ROM, a library) is stepped through until a new line comes up
        debug::Stop stop = over ? dbg_.step_over(kStepBudget) : dbg_.step(1, kStepBudget);
        if (stop.why == debug::Why::Checkpoint) {
            return stop;
        }
        const std::optional<debug::SourceLine> now = sources_.at(machine_.pc());
        if (now && now != from) {
            stop.why = debug::Why::Step;
            return stop;
        }
        if (machine_.ticks() - start_ticks > kStepBudget) {
            stop.why = debug::Why::Budget;
            return stop;
        }
    }
}

std::string Monitor::line_steps(std::uint64_t count, bool over) {
    if (sources_.empty()) {
        fail("no source lines: ll a .sld, .lst, .noi, .cdb or .map file first");
    }
    for (; count > 0; --count) {
        const debug::Stop stop = step_line(over);
        if (stop.why == debug::Why::Checkpoint) {
            return stop_report(stop);
        }
        if (stop.why == debug::Why::Budget) {
            return "(no other source line within 100 frames)\n" + stop_report({});
        }
    }
    const std::optional<debug::SourceLine> now = sources_.at(machine_.pc());
    return (now ? source_line_text(*now) + "\n" : std::string()) + stop_report({});
}

std::string Monitor::cmd_line_step(Args& a) {
    const std::int64_t count = a.size() > 0 ? value(a[0]) : 1;
    if (count < 1) {
        fail("count must be at least 1");
    }
    return line_steps(static_cast<std::uint64_t>(count), false);
}

std::string Monitor::cmd_line_next(Args& a) {
    const std::int64_t count = a.size() > 0 ? value(a[0]) : 1;
    if (count < 1) {
        fail("count must be at least 1");
    }
    return line_steps(static_cast<std::uint64_t>(count), true);
}

std::string Monitor::cmd_return(Args&) {
    dbg_.start_step_out();
    return leave();
}

std::string Monitor::cmd_until(Args& a) {
    const Place p = place(a.quoted_word(0, "address"));
    if (p.bank) {
        fail("until takes a CPU address");
    }
    dbg_.start_run_to(p.address, std::nullopt);
    return leave();
}

std::string Monitor::cmd_reset(Args& a) {
    const bool hard = a.size() > 0 && (lower(a[0]) == "hard" || a[0] == "1");
    dbg_.cancel_step();
    if (hard) {
        machine_.power_on();
    } else {
        machine_.reset();
    }
    return std::string(hard ? "(power cycled)\n" : "(reset)\n") + listing_line(machine_.pc());
}

std::string Monitor::cmd_quit(Args& a) {
    quit_ = a.size() > 0 ? static_cast<int>(value(a[0])) : 0;
    return "quit\n";
}

// --- state ---

std::string Monitor::cmd_registers(Args& a) {
    if (a.size() > 0) {
        std::string all;
        for (const std::string& w : a.words) {
            all += w + " ";
        }
        Z80Registers r = dbg_.registers();
        std::size_t start = 0;
        while (start < all.size()) {
            const std::size_t comma = all.find(',', start);
            const std::string item = trim(std::string_view(all).substr(start, comma - start));
            start = comma == std::string::npos ? all.size() : comma + 1;
            if (item.empty()) {
                continue;
            }
            const std::size_t eq = item.find('=');
            if (eq == std::string::npos) {
                fail("expected reg=value, got '" + item + "'");
            }
            const std::string reg = lower(trim(std::string_view(item).substr(0, eq)));
            const auto v = static_cast<std::uint16_t>(value(trim(std::string_view(item).substr(eq + 1))));
            const auto lo = static_cast<std::uint8_t>(v);
            const auto set_hi = [&](std::uint16_t& rr) { rr = static_cast<std::uint16_t>((rr & 0x00FF) | lo << 8); };
            const auto set_lo = [&](std::uint16_t& rr) { rr = static_cast<std::uint16_t>((rr & 0xFF00) | lo); };
            if (reg == "a") set_hi(r.af);
            else if (reg == "f") set_lo(r.af);
            else if (reg == "b") set_hi(r.bc);
            else if (reg == "c") set_lo(r.bc);
            else if (reg == "d") set_hi(r.de);
            else if (reg == "e") set_lo(r.de);
            else if (reg == "h") set_hi(r.hl);
            else if (reg == "l") set_lo(r.hl);
            else if (reg == "ixh") set_hi(r.ix);
            else if (reg == "ixl") set_lo(r.ix);
            else if (reg == "iyh") set_hi(r.iy);
            else if (reg == "iyl") set_lo(r.iy);
            else if (reg == "af") r.af = v;
            else if (reg == "bc") r.bc = v;
            else if (reg == "de") r.de = v;
            else if (reg == "hl") r.hl = v;
            else if (reg == "ix") r.ix = v;
            else if (reg == "iy") r.iy = v;
            else if (reg == "sp") r.sp = v;
            else if (reg == "pc") r.pc = v;
            else if (reg == "wz" || reg == "memptr") r.wz = v;
            else if (reg == "af'") r.af2 = v;
            else if (reg == "bc'") r.bc2 = v;
            else if (reg == "de'") r.de2 = v;
            else if (reg == "hl'") r.hl2 = v;
            else if (reg == "i") r.i = lo;
            else if (reg == "r") r.r = lo;
            else if (reg == "im") r.im = static_cast<std::uint8_t>(v > 2 ? 2 : v);
            else if (reg == "iff1") r.iff1 = v != 0;
            else if (reg == "iff2") r.iff2 = v != 0;
            else fail("unknown register '" + reg + "'");
        }
        if (!dbg_.set_registers(r)) {
            fail("not at an instruction boundary");
        }
    }
    const Z80Registers r = dbg_.registers();
    std::string flags;
    for (int bit = 7; bit >= 0; --bit) {
        flags.push_back((r.af >> bit) & 1 ? '1' : '0');
    }
    const BankAddress where = machine_.memory().locate(r.pc);
    std::string out = "  PC   AF   BC   DE   HL   IX   IY   SP   AF'  BC'  DE'  HL'  I  R  IM IFF WZ   SZ5H3VNC\n";
    out += ".;" + hex4(r.pc) + " " + hex4(r.af) + " " + hex4(r.bc) + " " + hex4(r.de) + " " + hex4(r.hl) + " " + hex4(r.ix) +
           " " + hex4(r.iy) + " " + hex4(r.sp) + " " + hex4(r.af2) + " " + hex4(r.bc2) + " " + hex4(r.de2) + " " +
           hex4(r.hl2) + " " + hex2(r.i) + " " + hex2(r.r) + " " + std::to_string(r.im) + "  " + (r.iff1 ? "1" : "0") +
           (r.iff2 ? "1" : "0") + "  " + hex4(r.wz) + " " + flags + "\n";
    out += "  " + std::string(bank_name(where.bank)) + ":" + hex4(where.offset) + "  frame #" +
           std::to_string(machine_.frame_count()) + "  line #" + std::to_string(machine_.video().beam_line()) +
           "  column #" + std::to_string(machine_.video().beam_column()) + "  T #" +
           std::to_string(machine_.ticks() / kTicksPerTState) + (machine_.halted() ? "  halted" : "") + "\n";
    return out;
}

std::string Monitor::cmd_memory(Args& a) {
    Place start = memory_cursor_.value_or(Place{std::nullopt, machine_.pc()});
    std::size_t length = kDefaultMemory;
    if (a.size() > 0) {
        start = place(a[0]);
    }
    if (a.size() > 1) {
        const Place end = place(a[1]);
        if (end.address < start.address) {
            fail("end before start");
        }
        length = static_cast<std::size_t>(end.address - start.address) + 1;
    } else if (start.bank) {
        // Without an end the listing stops at the bank's end
        length = std::min(length, machine_.memory().bank_bytes(*start.bank).size() - start.address);
    }
    std::string out;
    const std::string prefix = start.bank ? std::string(">") + bank_name(*start.bank) + ":" : ">C:";
    for (std::size_t row = 0; row < length; row += 16) {
        std::string hex;
        std::string ascii;
        for (std::size_t i = row; i < row + 16 && i < length; ++i) {
            const std::uint8_t b = read(start, static_cast<unsigned>(i));
            hex += hex2(b) + (i % 16 == 7 ? "  " : " ");
            ascii.push_back(b >= 0x20 && b < 0x7F ? static_cast<char>(b) : '.');
        }
        out += prefix + hex4(static_cast<unsigned>(start.address + row)) + "  " + pad(hex, 50) + ascii + "\n";
    }
    memory_cursor_ = Place{start.bank, static_cast<std::uint16_t>(start.address + length)};
    return out;
}

std::string Monitor::cmd_disassemble(Args& a) {
    std::uint16_t address = listing_cursor_.value_or(machine_.pc());
    std::optional<std::uint16_t> end;
    if (a.size() > 0) {
        const Place p = place(a[0]);
        if (p.bank) {
            fail("d takes CPU addresses; use m for a bank that is not paged in");
        }
        address = p.address;
    }
    if (a.size() > 1) {
        end = static_cast<std::uint16_t>(value(a[1]));
    }
    std::string out;
    for (int n = 0; n < 4096; ++n) {
        if (end ? address > *end : n >= kDefaultListing) {
            break;
        }
        std::uint8_t length = 1;
        out += listing_line(address, &length);
        if (static_cast<unsigned>(address) + length > 0xFFFF) {
            address = 0;
            break;
        }
        address = static_cast<std::uint16_t>(address + length);
    }
    listing_cursor_ = address;
    return out;
}

std::string Monitor::cmd_write(Args& a) {
    const Place p = place(a.quoted_word(0, "address"));
    if (a.size() < 2) {
        fail("no bytes");
    }
    // Every value and the whole range first, so a bad one writes nothing
    std::vector<std::uint8_t> bytes;
    for (std::size_t i = 1; i < a.size(); ++i) {
        bytes.push_back(static_cast<std::uint8_t>(value(a[i])));
    }
    if (p.bank && p.address + bytes.size() > machine_.memory().bank_bytes(*p.bank).size()) {
        fail(bank_end(*p.bank));
    }
    for (std::size_t i = 0; i < bytes.size(); ++i) {
        write(p, static_cast<unsigned>(i), bytes[i]);
    }
    return {};
}

std::string Monitor::cmd_fill(Args& a) {
    if (a.size() < 3) {
        fail("usage: f start end byte ...");
    }
    const Place start = place(a[0]);
    const Place end = place(a[1]);
    if (end.address < start.address) {
        fail("end before start");
    }
    std::vector<std::uint8_t> pattern;
    for (std::size_t i = 2; i < a.size(); ++i) {
        pattern.push_back(static_cast<std::uint8_t>(value(a[i])));
    }
    for (unsigned i = 0; i <= static_cast<unsigned>(end.address - start.address); ++i) {
        write(start, i, pattern[i % pattern.size()]);
    }
    return {};
}

std::string Monitor::cmd_hunt(Args& a) {
    if (a.size() < 3) {
        fail("usage: h start end byte ...");
    }
    const Place start = place(a[0]);
    const Place end = place(a[1]);
    std::vector<std::uint8_t> pattern;
    for (std::size_t i = 2; i < a.size(); ++i) {
        pattern.push_back(static_cast<std::uint8_t>(value(a[i])));
    }
    std::string out;
    unsigned found = 0;
    for (unsigned i = 0; start.address + i + pattern.size() - 1 <= end.address; ++i) {
        bool match = true;
        for (std::size_t k = 0; k < pattern.size() && match; ++k) {
            match = read(start, static_cast<unsigned>(i + k)) == pattern[k];
        }
        if (match) {
            if (found < 256) {
                out += (start.bank ? std::string(bank_name(*start.bank)) + ":" : "") + hex4(start.address + i) + "\n";
            }
            ++found;
        }
    }
    return out + std::to_string(found) + " found\n";
}

std::string Monitor::cmd_transfer(Args& a) {
    if (a.size() != 3) {
        fail("usage: t start end destination");
    }
    const Place start = place(a[0]);
    const Place end = place(a[1]);
    const Place dest = place(a[2]);
    if (end.address < start.address) {
        fail("end before start");
    }
    std::vector<std::uint8_t> buffer;
    for (unsigned i = 0; i <= static_cast<unsigned>(end.address - start.address); ++i) {
        buffer.push_back(read(start, i));
    }
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        write(dest, static_cast<unsigned>(i), buffer[i]);
    }
    return {};
}

std::string Monitor::cmd_compare(Args& a) {
    if (a.size() != 3) {
        fail("usage: c start end other");
    }
    const Place start = place(a[0]);
    const Place end = place(a[1]);
    const Place other = place(a[2]);
    std::string out;
    unsigned differ = 0;
    for (unsigned i = 0; start.address + i <= end.address; ++i) {
        const std::uint8_t x = read(start, i);
        const std::uint8_t y = read(other, i);
        if (x != y) {
            if (differ < 64) {
                out += hex4(start.address + i) + ": " + hex2(x) + "  " + hex4(other.address + i) + ": " + hex2(y) + "\n";
            }
            ++differ;
        }
    }
    return out + std::to_string(differ) + " differences\n";
}

std::string Monitor::cmd_bank(Args&) {
    const MemoryMap& map = machine_.memory();
    std::string out;
    for (unsigned page = 0; page < 4; ++page) {
        const auto base = static_cast<std::uint16_t>(page * 0x4000);
        const BankAddress low = map.locate(base);
        out += "page " + std::to_string(page) + "  " + hex4(base) + "-" + hex4(base + 0x3FFF) + "  " + bank_name(low.bank);
        if (page == 3 && low.bank != map.locate(0xE000).bank) {
            out += " (C000-DFFF), " + std::string(bank_name(map.locate(0xE000).bank)) + " (E000-FFFF)";
        }
        out += "\n";
    }
    out += "port 02h = " + hex2(map.page_register()) + "  IOMEM slot " + std::to_string(map.iomem_slot()) +
           "  video paging = " + hex2(map.video_paging()) + "  displayed VID" + std::to_string(map.displayed_bank()) + "\n";
    return out;
}

std::string Monitor::cmd_io(Args&) {
    const Video& v = machine_.video();
    static constexpr std::array<const char*, 4> kModes{"2-colour", "4-colour", "16-colour", "16-colour"};
    std::string out = "video: mode " + std::string(kModes[v.mode()]) + "  border " + hex2(v.border()) + "  palette " +
                      hex2(v.palette(0)) + " " + hex2(v.palette(1)) + " " + hex2(v.palette(2)) + " " + hex2(v.palette(3)) +
                      "  beam line #" + std::to_string(v.beam_line()) + " column #" + std::to_string(v.beam_column()) + "\n";
    out += "CRTC R0-R17:";
    for (const std::uint8_t r : v.crtc().registers()) {
        out += " " + hex2(r);
    }
    out += "  (R" + std::to_string(v.crtc().selected()) + " selected)\n";
    const Sound& s = machine_.sound();
    out += "sound: pitch " + hex4(s.pitch()) + "  tone " + (s.tone_enabled() ? "on" : "off") + "  amplitude " +
           std::to_string(s.amplitude()) + "  IRQ " + (s.irq_enabled() ? "on" : "off") + "\n";
    out += std::string("IRQ flip-flop: ") + (machine_.irq_pending() ? "set" : "clear") + "  IFF1 " +
           (dbg_.registers().iff1 ? "1" : "0") + "  IM " + std::to_string(dbg_.registers().im) + "\n";
    return out;
}

std::string Monitor::cmd_stopwatch(Args& a) {
    if (a.size() > 0) {
        stopwatch_ = machine_.ticks();
        return "stopwatch reset\n";
    }
    const std::uint64_t ticks = machine_.ticks() - stopwatch_;
    return "#" + std::to_string(ticks / kTicksPerTState) + " T (#" + std::to_string(ticks) + " half-T) since the reset\n";
}

std::string Monitor::cmd_history(Args& a) {
    if (a.size() > 0 && lower(a[0]) == "off") {
        dbg_.set_trace_capacity(0);
        return "history off\n";
    }
    if (a.size() > 0 && lower(a[0]) == "on") {
        dbg_.set_trace_capacity(kHistory);
        return "history on (" + std::to_string(kHistory) + " instructions)\n";
    }
    if (dbg_.trace().capacity() == 0) {
        return "history is off (chis on)\n";
    }
    const std::size_t count = a.size() > 0 ? static_cast<std::size_t>(value(a[0])) : 16;
    const debug::DisasmOptions options = debug::tvc_disasm_options(symbols_, machine_);
    std::string out;
    for (const debug::TraceEntry& e : dbg_.trace().last(count)) {
        const debug::Disassembly d = debug::disassemble(e.bytes, e.regs.pc, options);
        out += ".C:" + hex4(e.regs.pc) + "  " + pad(d.text, 24) + " AF " + hex4(e.regs.af) + " BC " + hex4(e.regs.bc) +
               " DE " + hex4(e.regs.de) + " HL " + hex4(e.regs.hl) + " SP " + hex4(e.regs.sp) + "  T #" +
               std::to_string(e.ticks / kTicksPerTState) + (e.interrupt ? "  (interrupt)" : "") + "\n";
    }
    return out;
}

std::string Monitor::cmd_profile(Args& a) {
    const std::string op = a.size() > 0 ? lower(a[0]) : std::string();
    if (op == "on" || op == "off") {
        dbg_.set_profile(op == "on");
        return op == "on" ? "profile on\n" : "profile off (the counts are kept)\n";
    }
    if (op == "clear") {
        dbg_.clear_profile();
        return "profile cleared\n";
    }
    const std::size_t count = op.empty() ? 16 : static_cast<std::size_t>(value(a[0]));
    const debug::Profile& p = dbg_.profile();
    const auto t_states = [](std::uint64_t ticks) { return std::to_string(ticks / kTicksPerTState); };
    std::string out = std::string(dbg_.profile_enabled() ? "profile on" : "profile off") + ": " + std::to_string(p.instructions()) +
                      " instructions, " + t_states(p.total_ticks()) + " T\n";
    std::vector<std::size_t> order(p.routines().size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        order[i] = i;
    }
    std::sort(order.begin(), order.end(), [&](std::size_t x, std::size_t y) { return p.routines()[x].exclusive > p.routines()[y].exclusive; });
    for (std::size_t n = 0; n < order.size() && n < count; ++n) {
        const debug::RoutineStats& r = p.routines()[order[n]];
        char share[16];
        std::snprintf(share, sizeof share, "%5.1f%%", p.total_ticks() ? 100.0 * static_cast<double>(r.exclusive) / static_cast<double>(p.total_ticks()) : 0.0);
        std::string name = "(outside any call)";
        if (r.entry.bank != Bank::Open) {
            name = std::string(bank_name(r.entry.bank)) + ":" + hex4(r.entry.offset);
            for (unsigned base = 0; base < 0x10000; base += 0x2000) {
                const BankAddress w = machine_.memory().locate(static_cast<std::uint16_t>(base));
                if (w.bank == r.entry.bank && r.entry.offset >= w.offset && r.entry.offset < w.offset + 0x2000u) {
                    const auto cpu = static_cast<std::uint16_t>(base + r.entry.offset - w.offset);
                    name = hex4(cpu) + " " + name;
                    if (const debug::Symbol* s = symbols_.at(cpu, r.entry)) {
                        name += " " + s->name;
                    }
                    break;
                }
            }
        }
        out += std::string(share) + "  excl " + pad(t_states(r.exclusive), 10) + " incl " + pad(t_states(p.inclusive(order[n], machine_.ticks())), 10) +
               " calls " + pad(std::to_string(r.calls), 7) + " " + name + "\n";
    }
    return out;
}

std::string Monitor::cmd_snapshot(Args& a) {
    const std::string op = a.size() > 0 ? lower(a[0]) : "list";
    if (op == "list") {
        std::string out;
        for (const std::string& n : dbg_.state_names()) {
            out += n + "\n";
        }
        return out.empty() ? "no states\n" : out;
    }
    const std::string name = a.quoted_word(1, "name");
    if (op == "save") {
        dbg_.save_state(name);
        return "saved '" + name + "'\n";
    }
    if (op == "load") {
        if (!dbg_.load_state(name)) {
            fail("no state '" + name + "'");
        }
        listing_cursor_.reset();
        return "loaded '" + name + "'\n" + listing_line(machine_.pc());
    }
    if (op == "delete") {
        if (!dbg_.delete_state(name)) {
            fail("no state '" + name + "'");
        }
        return "deleted '" + name + "'\n";
    }
    fail("snap save|load|delete name, or snap list");
}

// --- checkpoints ---

std::string Monitor::checkpoint_command(Args& a, std::uint8_t default_access, bool stop, const char* name) {
    if (a.size() == 0 && !a.has_condition && std::string_view(name) == "break") {
        std::string out;
        for (const debug::Checkpoint& c : dbg_.checkpoints()) {
            out += describe(c);
        }
        return out.empty() ? "no checkpoints\n" : out;
    }
    std::uint8_t access = 0;
    std::size_t i = 0;
    for (; i < a.size(); ++i) {
        const std::string word = lower(a[i]);
        const auto kind = std::find_if(kKinds.begin(), kKinds.end(), [&](const Kind& k) { return word == k.name; });
        if (kind == kKinds.end()) {
            break;
        }
        access |= kind->access;
    }
    if (access == 0) {
        access = default_access;
    }
    debug::Checkpoint c;
    c.access = access;
    c.stop = stop;
    if (i < a.size()) {
        const Place start = place(a[i]);
        Place end = start;
        if (i + 1 < a.size()) {
            end = place(a[i + 1]);
        }
        if (i + 2 < a.size()) {
            fail("too many arguments");
        }
        if (start.bank) {
            if (access & ~(kAccessExec | kAccessRead | kAccessWrite)) {
                fail("BANK:offset works for exec, load and store");
            }
            c.bank = start.bank;
            c.offsets = true;
        }
        c.start = start.address;
        c.end = end.address;
    } else if (access == kAccessIrq) {
        c.start = 0;
        c.end = 0xFFFF;
    } else {
        fail("missing address");
    }
    if (c.end < c.start) {
        fail("end before start");
    }
    if ((access & (kAccessIn | kAccessOut)) && c.end > 0xFF) {
        fail("ports are 00-FF");
    }
    if ((access & kAccessLine) && c.end > 511) {
        fail("rows are #0-#511 (write decimal rows with #)");
    }
    if (a.has_condition) {
        const auto [e, error] = debug::Expression::parse(a.condition, parse_options());
        if (!e) {
            fail("condition: " + error.message);
        }
        c.condition = debug::make_condition(*e);
        c.condition_text = a.condition;
    }
    const unsigned id = dbg_.add(std::move(c));
    return describe(*dbg_.find(id));
}

std::string Monitor::cmd_break(Args& a) { return checkpoint_command(a, kAccessExec, true, "break"); }
std::string Monitor::cmd_watch(Args& a) { return checkpoint_command(a, kAccessRead | kAccessWrite, true, "watch"); }
std::string Monitor::cmd_trace(Args& a) { return checkpoint_command(a, kAccessExec, false, "trace"); }

std::string Monitor::cmd_delete(Args& a) {
    if (a.size() == 0) {
        const std::size_t n = dbg_.checkpoints().size();
        dbg_.clear_checkpoints();
        return "deleted " + std::to_string(n) + " checkpoint(s)\n";
    }
    for (const std::string& w : a.words) {
        if (!dbg_.remove(checkpoint_id(w))) {
            fail("no checkpoint " + w);
        }
    }
    return {};
}

std::string Monitor::cmd_enable(Args& a) {
    const debug::Checkpoint* found = dbg_.find(checkpoint_id(a.quoted_word(0, "checkpoint number")));
    if (!found) {
        fail("no checkpoint " + a[0]);
    }
    debug::Checkpoint c = *found;
    c.enabled = true;
    dbg_.update(c);
    return describe(*dbg_.find(c.id));
}

std::string Monitor::cmd_disable(Args& a) {
    const debug::Checkpoint* found = dbg_.find(checkpoint_id(a.quoted_word(0, "checkpoint number")));
    if (!found) {
        fail("no checkpoint " + a[0]);
    }
    debug::Checkpoint c = *found;
    c.enabled = false;
    dbg_.update(c);
    return describe(*dbg_.find(c.id));
}

std::string Monitor::cmd_condition(Args& a) {
    const debug::Checkpoint* found = dbg_.find(checkpoint_id(a.quoted_word(0, "checkpoint number")));
    if (!found) {
        fail("no checkpoint " + a[0]);
    }
    std::string text = a.condition;
    for (std::size_t i = 1; i < a.size(); ++i) {
        text += (text.empty() ? "" : " ") + a[i];
    }
    debug::Checkpoint c = *found;
    if (trim(text).empty()) {
        c.condition = nullptr;
        c.condition_text.clear();
    } else {
        const auto [e, error] = debug::Expression::parse(text, parse_options());
        if (!e) {
            fail("condition: " + error.message);
        }
        c.condition = debug::make_condition(*e);
        c.condition_text = trim(text);
    }
    dbg_.update(c);
    return describe(*dbg_.find(c.id));
}

std::string Monitor::cmd_ignore(Args& a) {
    const debug::Checkpoint* found = dbg_.find(checkpoint_id(a.quoted_word(0, "checkpoint number")));
    if (!found) {
        fail("no checkpoint " + a[0]);
    }
    debug::Checkpoint c = *found;
    c.ignore = a.size() > 1 ? static_cast<std::uint32_t>(value(a[1])) : 1;
    dbg_.update(c);
    return describe(*dbg_.find(c.id));
}

// --- symbols, files, input ---

std::optional<debug::SourceMap::Loaded> Monitor::load_debug_file(const std::string& path, std::string& error) {
    const auto bytes = read_file(path);
    if (!bytes) {
        error = "cannot read '" + path + "'";
        return std::nullopt;
    }
    symbols_.remove_source(path);
    sources_.remove_source(path);
    source_files_.clear();
    const std::string directory = std::filesystem::path(path).parent_path().generic_string();
    return sources_.load(std::string_view(reinterpret_cast<const char*>(bytes->data()), bytes->size()), path, directory, symbols_);
}

std::string Monitor::cmd_load_labels(Args& a) {
    const std::string path = a.quoted_word(0, "file name");
    std::string error;
    const std::optional<debug::SourceMap::Loaded> result = load_debug_file(path, error);
    if (!result) {
        fail(error);
    }
    std::string out = "loaded " + std::to_string(result->labels) + " labels";
    if (result->format != debug::SourceMap::Format::Plain) {
        out += " and " + std::to_string(result->lines) + " source lines (" + debug::SourceMap::format_name(result->format) + ")";
    }
    out += " from " + path + "\n";
    for (std::size_t i = 0; i < result->errors.size() && i < 5; ++i) {
        out += "  " + result->errors[i] + "\n";
    }
    if (result->errors.size() > 5) {
        out += "  (" + std::to_string(result->errors.size() - 5) + " more errors)\n";
    }
    return out;
}

std::string Monitor::cmd_add_label(Args& a) {
    const Place p = place(a.quoted_word(0, "address"));
    const std::string name = a.quoted_word(1, "name");
    if (p.bank) {
        fail("al takes a CPU address");
    }
    symbols_.add({name, p.address, std::nullopt, "monitor"});
    return {};
}

std::string Monitor::cmd_delete_label(Args& a) {
    const std::string name = a.quoted_word(0, "name");
    if (!symbols_.remove_name(name)) {
        fail("no label '" + name + "'");
    }
    return {};
}

std::string Monitor::cmd_show_labels(Args& a) {
    const std::string filter = a.size() > 0 ? lower(a[0]) : std::string();
    std::string out;
    std::size_t shown = 0;
    for (const debug::Symbol& s : symbols_.symbols()) {
        if (!filter.empty() && lower(s.name).find(filter) == std::string::npos) {
            continue;
        }
        if (shown++ >= 1000) {
            continue;
        }
        out += pad(s.name, 20) + " = " + hex4(s.address);
        if (s.home) {
            out += "  " + std::string(bank_name(s.home->bank)) + ":" + hex4(s.home->offset);
        }
        out += "  (" + s.source + ")\n";
    }
    if (shown > 1000) {
        out += "(" + std::to_string(shown - 1000) + " more)\n";
    }
    return out + std::to_string(shown) + " labels\n";
}

std::string Monitor::cmd_clear_labels(Args&) {
    std::set<std::string> sources;
    for (const debug::Symbol& s : symbols_.symbols()) {
        if (s.source.rfind("rom:", 0) != 0) {
            sources.insert(s.source);
        }
    }
    std::size_t removed = 0;
    for (const std::string& source : sources) {
        removed += symbols_.remove_source(source);
    }
    sources_.clear();
    source_files_.clear();
    return "cleared " + std::to_string(removed) + " labels and the source lines\n";
}

std::string Monitor::cmd_source(Args& a) {
    const Place p = a.size() > 0 ? place(a[0]) : Place{std::nullopt, machine_.pc()};
    if (p.bank) {
        fail("src takes a CPU address");
    }
    const std::optional<debug::SourceLine> line = sources_.at(p.address);
    if (!line) {
        return sources_.empty() ? "no source lines loaded (ll)\n" : hex4(p.address) + ": no source line\n";
    }
    return hex4(p.address) + "  " + source_line_text(*line) + "\n";
}

std::string Monitor::cmd_bload(Args& a) {
    const std::string path = a.quoted_word(0, "file name");
    const Place p = place(a.quoted_word(1, "address"));
    const auto bytes = read_file(path);
    if (!bytes) {
        fail("cannot read '" + path + "'");
    }
    if (bytes->size() > 0x10000) {
        fail("'" + path + "' is larger than 64 KiB");
    }
    for (std::size_t i = 0; i < bytes->size(); ++i) {
        write(p, static_cast<unsigned>(i), (*bytes)[i]);
    }
    return "loaded " + std::to_string(bytes->size()) + " bytes at " +
           (p.bank ? std::string(bank_name(*p.bank)) + ":" : "") + hex4(p.address) + "\n";
}

std::string Monitor::cmd_bsave(Args& a) {
    const std::string path = a.quoted_word(0, "file name");
    const Place start = place(a.quoted_word(1, "start"));
    const Place end = place(a.quoted_word(2, "end"));
    if (end.address < start.address) {
        fail("end before start");
    }
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        fail("cannot write '" + path + "'");
    }
    const unsigned length = static_cast<unsigned>(end.address - start.address) + 1;
    for (unsigned i = 0; i < length; ++i) {
        out.put(static_cast<char>(read(start, i)));
    }
    return "saved " + std::to_string(length) + " bytes to " + path + "\n";
}

std::string Monitor::cmd_screenshot(Args& a) {
    const std::string path = a.quoted_word(0, "file name");
    const bool full = a.size() > 1 && lower(a[1]) == "full";
    const ScreenArea area = full ? kFullRaster : kTvArea;
    std::string error;
    if (!write_png(path, machine_.framebuffer(), area, error)) {
        fail(error);
    }
    return "saved " + path + " (" + std::to_string(area.width) + " x " + std::to_string(area.height) + ")\n";
}

std::string Monitor::cmd_keybuf(Args& a) {
    const std::string text = unescape(a.quoted_word(0, "text"));
    const auto chars = decode_utf8(text);
    if (!chars) {
        fail("text is not UTF-8");
    }
    const std::uint64_t start = std::max<std::uint64_t>(machine_.frame_count() + 1, keys_.empty() ? 0 : keys_.back().frame + 3);
    const auto end = queue_text(*chars);
    if (!end) {
        fail("no TVC key types one of the characters");
    }
    return "typing " + std::to_string(chars->size()) + " characters from frame #" + std::to_string(start) +
           " (until #" + std::to_string(*end) + ") when the machine runs\n";
}

std::string Monitor::cmd_attach(Args& a) {
    if (!media_) {
        fail("this front end has no program media");
    }
    if (a.size() == 0) {
        return media_->describe() + "\n";
    }
    std::string error;
    if (!media_->attach(a.quoted_word(0, "file name"), error)) {
        fail(error);
    }
    return media_->describe() + "\n";
}

std::string Monitor::cmd_detach(Args&) {
    if (!media_) {
        fail("this front end has no program media");
    }
    media_->detach();
    return media_->describe() + "\n";
}

std::string Monitor::cmd_autostart(Args& a) {
    if (!media_) {
        fail("this front end has no program media");
    }
    std::string error;
    dbg_.cancel_step();
    if (!media_->autostart(a.quoted_word(0, "file name"), error)) {
        fail(error);
    }
    leave();
    return "autostart: " + media_->describe() + "\n";
}

std::string Monitor::cmd_tape(Args& a) {
    if (!media_) {
        fail("this front end has no program media");
    }
    std::string error;
    const std::string what = a.size() > 0 ? lower(a[0]) : std::string();
    bool ok = true;
    if (what.empty()) {
        // status only
    } else if (what == "insert") {
        ok = media_->tape_insert(a.quoted_word(1, "file name"), error);
    } else if (what == "autostart") {
        dbg_.cancel_step();
        ok = media_->tape_autostart(a.quoted_word(1, "file name"), error);
        if (ok) {
            leave();
        }
    } else if (what == "record") {
        ok = media_->tape_record(a.quoted_word(1, "file name"), error);
    } else if (what == "play") {
        media_->tape_play();
    } else if (what == "stop") {
        ok = media_->tape_stop(error);
    } else if (what == "rewind") {
        media_->tape_rewind();
    } else if (what == "eject") {
        ok = media_->tape_eject(error);
    } else {
        fail("tape takes insert, autostart, record, play, stop, rewind or eject");
    }
    if (!ok) {
        fail(error);
    }
    return media_->tape_describe() + "\n";
}

std::string Monitor::cmd_dump(Args& a) {
    if (!media_) {
        fail("this front end has no program media");
    }
    const std::string path = a.quoted_word(0, "file name");
    std::string error;
    if (!media_->snapshot_save(path, error)) {
        fail(error);
    }
    return "snapshot saved to " + path + "\n";
}

std::string Monitor::cmd_undump(Args& a) {
    if (!media_) {
        fail("this front end has no program media");
    }
    std::string error;
    dbg_.cancel_step();
    if (!media_->snapshot_load(a.quoted_word(0, "file name"), error)) {
        fail(error);
    }
    machine_changed();
    return "snapshot loaded\n" + listing_line(machine_.pc());
}

std::string Monitor::cmd_rewind(Args& a) {
    if (!media_) {
        fail("this front end has no program media");
    }
    unsigned seconds = 1;
    if (a.size() > 0) {
        const std::string& word = a[0];
        if (word.empty() || word.size() > 3 || !std::all_of(word.begin(), word.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
            fail("seconds are decimal, 1-999");
        }
        seconds = static_cast<unsigned>(std::stoul(word));
    }
    std::string error;
    dbg_.cancel_step();
    if (!media_->rewind(seconds, error)) {
        fail(error + " (" + media_->rewind_describe() + ")");
    }
    return media_->rewind_describe() + "\n" + listing_line(machine_.pc());
}

std::string Monitor::cmd_cart(Args& a) {
    if (!media_) {
        fail("this front end has no program media");
    }
    if (a.size() == 0) {
        return media_->cart_describe() + "\n";
    }
    if (lower(a[0]) == "build") {
        if (a.size() < 3) {
            fail("cart build \"out\" en|hu|es \"file\"...");
        }
        std::vector<std::string> files;
        for (std::size_t i = 3; i < a.size(); ++i) {
            files.push_back(a.quoted_word(i, "file name"));
        }
        std::string error;
        const std::string out = a.quoted_word(1, "file name");
        if (!media_->multicart_build(out, files, lower(a[2]), error)) {
            fail(error);
        }
        return "MultiCart image " + out + " written with " + std::to_string(files.size()) + " files\n";
    }
    if (lower(a[0]) == "remove") {
        media_->cart_remove();
    } else {
        const std::string path = a.quoted_word(0, "file name");
        const std::string type = a.size() > 1 ? lower(a[1]) : std::string();
        unsigned bank = 0;
        if (a.size() > 2) {
            const std::string& word = a[2];
            if (word.empty() || word.size() > 2 || !std::all_of(word.begin(), word.end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
                fail("the bank is decimal, 0-63");
            }
            bank = static_cast<unsigned>(std::stoul(word));
        }
        std::string error;
        if (!media_->cart_insert(path, type == "auto" ? std::string() : type, bank, error)) {
            fail(error);
        }
    }
    dbg_.cancel_step();
    machine_changed();
    return media_->cart_describe() + " (powered on)\n";
}

std::string Monitor::cmd_gamecard(Args& a) {
    return switch_card(a, "gamecard on [slot] | off", kGameCardSlot, &MediaControl::gamecard, &MediaControl::gamecard_describe);
}

std::string Monitor::cmd_twodfx(Args& a) {
    return switch_card(a, "2dfx on [slot] | off", kCardSlots, &MediaControl::twodfx, &MediaControl::twodfx_describe);
}

std::string Monitor::cmd_multichip(Args& a) {
    return switch_card(a, "multichip on [slot] | off", kMultichipSlot, &MediaControl::multichip, &MediaControl::multichip_describe);
}

std::string Monitor::cmd_sid(Args& a) {
    if (!media_) {
        fail("this front end has no program media");
    }
    if (a.size() == 0) {
        return media_->sid_card_describe() + "\n";
    }
    const std::string before = media_->sid_card_model();
    std::string error;
    if (!media_->sid_card(lower(a[0]), error)) {
        fail(error);
    }
    if (media_->sid_card_model() == before) {
        return media_->sid_card_describe() + "\n";
    }
    dbg_.cancel_step();
    machine_changed();
    return media_->sid_card_describe() + " (powered on)\n";
}

std::string Monitor::cmd_nanosd(Args& a) {
    if (media_ && a.size() > 0 && lower(a[0]) == "sd") {
        if (a.size() != 2) {
            fail("nanosd sd \"folder\"|none");
        }
        std::string error;
        if (!media_->nanosd_sd(lower(a[1]) == "none" ? std::string("none") : a[1], error)) {
            fail(error);
        }
        return media_->nanosd_describe() + "\n";
    }
    return switch_card(a, "nanosd on [slot] | off | sd \"folder\"|none", kNanoSdSlot, &MediaControl::nanosd, &MediaControl::nanosd_describe);
}

std::string Monitor::cmd_printer(Args& a) {
    if (!media_) {
        fail("this front end has no program media");
    }
    if (a.size() == 0) {
        return media_->printer_describe() + "\n";
    }
    const std::string verb = lower(a[0]);
    if (verb == "read" && a.size() == 1) {
        return escape_bytes(media_->printer_take()) + "\n";
    }
    if (verb == "off" && a.size() == 1) {
        media_->printer_unplug();
        return media_->printer_describe() + "\n";
    }
    std::string raw, text, error;
    if (!(verb == "on" && a.size() == 1)) {
        if (a.size() > 2) {
            fail("printer on | off | \"file\" [\"file\"] | read");
        }
        for (std::size_t i = 0; i < a.size(); ++i) {
            const std::string file = a.quoted_word(i, "file name");
            (lower(file).ends_with(".txt") ? text : raw) = file;
        }
    }
    if (!media_->printer_plug(raw, text, error)) {
        fail(error);
    }
    return media_->printer_describe() + "\n";
}

std::string Monitor::cmd_serial(Args& a) {
    if (!media_) {
        fail("this front end has no program media");
    }
    const std::string verb = a.size() > 0 ? lower(a[0]) : std::string();
    std::string error;
    if (verb == "read" && a.size() == 1) {
        return escape_bytes(media_->serial_take()) + "\n";
    }
    if ((verb == "line" || verb == "flow" || verb == "send" || verb == "file" || verb == "capture") && a.size() == 2) {
        const std::string arg = a.quoted_word(1, "argument");
        bool ok = true;
        if (verb == "line") {
            ok = media_->serial_peer(lower(arg), error);
        } else if (verb == "flow") {
            if (lower(arg) != "rts" && lower(arg) != "none") {
                fail("serial flow rts|none");
            }
            media_->serial_flow(lower(arg) == "rts");
        } else if (verb == "send") {
            media_->serial_queue(unescape(arg, false));
        } else if (verb == "file") {
            ok = media_->serial_queue_file(arg, error);
        } else {
            ok = media_->serial_capture(lower(arg) == "none" ? std::string() : arg, error);
        }
        if (!ok) {
            fail(error);
        }
        return media_->serial_describe() + "\n";
    }
    return switch_card(a, "serial on [slot] | off | line off|on|listen[:port]|connect:port | flow rts|none | send \"text\" | file \"file\" | "
                          "capture \"file\"|none | read",
                       kSerialSlot, &MediaControl::serial_card, &MediaControl::serial_describe);
}

std::string Monitor::cmd_sddual(Args& a) {
    if (!media_) {
        fail("this front end has no program media");
    }
    if (a.size() == 0) {
        return media_->sd_describe() + "\n";
    }
    const std::string verb = lower(a[0]);
    std::string error;
    const auto socket = [&] {
        if (a.size() < 2 || (a[1] != "0" && a[1] != "1")) {
            fail("the socket is 0 (microSD) or 1 (SD)");
        }
        return a[1] == "0" ? 0u : 1u;
    };
    if (verb == "insert") {
        const unsigned s = socket();
        if (a.size() != 3) {
            fail("sddual insert 0|1 \"file\"");
        }
        if (!media_->sd_insert(s, a.quoted_word(2, "file name"), error)) {
            fail(error);
        }
        return media_->sd_describe() + "\n";
    }
    if (verb == "eject") {
        media_->sd_eject(socket());
        return media_->sd_describe() + "\n";
    }
    if (verb == "build") {
        if (a.size() < 3 || a[2].empty() || !std::all_of(a[2].begin(), a[2].end(), [](char ch) { return ch >= '0' && ch <= '9'; })) {
            fail("sddual build \"out\" mib \"path\"...");
        }
        std::vector<std::string> files;
        for (std::size_t i = 3; i < a.size(); ++i) {
            files.push_back(a.quoted_word(i, "file name"));
        }
        const std::string out = a.quoted_word(1, "file name");
        if (!media_->sd_build(out, std::stoull(a[2]), files, error)) {
            fail(error);
        }
        return "SD card image " + out + " written with " + std::to_string(files.size()) + (files.size() == 1 ? " path\n" : " paths\n");
    }
    if ((verb != "on" && verb != "off") || a.size() > 2 || (a.size() == 2 && (verb != "on" || lower(a[1]) != "gm"))) {
        fail("sddual on [gm] | off | insert 0|1 \"file\" | eject 0|1 | build \"out\" mib \"path\"...");
    }
    if (!media_->sddual(verb == "on", a.size() == 2, error)) {
        fail(error);
    }
    dbg_.cancel_step();
    machine_changed();
    return media_->sd_describe() + " (powered on)\n";
}

std::string Monitor::switch_card(Args& a, const char* usage, unsigned default_slot,
                                 bool (MediaControl::*place_card)(bool, unsigned, std::string&),
                                 std::string (MediaControl::*describe_card)() const) {
    if (!media_) {
        fail("this front end has no program media");
    }
    if (a.size() == 0) {
        return (media_->*describe_card)() + "\n";
    }
    const std::string what = lower(a[0]);
    unsigned slot = default_slot;
    if (what == "on" && a.size() > 1) {
        if (a[1].size() != 1 || a[1][0] < '0' || a[1][0] > '3') {
            fail("the slot is 0-3");
        }
        slot = static_cast<unsigned>(a[1][0] - '0');
    } else if (what != "on" && what != "off") {
        fail(usage);
    }
    std::string error;
    if (!(media_->*place_card)(what == "on", slot, error)) {
        fail(error);
    }
    dbg_.cancel_step();
    machine_changed();
    std::string out = (media_->*describe_card)() + " (powered on)\n";
    for (const std::string& line : media_->take_messages()) {
        out += line + "\n";
    }
    return out;
}

std::string Monitor::cmd_card256(Args& a) {
    if (!media_) {
        fail("this front end has no program media");
    }
    if (a.size() == 0) {
        return media_->card256_describe() + "\n";
    }
    const std::string what = lower(a[0]);
    std::string error;
    if (what == "usb" || what == "flash") {
        if (a.size() != 2) {
            fail("card256 usb \"folder\"|mouse|none | flash \"file\"");
        }
        const std::string arg = a[1] == "mouse" || a[1] == "none" ? lower(a[1]) : a[1];
        if (!(what == "usb" ? media_->card256_usb(arg, error) : media_->card256_flash(arg, error))) {
            fail(error);
        }
        return media_->card256_describe() + "\n";
    }
    if (what == "mouse") {
        // Decimal, as a mouse moves either way
        const auto number = [&](std::size_t i) {
            try {
                return std::stoi(a[i]);
            } catch (const std::exception&) {
                fail("card256 mouse dx dy [buttons]: decimal numbers");
            }
        };
        if (a.size() < 3 || a.size() > 4) {
            fail("card256 mouse dx dy [buttons]");
        }
        if (!media_->card256_mouse(number(1), number(2), a.size() > 3 ? static_cast<unsigned>(number(3)) : 0u, error)) {
            fail(error);
        }
        return "mouse report sent\n";
    }
    unsigned slot = kCardSlots;
    if (what == "on" && a.size() > 1) {
        if (a[1].size() != 1 || a[1][0] < '0' || a[1][0] > '3') {
            fail("the slot is 0-3");
        }
        slot = static_cast<unsigned>(a[1][0] - '0');
    } else if (what != "on" && what != "off") {
        fail("card256 on [slot] | off | usb ... | flash ... | mouse ...");
    }
    if (!media_->card256(what == "on", slot, error)) {
        fail(error);
    }
    dbg_.cancel_step();
    machine_changed();
    std::string out = media_->card256_describe() + " (powered on)\n";
    for (const std::string& line : media_->take_messages()) {
        out += line + "\n";
    }
    return out;
}

std::string Monitor::cmd_disk(Args& a) {
    if (!media_) {
        fail("this front end has no program media");
    }
    if (a.size() == 0) {
        return media_->disk_describe() + "\n";
    }
    const auto drive = [&](std::size_t i) -> unsigned {
        if (a.size() <= i) {
            return 0;
        }
        std::string word = lower(a[i]);
        if (word.size() == 2 && word[1] == ':') {
            word.pop_back();
        }
        if (word.size() != 1 || word[0] < 'a' || word[0] > 'd') {
            fail("the drive is A, B, C or D");
        }
        return static_cast<unsigned>(word[0] - 'a');
    };
    const std::string what = lower(a[0]);
    const bool card_was_on = media_->disk_card_on();
    std::string error;
    if (what == "insert") {
        const std::string path = a.quoted_word(1, "file name");
        if (!media_->disk_insert(drive(2), path, error)) {
            fail(error);
        }
    } else if (what == "eject") {
        if (!media_->disk_eject(drive(1), error)) {
            fail(error);
        }
    } else if (what == "autostart") {
        dbg_.cancel_step();
        if (!media_->disk_autostart(a.quoted_word(1, "file name"), error)) {
            fail(error);
        }
        machine_changed();
        leave();
        return "autostart: " + media_->disk_describe() + "\n";
    } else if (what == "card" && a.size() > 1 && (lower(a[1]) == "on" || lower(a[1]) == "off")) {
        if (!media_->disk_card(lower(a[1]) == "on", error)) {
            fail(error);
        }
    } else {
        fail("disk insert \"file\" [A-D] | eject [A-D] | autostart \"file\" | card on|off");
    }
    if (media_->disk_card_on() != card_was_on) {
        dbg_.cancel_step();
        machine_changed();
        return media_->disk_describe() + "\n(powered on)\n";
    }
    return media_->disk_describe() + "\n";
}

std::optional<std::uint64_t> Monitor::queue_text(std::u32string_view text) {
    std::uint64_t start = machine_.frame_count() + 1;
    if (!keys_.empty()) {
        start = std::max(start, keys_.back().frame + 3);
    }
    const TypedText typed = type_text(text, start);
    if (typed.unsupported != std::u32string::npos) {
        return std::nullopt;
    }
    keys_.insert(keys_.end(), typed.events.begin(), typed.events.end());
    return typed.end_frame;
}

std::uint64_t Monitor::queue_keys(std::span<const MatrixKey> keys, unsigned hold_frames) {
    std::uint64_t start = machine_.frame_count() + 1;
    if (!keys_.empty()) {
        start = std::max(start, keys_.back().frame + 3);
    }
    for (const MatrixKey key : keys) {
        keys_.push_back({start, key, true});
    }
    for (const MatrixKey key : keys) {
        keys_.push_back({start + hold_frames, key, false});
    }
    return start + hold_frames + 1;
}

std::string Monitor::cmd_radix(Args& a) {
    if (a.size() > 0) {
        const std::string r = lower(a[0]);
        if (r == "h" || r == "hex" || r == "16") {
            radix_ = 16;
        } else if (r == "d" || r == "dec" || r == "10") {
            radix_ = 10;
        } else {
            fail("radix h or d");
        }
    }
    return std::string("radix ") + (radix_ == 16 ? "hex" : "decimal") + "\n";
}

std::string Monitor::cmd_help(Args& a) {
    std::string out;
    for (const Command& c : commands()) {
        const std::string names = c.names;
        if (a.size() > 0) {
            const std::string want = lower(a[0]);
            if ((" " + names + " ").find(" " + want + " ") == std::string::npos) {
                continue;
            }
            return std::string(c.usage) + "\n  " + c.help + (names.find(' ') != std::string::npos ? "\n  names: " + names : "") + "\n";
        }
        out += pad(c.usage, 50) + c.help + "\n";
    }
    if (a.size() > 0) {
        fail("unknown command '" + a[0] + "'");
    }
    return out + "Numbers are hex by default ($1F, 0x1F, 1Fh also; #31 decimal, %101 binary); names are registers or labels.\n";
}

}  // namespace freetvc::remote
