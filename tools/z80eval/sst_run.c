// SPDX-License-Identifier: GPL-3.0-or-later
// Z80 core evaluation: check floooh z80.h against flattened SingleStepTests/z80 (final state + per-T-state bus accesses)
#define CHIPS_IMPL
#include "z80.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { R_PC, R_SP, R_A, R_B, R_C, R_D, R_E, R_F, R_H, R_L, R_I, R_R, R_EI, R_WZ, R_IX, R_IY,
       R_AF2, R_BC2, R_DE2, R_HL2, R_IM, R_P, R_Q, R_IFF1, R_IFF2, R_NUM };
static const char* reg_name[R_NUM] = { "pc", "sp", "a", "b", "c", "d", "e", "f", "h", "l", "i", "r", "ei", "wz",
    "ix", "iy", "af_", "bc_", "de_", "hl_", "im", "p", "q", "iff1", "iff2" };

#define MAXN 256
typedef struct { int addr, data; char fl[8]; } cyc_t;
typedef struct { int addr, val; char dir; } port_t;
typedef struct { int tick; char kind; int addr, data; } ev_t;  // kind: F=M1 fetch, R/W=memory, I/O=port

static struct {
    char name[64];
    int ini[R_NUM], fin[R_NUM];
    int nini, nfin, ncyc, nport;
    int iniram[MAXN][2], finram[MAXN][2];
    cyc_t cyc[MAXN];
    port_t port[16];
} t;

static uint8_t mem[1 << 16];
static long n_tests, n_fail, f_len, f_ram, f_bus, f_rfsh, f_tim, f_reg[R_NUM], f_xy_only;
static long off_hist[5][16];  // per kind F,R,W,I,O: SST cycle index minus z80.h tick index, +8
static const int off_expect[5] = { 1, 0, 0, 0, 1 };  // z80.h pins: M1 at T1, MR/MW/IOW at T2, IOR at TW; SST marks T2/T2/T2/TW/TW
static int shown;

static int kind_idx(char k) { return k == 'F' ? 0 : k == 'R' ? 1 : k == 'W' ? 2 : k == 'I' ? 3 : 4; }

static void run_test(void) {
    memset(mem, 0, sizeof(mem));
    for (int i = 0; i < t.nini; i++) mem[t.iniram[i][0]] = (uint8_t)t.iniram[i][1];
    z80_t cpu;
    z80_init(&cpu);
    cpu.sp = t.ini[R_SP]; cpu.a = t.ini[R_A]; cpu.f = t.ini[R_F];
    cpu.b = t.ini[R_B]; cpu.c = t.ini[R_C]; cpu.d = t.ini[R_D]; cpu.e = t.ini[R_E];
    cpu.h = t.ini[R_H]; cpu.l = t.ini[R_L]; cpu.i = t.ini[R_I]; cpu.r = t.ini[R_R];
    cpu.wz = t.ini[R_WZ]; cpu.ix = t.ini[R_IX]; cpu.iy = t.ini[R_IY];
    cpu.af2 = t.ini[R_AF2]; cpu.bc2 = t.ini[R_BC2]; cpu.de2 = t.ini[R_DE2]; cpu.hl2 = t.ini[R_HL2];
    cpu.im = t.ini[R_IM]; cpu.iff1 = t.ini[R_IFF1]; cpu.iff2 = t.ini[R_IFF2];
    uint64_t pins = z80_prefetch(&cpu, (uint16_t)t.ini[R_PC]);

    ev_t ev[MAXN]; int nev = 0, nport_used = 0;
    int rfsh[MAXN]; for (int i = 0; i < MAXN; i++) rfsh[i] = -1;
    int k;
    for (k = 0; k < 200; k++) {
        pins = z80_tick(&cpu, pins);
        if (k > 0 && z80_opdone(&cpu)) break;
        const uint16_t addr = Z80_GET_ADDR(pins);
        if ((pins & (Z80_MREQ|Z80_RFSH)) == (Z80_MREQ|Z80_RFSH)) { if (k < MAXN) rfsh[k] = addr; }
        else if (pins & Z80_MREQ) {
            if (pins & Z80_RD) { Z80_SET_DATA(pins, mem[addr]); ev[nev++] = (ev_t){ k, (pins & Z80_M1) ? 'F' : 'R', addr, mem[addr] }; }
            else if (pins & Z80_WR) { mem[addr] = Z80_GET_DATA(pins); ev[nev++] = (ev_t){ k, 'W', addr, Z80_GET_DATA(pins) }; }
        }
        else if (pins & Z80_IORQ) {
            if (pins & Z80_RD) {
                int v = 0xFF;
                while (nport_used < t.nport && t.port[nport_used].dir != 'r') nport_used++;
                if (nport_used < t.nport) v = t.port[nport_used++].val;
                Z80_SET_DATA(pins, v); ev[nev++] = (ev_t){ k, 'I', addr, v };
            }
            else if (pins & Z80_WR) { ev[nev++] = (ev_t){ k, 'O', addr, Z80_GET_DATA(pins) }; }
        }
        if (nev >= MAXN - 1) break;
    }
    const int ticks = k;

    int fail = 0; char why[512] = "";
    if (ticks != t.ncyc) { f_len++; fail = 1; snprintf(why + strlen(why), sizeof(why) - strlen(why), " len %d!=%d", ticks, t.ncyc); }
    int got[R_NUM];
    got[R_PC] = (uint16_t)(cpu.pc - 1); got[R_SP] = cpu.sp; got[R_A] = cpu.a; got[R_B] = cpu.b; got[R_C] = cpu.c;
    got[R_D] = cpu.d; got[R_E] = cpu.e; got[R_F] = cpu.f; got[R_H] = cpu.h; got[R_L] = cpu.l; got[R_I] = cpu.i;
    got[R_R] = cpu.r; got[R_WZ] = cpu.wz; got[R_IX] = cpu.ix; got[R_IY] = cpu.iy; got[R_AF2] = cpu.af2;
    got[R_BC2] = cpu.bc2; got[R_DE2] = cpu.de2; got[R_HL2] = cpu.hl2; got[R_IM] = cpu.im;
    got[R_IFF1] = cpu.iff1; got[R_IFF2] = cpu.iff2;
    for (int r = 0; r < R_NUM; r++) {
        if (r == R_EI || r == R_P || r == R_Q) continue;  // no such state in z80.h
        if (got[r] != t.fin[r]) {
            f_reg[r]++; fail = 1;
            if (r == R_F && !((got[r] ^ t.fin[r]) & ~0x28)) f_xy_only++;
            snprintf(why + strlen(why), sizeof(why) - strlen(why), " %s %X!=%X", reg_name[r], got[r], t.fin[r]);
        }
    }
    for (int i = 0; i < t.nfin; i++) {
        if (mem[t.finram[i][0]] != t.finram[i][1]) { f_ram++; fail = 1; snprintf(why + strlen(why), sizeof(why) - strlen(why), " ram[%X]", t.finram[i][0]); break; }
    }
    // pair z80.h accesses with SST bus states in order; record the tick offset per access kind
    int j = 0, busbad = 0, timbad = 0;
    for (int e = 0; e < nev && !busbad; e++) {
        while (j < t.ncyc && t.cyc[j].fl[0] != 'r' && t.cyc[j].fl[1] != 'w') j++;
        if (j >= t.ncyc) { busbad = 1; break; }
        const cyc_t* c = &t.cyc[j];
        const char sk = (c->fl[3] == 'i') ? (c->fl[0] == 'r' ? 'I' : 'O') : (c->fl[0] == 'r' ? 'R' : 'W');
        const char ek = (ev[e].kind == 'F') ? 'R' : ev[e].kind;
        if (sk != ek || c->addr != ev[e].addr || (c->fl[1] == 'w' && c->data >= 0 && c->data != ev[e].data)) busbad = 1;
        else {
            const int q = kind_idx(ev[e].kind), d = j - ev[e].tick;
            if (d + 8 >= 0 && d + 8 < 16) off_hist[q][d + 8]++;
            if (d != off_expect[q]) timbad = 1;
        }
        j++;
    }
    if (!busbad) { while (j < t.ncyc && t.cyc[j].fl[0] != 'r' && t.cyc[j].fl[1] != 'w') j++; if (j < t.ncyc) busbad = 1; }
    if (busbad) { f_bus++; fail = 1; strcat(why, " bus"); }
    if (timbad) { f_tim++; fail = 1; strcat(why, " timing"); }
    for (int i = 0; i < ticks && i < t.ncyc; i++) {
        if (rfsh[i] >= 0 && t.cyc[i].addr != rfsh[i]) { f_rfsh++; fail = 1; strcat(why, " rfsh"); break; }
    }
    n_tests++;
    if (fail) { n_fail++; if (shown < 3) { shown++; printf("    FAIL %s:%s\n", t.name, why); } }
}

