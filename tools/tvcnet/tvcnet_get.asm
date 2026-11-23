; SPDX-License-Identifier: GPL-3.0-or-later
; TVCnet GET test: GET a URL, then NEXT until END; the body goes to 7600h
        DEVICE ZXSPECTRUM48
PORT05  equ 0B12h
BUF     equ 7000h
RBUF    equ 7400h
DEST    equ 7600h
THRESH  equ 26
BITN    equ 77

        org 5000h
start:  di
        xor a
        ld (timeout),a
        ld (rounds),a
        ld hl,DEST
        ld (dst),hl
        ld a,10h            ; GET, bit 0
        ld hl,url
        ld b,urlend-url
        call send_req
        ld hl,BUF
        call recv_frame
        ld a,80h
        ld (tog),a
nloop:  ld hl,rounds
        inc (hl)
        jr z,finish
        xor a
        ld (timeout),a
        ld a,(tog)
        or 11h              ; NEXT
        ld b,0
        call send_req
        ld hl,RBUF
        call recv_frame
        ld a,(timeout)
        or a
        jr nz,nloop         ; lost reply: ask again with the same bit
        ld a,(RBUF+1)
        and 7Fh
        ld (laststat),a
        ld a,(RBUF)
        or a
        jr z,.nocopy
        ld c,a
        ld b,0
        ld hl,RBUF+2
        ld de,(dst)
        ldir
        ld (dst),de
.nocopy:
        ld a,(tog)
        xor 80h
        ld (tog),a
        ld a,(laststat)
        cp 3
        jr z,nloop          ; DATA
        cp 2
        jr z,nloop          ; BUSY: a new request, so the bit flips too
finish: ld a,(laststat)
        ld (BUF+0F0h),a
        ld hl,(dst)
        ld (BUF+0F2h),hl
        ld a,(rounds)
        ld (BUF+0F4h),a
done:   jr done

; A: CTRL, HL: data, B: length
send_req:
        ld (rctrl),a
        ld a,b
        ld (rlen),a
        ld (rdata),hl
        ld a,(rlen)
        ld c,a
        ld a,(rctrl)
        add a,c
        ld c,a              ; checksum so far
        ld a,b
        or a
        jr z,.sum
.s:     ld a,(hl)
        add a,c
        ld c,a
        inc hl
        djnz .s
.sum:   ld a,c
        ld (rchk),a
        ld a,0A5h
        call send_byte
        ld a,(rlen)
        call send_byte
        ld a,(rctrl)
        call send_byte
        ld a,(rlen)
        or a
        jr z,.chk
        ld b,a
        ld hl,(rdata)
.d:     ld a,(hl)
        inc hl
        push bc
        push hl
        call send_byte
        pop hl
        pop bc
        djnz .d
.chk:   ld a,(rchk)
        jp send_byte

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
        call getbyte        ; LEN
        ld (hl),a
        inc hl
        ld (rxsum),a
        push af
        call getbyte        ; CTRL
        ld (hl),a
        inc hl
        call addsum
        pop af
        or a
        jr z,.chk
        ld d,a
.data:  push de
        call getbyte
        pop de
        ld (hl),a
        inc hl
        call addsum
        dec d
        jr nz,.data
.chk:   call getbyte        ; CHK
        ld (hl),a
        ld e,a
        ld a,(rxsum)
        cp e
        ret z
        ld a,3
        ld (timeout),a      ; a bad checksum counts as a lost reply
        ret

addsum: push bc
        ld c,a
        ld a,(rxsum)
        add a,c
        ld (rxsum),a
        pop bc
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
rxsum:  db 0
rounds: db 0
tog:    db 0
laststat: db 0
rctrl:  db 0
rlen:   db 0
rchk:   db 0
rdata:  dw 0
dst:    dw 0
url:    db "http://127.0.0.1:8765/hello.txt"
urlend:
        SAVEBIN "tvcnet_get.bin", start, $ - start
