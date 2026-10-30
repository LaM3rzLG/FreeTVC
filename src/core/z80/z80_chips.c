/* SPDX-License-Identifier: GPL-3.0-or-later */
/* The only translation unit that sees z80.h: implementation plus the bridge functions. */
#define CHIPS_IMPL
#include "z80.h"

#include "z80_bridge.h"

#include <stdalign.h>

_Static_assert(sizeof(z80_t) <= FTVC_Z80_STATE_SIZE, "FTVC_Z80_STATE_SIZE too small for z80_t");
_Static_assert(alignof(z80_t) <= FTVC_Z80_STATE_ALIGN, "FTVC_Z80_STATE_ALIGN too small for z80_t");
_Static_assert(Z80_D0 == (1ULL << FTVC_Z80_PIN_D0), "pin D0");
_Static_assert(Z80_M1 == (1ULL << FTVC_Z80_PIN_M1), "pin M1");
_Static_assert(Z80_MREQ == (1ULL << FTVC_Z80_PIN_MREQ), "pin MREQ");
_Static_assert(Z80_IORQ == (1ULL << FTVC_Z80_PIN_IORQ), "pin IORQ");
_Static_assert(Z80_RD == (1ULL << FTVC_Z80_PIN_RD), "pin RD");
_Static_assert(Z80_WR == (1ULL << FTVC_Z80_PIN_WR), "pin WR");
_Static_assert(Z80_HALT == (1ULL << FTVC_Z80_PIN_HALT), "pin HALT");
_Static_assert(Z80_INT == (1ULL << FTVC_Z80_PIN_INT), "pin INT");
_Static_assert(Z80_NMI == (1ULL << FTVC_Z80_PIN_NMI), "pin NMI");
_Static_assert(Z80_RFSH == (1ULL << FTVC_Z80_PIN_RFSH), "pin RFSH");

uint64_t ftvc_z80_init(void* state) { return z80_init((z80_t*)state); }

uint64_t ftvc_z80_tick(void* state, uint64_t pins) { return z80_tick((z80_t*)state, pins); }

uint64_t ftvc_z80_prefetch(void* state, uint16_t pc) { return z80_prefetch((z80_t*)state, pc); }

bool ftvc_z80_opdone(const void* state) { return z80_opdone((z80_t*)state); }

void ftvc_z80_get_regs(const void* state, ftvc_z80_regs* out) {
    const z80_t* cpu = (const z80_t*)state;
    out->af = cpu->af;
    out->bc = cpu->bc;
    out->de = cpu->de;
    out->hl = cpu->hl;
    out->ix = cpu->ix;
    out->iy = cpu->iy;
    out->sp = cpu->sp;
    out->pc = cpu->pc;
    out->wz = cpu->wz;
    out->af2 = cpu->af2;
    out->bc2 = cpu->bc2;
    out->de2 = cpu->de2;
    out->hl2 = cpu->hl2;
    out->i = cpu->i;
    out->r = cpu->r;
    out->im = cpu->im;
    out->iff1 = cpu->iff1;
    out->iff2 = cpu->iff2;
    out->q = cpu->q;
}

void ftvc_z80_set_regs(void* state, const ftvc_z80_regs* in) {
    z80_t* cpu = (z80_t*)state;
    cpu->af = in->af;
    cpu->bc = in->bc;
    cpu->de = in->de;
    cpu->hl = in->hl;
    cpu->ix = in->ix;
    cpu->iy = in->iy;
    cpu->sp = in->sp;
    cpu->wz = in->wz;
    cpu->af2 = in->af2;
    cpu->bc2 = in->bc2;
    cpu->de2 = in->de2;
    cpu->hl2 = in->hl2;
    cpu->i = in->i;
    cpu->r = in->r;
    cpu->im = in->im;
    cpu->iff1 = in->iff1;
    cpu->iff2 = in->iff2;
    cpu->q = in->q; /* z80_prefetch() carries it across its restart fetch */
}

void ftvc_z80_get_internal(const void* state, ftvc_z80_internal* out) {
    const z80_t* cpu = (const z80_t*)state;
    out->step = cpu->step;
    out->addr = cpu->addr;
    out->pc = cpu->pc;
    out->dlatch = cpu->dlatch;
    out->opcode = cpu->opcode;
    out->hlx_idx = cpu->hlx_idx;
    out->prefix_active = cpu->prefix_active;
    out->qw = cpu->qw;
    out->pins = cpu->pins;
    out->int_bits = cpu->int_bits;
}

void ftvc_z80_set_internal(void* state, const ftvc_z80_internal* in) {
    z80_t* cpu = (z80_t*)state;
    cpu->step = in->step;
    cpu->addr = in->addr;
    cpu->pc = in->pc;
    cpu->dlatch = in->dlatch;
    cpu->opcode = in->opcode;
    cpu->hlx_idx = in->hlx_idx;
    cpu->prefix_active = in->prefix_active;
    cpu->qw = in->qw;
    cpu->pins = in->pins;
    cpu->int_bits = in->int_bits;
}
