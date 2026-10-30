// SPDX-License-Identifier: GPL-3.0-or-later
#include "remote/gdb.hpp"

#include <algorithm>
#include <cstdio>
#include <optional>
#include <span>

namespace freetvc::remote {

namespace {

constexpr std::uint64_t kStepBudget = 100 * kTStatesPerFrame * kTicksPerTState;  // as the monitor's z
constexpr std::size_t kMaxRead = 0x1F00;  // fits PacketSize 4000h as hex
constexpr unsigned kRegisterCount = 13;
constexpr std::uint8_t kSigInt = 2;
constexpr std::uint8_t kSigTrap = 5;

int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

// A hex number up to the first non-hex character; `text` is advanced past it.
std::optional<std::uint32_t> take_hex(std::string_view& text) {
    std::uint32_t value = 0;
    std::size_t i = 0;
    for (; i < text.size() && i < 8 && hex_digit(text[i]) >= 0; ++i) {
        value = (value << 4) | static_cast<std::uint32_t>(hex_digit(text[i]));
    }
    if (i == 0) {
        return std::nullopt;
    }
    text.remove_prefix(i);
    return value;
}

bool skip(std::string_view& text, char c) {
    if (text.empty() || text.front() != c) {
        return false;
    }
    text.remove_prefix(1);
    return true;
}

void put_hex2(std::string& out, unsigned value) {
    static constexpr char kDigits[] = "0123456789abcdef";
    out.push_back(kDigits[(value >> 4) & 15]);
    out.push_back(kDigits[value & 15]);
}

// Registers go over the wire as target byte order: low byte first
std::string word_le(std::uint16_t value) {
    std::string out;
    put_hex2(out, value & 0xFF);
    put_hex2(out, value >> 8);
    return out;
}

std::string hex_number(std::uint32_t value) {
    char buffer[12];
    std::snprintf(buffer, sizeof buffer, "%x", value);
    return buffer;
}

std::optional<std::uint16_t> parse_word_le(std::string_view hex) {
    if (hex.size() < 4) {
        return std::nullopt;
    }
    int digits[4];
    for (int i = 0; i < 4; ++i) {
        digits[i] = hex_digit(hex[static_cast<std::size_t>(i)]);
        if (digits[i] < 0) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint16_t>((digits[0] << 4 | digits[1]) | (digits[2] << 4 | digits[3]) << 8);
}

}  // namespace

GdbServer::GdbServer(Monitor& monitor)
    : monitor_(monitor), dbg_(monitor.debugger()), machine_(monitor.debugger().machine()) {}

GdbServer::~GdbServer() {
    for (const auto& [point, id] : points_) {
        dbg_.remove(id);
    }
}

const std::string& GdbServer::target_xml() {
    // Names as GDB's z80-tdep.c checks them; regnum is MAME's order, which DeZog's gdb remote expects
    static const std::string xml =
        "<?xml version=\"1.0\"?>\n"
        "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
        "<target version=\"1.0\">\n"
        "  <architecture>z80</architecture>\n"
        "  <feature name=\"org.gnu.gdb.z80.cpu\">\n"
        "    <flags id=\"af_flags\" size=\"2\">\n"
        "      <field name=\"C\" start=\"0\" end=\"0\"/>\n"
        "      <field name=\"N\" start=\"1\" end=\"1\"/>\n"
        "      <field name=\"P/V\" start=\"2\" end=\"2\"/>\n"
        "      <field name=\"F3\" start=\"3\" end=\"3\"/>\n"
        "      <field name=\"H\" start=\"4\" end=\"4\"/>\n"
        "      <field name=\"F5\" start=\"5\" end=\"5\"/>\n"
        "      <field name=\"Z\" start=\"6\" end=\"6\"/>\n"
        "      <field name=\"S\" start=\"7\" end=\"7\"/>\n"
        "    </flags>\n"
        "    <reg name=\"af\" bitsize=\"16\" type=\"af_flags\" regnum=\"0\"/>\n"
        "    <reg name=\"bc\" bitsize=\"16\" type=\"uint16\" regnum=\"1\"/>\n"
        "    <reg name=\"de\" bitsize=\"16\" type=\"data_ptr\" regnum=\"2\"/>\n"
        "    <reg name=\"hl\" bitsize=\"16\" type=\"data_ptr\" regnum=\"3\"/>\n"
        "    <reg name=\"af'\" bitsize=\"16\" type=\"af_flags\" regnum=\"4\"/>\n"
        "    <reg name=\"bc'\" bitsize=\"16\" type=\"uint16\" regnum=\"5\"/>\n"
        "    <reg name=\"de'\" bitsize=\"16\" type=\"data_ptr\" regnum=\"6\"/>\n"
        "    <reg name=\"hl'\" bitsize=\"16\" type=\"data_ptr\" regnum=\"7\"/>\n"
        "    <reg name=\"ix\" bitsize=\"16\" type=\"data_ptr\" regnum=\"8\"/>\n"
        "    <reg name=\"iy\" bitsize=\"16\" type=\"data_ptr\" regnum=\"9\"/>\n"
        "    <reg name=\"sp\" bitsize=\"16\" type=\"data_ptr\" regnum=\"10\"/>\n"
        "    <reg name=\"pc\" bitsize=\"16\" type=\"code_ptr\" regnum=\"11\"/>\n"
        "    <reg name=\"ir\" bitsize=\"16\" type=\"uint16\" regnum=\"12\"/>\n"
        "  </feature>\n"
        "</target>\n";
    return xml;
}

std::string GdbServer::frame(std::string_view payload) {
    std::string out = "$";
    unsigned sum = 0;
    for (const char c : payload) {
        if (c == '$' || c == '#' || c == '}' || c == '*') {  // '*' would start run-length encoding
            out.push_back('}');
            out.push_back(static_cast<char>(c ^ 0x20));
            sum += static_cast<unsigned char>('}') + static_cast<unsigned char>(c ^ 0x20);
        } else {
            out.push_back(c);
            sum += static_cast<unsigned char>(c);
        }
    }
    out.push_back('#');
    put_hex2(out, sum & 0xFF);
    return out;
}

void GdbServer::begin_session() {
    end_session();
    monitor_.enter("GDB");
}

void GdbServer::end_session() {
    for (const auto& [point, id] : points_) {
        dbg_.remove(id);
    }
    points_.clear();
    running_ = false;
    ack_ = true;
    swbreak_ = false;
    pending_.clear();
    last_sent_.clear();
    if (monitor_.active()) {
        monitor_.execute("x");
    }
}

std::string GdbServer::reply(std::string_view payload) {
    last_sent_ = frame(payload);
    return last_sent_;
}

std::string GdbServer::feed(std::string_view bytes) {
    std::string out;
    for (const char c : bytes) {
        if (pending_.empty()) {
            if (c == '$') {
                pending_.push_back(c);
            } else if (c == '-') {
                out += last_sent_;
            } else if (c == '\x03' && running_ && !monitor_.active()) {
                monitor_.enter("GDB interrupt");
                out += poll();
            }
            continue;  // '+' and noise between packets
        }
        pending_.push_back(c);
        const std::size_t hash = pending_.find('#');
        if (hash == std::string::npos || pending_.size() < hash + 3) {
            if (pending_.size() > 0x10000) {
                pending_.clear();  // no packet is that long
            }
            continue;
        }
        const std::string_view body = std::string_view(pending_).substr(1, hash - 1);
        unsigned sum = 0;
        for (const char b : body) {
            sum += static_cast<unsigned char>(b);
        }
        const int high = hex_digit(pending_[hash + 1]);
        const int low = hex_digit(pending_[hash + 2]);
        const bool good = high >= 0 && low >= 0 && static_cast<unsigned>(high << 4 | low) == (sum & 0xFF);
        if (ack_) {
            out.push_back(good ? '+' : '-');
        }
        if (good || !ack_) {
            const bool no_ack = body == "QStartNoAckMode";
            bool answer = true;
            const std::string payload = handle(body, answer);
            if (answer) {
                out += reply(payload);
            }
            if (no_ack) {
                ack_ = false;
            }
        }
        pending_.clear();
    }
    return out;
}

std::string GdbServer::handle(std::string_view p, bool& answer) {
    if (p.empty()) {
        return {};
    }
    const char command = p.front();
    std::string_view args = p.substr(1);
    switch (command) {
    case '?':
        return stop_reply(kSigTrap, {});
    case 'g':
        return registers();
    case 'G':
        for (unsigned n = 0; n < kRegisterCount; ++n) {
            const auto value = parse_word_le(args.substr(std::min<std::size_t>(args.size(), n * 4)));
            if (!value || !write_register(n, *value)) {
                return "E01";
            }
        }
        return "OK";
    case 'p': {
        const auto n = take_hex(args);
        return n ? read_register(*n) : "E01";
    }
    case 'P': {
        const auto n = take_hex(args);
        if (!n || !skip(args, '=')) {
            return "E01";
        }
        const auto value = parse_word_le(args);
        return value && write_register(*n, *value) ? "OK" : "E01";
    }
    case 'm':
        return read_memory(args);
    case 'M':
        return write_memory(args);
    case 'c':
        resume(args);
        answer = false;
        return {};
    case 'C': {
        const std::size_t semicolon = args.find(';');  // the signal is ignored
        resume(semicolon == std::string_view::npos ? std::string_view{} : args.substr(semicolon + 1));
        answer = false;
        return {};
    }
    case 's':
    case 'S': {
        if (command == 'S') {
            const std::size_t semicolon = args.find(';');
            args = semicolon == std::string_view::npos ? std::string_view{} : args.substr(semicolon + 1);
        }
        if (!args.empty()) {
            const auto address = take_hex(args);
            if (!address || !write_register(11, static_cast<std::uint16_t>(*address))) {
                return "E01";
            }
        }
        if (!monitor_.active()) {
            monitor_.enter("GDB step");
        }
        return stop_reply(kSigTrap, stop_reason(dbg_.step(1, kStepBudget)));
    }
    case 'Z':
        return insert_point(args);
    case 'z':
        return remove_point(args);
    case 'D':
        end_session();
        return "OK";
    case 'k':
        end_session();
        answer = false;
        return {};
    case 'H':
        return "OK";
    case 'T':
        return "OK";  // thread 1 is alive
    case 'q':
    case 'Q':
        return query(p);
    default:
        return {};  // not supported: vCont, X, !, and the rest
    }
}

std::string GdbServer::query(std::string_view p) {
    if (p.starts_with("qSupported")) {
        swbreak_ = p.find("swbreak+") != std::string_view::npos;
        return "PacketSize=4000;qXfer:features:read+;QStartNoAckMode+;swbreak+;hwbreak+";
    }
    if (p == "QStartNoAckMode") {
        return "OK";
    }
    if (p.starts_with("qXfer:features:read:")) {
        std::string_view rest = p.substr(20);
        const std::size_t colon = rest.find(':');
        if (colon == std::string_view::npos || rest.substr(0, colon) != "target.xml") {
            return "E00";
        }
        rest.remove_prefix(colon + 1);
        const auto offset = take_hex(rest);
        const bool comma = skip(rest, ',');
        const auto length = take_hex(rest);
        if (!offset || !comma || !length) {
            return "E01";
        }
        const std::string& xml = target_xml();
        if (*offset >= xml.size()) {
            return "l";
        }
        const std::string part = xml.substr(*offset, std::min<std::size_t>(*length, 0x3F00));
        return (*offset + part.size() >= xml.size() ? "l" : "m") + part;
    }
    if (p.starts_with("qRcmd,")) {
        return monitor_command(p.substr(6));
    }
    if (p == "qC") {
        return "QC1";
    }
    if (p == "qfThreadInfo") {
        return "m1";
    }
    if (p == "qsThreadInfo") {
        return "l";
    }
    if (p.starts_with("qAttached")) {
        return "1";
    }
    if (p == "qOffsets") {
        return "Text=0;Data=0;Bss=0";
    }
    if (p.starts_with("qSymbol")) {
        return "OK";
    }
    return {};
}

std::string GdbServer::registers() const {
    std::string out;
    for (unsigned n = 0; n < kRegisterCount; ++n) {
        out += read_register(n);
    }
    return out;
}

std::string GdbServer::read_register(unsigned number) const {
    const Z80Registers r = dbg_.registers();
    const std::uint16_t values[kRegisterCount] = {r.af,  r.bc, r.de, r.hl, r.af2, r.bc2, r.de2, r.hl2, r.ix,
                                                  r.iy,  r.sp, r.pc, static_cast<std::uint16_t>(r.i << 8 | r.r)};
    return number < kRegisterCount ? word_le(values[number]) : "E00";
}

bool GdbServer::write_register(unsigned number, std::uint16_t value) {
    Z80Registers r = dbg_.registers();
    std::uint16_t* const words[] = {&r.af, &r.bc, &r.de, &r.hl, &r.af2, &r.bc2, &r.de2,
                                    &r.hl2, &r.ix, &r.iy, &r.sp, &r.pc};
    if (number < 12) {
        *words[number] = value;
    } else if (number == 12) {
        r.i = static_cast<std::uint8_t>(value >> 8);
        r.r = static_cast<std::uint8_t>(value);
    } else {
        return false;
    }
    return dbg_.set_registers(r);  // only between instructions, as after any stop
}

std::uint8_t GdbServer::read_byte(std::uint32_t address) const {
    if (address < 0x10000) {
        return machine_.peek(static_cast<std::uint16_t>(address));
    }
    const std::uint32_t bank = (address >> 16) - 1;
    if (bank >= kBankCount) {
        return MemoryMap::kOpenBus;
    }
    const std::span<const std::uint8_t> bytes = machine_.memory().bank_bytes(static_cast<Bank>(bank));
    const std::uint32_t offset = address & 0xFFFF;
    return offset < bytes.size() ? bytes[offset] : MemoryMap::kOpenBus;
}

void GdbServer::write_byte(std::uint32_t address, std::uint8_t value) {
    if (address < 0x10000) {
        machine_.poke(static_cast<std::uint16_t>(address), value);
        return;
    }
    const std::uint32_t bank = (address >> 16) - 1;
    if (bank < kBankCount) {
        const std::span<std::uint8_t> bytes = machine_.bank_bytes(static_cast<Bank>(bank));
        const std::uint32_t offset = address & 0xFFFF;
        if (offset < bytes.size()) {
            bytes[offset] = value;
        }
    }
}

std::string GdbServer::read_memory(std::string_view args) const {
    const auto address = take_hex(args);
    const bool comma = skip(args, ',');
    const auto length = take_hex(args);
    if (!address || !comma || !length) {
        return "E01";
    }
    std::string out;
    const std::uint32_t count = std::min<std::uint32_t>(*length, kMaxRead);
    for (std::uint32_t i = 0; i < count; ++i) {
        put_hex2(out, read_byte(*address + i));
    }
    return out;
}

std::string GdbServer::write_memory(std::string_view args) {
    const auto address = take_hex(args);
    const bool comma = skip(args, ',');
    const auto length = take_hex(args);
    if (!address || !comma || !length || !skip(args, ':') || args.size() < *length * 2u) {
        return "E01";
    }
    for (std::uint32_t i = 0; i < *length; ++i) {
        const int high = hex_digit(args[i * 2]);
        const int low = hex_digit(args[i * 2 + 1]);
        if (high < 0 || low < 0) {
            return "E01";
        }
        write_byte(*address + i, static_cast<std::uint8_t>(high << 4 | low));
    }
    return "OK";
}

std::string GdbServer::insert_point(std::string_view args) {
    if (args.empty() || args.front() < '0' || args.front() > '4') {
        return {};
    }
    Point point;
    point.type = args.front();
    args.remove_prefix(1);
    const bool comma = skip(args, ',');
    const auto address = take_hex(args);
    const bool comma2 = skip(args, ',');
    const auto kind = take_hex(args);  // breakpoints: a kind GDB chooses; watchpoints: the length
    if (!comma || !address || !comma2 || !kind || *address >= (kBankCount + 1) << 16) {
        return "E01";
    }
    point.address = *address;
    point.length = point.type >= '2' ? std::max<std::uint32_t>(*kind, 1) : 1;
    if (points_.count(point) > 0) {
        return "OK";
    }
    debug::Checkpoint c;
    static constexpr std::uint8_t kAccess[] = {kAccessExec, kAccessExec, kAccessWrite, kAccessRead,
                                               kAccessRead | kAccessWrite};
    c.access = kAccess[point.type - '0'];
    const std::uint32_t low = point.address & 0xFFFF;
    c.start = static_cast<std::uint16_t>(low);
    c.end = static_cast<std::uint16_t>(std::min<std::uint32_t>(0xFFFF, low + point.length - 1));
    if (point.address >= 0x10000) {
        c.bank = static_cast<Bank>((point.address >> 16) - 1);
        c.offsets = true;  // an offset in the bank, wherever it is paged
    }
    points_[point] = dbg_.add(std::move(c));
    return "OK";
}

std::string GdbServer::remove_point(std::string_view args) {
    if (args.empty() || args.front() < '0' || args.front() > '4') {
        return {};
    }
    Point point;
    point.type = args.front();
    args.remove_prefix(1);
    const bool comma = skip(args, ',');
    const auto address = take_hex(args);
    const bool comma2 = skip(args, ',');
    const auto kind = take_hex(args);
    if (!comma || !address || !comma2 || !kind) {
        return "E01";
    }
    point.address = *address;
    point.length = point.type >= '2' ? std::max<std::uint32_t>(*kind, 1) : 1;
    const auto it = points_.find(point);
    if (it != points_.end()) {
        dbg_.remove(it->second);
        points_.erase(it);
    }
    return "OK";
}

std::string GdbServer::monitor_command(std::string_view hex) {
    std::string line;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int high = hex_digit(hex[i]);
        const int low = hex_digit(hex[i + 1]);
        if (high < 0 || low < 0) {
            return "E01";
        }
        line.push_back(static_cast<char>(high << 4 | low));
    }
    std::string text = monitor_.execute(line);
    if (!monitor_.active()) {
        text += monitor_.enter("GDB holds the machine");  // run commands would leave GDB's stopped state behind
        if (!text.empty() && text.back() != '\n') {
            text += "\n";
        }
    }
    if (text.empty()) {
        return "OK";
    }
    std::string out;
    for (const char c : text) {
        put_hex2(out, static_cast<unsigned char>(c));
    }
    return out;
}

void GdbServer::resume(std::string_view address) {
    if (!address.empty()) {
        if (const auto pc = take_hex(address)) {
            write_register(11, static_cast<std::uint16_t>(*pc));
        }
    }
    running_ = true;
    stops_at_continue_ = monitor_.stop_count();
    if (monitor_.active()) {
        monitor_.execute("x");
    }
}

std::string GdbServer::poll() {
    if (!running_ || !monitor_.active()) {
        return {};
    }
    return reply(stop_reply());
}

std::string GdbServer::stop_reply() {
    running_ = false;
    const std::optional<debug::Stop>& stop = monitor_.last_stop();
    if (monitor_.stop_count() == stops_at_continue_ || !stop) {
        return stop_reply(kSigInt, {});  // Ctrl-C, or another front end held the machine
    }
    return stop_reply(kSigTrap, stop_reason(*stop));
}

std::string GdbServer::stop_reason(const debug::Stop& stop) const {
    if (stop.why != debug::Why::Checkpoint) {
        return {};
    }
    for (const debug::Hit& hit : stop.hits) {
        for (const auto& [point, id] : points_) {
            if (id != hit.checkpoint || point.type < '2') {
                continue;
            }
            static constexpr const char* kKinds[] = {"watch", "rwatch", "awatch"};
            // A bank watchpoint reports the address in the same form GDB set it
            const std::uint32_t address = point.address >= 0x10000
                                              ? (static_cast<std::uint32_t>(hit.where.bank) + 1) << 16 | hit.where.offset
                                              : hit.address;
            return std::string(kKinds[point.type - '2']) + ":" + hex_number(address);
        }
    }
    return swbreak_ ? "swbreak:" : "";
}

std::string GdbServer::stop_reply(std::uint8_t signal, std::string_view reason) const {
    const Z80Registers r = dbg_.registers();
    std::string out = "T";
    put_hex2(out, signal);
    if (!reason.empty()) {
        out += std::string(reason) + ";";  // first: DeZog reads the reason up to the first ':'
    }
    out += "0a:" + word_le(r.sp) + ";0b:" + word_le(r.pc) + ";";
    return out;
}

}  // namespace freetvc::remote
