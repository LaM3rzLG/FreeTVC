// SPDX-License-Identifier: GPL-3.0-or-later
// Expressions and symbol tables: numbers in each radix, C precedence, registers, memory, banks, hits, symbol files.
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "core/machine/machine.hpp"
#include "debug/debugger.hpp"
#include "debug/expression.hpp"
#include "debug/symbols.hpp"

namespace {

using freetvc::Bank;
using freetvc::Machine;
using namespace freetvc::debug;

int g_failures = 0;

void fail(int line, const std::string& what) {
    ++g_failures;
    std::printf("FAIL line %d: %s\n", line, what.c_str());
}

#define CHECK(cond)                 \
    do {                            \
        if (!(cond)) {              \
            fail(__LINE__, #cond);  \
        }                           \
    } while (false)

std::unique_ptr<Machine> machine() {
    freetvc::MachineConfig config;
    config.audio_rate = 0;
    auto m = std::make_unique<Machine>(config);
    m->io_write(0x02, 0xB0);
    return m;
}

// Value of `text`, or a failure line with the parse error
std::int64_t value(const Machine& m, std::string_view text, int line, const ParseOptions& options = {},
                   const Hit* hit = nullptr) {
    auto [e, error] = Expression::parse(text, options);
    if (!e) {
        fail(line, "parse '" + std::string(text) + "': " + error.message + " at " + std::to_string(error.position));
        return -1;
    }
    return e->evaluate({m, hit});
}

#define VALUE(text, expected)                                                                                        \
    do {                                                                                                             \
        const std::int64_t got_ = value(*m, text, __LINE__);                                                         \
        if (got_ != static_cast<std::int64_t>(expected)) {                                                           \
            fail(__LINE__, std::string(text) + " = " + std::to_string(got_) + ", expected " + std::to_string(expected)); \
        }                                                                                                            \
    } while (false)

Expression::Error error_of(std::string_view text, const ParseOptions& options = {}) {
    return Expression::parse(text, options).second;
}

void numbers_and_operators() {
    auto m = machine();
    VALUE("10", 16);  // Hex by default
    VALUE("#10", 10);
    VALUE("$1F", 31);
    VALUE("0x1F", 31);
    VALUE("1Fh", 31);
    VALUE("0FFh", 255);
    VALUE("%101", 5);
    VALUE("2+3*4", 14);
    VALUE("(2+3)*4", 20);
    VALUE("1+2<<3", 24);
    VALUE("1 | 2 == 2", 1);
    VALUE("-1 & 0FF", 255);
    VALUE("!0", 1);
    VALUE("~0 & 0F", 15);
    VALUE("7 / 0", 0);
    VALUE("#17 % 5", 2);
    VALUE("1 < 2 && 2 < 1", 0);
    VALUE("1 <= 1 || 0", 1);
    VALUE("3 >= 4", 0);
    VALUE("5 = 5", 1);
    VALUE("5 != 5", 0);
    VALUE("6 ^ 3", 5);
    VALUE("80 >> 4", 8);

    ParseOptions decimal;
    decimal.radix = 10;
    CHECK(value(*m, "10", __LINE__, decimal) == 10);
    CHECK(value(*m, "1Fh", __LINE__, decimal) == 31);
    CHECK(!Expression::parse("1F", decimal).first);

    CHECK(error_of("1 + * 2").position == 4);
    CHECK(error_of("(1 + 2").message.find("')' expected") != std::string::npos);
    CHECK(error_of("FF").message.find("$FF") != std::string::npos);  // hex starting with a letter: a hint
    CHECK(error_of("nosuchname").message.find("unknown name") != std::string::npos);
    CHECK(error_of("1 2").message.find("unexpected") != std::string::npos);
    CHECK(error_of("").message == "expression expected");
}

void registers_and_memory() {
    auto m = machine();
    // 8000: LD HL,9000h (then stopped at 8003h)
    m->load(0x8000, std::vector<std::uint8_t>{0x21, 0x00, 0x90, 0x00});
    m->load(0x9000, std::vector<std::uint8_t>{0x34, 0x12});
    m->load(0x9FFE, std::vector<std::uint8_t>{0xCD, 0xAB});
    m->jump(0x8000);
    m->run({.max_ticks = 1000, .max_instructions = 1});
    freetvc::Z80Registers r = m->cpu().registers();
    r.af = 0x4241;  // A = 42h, F = Z | C... 41h = Z (40h) + C (01h)
    r.sp = 0x9FFE;
    r.ix = 0xA1B2;
    r.af2 = 0x1111;
    m->cpu().set_registers(r);
    VALUE("A", 0x42);
    VALUE("F", 0x41);
    VALUE("hl", 0x9000);
    VALUE("H", 0x90);
    VALUE("AF'", 0x1111);
    VALUE("IXH", 0xA1);
    VALUE("IXL", 0xB2);
    VALUE("ZF", 1);
    VALUE("CF", 1);
    VALUE("SF", 0);
    VALUE("PC", 0x8003);
    VALUE("[HL]", 0x34);
    VALUE("{HL}", 0x1234);
    VALUE("{SP}", 0xABCD);
    VALUE("[HL+1] == 12", 1);
    VALUE("[U2:1000]", 0x34);   // 9000h is U2 offset 1000h in all-RAM mode
    VALUE("{u2 : 1000}", 0x1234);
    VALUE("[VID0:0]", 0);
    VALUE("[CARD0:0]", 0xFF);  // no bytes there: open bus
    VALUE("CYCLE * 2 <= TICKS", 1);
    VALUE("FRAME", 0);
    CHECK(value(*m, "LINE", __LINE__) == m->video().beam_line());

    Hit hit;
    hit.value = 0x5A;
    hit.address = 0x1234;
    CHECK(value(*m, "VALUE == 5A && ADDRESS == 1234", __LINE__, {}, &hit) == 1);
    CHECK(value(*m, "VALUE", __LINE__) == 0);

    const auto [constant, e1] = Expression::parse("2*8+1");
    const auto [variable, e2] = Expression::parse("A+1");
    CHECK(constant && constant->constant());
    CHECK(variable && !variable->constant());
    CHECK(Expression().evaluate({*m}) == 0);
}

void symbol_tables() {
    SymbolTable table;
    // Our plain format and sjasmplus --sym output (as written by sjasmplus 1.24.0)
    const LoadResult plain = table.load_text("; comment\nstart = 0x6639  ; entry\nSCREEN EQU 8000h\n\nbad line\n", "plain");
    CHECK(plain.loaded == 2);
    CHECK(plain.errors.size() == 1 && plain.errors[0].find("line 5") == 0);
    const LoadResult sjasm = table.load_text(
        "SCREEN2: EQU 0x00008000\ngfx.plot: EQU 0x0000663F\nstart.loop: EQU 0x0000663C\nbig: EQU 0x00012345\n", "prog.sym");
    CHECK(sjasm.loaded == 3);
    CHECK(sjasm.errors.empty());
    CHECK(table.find("start") && table.find("start")->address == 0x6639);
    CHECK(table.find("START") && table.find("START")->name == "start");
    CHECK(table.find("gfx.plot") && table.find("gfx.plot")->address == 0x663F);
    CHECK(!table.find("big"));

    // Banked ROM symbols: one image, two CPU addresses
    table.load_text("reset = 0x0000\n", "p0", freetvc::debug::Placement{Bank::Sys, 0, 0, 0x3FFF});
    table.load_text("os_c000 = 0xC000\nfar = 0x4000\n", "p3", freetvc::debug::Placement{Bank::Sys, 0xC000, 0xC000, 0xFFFF});
    CHECK(!table.find("far"));  // outside the placement range
    const Symbol* s0 = table.at(0x0000, freetvc::BankAddress{Bank::Sys, 0});
    const Symbol* s3 = table.at(0xC000, freetvc::BankAddress{Bank::Sys, 0});
    CHECK(s0 && s0->name == "reset");
    CHECK(s3 && s3->name == "os_c000");
    CHECK(!table.at(0x0000, freetvc::BankAddress{Bank::U0, 0}));  // RAM in page 0: no ROM name
    const Symbol* u = table.at(0x6639, freetvc::BankAddress{Bank::U1, 0x2639});
    CHECK(u && u->name == "start");

    // Expressions see symbols; banks win over symbols before ':'
    auto m = machine();
    ParseOptions options;
    options.symbols = &table;
    CHECK(value(*m, "start + 1", __LINE__, options) == 0x663A);
    CHECK(value(*m, "gfx.plot - start", __LINE__, options) == 6);
    const auto [spec, error] = parse_address("U2:start", options);
    CHECK(spec && spec->bank == Bank::U2 && spec->value.evaluate({*m}) == 0x6639);
    const auto [plain_spec, e2] = parse_address("1234", options);
    CHECK(plain_spec && !plain_spec->bank && plain_spec->value.evaluate({*m}) == 0x1234);
    const auto [bad, e3] = parse_address("VID1:", options);
    CHECK(!bad && e3.position == 5);
    CHECK(parse_bank("vid3") == Bank::Vid3);
    CHECK(!parse_bank("VID4"));

    CHECK(table.remove_source("prog.sym") == 3);
    CHECK(!table.find("gfx.plot"));
    CHECK(table.find("start"));

    const std::string_view check = "123456789";
    CHECK(crc32(std::span(reinterpret_cast<const std::uint8_t*>(check.data()), check.size())) == 0xCBF43926u);
}

void conditions_in_checkpoints() {
    // 8000 INC A / JR 8000h: stop when A reaches 3 through an expression condition
    auto m = machine();
    m->load(0x8000, std::vector<std::uint8_t>{0x3C, 0x18, 0xFD});
    freetvc::Z80Registers r = m->cpu().registers();
    r.af = 0;
    m->cpu().set_registers(r);
    m->jump(0x8000);
    Debugger dbg(*m);
    Checkpoint c;
    c.access = freetvc::kAccessExec;
    c.start = c.end = 0x8001;
    auto [e, error] = Expression::parse("A == 3");
    CHECK(e.has_value());
    if (e) {
        c.condition = make_condition(*e);
        c.condition_text = e->text();
    }
    const unsigned id = dbg.add(c);
    const Stop stop = dbg.run(2 * 62800);
    CHECK(stop.why == Why::Checkpoint);
    CHECK((dbg.registers().af >> 8) == 3);
    CHECK(dbg.find(id)->hits == 1);
    CHECK(dbg.find(id)->condition_text == "A == 3");
}

}  // namespace

void hostile_input() {
    // Deep brackets, signs and operator chains are refused before they exhaust the stack (an MCP request crashed on 100 000)
    const auto parses = [](const std::string& text) { return Expression::parse(text).first.has_value(); };
    CHECK(parses(std::string(100, '(') + "1" + std::string(100, ')')));
    CHECK(!parses(std::string(101, '(') + "1" + std::string(101, ')')));
    CHECK(!parses(std::string(100000, '(') + "1" + std::string(100000, ')')));
    CHECK(!parses(std::string(100000, '[') + "0" + std::string(100000, ']')));
    CHECK(!parses(std::string(100000, '-') + "1"));
    std::string chain = "1";
    for (int i = 0; i < 999; ++i) {
        chain += "+1";
    }
    const auto [sum, error] = Expression::parse(chain);
    auto m = machine();
    CHECK(sum && sum->evaluate({*m}) == 1000);
    CHECK(!parses(chain + "+1"));
    std::string longer = "1";
    for (int i = 0; i < 500000; ++i) {
        longer += "+1";
    }
    const auto [none, too_long] = Expression::parse(longer);
    CHECK(!none && too_long.message.find("too long") != std::string::npos);
}

int main() {
    hostile_input();
    numbers_and_operators();
    registers_and_memory();
    symbol_tables();
    conditions_in_checkpoints();
    if (g_failures == 0) {
        std::printf("expression_test: all checks passed\n");
        return 0;
    }
    std::printf("expression_test: %d check(s) failed\n", g_failures);
    return 1;
}
