/* SPDX-License-Identifier: GPL-3.0-or-later */
/* C boundary between the vendored z80.h (compiled as C11 in z80_chips.c) and the C++ adapter. */
#ifndef FREETVC_Z80_BRIDGE_H
#define FREETVC_Z80_BRIDGE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Storage the adapter reserves for z80_t; z80_chips.c checks it at compile time. */
#define FTVC_Z80_STATE_SIZE 64
#define FTVC_Z80_STATE_ALIGN 8

/* Pin bit numbers of the 64-bit pin word (same as z80.h, checked in z80_chips.c). */
#define FTVC_Z80_PIN_D0 16
#define FTVC_Z80_PIN_M1 24
#define FTVC_Z80_PIN_MREQ 25
#define FTVC_Z80_PIN_IORQ 26
#define FTVC_Z80_PIN_RD 27
#define FTVC_Z80_PIN_WR 28
#define FTVC_Z80_PIN_HALT 29
#define FTVC_Z80_PIN_INT 30
#define FTVC_Z80_PIN_NMI 32
#define FTVC_Z80_PIN_RFSH 34

typedef struct ftvc_z80_regs {
    uint16_t af, bc, de, hl, ix, iy, sp, pc, wz;
    uint16_t af2, bc2, de2, hl2;
    uint8_t i, r, im;
    bool iff1, iff2;
    uint8_t q; /* Q register: F if the last instruction wrote F, else 0 */
} ftvc_z80_regs;

/* The decoder's own state beside the registers, for snapshots (design/media.md "Snapshots"). */
typedef struct ftvc_z80_internal {
    uint16_t step, addr, pc;
    uint8_t dlatch, opcode, hlx_idx;
    bool prefix_active, qw;
    uint64_t pins, int_bits;
} ftvc_z80_internal;

uint64_t ftvc_z80_init(void* state);
uint64_t ftvc_z80_tick(void* state, uint64_t pins);
uint64_t ftvc_z80_prefetch(void* state, uint16_t pc);
bool ftvc_z80_opdone(const void* state);
void ftvc_z80_get_regs(const void* state, ftvc_z80_regs* out);
void ftvc_z80_set_regs(void* state, const ftvc_z80_regs* in);
void ftvc_z80_get_internal(const void* state, ftvc_z80_internal* out);
void ftvc_z80_set_internal(void* state, const ftvc_z80_internal* in);

#ifdef __cplusplus
}
#endif

#endif
