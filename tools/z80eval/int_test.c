// SPDX-License-Identifier: GPL-3.0-or-later
// Z80 core evaluation: interrupt timing of floooh z80.h (IM 1 + EI delay, NMI, HALT exit) as a bus timeline
#define CHIPS_IMPL
#include "z80.h"
#include <stdio.h>
#include <string.h>

static uint8_t mem[1 << 16];

// int_on/nmi_at: tick from which INT is held high / tick at which NMI rises (-1 = never)
static void run(const char* title, const uint8_t* prog, int len, int int_on, int nmi_at, int nticks) {
    memset(mem, 0, sizeof(mem));
    memcpy(mem, prog, len);
    z80_t cpu;
    uint64_t pins = z80_init(&cpu);
    cpu.sp = 0x8000;
    printf("== %s\n", title);
    for (int k = 0; k < nticks; k++) {
        if (int_on >= 0 && k >= int_on) pins |= Z80_INT; else pins &= ~Z80_INT;
        if (nmi_at >= 0 && k >= nmi_at) pins |= Z80_NMI; else pins &= ~Z80_NMI;
        pins = z80_tick(&cpu, pins);
        const uint16_t a = Z80_GET_ADDR(pins);
        if ((pins & (Z80_M1|Z80_MREQ|Z80_RD)) == (Z80_M1|Z80_MREQ|Z80_RD)) {
            Z80_SET_DATA(pins, mem[a]);
            printf("  t%-3d M1   %04X op %02X%s\n", k, a, mem[a], (pins & Z80_HALT) ? " (HALT pin)" : "");
        }
        else if ((pins & (Z80_M1|Z80_IORQ)) == (Z80_M1|Z80_IORQ)) { Z80_SET_DATA(pins, 0xFF); printf("  t%-3d INTA (M1|IORQ)\n", k); }
        else if ((pins & (Z80_MREQ|Z80_RFSH)) == (Z80_MREQ|Z80_RFSH)) { }
        else if (pins & Z80_MREQ) {
            if (pins & Z80_RD) { Z80_SET_DATA(pins, mem[a]); }
            else if (pins & Z80_WR) { mem[a] = Z80_GET_DATA(pins); printf("  t%-3d MW   %04X <- %02X\n", k, a, Z80_GET_DATA(pins)); }
        }
    }
}

int main(void) {
    // IM 1; EI; NOP; NOP ... with INT held high from tick 0
    static const uint8_t p1[] = { 0xED, 0x56, 0xFB, 0x00, 0x00, 0x00 };
    run("IM 1, EI, NOP: INT high from t0", p1, sizeof(p1), 0, -1, 34);
    // NOPs, NMI rises at tick 5 (inside the second NOP)
    static const uint8_t p2[] = { 0x00, 0x00, 0x00, 0x00 };
    run("NOPs: NMI rises at t5", p2, sizeof(p2), -1, 5, 24);
    // IM 1; EI; HALT; INT rises at tick 30
    static const uint8_t p3[] = { 0xED, 0x56, 0xFB, 0x76, 0x00 };
    run("IM 1, EI, HALT at 0003h: INT high from t30", p3, sizeof(p3), 30, -1, 50);
    return 0;
}
