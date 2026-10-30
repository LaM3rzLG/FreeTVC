// SPDX-License-Identifier: GPL-3.0-or-later
// Z80 core evaluation: run ZEXDOC/ZEXALL on floooh z80.h with a flat 64 KiB RAM
// .cas: YATE's TVC port (payload at 19EFh, entry 1A03h, RST 30h trapped); .com: CP/M original (BDOS 2/9 trapped at 0005h)
#define CHIPS_IMPL
#include "z80.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static uint8_t mem[1 << 16];

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: zex_run <file.cas|file.com> [max_ticks]\n");
        return 2;
    }
    const unsigned long long max_ticks = (argc > 2) ? strtoull(argv[2], 0, 0) : 0;
    const size_t namelen = strlen(argv[1]);
    const int cpm = namelen > 4 && strcmp(argv[1] + namelen - 4, ".com") == 0;
    static uint8_t buf[1 << 16];
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    const size_t n = fread(buf, 1, sizeof(buf), f);
    fclose(f);

    z80_t cpu;
    uint64_t pins = z80_init(&cpu);
    if (cpm) {
        if (n > 0xFE00 - 0x100) { fprintf(stderr, "COM file too large\n"); return 2; }
        memcpy(&mem[0x100], buf, n);
        // BDOS entry 0005h is trapped; its address word at 0006h gives the program's stack top
        mem[0x0006] = 0x00; mem[0x0007] = 0xFE;
        cpu.sp = 0xFE00;
        pins = z80_prefetch(&cpu, 0x0100);
    }
    else {
        if (n < 0x90 || buf[0] != 0x11) { fprintf(stderr, "not an unbuffered CAS\n"); return 2; }
        const unsigned len = buf[0x82] | (buf[0x83] << 8);
        if (0x90 + len > n || 0x19EF + len > sizeof(mem)) { fprintf(stderr, "bad payload length %u\n", len); return 2; }
        memcpy(&mem[0x19EF], &buf[0x90], len);
        // return address 0000h on the stack marks the end of the run
        cpu.sp = 0xFFFE;
        pins = z80_prefetch(&cpu, 0x1A03);
    }

    unsigned long long ticks = 0, instrs = 0;
    const clock_t t0 = clock();
    for (;;) {
        pins = z80_tick(&cpu, pins);
        ticks++;
        if (z80_opdone(&cpu)) {
            instrs++;
            const uint16_t pc = Z80_GET_ADDR(pins);
            if (pc == 0x0000) break;
            if (cpm && pc == 0x0005) {
                if (cpu.c == 2) { putchar(cpu.e); }
                else if (cpu.c == 9) { for (uint16_t a = cpu.de; mem[a] != '$'; a++) putchar(mem[a]); }
                else { fprintf(stderr, "\nunhandled BDOS function %u\n", cpu.c); return 3; }
                fflush(stdout);
                const uint16_t ret = mem[cpu.sp] | (mem[(uint16_t)(cpu.sp + 1)] << 8);
                cpu.sp += 2;
                pins = z80_prefetch(&cpu, ret);
                continue;
            }
            if (!cpm && pc == 0x0030) {
                const uint16_t ret = mem[cpu.sp] | (mem[(uint16_t)(cpu.sp + 1)] << 8);
                const uint8_t fn = mem[ret];
                if (fn == 0x21) { putchar(cpu.c); fflush(stdout); }
                else if (fn != 0x04) { fprintf(stderr, "\nunhandled RST 30h function %02Xh at %04Xh\n", fn, ret); return 3; }
                cpu.sp += 2;
                pins = z80_prefetch(&cpu, (uint16_t)(ret + 1));
                continue;
            }
        }
        if (pins & Z80_MREQ) {
            const uint16_t addr = Z80_GET_ADDR(pins);
            if (pins & Z80_RD) { Z80_SET_DATA(pins, mem[addr]); }
            else if (pins & Z80_WR) { mem[addr] = Z80_GET_DATA(pins); }
        }
        else if (pins & Z80_IORQ) {
            if (pins & Z80_RD) { Z80_SET_DATA(pins, 0xFF); }
        }
        if (max_ticks && ticks >= max_ticks) { printf("\n[stopped at tick limit]\n"); break; }
    }
    const double secs = (double)(clock() - t0) / CLOCKS_PER_SEC;
    printf("\n[ticks %llu, instructions %llu, cpu %.1f s, %.1f M ticks/s, %.1f x TVC speed]\n",
           ticks, instrs, secs, ticks / secs / 1e6, ticks / secs / 3125000.0);
    return 0;
}