int main(int argc, char** argv) {
    char line[256];
    int in_test = 0;
    while (fgets(line, sizeof(line), stdin)) {
        char* p = line + 2;
        switch (line[0]) {
        case 'T': memset(&t, 0, sizeof(t)); sscanf(p, "%63s", t.name); in_test = 1; break;
        case 'I': case 'F': {
            int* dst = (line[0] == 'I') ? t.ini : t.fin;
            for (int r = 0; r < R_NUM; r++) dst[r] = (int)strtol(p, &p, 10);
            break;
        }
        case 'M': if (t.nini < MAXN) { sscanf(p, "%d %d", &t.iniram[t.nini][0], &t.iniram[t.nini][1]); t.nini++; } break;
        case 'N': if (t.nfin < MAXN) { sscanf(p, "%d %d", &t.finram[t.nfin][0], &t.finram[t.nfin][1]); t.nfin++; } break;
        case 'C': if (t.ncyc < MAXN) { sscanf(p, "%d %d %7s", &t.cyc[t.ncyc].addr, &t.cyc[t.ncyc].data, t.cyc[t.ncyc].fl); t.ncyc++; } break;
        case 'P': if (t.nport < 16) { char d; sscanf(p, "%d %d %c", &t.port[t.nport].addr, &t.port[t.nport].val, &d); t.port[t.nport].dir = d; t.nport++; } break;
        case 'E': if (in_test) run_test(); in_test = 0; break;
        default: break;
        }
    }
    printf("%-14s tests %ld fail %ld | len %ld ram %ld bus %ld timing %ld rfsh %ld | F %ld (XY only %ld)",
           argc > 1 ? argv[1] : "-", n_tests, n_fail, f_len, f_ram, f_bus, f_tim, f_rfsh, f_reg[R_F], f_xy_only);
    for (int r = 0; r < R_NUM; r++) if (r != R_F && f_reg[r]) printf(" %s %ld", reg_name[r], f_reg[r]);
    printf("\n  offsets(SST-z80.h)");
    static const char kn[] = "FRWIO";
    for (int q = 0; q < 5; q++) for (int d = 0; d < 16; d++) if (off_hist[q][d]) printf(" %c%+d:%ld", kn[q], d - 8, off_hist[q][d]);
    printf("\n");
    return 0;
}
