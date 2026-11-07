// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote/mcp_tools.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <optional>

#include "core/cards/card256.hpp"
#include "core/input/keyboard.hpp"
#include "debug/disassembler.hpp"
#include "debug/expression.hpp"
#include "remote/png.hpp"

namespace freetvc::remote::mcp {

namespace {

constexpr std::uint64_t kFrameTicks = kTStatesPerFrame * kTicksPerTState;
constexpr std::int64_t kMaxFrames = 30000;  // 10 emulated minutes per call
constexpr std::int64_t kDefaultTimeoutMs = 10000;
constexpr std::int64_t kMaxTimeoutMs = 60000;
constexpr std::uint16_t kScreenCopy = 0x0100;  // the OS keeps an ASCII copy of the text screen in U0 (tvc-ram.sym)
constexpr unsigned kScreenColumns = 64;
constexpr unsigned kScreenRows = 24;

struct Ctx {
    Monitor& monitor;
    debug::Debugger& dbg;
    Machine& machine;
    const McpOptions& options;
};

struct Place {
    std::optional<Bank> bank;  // with a bank: `address` is an offset in it
    std::uint16_t address = 0;
};

[[noreturn]] void fail(std::string message) { throw ToolError{std::move(message)}; }

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

json text(std::string t) { return {{"type", "text"}, {"text", std::move(t)}}; }

// Structured result, with its JSON also as text for clients that read only text (tools spec "Structured Content")
json structured(json data) { return {{"content", json::array({text(data.dump())})}, {"structuredContent", std::move(data)}}; }

json schema(json properties, std::vector<std::string> required = {}) {
    json s{{"type", "object"}, {"properties", std::move(properties)}, {"additionalProperties", false}};
    if (!required.empty()) {
        s["required"] = required;
    }
    return s;
}

// anyOf with one type per branch rather than a type array: portable to single-type schema dialects (Inspector --strict)
const json kAddressSchema{
    {"anyOf", json::array({{{"type", "integer"}}, {{"type", "string"}}})},
    {"description", "integer (decimal) or monitor expression string: hex by default (\"8000\", \"$C000\", \"0C000h\"), "
                    "#decimal, registers, symbols, BANK:offset (\"U2:1000\", \"VID0:0\", \"SYS:0405\")"}};

json annotations(bool read_only, bool destructive, bool idempotent) {
    return {{"readOnlyHint", read_only}, {"destructiveHint", destructive}, {"idempotentHint", idempotent}, {"openWorldHint", false}};
}

std::int64_t int_arg(const json& args, const char* key, std::int64_t fallback, std::int64_t lo, std::int64_t hi) {
    if (!args.contains(key) || args[key].is_null()) {
        return fallback;
    }
    if (!args[key].is_number_integer()) {
        fail(std::string(key) + " must be an integer");
    }
    const std::int64_t v = args[key].get<std::int64_t>();
    if (v < lo || v > hi) {
        fail(std::string(key) + " must be between " + std::to_string(lo) + " and " + std::to_string(hi));
    }
    return v;
}

bool bool_arg(const json& args, const char* key, bool fallback) {
    if (!args.contains(key) || args[key].is_null()) {
        return fallback;
    }
    if (!args[key].is_boolean()) {
        fail(std::string(key) + " must be true or false");
    }
    return args[key].get<bool>();
}

std::string string_arg(const json& args, const char* key, const char* fallback = nullptr) {
    if (!args.contains(key) || args[key].is_null()) {
        if (!fallback) {
            fail(std::string("missing ") + key);
        }
        return fallback;
    }
    if (!args[key].is_string()) {
        fail(std::string(key) + " must be a string");
    }
    return args[key].get<std::string>();
}

debug::ParseOptions parse_options(Ctx& c) {
    debug::ParseOptions o;
    o.radix = 16;
    o.symbols = &c.monitor.symbols();
    return o;
}

// A banked range must stay inside its bank: past the end there is nothing to read or write
void check_range(Ctx& c, const Place& p, std::size_t length) {
    if (!p.bank) {
        return;
    }
    const std::size_t size = c.machine.memory().bank_bytes(*p.bank).size();
    if (p.address + length > size) {
        fail(std::string(bank_name(*p.bank)) + " has " + std::to_string(size) + " bytes (offsets 0000-" + hex4(static_cast<unsigned>(size - 1)) +
             "); " + std::string(bank_name(*p.bank)) + ":" + hex4(p.address) + " + " + std::to_string(length) + " runs past its end");
    }
}

Place place_arg(Ctx& c, const json& v, const char* what) {
    if (v.is_number_integer()) {
        const std::int64_t a = v.get<std::int64_t>();
        if (a < 0 || a > 0xFFFF) {
            fail(std::string(what) + " must be 0-65535");
        }
        return {std::nullopt, static_cast<std::uint16_t>(a)};
    }
    if (!v.is_string()) {
        fail(std::string(what) + " must be an integer or an expression string");
    }
    const auto [spec, error] = debug::parse_address(v.get<std::string>(), parse_options(c));
    if (!spec) {
        fail(std::string(what) + " '" + v.get<std::string>() + "': " + error.message);
    }
    Place p{spec->bank, static_cast<std::uint16_t>(spec->value.evaluate({c.machine}))};
    if (p.bank && c.machine.memory().bank_bytes(*p.bank).empty()) {
        fail(std::string("this machine has no ") + bank_name(*p.bank));
    }
    check_range(c, p, 1);
    return p;
}

Place place_arg(Ctx& c, const json& args, const char* key, std::optional<std::uint16_t> fallback) {
    if (!args.contains(key) || args[key].is_null()) {
        if (!fallback) {
            fail(std::string("missing ") + key);
        }
        return {std::nullopt, *fallback};
    }
    return place_arg(c, args[key], key);
}

std::uint8_t read_byte(Ctx& c, const Place& p, unsigned i) {
    if (!p.bank) {
        return c.machine.peek(static_cast<std::uint16_t>(p.address + i));
    }
    const auto bytes = c.machine.memory().bank_bytes(*p.bank);
    const std::size_t at = static_cast<std::size_t>(p.address) + i;
    return at < bytes.size() ? bytes[at] : MemoryMap::kOpenBus;
}

void write_byte(Ctx& c, const Place& p, unsigned i, std::uint8_t v) {
    if (!p.bank) {
        c.machine.poke(static_cast<std::uint16_t>(p.address + i), v);
        return;
    }
    const auto bytes = c.machine.bank_bytes(*p.bank);
    const std::size_t at = static_cast<std::size_t>(p.address) + i;
    if (at < bytes.size()) {
        bytes[at] = v;
    }
}

std::string place_name(const Place& p) { return (p.bank ? std::string(bank_name(*p.bank)) + ":" : std::string()) + hex4(p.address); }

std::string base64(const std::vector<std::uint8_t>& data) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const unsigned n = data[i] << 16 | (i + 1 < data.size() ? data[i + 1] << 8 : 0) | (i + 2 < data.size() ? data[i + 2] : 0);
        out.push_back(kTable[(n >> 18) & 63]);
        out.push_back(kTable[(n >> 12) & 63]);
        out.push_back(i + 1 < data.size() ? kTable[(n >> 6) & 63] : '=');
        out.push_back(i + 2 < data.size() ? kTable[n & 63] : '=');
    }
    return out;
}

