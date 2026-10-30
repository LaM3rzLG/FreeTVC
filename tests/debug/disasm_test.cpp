// SPDX-License-Identifier: GPL-3.0-or-later
// Disassembler: spot checks, then every opcode family assembled back with sjasmplus and compared byte for byte.
// usage: disasm_test [<sjasmplus> <work dir>]; without arguments only the spot checks run; exit 77 if sjasmplus is missing.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "debug/disassembler.hpp"

namespace {

using freetvc::debug::disassemble;
using freetvc::debug::Disassembly;
using freetvc::debug::DisasmOptions;

constexpr int kExitSkip = 77;
int g_failures = 0;

Disassembly dis(std::initializer_list<std::uint8_t> bytes, std::uint16_t address = 0x8000, const DisasmOptions& options = {}) {
    const std::vector<std::uint8_t> v(bytes);
    return disassemble(v, address, options);
}

void expect_text(int line, const Disassembly& d, const std::string& text, unsigned length) {
    if (d.text != text || d.length != length) {
        ++g_failures;
        std::printf("FAIL line %d: '%s' (%u bytes), expected '%s' (%u bytes)\n", line, d.text.c_str(), d.length, text.c_str(),
                    length);
    }
}

#define TEXT(bytes_, text_, length_) expect_text(__LINE__, dis bytes_, text_, length_)
#define CHECK(cond)                                    \
    do {                                               \
        if (!(cond)) {                                 \
            ++g_failures;                              \
            std::printf("FAIL line %d: %s\n", __LINE__, #cond); \
        }                                              \
    } while (false)

void spot_checks() {
    TEXT(({0x00}), "NOP", 1);
    TEXT(({0x01, 0x34, 0x12}), "LD BC,1234h", 3);
    TEXT(({0xC3, 0x00, 0xC0}), "JP 0C000h", 3);
    TEXT(({0x18, 0xFE}), "JR 8000h", 2);
    TEXT(({0x10, 0x10}), "DJNZ 8012h", 2);
    TEXT(({0x08}), "EX AF,AF'", 1);
    TEXT(({0x32, 0x00, 0x90}), "LD (9000h),A", 3);
    TEXT(({0x2A, 0x00, 0x90}), "LD HL,(9000h)", 3);
    TEXT(({0xDD, 0x36, 0x05, 0x7F}), "LD (IX+05h),7Fh", 4);
    TEXT(({0xFD, 0x7E, 0xFD}), "LD A,(IY-03h)", 3);
    TEXT(({0xDD, 0x66, 0x01}), "LD H,(IX+01h)", 3);
    TEXT(({0xDD, 0x26, 0x12}), "LD IXH,12h", 3);
    TEXT(({0xFD, 0xE9}), "JP (IY)", 2);
    TEXT(({0xDD, 0x29}), "ADD IX,IX", 2);
    TEXT(({0xDD, 0xEB}), "DB 0DDh : EX DE,HL", 2);
    TEXT(({0xDD, 0x00}), "DB 0DDh : NOP", 2);
    TEXT(({0xDD, 0xDD, 0x21, 0x34, 0x12}), "DB 0DDh : LD IX,1234h", 5);
    TEXT(({0xFD, 0xED, 0xB0}), "DB 0FDh : LDIR", 3);
    TEXT(({0xDD, 0xCB, 0x05, 0x06}), "RLC (IX+05h)", 4);
    TEXT(({0xDD, 0xCB, 0x05, 0x00}), "RLC (IX+05h),B", 4);
    TEXT(({0xFD, 0xCB, 0xFF, 0xFE}), "SET 7,(IY-01h)", 4);
    TEXT(({0xCB, 0x37}), "SLL A", 2);
    TEXT(({0xED, 0x70}), "IN F,(C)", 2);
    TEXT(({0xED, 0x71}), "OUT (C),0", 2);
    TEXT(({0xED, 0x77}), "DB 0EDh,77h", 2);
    TEXT(({0xED, 0x43, 0x00, 0x90}), "LD (9000h),BC", 4);
    TEXT(({0xED, 0xB0}), "LDIR", 2);
    TEXT(({0xED, 0x5E}), "IM 2", 2);
    TEXT(({0xD3, 0x02}), "OUT (02h),A", 2);
    TEXT(({0xE6, 0xF0}), "AND 0F0h", 2);
    TEXT(({0xFF}), "RST 38h", 1);

    const Disassembly neg = dis({0xED, 0x4C});
    CHECK(neg.text == "NEG" && neg.alias && neg.undocumented);
    CHECK(!dis({0xED, 0x44}).alias);
    CHECK(dis({0xDD, 0x26, 0x12}).undocumented);
    CHECK(!dis({0xDD, 0x21, 0x00, 0x00}).undocumented);

    // TVC inline data (rom.md §2, §4)
    const Disassembly call = dis({0xF7, 0x21, 0x00});
    expect_text(__LINE__, call, "RST 30h : DB 21h", 2);
    CHECK(call.comment == "ED_CHOUT (class 2, function 1)");
    CHECK(dis({0xF7, 0x91}).comment == "KBD_CHIN (class 1, function 1)");
    CHECK(dis({0xF7, 0x16}).comment == "class 1, function 6");
    const Disassembly error = dis({0xCF, 0x07});
    expect_text(__LINE__, error, "RST 08h : DB 07h", 2);
    CHECK(error.comment == "BASIC error 07h");
    DisasmOptions bare;
    bare.tvc_inline = false;
    expect_text(__LINE__, dis({0xF7, 0x21}, 0x8000, bare), "RST 30h", 1);
    // RST 18h: BASIC ops to the first with bit 7 set, op 05h with an operand byte, also as the last op (rom.md §6)
    const Disassembly ops = dis({0xDF, 0x05, 0x03, 0x8A});
    expect_text(__LINE__, ops, "RST 18h : DB 05h,03h,8Ah", 4);
    CHECK(ops.comment == "BASIC threaded code, 3 bytes");
    expect_text(__LINE__, dis({0xDF, 0x85, 0x90, 0x00}), "RST 18h : DB 85h,90h", 3);
    expect_text(__LINE__, dis({0xDF, 0x01, 0x02}), "RST 18h", 1);  // no end op in the bytes given
    expect_text(__LINE__, dis({0xDF, 0x05, 0x03, 0x8A}, 0x8000, bare), "RST 18h", 1);
    // A CALL to the inline print: a length byte and the characters, quotes and control codes as numbers
    DisasmOptions print;
    print.string_calls = {0xFB78};
    const Disassembly text = dis({0xCD, 0x78, 0xFB, 0x05, 'H', 'e', '"', 'l', 0x0D}, 0x8000, print);
    expect_text(__LINE__, text, "CALL 0FB78h : DB 05h,\"He\",22h,\"l\",0Dh", 9);
    CHECK(text.comment == "prints \"He\"l.\"");
    expect_text(__LINE__, dis({0xCD, 0x78, 0xFB, 0x00}, 0x8000, print), "CALL 0FB78h : DB 00h", 4);
    expect_text(__LINE__, dis({0xCD, 0x78, 0xFB, 0x09, 'A'}, 0x8000, print), "CALL 0FB78h", 3);  // the string is cut off
    expect_text(__LINE__, dis({0xCD, 0x78, 0xFB, 0x01, 'A'}), "CALL 0FB78h", 3);  // not a string call

    // Labels for jump targets and memory operands, not for immediates
    DisasmOptions labelled;
    labelled.label = [](std::uint16_t a) { return a == 0xC000 ? std::string("target") : std::string(); };
    expect_text(__LINE__, dis({0xCD, 0x00, 0xC0}, 0x8000, labelled), "CALL target", 3);
    expect_text(__LINE__, dis({0x3A, 0x00, 0xC0}, 0x8000, labelled), "LD A,(target)", 3);
    expect_text(__LINE__, dis({0x21, 0x00, 0xC0}, 0x8000, labelled), "LD HL,0C000h", 3);
    expect_text(__LINE__, dis({0x18, 0x00}, 0xBFFE, labelled), "JR target", 2);
}

// Every opcode family with sample operand bytes; each instruction sits in its own 8-byte slot
int round_trip(const std::string& sjasmplus, const std::filesystem::path& dir) {
    if (!std::filesystem::exists(sjasmplus)) {
        std::printf("SKIP: no sjasmplus at %s\n", sjasmplus.c_str());
        return kExitSkip;
    }
    std::vector<std::vector<std::uint8_t>> cases;
    for (unsigned op = 0; op < 256; ++op) {
        const auto b = static_cast<std::uint8_t>(op);
        cases.push_back({b, 0x12, 0x34, 0x56});
        cases.push_back({0xCB, b, 0x12, 0x34});
        cases.push_back({0xED, b, 0x12, 0x34});
        cases.push_back({0xDD, b, 0x12, 0x34});
        cases.push_back({0xFD, b, 0xF0, 0x34});
        cases.push_back({0xDD, 0xCB, 0x05, b});
        cases.push_back({0xFD, 0xCB, 0xFB, b});
    }
    // TVC inline data: RST 18h ops and an inline-print string, each within a slot
    cases.push_back({0xDF, 0x05, 0x03, 0x8A});
    cases.push_back({0xDF, 0x85, 0x90, 0x00});
    cases.push_back({0xDF, 0x01, 0x2B, 0x05, 0xFF, 0x81, 0x00});
    cases.push_back({0xCD, 0x78, 0xFB, 0x04, 'O', 'K', '"', 0x0D});
    cases.push_back({0xCD, 0x78, 0xFB, 0x02, '\\', 0x7F});
    DisasmOptions options;
    options.string_calls = {0xFB78};
    constexpr std::uint16_t kBase = 0x4000;
    constexpr unsigned kSlot = 8;
    std::filesystem::create_directories(dir);
    const std::filesystem::path asm_path = dir / "roundtrip.asm";
    const std::filesystem::path bin_path = dir / "roundtrip.bin";
    std::vector<Disassembly> texts;
    {
        std::ofstream out(asm_path);
        out << " DEVICE NONE\n ORG " << kBase << "\n";
        for (std::size_t i = 0; i < cases.size(); ++i) {
            const auto address = static_cast<std::uint16_t>(kBase + i * kSlot);
            texts.push_back(disassemble(cases[i], address, options));
            out << "s" << i << ": " << texts.back().text << "\n DS s" << i << " + " << kSlot << " - $, 0\n";
        }
    }
    const std::string command = "\"" + sjasmplus + "\" --nologo --msg=err --raw=\"" + bin_path.string() + "\" \"" +
                                asm_path.string() + "\"";
    if (std::system(command.c_str()) != 0) {
        std::printf("FAIL: sjasmplus rejected %s\n", asm_path.string().c_str());
        return 1;
    }
    std::ifstream in(bin_path, std::ios::binary);
    const std::vector<std::uint8_t> bin((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bin.size() != cases.size() * kSlot) {
        std::printf("FAIL: %zu bytes assembled, expected %zu\n", bin.size(), cases.size() * kSlot);
        return 1;
    }
    unsigned exact = 0;
    unsigned aliases = 0;
    for (std::size_t i = 0; i < cases.size(); ++i) {
        const Disassembly& d = texts[i];
        const auto slot = std::span(bin).subspan(i * kSlot, kSlot);
        const auto address = static_cast<std::uint16_t>(kBase + i * kSlot);
        bool ok;
        if (d.alias) {
            // the usual encoding must read back as the same text
            ok = disassemble(slot, address, options).text == d.text;
            ++aliases;
        } else {
            ok = std::equal(cases[i].begin(), cases[i].begin() + d.length, slot.begin()) &&
                 std::all_of(slot.begin() + d.length, slot.end(), [](std::uint8_t b) { return b == 0; });
            ++exact;
        }
        if (!ok && g_failures++ < 20) {
            std::printf("FAIL %02X %02X %02X %02X '%s' (%u bytes) -> %02X %02X %02X %02X %02X\n", cases[i][0], cases[i][1],
                        cases[i][2], cases[i][3], d.text.c_str(), d.length, slot[0], slot[1], slot[2], slot[3], slot[4]);
        }
    }
    std::printf("round trip: %u instructions byte-exact, %u aliases re-read, through %s\n", exact, aliases, sjasmplus.c_str());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    spot_checks();
    if (argc == 3) {
        const int rc = round_trip(argv[1], argv[2]);
        if (rc == kExitSkip) {
            return g_failures == 0 ? kExitSkip : 1;
        }
    }
    if (g_failures == 0) {
        std::printf("disasm_test: all checks passed\n");
        return 0;
    }
    std::printf("disasm_test: %d check(s) failed\n", g_failures);
    return 1;
}
