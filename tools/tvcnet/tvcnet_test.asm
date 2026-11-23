; SPDX-License-Identifier: GPL-3.0-or-later
; TVCnet loop test: PING and ECHO "HELLO" over 05h b7 (UART 2400 8N1) and 59h b5 (half-period code, T 250 us)
        DEVICE ZXSPECTRUM48
PORT05  equ 0B12h
BUF     equ 7000h
THRESH  equ 26
BITN    equ 77

        org 5000h
start:  di
        xor a
        ld (timeout),a
        ld hl,req_ping
        call send_frame
        ld hl,BUF
        call recv_frame
        ld hl,req_echo
        call send_frame
        ld hl,BUF+40h
        call recv_frame
        ld a,(timeout)
        ld (BUF+0F0h),a
done:   jr done

; HL: count, then the frame's bytes
send_frame:
        ld b,(hl)
        inc hl
.next:  ld a,(hl)
        inc hl
        push bc
        push hl
        call send_byte
        pop hl
        pop bc
        djnz .next
        ret

send_byte:
        ld c,a
        ld a,(PORT05)
        and 3Fh
        ld d,a              ; line high: motor bit 0
        or 80h
        ld e,a              ; line low: motor bit 1
        ld a,e
        out (05h),a         ; start bit
        call bitdelay
        ld b,8
.bit:   rr c
        ld a,d
        jr c,.one
        ld a,e
.one:   out (05h),a
        call bitdelay
        djnz .bit
        ld a,d
        out (05h),a         ; stop bit
        call bitdelay
        ret

bitdelay:
        ld h,BITN
.l:     dec h
        jr nz,.l
        ret

; HL: where the frame goes (LEN, CTRL, data, CHK)
recv_frame:
        in a,(59h)
        and 20h
        ld b,a
        ld de,0
.w0:    in a,(59h)
        and 20h
        cp b
        jr nz,.first
        dec de
        ld a,d
        or e
        jr nz,.w0
        ld a,1
        ld (timeout),a
        ret
.first: ld b,a
        ld c,0
.sync:  call halfbit
        rr c
        ld a,(timeout)
        or a
        ret nz
        ld a,c
        cp 6Ah
        jr nz,.sync
        call getbyte
        ld (hl),a
        inc hl
        add a,2             ; CTRL, data, CHK
        ld d,a
.rest:  push de
        call getbyte
        pop de
        ld (hl),a
        inc hl
        dec d
        jr nz,.rest
        ret

getbyte:
        ld e,8
.b:     push de
        call halfbit
        pop de
        rr c
        dec e
        jr nz,.b
        ld a,c
        ret

; Time to the next edge: CY = 1 for a short half-period
halfbit:
        ld e,0
.w:     inc e
        jr z,.to
        in a,(59h)
        and 20h
        cp b
        jr z,.w
        ld b,a
        ld a,e
        cp THRESH
        ret
.to:    ld a,2
        ld (timeout),a
        or a
        ret

timeout: db 0
req_ping: db 4, 0A5h, 00h, 01h, 01h
req_echo: db 9, 0A5h, 05h, 82h, "HELLO", 0FBh

        SAVEBIN "tvcnet_test.bin", start, $ - start
