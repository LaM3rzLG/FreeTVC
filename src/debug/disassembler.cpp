// SPDX-License-Identifier: GPL-3.0-or-later
#include "debug/disassembler.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <utility>

#include "core/machine/machine.hpp"

namespace freetvc::debug {

namespace {

constexpr std::array<const char*, 8> kR{"B", "C", "D", "E", "H", "L", "(HL)", "A"};
constexpr std::array<const char*, 4> kRp{"BC", "DE", "HL", "SP"};
constexpr std::array<const char*, 4> kRp2{"BC", "DE", "HL", "AF"};
constexpr std::array<const char*, 8> kCc{"NZ", "Z", "NC", "C", "PO", "PE", "P", "M"};
constexpr std::array<const char*, 8> kAlu{"ADD A,", "ADC A,", "SUB ", "SBC A,", "AND ", "XOR ", "OR ", "CP "};
constexpr std::array<const char*, 8> kRot{"RLC", "RRC", "RL", "RR", "SLA", "SRA", "SLL", "SRL"};
constexpr std::array<const char*, 8> kAcc{"RLCA", "RRCA", "RLA", "RRA", "DAA", "CPL", "SCF", "CCF"};
constexpr std::array<const char*, 3> kIndex{"HL", "IX", "IY"};
constexpr std::array<std::array<const char*, 4>, 4> kBlock{{
    {"LDI", "CPI", "INI", "OUTI"},
    {"LDD", "CPD", "IND", "OUTD"},
    {"LDIR", "CPIR", "INIR", "OTIR"},
    {"LDDR", "CPDR", "INDR", "OTDR"},
}};
constexpr std::uint8_t kRst08 = 0xCF;
constexpr std::uint8_t kRst18 = 0xDF;
constexpr std::size_t kMaxThreaded = 32;  // RST 18h ops without an end op this soon are not decoded
constexpr std::uint8_t kRst30 = 0xF7;

struct SystemCall {
    std::uint8_t code;
    const char* name;
};

// [OS] names by function code (rom.md §5); one handler serves both directions where both codes are listed
constexpr std::array<SystemCall, 52> kSystemCalls{{
    {0x01, "VID_CHOUT"}, {0x02, "VID_BKOUT"}, {0x03, "BTEXT"}, {0x04, "VMODE"}, {0x05, "CLS"}, {0x06, "BABS"},
    {0x07, "BREL"}, {0x08, "BON"}, {0x09, "BOFF"}, {0x0A, "FILL"}, {0x0B, "DEFC"}, {0x0C, "PAL"},
    {0x91, "KBD_CHIN"}, {0x93, "KBD_STATUS"},
    {0xA1, "ED_CHIN"}, {0x21, "ED_CHOUT"}, {0xA2, "ED_BKIN"}, {0x22, "ED_BKOUT"}, {0x23, "CPOS"}, {0xA3, "CPOS"},
    {0x24, "CFIX"}, {0xA4, "CFIX"}, {0x33, "TONE_SET"}, {0x41, "PAR_CHOUT"}, {0x42, "PAR_BKOUT"},
    {0xD1, "CAS_CHIN"}, {0x51, "CAS_CHOUT"}, {0xD2, "CAS_BKIN"}, {0x52, "CAS_BKOUT"}, {0xD3, "CAS_OPEN"},
    {0x53, "CAS_CRTE"}, {0xD4, "CAS_CLOSE"}, {0x54, "CAS_CLOSE"}, {0xD5, "CAS_VERIFY"},
    {0xE1, "SER_CHIN"}, {0x61, "SER_CHOUT"}, {0xE2, "SER_BKIN"}, {0x62, "SER_BKOUT"}, {0xE3, "RS232_SET"},
    {0x63, "RS232_SET"}, {0xE4, "RS232_CLOSE"}, {0x64, "RS232_CLOSE"},
    {0x71, "HI_MEM_SET"}, {0xF1, "HI_MEM_SET"}, {0x72, "EXP_DIRECT"}, {0xF2, "EXP_DIRECT"}, {0x73, "IO_DIRECT"},
    {0xF3, "IO_DIRECT"}, {0x74, "SLOT_NUM"}, {0xF4, "SLOT_NUM"}, {0x0D, "polygon"}, {0x0E, "ellipse"},
}};

struct Ctx {
    std::span<const std::uint8_t> bytes;
    std::uint16_t address = 0;
    unsigned length = 1;  // decoded length, inline data excluded
    int index = 0;        // 0 HL, 1 IX, 2 IY
    std::size_t p = 1;    // next operand byte
    bool used_index = false;
    bool disp_read = false;
    int disp = 0;
    const DisasmOptions* options = nullptr;
    Disassembly* out = nullptr;

