# TVC hardware reference

The TVC's hardware as the books, schematics, ROMs and other emulators describe it: models, CPU timing and the video RAM clock stretch, paging, I/O ports, video, sound, keyboard, cassette and expansion cards. It is the reference that freeTVC's emulation cites; how freeTVC implements each part is in [machine.md](../design/machine.md), [media.md](../design/media.md) and [cards.md](../design/cards.md).

The claims were audited against [HW] and [OS] (the text and, for fig. 9, 10 and 20, the scans), MAME, ep128emu, YATE and the SYS 2.2 / EXT 2.x ROM; claims with no source found say so. Nothing here is verified against real hardware. WinTVC's clock stretch is only an approximation (a flat +1 T), so it is not a timing reference. **⚠ CONFLICT** = sources disagree, **❓** = unknown, **⚑** = needs a dynamic check. Emulated behaviour should cite a section here (or add one).

Sources by authority:
- **[HW]** Benedek–Monok, *TV-COMPUTER HARDWARE* (Novotrade, 1988), 254 pages; scan at https://tvc.homeserver.hu/doc/konyvek/tvchardver.pdf (its drawings: https://tvc.homeserver.hu/html/tvc_hardvertervrajzok.html). Page numbers are pages of that PDF.
- **[OS]** Garai Géza, *TV-Computer Operációs rendszer* (Novotrade, 1987); scan at https://tvc.homeserver.hu/doc/konyvek/operaciosrendszer.pdf. "PDF p." numbers are pages of that PDF; § numbers are the book's chapters.
- **[PG]** *TV-Computer Programming Guide* (version of 9 February 2023), https://tvc.homeserver.hu/doc/konyvek/TV-Computer%20Programming%20Guide%20-%20Feb%209%202023.pdf.
- **[BZ]** Bertók Zsolt, *Grafika és hang programozása gépi kódban* (unfinished version of 23 February 2023), https://tvc.homeserver.hu/doc/konyvek/TVC_Grafika_Gepi_kodban_2023.februar_23.pdf.
- Also from tvc.homeserver.hu: *A TVC 64K+ többletszolgáltatásai a TVC 64K-hoz képest* (`doc/konyvek/tvc64kplus_bw.pdf`), *TVC Biblia* (`doc/konyvek/tvcbiblia.pdf`, also MEK 03604, https://mek.oszk.hu/03600/03604/), *Memóriabővítés TV-Computerhez* (`doc/konyvek/32krambovites.pdf`), the HBF disk card documents (`doc/tervrajzok/hba2/floppydoc.zip`), the HBA-2 schematics (`doc/tervrajzok/TVC_HBA2_rajzok.pdf`) and the site's hardware pages (`html/tvc_*.html`, cp1250). The site's TLS chain is incomplete, so some clients need `curl -k`; freeTVC's own downloads are pinned by SHA-256 either way.
- Code, read as documentation (no code copied): **[MAME]** github.com/mamedev/mame (BSD-3-Clause): `src/mame/videoton/tvc.cpp`, `src/devices/bus/tvc/`, `src/lib/formats/tvc_cas.cpp` and `tvc_dsk.cpp`; line numbers refer to the source of October 2026. **[EP]** ep128emu 2.0.11.2 (github.com/istvan-v/ep128emu, GPL-2.0-or-later) and the libretro/ep128emu-core branches `core` (`6af5d03`), `standalone` (`1edc033`), `tvc_gamecard` (`b7b9755`) and `devtool` (`ae41308`). **[YATE]** github.com/hightower70/YATE at `8d9eff4` (GPL-3 repository, LGPL-2.1+ file headers).
- **[256K]** dikdom's TVC256k++ sources and manual, github.com/dikdom/TVC256kplusplus at commit `4580b14` (firmware v0.24). This is the card's own implementation, so it is the authority for that card (§9).
- **[WinTVC]** behaviour of WinTVC 1.4.1 from static analysis (Grósz Attila, freeware, https://tvc.homeserver.hu/bin/emu/WinTVC1.4.1.zip), some of it checked by runs under Wine; only behaviour is recorded, no code is taken. It records an emulator's choices, not hardware facts; static readings are marked ⚑.
- **[ROM]** freeTVC's disassembly of SYS 2.2 / EXT 2.x ([rom.md](rom.md), symbols in [rom-symbols/](rom-symbols/)). It shows what the original software expects of the hardware.

## 1. Models, boards, ROMs

| Model | Type no. | Board | User RAM | Video RAM | Notes |
|---|---|---|---|---|---|
| TVC 32k | 49100 | HBA | 32 KiB (U0, U1) | 16 KiB | U2/U3 only with the HBM 32K RAM card (49102); with it U3 can map to page 0 |
| TVC 64k | 49101 | HBA-1 | 64 KiB | 16 KiB | **U3 cannot be mapped to page 0** (`11` in page-0 field gives U0); VT-DOS needs a 74LS02 mod |
| TVC 64k+ | 49108 | HBA-2 | 64 KiB | 64 KiB (4 × 16 KiB banks) | 6-bit page register (VID in page 1), video paging register 0Ch–0Fh, U3 in page 0, BASIC 2.x, **no M1 WAIT circuit**, buffered cartridge bus |
| Cyrillic | – | ❓ | 64k | ❓ | Cyrillic keyboard matrix (MAME `tvc64pru` on 64k+ hardware) |

WinTVC's "first/second series" distinction is unexplained ❓. About 3 000 × 32k and 9 000 × 64k sold by 1988.

ROMs [HW §2.2.2, §3.2.3]:
- **SYS** 16 KiB, appears in page 0 or page 3. Chips by jumper: 4 × 2732, 2 × 2764 or 1 × 27128 (HBA-2: 2 × 2764 or 1 × 27128).
- **EXT** 8 KiB socket D7, visible at E000–FFFF when page 3 = EXT. Holds part of the OS, entered through bridges (SYS→EXT `OUT (2),F0h`; EXT→SYS `OUT (2),70h`), and the RS-232 driver. The spec says "20 KiB, max 24 KiB" in total.
  - EXT 2.x is 8 KiB. **EXT 1.x is a 4 KiB ROM in the upper half of its socket** (F000–FFFF); E000–EFFF reads FFh. MAME's `tvc12_d7` is the 8 KiB socket image (FFh below F000h); WinTVC embeds the 4 KiB ROM (its image plus 4 KiB of FFh equals `tvc12_d7`, compared by CRC-32). freeTVC's `normalise_ext()` takes the upper half of such an 8 KiB image, so both forms load as the same 4 KiB ROM (CRC-32 59C6B9F9). EP mirrors a short image.
- Versions:
  - 1.1: club prototypes.
  - 1.2: production 32k/64k.
  - 1.3: mentioned in [OS], no known dump ❓.
  - "V2.00": English manual.
  - 2.1 and 2.2: 64k+.
  - Cyrillic.
- MAME image names and CRCs:
  - 1.2: `tvc12_d4` 834ca9be + `tvc12_d3` 71753d02, EXT `tvc12_d7` 1cbbeac6.
  - 2.2: `tvc22_d6` 05ac3a34 + `tvc22_d4` ba6ad589, EXT `tvc22_d7` 05e1c3a8.
  - 2.1: `tvc21_d6` f197ffce + `tvc21_d4` b054c0b2.
  - RU: `tvcru_d6` 1e0fa0b8, `tvcru_d4` bac5dd4f, `tvcru_d7` 70cde756.
- freeTVC gets its images from the original EPROM dumps on tvc.homeserver.hu through the ROM autofetch ([build.md](../design/build.md#rom-autofetch)).

## 2. CPU, clocks, wait states, interrupts

Clocks:
- 25 MHz crystal (3rd overtone), divided to 12.5, 6.25 and 3.125 MHz.
- **CPU Φ = 3.125 MHz** (Z80A), so 1 T = 320 ns.
- CRTC character clock 1.5625 MHz (640 ns per character); sound/serial divider at 3.125 MHz.
- Pixel shift clock: 12.5 MHz (2-colour), 6.25 MHz (4-colour), 3.125 MHz (16-colour).

### Video RAM contention ("clock stretching") [HW §2.2.4, fig. 20–22]

- VRAM is time-multiplexed by signal DEB: a 320 ns CPU slot, then a 320 ns CRTC slot, repeating every 640 ns. The CRTC reads one byte per 640 ns, which also refreshes the DRAM.
- On any CPU access to VID the circuit **holds Φ** for 1–4 half-periods of 160 ns. Odd stretches shift the CPU clock phase relative to DEB.
  - Memory read/write: **T3** high phase is extended; the T3 falling edge lands 160 ns after a DEB 1→0 edge.
  - M1 opcode fetch: **T2** low phase is extended; the T3 rising edge lands 160 ns after a DEB 1→0 edge.

| T1 rising edge at | Mem R/W extra | M1 extra |
|---|---|---|
| CPU slot start (DEB↑) | +2 half (1 T) | +3 half |
| +1 half | +1 half | +2 half |
| CRTC slot start (DEB↓) | +4 half (2 T) | +1 half |
| +3 half | +3 half | +4 half |

- [EP] counts half-cycles (`tvc64vm.cpp` `memoryWait`/`memoryWaitM1`); its phase 0 is the CRTC slot start. The phase `hc` is sampled **after** the fixed part has been added:
  - VRAM R/W: add 4, then `4 − (hc & 3)`, then 1. Normal memory takes 5.
  - M1: add 3, then `4 − (hc & 3)`, then 1. Normal M1 takes 4.
  - Relative to T1 the M1 extra is therefore `4 − ((hc + 3) & 3)`, which matches the table above. Taking `hc` at T1 instead would give +4 instead of +1 at the CRTC slot start.
- Not stretched: I/O cycles (CRTC and palette ports too) and non-VID memory.
- Refresh cycles: no source found ❓. The stretch logic gates on NVRAM·NMREQ ([HW] PDF pp. 53–54); what happens when I:R points into VID is unknown.
- Expansion cards *can* generate WAIT for memory and I/O cycles ([HW] PDF p. 19).
- Applies whenever VID is paged in: page 2, or page 1 on the 64k+.
- **Implication**: the CPU emulation must expose the bus-cycle sequence (M1/MR/MW/IO with T-state positions) and track the 160 ns half-clock phase. freeTVC therefore uses a T-state-stepped core ([z80-core.md](../design/z80-core.md)); the alternative is an instruction core with per-instruction bus-cycle tables, as in rtvc.

### WAIT circuit (32k/64k only) [HW §2.2.2, §3.2.5]

- Exactly +1 wait state, M1 only, and only when a slow device asks: expansion card via `/SLOWEXP`, cartridge via `/FAST`, or SYS/EXT ROM if jumpers TB1/TB9 are fitted (not in production).
- The original HBF card reportedly drives the slow line ("only used on 32k machines") ❓.
- The 64k+ has no WAIT circuit; cards can still pull `/WAIT` directly.

### Interrupts

- **IM 1** only. The OS handler at 38h sets U0, U1, U2, SYS, runs the ROM handler and exits via `INT_EXIT` (0B41h), restoring `P_SAVE` (0003h).
- During INTA the bus is not driven; [EP] assumes FFh so IM 2 software works ❓.
- Sources:
  - CRTC CURSOR output, one 640 ns pulse per frame.
  - Sound divider (end of period).
  - Four slot interrupts.
  - `/NMI` and a direct `/INT` on the expansion bus.
- Cursor and sound are **ORed into one edge-triggered flip-flop** (software cannot tell them apart); any write to port 07h clears it.
- **⚠ CONFLICT**: MAME sets the flip-flop on the cursor's rising edge, EP on the falling edge (640 ns apart).
- Slot interrupts: each slot's /INTn (pull-ups R1–R4) goes through J2 to /EXPINT and, with the cursor/sound flip-flop, to the CPU's /INT (J18, J16); port 59h b3–0 read the four lines directly through E1 (0 = pending). The board has no latch or enable: a write to 58h+n is slot n's /CLRINT strobe to the card ("IT kérésének törlése", [HW] port table pp. 125–126), and what its data bits mean is the card's (the OS writes b7). Source: HBA-2 schematic sheets 7 and 10 (tvc.homeserver.hu `doc/tervrajzok/TVC_HBA2_rajzok.pdf`, read off the rendered sheets, static ⚑). A reading of "b7 = 1 enables slot n" describes a card's register, not the board.
- OS order: cursor/sound, then CSTL0..3. `INT_DES` (0B10h, default FCh) selects which drivers run on the tick; `INT_INC` (0B1Dh) counts ticks.
- Reset: the button is synchronised to `/M1`; a double press within about 0.5 s forces a cold reset.

## 3. Memory map and paging

### Port 02h page register (write-only; 5 bits on 32k/64k, 6 bits on 64k+)

| Bits | Page | Values |
|---|---|---|
| 7–6 | Page 3, C000–FFFF | `00` CART (reset) · `01` SYS · `10` U3 · `11` EXT: C000–DFFF = IOMEM of the slot selected by port 03h b7–6, E000–FFFF = EXT ROM |
| 5 | Page 2, 8000–BFFF | `0` VID (reset) · `1` U2 |
| 4–3 | Page 0, 0000–3FFF | `00` SYS (reset) · `01` CART · `10` U0 · `11` U3 (64k+ and 32k+HBM; gives U0 on an unmodified 64k) |
| 2 | Page 1, 4000–7FFF | `0` U1 (reset) · `1` VID (64k+ only) |

- Reset value 00h = SYS, U1, VID, CART; the CPU starts at 0000h in SYS. After BASIC init it is **70h** (U0, U1, U2, SYS).
- Common values: 50h U0,U1,VID,SYS · B0h all RAM · 30h U0,U1,U2,CART · F0h U0,U1,U2,EXT.
- Convention: write `P_SAVE` (0003h) before `OUT (2)`. P_SAVE holds the whole port-02h value, and the OS rewrites port 02h (all four pages) from it ([OS] PDF pp. 16, 24).
- The `/PHANTOM` bus line disables the on-board memory page decoder, i.e. all on-board memory; I/O decoding is unaffected ([HW] PDF pp. 21, 80). The original machine does not use it; the TVC256k++ does (§9).
- Unmapped reads give FFh (YATE/EP); ROM writes are ignored. [WinTVC] differs: an empty slot reads 00h, both its IOMEM window and its ports. Only D000–DFFF writes reach a card (static ⚑).

### Port 03h

- Bits 7–6: IOMEM slot (00 = CSTL0 … 11 = CSTL3); mirror variable `PORT03` at 0B11h.
- Bits 3–0: keyboard row.
- Bits 5–4: unused.

### Ports 0Ch–0Fh video paging (64k+ only)

- 6-bit register, cleared by reset; A2/A3 gating makes 0Ch–0Fh all mirrors.
- Bits 5–4: displayed bank. Bits 3–2: bank seen in page 2. Bits 1–0: bank seen in page 1.
- **⚠ CONFLICT**: [HW] states the mirror explicitly ("0DH-0FH ugyanaz, mint a 0CH", p. 125 table; also PDF pp. 36 and 42), and EP follows it.
  - MAME, YATE and WinTVC decode only 0Fh.
  - MAME differs from [HW] in more places (`tvc.cpp` lines 164–180, 288–305): page-0 `11` selects VRAM, 02h b2 is ignored (no VID in page 1), and 0Fh b1–0 is applied to page 0.
  - freeTVC follows [HW].
- 32k/64k: one 16 KiB VRAM bank.

### Video RAM layout

- Byte (x, y) at 8000h + y·64 + x for x 0–63, y 0–239 (4000h+ when VID is in page 1).
- 240 lines use 15 360 bytes; BC00–BFFF (lines 240–255) is shown only if R6 is reprogrammed.

### OS RAM in U0

| Address | Use |
|---|---|
| 0000–002F | BASIC area (`USRTAB` 0021h) |
| 0003 | `P_SAVE` |
| 0030 | RST 30h system call; function code in the next byte |
| 0038 | IRQ entry |
| 0040–00FF | card tables at 40/70/A0/D0h: name length, name ≤ 6, unit, 40-byte buffer |
| 0100 | ASCII screen copy 64×25; row 25 is unused ([OS] PDF p. 42), so 24 usable rows (useful for `screen_text`) |
| 0740 | user character matrices for codes 128–223, 10 bytes each |
| 0B00 / 0B08 | `IN_TABLE` / `OUT_TABLE` |
| 0B11 / 0B12 / 0B13 | mirrors of ports 03 / 05 / 06 |
| 0B19 | `HI_MEM` |
| 0B1B | `P3RAM` |
| 0B21 / 0B22 | `WARM_FLAG` / `COLD_FLAG` |
| 0B4F | `BORDER` |
| 0B51 | `PICTURE` (10-byte keyboard matrix) |
| 0B6B–0B70 | tape variables (`BUFFER`, `REMRED`, `PROTECT`, `EOF`, `MUDDLE`) |
| 0B71 | `SER_OK` |
| 0EAC | 2 KiB stack |
| 19EF | BASIC program start |

### Boot [OS §6]

1. Init the CRTC.
2. Warm/cold decision.
3. Test U0 (border flashes on error).
4. Test VRAM (64k+: first 16 KiB only; red border on error).
5. Test U1–U3.
6. Copy RAM routines.
7. Read 5Ah. Cards with a known ID (00 RS-232, 10 floppy) get their name-table entry and their ROM init; the RS-232 init lives in EXT. The OS probes IOMEM for `MOPS` only when the ID is 11 ([OS] PDF pp. 27, 46–47).
   - [ROM] confirms this and adds ID 01 = name "VGB" (unknown card ❓). Every card with a non-00 ID gets its init called through C00Bh ([rom.md](rom.md#3-boot-reset--cartridge-or-basic) §3, step 11).
8. If a floppy card is present, redirect the cassette class to it.
9. Map SYS in page 0 and CART in page 3; if C000h holds `MOPS`, jump to **C004h**, otherwise start BASIC.

### Card IOMEM ROM header (C000h)

`MOPS` · C004 name length · C005 name (≤ 6) · C00B init addr · C00D function count · C00E IRQ routine · C010 char I/O · C012 block I/O · ….

[EP] `tvcfileio.s` (standalone branch) fills in the rest of the table:
- C014 open, C016 close, C018 verify, then extra entries; it declares 15 functions.
- These are the CAS functions called through RST 30h. Function code: b7 = 1 in / 0 out, b6–4 = 101, b3–0 = index.
- D0/50 IRQ, D1/51 CHIN/CHOUT, D2/52 BKIN/BKOUT, D3/53 OPEN/CREATE, D4/54 CLOSE, D5 VERIFY.
- Floppy extensions: D6–D8 seek (absolute / relative / to end; new position in BCDE), D9–DB CLI buffer / FISH.
- TVC256++ reuses D6–DE: GET_PWD/CH_DIR, OPEN_DIR, READ_DIR/CLOSE_DIR, MK_DIR, DELETE, RENAME, SEEK, GET_IOBASE, GET_MEMBASE.
- The init routine computes the slot from IX (slot = (IX & F0h − 10h) >> 6) and writes 80h | slot to 0B05h/0B0Dh.

Cross-checked against [ROM] and [OS] ([rom.md](rom.md#4-system-calls-rst-30h) §4):
- The EXT dispatcher reads the count at C00Dh, rejects a function ≥ count with FFh, and calls the word at C00Eh + 2·function. It enters the handler with **IX = 0048h + 30h·slot**, the card's buffer in its name-table entry. That is the IX the formula above relies on.
- 0B05h/0B0Dh are `IN_TABLE`/`OUT_TABLE` entry 5, the cassette class. A device byte with bit 7 set sends **every** function of the class to the card. A built-in device would get only functions 0–2 redirected ([OS] §4.3).
- So D6h–DEh exist only on cards. The built-in cassette device has 6 functions, so D6h and above return FFh.

## 4. I/O ports

- Only A0–A7 are decoded and **A7 must be 0**.
  - **⚠** [HW] is inconsistent: "only 00–7Fh used" (PDF pp. 33, 34) vs. "80h–9Fh R/W reserved for the video game module" (p. 126 table; the module was never made ❓).
  - A0h–FFh are unused.
  - [EP] registers handlers only for 00–7Fh (`tvc64vm.cpp` lines 1342–1347), so 80h–FFh are unmapped there as well. WinTVC sends E0h–F8h to its SID device (static analysis); freeTVC's SID card takes E0h–FFh there ([cards.md](../design/cards.md)).
- INTA cycles are excluded from decoding. Unmapped reads give FFh (MAME).

| Port | Dir | Function |
|---|---|---|
| 00h | W | border: b7 I, b5 G, b3 R, b1 B; OS rewrites it from `BORDER` every IRQ |
| 01h | W | Centronics data latch |
| 02h | W | page register |
| 03h | W | b7–6 IOMEM slot, b3–0 keyboard row |
| 04h | W | sound pitch low 8 bits |
| 05h | W | b3–0 pitch bits 11–8 · b4 tone enable (1 = on) · b5 sound IRQ enable · b6 tape motor 1 · b7 tape motor 2 |
| 06h | W | b1–0 video mode (00 2-col, 01 4-col, 1x 16-col) · b5–2 amplitude · b7 printer `/STROBE` · b6 unused ([HW] p.125, table 6) |
| 07h | W | clear cursor/sound IRQ flip-flop |
| 08h–0Bh | – | unused per [HW] (the HBF card's EPROM page register is at slot base + 8, §9) |
| 0Ch–0Fh | W | video paging (64k+) |
| 10h–1Fh / 20h–2Fh / 30h–3Fh / 40h–4Fh | R/W | slot 0 / 1 / 2 / 3 (slot 0 is the right-most connector) |
| 50h–57h | R/W | any access toggles the tape-output flip-flop |
| 58h (5Ch) | R | keyboard columns of the selected row, active low |
| 58h–5Bh (5Ch–5Fh) | W | slot n's /CLRINT strobe to the card; the data bits mean what the card makes of them (the OS writes b7; §2 "Interrupts") |
| 59h (5Dh) | R | b3–0 slot IRQ pending (0 = pending) · b4 cursor/sound IRQ (0 = pending) · b5 tape in · b6 colour switch (1 = colour) · b7 printer ACK |
| 5Ah (5Eh) | R | card IDs, 2 bits per slot (b1–0 slot 0 … b7–6 slot 3): 00 RS-232, 01 unused ([HW] table 5; [OS] says it was once reserved for the video game board, which must report 11), 10 floppy, 11 empty/unidentified |
| 5Bh (5Fh) | R | `INSTART`: reset the sound divider chain |
| 60h–63h (mirrored to 6Fh) | W | palette 0–3: b6 I, b4 G, b2 R, b0 B (74LS670) |
| 70h (even 70–7E) | W | 6845 address |
| 71h (odd 71–7F) | R/W | 6845 data |

**⚠** 05h b4: [PG] says "1 = disable"; [HW] table 4 (p. 134), [OS], [BZ], MAME and EP say 1 = enable, and reset disables. freeTVC follows the majority. 05h b5 is the *sound* IRQ enable ([HW] table 8), not a cursor-IRQ enable.

## 5. Video

### Pixel formats (64 bytes per line in every mode)

- **2-colour** 512×240: bit 7 is the leftmost pixel; the bit selects palette 0/1.
- **4-colour** 256×240: pixel k (0 = left) has index C1:C0 = (bit 3−k, bit 7−k), so bits 7/3 make the leftmost pixel and bits 4/0 the rightmost. Indexes palette 0–3. Interleaved per [HW] table 2, [OS], [PG] and MAME.
- **16-colour** 128×240: left pixel I(b7) G(b5) R(b3) B(b1), right pixel I(b6) G(b4) R(b2) B(b0); direct colour, no palette.
- Formats: palette `-I-G-R-B` (bits 6, 4, 2, 0), border `I-G-R-B-` (bits 7, 5, 3, 1).

### Colour levels

- [OS]: component = C·(4/7 + I·3/7), so dark ≈ 146/255 and bright 255; two blacks (I = 0 and I = 1).
- MAME uses 7Fh/FFh ❓.
- The rear B/W switch kills the PAL subcarrier; readable at 59h b6.

### 6845 usage

- One character = one byte; one character row = **4 TV lines** (R9 = 3).
- VRAM address = `(MA & 3Fh) | (RA & 3) << 6 | ((MA >> 6) & 3Fh) << 8`. MAME's `ma*4 + ra*0x40` is correct only when the start address is a multiple of 64.
- One R12/R13 step moves the start by one byte (x + 1); +64 (40h) scrolls by one character row = 4 lines.

ROM register values [HW table 7, OS §8.2]; confirmed from the bytes at SYS 2.2 C20Ch, written R15 down to R0 at boot ([ROM], [rom.md](rom.md#3-boot-reset--cartridge-or-basic) §3):

| R0 | R1 | R2 | R3 | R4 | R5 | R6 | R7 | R8 | R9 | R10 | R11 | R12 | R13 | R14 | R15 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 99 | 64 | 75 | 50 (32h) | 77 | 2 | 60 | 66 | 0 | 3 | 3 | 3 | 0 | 0 | 14 | 255 |

### Timing

- Line = 100 characters × 640 ns = **64 µs** (= 200 CPU T-states); active area 64 characters.
- Frame = (77+1)·4 + 2 = **314 lines** = 20.096 ms = **49.76 Hz**, non-interlaced; 240 visible lines; vsync at character row 66 (raster 264).
- **⚠ CONFLICT**: the [HW] text puts the vsync start at "the 268th TV raster (78th char row)" (PDF p. 72). That does not fit R7 = 66 ❓.
- **CPU T-states per frame = 314 × 200 = 62 800.**
- Variants: R5 = 0 gives 312 lines; R5 = 0 with R8 = 1 gives 312.5-line interlace at 50.00 Hz; showing all 256 lines needs R6 = 64 and R7 = 69 per [OS]'s example (its text says 67 or 68 depending on the TV; PDF p. 67).
- Sync widths come from 74LS123 monostables, not R3; the real hsync position is R2 + width; horizontal blanking 25.26 µs ([HW] PDF p. 71).
- Border/blank: display-enable selects pixel vs border register; video is blanked from VSYNC until **MA9 goes high** (row 72 with the default start address; behaviour with other R12/R13 unverified ❓).
- Cursor as raster IRQ:
  - Default R14:R15 = 0EFFh, R10 = R11 = 3, so the pulse falls on the last byte of line 239.
  - Movable: address = row·64 + column, with R10 = R11 = line within the row.
  - **Never column 0** (double IRQ spike).
  - Disable with R10 b6–5 = 01.
- 6845 variant ❓ (MC6845 / HD46505 / clone; R12/R13 readback differs). [WinTVC] reads back R12 (& 3Fh), R14 and R15 on port 71h only; every other register reads 00h. LPEN is on the bus; no light pen was made.

## 6. Sound

- 12-bit reloading counter at 3.125 MHz dividing by (4096 − P), then ÷2 (TxRxCLK for the serial card), then ÷8: **f = 195 312.5 / (4096 − P) Hz**. P = 4095 stops the oscillator ([HW] PDF p. 127: "Ha n=4095, a hangoszcillátor leáll"; [OS]: no sound). ❓ Inferred, not stated: no sound IRQ either. [WinTVC] agrees: its IRQ counter stops at P = FFFh (static).
- Reading 5Bh (`INSTART`) reloads the counter and clears the ÷16 chain.
- Port 05h b4 gates the tone to the amplitude stage; b5 gates it to the IRQ.
- Amplitude: 4 NOR gates into a 1/2/4/8 resistor ladder, 16 steps. [EP] levels (0–30000): 0, 1948, 4095, 6043, 7986, 9933, 12081, 14029, 15971, 17919, 20067, 22014, 23957, 25905, 28052, 30000.
- **With b4 = 0 the output is a DC level equal to the amplitude**, so 06h b5–2 works as a 4-bit DAC for sample playback. MAME outputs silence there, unlike the circuit.
- Sound IRQ: once per tone period. **⚠ CONFLICT**: [OS] says the 1→0 edge; [HW] says "after the 8th reload following INSTART, ÷8 output going high".
  - **Resolved for the timing: a full period first, as [OS] and WinTVC.** The ROM's tape writer (EXT 1.x FA4Fh–FA78h: `HALT`, `OUT (50h)`, `OUT (04h)` from the table FAF3h–FAF6h = D6h/DEh/CEh/BCh, `IN A,(5Bh)`) times every half cycle as INSTART → first sound IRQ. Only 16·(4096 − P) T (plus about 20 µs of code) gives the documented 470/388/552/736 µs; 8·(4096 − P) T gives half of each. Verified: ROM disassembly, and a `SAVE` recorded by our deck on 2.2 and 1.2, whose decoded blocks equal the encoder's.
  - [WinTVC] (static ⚑): `INSTART` restarts the tone and the IRQ counter together. The first IRQ comes 16·(4096 − P) T later, near the end of the first low half-period, which is closer to [OS].
  - WinTVC renders the tone itself per audio sample with a slightly different divisor (4095 − P), so it is not a phase reference.
- Mixed with the bus SOUND pin into RGB DIN and the RF modulator; EP also mixes tape in/out.
- OS `TONE SET`: RST 30h code 33h (B = duration in 20 ms units, C = amplitude, DE = pitch).
- BASIC: `SOUND PITCH p,VOLUME v,DURATION d` (example programs in *TVC Biblia*, MEK 03604, https://mek.oszk.hu/03600/03604/).
  - In freeTVC (dynamic run), the ROM sound code (SYS in 1.2, EXT in 2.1/2.2; addresses in [machine.md](../design/machine.md)) writes p straight to 04h/05h (P = p), turns the tone gate on, and turns it off about d × 20 ms later.
  - It leaves the amplitude set, so the output stays at a DC level afterwards. It uses neither `INSTART` nor the sound IRQ.
  - At boot EXT writes P = FAFh with the tone gate on and amplitude 0.
  - Static ROM disassembly not cross-checked ❓.

## 7. Keyboard and joysticks

- Port 03h b3–0 selects a row (rows 0–9; the "row decoder" IC is at G1/H1 ([HW] PDF p. 96), and no source names the 74LS145 part number); 58h reads 8 columns, active low.
- Rows 10–15 read FFh in MAME, YATE and EP; no hardware text says so ❓. No diodes, so 3+ keys ghost ([HW] PDF p. 38). The OS scans all rows into `PICTURE` (0B51h) every IRQ.

[HW fig. 10], Hungarian layout:

| Row | b0 | b1 | b2 | b3 | b4 | b5 | b6 | b7 |
|---|---|---|---|---|---|---|---|---|
| 0 | 5 % | 3 + | 2 " | 0 & | 6 / | Í | 1 ' | 4 ! |
| 1 | ^ ~ | 8 ( | 9 ) | Ü | * # | Ó | Ö | 7 = |
| 2 | T | E | W | ; $ | Z | @ ` | Q | R |
| 3 | ] } | I | O | Ő | [ { | Ú | P | U |
| 4 | G | D | S | \ \| | H | < > | A | F |
| 5 | DEL | K | L | Á | RETURN | Ű | É | J |
| 6 | B | C | X | SHIFT | N | LOCK | Y | V |
| 7 | ALT | , ? | . : | ESC | CTRL | SPACE | - _ | M |
| 8 | INS | ↑ | ↓ | Fire | Accel. | → | ← | – |
| 9 | – | ↑ | ↓ | Fire | Accel. | → | ← | – |

- Row 8 is the built-in joystick in parallel with one external port; row 9 is the other port.
  - **⚠ CONFLICT** on which port is which:
    - Fig. 10 and the text put the "right" joystick (CH7) on row 8 and the "left" (CH6) on row 9 ([HW] PDF p. 38).
    - Fig. 9 (PDF p. 203) draws the opposite.
    - [OS] calls them front (row 8) and rear (row 9) (PDF p. 150).
    - The tvc.homeserver.hu hardware page puts both sockets on the machine's left side.
  - YATE and EP avoid the naming: joystick 1 = row 8, joystick 2 = row 9. **freeTVC uses "joystick 1/2 = row 8/9".**
- The built-in stick has neither fire nor accelerator ([HW] PDF p. 38), so row 8 b3/b4 only ever come from the external port.
- **Ghosting**, worked example in [OS] (PDF p. 87): hold A (4,6), then D (4,1), then E (2,1). The ROM types "a", "d", then "e" and a phantom fourth key. Selecting row 2 reaches row 4 through E and D (column b1), and then column b6 through A, which is Q (2,6). The OCR renders that letter as "gy", but the explanation names row 2 and bit B6.
- **PICTURE** (0B51h + row, rows 0–9 = 2897–2906) holds 1 = pressed. In freeTVC, `PEEK(2906)` reads 10 with joystick 2 UP+FIRE held, and `PEEK(2905)` reads 80 with joystick 1 LEFT+ACCEL. ALT, SHIFT, CTRL and LOCK set no bit there ([OS] PDF p. 150).

### Physical layout

Source: the Wikimedia Commons drawing "Videoton TV Computer (1984) keyboard layout.png" (Rev-san, CC BY-SA 4.0). Two photos on Commons, "Videoton TV-Computer (1986) (37650450314).jpg" (a 64k, CC0) and "Videoton TVC 01.JPG" (an original TVC, public domain), match it key for key (compared by eye).

```
Í  0& 1' 2" 3+ 4! 5% 6/ 7= 8( 9) Ö  Ü  Ó  *# ^~      INS ESC
 @` ;$ Q  W  E  R  T  Z  U  I  O  P  Ő  Ú  [{ ]}        DEL
  <> \| A  S  D  F  G  H  J  K  L  É  Á  Ű   RETURN
LOCK SHIFT Y  X  C  V  B  N  M  ,? .: -_ SHIFT
         CTRL [      SPACE       ] ALT
```

- The layout is a Hungarian typewriter layout (QWERTZ, 0 left of 1). Eight keys have no counterpart on a Hungarian PC keyboard: `@` `;` `<` `\` `[` `]` `*` `^`.
- Both SHIFT keys are the one matrix switch (6,3). The built-in joystick sits to the right of the keyboard.

### ROM key tables

- SYS 2.1 and 2.2 hold them at **CD28h** (file offset 0D28h), SYS 1.2 at file offset **17BFh**. The 320 bytes are byte-identical in all three ROMs (compared by script).
- Four tables of 80 bytes, entry **(7 − column) · 10 + row**:
  - table 0: unshifted;
  - table 1: SHIFT;
  - tables 2 and 3: CTRL and ALT, order ❓ (not traced in the ROM code).
- Codes: ASCII; accented capitals Á É Í Ó Ö Ő Ú Ü Ű = 80h–88h, small letters 90h–98h in the same order. DEL is 08h, SHIFT+DEL 07h, RETURN 0Dh, ESC 1Bh. Row 8: INS 16h, ↑ 05h, ↓ 18h, → 04h, ← 13h, fire 06h, accel 01h. Row 9 gives E1h–F8h.
- These tables back `src/core/input/keyboard.cpp`. `keymap_rom_test` checks the two in both directions against all three ROMs. `typing_test` types every character at the BASIC prompt on all five presets and reads the codes back from the screen copy.

### Editor behaviour (dynamic runs in freeTVC, SYS 2.2)

- LOCK is off after boot, so letters type in lower case.
- DEL deletes the character left of the cursor, like PC Backspace. SHIFT+DEL deletes the character under the cursor: "abcdef" with the cursor on "d" became "abcef". WinTVC's readme instead maps PC Backspace to SHIFT+DEL.
- At the prompt, one frame held and one frame released per key is enough. Keys pressed while BASIC handles RETURN are lost: storing a 47-character program line needed 15 frames, while 10 lost the next key.
- **The start-up logo takes the first key, and a quick second key is lost** (freeTVC monitor traces on 64k+-2.2, cross-checked by typing into WinTVC 1.4.1 under Wine). The keyboard IRQ (CB34h) registers a key only on its press edge, and only while no key is waiting: CB84h stores the code in 0BE9h, CB89h sets 0BE5h = FFh, and further presses are ignored until `KBD_CHIN` clears 0BE5h (CABEh). The logo loop polls `KBD_STATUS` (93h, from CF87h) only once per animation step, about every 6 frames, and then reads the key with `KBD_CHIN`. So `PRINT 2` typed over the logo at 2 frames held, 2 released arrives as `INT 2` (P leaves the logo, R falls into the wait), at 5/10 frames as `RINT 2`. WinTVC gives `rint 2` at 5/10 frames and loses more at 2/2 (`t 2`). Scripts press a key for the logo first (`SPACE`, as the MCP instructions and the ROM tests do).
- Cyrillic matrix: see MAME `tvc64pru`.
- DE-9 joystick: 1 up, 2 down, 3 left, 4 right, 6 fire A, 8 common, 9 button B (accel.).

## 8. Cassette

Hardware:
- Out: any read or write of 50h–57h toggles a flip-flop ([HW] PDF p. 36; [OS] only mentions writes). The 200 mVpp level comes from the tvc.homeserver.hu overview page only.
- In: op-amp to 59h b5.
- Motors: 05h b6/b7 (two DIN sockets, data lines in parallel); `REMRED` (0B6Ch, default 80h) selects which one.

ROM encoding [OS §13, HS]:
- One cycle per bit, LSB first: "0" = 552 µs (1812 Hz), "1" = 388 µs (2577 Hz), leader 470 µs (2128 Hz), sync one 736 µs cycle (1359 Hz).
- Block: leader of **10 240** cycles (header block) or **5 120** (data block) → 1 sync cycle → bytes → 5 leader cycles. The motor starts about 1 s before the leader.
- Block bytes: 00h, **6Ah**, block type (FFh header / 00h data), file type (01h buffered / 11h unbuffered), copy-protect, sector count (1–256).
- Each sector: sector number (0 in the header block, data blocks from 1), byte count (0 = 256), data, EOF flag, CRC16.
  - EOF flag: 00h in every sector but the last, FFh in the last. [OS] gives only the 00h part; the ROM and MAME write FFh. The tvc.homeserver.hu page's "00h in the last sector" differs from the ROM.
- CRC:
  - Coverage: from 6Ah through the EOF flag for the first sector; from the sector number for later sectors.
  - Seed `MUDDLE` (0B6Fh). The default of 0 is set by ROM init; no text states it.
  - Per bit, LSB first: `a = bit<<7 ^ (crc>>8); if (a & 80h) crc ^= 0810h; crc <<= 1; if (a & 80h) crc++`.
    - Matches the ROM (EXT 2.x EAFCh–EB12h), MAME and YATE.
    - It equals CRC-CCITT polynomial 1021h with an MSB-first register and data fed LSB first.
  - **Byte order: low byte first.** EXT 2.x sends L, then H (EA4Ch, file offset 0A4Ch of `ext2x.bin`) and reads L, then H (E8C3h); MAME agrees.
    - [OS] (PDF p. 113) says high byte first, which differs from the ROM. The tvc.homeserver.hu page gives both orders.
  - MAME notes:
    - It feeds the leading 00h into the CRC, which is harmless while MUDDLE = 0.
    - When the length is a multiple of 256 it writes an extra, uncounted sector (`tvc_cas.cpp` lines 156–179).
    - It resets the CRC for every sector, as the ROM does.
- Unbuffered file: header block = 1 sector with the name (length + ≤ 16 chars) + 16-byte program header (see [file-formats.md](file-formats.md#cas--program-file-vt-dos-on-disk-format-de-facto-exchange-format)), then one data block of 1–256 sectors. Buffered file: header with the name only, then 256-byte one-sector data blocks.
- Error codes: ECh EOF, EBh already open, EAh CRC, E9h not open, E8h verify, E7h internal, E6h protection, E5h bad block.
- The MiSTer core's `doc/TAPE.md` (github.com/vinej/videoton_mister) has another timing write-up.

## 9. Expansion

Bus [MAME bus/tvc/tvc.h]:
- 2 × 43-pin connector.
- Signals:
  - buffered address/data;
  - `/EXP` (IOMEM), `/P2`, `P3`;
  - `/BIORQ /BWR /BM /BRFSH`;
  - slot `/INT`, `/CLRINT`, direct `/INT`, `/NMI`, `/WAIT`, `/BUSRQ`, `DMA` (64k+);
  - 3.125 and 6.25 MHz, `TXRXCLK`, `SOUND`, `/HSYNC /VSYNC BORDER LPEN`;
  - `IDA/IDB`, `/PHANTOM`, `MUX`, `/VIDLSB`, `EC0–3` with `/EXTC` (A13/B13/A14/B14 [MAME bus/tvc/tvc.h]);
  - ±12 V, +5 V.
- Each slot has 16 I/O ports, an 8 KiB IOMEM window, an IRQ and a 2-bit ID.

### Original cards

- **HBF floppy (49111)**:
  - FD/WD1793 at 1 MHz, WD9216 data separator, up to 4 drives, 250 kbit/s MFM.
  - IOMEM C000–CFFF = 4 KiB window into a 16 KiB EPROM (4 pages); D000–DFFF = 4 KiB SRAM.
  - Ports: +0–3 FDC; +4 write b0–3 DS0–3, b4 head load ❓, b5 DDEN, b6 motor, b7 side; +4 read b0 INTRQ, b7 DRQ, b6 disk change ❓; +8 write b5–4 EPROM page.
  - ID 10.
  - EPROMs: VT-DOS 1.1 (`hbf.rom`, CRC-32 AE34982B, the image WinTVC ships), VT-DOS 1.2 (`hbf12.rom`, 2ACF8477) or UPM (B3A567AD). BASIC's LOAD and SAVE work with the card EPROM alone; booting VT-DOS or UPM also needs the matching cartridge ROM and U3 in page 0.
  - +4 read: the card ROM's transfer loops (VT-DOS 1.1 `hbf.rom`, page 2 at CCE0h: `IN A,(C)` / `JR Z` / `JP M`) need every bit other than b7 and b0 to read 0 (ROM disassembly; the loops run unchanged in freeTVC, [cards.md](../design/cards.md)). So b6 "disk change" reads 0 at least on the card this ROM was written for ❓.
  - The ROM writes the +4 latch with b6 (motor) set and one select bit, and waits for status b7 (not ready) to clear before each type II command (CC29h–CC31h, CDB6h–CDDBh).
- **HBS serial (49103)**: 8251A (+0 data, +1 mode/command and status), clock TXRXCLK, baud = 97 656.25/(4096 − P), so the sound generator affects the baud rate. ID 00; driver in EXT ROM.
  - From Csontos András' redrawn factory board `tvcser_4` (https://tvc.homeserver.hu/html/pic/soros/kapcs/tvcser_4.jpg, with three other redrawn boards; drawn from two altered boards, "not built, may contain errors" ❓) and the card's manual (`doc/konyvek/sorosvonaliif.pdf` on the same site): the 8251A-P5's /CS is the slot's /SLOTX, so it answers on all 16 ports with A0 as C/D (even = data, odd = control); C/D on A0 (A31), /WR on B42, TxC and RxC on the bus TXRXCLK (A15), CLK (pin 20) on A41, drawn as "!BC" (presumably a bus clock ❓), RESET on the bus RESET (A35); a 74LS245 buffers the data bus; IDA/IDB grounded through jumper TB1 (ID 00); TxRDY, RxRDY and SYNDET reach no bus line, so the card has no interrupt (the army's rebuilt boards wired one to B37 for a synchronous link, per the tvc.homeserver.hu page); 75150/75154 V.24 drivers, a 25-pin D connector with 102, 103, 104, 105 (RTS), 106 (CTS), 107 (DSR), 108, 109, 111; jumpers TB2–TB4 set 108, 105 and 111 for the three cable types (the factory one is the handshake cable between two TVCs: each side's RTS into the other's DSR, 106/109/111 tied together in each plug).
  - The emulated card: [cards.md](../design/cards.md).
- **HBM** 32 KiB RAM (49102); HBU parallel (49105) ❓; HBI-2 IEC-625 (49106) ❓; HBE EPROM programmer (YATE: 2 × 8255); TVC-NET ❓.
- **Cartridge (HBR)**: ≤ 16 KiB EPROM in page 0 (`01`) or page 3 (`00`); `MOPS` + entry at offset 4. Factory titles: 49201 Space Invaders, 49202 UPM, 49203 Bridge, 49204 Debugger.

### Modern cards (YATE source; authoritative specs at github.com/dikdom)

- **GameCard**: SN76489 at +0 (3.125 MHz); joysticks 3/4 at +2/+3 (b0 up, b1 down, b2 left, b3 right, b4 fire, active low); +0Fh write b2–0 ROM page (8 × 8 KiB), b3 sound enable. ID 11. The [EP] libretro fork agrees on the joystick bits (b7–5 read 1). Its placement differs between branches: 30h in `core`, 40h (slot 3) in `tvc_gamecard`, `devtool` and `standalone`; the `tvc_gamecard` README says slot 2.
  - **From the designer's sources** (github.com/dikdom/TVC-GameCard at `56e9c2b`, MIT; [GC] = its `doc/TVC GameCard.pdf`; read from the GAL sources `src/gal/*.PLD` and the Eagle schematic `src/hw/tvc-gamecard-GAL-v2.2.1.sch`):
    - Decode (`TVC-JOY.PLD`): reads at +2 / +3 drive D0–D4 only, from joystick 3 / 4 (A0 picks); D5–D7 are not driven (undefined in [GC]). Writes at +0 and +Fh select the second GAL (`SN_REGISTER_CE`); nothing else in the slot's 16 ports answers.
    - `TVC-LOGIC.PLD`: +0 latches the byte into a 74x574 for the SN76489 (written when the chip's READY allows; READY is not wired to the bus /WAIT, so the CPU never waits ❓ whether writes closer than ~32 chip clocks are lost); +Fh latches D0–D2 into EPROM A13–A15 and D3 gates the chip's clock (1 = running). **Reset loads page 7 and stops the clock** ([GC]: "xxxx0111"). The polarity is confirmed by the software (dynamic, freeTVC monitor trace): BattleField writes 08h (from 4A05h) and RasterRunner 0Fh (from 1ACAh) to +Fh before they play, the boot menu writes pages only (`AND $07`).
    - Schematic: the chip's clock is the bus `3M125` (3.125 MHz, as [GC] states; the GAL pin name `CLKIN_359` is a leftover), its output goes to the bus `SOUND` line; IDA/IDB, /INT and /WAIT are not connected, so port 5Ah reads 11 for the slot and the card is named by its ROM header. EPROM W27C512 (64 KiB), enabled by the slot's /EXP, never written.
    - The EPROM (`bin/TVC-GameCard-r1229.BIN`, CRC-32 F782DB9B): page 7 = header `MOPS` 06h `JOY+SN` and the boot menu (`src/menu/TVC-GameCard.z80`); pages 0–6 hold four games as CAS images that the menu copies to RAM: TVC4Snake (dikdom), Rasterrunner, BattleField and Bomberman (credited in [GC] to Bertók Zsolt, Major Tamás, Persa Noel, Sáránszki Mihály). The menu makes later resets cold, back to the menu; ESC hands the boot on to the next card or BASIC, with the card's ports still usable.
    - The chip ([GC] ships TI's SN76489AN manual): f = N / (32 n); 2/4/8/16 dB attenuators, 1111 = off; noise rates N/512, N/1024, N/2048 or tone 3's output, the shift register cleared on every noise-control write. MAME's `src/devices/sound/sn76496.cpp` (BSD-3-Clause, read as documentation): the SN76489A has a 15-bit register with taps on bits 13 and 14, XOR, output not inverted; every TI chip treats a period of 0 as 400h.
- **SoundMagic / Multichip**: SN76489 (3.579545 MHz) at +0/1, SAA1099 at +2/+3, YM3812 at +4/+5, ROM page +6/7.
  - **From the designer's sources** (Vass Sándor / dikdom, github.com/dikdom/TVC-Multichip-Soundcard commit `fa23e43`, CC BY-NC-SA 4.0, read as documentation; [MC] = its manual `doc/TVC Multichip Soundcard-v1.0.pdf`, board v1.3; [SCH] = `hardware/pcb-eaglev8.3.1/tvc-soundmagic-sch-v1.3.pdf`, read off the rendered sheet; static ⚑ unless a run is named):
    - Decode [SCH]: a 74HCT139 enabled by /SLOT_N decodes A2–A1 into /CS-SN76 (+0/+1), /CS-SAA (+2/+3), /CS-YM (+4/+5) and the bank latch (+6/+7); A3 is not decoded, so +8…+15 mirror ([MC] says so too). A0 goes to the SAA's and the YM's A0: SAA +2 data, +3 address; YM +4 address/status, +5 data ([MC] "Speciális IO portok").
    - Reads [SCH, MC]: only the YM has /RD (/BRD); nothing else drives the bus, and the latch and registers cannot be read back.
    - SN76489: data through a 74HCT273 latch, written when the chip's READY allows (74LS02 gates); READY is not on the bus /WAIT ([MC]: ≥ 28 T between writes). Clock CLK-3.579 from the card's own 3.579 MHz crystal oscillator (74LS04), always running; no reset pin.
    - SAA1099: clock CLK-8 from a second, 8 MHz crystal oscillator; OUTL/OUTR into 1k loads to the filtered +5 V, IREF through 10k; 1k / 10 nF filters to SND-SAA-L/R. No reset pin.
    - YM3812: clock CLK-3.579, /IC = /RESET, **/IRQ = the slot's /INT_N** (so its timers can interrupt the TVC); YM3014B DAC with an LM358 buffer to SND-YM.
    - Bank latch: two 74HCT74 flip-flops clocked by /IOMEMMAP NOR /BWR take D0 → EEPROM A13 and D1 → A14; /PRE = /RESET, so reset selects bank 3 (11, [MC]: "Bekapcsoláskor ez az érték 11"). The AT28C256's /CE is /EXP, /OE /BRD, /WE tied to VCC. Its upper two banks are the only ones every fitted chip type can use ([MC]); the firmware keeps to them.
    - IDA_N/IDB_N are not connected: port 5Ah reads 11 for the slot; the card is named by `MOPS` 05 `SndMx` ([MC] "Kártya lokalizálása").
    - Sound out [SCH]: two LM358 mixers (22k for the TVC's SOUND, the SN and the YM, 12k for each SAA side, 22k feedback, ±9 V) into OUT-LEFT/OUT-RIGHT; through 1k each to the stereo jack and, with jumper SV1 on TV-ALL, onto the bus SOUND line (the TV then hears all three chips; [MC] "A jumper").
    - Timing [MC]: YM ≥ 12 T between address and data and ≥ 71 T after data; SAA ≥ 2 T (7 T after a volume write); the CPU never waits.
    - The firmware 1.0.1 runs unchanged in freeTVC (dynamic, freeTVC's emulation): its board check switches to bank 2 and back, and its demos play on all three chips ([cards.md](../design/cards.md)).
- **SoundQuartett**: 2 × 8255. Hanák 6-channel SAA1099 mod uses ports **FEh/FFh** (outside the official decode). Mikrovilág 1991 4-channel card on 10h–17h ❓.
- **NanoSD**: Arduino byte stream; 8 KiB ROM pages.
  - **From the designer's sources** (Vass Sándor / dikdom, github.com/dikdom/TVC-NanoSD commit `3fb88f2`, BSD-3-Clause, read as documentation; [NSCH] = `hardware/TVC-NanoSD-IO-2-sch.pdf` rev 2, read off the rendered sheet; [NM] = the manual `doc/NanoSD IO Card.pdf` v0.3; [NFW] = the Nano firmware v0.32 `software/Arduino/TVC_NanoCartSD/TVC_NanoCartSD.ino`; [NROM] = the ROM v0.33 sources `software/EEPROM/`; static ⚑ unless a run is named):
    - Decode [NSCH]: a 74HCT02 makes SLOT_N_WR = NOR(/SLOT_N, /WR) and SLOT_N_RD = NOR(/SLOT_N, /RD); no address line is decoded, so all 16 ports of the slot are the same register ([NM] "Alacsony szintű programozás" says so).
    - Two 74HCT574 latches [NSCH]: TVC→Nano (IC1) clocked by SLOT_N_WR, read by the Nano with its /OE; Nano→TVC (IC2) clocked by the Nano, its /OE driven while SLOT_N_RD is high. SLOT_N_RD and SLOT_N_WR also reach the Nano's INT0/INT1, falling edge, so the Nano latches its next byte only after a read ends ([NFW] `ISR(INT0_vect)`): a read returns what the Nano latched after the previous read ([NM]: the first IN after an OUT is stale, the ROM drops it).
    - EEPROM W27C512 [NSCH]: /CE = /EXP_N, /OE = /RD, A0–A12 from the bus, **A13 = the Nano's bank line** (Arduino A4, PC4), A14/A15 tied high: the window is file offset C000h (bank 0) or E000h (bank 1). The v0.33 image fills C000h–DD2Fh; bank 1 is blank (FFh).
    - IDA/IDB, /INT and RESET are not connected [NSCH]: port 5Ah reads 11 for the slot (the ROM's header `MOPS` 06 `NANOSD` names it), the card never interrupts, and the TVC's reset never reaches the Nano ([NM]: after a loss of sync, reset the TVC, then the Nano by its own button).
    - SD socket 112J-TDAR-R01 with its card-detect switch on the Nano's PC5 (pin change interrupt), through a 74LVC245 level shifter on SPI [NSCH].
    - Slot: any; the ROM's init derives its port from IX (`NanoSDROM_INIT.z80`, `(IX+8)` = 10h·(slot + 1)) and takes the cassette class (0B05h/0B0Dh) on every reset, warm or cold ([NM] "Eszközosztály").
- **SD Dual** (Sebestyén Pál, 2015; details and evidence in [cards.md](../design/cards.md)): a cartridge; C000–DFFF flash in 8 KiB pages (64 KiB), E000–FBFF 8 KiB SRAM, FC00–FFFF the CPLD's registers (A1–A0: +0 SPI data, +1 status b7 SD write-protect / b6 no card / b5 card changed and control b7 CS microSD / b6 CS SD / b5 clear change, +2 page in b7–5, +3 b7 fast reads); two SD sockets on one SPI bus; firmware with VT-DOS 1.2 (`EXT 2` CLI, `EXT 3` file manager). From jtvcemu's model (Apache-2.0, co-written by the designer; read as documentation) and the firmware's code; no schematic was published. The earlier SD interface (2009, a slot card) and the single-socket SD cartridges (2011, 2013) select the card differently ⚑ and are not emulated.
- **SDCart** (YATE): ROM 4 × 16 KiB at C000–DFFF plus co-processor windows at E000/E800/F800; an emulator-side design with no known hardware, not emulated.
- **MultiCart v2** (board `seleprom6-7`, from its Eagle schematic and `tvcpla.bin` in github.com/dikdom/TVC-Multicart-v2 at `631b251`; details and evidence in [cards.md](../design/cards.md)): register at cart offset 0000–0003 (A0 ignored, A1–A12 decoded; at 2000–2003 while b7 is set): b4–0 16 KiB page, b5 second flash chip, b6 RAM, b7 relocation. A write at +0/+1 makes a later read of +0/+1 clear the register (a 74HCT74 latches A1), a write at +2/+3 stops that; power-on (an RC circuit, no reset line on the port) clears both. Two SST39SF040 and a 128 KiB SRAM; the port has no /WR, so a write is /CART without /BRD. MultiCart v1 not examined.
- **MegaCart**: 1 MiB, write to 3C00–3FFF selects the page.
- **KiloCart**: 64 KiB.
- **TVC256k++**: extra RAM, USB storage, SID (vSID, no filters), mouse, graphics overlay and sprites. An RP2350 MCU emulates the card's ROM, RAM and registers on the bus.
  - **Authority [256K]**: dikdom's sources at commit `4580b14` (2026-10-14), firmware **v0.24**, and the Hungarian manual `doc/TVC256k++ kártya-20261011.pdf` (73 pp.), github.com/dikdom/TVC256kplusplus.
    - The repository as a whole is GPL-3 (it links the GPL-3.0-or-later `psram.c`). The author's own files are individually MIT (README). freeTVC's card is written from them as documentation; only data tables come from the firmware's sources: the vSID's tables and the power-on sprites ([THIRD_PARTY.md](../../THIRD_PARTY.md)).
    - Read from the sources and the manual; not tested on hardware.
    - File paths below are relative to `sources/firmware/TVC-IO/`.
  - [EP] (`spriteext.hpp` in `core`, `ep128emu.hpp` lines 128–262 in `standalone`) uses the firmware's register names.
  - **Slot: any.**
    - The manual (p. 21) has software find the name entry `06 "256k++"` at 0040h/0070h/00A0h/00D0h, giving slot 0–3 and base port 10h/20h/30h/40h. A system call also returns the base port in C.
    - The card ROM derives the same from IX at init (C02Eh–C048h).
    - The manual tells users to plug it into the upper right-most connector. ep128emu uses 30h (40h in `core`).
  - **Ports** (`TVC-IO-main-sys.c:275-368`, `irq_handler_slotn`):
    - Only A0–A2 are decoded, so +8…+15 mirror +0…+7. The manual (p. 22) says +3 and +7…+15 are unused; +3/+7 read 00h.
    - +0/+4: register index (read back); +1/+5: data; +2/+6: increment, reset 1. Each data access adds the increment to the index.
    - A data access goes through a per-register read/write pointer table (`:500-595`): no read pointer → reads 00h; no write pointer → the write is ignored (read-only).
    - Writes to 90h+ and 60h–8Fh are then posted to the main loop, so some take effect later (manual p. 22). Writes to E0h+ go to a time-stamped SID queue.
  - **IOMEM** (`:258-273`): C000–DFFF is an 8 KiB array.
    - C000–D7FF is a 6 KiB bank of the 12 KiB ROM. B7h = 0/1 copies bank 0 or 1 in (`:927-932`).
    - D800–DFFF is 2 KiB of RAM for the ROM's variables: D810h = drive select (C5h), D824h/D825h = PSRAM/DSK valid flags.
    - Writes below D800h are ignored.
  - **Memory** (`:219-253`, `tvc-system.pio`):
    - The card claims a memory cycle when /MREQ and **/P2 or /P3** are low. These lines are the U2/U3 select signals (NP2/NP3, brought out to the connector; [HW] PDF p. 21).
    - If B0h (for U2) or B1h (for U3) is not FFh, the card asserts **/PHANTOM** and serves the cycle from fast-RAM page n (address & 3FFFh).
    - So the card replaces the **U2/U3 units wherever port 02h maps them**. That includes U3 in page 0, since the paging circuit drives NP3 there ([HW] PDF p. 26); this conclusion is static ⚑.
  - **Border snoop** (`:179-200`, PIO `border_io`): writes to port 00h are copied to A7h, converted IGRB → IRGB.
  - **Card ID and interrupt** (rev3 schematic `doc/Schematic_TVC-rev3-smd.pdf`, connector pins B35–B38, read off the rendered sheet): IDA_N and IDB_N are not connected, so port 5Ah reads 11 for its slot; /INT_N is wired (the collision interrupt), /CLRINT_N is not.
  - **Overlay** (`tvc-graphics.c`, `tvc-overlay.pio`; manual pp. 13–17): a 256 × 240 picture, one byte a pixel (`---eirgb`, e = /EXTC), clocked out on the bus 6.25 MHz from each paper start (BORDER) and counted from the end of VSYNC; line N is built at the paper start of line N − 1. Modes, sprites, collisions and the frame-end /INT are in [cards.md](../design/cards.md).
  - **Where the overlay enters the TVC** (HBA-2 schematic sheet 9, tvc.homeserver.hu `doc/tervrajzok/TVC_HBA2_rajzok.pdf` PDF page 11, read off the rendered sheet, static ⚑): EC0–EC3 drive the colour bus through the second half of B11 (74LS244) while /EXTC (pull-up R7) is low, in place of the 16-colour bits (B11's first half) and the palette RAM B7. The bus is input 1 of the 74LS298 B9, whose input 2 (border register B8) WSE selects during the border, and B9 is clocked at the mode's pixel clock by H5 (74S153; [HW] p. 110). So the overlay shows only in the paper, at full resolution in 2- and 4-colour modes and every other pixel in 16-colour mode ❓ (which of a pair).
  - Emulated in freeTVC from v0.24 ([cards.md](../design/cards.md)): the card ROM's init, `EXT n`, `VER`, `HELP` and its bank switch run unchanged on SYS 1.2 and 2.2; a 32k board then boots with a 64k board's 42255 bytes free (dynamic, freeTVC's emulation, not a real card).
  - Registers (enum `TVC-IO-main-sys.h:95-156`; reset values from the initialisers):
    - 00–9Dh sprites. 16 sprites: X 00h, Y 20h (16-bit, low/high pairs), colour 40h, phase 50h, foreground 60h, enable 70h, colour mode 80h.
      - Packed enable/foreground/colour-mode in 90h–95h; sprite-sprite and sprite-background collision in 96h/97h and 98h/99h; IRQ masks in 9Ah–9Dh.
      - **Reading 96h (98h) clears the 16-bit collision word and releases the slot /INT** (`:306-314`).
    - 9Eh sprite base (reset 0), A0h video mode (reset 0), A1h–A4h screen/colour/font/bitmap base (reset 01 02 02 01), A5h/A6h scroll, A7h border, A8h overlay renderer's line counter (read-only, starts at 21; `tvc-graphics.c:71`), A9h max Y (write accepted if 2–239; reset 240).
    - **B0h/B1h** U2/U3 page: 00–0Fh = fast-RAM page (256 KiB, pages 0–3 hold the graphics data), FFh = the machine's own RAM. **Reset 04h/05h**, so a 32k machine looks like a 64k one after power-on (manual p. 12).
      - A written value other than FFh is masked to b3–0 a little later (`:901-908`).
      - Reads give 10h/11h while a PSRAM page is mapped.
    - **B2h–B5h** PSRAM page (16 KiB, copy-in/copy-out, delayed; `processPSRAMMapping`, `:675-704`). B3h/B5h latch the high part without effect; B2h/B4h start the mapping; a page ≥ B6h·64 is refused.
      - **⚠ CONFLICT**: the manual (p. 31) gives a 9-bit page (512 pages in 8 MiB). The v0.24 code uses only the B2h/B4h byte (`:910`, `:919`), so pages 0–255.
    - B6h PSRAM size in MiB, as detected (8 on the board; 0 without PSRAM). B7h ROM bank (0/1).
    - C0h function call:
      - Writing n runs function n with its parameters at the address given by C1h; the result goes to C2h (FFh = unknown function).
      - C0h reads back n until the function has run, then FFh (`:933-945`; manual p. 23).
      - C6h runs a chain of n functions. On failure C2h = 1-based index of the failing function, or 80h + that index for an unknown function (`:950-987`).
    - C3h/C4h firmware version, **00h/24** (v0.24). C5h drive select (clamped to 3). C7h bitmap base (FFh or 0–3). C8h–CAh PSRAM/flash/DSK "valid" flags.
    - **D0h** = connected USB device: 00h none, 01h mouse, 02h pendrive, one at a time (no hub; manual p. 36). FFh/F0h/F1h/FEh = USB host start-up states (`tvc-usb.c:23-137`).
      - So ep128emu's 01h (a mouse) is one valid configuration; a pendrive gives 02h.
    - D1h buttons; D2h/D3h dX/dY, cleared on read; D4h wheel; D5h speed (7 bits, stored in flash); D6h mass-storage command (`msc_command_t`, `TVC-IO-main-sys.h:158-179`).
    - E0h–FFh vSID registers (E0h + n = SID register n).
- **SID card**: no real card's documentation found (the English Wikipedia's "a SID card was created" has no source; no schematic, ports or software). WinTVC 1.4 puts a SID at E0h–F8h, register = port − E0h, write-only (reads FFh), 985 248 Hz, a 6581 (WinTVC, static analysis); freeTVC follows it ([cards.md](../design/cards.md)) ❓.
- **2dfx** (Vaczkó Károly, 2026; for the TVC and the Enterprise): a graphics coprocessor card (an RP2354B MCU) that draws sprites, 2D primitives, text, bitmaps, tilemaps and 3D meshes into its own frame and puts it over the TVC picture through EC0–EC3 and /EXTC. Emulated with the creator's library ([cards.md](../design/cards.md), "2dfx graphics card").
  - **Authority [2DFX]**: the creator's portable emulator of the public interface, **2dfx v1.1.0**, in github.com/kks2003/Ep128web `web/src/twodfx/` (read 2026-11-06): `docs/COMMAND_REFERENCE.md`, `docs/INTEGRATION.md`, `docs/COMPATIBILITY.md`, `include/twodfx_emu.h`. MIT (Vaczkó Károly), except the Elite-derived (`egg_D3_elite_showcase_data.h`, used by D3h–D5h) and Wriggler-derived (`egg_C9_wriggler_assets.h`) demo data, which carry no licence (`ASSET_LICENSES.md`). The MCU firmware itself is private. The creator found the emulated demos visually identical to the card on 2026-10-22 (`COMPATIBILITY.md`); not tested on hardware by freeTVC.
  - **Ports** [2DFX]: **F8h** write = command, read = status; **F9h** write = parameter and data bytes; F9h reads are not the card's. Fixed ports, not slot-relative (Ep128web maps F8h–F9h on the TVC in `patches/zzzz-2dfx.patch`). ⚠ freeTVC's SID card takes E0h–FFh, so the two overlap; beside the 2dfx it keeps E0h–F7h.
  - **Wake-up**: after power-on only `50h SET_ENGINE_MODE` is accepted (one F9h byte: 00h off, 01h–FEh normal, FFh forced); status b6 reads 1 until the engine is ready, then 0 (`OUT 248,80: OUT 249,1`).
  - **Status (IN F8h)**: b7 collision in the previous frame, b6 not ready, b5 render overrun, b4–1 raster interrupts 4–1 pending, b0 render-time pin (during `8Fh SYS_RESET`: 0 busy, 1 done). b7, b6, b5 and b0 are active-low at the pin.
  - **Commands** (F8h): 00h–3Fh select sprite 0–63 and write its 9-byte block (enable/collision/high bits, X, Y, 23-bit address, flags X/Y double and flip, W, H); 40h–4Fh transparent colour; 50h–5Fh engine, 25/50 fps, raster interrupts (54h–57h, a line 1–311, high byte first), font/bitmap/pattern/tileset/tilemap descriptors; 60h–7Fh background and E0h–FFh foreground draw lists (256 slots of 12 bytes: plot, line, rect, ellipse, arc, pie, triangle, fills, text, blit, crop/stretch blit, pattern fill, tilemap, clip, XY offset, mesh, RET 7Fh); 80h–82h upload (3-byte address, 2-byte length, payload; raw, Enterprise NICK or TVC Graphics-16 converted); 83h–85h ZX1 decompression; 86h–88h mesh, projection, camera; 8Eh firmware page (ignores everything after it until power-off); 8Fh reset; 90h/91h logo; C0h–DFh built-in demos (CFh and D0h TVC only).
  - **Memory**: 8 MiB resource RAM, 23-bit little-endian addresses, reachable only through the uploads; 64 sprites; 16 colours; the frame is 704 × 256 on the TVC, local (0,0) = raw line 41.
  - **Picture**: double-buffered; sprite, descriptor and draw-list writes go to shadow copies and take effect at the next VSYNC. The overlay shows only in the paper; the colour index bits are I R G B (b0–b3), mapped to the TVC's colour byte. Ep128web's integration: full 2dfx resolution in Graphics 2, every second pixel doubled in Graphics 4 and 16 ⚑ (the HBA-2 reading under TVC256k++ above says 2 and 4 colours at full resolution). freeTVC follows Ep128web: AnyStone's Wolf3D title, a 512-pixel checkerboard dither in 16-colour mode, needs at least every second pixel.
  - **Interrupt**: the raster latches hold the TVC's /INT low (level) until `58h CLEAR_RASTER_INT`.
- **UPM** (Videoton's CP/M 2.2-compatible OS; cartridge 49202): runs on the HBF disk card with the UPM card EPROM (B3A567AD, accepted as `hbf.rom`) and the UPM cartridge (the HBF entry above). Ep128web (README, read 2026-11-06) calls them `tvcupm_d.rom` and `tvcupm_c.rom` (from the ep128emu ROM package `ep128emu_roms-2.0.11.bin`); the cartridge prints `*` and waits for RETURN. Disk geometry (kks2003/TVC-DiskManager `src/types.ts` at `687900c`, MIT): 80 tracks × 2 sides × 9 × 512 bytes (720 KiB) or 40 × 1 (180 KiB), the first 4 tracks reserved, 2 KiB blocks, 2 directory blocks, 128 (720 KiB) or 64 entries, empty bytes E5h; 351 blocks on 720 KiB ❓ (not checked against a UPM disk).
- Not yet found: mouse (other than the TVC256k++'s), RTC.

## 10. Media formats

See [file-formats.md](file-formats.md).

## Open questions

1. Which edge sets the cursor/sound IRQ flip-flop (MAME and EP differ, §2). (WinTVC: a level flag set on every matching character clock, only in the displayed area.)
2. Whether a sound IRQ can still occur at P = 4095, where the oscillator stops (§6; WinTVC: no).
3. Video-blank flip-flop timing with non-zero R12/R13; exact sync widths; the vsync raster 264 vs. 268 conflict (§5).
4. 6845 variant and register readback.
5. Does HBF assert `/SLOWEXP`? Do factory carts assert `/FAST`?
6. HBF +4 bit meanings (head load vs DDEN, disk-change polarity, FDC reset).
7. "First vs second series"; a BASIC 1.3 dump.
8. Read values of undecoded ports and of keyboard rows 10–15 on real hardware.
9. Registers of cards with no published documentation: a real SID card, mouse and RTC cards, the earlier SD interfaces.
10. Bus value during INTA.
11. Exact RGB levels.
12. Are refresh cycles stretched when I:R points into VID (§2)?
13. Which joystick port is "right"/"left" on rows 8/9 (§7, fig. 9 vs fig. 10).

Ways to resolve them: comparing with WinTVC's behaviour, purpose-built test programs on real hardware (ask the community), and the dikdom repositories.
