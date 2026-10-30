// SPDX-License-Identifier: GPL-3.0-or-later
// Unit checks for the Z80 adapter and the machine run loop; expected values come from the Zilog timings.
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>

#include "core/machine/machine.hpp"
#include "core/z80/z80.hpp"

namespace {

int g_failures = 0;

void check(bool condition, const char* what, unsigned long long got, unsigned long long expected) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL %s: got %llu (0x%llX), expected %llu (0x%llX)\n", what, got, got, expected, expected);
    }
}

#define CHECK_EQ(got, expected) check((got) == (expected), #got, (got), (expected))

void z80_power_on_state() {
    // "The Undocumented Z80 Documented": AF, BC, DE, HL, IX, IY, SP, WZ and the shadow set are FFFFh
    freetvc::Z80 cpu;
    const freetvc::Z80Registers r = cpu.registers();
    CHECK_EQ(r.af, 0xFFFFu);
    CHECK_EQ(r.sp, 0xFFFFu);
    CHECK_EQ(r.ix, 0xFFFFu);
    CHECK_EQ(r.hl2, 0xFFFFu);
    CHECK_EQ(r.i, 0u);
    CHECK_EQ(r.r, 0u);
    CHECK_EQ(r.im, 0u);
    CHECK_EQ(static_cast<unsigned>(r.iff1), 0u);
}

void z80_pin_helpers() {
    using freetvc::Z80;
    const Z80::Pins pins = Z80::kMREQ | Z80::kRD | 0x1234u;
    const Z80::Pins with = Z80::with_data(pins | (Z80::Pins{0xAB} << 16), 0x5A);
    CHECK_EQ(Z80::address(with), 0x1234u);
    CHECK_EQ(Z80::data(with), 0x5Au);
    CHECK_EQ(with & ~(Z80::Pins{0xFF} << 16), pins);
}

struct HaltRun {
    int halted_m1 = 0;           // M1 cycles with the HALT pin
    int halted_m1_off_addr = 0;  // of those, M1 cycles not at HALT+1
    int ack_tick = -1;           // INTA (IM 1) or the NMI M1 at HALT+1 without the HALT pin
    int vector_tick = -1;        // fetch at the vector
    std::uint8_t a_at_ack = 0, r_at_ack = 0;
    std::uint8_t pushed_hi = 0, pushed_lo = 0;
};

// 0000: IM 1; EI; HALT; INC A (must not run while halted). INT (or a rising NMI) from int_tick; SP = 8000h.
HaltRun run_halt(int int_tick, bool nmi, std::uint16_t vector) {
    using freetvc::Z80;
    std::array<std::uint8_t, 0x10000> mem{};
    const std::array<std::uint8_t, 5> prog{0xED, 0x56, 0xFB, 0x76, 0x3C};
    std::copy(prog.begin(), prog.end(), mem.begin());
    Z80 cpu;
    freetvc::Z80Registers regs = cpu.registers();
    regs.sp = 0x8000;
    cpu.set_registers(regs);
    Z80::Pins pins = cpu.prefetch(0x0000);
    HaltRun run;
    for (int t = 0; t < 80 && run.vector_tick < 0; ++t) {
        const Z80::Pins irq = nmi ? Z80::kNMI : Z80::kINT;
        pins = t >= int_tick ? (pins | irq) : (pins & ~irq);
        pins = cpu.tick(pins);
        const std::uint16_t addr = Z80::address(pins);
        if ((pins & (Z80::kM1 | Z80::kIORQ)) == (Z80::kM1 | Z80::kIORQ)) {
            pins = Z80::with_data(pins, 0xFF);
            run.ack_tick = t;
            run.a_at_ack = static_cast<std::uint8_t>(cpu.registers().af >> 8);
            run.r_at_ack = cpu.registers().r;
        } else if ((pins & (Z80::kM1 | Z80::kMREQ | Z80::kRD)) == (Z80::kM1 | Z80::kMREQ | Z80::kRD)) {
            pins = Z80::with_data(pins, mem[addr]);
            if (pins & Z80::kHALT) {
                ++run.halted_m1;
                run.halted_m1_off_addr += addr != 0x0004;
            } else if (nmi && addr == 0x0004 && run.halted_m1 > 0 && run.ack_tick < 0) {
                run.ack_tick = t;
                run.a_at_ack = static_cast<std::uint8_t>(cpu.registers().af >> 8);
                run.r_at_ack = cpu.registers().r;
            } else if (addr == vector) {
                run.vector_tick = t;
            }
        } else if ((pins & (Z80::kMREQ | Z80::kRFSH)) == Z80::kMREQ) {
            if (pins & Z80::kRD) {
                pins = Z80::with_data(pins, mem[addr]);
            } else if (pins & Z80::kWR) {
                mem[addr] = Z80::data(pins);
            }
        }
    }
    run.pushed_hi = mem[0x7FFF];
    run.pushed_lo = mem[0x7FFE];
    return run;
}