    std::uint8_t at(std::size_t i) const { return i < bytes.size() ? bytes[i] : 0; }
    std::string n() { return hex8(at(p++)); }
    std::uint16_t word() {
        const auto v = static_cast<std::uint16_t>(at(p) | at(p + 1) << 8);
        p += 2;
        return v;
    }
    std::string target(std::uint16_t v) const {
        if (options->label) {
            std::string name = options->label(v);
            if (!name.empty()) {
                return name;
            }
        }
        return hex16(v);
    }
    std::string hl() {
        used_index |= index != 0;
        return kIndex[static_cast<std::size_t>(index)];
    }
    std::string mem() {
        if (index == 0) {
            return "(HL)";
        }
        used_index = true;
        if (!disp_read) {
            disp = static_cast<std::int8_t>(at(p++));
            disp_read = true;
        }
        return std::string("(") + kIndex[static_cast<std::size_t>(index)] + (disp < 0 ? "-" : "+") + hex8(static_cast<unsigned>(std::abs(disp))) + ")";
    }
    // plain_hl: the instruction also has an (IX+d) operand, so H and L stay H and L
    std::string reg(unsigned r, bool plain_hl) {
        if (r == 6) {
            return mem();
        }
        if ((r == 4 || r == 5) && index != 0 && !plain_hl) {
            used_index = true;
            return std::string(kIndex[static_cast<std::size_t>(index)]) + (r == 4 ? "H" : "L");
        }
        return kR[r];
    }
    std::string rp(unsigned i) { return i == 2 ? hl() : kRp[i]; }
    std::string rp2(unsigned i) { return i == 2 ? hl() : kRp2[i]; }
    std::string rel() {
        const auto d = static_cast<std::int8_t>(at(p++));
        return target(static_cast<std::uint16_t>(address + length + d));
    }
    std::string addr() { return "(" + target(word()) + ")"; }
    void undocumented(const char* note, bool alias) {
        out->undocumented = true;
        out->alias |= alias;
        if (note && out->comment.empty()) {
            out->comment = note;
        }
    }
    // RST 30h / RST 08h data byte
    std::string inline_data(std::uint8_t op) {
        if (!options->tvc_inline || (op != kRst30 && op != kRst08)) {
            return {};
        }
        const std::uint8_t data = at(p++);
        ++out->length;
        if (op == kRst30) {
            const char* name = system_call_name(data);
            char note[48];
            std::snprintf(note, sizeof note, "class %u, function %u", (data >> 4) & 7u, data & 15u);
            out->comment = name ? std::string(name) + " (" + note + ")" : std::string(note);
        } else {
            out->comment = "BASIC error " + hex8(data);
        }
        return " : DB " + hex8(data);
    }
    // RST 18h: BASIC ops to the first with bit 7 set; op 05h takes an operand byte (rom.md §6)
    std::string threaded() {
        std::size_t i = p;
        while (true) {
            if (i >= bytes.size() || i - p >= kMaxThreaded) {
                return {};
            }
            const std::uint8_t op = at(i++);
            if ((op & 0x7F) == 0x05) {
                ++i;
            }
            if (op & 0x80) {
                break;
            }
        }
        if (i > bytes.size()) {
            return {};
        }
        std::string data;
        for (std::size_t k = p; k < i; ++k) {
            data += (k == p ? "" : ",") + hex8(at(k));
        }
        out->length = static_cast<std::uint8_t>(out->length + (i - p));
        out->comment = "BASIC threaded code, " + std::to_string(i - p) + " bytes";
        p = i;
        return " : DB " + data;
    }
    // After a CALL to the ROM's inline print: a length byte and the characters (rom.md §6)
    std::string inline_string() {
        const std::size_t n = at(p);
        if (p + 1 + n > bytes.size()) {
            return {};
        }
        std::string data = hex8(static_cast<unsigned>(n));
        std::string shown;
        bool quoted = false;
        for (std::size_t k = p + 1; k < p + 1 + n; ++k) {
            const std::uint8_t c = at(k);
            const bool plain_char = c >= 0x20 && c < 0x7F && c != '"' && c != '\\';
            if (plain_char) {
                data += quoted ? std::string(1, static_cast<char>(c)) : ",\"" + std::string(1, static_cast<char>(c));
            } else {
                data += (quoted ? "\"," : ",") + hex8(c);
            }
            quoted = plain_char;
            shown += c >= 0x20 && c < 0x7F ? static_cast<char>(c) : '.';
        }
        if (quoted) {
            data += "\"";
        }
        out->length = static_cast<std::uint8_t>(out->length + 1 + n);
        out->comment = "prints \"" + shown + "\"";
        p += 1 + n;
        return " : DB " + data;
    }
};

std::string plain(std::uint8_t op, Ctx& c) {
    const unsigned x = op >> 6;
    const unsigned y = (op >> 3) & 7;
    const unsigned z = op & 7;
    const unsigned p = y >> 1;
    const unsigned q = y & 1;
    switch (x) {
    case 0:
        switch (z) {
        case 0:
            if (y == 0) {
                return "NOP";
            }
            if (y == 1) {
                return "EX AF,AF'";
            }
            if (y == 2) {
                return "DJNZ " + c.rel();
            }
            if (y == 3) {
                return "JR " + c.rel();
            }
            return std::string("JR ") + kCc[y - 4] + "," + c.rel();
        case 1:
            if (q == 0) {
                std::string dst = c.rp(p);
                return "LD " + dst + "," + hex16(c.word());
            } else {
                std::string dst = c.hl();
                return "ADD " + dst + "," + c.rp(p);
            }
        case 2:
            switch (y) {
            case 0: return "LD (BC),A";
            case 1: return "LD A,(BC)";
            case 2: return "LD (DE),A";
            case 3: return "LD A,(DE)";
            case 4: {
                std::string a = c.addr();
                return "LD " + a + "," + c.hl();
            }
            case 5: {
                std::string dst = c.hl();
                return "LD " + dst + "," + c.addr();
            }
            case 6: return "LD " + c.addr() + ",A";
            default: return "LD A," + c.addr();
            }
        case 3:
            return (q == 0 ? "INC " : "DEC ") + c.rp(p);
        case 4:
            return "INC " + c.reg(y, false);
        case 5:
            return "DEC " + c.reg(y, false);
        case 6: {
            std::string dst = c.reg(y, false);
            return "LD " + dst + "," + c.n();
        }
        default:
            return kAcc[y];
        }
    case 1: {
        if (y == 6 && z == 6) {
            return "HALT";
        }
        const bool memory = y == 6 || z == 6;
        std::string dst = c.reg(y, memory);
        return "LD " + dst + "," + c.reg(z, memory);
    }
    case 2:
        return kAlu[y] + c.reg(z, false);
    default:
        switch (z) {
        case 0:
            return std::string("RET ") + kCc[y];
        case 1:
            if (q == 0) {
                return "POP " + c.rp2(p);
            }
            switch (p) {
            case 0: return "RET";
            case 1: return "EXX";
            case 2: return "JP (" + c.hl() + ")";
            default: return "LD SP," + c.hl();
            }
        case 2:
            return std::string("JP ") + kCc[y] + "," + c.target(c.word());
        case 3:
            switch (y) {
            case 0: return "JP " + c.target(c.word());
            case 2: return "OUT (" + c.n() + "),A";
            case 3: return "IN A,(" + c.n() + ")";
            case 4: return "EX (SP)," + c.hl();
            case 5: return "EX DE,HL";
            case 6: return "DI";
            default: return "EI";  // y == 1 (CB) never reaches here
            }
        case 4:
            return std::string("CALL ") + kCc[y] + "," + c.target(c.word());
        case 5:
            if (q == 0) {
                return "PUSH " + c.rp2(p);
            }
            {
                const std::uint16_t to = c.word();  // p == 0; the prefixes never reach here
                const auto& calls = c.options->string_calls;
                const bool string = c.options->tvc_inline && std::find(calls.begin(), calls.end(), to) != calls.end();
                return "CALL " + c.target(to) + (string ? c.inline_string() : std::string());
            }
        case 6:
            return kAlu[y] + c.n();
        default: {
            std::string text = "RST " + hex8(y * 8);
            if (op == kRst18 && c.options->tvc_inline) {
                return text + c.threaded();
            }
            return text + c.inline_data(op);
        }
        }
    }
}

std::string bits(std::uint8_t op, const std::string& operand, Ctx& c) {
    const unsigned x = op >> 6;
    const unsigned y = (op >> 3) & 7;
    if (x == 0) {
        if (y == 6) {
            c.undocumented("undocumented SLL", false);
        }
        return std::string(kRot[y]) + " " + operand;
    }
    return std::string(x == 1 ? "BIT " : x == 2 ? "RES " : "SET ") + std::to_string(y) + "," + operand;
}

std::string extended(std::uint8_t op, Ctx& c) {
    const unsigned x = op >> 6;
    const unsigned y = (op >> 3) & 7;
    const unsigned z = op & 7;
    const unsigned p = y >> 1;
    const unsigned q = y & 1;
    if (x == 1) {
        switch (z) {
        case 0:
            if (y == 6) {
                c.undocumented("undocumented", false);
                return "IN F,(C)";
            }
            return std::string("IN ") + kR[y] + ",(C)";
        case 1:
            if (y == 6) {
                c.undocumented("undocumented; NMOS Z80 outputs 0", false);
                return "OUT (C),0";
            }
            return std::string("OUT (C),") + kR[y];
        case 2:
            return std::string(q == 0 ? "SBC HL," : "ADC HL,") + kRp[p];
        case 3:
            if (p == 2) {
                c.undocumented("ED form of the HL load", true);
            }
            if (q == 0) {
                std::string a = c.addr();
                return "LD " + a + "," + kRp[p];
            }
            return std::string("LD ") + kRp[p] + "," + c.addr();
        case 4:
            if (y != 0) {
                c.undocumented("undocumented copy of ED 44h", true);
            }
            return "NEG";
        case 5:
            if (y > 1) {
                c.undocumented("undocumented copy of ED 45h", true);
            }
            return y == 1 ? "RETI" : "RETN";
        case 6: {
            static constexpr std::array<const char*, 4> kIm{"IM 0", "IM 0", "IM 1", "IM 2"};
            if (y != 0 && y != 2 && y != 3) {
                c.undocumented((y & 3) == 1 ? "undocumented IM 0/1" : "undocumented copy", true);
            }
            return kIm[y & 3];
        }
        default:
            switch (y) {
            case 0: return "LD I,A";
            case 1: return "LD R,A";
            case 2: return "LD A,I";
            case 3: return "LD A,R";
            case 4: return "RRD";
            case 5: return "RLD";
            default: break;
            }
            break;
        }
    } else if (x == 2 && z <= 3 && y >= 4) {
        return kBlock[y - 4][z];
    }
    c.undocumented("undefined: an 8 T no-op", false);
    return "DB 0EDh," + hex8(op);
}

void render(Ctx& c, Disassembly& d) {
    const std::uint8_t op = c.at(0);
    if (op == 0xCB) {
        const std::uint8_t cb = c.at(1);
        c.p = 2;
        d.text = bits(cb, c.reg(cb & 7, false), c);
        return;
    }
    if (op == 0xED) {
        c.p = 2;
        d.text = extended(c.at(1), c);
        return;
    }
    if (op != 0xDD && op != 0xFD) {
        c.p = 1;
        d.text = plain(op, c);
        return;
    }
    const std::uint8_t next = c.at(1);
    const std::string prefix = op == 0xDD ? "DB 0DDh : " : "DB 0FDh : ";
    if (next == 0xDD || next == 0xFD || next == 0xED) {
        // z80.h runs the chain as one instruction; only the last prefix counts
        Ctx rest = c;
        rest.bytes = c.bytes.subspan(std::min<std::size_t>(1, c.bytes.size()));
        rest.address = static_cast<std::uint16_t>(c.address + 1);
        rest.length = c.length - 1;
        render(rest, d);
        d.text = prefix + d.text;
        c.undocumented("prefix ignored", false);
        return;
    }
    c.index = op == 0xDD ? 1 : 2;
    if (next == 0xCB) {
        c.p = 2;
        const std::string operand = c.mem();
        const std::uint8_t cb = c.at(3);
        const unsigned z = cb & 7;
        d.text = bits(cb, operand, c);
        if (z != 6) {
            if ((cb >> 6) == 1) {
                c.undocumented("undocumented copy of BIT n,(IX+d)", true);
            } else {
                c.undocumented("undocumented: the result is also stored in the register", false);
                d.text += std::string(",") + kR[z];
            }
        }
        return;
    }
    c.p = 2;
    d.text = plain(next, c);
    if (!c.used_index) {
        d.text = prefix + d.text;
        c.undocumented("prefix has no effect", false);
    } else if (d.text.find("IXH") != std::string::npos || d.text.find("IXL") != std::string::npos ||
               d.text.find("IYH") != std::string::npos || d.text.find("IYL") != std::string::npos) {
        c.undocumented("undocumented index register half", false);
    }
}

}  // namespace

std::string hex8(unsigned value) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%02Xh", value & 0xFF);
    return (buf[0] >= 'A' ? "0" : "") + std::string(buf);
}