std::vector<std::uint8_t> unbase64(const std::string& s) {
    std::vector<std::uint8_t> out;
    unsigned buffer = 0;
    int bits = 0;
    for (const char ch : s) {
        int v;
        if (ch >= 'A' && ch <= 'Z') v = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') v = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') v = ch - '0' + 52;
        else if (ch == '+') v = 62;
        else if (ch == '/') v = 63;
        else if (ch == '=' || ch == '\n' || ch == '\r' || ch == ' ') continue;
        else fail("data is not base64");
        buffer = (buffer << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

// Bytes from "21 00 90" / "210090" (hex), base64, or a JSON array of integers
std::vector<std::uint8_t> bytes_arg(const json& args, const char* key, const std::string& format) {
    if (!args.contains(key)) {
        fail(std::string("missing ") + key);
    }
    const json& v = args[key];
    std::vector<std::uint8_t> out;
    if (v.is_array()) {
        for (const json& b : v) {
            if (!b.is_number_integer() || b.get<int>() < 0 || b.get<int>() > 255) {
                fail(std::string(key) + " array items must be integers 0-255");
            }
            out.push_back(static_cast<std::uint8_t>(b.get<int>()));
        }
        return out;
    }
    if (!v.is_string()) {
        fail(std::string(key) + " must be a string or an array of integers");
    }
    const std::string s = v.get<std::string>();
    if (format == "base64") {
        return unbase64(s);
    }
    std::string digits;
    for (const char ch : s) {
        if (std::isxdigit(static_cast<unsigned char>(ch))) {
            digits.push_back(ch);
        } else if (ch != ' ' && ch != ',') {
            fail(std::string(key) + " is not hex");
        }
    }
    if (digits.size() % 2 != 0) {
        fail(std::string(key) + " has an odd number of hex digits");
    }
    for (std::size_t i = 0; i < digits.size(); i += 2) {
        out.push_back(static_cast<std::uint8_t>(std::stoi(digits.substr(i, 2), nullptr, 16)));
    }
    return out;
}

const char* kind_name(std::uint8_t access) {
    switch (access) {
    case kAccessExec: return "exec";
    case kAccessRead: return "load";
    case kAccessWrite: return "store";
    case kAccessIn: return "in";
    case kAccessOut: return "out";
    case kAccessIrq: return "irq";
    default: return "line";
    }
}

json kinds_json(std::uint8_t access) {
    json k = json::array();
    for (std::uint8_t bit = 1; bit != 0 && bit <= kAccessLine; bit = static_cast<std::uint8_t>(bit << 1)) {
        if (access & bit) {
            k.push_back(kind_name(bit));
        }
    }
    return k;
}

json line_json(Ctx& c, std::uint16_t address, std::uint8_t* length = nullptr) {
    debug::DisasmOptions o = debug::tvc_disasm_options(c.monitor.symbols(), c.machine);
    const debug::Disassembly d = debug::disassemble(c.machine, address, o);
    std::string bytes;
    for (unsigned i = 0; i < d.length; ++i) {
        bytes += (i ? " " : "") + hex2(c.machine.peek(static_cast<std::uint16_t>(address + i)));
    }
    json line{{"address", hex4(address)}, {"bytes", bytes}, {"text", d.text}};
    if (!d.comment.empty()) {
        line["comment"] = d.comment;
    }
    if (const debug::Symbol* s = c.monitor.symbols().at(address, c.machine)) {
        line["label"] = s->name;
    }
    if (const auto source = c.monitor.sources().at(address)) {
        line["source"] = c.monitor.source_line_text(*source);
    }
    if (length) {
        *length = d.length;
    }
    return line;
}

json hit_json(Ctx& c, const debug::Hit& h) {
    const bool port = h.access == kAccessIn || h.access == kAccessOut;
    json j{{"checkpoint", h.checkpoint}, {"kind", kind_name(h.access)},
           {"address", h.access == kAccessLine ? std::to_string(h.address) : port ? hex2(h.address) : hex4(h.address)},
           {"value", hex2(h.value)}, {"pc", hex4(h.pc)}};
    if (h.where.bank != Bank::Open) {
        j["bank"] = bank_name(h.where.bank);
        j["offset"] = hex4(h.where.offset);
    }
    (void)c;
    return j;
}

json stop_json(Ctx& c, const debug::Stop* stop, const char* why) {
    json j{{"stopped_by", why}, {"pc", hex4(c.machine.pc())}, {"next_instruction", line_json(c, c.machine.pc())}};
    if (stop) {
        json hits = json::array();
        for (const debug::Hit& h : stop->hits) {
            hits.push_back(hit_json(c, h));
        }
        if (!hits.empty()) {
            j["hits"] = hits;
        }
    }
    return j;
}

void hold(Ctx& c) {
    if (!c.monitor.active()) {
        c.monitor.enter("MCP");
    }
}

// From a held machine: runs until a stop, the tick budget or the wall-clock timeout; leaves it held
json run_until(Ctx& c, std::uint64_t max_ticks, std::int64_t timeout_ms) {
    const std::uint64_t frame0 = c.machine.frame_count();
    const std::uint64_t ticks0 = c.machine.ticks();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    c.monitor.execute("x");
    std::uint64_t ran = 0;
    std::string report;
    bool timed_out = false;
    while (!c.monitor.active() && ran < max_ticks) {
        if (std::chrono::steady_clock::now() > deadline) {
            timed_out = true;
            break;
        }
        ran += c.monitor.run(std::min(max_ticks - ran, kFrameTicks), report);
    }
    json out;
    if (c.monitor.active() && c.monitor.last_stop()) {
        const debug::Stop& s = *c.monitor.last_stop();
        out = stop_json(c, &s, s.why == debug::Why::Checkpoint ? "checkpoint" : "step");
    } else {
        c.monitor.enter("MCP");
        c.dbg.cancel_step();
        out = stop_json(c, nullptr, timed_out ? "timeout" : "budget");
    }
    out["frames_run"] = c.machine.frame_count() - frame0;
    out["tstates_run"] = (c.machine.ticks() - ticks0) / kTicksPerTState;
    return out;
}

json registers_json(Ctx& c) {
    const Z80Registers r = c.dbg.registers();
    const BankAddress where = c.machine.memory().locate(r.pc);
    std::string flags;
    static constexpr char kFlags[] = "SZ5H3VNC";
    for (int bit = 7; bit >= 0; --bit) {
        flags.push_back((r.af >> bit) & 1 ? kFlags[7 - bit] : '-');
    }
    return {{"pc", hex4(r.pc)}, {"sp", hex4(r.sp)}, {"af", hex4(r.af)}, {"bc", hex4(r.bc)}, {"de", hex4(r.de)},
            {"hl", hex4(r.hl)}, {"ix", hex4(r.ix)}, {"iy", hex4(r.iy)}, {"af'", hex4(r.af2)}, {"bc'", hex4(r.bc2)},
            {"de'", hex4(r.de2)}, {"hl'", hex4(r.hl2)}, {"i", hex2(r.i)}, {"r", hex2(r.r)}, {"wz", hex4(r.wz)},
            {"im", r.im}, {"iff1", r.iff1}, {"iff2", r.iff2}, {"flags", flags},
            {"pc_bank", std::string(bank_name(where.bank)) + ":" + hex4(where.offset)}, {"halted", c.machine.halted()}};
}

std::vector<std::string> screen_rows(Ctx& c) {
    // TVC character codes: ASCII plus the accented letters (keyboard.hpp CharKey codes)
    std::array<char32_t, 256> map{};
    for (const CharKey& k : char_keys()) {
        map[k.code] = k.ch;
    }
    const auto u0 = c.machine.memory().bank_bytes(Bank::U0);
    std::vector<std::string> rows;
    for (unsigned row = 0; row < kScreenRows; ++row) {
        std::string line;
        for (unsigned col = 0; col < kScreenColumns; ++col) {
            const std::uint8_t b = u0[kScreenCopy + row * kScreenColumns + col];
            char32_t ch = b >= 0x20 && b < 0x7F ? char32_t(b) : map[b];
            if (ch == 0) {
                ch = U'?';
            }
            if (ch < 0x80) {
                line.push_back(static_cast<char>(ch));
            } else {
                line.push_back(static_cast<char>(0xC0 | (ch >> 6)));
                line.push_back(static_cast<char>(0x80 | (ch & 0x3F)));
            }
        }
        line.erase(line.find_last_not_of(' ') + 1);
        rows.push_back(std::move(line));
    }
    while (!rows.empty() && rows.back().empty()) {
        rows.pop_back();
    }
    return rows;
}

std::string screen_text_of(const std::vector<std::string>& rows) {
    std::string t;
    for (const std::string& r : rows) {
        t += r + "\n";
    }
    return t;
}

// Runs until the queued keys are released plus `settle` frames (the OS reads keys from its IRQ)
json run_keys(Ctx& c, std::uint64_t end_frame, std::int64_t settle, std::int64_t timeout_ms) {
    const std::uint64_t target = end_frame + static_cast<std::uint64_t>(settle);
    const std::uint64_t now = c.machine.frame_count();
    const std::uint64_t frames = target > now ? target - now : 0;
    json out = run_until(c, (frames + 1) * kFrameTicks, timeout_ms);
    out["screen"] = screen_rows(c);
    return out;
}

std::string crc_name(std::uint32_t crc) {
    switch (crc) {
    case 0x79FA818C: return "SYS 2.2";
    case 0x53C8FF98: return "SYS 2.1";
    case 0x6DA8809A: return "SYS 1.2";
    case 0x05E1C3A8: return "EXT 2.x";
    case 0x59C6B9F9: return "EXT 1.x";
    default: return "unknown";
    }
}

// --- the tools ---

json machine_info(Ctx& c, const json&) {
    const MemoryMap& map = c.machine.memory();
    json pages = json::array();
    for (unsigned page = 0; page < 4; ++page) {
        pages.push_back(bank_name(map.locate(static_cast<std::uint16_t>(page * 0x4000)).bank));
    }
    const auto sys = map.bank_bytes(Bank::Sys);
    const auto ext = map.bank_bytes(Bank::Ext);
    const std::uint32_t ext_crc = debug::crc32(ext);
    const std::uint32_t ext1_crc = debug::crc32(ext.subspan(0x1000));
    json info{{"machine", c.options.machine_name.empty() ? "TVC" : c.options.machine_name},
              {"sys_rom", crc_name(debug::crc32(sys))},
              {"ext_rom", crc_name(ext_crc) != "unknown" ? crc_name(ext_crc) : crc_name(ext1_crc)},
              {"state", c.monitor.active() ? "paused" : "running"},
              {"pc", hex4(c.machine.pc())},
              {"frame", c.machine.frame_count()},
              {"tstates", c.machine.ticks() / kTicksPerTState},
              {"beam", {{"line", c.machine.video().beam_line()}, {"column", c.machine.video().beam_column()}}},
              {"pages", pages},
              {"port02", hex2(map.page_register())},
              {"checkpoints", c.dbg.checkpoints().size()},
              {"typing", c.monitor.typing()},
              {"symbols", c.monitor.symbols().size()},
              {"time", "1 frame = 62800 T-states = 20.096 ms; 3.125 MHz Z80A"}};
    if (c.monitor.last_stop()) {
        info["last_stop_hits"] = c.monitor.last_stop()->hits.size();
    }
    return structured(info);
}

json control(Ctx& c, const json& args) {
    const std::string action = string_arg(args, "action");
    if (action == "pause") {
        hold(c);
    } else if (action == "resume") {
        hold(c);
        c.monitor.execute("x");
    } else if (action == "reset" || action == "power_cycle") {
        hold(c);
        c.monitor.execute(action == "reset" ? "reset" : "reset hard");
    } else {
        fail("action is pause, resume, reset or power_cycle");
    }
    return structured({{"state", c.monitor.active() ? "paused" : "running"}, {"pc", hex4(c.machine.pc())}});
}

json run(Ctx& c, const json& args) {
    const std::int64_t timeout = int_arg(args, "timeout_ms", kDefaultTimeoutMs, 1, kMaxTimeoutMs);
    const bool has_frames = args.contains("frames") && !args["frames"].is_null();
    const bool has_tstates = args.contains("tstates") && !args["tstates"].is_null();
    const bool has_instructions = args.contains("instructions") && !args["instructions"].is_null();
    if (int(has_frames) + int(has_tstates) + int(has_instructions) > 1) {
        fail("give at most one of frames, tstates, instructions");
    }
    hold(c);
    if (has_instructions) {
        const std::int64_t n = int_arg(args, "instructions", 1, 1, 10'000'000);
        const debug::Stop stop = c.dbg.step(static_cast<std::uint64_t>(n), static_cast<std::uint64_t>(kMaxFrames) * kFrameTicks);
        return structured(stop_json(c, &stop, stop.why == debug::Why::Checkpoint ? "checkpoint" : "step"));
    }
    std::uint64_t ticks = kFrameTicks;
    if (has_frames) {
        ticks = static_cast<std::uint64_t>(int_arg(args, "frames", 1, 1, kMaxFrames)) * kFrameTicks;
    } else if (has_tstates) {
        ticks = static_cast<std::uint64_t>(int_arg(args, "tstates", 1, 1, kMaxFrames * static_cast<std::int64_t>(kTStatesPerFrame))) * kTicksPerTState;
    }
    if (args.contains("until") && !args["until"].is_null()) {
        const Place p = place_arg(c, args["until"], "until");
        if (p.bank) {
            fail("until takes a CPU address (no BANK:)");
        }
        c.dbg.start_run_to(p.address, std::nullopt);
        if (!has_frames && !has_tstates) {
            ticks = static_cast<std::uint64_t>(kMaxFrames) * kFrameTicks;
        }
    }
    return structured(run_until(c, ticks, timeout));
}

json step(Ctx& c, const json& args) {
    const std::string mode = string_arg(args, "mode", "into");
    const std::int64_t count = int_arg(args, "count", 1, 1, 100000);
    const std::int64_t timeout = int_arg(args, "timeout_ms", kDefaultTimeoutMs, 1, kMaxTimeoutMs);
    hold(c);
    json out;
    if (mode == "into") {
        const debug::Stop stop = c.dbg.step(static_cast<std::uint64_t>(count), 100 * kFrameTicks);
        out = stop_json(c, &stop, stop.why == debug::Why::Checkpoint ? "checkpoint" : stop.why == debug::Why::Step ? "step" : "budget");
    } else if (mode == "over" || mode == "out") {
        for (std::int64_t i = 0; i < count; ++i) {
            if (mode == "out") {
                c.dbg.start_step_out();
            } else if (!c.dbg.start_step_over()) {
                const debug::Stop stop = c.dbg.step(1, 100 * kFrameTicks);
                out = stop_json(c, &stop, stop.why == debug::Why::Checkpoint ? "checkpoint" : "step");
                if (stop.why == debug::Why::Checkpoint) {
                    break;
                }
                continue;
            }
            out = run_until(c, static_cast<std::uint64_t>(kMaxFrames) * kFrameTicks, timeout);
            if (out["stopped_by"] != "step") {
                break;
            }
        }
    } else if (mode == "line" || mode == "line_over") {
        if (c.monitor.sources().empty()) {
            fail("no source lines: load a debug file first with symbols {\"action\": \"load\", \"path\": ...} "
                 "(sjasmplus --sld or --lst, SDCC .noi/.cdb/.map, z88dk .map)");
        }
        for (std::int64_t i = 0; i < count; ++i) {
            const debug::Stop stop = c.monitor.step_line(mode == "line_over");
            out = stop_json(c, &stop, stop.why == debug::Why::Checkpoint ? "checkpoint" : stop.why == debug::Why::Step ? "step" : "budget");
            if (stop.why != debug::Why::Step) {
                break;
            }
        }
    } else {
        fail("mode is into, over, out, line or line_over");
    }
    out["registers"] = registers_json(c);
    return structured(out);
}

json registers(Ctx& c, const json& args) {
    if (args.contains("set") && !args["set"].is_null()) {
        if (!args["set"].is_object()) {
            fail("set must be an object like {\"a\": 5, \"hl\": \"9000\"}");
        }
        hold(c);
        std::string command = "r";
        for (const auto& [reg, v] : args["set"].items()) {
            std::string value;
            if (v.is_number_integer()) {
                value = "#" + std::to_string(v.get<std::int64_t>());
            } else if (v.is_string()) {
                value = v.get<std::string>();
            } else {
                fail("register values are integers or expression strings");
            }
            command += (command.size() > 1 ? ", " : " ") + reg + "=" + value;
        }
        const std::string out = c.monitor.execute(command);
        if (out.rfind("error:", 0) == 0) {
            fail(out.substr(7, out.size() - 8));
        }
    }
    return structured(registers_json(c));
}

json memory(Ctx& c, const json& args) {
    const std::string action = string_arg(args, "action", "read");
    const std::string format = string_arg(args, "format", "hex");
    if (format != "hex" && format != "base64") {
        fail("format is hex or base64");
    }
    const Place p = place_arg(c, args, "address", std::nullopt);
    if (action == "read") {
        const auto length = static_cast<unsigned>(int_arg(args, "length", 16, 1, 0x10000));
        check_range(c, p, length);
        std::vector<std::uint8_t> data;
        for (unsigned i = 0; i < length; ++i) {
            data.push_back(read_byte(c, p, i));
        }
        std::string hex;
        if (format == "hex") {
            for (std::size_t i = 0; i < data.size(); ++i) {
                hex += (i ? " " : "") + hex2(data[i]);
            }
        }
        return structured({{"address", place_name(p)}, {"length", length}, {"format", format},
                           {"data", format == "hex" ? hex : base64(data)}});
    }
    if (action == "write") {
        const std::vector<std::uint8_t> data = bytes_arg(args, "data", format);
        check_range(c, p, data.size());
        for (std::size_t i = 0; i < data.size(); ++i) {
            write_byte(c, p, static_cast<unsigned>(i), data[i]);
        }
        return structured({{"address", place_name(p)}, {"written", data.size()}});
    }
    if (action == "fill") {
        const auto length = static_cast<unsigned>(int_arg(args, "length", 1, 1, 0x10000));
        check_range(c, p, length);
        const std::vector<std::uint8_t> pattern = bytes_arg(args, "data", format);
        if (pattern.empty()) {
            fail("data is empty");
        }
        for (unsigned i = 0; i < length; ++i) {
            write_byte(c, p, i, pattern[i % pattern.size()]);
        }
        return structured({{"address", place_name(p)}, {"filled", length}});
    }
    if (action == "search") {
        const std::size_t room = p.bank ? c.machine.memory().bank_bytes(*p.bank).size() - p.address : 0x10000u - p.address;
        const auto length = static_cast<unsigned>(int_arg(args, "length", static_cast<std::int64_t>(room), 1, 0x10000));
        check_range(c, p, length);
        const std::vector<std::uint8_t> pattern = bytes_arg(args, "data", format);
        if (pattern.empty()) {
            fail("data is empty");
        }
        json found = json::array();
        for (unsigned i = 0; i + pattern.size() <= length && found.size() < 256; ++i) {
            bool match = true;
            for (std::size_t k = 0; k < pattern.size() && match; ++k) {
                match = read_byte(c, p, static_cast<unsigned>(i + k)) == pattern[k];
            }
            if (match) {
                found.push_back(place_name({p.bank, static_cast<std::uint16_t>(p.address + i)}));
            }
        }
        return structured({{"matches", found}, {"truncated", found.size() >= 256}});
    }
    fail("action is read, write, fill or search");
}

json disassemble(Ctx& c, const json& args) {
    const Place p = place_arg(c, args, "address", c.machine.pc());
    if (p.bank) {
        fail("disassemble takes a CPU address; read a bank that is not paged in with memory");
    }
    const auto count = int_arg(args, "count", 16, 1, 512);
    json lines = json::array();
    std::string listing;
    std::uint16_t a = p.address;
    for (std::int64_t i = 0; i < count; ++i) {
        std::uint8_t length = 1;
        json line = line_json(c, a, &length);
        if (line.contains("label")) {
            listing += line["label"].get<std::string>() + ":\n";
        }
        char buf[96];
        std::snprintf(buf, sizeof buf, "%s  %-12s %s", line["address"].get<std::string>().c_str(),
                      line["bytes"].get<std::string>().c_str(), line["text"].get<std::string>().c_str());
        listing += buf;
        if (line.contains("comment")) {
            listing += "  ; " + line["comment"].get<std::string>();
        }
        listing += "\n";
        lines.push_back(std::move(line));
        a = static_cast<std::uint16_t>(a + length);
    }
    return {{"content", json::array({text(listing)})}, {"structuredContent", {{"lines", lines}, {"next", hex4(a)}}}};
}

json checkpoint_json(const debug::Checkpoint& cp) {
    const bool rows = (cp.access & kAccessLine) != 0;
    const bool ports = (cp.access & (kAccessIn | kAccessOut)) != 0;
    const auto addr = [&](unsigned v) { return rows ? std::to_string(v) : ports ? hex2(v) : hex4(v); };
    json j{{"id", cp.id}, {"kinds", kinds_json(cp.access)}, {"start", addr(cp.start)}, {"end", addr(cp.end)},
           {"stop", cp.stop}, {"enabled", cp.enabled}, {"hits", cp.hits}, {"ignore", cp.ignore}, {"temporary", cp.temporary}};
    if (cp.bank) {
        j["bank"] = bank_name(*cp.bank);
        j["bank_offsets"] = cp.offsets;
    }
    if (!cp.condition_text.empty()) {
        j["condition"] = cp.condition_text;
    }
    return j;
}

json breakpoints(Ctx& c, const json& args) {
    const std::string action = string_arg(args, "action", "list");
    if (action == "list") {
        json list = json::array();
        for (const debug::Checkpoint& cp : c.dbg.checkpoints()) {
            list.push_back(checkpoint_json(cp));
        }
        return structured({{"checkpoints", list}});
    }
    if (action == "clear") {
        const std::size_t n = c.dbg.checkpoints().size();
        c.dbg.clear_checkpoints();
        return structured({{"deleted", n}});
    }
    if (action == "delete" || action == "enable" || action == "disable") {
        const auto id = static_cast<unsigned>(int_arg(args, "id", -1, 1, 1'000'000));
        const debug::Checkpoint* found = c.dbg.find(id);
        if (!found) {
            fail("no checkpoint " + std::to_string(id));
        }
        if (action == "delete") {
            c.dbg.remove(id);
            return structured({{"deleted", 1}});
        }
        debug::Checkpoint cp = *found;
        cp.enabled = action == "enable";
        c.dbg.update(cp);
        return structured(checkpoint_json(*c.dbg.find(id)));
    }
    if (action != "add") {
        fail("action is add, list, delete, enable, disable or clear");
    }
    std::uint8_t access = 0;
    json kinds = args.contains("kind") ? args["kind"] : json("exec");
    if (kinds.is_string()) {
        kinds = json::array({kinds});
    }
    static constexpr std::array<std::pair<const char*, std::uint8_t>, 7> kKinds{{
        {"exec", kAccessExec}, {"load", kAccessRead}, {"store", kAccessWrite}, {"in", kAccessIn},
        {"out", kAccessOut}, {"irq", kAccessIrq}, {"line", kAccessLine}}};
    for (const json& k : kinds) {
        const auto it = std::find_if(kKinds.begin(), kKinds.end(), [&](const auto& e) { return k.is_string() && k.get<std::string>() == e.first; });
        if (it == kKinds.end()) {
            fail("kind is exec, load, store, in, out, irq or line (or an array of them)");
        }
        access |= it->second;
    }
    debug::Checkpoint cp;
    cp.access = access;
    cp.stop = bool_arg(args, "stop", true);
    cp.temporary = bool_arg(args, "temporary", false);
    cp.ignore = static_cast<std::uint32_t>(int_arg(args, "ignore", 0, 0, 1'000'000'000));
    if (args.contains("address") && !args["address"].is_null()) {
        const Place start = place_arg(c, args["address"], "address");
        const Place end = args.contains("end") && !args["end"].is_null() ? place_arg(c, args["end"], "end") : start;
        if (start.bank) {
            if (access & ~(kAccessExec | kAccessRead | kAccessWrite)) {
                fail("BANK:offset works for exec, load and store");
            }
            cp.bank = start.bank;
            cp.offsets = true;
        }
        cp.start = start.address;
        cp.end = end.address;
    } else if (access == kAccessIrq) {
        cp.end = 0xFFFF;
    } else {
        fail("missing address");
    }
    if (cp.end < cp.start) {
        fail("end before start");
    }
    if ((access & (kAccessIn | kAccessOut)) && cp.end > 0xFF) {
        fail("ports are 0-255 (00-FF)");
    }
    if ((access & kAccessLine) && cp.end > 511) {
        fail("rows are 0-511; give them as integers or as \"#100\"");
    }
    if (args.contains("condition") && !args["condition"].is_null()) {
        const std::string cond = string_arg(args, "condition");
        const auto [e, error] = debug::Expression::parse(cond, parse_options(c));
        if (!e) {
            fail("condition: " + error.message);
        }
        cp.condition = debug::make_condition(*e);
        cp.condition_text = cond;
    }
    const unsigned id = c.dbg.add(std::move(cp));
    return structured(checkpoint_json(*c.dbg.find(id)));
}

json screenshot(Ctx& c, const json& args) {
    const std::string area_name = string_arg(args, "area", "tv");
    if (area_name != "tv" && area_name != "full") {
        fail("area is tv or full");
    }
    const ScreenArea area = area_name == "full" ? kFullRaster : kTvArea;
    const std::vector<std::uint8_t> png = encode_png(c.machine.framebuffer(), area);
    return {{"content", json::array({{{"type", "image"}, {"data", base64(png)}, {"mimeType", "image/png"}},
                                     text("TVC screen, " + std::to_string(area.width) + " x " + std::to_string(area.height) +
                                          " pixels (" + area_name + " area), frame " + std::to_string(c.machine.frame_count()))})}};
}

json screen_text(Ctx& c, const json&) {
    const std::vector<std::string> rows = screen_rows(c);
    json data{{"rows", rows}, {"columns", kScreenColumns},
              {"source", "the OS's ASCII copy of the text screen at U0:0100 (64 x 24); graphics and text a program draws "
                         "straight into VRAM are not in it: use screenshot"}};
    return {{"content", json::array({text(rows.empty() ? "(the screen copy is empty)\n" : screen_text_of(rows))})},
            {"structuredContent", data}};
}

json type_text_tool(Ctx& c, const json& args) {
    const std::string t = string_arg(args, "text");
    const auto chars = decode_utf8(t);
    if (!chars) {
        fail("text is not UTF-8");
    }
    hold(c);
    const auto end = c.monitor.queue_text(*chars);
    if (!end) {
        fail("a character in text has no TVC key (use \\n for RETURN)");
    }
    if (!bool_arg(args, "wait", true)) {
        return structured({{"queued", chars->size()}, {"until_frame", *end}});
    }
    return structured(run_keys(c, *end, int_arg(args, "settle_frames", 10, 0, 3000),
                               int_arg(args, "timeout_ms", kDefaultTimeoutMs, 1, kMaxTimeoutMs)));
}

json press_keys(Ctx& c, const json& args) {
    if (!args.contains("keys") || !args["keys"].is_array() || args["keys"].empty()) {
        fail("keys is a non-empty array of key names, e.g. [\"SHIFT\", \"A\"] or [\"RETURN\"] or [\"8:6\"] (row:column)");
    }
    std::vector<MatrixKey> keys;
    for (const json& k : args["keys"]) {
        if (!k.is_string()) {
            fail("key names are strings");
        }
        const std::string name = k.get<std::string>();
        const auto colon = name.find(':');
        if (colon != std::string::npos && colon > 0) {
            const int row = std::atoi(name.substr(0, colon).c_str());
            const int col = std::atoi(name.substr(colon + 1).c_str());
            if (row < 0 || row >= static_cast<int>(kMatrixRows) || col < 0 || col > 7) {
                fail("matrix keys are ROW:COLUMN with row 0-11 and column 0-7");
            }
            keys.push_back({static_cast<std::uint8_t>(row), static_cast<std::uint8_t>(col)});
        } else if (const auto key = find_key(name)) {
            keys.push_back(*key);
        } else {
            fail("unknown key '" + name + "'");
        }
    }
    hold(c);
    const std::uint64_t end = c.monitor.queue_keys(keys, static_cast<unsigned>(int_arg(args, "hold_frames", 3, 1, 3000)));
    if (!bool_arg(args, "wait", true)) {
        return structured({{"queued", keys.size()}, {"until_frame", end}});
    }
    return structured(run_keys(c, end, int_arg(args, "settle_frames", 5, 0, 3000),
                               int_arg(args, "timeout_ms", kDefaultTimeoutMs, 1, kMaxTimeoutMs)));
}

json game_saves(Ctx& c, const json& args) {
    MediaControl* media = c.monitor.media();
    if (!media) {
        fail("this front end has no program media");
    }
    const std::string action = string_arg(args, "action", "list");
    std::string error;
    if (action == "list") {
        const std::string game = string_arg(args, "game", "");
        if (game.empty()) {
            return structured({{"root", media->game_root()}, {"game", media->game_name()}, {"games", media->game_list()}});
        }
        json saves = json::array();
        for (const GameSaveEntry& save : media->game_saves(game)) {
            saves.push_back({{"name", save.name}, {"when", save.when}, {"file", save.file}, {"picture", save.picture}});
        }
        return structured({{"game", game}, {"saves", std::move(saves)}});
    }
    if (action == "save") {
        hold(c);
        c.dbg.cancel_step();
        std::string file;
        if (!media->game_save(string_arg(args, "game", ""), string_arg(args, "name", ""), file, error)) {
            fail(error);
        }
        return structured({{"action", action}, {"file", file}, {"game", media->game_name()}});
    }
    if (action == "check") {
        const auto paths = media->snapshot_check(string_arg(args, "path"), error);
        if (!paths) {
            fail(error);
        }
        json list = json::array();
        bool all = true;
        for (const SnapshotPath& p : *paths) {
            list.push_back({{"kind", p.kind}, {"path", p.path}, {"directory", p.directory}, {"found", p.found}});
            all &= p.found;
        }
        return structured({{"paths", std::move(list)}, {"all_found", all}});
    }
    if (action == "load") {
        SnapshotPathMap moved;
        if (const auto it = args.find("moved"); it != args.end() && it->is_object()) {
            for (const auto& [from, to] : it->items()) {
                if (to.is_string()) {
                    moved[from] = to.get<std::string>();
                }
            }
        }
        hold(c);
        c.dbg.cancel_step();
        if (!media->snapshot_load_moved(string_arg(args, "path"), moved, error)) {
            fail(error);
        }
        c.monitor.machine_changed();
        return structured({{"action", action}, {"game", media->game_name()}, {"pc", hex4(c.machine.pc())}});
    }
    if (action == "delete") {
        if (!media->game_save_delete(string_arg(args, "path"), error)) {
            fail(error);
        }
        return structured({{"action", action}});
    }
    fail("action is list, save, check, load or delete");
}

json snapshot(Ctx& c, const json& args) {
    const std::string action = string_arg(args, "action", "list");
    if (action == "list") {
        return structured({{"states", c.dbg.state_names()}});
    }
    if (action == "rewind") {
        MediaControl* media = c.monitor.media();
        if (!media) {
            fail("this front end has no program media");
        }
        hold(c);
        c.dbg.cancel_step();
        std::string error;
        if (!media->rewind(static_cast<unsigned>(int_arg(args, "seconds", 1, 1, 3600)), error)) {
            fail(error + " (" + media->rewind_describe() + ")");
        }
        return structured({{"action", action}, {"rewind", media->rewind_describe()}, {"pc", hex4(c.machine.pc())}});
    }
    if (action == "save_file" || action == "load_file") {
        MediaControl* media = c.monitor.media();
        if (!media) {
            fail("this front end has no program media");
        }
        const std::string path = string_arg(args, "path");
        std::string error;
        hold(c);
        c.dbg.cancel_step();
        if (!(action == "save_file" ? media->snapshot_save(path, error) : media->snapshot_load(path, error))) {
            fail(error);
        }
        if (action == "load_file") {
            c.monitor.machine_changed();
        }
        return structured({{"action", action}, {"path", path}, {"pc", hex4(c.machine.pc())}});
    }
    const std::string name = string_arg(args, "name");
    if (action == "save") {
        hold(c);
        c.dbg.save_state(name);
    } else if (action == "load") {
        hold(c);
        if (!c.dbg.load_state(name)) {
            fail("no state '" + name + "'");
        }
    } else if (action == "delete") {
        if (!c.dbg.delete_state(name)) {
            fail("no state '" + name + "'");
        }
    } else {
        fail("action is save, load, delete, list, save_file, load_file or rewind");
    }
    return structured({{"action", action}, {"name", name}, {"pc", hex4(c.machine.pc())}});
}

json symbols(Ctx& c, const json& args) {
    const std::string action = string_arg(args, "action", "lookup");
    debug::SymbolTable& table = c.monitor.symbols();
    const auto symbol_json = [](const debug::Symbol& s) {
        json j{{"name", s.name}, {"address", hex4(s.address)}, {"source", s.source}};
        if (s.home) {
            j["bank"] = std::string(bank_name(s.home->bank)) + ":" + hex4(s.home->offset);
        }
        return j;
    };
    if (action == "lookup") {
        if (args.contains("name")) {
            const debug::Symbol* s = table.find(string_arg(args, "name"));
            if (!s) {
                fail("no symbol '" + string_arg(args, "name") + "'");
            }
            return structured(symbol_json(*s));
        }
        const Place p = place_arg(c, args, "address", std::nullopt);
        const debug::Symbol* s = p.bank ? table.at(p.address, BankAddress{*p.bank, p.address}) : table.at(p.address, c.machine);
        json found = s ? symbol_json(*s) : json{{"name", nullptr}, {"address", place_name(p)}};
        if (const auto source = p.bank ? std::nullopt : c.monitor.sources().at(p.address)) {
            found["source_line"] = c.monitor.source_line_text(*source);
        }
        return structured(found);
    }
    if (action == "list") {
        const std::string filter = string_arg(args, "filter", "");
        json list = json::array();
        for (const debug::Symbol& s : table.symbols()) {
            if (filter.empty() || s.name.find(filter) != std::string::npos) {
                if (list.size() < 500) {
                    list.push_back(symbol_json(s));
                }
            }
        }
        return structured({{"symbols", list}, {"total", table.size()}});
    }
    if (action == "add") {
        const Place p = place_arg(c, args, "address", std::nullopt);
        if (p.bank) {
            fail("add takes a CPU address");
        }
        table.add({string_arg(args, "name"), p.address, std::nullopt, "mcp"});
        return structured({{"added", string_arg(args, "name")}, {"address", hex4(p.address)}});
    }
    if (action == "load") {
        const std::string path = string_arg(args, "path");
        std::string error;
        const std::optional<debug::SourceMap::Loaded> r = c.monitor.load_debug_file(path, error);
        if (!r) {
            fail(error);
        }
        json errors = json::array();
        for (std::size_t i = 0; i < r->errors.size() && i < 10; ++i) {
            errors.push_back(r->errors[i]);
        }
        return structured({{"format", debug::SourceMap::format_name(r->format)}, {"loaded", r->labels}, {"source_lines", r->lines},
                           {"source_files", c.monitor.sources().files().size()}, {"errors", errors}, {"error_count", r->errors.size()}});
    }
    fail("action is lookup, list, add or load");
}

json trace(Ctx& c, const json& args) {
    const auto count = static_cast<std::size_t>(int_arg(args, "count", 32, 1, 4096));
    json history = json::array();
    debug::DisasmOptions o = debug::tvc_disasm_options(c.monitor.symbols(), c.machine);
    for (const debug::TraceEntry& e : c.dbg.trace().last(count)) {
        const debug::Disassembly d = debug::disassemble(e.bytes, e.regs.pc, o);
        json j{{"pc", hex4(e.regs.pc)}, {"text", d.text}, {"af", hex4(e.regs.af)}, {"bc", hex4(e.regs.bc)}, {"de", hex4(e.regs.de)},
               {"hl", hex4(e.regs.hl)}, {"sp", hex4(e.regs.sp)}, {"tstate", e.ticks / kTicksPerTState},
               {"bank", std::string(bank_name(e.where.bank)) + ":" + hex4(e.where.offset)}};
        if (e.interrupt) {
            j["interrupt"] = true;
        }
        history.push_back(std::move(j));
    }
    json hits = json::array();
    for (const debug::Hit& h : c.dbg.hit_log().last(static_cast<std::size_t>(int_arg(args, "hits", 32, 0, 1024)))) {
        hits.push_back(hit_json(c, h));
    }
    return structured({{"history", history}, {"history_enabled", c.dbg.trace().capacity() > 0}, {"checkpoint_hits", hits}});
}

json io_state(Ctx& c, const json&) {
    const Video& v = c.machine.video();
    const Sound& s = c.machine.sound();
    const MemoryMap& map = c.machine.memory();
    json crtc = json::array();
    for (const std::uint8_t r : v.crtc().registers()) {
        crtc.push_back(hex2(r));
    }
    static constexpr std::array<const char*, 4> kModes{"2-colour", "4-colour", "16-colour", "16-colour"};
    return structured({{"video", {{"mode", kModes[v.mode()]}, {"border", hex2(v.border())},
                                  {"palette", {hex2(v.palette(0)), hex2(v.palette(1)), hex2(v.palette(2)), hex2(v.palette(3))}},
                                  {"beam_line", v.beam_line()}, {"beam_column", v.beam_column()}}},
                       {"crtc", {{"registers", crtc}, {"selected", v.crtc().selected()}}},
                       {"sound", {{"pitch", hex4(s.pitch())}, {"tone", s.tone_enabled()}, {"amplitude", s.amplitude()}, {"irq", s.irq_enabled()}}},
                       {"irq_flipflop", c.machine.irq_pending()},
                       {"paging", {{"port02", hex2(map.page_register())}, {"video_paging", hex2(map.video_paging())},
                                   {"iomem_slot", map.iomem_slot()}, {"displayed_vid", map.displayed_bank()}}}});
}

json evaluate(Ctx& c, const json& args) {
    const std::string expr = string_arg(args, "expression");
    const auto [e, error] = debug::Expression::parse(expr, parse_options(c));
    if (!e) {
        fail(error.message + " (at " + std::to_string(error.position) + ")");
    }
    const std::int64_t v = e->evaluate({c.machine});
    return structured({{"value", v}, {"hex", hex4(static_cast<unsigned>(v)) + (v > 0xFFFF || v < 0 ? " (low 16 bits)" : "")}});
}

json load_binary(Ctx& c, const json& args) {
    const std::string path = string_arg(args, "path");
    const Place p = place_arg(c, args, "address", std::nullopt);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        fail("cannot read '" + path + "'");
    }
    const std::vector<std::uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() > 0x10000) {
        fail("the file is larger than 64 KiB");
    }
    for (std::size_t i = 0; i < data.size(); ++i) {
        write_byte(c, p, static_cast<unsigned>(i), data[i]);
    }
    json out{{"loaded", data.size()}, {"address", place_name(p)}};
    if (bool_arg(args, "jump", false)) {
        if (p.bank) {
            fail("jump needs a CPU address");
        }
        hold(c);
        Z80Registers r = c.dbg.registers();
        r.pc = p.address;
        c.dbg.set_registers(r);
        out["pc"] = hex4(c.machine.pc());
    }
    return structured(out);
}

json media(Ctx& c, const json& args) {
    MediaControl* media = c.monitor.media();
    if (!media) {
        fail("this front end has no program media");
    }
    const std::string action = string_arg(args, "action");
    std::string error;
    if (action == "attach") {
        if (!media->attach(string_arg(args, "path"), error)) {
            fail(error);
        }
    } else if (action == "detach") {
        media->detach();
    } else if (action == "tape_insert" || action == "tape_record") {
        if (!(action == "tape_insert" ? media->tape_insert(string_arg(args, "path"), error)
                                      : media->tape_record(string_arg(args, "path"), error))) {
            fail(error);
        }
    } else if (action == "cart_insert" || action == "cart_remove") {
        hold(c);
        c.dbg.cancel_step();
        if (action == "cart_remove") {
            media->cart_remove();
        } else if (!media->cart_insert(string_arg(args, "path"), string_arg(args, "cart_type", ""),
                                       static_cast<unsigned>(int_arg(args, "bank", 0, 0, 63)), error)) {
            fail(error);
        }
        c.monitor.machine_changed();
    } else if (action == "cart_build" || action == "sd_build") {
        std::vector<std::string> files;
        if (args.contains("files") && args["files"].is_array()) {
            for (const json& f : args["files"]) {
                if (!f.is_string()) {
                    fail("files holds file names");
                }
                files.push_back(f.get<std::string>());
            }
        }
        if (action == "sd_build" ? !media->sd_build(string_arg(args, "path"), static_cast<std::uint64_t>(int_arg(args, "size_mib", 64, 1, 2 * 1024 * 1024)), files, error)
                                 : !media->multicart_build(string_arg(args, "path"), files, string_arg(args, "language", "en"), error)) {
            fail(error);
        }
    } else if (action == "sddual_on" || action == "sddual_off") {
        hold(c);
        c.dbg.cancel_step();
        if (!media->sddual(action == "sddual_on", string_arg(args, "firmware", "plain") == "gamer", error)) {
            fail(error);
        }
        c.monitor.machine_changed();
    } else if (action == "sd_insert") {
        if (!media->sd_insert(static_cast<unsigned>(int_arg(args, "socket", 0, 0, 1)), string_arg(args, "path"), error)) {
            fail(error);
        }
    } else if (action == "sd_eject") {
        media->sd_eject(static_cast<unsigned>(int_arg(args, "socket", 0, 0, 1)));
    } else if (action == "disk_insert" || action == "disk_eject" || action == "disk_card_on" || action == "disk_card_off") {
        const std::string letter = string_arg(args, "drive", "A");
        const char d = static_cast<char>(letter.empty() ? 'a' : (letter[0] | 0x20));
        if (letter.size() > 2 || d < 'a' || d > 'd') {
            fail("drive is A, B, C or D");
        }
        const bool card_was_on = media->disk_card_on();
        bool ok = true;
        if (action == "disk_insert") {
            ok = media->disk_insert(static_cast<unsigned>(d - 'a'), string_arg(args, "path"), error);
        } else if (action == "disk_eject") {
            ok = media->disk_eject(static_cast<unsigned>(d - 'a'), error);
        } else {
            ok = media->disk_card(action == "disk_card_on", error);
        }
        if (!ok) {
            fail(error);
        }
        if (media->disk_card_on() != card_was_on) {
            hold(c);
            c.dbg.cancel_step();
            c.monitor.machine_changed();
        }
    } else if (action == "gamecard_on" || action == "gamecard_off") {
        hold(c);
        c.dbg.cancel_step();
        if (!media->gamecard(action == "gamecard_on", static_cast<unsigned>(int_arg(args, "slot", kGameCardSlot, 0, 3)), error)) {
            fail(error);
        }
        c.monitor.machine_changed();
    } else if (action == "multichip_on" || action == "multichip_off") {
        hold(c);
        c.dbg.cancel_step();
        if (!media->multichip(action == "multichip_on", static_cast<unsigned>(int_arg(args, "slot", kMultichipSlot, 0, 3)), error)) {
            fail(error);
        }
        c.monitor.machine_changed();
    } else if (action == "twodfx_on" || action == "twodfx_off") {
        hold(c);
        c.dbg.cancel_step();
        if (!media->twodfx(action == "twodfx_on", static_cast<unsigned>(int_arg(args, "slot", 4, 0, 4)), error)) {
            fail(error);
        }
        c.monitor.machine_changed();
    } else if (action == "sid_card") {
        hold(c);
        c.dbg.cancel_step();
        if (!media->sid_card(string_arg(args, "model", "6581"), error)) {
            fail(error);
        }
        c.monitor.machine_changed();
    } else if (action == "printer_on") {
        const std::string raw = string_arg(args, "path", "");
        const std::string text_file = string_arg(args, "text_path", "");
        if (!media->printer_plug(raw, text_file, error)) {
            fail(error);
        }
    } else if (action == "printer_off") {
        media->printer_unplug();
    } else if (action == "printer_read") {
        const std::string bytes = media->printer_take();
        return structured({{"printed", escape_bytes(bytes)}, {"count", bytes.size()}, {"printer", media->printer_describe()}});
    } else if (action == "serial_on" || action == "serial_off") {
        hold(c);
        c.dbg.cancel_step();
        if (!media->serial_card(action == "serial_on", static_cast<unsigned>(int_arg(args, "slot", kSerialSlot, 0, 3)), error)) {
            fail(error);
        }
        c.monitor.machine_changed();
    } else if (action == "serial_line") {
        if (!media->serial_peer(string_arg(args, "peer"), error)) {
            fail(error);
        }
    } else if (action == "serial_flow") {
        media->serial_flow(string_arg(args, "flow") == "rts");
    } else if (action == "serial_send") {
        if (args.contains("hex")) {
            const std::string hex = string_arg(args, "hex");
            if (hex.size() % 2 != 0 || !std::all_of(hex.begin(), hex.end(), [](char ch) { return std::isxdigit(static_cast<unsigned char>(ch)) != 0; })) {
                fail("hex is pairs of hex digits");
            }
            std::string bytes;
            for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
                bytes.push_back(static_cast<char>(std::stoi(hex.substr(i, 2), nullptr, 16)));
            }
            media->serial_queue(bytes);
        } else {
            media->serial_queue(string_arg(args, "text"));
        }
    } else if (action == "serial_send_file") {
        if (!media->serial_queue_file(string_arg(args, "path"), error)) {
            fail(error);
        }
    } else if (action == "serial_capture") {
        if (!media->serial_capture(string_arg(args, "path", ""), error)) {
            fail(error);
        }
    } else if (action == "serial_read") {
        const std::string bytes = media->serial_take();
        return structured({{"received", escape_bytes(bytes)}, {"count", bytes.size()}, {"serial", media->serial_describe()}});
    } else if (action == "card256_on" || action == "card256_off") {
        hold(c);
        c.dbg.cancel_step();
        if (!media->card256(action == "card256_on", static_cast<unsigned>(int_arg(args, "slot", kCardSlots, 0, 3)), error)) {
            fail(error);
        }
        c.monitor.machine_changed();
    } else if (action == "card256_usb" || action == "card256_flash") {
        if (!(action == "card256_usb" ? media->card256_usb(string_arg(args, "path"), error) : media->card256_flash(string_arg(args, "path"), error))) {
            fail(error);
        }
    } else if (action == "nanosd_on" || action == "nanosd_off") {
        hold(c);
        c.dbg.cancel_step();
        if (!media->nanosd(action == "nanosd_on", static_cast<unsigned>(int_arg(args, "slot", kNanoSdSlot, 0, 3)), error)) {
            fail(error);
        }
        c.monitor.machine_changed();
    } else if (action == "nanosd_sd") {
        if (!media->nanosd_sd(string_arg(args, "path"), error)) {
            fail(error);
        }
    } else if (action == "card256_mouse") {
        if (!media->card256_mouse(static_cast<int>(int_arg(args, "dx", 0, -10000, 10000)), static_cast<int>(int_arg(args, "dy", 0, -10000, 10000)),
                                  static_cast<unsigned>(int_arg(args, "buttons", 0, 0, 7)), error)) {
            fail(error);
        }
    } else if (action == "tape_play") {
        media->tape_play();
    } else if (action == "tape_rewind") {
        media->tape_rewind();
    } else if (action == "tape_stop" || action == "tape_eject") {
        if (!(action == "tape_stop" ? media->tape_stop(error) : media->tape_eject(error))) {
            fail(error);
        }
    } else if (action == "autostart" || action == "tape_autostart") {
        hold(c);
        c.dbg.cancel_step();
        if (!(action == "autostart" ? media->autostart(string_arg(args, "path"), error)
                                    : media->tape_autostart(string_arg(args, "path"), error))) {
            fail(error);
        }
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(int_arg(args, "timeout_ms", kDefaultTimeoutMs, 1, kMaxTimeoutMs));
        c.monitor.execute("x");
        std::string report;
        while (!c.monitor.active() && media->media().busy() && std::chrono::steady_clock::now() < deadline) {
            c.monitor.run(kFrameTicks, report);
        }
        const bool stopped = c.monitor.active();
        hold(c);
        const Media& m = media->media();
        json out{{"media", media->describe()},
                 {"tape", media->tape_describe()},
                 {"autostart", m.busy() ? (stopped ? "stopped at a checkpoint" : "timeout") : m.autostart_result()},
                 {"done", m.state() == AutostartState::Done},
                 {"screen", screen_rows(c)}};
        return structured(out);
    } else if (action != "status") {
        fail("action is attach, detach, autostart, status, tape_insert, tape_autostart, tape_record, tape_play, tape_stop, "
             "tape_rewind, tape_eject, cart_insert, cart_remove, cart_build, disk_insert, disk_eject, disk_card_on, disk_card_off, gamecard_on, "
             "gamecard_off, multichip_on, multichip_off, twodfx_on, twodfx_off, sid_card, card256_on, card256_off, card256_usb, card256_flash, card256_mouse, nanosd_on, "
             "nanosd_off, nanosd_sd, sddual_on, sddual_off, sd_insert, sd_eject, sd_build, printer_on, printer_off, printer_read, "
             "serial_on, serial_off, serial_line, serial_flow, serial_send, serial_send_file, serial_capture or serial_read");
    }
    return structured({{"media", media->describe()},
                       {"tape", media->tape_describe()},
                       {"cartridge", media->cart_describe()},
                       {"disk", media->disk_describe()},
                       {"gamecard", media->gamecard_describe()},
                       {"multichip", media->multichip_describe()},
                       {"twodfx", media->twodfx_describe()},
                       {"sid_card", media->sid_card_describe()},
                       {"card256", media->card256_describe()},
                       {"nanosd", media->nanosd_describe()},
                       {"sddual", media->sd_describe()},
                       {"printer", media->printer_describe()},
                       {"serial", media->serial_describe()}});
}

// The TVC256k++ in the machine and its slot; fails when no slot holds one
std::pair<const Card256*, unsigned> card256_of(Ctx& c) {
    for (unsigned slot = 0; slot < kCardSlots; ++slot) {
        if (const auto* card = dynamic_cast<const Card256*>(c.machine.cards().card(slot))) {
            return {card, slot};
        }
    }
    fail("no TVC256k++ in the machine (media card256_on puts it in)");
}

// A fast-RAM address: five hex digits
std::string hex5(std::uint32_t v) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%05X", v & 0xFFFFF);
    return buf;
}

