# TVC media file formats

The file formats of TVC programs, tapes, disks, cartridges and snapshots. Sources: [tvc-hardware.md](tvc-hardware.md) and its books, freeTVC's disassembly of the ROMs ([rom.md](rom.md)), a survey of CAS, cartridge and disk files of TVC programs in circulation, and MAME's `src/lib/formats/tvc_cas.cpp` and `tvc_dsk.cpp` (github.com/mamedev/mame, BSD-3-Clause, read as documentation).

## CAS — program file (VT-DOS on-disk format, de facto exchange format)

128-byte UPM header:

| Offset | Content |
|---|---|
| 00 | file type: 01h buffered, **11h unbuffered** (MAME identifies CAS by 11h) |
| 01 | copy-protect (unreliable in the wild, see below) |
| 02–03 | number of **full** 128-byte records, LE = floor(size / 128) |
| 04 | bytes in the trailing partial record, 0–127 = size mod 128 |
| 05–7F | 00 |

Unbuffered files then have the 16-byte program header (the same as the tape header block):

| Offset | Content |
|---|---|
| 80 | 00 |
| 81 | type: 01 program, 00 ASCII/data |
| 82–83 | payload length, LE |
| 84 | autorun (non-zero, typically FFh = autostart) |
| 85–8E | 00 |
| 8F | version |
| 90… | payload; program images load at **19EFh** |

So file size = records·128 + byte 04. Example: BoingBall.cas = 128 + 16 + 0x3144 bytes, records 0x63, last 0x54, so 99·128 + 84 = 12 756 = file size. Nyari_Tabor_tomoritett.cas = 19 072 B = 149·128 has records 149 and last 0; this is the case that rules out "records − 1". YATE builds the header the same way.

Survey of 112 `.cas` files of TVC programs:
- Always as documented: byte 00 = 11h, 05–7F = 0, 80 = 0, 81 = 01 (no data-type samples), 85–8E = 0.
- 84 is FFh in 74 files and 00h in 38. 8F (version) is 00, 01 or 02.
- The payload length at 82–83 equals size − 144 in 110 files. The exceptions are GOMEX.CAS and tvc4snakes.cas.
- **Record counts are often wrong**: only 79 of 112 match the size. Some count without the 16-byte program header; some are short by 164 bytes; three have 0/0.
- 15 files have junk in bytes 01–04. So a loader must not trust the record count or byte 01, and should use the file size and 82–83.
- Wrong record counts break real-tape conversions (error 231; the `CasRepair` tool fixes them). MAME's quickload checks only byte 00 = 11h, then loads from offset 90h to 19EFh.
- The ROM's own check (SYS 2.2 `LOAD`, E4E8h → E5C1h; [rom.md](rom.md#6-basic-layer) §6): it reads the 16-byte program header and raises BASIC error 10h ("Bad file") unless **byte 80 = 00h and byte 81 = 01h**. It then reads the payload length from 82–83 and autostarts if 84 ≠ 0. `SAVE` clears the header and sets only 81–84. No 2.2 code addresses 85–8F (19E4h–19EEh), so the "version" byte 8F seen as 01h/02h in some files comes from other writers. A file with junk in byte 81 loads in an emulator that injects the payload, but not through the ROM.

## Tape signals