std::string hex16(unsigned value) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%04Xh", value & 0xFFFF);
    return (buf[0] >= 'A' ? "0" : "") + std::string(buf);
}

const char* system_call_name(std::uint8_t code) {
    for (const SystemCall& s : kSystemCalls) {
        if (s.code == code) {
            return s.name;
        }
    }
    return nullptr;
}

Disassembly disassemble(std::span<const std::uint8_t> bytes, std::uint16_t address, const DisasmOptions& options) {
    Disassembly d;
    d.decoded = decode(bytes);
    d.length = d.decoded.length;
    Ctx c;
    c.bytes = bytes;
    c.address = address;
    c.length = d.decoded.length;
    c.options = &options;
    c.out = &d;
    render(c, d);
    return d;
}

Disassembly disassemble(const Machine& machine, std::uint16_t address, const DisasmOptions& options) {
    // A string or threaded code after the instruction needs up to a length byte and 255 characters
    std::array<std::uint8_t, 260> bytes{};
    const std::uint8_t op = machine.peek(address);
    const std::size_t count = options.tvc_inline && (op == 0xCD || op == kRst18) ? bytes.size() : 8;
    for (std::size_t i = 0; i < count; ++i) {
        bytes[i] = machine.peek(static_cast<std::uint16_t>(address + i));
    }
    return disassemble(std::span<const std::uint8_t>(bytes.data(), count), address, options);
}

DisasmOptions tvc_disasm_options(const SymbolTable& table, const Machine& machine) {
    DisasmOptions options;
    options.label = symbol_labels(table, machine);
    for (const Symbol& s : table.symbols()) {
        if (s.name == "print_inline") {
            options.string_calls.push_back(s.address);
        }
    }
    return options;
}

std::function<std::string(std::uint16_t)> symbol_labels(const SymbolTable& table, const Machine& machine) {
    return [&table, &machine](std::uint16_t address) -> std::string {
        const Symbol* s = table.at(address, machine);
        return s ? s->name : std::string{};
    };
}

}  // namespace freetvc::debug