std::string hex_bytes(const Card256& card, std::uint32_t at, unsigned count) {
    std::string out;
    const auto ram = card.fast_ram();
    for (unsigned i = 0; i < count; ++i) {
        out += (i ? " " : "") + hex2(at + i < ram.size() ? ram[at + i] : 0);
    }
    return out;
}

// design/cards.md "TVC256k++", "Registers" and "Overlay"; everything through peek_register, so nothing is cleared
json card256_tool(Ctx& c, const json& args) {
    const auto [card, slot] = card256_of(c);
    const std::string action = string_arg(args, "action", "registers");
    const auto reg = [&](unsigned index) { return card->peek_register(static_cast<std::uint8_t>(index)); };
    const auto word = [&](unsigned index) { return static_cast<unsigned>(reg(index) | reg(index + 1) << 8); };
    const auto bits16 = [](unsigned v) {
        std::string out(16, '0');
        for (unsigned i = 0; i < 16; ++i) {
            out[i] = (v >> (15 - i)) & 1 ? '1' : '0';
        }
        return out;
    };
    if (action == "registers") {
        std::string all;
        for (unsigned i = 0; i < 256; ++i) {
            all += (i ? " " : "") + hex2(reg(i));
        }
        static constexpr std::array<const char*, 4> kModes{"off", "2-colour text", "16-colour text", "bitmap"};
        const unsigned mode = reg(0xA0) & 3;
        return structured(
            {{"slot", slot},
             {"ports", hex2(0x10 + 0x10 * slot) + "h-" + hex2(0x1F + 0x10 * slot) + "h"},
             {"overlay",
              {{"mode", kModes[mode]},
               {"A0_mode", hex2(reg(0xA0))},
               {"A1_screen_base", hex2(reg(0xA1))}, {"screen_address", hex5(reg(0xA1) * 0x400u)},
               {"A2_colour_base", hex2(reg(0xA2))}, {"colour_address", hex5(reg(0xA2) * 0x400u)},
               {"A3_font_base", hex2(reg(0xA3))},
               {"A4_bitmap_base", hex2(reg(0xA4))},
               {"A5_scroll_x", hex2(reg(0xA5))}, {"A6_scroll_y", hex2(reg(0xA6))}, {"A7_border", hex2(reg(0xA7))},
               {"A8_line", reg(0xA8)}, {"A9_height", reg(0xA9)}}},
             {"sprites",
              {{"9E_sprite_base", hex2(reg(0x9E))},
               {"enabled", bits16(word(0x90))}, {"foreground", bits16(word(0x92))}, {"colour_mode_16", bits16(word(0x94))},
               {"collision_sprite_sprite", bits16(word(0x96))}, {"collision_sprite_background", bits16(word(0x98))},
               {"mask_sprite_sprite", bits16(word(0x9A))}, {"mask_sprite_background", bits16(word(0x9C))},
               {"irq", card->irq()}}},
             {"memory",
              {{"B0_u2_page", hex2(reg(0xB0))}, {"B1_u3_page", hex2(reg(0xB1))},
               {"psram_u2", hex4(word(0xB2))}, {"psram_u3", hex4(word(0xB4))},
               {"B7_rom_bank", reg(0xB7)}, {"psram_pages_used", card->psram_pages_used()}}},
             {"functions",
              {{"C0_running", hex2(reg(0xC0))}, {"C1_parameters", hex2(reg(0xC1))}, {"C2_result", hex2(reg(0xC2))},
               {"C6_chain", hex2(reg(0xC6))}, {"C7_bitmap_base", hex2(reg(0xC7))},
               {"firmware", std::to_string(reg(0xC3)) + "." + std::to_string(reg(0xC4))}}},
             {"usb", {{"D0_device", hex2(reg(0xD0))}, {"D1_buttons", hex2(reg(0xD1))}, {"D5_mouse_speed", hex2(reg(0xD5))},
                      {"D6_storage", hex2(reg(0xD6))}}},
             {"all", all},
             {"note", "read without side effects: the collision words and mouse movement are not cleared"}});
    }
    if (action == "sprites") {
        const auto only = int_arg(args, "sprite", -1, 0, 15);
        const bool pixels = bool_arg(args, "pixels", only >= 0);
        const std::uint32_t base = (reg(0x9E) & 7u) * 0x4000u;
        const auto ram = card->fast_ram();
        const auto at = [&](std::uint32_t a) -> std::uint8_t { return a < ram.size() ? ram[a] : 0; };
        json list = json::array();
        for (unsigned n = 0; n < 16; ++n) {
            if (only >= 0 && n != static_cast<unsigned>(only)) {
                continue;
            }
            const bool full = reg(0x80 + n) != 0;
            const unsigned phase = reg(0x50 + n);
            const std::uint32_t data = base + phase * (full ? 256u : 64u);
            json s{{"sprite", n}, {"x", word(n * 2)}, {"y", word(0x20 + n * 2)}, {"colour", reg(0x40 + n) & 0x0F},
                   {"phase", hex2(phase)}, {"foreground", reg(0x60 + n) != 0}, {"enabled", reg(0x70 + n) != 0},
                   {"colours", full ? 16 : 2}, {"data_address", hex5(data)}};
            if (pixels) {
                // 24 x 21: hex digit = colour, '.' = transparent (colour 8 in 16-colour data, a clear bit in 2-colour data)
                json rows = json::array();
                const unsigned colour = reg(0x40 + n) & 0x0F;
                for (unsigned row = 0; row < 21; ++row) {
                    std::string line;
                    for (unsigned k = 0; k < (full ? 12u : 3u); ++k) {
                        const std::uint8_t b = at(data + row * (full ? 12u : 3u) + k);
                        if (full) {
                            for (const unsigned nib : {static_cast<unsigned>(b >> 4), static_cast<unsigned>(b & 0x0F)}) {
                                line += nib == 8 ? '.' : "0123456789ABCDEF"[nib];
                            }
                        } else {
                            for (unsigned bit = 0; bit < 8; ++bit) {
                                line += (b & (0x80 >> bit)) ? "0123456789ABCDEF"[colour] : '.';
                            }
                        }
                    }
                    rows.push_back(line);
                }
                s["pixels"] = rows;
                s["bytes"] = hex_bytes(*card, data, full ? 252u : 63u);
            }
            list.push_back(std::move(s));
        }
        return structured({{"slot", slot}, {"sprite_base", hex2(reg(0x9E))}, {"sprites", list}});
    }
    if (action == "tiles") {
        const std::uint32_t screen = reg(0xA1) * 0x400u;
        const std::uint32_t colours = reg(0xA2) * 0x400u;
        const auto rows = static_cast<unsigned>(int_arg(args, "rows", 30, 1, 30));
        json codes = json::array();
        json colour_rows = json::array();
        for (unsigned r = 0; r < rows; ++r) {
            codes.push_back(hex_bytes(*card, screen + r * 32, 32));
            colour_rows.push_back(hex_bytes(*card, colours + r * 32, 32));
        }
        static constexpr std::array<const char*, 4> kModes{"off", "2-colour text", "16-colour text", "bitmap"};
        return structured({{"slot", slot}, {"mode", kModes[reg(0xA0) & 3]},
                           {"screen_address", hex5(screen)},
                           {"colour_address", hex5(colours)},
                           {"columns", 32}, {"rows", rows}, {"codes", codes}, {"colours", colour_rows},
                           {"note", "fast-RAM addresses; colours are IRGB in the low nibble (2-colour text only; 16-colour glyphs carry their own)"}});
    }
    if (action == "ram") {
        const bool psram = bool_arg(args, "psram", false);
        const auto limit = static_cast<std::int64_t>(psram ? Card256::kPsramSize : Card256::kFastSize);
        const auto address = static_cast<std::uint32_t>(int_arg(args, "address", 0, 0, limit - 1));
        const auto length = static_cast<unsigned>(int_arg(args, "length", 256, 1, 0x10000));
        if (address + length > limit) {
            fail("the range goes past the end of the " + std::string(psram ? "PSRAM" : "fast RAM"));
        }
        std::vector<std::uint8_t> data(length);
        const auto ram = card->fast_ram();
        for (unsigned i = 0; i < length; ++i) {
            data[i] = psram ? card->psram_read(address + i) : ram[address + i];
        }
        const std::string format = string_arg(args, "format", "hex");
        std::string hex;
        if (format == "hex") {
            for (unsigned i = 0; i < length; ++i) {
                hex += (i ? " " : "") + hex2(data[i]);
            }
        } else if (format != "base64") {
            fail("format is hex or base64");
        }
        char where[16];
        std::snprintf(where, sizeof where, "%06X", static_cast<unsigned>(address));
        return structured({{"slot", slot}, {"memory", psram ? "psram" : "fast"}, {"address", std::string(where)}, {"length", length},
                           {"format", format}, {"data", format == "hex" ? hex : base64(data)}});
    }
    fail("action is registers, sprites, tiles or ram");
}

