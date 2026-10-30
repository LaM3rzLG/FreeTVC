// SPDX-License-Identifier: GPL-3.0-or-later
// The debug core on the real ROMs: an attached debugger changes nothing, checkpoints see the OS at work,
// and step out / step over follow the RST 30h convention (rom.md §4). Prints boot times with and without the debugger.
// usage: debug_rom_test <rom dir> <preset>; exit 77 (CTest skip) when the ROM files are missing.
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "core/machine/machine.hpp"
#include "debug/debugger.hpp"
#include "debug/disassembler.hpp"
#include "debug/expression.hpp"
#include "debug/symbols.hpp"
#include "frontend/common/presets.hpp"

namespace {

using freetvc::Machine;
using freetvc::debug::Checkpoint;
using freetvc::debug::Debugger;
using freetvc::debug::Disassembly;
using freetvc::debug::DisasmOptions;
using freetvc::debug::Expression;
using freetvc::debug::ParseOptions;
using freetvc::debug::SymbolTable;
using freetvc::debug::Why;

constexpr int kExitSkip = 77;
constexpr int kKeyFrame = 330;  // as boot_test: SPACE dismisses the logo
constexpr int kKeyFrames = 5;
constexpr int kFrames = 450;
constexpr std::uint64_t kFrameTicks = freetvc::kTStatesPerFrame * freetvc::kTicksPerTState;

int g_failures = 0;

void expect(bool ok, const char* what) {
    if (!ok) {
        ++g_failures;
        std::printf("FAIL %s\n", what);
    }
}

// Boots to the prompt; with a debugger every frame goes through Debugger::run.
double boot(Machine& m, Debugger* dbg) {
    const auto start = std::chrono::steady_clock::now();
    for (int frame = 0; frame < kFrames; ++frame) {
        if (frame == kKeyFrame || frame == kKeyFrame + kKeyFrames) {
            m.set_key(7, 5, frame == kKeyFrame);
        }
        if (dbg) {
            const auto stop = dbg->run(kFrameTicks);
            if (stop.why != Why::Budget) {
                std::printf("FAIL unexpected stop at %04X in frame %d\n", stop.pc, frame);
                ++g_failures;
                return 0;
            }
        } else {
            m.run({.max_ticks = kFrameTicks});
        }
    }
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

std::uint16_t word_at(const Machine& m, std::uint16_t address) {
    return static_cast<std::uint16_t>(m.peek(address) | m.peek(static_cast<std::uint16_t>(address + 1)) << 8);
}

Checkpoint watch(std::uint8_t access, std::uint16_t start, std::uint16_t end, bool stop) {
    Checkpoint c;
    c.access = access;
    c.start = start;
    c.end = end;
    c.stop = stop;
    return c;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: debug_rom_test <rom dir> <preset>\n");
        return 2;
    }
    const freetvc::Preset* preset = freetvc::find_preset(argv[2]);
    if (!preset) {
        std::fprintf(stderr, "unknown preset '%s'\n", argv[2]);
        return 2;
    }
    std::string error;
    const auto config = freetvc::load_preset(*preset, argv[1], error);
    if (!config) {
        std::printf("SKIP: %s (set FREETVC_ROM_DIR)\n", error.c_str());
        return kExitSkip;
    }

    // Reference boot, then the same boot with an idle debugger, then with trace and logging checkpoints
    Machine plain(*config);
    const double plain_ms = boot(plain, nullptr);
    Machine idle(*config);
    double idle_ms = 0;
    {
        Debugger dbg(idle);
        idle_ms = boot(idle, &dbg);
    }
    Machine m(*config);
    Debugger dbg(m);
    dbg.set_trace_capacity(65536);
    Checkpoint rst30 = watch(freetvc::kAccessExec, 0x0030, 0x0030, false);
    rst30.bank = freetvc::Bank::U0;  // the OS runs with U0 in page 0 (P_SAVE = 70h, rom.md §4), vectors copied there
    const unsigned rst30_id = dbg.add(rst30);
    Checkpoint vram = watch(freetvc::kAccessWrite, 0x8000, 0xBFFF, false);
    vram.bank = freetvc::Bank::Vid0;
    const unsigned vram_id = dbg.add(vram);
    const unsigned paging_id = dbg.add(watch(freetvc::kAccessOut, 0x02, 0x02, false));
    dbg.save_state("power-on");
    const double busy_ms = boot(m, &dbg);
    std::printf("%s: boot of %d frames: %.0f ms plain, %.0f ms idle debugger, %.0f ms with trace + 3 logging checkpoints\n", argv[2],
                kFrames, plain_ms, idle_ms, busy_ms);

    expect(plain.ticks() == idle.ticks() && plain.ticks() == m.ticks(), "same ticks with a debugger");
    expect(plain.framebuffer().pixels == idle.framebuffer().pixels && plain.framebuffer().pixels == m.framebuffer().pixels,
           "same frame with a debugger");
    expect(plain.memory().ram().size() == m.memory().ram().size() &&
               std::equal(plain.memory().ram().begin(), plain.memory().ram().end(), m.memory().ram().begin()),
           "same RAM with a debugger");
    const std::uint64_t rst30_hits = dbg.find(rst30_id)->hits;
    const std::uint64_t vram_hits = dbg.find(vram_id)->hits;
    const std::uint64_t paging_hits = dbg.find(paging_id)->hits;
    std::printf("hits: RST 30h entry %llu, VID0 writes %llu, OUT (02h) %llu; trace holds %zu instructions\n",
                static_cast<unsigned long long>(rst30_hits), static_cast<unsigned long long>(vram_hits),
                static_cast<unsigned long long>(paging_hits), dbg.trace().size());
    expect(rst30_hits > 0 && vram_hits > 0 && paging_hits > 0, "checkpoints hit during boot");
    for (const auto& hit : dbg.hit_log().last(dbg.hit_log().size())) {
        if (hit.checkpoint == paging_id && hit.where.bank != freetvc::Bank::Open) {
            expect(false, "port hits carry no bank");
            break;
        }
        if (hit.checkpoint == vram_id && hit.where.bank != freetvc::Bank::Vid0) {
            expect(false, "VID0 hits are in VID0");
            break;
        }
    }
    dbg.clear_checkpoints();
    dbg.save_state("prompt");

    // Built-in ROM symbols, found by CRC-32; the vectors at the prompt are the U0 copy (OS_ENTRY: JP INFUNC)
    SymbolTable symbols;
    const std::vector<std::string> files = load_rom_symbols(symbols, m);
    std::string file_list;
    for (const std::string& f : files) {
        file_list += " " + f;
    }
    std::printf("ROM symbols:%s (%zu names)\n", file_list.c_str(), symbols.size());
    expect(files.size() == 4, "symbol files for SYS (2), EXT and RAM");
    const freetvc::debug::Symbol* os_entry = symbols.at(0x0030, m);
    expect(os_entry && os_entry->name == "OS_ENTRY", "0030h is OS_ENTRY in U0");
    DisasmOptions labels;
    labels.label = symbol_labels(symbols, m);
    const Disassembly vector = disassemble(m, 0x0030, labels);
    std::printf("0030  %s\n", vector.text.c_str());
    expect(vector.text == "JP INFUNC", "0030h: JP INFUNC");
    // IT_ENTRY (tvc-ram.sym): PUSH AF / OUT (2),70h / JP into SYS
    std::uint16_t at = 0x0038;
    std::vector<std::string> irq_entry;
    for (int i = 0; i < 4; ++i) {
        const Disassembly d = disassemble(m, at, labels);
        const freetvc::debug::Symbol* s = symbols.at(at, m);
        std::printf("%04X  %-10s %s\n", at, s ? s->name.c_str() : "", d.text.c_str());
        irq_entry.push_back(d.text);
        at = static_cast<std::uint16_t>(at + d.length);
    }
    expect(irq_entry[0] == "PUSH AF" && irq_entry[1] == "LD A,70h" && irq_entry[2] == "OUT (02h),A" &&
               irq_entry[3].rfind("JP ", 0) == 0,
           "0038h: the IRQ entry");

    // Step out of the RST 30h handler: back after the function byte, with the return address popped.
    // The first output call of the boot (function byte b7 = 0); input calls such as 91h can wait for a key.
    expect(dbg.load_state("power-on"), "load the power-on state");
    ParseOptions with_symbols;
    with_symbols.symbols = &symbols;
    const auto [entry, entry_error] = parse_address("U0:OS_ENTRY", with_symbols);
    const auto [output_only, cond_error] = Expression::parse("([{SP}] & 80h) == 0", with_symbols);
    expect(entry && entry->bank == freetvc::Bank::U0 && output_only, "address and condition parse");
    if (!entry || !output_only) {
        return 1;
    }
    Checkpoint output_call = watch(freetvc::kAccessExec, static_cast<std::uint16_t>(entry->value.evaluate({m})), 0, true);
    output_call.end = output_call.start;
    output_call.bank = entry->bank;
    output_call.condition = make_condition(*output_only);
    dbg.add(output_call);
    auto stop = dbg.run(kFrames * kFrameTicks);
    std::printf("first output call in frame %llu\n", static_cast<unsigned long long>(m.frame_count()));
    expect(stop.why == Why::Checkpoint && stop.pc == 0x0030, "an output system call during boot");
    const std::uint16_t sp_in = dbg.registers().sp;
    const std::uint16_t callsite = static_cast<std::uint16_t>(word_at(m, sp_in) - 1);
    const auto recent = dbg.trace().last(2);
    expect(m.peek(callsite) == 0xF7, "RST 30h at the return address - 1");
    expect(recent.size() == 2 && recent[0].regs.pc == callsite && recent[1].regs.pc == 0x0030, "trace shows the RST 30h");
    const std::uint8_t function = m.peek(static_cast<std::uint16_t>(callsite + 1));
    const Disassembly call = disassemble(m, callsite, labels);
    std::printf("%04X  %s  ; %s\n", callsite, call.text.c_str(), call.comment.c_str());
    expect(call.text == "RST 30h : DB " + freetvc::debug::hex8(function) && call.length == 2, "the call disassembles with its byte");
    const char* call_name = freetvc::debug::system_call_name(function);
    expect(call_name && call.comment.rfind(call_name, 0) == 0, "the call is named");
    dbg.clear_checkpoints();
    stop = dbg.step_out(10 * kFrameTicks);
    std::printf("RST 30h at %04X, function %02X: step out after %llu instructions at %04X, SP %04X -> %04X\n", callsite,
                function, static_cast<unsigned long long>(stop.run.instructions_run), stop.pc, sp_in, dbg.registers().sp);
    expect(stop.why == Why::Step && stop.pc == callsite + 2, "step out lands after the function byte");
    expect(dbg.registers().sp == static_cast<std::uint16_t>(sp_in + 2), "step out pops the return address");

    // From power-on again: the same call (a temporary checkpoint, since 001Eh's function byte is patched at run time), step over it
    expect(dbg.load_state("power-on"), "load the power-on state again");
    Checkpoint at_call = watch(freetvc::kAccessExec, callsite, callsite, true);
    at_call.temporary = true;
    at_call.condition = [callsite](const Machine& mm, const freetvc::debug::Hit&) {
        return (mm.peek(static_cast<std::uint16_t>(callsite + 1)) & 0x80) == 0;
    };
    dbg.add(at_call);
    stop = dbg.run(kFrames * kFrameTicks);
    expect(stop.why == Why::Checkpoint && stop.pc == callsite && dbg.checkpoints().empty(), "back at the call site");
    expect(m.peek(static_cast<std::uint16_t>(callsite + 1)) == function, "same function code");
    const std::uint16_t sp_call = dbg.registers().sp;
    stop = dbg.step_over(10 * kFrameTicks);
    expect(stop.why == Why::Step && stop.pc == callsite + 2 && dbg.registers().sp == sp_call, "step over RST 30h");

    // A stopping port checkpoint: the page register holds the value written
    expect(dbg.load_state("prompt"), "load the prompt state again");
    dbg.add(watch(freetvc::kAccessOut, 0x02, 0x02, true));
    stop = dbg.run(50 * kFrameTicks);
    expect(stop.why == Why::Checkpoint && !stop.hits.empty(), "OUT (02h) at the prompt");
    if (!stop.hits.empty()) {
        std::printf("OUT (02h),%02X from %04X\n", stop.hits[0].value, stop.hits[0].pc);
        expect(m.memory().page_register() == stop.hits[0].value, "page register = value written");
    }

    // Raster row 100 with the ROM's CRTC values: one frame (62 800 T) apart, to within an instruction
    expect(dbg.load_state("prompt"), "load the prompt state for the raster");
    dbg.clear_checkpoints();
    dbg.add(watch(freetvc::kAccessLine, 100, 100, true));
    stop = dbg.run(2 * kFrameTicks);
    const std::uint64_t first_row = m.ticks();
    expect(stop.why == Why::Checkpoint && m.video().beam_line() == 100, "row 100");
    stop = dbg.run(2 * kFrameTicks);
    const std::uint64_t period = m.ticks() - first_row;
    std::printf("row 100 again after %llu half-T\n", static_cast<unsigned long long>(period));
    expect(stop.why == Why::Checkpoint && period + 2 * 23 >= kFrameTicks && period <= kFrameTicks + 2 * 23, "a frame later");

    // Profile 50 frames at the prompt: every tick is in some address and some routine; the interrupt entry once a frame
    expect(dbg.load_state("prompt"), "load the prompt state for the profile");
    dbg.clear_checkpoints();
    dbg.set_profile(true);
    const std::uint64_t profile_start = m.ticks();
    dbg.run(50 * kFrameTicks);
    const auto& profile = dbg.profile();
    std::uint64_t by_routine = 0;
    std::uint64_t irq_calls = 0;
    const freetvc::BankAddress irq_vector = m.memory().locate(0x0038);
    for (const auto& r : profile.routines()) {
        by_routine += r.exclusive;
        if (r.entry.bank == irq_vector.bank && r.entry.offset == irq_vector.offset) {
            irq_calls = r.calls;
        }
    }
    std::uint64_t by_address = 0;
    for (std::size_t b = 0; b < freetvc::kBankCount; ++b) {
        for (std::uint16_t o = 0; o < 0x4000; ++o) {
            by_address += profile.ticks({static_cast<freetvc::Bank>(b), o});
        }
    }
    const std::uint64_t ran = m.ticks() - profile_start;
    std::printf("profile: %llu instructions, %llu of %llu half-T, %zu routines, interrupt entry %llu times, depth %zu\n",
                static_cast<unsigned long long>(profile.instructions()), static_cast<unsigned long long>(profile.total_ticks()),
                static_cast<unsigned long long>(ran), profile.routines().size(), static_cast<unsigned long long>(irq_calls), profile.depth());
    expect(by_routine == profile.total_ticks() && by_address == profile.total_ticks(), "profile sums agree");
    expect(profile.total_ticks() <= ran && ran - profile.total_ticks() < 2 * 30, "profile covers the run but the last instruction");
    expect(irq_calls >= 49 && irq_calls <= 51, "interrupt entry once a frame");
    expect(profile.depth() <= 4, "shadow stack unwinds");
    dbg.set_profile(false);

    std::printf("%s\n", g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
