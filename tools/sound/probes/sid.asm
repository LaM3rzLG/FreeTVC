; SPDX-License-Identifier: GPL-3.0-or-later
; SID card check: about 7 s of notes on ports E0h-FFh, the same program for WinTVC and freeTVC
; (tools/sound/sid_check.py). Interrupts off, no ROM calls, so it also runs straight from power-on. Starts when GO is
; non-zero; RES gets OSC3 and ENV3 sampled at the end (WinTVC's ports read FFh).
        INCLUDE "common.inc"

SID     EQU 0xE0

    MACRO SIDW reg, value
        LD A,value
        OUT (SID + reg),A
    ENDM

    MACRO FREQ voice, f
        SIDW 7 * voice + 0, low f
        SIDW 7 * voice + 1, high f
    ENDM

    MACRO WAIT ms
        LD HL,ms
        CALL waitms
    ENDM

        ORG 0x6000
R_ENTRY_START:
ENTRY:  DI
        LD SP,STACK
.wait   LD A,(GO) : OR A : JR Z,.wait
        LD BC,25 * 256 + SID
        XOR A
.clear  OUT (C),A
        INC C
        DJNZ .clear
        SIDW 0x18, 0x0F
        ; sawtooth A4 (440 Hz at 985 248 Hz: F = 7493), attack 2 ms, decay 750 ms, sustain 0: a pluck
        FREQ 0, 7493
        SIDW 5, 0x09
        SIDW 6, 0x00
        SIDW 4, 0x21
        WAIT 600
        SIDW 4, 0x20
        WAIT 200
        ; pulse C5, width 50 %, sustain 15, release 6 ms
        FREQ 0, 8910
        SIDW 2, 0x00
        SIDW 3, 0x08
        SIDW 5, 0x00
        SIDW 6, 0xF0
        SIDW 4, 0x41
        WAIT 500
        SIDW 4, 0x40
        WAIT 100
        ; triangle E5
        FREQ 0, 11226
        SIDW 4, 0x11
        WAIT 500
        SIDW 4, 0x10
        WAIT 100
        ; noise, release 300 ms
        FREQ 0, 0x4000
        SIDW 6, 0xF9
        SIDW 4, 0x81
        WAIT 300
        SIDW 4, 0x80
        WAIT 400
        ; chord: sawtooth A3, pulse C4, triangle E4 through the resonant low-pass, cut-off swept up over 2 s
        FREQ 0, 3746
        FREQ 1, 4455
        FREQ 2, 5613
        SIDW 9, 0x00
        SIDW 10, 0x08
        SIDW 5, 0x00
        SIDW 12, 0x00
        SIDW 19, 0x00
        SIDW 6, 0xF0
        SIDW 13, 0xF0
        SIDW 20, 0xF0
        SIDW 0x15, 0x00
        SIDW 0x16, 0x00
        SIDW 0x17, 0xF7
        SIDW 0x18, 0x1F
        SIDW 4, 0x21
        SIDW 11, 0x41
        SIDW 18, 0x11
        LD E,0
.sweep  LD A,E
        OUT (SID + 0x16),A
        WAIT 8
        INC E
        JR NZ,.sweep
        SIDW 4, 0x20
        SIDW 11, 0x40
        SIDW 18, 0x10
        WAIT 200
        SIDW 0x17, 0x00
        SIDW 0x18, 0x0F
        ; ring modulation: triangle A4 on voice 1 ringed by voice 3 at 120 Hz
        FREQ 2, 2043
        FREQ 0, 7493
        SIDW 4, 0x15
        WAIT 600
        SIDW 4, 0x14
        WAIT 100
        ; hard sync: sawtooth on voice 1 synced to voice 3 (A3), its own pitch swept up
        FREQ 2, 3746
        LD DE,7493
        SIDW 4, 0x23
        LD B,100
.sync   LD A,E
        OUT (SID + 0),A
        LD A,D
        OUT (SID + 1),A
        LD HL,64
        ADD HL,DE
        EX DE,HL
        PUSH BC
        WAIT 8
        POP BC
        DJNZ .sync
        SIDW 4, 0x22
        WAIT 100
        ; voice 3 noise, silent through 18h b7 (3 OFF), sampled through OSC3; then voice 3's envelope through ENV3
        SIDW 0x18, 0x8F
        FREQ 2, 0x2000
        SIDW 18, 0x80
        LD HL,RES
        LD B,8
.osc    IN A,(SID + 0x1B)
        LD (HL),A
        INC HL
        PUSH HL
        PUSH BC
        WAIT 1
        POP BC
        POP HL
        DJNZ .osc
        SIDW 19, 0x40
        SIDW 20, 0x00
        SIDW 18, 0x81
        LD B,8
.env    IN A,(SID + 0x1C)
        LD (HL),A
        INC HL
        PUSH HL
        PUSH BC
        WAIT 4
        POP BC
        POP HL
        DJNZ .env
        SIDW 18, 0x80
        SIDW 0x18, 0x00
ENDSEQ: FINISH

; HL milliseconds (3125 T each, within 0.2 %)
waitms: LD BC,119
.inner  DEC BC
        LD A,B
        OR C
        JR NZ,.inner
        DEC HL
        LD A,H
        OR L
        JR NZ,waitms
        RET
R_ENTRY_END:

        ORG RESBASE
R_RES_START:
DONE:   DB 0
GO:     DB 0
RES:    DS 16
RES_END:
R_RES_END:

        SAVEBIN IMAGE,0,0x10000