json monitor_command(Ctx& c, const json& args) {
    if (!c.options.allow_monitor_command) {
        fail("monitor_command is off; start freeTVC with --mcp-allow-monitor to allow it");
    }
    hold(c);
    return {{"content", json::array({text(c.monitor.execute(string_arg(args, "command")))})}};
}

}  // namespace

const char* instructions() {
    return "freeTVC emulates the Videoton TV Computer (TVC): Z80A at 3.125 MHz, 50 Hz PAL, one frame = 62800 T-states. "
           "Tools that advance time (run, step, type_text, press_keys) leave the machine paused; control resume lets it run "
           "freely (in the GUI, in real time) until a checkpoint stops it. Typical use: machine_info; type_text with \\n for "
           "RETURN to give BASIC commands; screen_text to read the text screen; screenshot for graphics; breakpoints + run "
           "to debug. Address arguments: integers are decimal; strings are monitor expressions with hex as default "
           "(\"8000\" = 8000h; hex starting with a letter needs $ or 0: \"$C000\"), #decimal, registers, symbols such as "
           "OS_ENTRY, and BANK:offset (SYS, EXT, CART, U0-U3, VID0-VID3). After boot the OS has U0 in page 0, so its RST "
           "vectors run from U0 (e.g. U0:OS_ENTRY). After power-on the start-up logo waits for a key: run about 330 "
           "frames, press_keys [\"SPACE\"] (settle_frames 100), and BASIC shows 'ok'; keys pressed earlier are lost.";
}

