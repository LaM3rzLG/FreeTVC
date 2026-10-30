; SPDX-License-Identifier: GPL-3.0-or-later
; DeZog test for freeTVC's DZRP server; build: sjasmplus --sld=hello.sld --fullpath hello.asm (tools/dezog/README.md)

        DEVICE ZXSPECTRUM48     ; only the 4 x 16 K layout; DeZog maps its labels onto the initial TVC banks
        SLDOPT COMMENT WPMEM, LOGPOINT, ASSERTION
        ORG 6000h

start:  ld hl,message
next:   ld a,(hl)
        or a
        jr z,count
        ld c,a
        call print
        inc hl
        jr next

count:  ld hl,counter
        inc (hl)
        jr count

print:  push hl
        rst 30h
        db 21h                  ; ED_CHOUT: C = character (rom.md §5)
        pop hl
        ret

message: db "HELLO FROM DEZOG", 0
counter: db 0

        SAVEBIN "hello.bin", start, $ - start
