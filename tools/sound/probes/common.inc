; SPDX-License-Identifier: GPL-3.0-or-later
; Shared layout for the probe programs (sid.asm): loaded with --load and started with --pc at ENTRY,
; which disables interrupts; they assume port 02h = 70h (U0, U1, U2, SYS).
        DEVICE NOSLOT64K

RESBASE EQU 0x6800              ; results area in U1
STACK   EQU 0x6F00

; port 02h values: b7-6 page 3, b5 page 2, b4-3 page 0, b2 page 1
P3_CART EQU 0x00
P3_SYS  EQU 0x40
P3_U3   EQU 0x80
P3_IOM  EQU 0xC0
P2_VID  EQU 0x00
P2_U2   EQU 0x20
P0_SYS  EQU 0x00
P0_CART EQU 0x08
P0_U0   EQU 0x10
P0_11   EQU 0x18
P1_U1   EQU 0x00
P1_VID  EQU 0x04

    MACRO FINISH
        LD A,0xA5
        LD (DONE),A
.hang   JR .hang
    ENDM