std::vector<Tool> make_tools(Monitor& monitor, const McpOptions& options) {
    auto ctx = std::make_shared<Ctx>(Ctx{monitor, monitor.debugger(), monitor.debugger().machine(), options});
    const auto bind = [ctx](json (*fn)(Ctx&, const json&)) {
        return [ctx, fn](const json& args) { return fn(*ctx, args); };
    };
    const json no_args = schema(json::object());
    const json timeout{{"type", "integer"}, {"description", "wall-clock limit in ms (default 10000, at most 60000)"}};
    std::vector<Tool> tools;
    tools.push_back({"machine_info", "Machine state", "Machine, ROMs, paused or running, PC, frame, beam, paging, checkpoints.",
                     no_args, annotations(true, false, true), bind(machine_info)});
    tools.push_back({"control", "Pause, resume, reset", "pause; resume (runs freely until a checkpoint); reset (the reset button, memory kept); power_cycle (RAM cleared).",
                     schema({{"action", {{"type", "string"}, {"enum", {"pause", "resume", "reset", "power_cycle"}}}}}, {"action"}),
                     annotations(false, true, false), bind(control)});
    tools.push_back({"run", "Run the machine", "Runs for frames (default 1), tstates or instructions, or until a CPU address (until), and stops early on a checkpoint. Returns why it stopped and the next instruction; leaves the machine paused.",
                     schema({{"frames", {{"type", "integer"}, {"minimum", 1}, {"maximum", kMaxFrames}}},
                             {"tstates", {{"type", "integer"}, {"minimum", 1}}},
                             {"instructions", {{"type", "integer"}, {"minimum", 1}}},
                             {"until", kAddressSchema}, {"timeout_ms", timeout}}),
                     annotations(false, false, false), bind(run)});
    tools.push_back({"step", "Step instructions", "into: one instruction (count times); over: a CALL or RST runs until SP is back at its level (after RST 30h's function byte, RST 18h's BASIC ops or an inline string), block repeats and HALT to the next instruction; out: until a return leaves the current stack frame; line / line_over: until the PC is on another source line (needs a debug file loaded with symbols load), line_over running calls through.",
                     schema({{"mode", {{"type", "string"}, {"enum", {"into", "over", "out", "line", "line_over"}}}}, {"count", {{"type", "integer"}, {"minimum", 1}}}, {"timeout_ms", timeout}}),
                     annotations(false, false, false), bind(step)});
    tools.push_back({"registers", "Z80 registers", "Returns all registers; with set (e.g. {\"a\": 5, \"hl\": \"9000\", \"pc\": \"8000\"}) changes them first (the machine is paused).",
                     schema({{"set", {{"type", "object"}, {"description", "register name -> integer or expression string"}}}}),
                     annotations(false, false, true), bind(registers)});
    tools.push_back({"memory", "Read and write memory", "read / write / fill / search through the current paging, or a bank with BANK:offset (works even when the bank is not paged in). data: hex string (\"21 00 90\"), base64 (format base64) or an array of integers.",
                     schema({{"action", {{"type", "string"}, {"enum", {"read", "write", "fill", "search"}}}}, {"address", kAddressSchema},
                             {"length", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65536}}},
                             {"data", {{"anyOf", json::array({{{"type", "string"}}, {{"type", "array"}, {"items", {{"type", "integer"}}}}})}}},
                             {"format", {{"type", "string"}, {"enum", {"hex", "base64"}}}}}, {"address"}),
                     annotations(false, true, false), bind(memory)});
    tools.push_back({"disassemble", "Disassemble", "Z80 listing in sjasmplus syntax with labels; RST 30h shows its system-call name, RST 18h its BASIC ops, the ROM's inline print its string; with a debug file loaded each line has its source line. Default: 16 instructions from PC.",
                     schema({{"address", kAddressSchema}, {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 512}}}}),
                     annotations(true, false, true), bind(disassemble)});
    tools.push_back({"breakpoints", "Checkpoints", "add (kind exec/load/store/in/out/irq/line or an array; address and end; condition is a monitor expression like \"A == 3\" or \"([{SP}] & 80h) == 0\"; stop false only logs hits, see trace), list, delete/enable/disable (id), clear.",
                     schema({{"action", {{"type", "string"}, {"enum", {"add", "list", "delete", "enable", "disable", "clear"}}}},
                             {"kind", {{"anyOf", json::array({{{"type", "string"}}, {{"type", "array"}, {"items", {{"type", "string"}}}}})}}},
                             {"address", kAddressSchema}, {"end", kAddressSchema},
                             {"condition", {{"type", "string"}}}, {"stop", {{"type", "boolean"}}}, {"temporary", {{"type", "boolean"}}},
                             {"ignore", {{"type", "integer"}, {"minimum", 0}}}, {"id", {{"type", "integer"}, {"minimum", 1}}}}),
                     annotations(false, false, false), bind(breakpoints)});
    tools.push_back({"screenshot", "Screenshot", "PNG of the TV picture (area tv, 650 x 287) or the whole raster with sync and blanking (area full, 800 x 314).",
                     schema({{"area", {{"type", "string"}, {"enum", {"tv", "full"}}}}}), annotations(true, false, true), bind(screenshot)});
    tools.push_back({"screen_text", "Screen as text", "The text screen as lines, from the OS's ASCII copy at U0:0100 (what BASIC and the editor print).",
                     no_args, annotations(true, false, true), bind(screen_text)});
    tools.push_back({"type_text", "Type text", "Types text on the TVC keyboard (\\n = RETURN) while the machine runs; by default waits until it is typed plus settle_frames and returns the screen text.",
                     schema({{"text", {{"type", "string"}}}, {"wait", {{"type", "boolean"}}}, {"settle_frames", {{"type", "integer"}, {"minimum", 0}}}, {"timeout_ms", timeout}}, {"text"}),
                     annotations(false, false, false), bind(type_text_tool)});
    tools.push_back({"press_keys", "Press keys", "Holds keys together for hold_frames (default 3): names like \"A\", \"5\", \"SPACE\", \"RETURN\", \"SHIFT\", \"CTRL\", \"ESC\", \"UP\", or \"ROW:COLUMN\" of the matrix (rows 8-9 are the joysticks); with the GameCard, \"JOY3_UP\" ... \"JOY4_FIRE\" (rows 10-11: b0 up, b1 down, b2 left, b3 right, b4 fire).",
                     schema({{"keys", {{"type", "array"}, {"items", {{"type", "string"}}}}}, {"hold_frames", {{"type", "integer"}, {"minimum", 1}}},
                             {"wait", {{"type", "boolean"}}}, {"settle_frames", {{"type", "integer"}, {"minimum", 0}}}, {"timeout_ms", timeout}}, {"keys"}),
                     annotations(false, false, false), bind(press_keys)});
    tools.push_back({"game_saves", "Game saves", "Snapshots kept per program in <root>/<game>/<game> NNN.tvcs with a PNG of the screen beside each. list: the root, the current game and every game with saves; list with game: its saves (name, when: the file's time), newest first. save (game, default the program started last; name: the file's name, replacing a save of that name, default the next \"<game> NNN\"): a save of the machine as it is. check (path): the files and folders the save names (tape, cassette, cartridge, disks, SD images, the NanoSD's folder, the TVC256k++'s pendrive) and whether each is there now; nothing changes. load (path; moved: {old path: new path} for ones that moved): the board, cards and ROM set of the save, the media by path, then the memory; missing media are left out with a message; leaves the machine paused. delete (path): a save and its picture.",
                     schema({{"action", {{"type", "string"}, {"enum", {"list", "save", "check", "load", "delete"}}}},
                             {"game", {{"type", "string"}}}, {"name", {{"type", "string"}}}, {"path", {{"type", "string"}}}, {"moved", {{"type", "object"}}}}),
                     annotations(false, true, false), bind(game_saves)});
    tools.push_back({"snapshot", "Machine states", "save / load / delete a named in-memory copy of the whole machine, or list them; load returns to exactly that moment. save_file / load_file (path) write and read a TVCS snapshot file, with the tape and cassette by path; load_file follows the file's board and ROM set (found by CRC-32) and leaves the machine paused. rewind (seconds, default 1) goes back in machine time to a point kept once a second for the last 60 s; asking again goes further back.",
                     schema({{"action", {{"type", "string"}, {"enum", {"save", "load", "delete", "list", "save_file", "load_file", "rewind"}}}},
                             {"name", {{"type", "string"}}}, {"path", {{"type", "string"}}},
                             {"seconds", {{"type", "integer"}, {"minimum", 1}}}}),
                     annotations(false, true, false), bind(snapshot)});
    tools.push_back({"symbols", "Symbols", "lookup (name, or address: with its source line when known), list (filter), add (name, address), load (path: a label file (name = value, name EQU value, sjasmplus --sym) or a debug file with source lines: sjasmplus --sld or --lst, SDCC .noi/.cdb/.map, z88dk .map; the format is detected). ROM symbols are built in.",
                     schema({{"action", {{"type", "string"}, {"enum", {"lookup", "list", "add", "load"}}}}, {"name", {{"type", "string"}}},
                             {"address", kAddressSchema}, {"filter", {{"type", "string"}}}, {"path", {{"type", "string"}}}}),
                     annotations(false, false, false), bind(symbols)});
    tools.push_back({"trace", "Instruction history", "The last count instructions with registers (before each), and the latest checkpoint hits (including log-only ones).",
                     schema({{"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4096}}}, {"hits", {{"type", "integer"}, {"minimum", 0}}}}),
                     annotations(true, false, true), bind(trace)});
    tools.push_back({"io_state", "I/O state", "Video mode, border, palette, beam, CRTC registers, sound, IRQ flip-flop and paging.",
                     no_args, annotations(true, false, true), bind(io_state)});
    tools.push_back({"evaluate", "Evaluate", "Evaluates a monitor expression, e.g. \"HL + 2\", \"{SP}\", \"[U2:1000]\", \"ZF\", \"OS_ENTRY\".",
                     schema({{"expression", {{"type", "string"}}}}, {"expression"}), annotations(true, false, true), bind(evaluate)});
    tools.push_back({"load_binary", "Load a binary", "Copies a host file into memory at address (CPU view or BANK:offset); jump true also sets PC there.",
                     schema({{"path", {{"type", "string"}}}, {"address", kAddressSchema}, {"jump", {{"type", "boolean"}}}}, {"path", "address"}),
                     annotations(false, true, false), bind(load_binary)});
    tools.push_back({"media", "Program media", "attach (path: a .cas file becomes the host cassette; BASIC LOAD reads it), detach, status, or autostart (path): cold start, LOAD and RUN in fast mode, waits until the program runs and returns the screen text; leaves the machine paused. The tape deck plays the real signal to the ROM's own tape routines: tape_insert (path: .wav, .ttp, or .cas played as a tape), tape_autostart (path, like autostart through the deck), tape_record (path: a WAV written at tape_stop or tape_eject), tape_play, tape_stop, tape_rewind, tape_eject. Cartridges: cart_insert (path: a .crt/.rom/.bin image; cart_type plain, kilocart, megacart or multicart: a MultiCart v2 image goes by its firmware's header, the others by size, when omitted; bank: a plain board's switches) and cart_remove both power the machine on and leave it paused. A MultiCart v2 (flash with a file-select menu at power-on; its firmware saves, deletes and renames there and through BASIC's EXT 0) writes its flash back to the image file. cart_build (path: the new image; files: .cas programs and MOPS carts in menu order; language: en, hu or es for its firmware) writes a MultiCart image without inserting it. Disks (HBF card, drives A-D): disk_insert (path: a .dsk/.img raw image; drive, default A; puts the card in first, powering on, when it is out), disk_eject (drive; a written disk is saved to its file first), disk_card_on / disk_card_off (power on). The card takes over LOAD and SAVE. autostart with a .dsk/.img path loads its first CAS file from A:. GameCard (SN76489 sound, joysticks 3/4 as press_keys JOY3_*/JOY4_*): gamecard_on (slot, default 1) and gamecard_off, both power on. Multichip sound card (SN76489, SAA1099 and YM3812 on the TV's sound; its start-up screen plays a demo on each chip after SPACE): multichip_on (slot, default 2) and multichip_off, both power on. 2dfx graphics card (Vaczkó Károly's: sprites, 2D and 3D graphics over the paper; commands on port F8h, data on F9h, status read from F8h; not beside the TVC256k++; beside a SID card F8h/F9h are the 2dfx's): twodfx_on (slot, default 2 or the first free) and twodfx_off, both power on. SID card (a MOS 6581 or 8580 at ports E0h-FFh, outside the slots, where WinTVC puts it; write registers 00h-18h at E0h-F8h, read POTX/POTY/OSC3/ENV3 at F9h-FCh): sid_card (model: 6581, 8580 or off) powers on. TVC256k++ (256 KiB fast RAM in place of U2/U3, 8 MiB PSRAM, its EXT n command line, a 256 x 240 overlay with 16 sprites over the TVC's picture; status reports its mode and collisions): card256_on (slot, default 0 or the first free) and card256_off, both power on. Its drives serve LOAD and SAVE (pendrive, PSRAM, flash, DSK; the card's own CLI selects them): card256_usb (path: a host folder becomes the pendrive, or 'mouse' or 'none' for the single USB port), card256_flash (path: a littlefs image or a folder packed into one), card256_mouse (dx, dy: movement, y down; buttons: 1 left, 2 right, 4 middle). NanoSD IO card (an SD card through an Arduino Nano; it takes over LOAD and SAVE and shows its file-select menu at a cold start until ESC or a choice): nanosd_on (slot, default 3) and nanosd_off, both power on; nanosd_sd (path: a host folder becomes its SD card, or 'none' for an empty socket). SD Dual cartridge (Sebestyén Pál's; two SD sockets on SPI, VT-DOS 1.2 and a file manager in its flash; it boots to BASIC: EXT 2 starts the VT-DOS CLI, EXT 3 the file manager, LOAD and SAVE use the card): sddual_on (firmware: plain or gamer) and sddual_off take the cartridge port and power on; sd_insert (socket: 0 microSD, the first drives, or 1 SD; path: a raw whole-card image, FAT12/16/32, MBR or super floppy, written in place) and sd_eject (socket); sd_build (path: the new image; size_mib, default 64; files: host files and folders for its root) writes a FAT card image without inserting it. Printer on the Centronics port (none by default: an LPRINT then waits after its first character until STOP, which is CTRL+ESC): printer_on (path: raw bytes appended to a file; text_path: UTF-8 text appended to a file; both optional, the output is always kept for printer_read), printer_off, printer_read (what was printed since the last read; bytes outside printable ASCII as \\r \\n \\xNN). Videoton's RS-232 card (an 8251A; BASIC uses it as device #6: LIST #6, SAVE #6, LOAD #6, PRINT #6, OPEN #6): serial_on (slot, default 1) and serial_off, both power on; serial_line (peer: off = cable unplugged, on = this server is the peer, listen[:PORT] or connect:PORT for a TCP peer on 127.0.0.1); serial_flow (flow: rts, the default, sends to the TVC only while its RTS is on, or none); serial_send (text, or hex: bytes for the TVC to receive); serial_send_file (path); serial_capture (path: append what the TVC sends to a file; empty stops it); serial_read (what the TVC sent since the last read).",
                     schema({{"action", {{"type", "string"}, {"enum", {"attach", "detach", "autostart", "status", "tape_insert", "tape_autostart", "tape_record",
                                                                       "tape_play", "tape_stop", "tape_rewind", "tape_eject", "cart_insert", "cart_remove",
                                                                       "cart_build", "disk_insert", "disk_eject", "disk_card_on", "disk_card_off",
                                                                       "gamecard_on", "gamecard_off", "multichip_on", "multichip_off", "twodfx_on", "twodfx_off", "sid_card",
                                                                       "card256_on", "card256_off",
                                                                       "card256_usb", "card256_flash", "card256_mouse",
                                                                       "nanosd_on", "nanosd_off", "nanosd_sd",
                                                                       "sddual_on", "sddual_off", "sd_insert", "sd_eject", "sd_build",
                                                                       "printer_on", "printer_off", "printer_read", "serial_on", "serial_off",
                                                                       "serial_line", "serial_flow", "serial_send", "serial_send_file",
                                                                       "serial_capture", "serial_read"}}}},
                             {"text_path", {{"type", "string"}}},
                             {"peer", {{"type", "string"}}},
                             {"flow", {{"type", "string"}, {"enum", {"rts", "none"}}}},
                             {"text", {{"type", "string"}}},
                             {"hex", {{"type", "string"}}},
                             {"socket", {{"type", "integer"}, {"minimum", 0}, {"maximum", 1}}},
                             {"size_mib", {{"type", "integer"}, {"minimum", 1}, {"maximum", 2 * 1024 * 1024}}},
                             {"firmware", {{"type", "string"}, {"enum", {"plain", "gamer"}}}},
                             {"model", {{"type", "string"}, {"enum", {"6581", "8580", "off"}}}},
                             {"slot", {{"type", "integer"}, {"minimum", 0}, {"maximum", 3}}},
                             {"dx", {{"type", "integer"}}}, {"dy", {{"type", "integer"}}},
                             {"buttons", {{"type", "integer"}, {"minimum", 0}, {"maximum", 7}}},
                             {"path", {{"type", "string"}}}, {"timeout_ms", timeout},
                             {"drive", {{"type", "string"}, {"enum", {"A", "B", "C", "D"}}}},
                             {"cart_type", {{"type", "string"}, {"enum", {"plain", "kilocart", "megacart", "multicart", "sddual"}}}},
                             {"files", {{"type", "array"}, {"items", {{"type", "string"}}}}},
                             {"language", {{"type", "string"}, {"enum", {"en", "hu", "es"}}}},
                             {"bank", {{"type", "integer"}, {"minimum", 0}, {"maximum", 63}}}}, {"action"}),
                     annotations(false, true, false), bind(media)});
    tools.push_back({"card256", "TVC256k++ card state", "Inspects dikdom's TVC256k++ card without side effects (collision words and mouse movement are not cleared). registers: every register (all 256 as hex) and the decoded overlay, sprite, memory, function and USB registers; sprites: the 16 sprites' position, colour, phase, flags and data address, with pixels (24 x 21 rows, hex colour digits, '.' transparent) and raw bytes for one sprite or with pixels true; tiles: the overlay's 32 x 30 screen codes and colour bytes from the card's fast RAM (rows: 1-30); ram: read the card's fast RAM (288 KiB; address 0-47FFFh as an integer) or, with psram true, its 8 MiB PSRAM (format hex or base64).",
                     schema({{"action", {{"type", "string"}, {"enum", {"registers", "sprites", "tiles", "ram"}}}},
                             {"sprite", {{"type", "integer"}, {"minimum", 0}, {"maximum", 15}}},
                             {"pixels", {{"type", "boolean"}}},
                             {"rows", {{"type", "integer"}, {"minimum", 1}, {"maximum", 30}}},
                             {"address", {{"type", "integer"}, {"minimum", 0}}},
                             {"length", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65536}}},
                             {"psram", {{"type", "boolean"}}},
                             {"format", {{"type", "string"}, {"enum", {"hex", "base64"}}}}}),
                     annotations(true, false, true), bind(card256_tool)});
    tools.push_back({"monitor_command", "Text monitor command", "Runs one text monitor command (VICE-style; 'help' lists them). Off unless the emulator was started with --mcp-allow-monitor.",
                     schema({{"command", {{"type", "string"}}}}, {"command"}), annotations(false, true, false), bind(monitor_command)});
    return tools;
}

}  // namespace freetvc::remote::mcp