void z80_halt_int_exit() {
    // Halted M1s read HALT+1 and run a forced NOP (redcode/Z80 wiki "Z80 Special Reset", real-chip tests);
    // IM 1 timeline as in docs/design/z80-core.md: INT sampled in the last T of the t28 M1, INTA at t34, 0038h at t45.
    const HaltRun run = run_halt(30, false, 0x0038);
    CHECK_EQ(run.halted_m1, 4);
    CHECK_EQ(run.halted_m1_off_addr, 0);
    CHECK_EQ(run.ack_tick, 34);
    CHECK_EQ(run.vector_tick, 45);
    CHECK_EQ(run.a_at_ack, 0xFFu);  // INC A at HALT+1 was never executed
    CHECK_EQ(run.r_at_ack, 8u);     // IM 1, EI (3 M1), HALT, 4 halted M1 (one refresh each)
    CHECK_EQ(run.pushed_hi, 0x00u);
    CHECK_EQ(run.pushed_lo, 0x04u);
}

void z80_halt_nmi_exit() {
    // NMI leaves HALT the same way: return address HALT+1, then the fetch at 0066h
    const HaltRun run = run_halt(30, true, 0x0066);
    CHECK_EQ(run.halted_m1_off_addr, 0);
    CHECK_EQ(static_cast<unsigned>(run.halted_m1 > 0), 1u);
    CHECK_EQ(static_cast<unsigned>(run.vector_tick > 0), 1u);
    CHECK_EQ(run.a_at_ack, 0xFFu);
    CHECK_EQ(run.pushed_hi, 0x00u);
    CHECK_EQ(run.pushed_lo, 0x04u);
}

// 0100: LD A,2Ah (7 T); LD (8000h),A (13 T); HALT
constexpr std::array<std::uint8_t, 6> kProgram{0x3E, 0x2A, 0x32, 0x00, 0x80, 0x76};

// Page register B0h maps U0-U3, so the program runs from plain RAM without clock stretch
void start_program(freetvc::Machine& m) {
    m.io_write(0x02, 0xB0);
    m.load(0x0100, kProgram);
    m.jump(0x0100);
}

void machine_instruction_stop() {
    freetvc::Machine m;
    start_program(m);
    const freetvc::StopInfo stop = m.run({.max_ticks = 1000, .max_instructions = 2});
    CHECK_EQ(static_cast<int>(stop.reason), static_cast<int>(freetvc::StopReason::InstructionCount));
    CHECK_EQ(stop.instructions_run, 2u);
    // 7 + 13 T, plus T1 of the HALT fetch that marks the boundary; 2 ticks per T
    CHECK_EQ(stop.ticks_run, (7u + 13u + 1u) * 2u);
    CHECK_EQ(m.ticks(), stop.ticks_run);
    CHECK_EQ(stop.pc, 0x0105u);
    CHECK_EQ(m.ram()[0x8000], 0x2Au);
    CHECK_EQ(m.cpu().registers().af >> 8, 0x2Au);
}

void machine_tick_budget() {
    freetvc::Machine m;
    start_program(m);
    const freetvc::StopInfo first = m.run({.max_ticks = 11});
    CHECK_EQ(static_cast<int>(first.reason), static_cast<int>(freetvc::StopReason::TickBudget));
    CHECK_EQ(first.ticks_run, 10u);
    // LD A,n is 7 T: its boundary is T1 of the next fetch, the 8th T-state after the jump
    const freetvc::StopInfo second = m.run({.max_ticks = 4});
    CHECK_EQ(second.instructions_run, 0u);
    const freetvc::StopInfo third = m.run({.max_ticks = 2});
    CHECK_EQ(third.instructions_run, 1u);
    CHECK_EQ(third.pc, 0x0102u);
    CHECK_EQ(m.ticks(), 16u);
}

void machine_deterministic() {
    freetvc::Machine a;
    freetvc::Machine b;
    for (freetvc::Machine* m : {&a, &b}) {
        start_program(*m);
        m->run({.max_ticks = 12345});
    }
    CHECK_EQ(a.ticks(), b.ticks());
    CHECK_EQ(a.cpu().registers().af, b.cpu().registers().af);
    CHECK_EQ(a.cpu().registers().r, b.cpu().registers().r);
    CHECK_EQ(static_cast<unsigned>(a.ram()[0x8000] == b.ram()[0x8000]), 1u);
}

void machine_power_on_clears() {
    freetvc::Machine m;
    start_program(m);
    m.run({.max_ticks = 100});
    CHECK_EQ(m.ram()[0x8000], 0x2Au);
    m.power_on();
    CHECK_EQ(m.ticks(), 0u);
    CHECK_EQ(m.ram()[0x0100], 0u);
    CHECK_EQ(m.ram()[0x8000], 0u);
    CHECK_EQ(m.memory().page_register(), 0u);
}

}  // namespace

int main() {
    z80_power_on_state();
    z80_pin_helpers();
    z80_halt_int_exit();
    z80_halt_nmi_exit();
    machine_instruction_stop();
    machine_tick_budget();
    machine_deterministic();
    machine_power_on_clears();
    if (g_failures == 0) {
        std::printf("core_test: all checks passed\n");
        return 0;
    }
    std::printf("core_test: %d check(s) failed\n", g_failures);
    return 1;
}