Bit encoding and block/sector/CRC structure: [tvc-hardware.md](tvc-hardware.md#8-cassette) §8. Containers:
- **WAV**: plain PCM; WinTVC also reads and writes an unofficial **1-bit WAV**, with 8 samples per byte, MSB first. WinTVC's 8-bit input uses a comparator with hysteresis of ± the configurable level around 80h (static analysis of WinTVC 1.4.1).
- **TTP**: WinTVC's byte-level tape stream without sync tones, used through ROM traps (standard ROMs only), with an automount folder mode.
  - From static analysis of WinTVC 1.4.1 (its TTP traps): the file starts with the 4-byte magic `TTP0`. WinTVC rejects any other start with tape error E9h.
  - After the magic come the raw bytes that the EXT ROM's byte-read/byte-write routines would move: the ROM's tape blocks ([tvc-hardware.md](tvc-hardware.md#8-cassette) §8) back to back, each from its 00h 6Ah, without leader, sync or bit encoding.
  - **Confirmed** from `cas2ttp.pas` in `casttpwav.zip` (https://tvc.homeserver.hu/bin/util/casttpwav.zip, read as documentation). That converter writes **257** data bytes in a full sector (`for i:=0 to 256`, with the CRC over all of them), so it reads one byte ahead per sector. freeTVC's reader takes 256 or 257 bytes, whichever matches the CRC, and cuts the program to its header's length (`core/media/tape_signal.cpp`, test `core.tape`). No `.ttp` made by WinTVC itself was available.

## DSK / IMG — disk images

- Raw sector dump: 80 tracks × 9 sectors × 512 B, 1 side (368 640 B) or 2 sides (737 280 B), MFM double density.
- VT-DOS: PC-compatible FAT12 (samples report "DiskMgr1"/"WINIMAGE" boot sectors, media F8h/F9h), so PC tools (WinImage, DiskMgr, mtools) work. Files on disk are CAS images.
- UPM uses a CP/M-like layout ❓.
- MAME gap values: gap3 100, gap2 22, gap1 20 (unverified).

## CRT / ROM — cartridges

- Raw ≤ 16 KiB image (FFh padded); `MOPS` at offset 0, entry point at C004h (cart mapped to page 3 at boot).
- Samples: Tetris.crt (`MOPS` + 12 × 00 (NOPs) + code), Invaders.crt (`MOPS` + code directly).
- The cartridge dumps on tvc.homeserver.hu (`/bin/rom/`): 8 and 16 KiB images; the 4 KiB Debugger cart starts `JP C086h` without `MOPS`; Végteszt is two 4 KiB EPROMs back to back; `.128`/`.32` suffixes give the EPROM size in kbit. Zozo's compiled carts (the site's archive folder `2019/Zozo/`, with `cart.asm`) are unpadded images of 4–15 KiB whose loader copies a BASIC program to 19EFh.
- Larger images are paged carts (MegaCart, MultiCart, KiloCart, GameCard ROM) with no header that says so: freeTVC goes by size (up to 64 KiB KiloCart, larger MegaCart) unless a type is given ([media.md](../design/media.md)).

## Snapshots

- No standard.
- WinTVC `.tvs`: see [WinTVC `.tvs`](#wintvc-tvs) below.
- ep128emu: chunked; its TVC memory chunk stores the paging word, RAM segments and the 4 KiB VT-DOS RAM.
- rtvc: `.rtvcsnap`, chunked.
- freeTVC's own format is TVCS ([media.md](../design/media.md)); freeTVC also imports `.tvs` files.

### WinTVC `.tvs`

WinTVC 1.4.1's snapshot. The layout was found by saving snapshots of probe programs in WinTVC (each register loaded with a distinct value) and comparing the files with the emulator's state. freeTVC's importer is `src/core/snapshot/tvs_import.*`.

The file has a fixed size of 99 375 (1842Fh) bytes; WinTVC rejects any other size. Sections carry 4-character tags at fixed offsets (it is not a free chunk list):

| Offset | Tag | Bytes | Content |
|---|---|---|---|
| 00000h | `TS01` | – | magic |
| 00004h | `CPU ` | 5Bh | Z80 state (below) |
| 00063h | `IOUT` | 100h + 4 | the last value written to each port 00h–FFh, then 4 bytes of other state ❓ |
| 0016Bh | `IOIN` | 100h | per-port input values (WinTVC's loader ignores them) |
| 0026Fh | `BRAM` | 10000h | 64 KiB RAM, U0–U3 |
| 10273h | `VRAM` | 8000h | 32 KiB from the **displayed** VRAM bank on: on a 64k+ that bank and the next (with bank 3 displayed, the second half is U0 0000–3FFFh), so a 64k+ snapshot loses two VRAM banks |
| 18277h | `CRTC` | 6Ch | decoded 6845 fields as 32-bit words, the raw registers R0–R15 at +48h–+57h, then zeros; no beam counters |
| 182E7h | `EVTS` | 128h | WinTVC's internal event list: host pointers and "never" times, identical in every file; carries no machine state |
| 18413h | `IRQ ` | 18h | interrupt state (below) |

`CPU ` section (16-bit values low byte first, so +00h is F):

| Offset | Field |
|---|---|
| +00h … +0Eh | AF, BC, DE, HL, IX, IY, SP, PC |
| +10h … +16h | AF', BC', DE', HL' |
| +18h, +19h | I, R |
| +1Ah, +1Bh | IFF1, IFF2 |
| +1Ch | interrupt mode |
| +1Dh | the CPU's INT input |
| +29h | 64-bit T-state counter |
| +31h | CPU clock in Hz (3 125 000) |

A HALT leaves PC on the HALT opcode. +1Eh–+28h and +35h–+5Ah were 0 in every file ❓.

`IRQ ` section (32-bit words): +00h slot pending bits, +04h cursor/sound flag (bit 4), +08h a second flag word, also cleared by port 07h ❓, +0Ch the sound IRQ enable (05h b5), +10h slot enables, +14h a mask that hides +04h ❓. INT = (+14h = 0 and +04h ≠ 0) or (+00h & +10h & Fh) or +08h ≠ 0.

WinTVC's loader writes each of ports 00h–06h first inverted and then with the saved value, so every side effect fires, then restores RAM, VRAM, the 6845 (R0–R15 rewritten) and the IRQ block. A `.tvs` stores no model, ROM choice, card, disk or cartridge state; WinTVC saves it while paused, at a 20 ms boundary of its own, not at a frame start.

## Other

- **BAS**: plain-text BASIC (YATE supports it).
- **SIS**: SID register dumps played by the TVC256k++ jukebox ❓ layout.
- Symbol and source files freeTVC's debugger reads: plain `label = addr` (or `EQU`) text, sjasmplus `--sym`/`--exp` symbols, sjasmplus SLD and `--lst` listings (what YATE calls an SJASM list), SDCC `.noi`, `.cdb` and `.map` files, z88dk maps ([debug-and-remote.md](../design/debug-and-remote.md)).
