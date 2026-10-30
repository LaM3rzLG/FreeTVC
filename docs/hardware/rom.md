# TVC system ROMs: SYS 2.2 / EXT 2.x and SYS 1.2 / EXT 1.x

A map of the TVC system ROMs: the operating-system and BASIC entry points, the system-call (RST 30h) mechanism and its function table, and the cassette routines. §1–§10 cover the 64k+ set (SYS 2.2 / EXT 2.x); §11 covers the 1.x set and how it differs; §12 the cassette ABI that freeTVC's host cassette serves. Machine-readable symbols are in [`rom-symbols/`](rom-symbols/). The BASIC 2.1 differences are not analysed (§10).

This file documents behaviour. The ROMs are not in the repository; the ROM autofetch downloads them ([build.md](../design/build.md#rom-autofetch)).

## Method and evidence tags

- Images: the ROM autofetch's `sys12.bin`, `sys21.bin`, `sys22.bin`, `ext1x.bin` and `ext2x.bin`, joined from the original EPROM dumps on tvc.homeserver.hu. They equal MAME's chips: SYS 1.2 = `tvc12_d4` (834CA9BE) + `tvc12_d3` (71753D02), CRC-32 6DA8809A; SYS 2.1 = `tvc21_d6` (F197FFCE) + `tvc21_d4` (B054C0B2), 53C8FF98; SYS 2.2 = `tvc22_d6` (05AC3A34) + `tvc22_d4` (BA6AD589), 79FA818C; EXT 2.x = `tvc22_d7`, 05E1C3A8; EXT 1.x = `tvc12_d7`, 1CBBEAC6, the 8 KiB socket image whose upper half is the 4 KiB ROM (59C6B9F9, §1).
- `tools/re/z80trace.py` with `tools/re/tvc_roms.json` traces the code recursively and writes z80dasm block and symbol files into a work folder (ROM-derived, not distributed; commands at the end of §10).
  - It follows the paging (`LD A,n … OUT (2),A`), the FFF5h bridges (HL tracked), the dispatch tables, and the inline data after RST 30h, RST 08h, RST 18h and `CALL FB78h`.
  - Its instruction decoder agrees with z80dasm on all 37 212 legal instructions of a linear sweep over the five images.
  - Every remaining instruction overlap was checked: 26 in SYS and 7 in EXT, all deliberate skip tricks (`LD BC,nn` / `OR n` / `JP cc` hiding a second entry).
  - Every remaining external target was explained: RAM vectors, RAM hooks, and never-taken skip jumps.
- `tools/re/check_rom_syms.py` checks the symbol files. Each code label must be an instruction start of the trace, and each named handler must equal the word in the ROM's own dispatch table. Result: 246 symbols and 144 table entries, 0 problems.
- Tags used below:
  - **R**: read in the traced listing.
  - **OS p.N**: matches page N of [OS] (Garai Géza, *TV-Computer Operációs rendszer*, Novotrade 1987, https://tvc.homeserver.hu/doc/konyvek/operaciosrendszer.pdf; the appendix pages were read from the scan).
  - **W**: matches the ROM addresses WinTVC 1.4.1 patches for its tape traps (found by static analysis of WinTVC's patch descriptors), an independent source.
  - **C**: checked by `check_rom_syms.py`.
  - **Z**: z80dasm of the image at the given address, not the traced listing.
  - **D**: dynamic run in freeTVC: the ROM's BASIC driving the host cassette ([media.md](../design/media.md)), or a trace with the debugger.
  - **❓**: reading not confirmed.

## 1. Images and versions

| Image | Size | Seen at | Notes |
|---|---|---|---|
| SYS 2.2 | 16 KiB | 0000–3FFF (page 0, boot only) and C000–FFFF (page 3) | offset *o* appears at *o* and C000h + *o*; all code after the boot stub is linked for page 3 |
| EXT 2.x | 8 KiB | E000–FFFF when page 3 = EXT (port 02h b7–6 = 11) | C000–DFFF of page 3 is then the selected card's IOMEM |
| SYS 2.1 | 16 KiB | as 2.2 | C0E8–CF01 (boot, kernel, devices, font, keyboard tables) is byte-identical to 2.2 except **C2C6h**, the version byte (21h/22h) that boot stores at **0EB7h**. The BASIC part differs (R) |
| SYS 1.2 | 16 KiB | as 2.2 | different layout: only 2.2 % of the bytes coincide with 2.2 at the same offset; traced in §11 |
| EXT 1.x | 4 KiB | F000–FFFF when page 3 = EXT; E000–EFFF reads FFh | the ROM sits in the upper half of its 8 KiB socket. MAME's `tvc12_d7` is the 8 KiB socket image (FFh below F000h), WinTVC embeds the 4 KiB ROM; freeTVC's `normalise_ext()` takes the upper half of the 8 KiB image ([tvc-hardware.md](tvc-hardware.md#1-models-boards-roms) §1). Traced in §11 |

## 2. Paging bridges and RAM stubs

- **Bridges** (R): both ROMs carry the same stub at **FFF5h**: `PUSH AF / LD A,n / LD (P_SAVE),A / OUT (2),A / POP AF / JP (HL)`.
  - In SYS, n = F0h (EXT in page 3); in EXT, n = 70h (SYS in page 3).
  - A cross-ROM jump is `LD HL,target / JP FFF5h`. FFFEh (`JP (HL)`) is the matching `CALL (HL)` helper.
- **RAM stubs** copied by EXT at boot (E00Eh, three `LDIR`s; R):

| Source | Destination | Length | Content |
|---|---|---|---|
| E259h | 0B00h | 16 | `IN_TABLE` = `FF 01 02 FF FF 05 06 FF`, `OUT_TABLE` = `00 FF 02 FF 04 05 06 FF` |
| E269h | 0030h | 16 | `OS_ENTRY` (0030h): `JP 0B23h`; `IT_ENTRY` (0038h): `PUSH AF / LD A,70h / OUT (2),A / JP C405h` |
| E279h | 0B23h | 38 | `INFUNC` (0B23h), `OUTFUNC` (0B37h), `INT_EXIT` (0B41h); see §4. Names and sizes: OS p.160 |

- **BASIC vectors** copied by BASIC init (CF01h: F7B4h → 0008h, 27h bytes; R):

| RAM | Code | Meaning |
|---|---|---|
| 0008h RST 08h | `POP HL / LD A,(HL) / OR A / PUSH HL / NOP×4`, then into 0010h | **raise BASIC error n**, with n the inline byte after the RST. Does not return (n = 0 would return onto the 00h byte, a `NOP`) |
| 0010h RST 10h | `RET Z / POP HL / JP F9D6h` | return if Z, else raise error A |
| 0015h | `JP 0000h` | patched by the RST 18h interpreter for each op |
| 0018h RST 18h | `JP F7DBh` | threaded-code interpreter (§6) |
| 001Bh | `LD (001Fh),A / RST 30h / DB 0 / RET` | **system call with the function code in A**. BASIC uses this, so it has few literal RST 30h sites |
| 001Eh | `RST 30h / DB code / RET` | repeat the code stored last time |
| 0021h | 14 × 00h | `USRTAB` (OS p.160) |

- **Hooks** (R): a cold start (C2E2h → fill routine CA7Dh) fills **0725h–0733h** with `RET`. That gives five 3-byte hooks at 0725h, 0728h, 072Bh, 072Eh and 0731h, inside the unused 25th row of the ASCII screen copy.
  - 072Bh is then set to `JP 0734h`, and the 8-byte reboot stub from SYS C338h (`DI / OUT (2),70h / JP C21Ch`) is copied to 0734h.
  - Callers: `PAR_CHOUT` → 0725h; RS-232 character I/O → 0728h; BASIC → 072Eh (F359h) and 0731h (DD25h).
  - The fill happens when 0EB3h was 0 (C2D1h–C2E0h); that this means "cold start" is ❓.

## 3. Boot (reset → cartridge or BASIC)

Cross-checked with [OS] §6 (summary in [tvc-hardware.md](tvc-hardware.md#boot-os-6) §3 "Boot"). All R; page-0 addresses refer to the SYS image running in page 0.

| Step | Address | What it does |
|---|---|---|
| 1 | 0000h → 021Ch | `DI / IM 1 / OUT (2),40h` (SYS in pages 0 and 3) |
| 2 | 0226h → EXT E14Bh | `OUT (2),C0h`. EXT pages in U0 (D0h) and compares 30 bytes at 0B23h with its own copy at E279h (is RAM still intact?). `WARM_FLAG` (0B21h) becomes FFh only if all 30 match and it was 0, otherwise 0. The result also goes to A', A = C0h is restored, then `JP 022Dh` |
| 3 | 022Dh → C234h | `OUT (2),40h`; continue in page 3; `OUT (2),50h`; ports 60h–63h ← 0 (palette); CRTC R15…R0 from **C20Ch**: 63 40 4B 32 4D 02 3C 42 00 03 03 03 00 00 0E FF for R0…R15 (= the table in [tvc-hardware.md](tvc-hardware.md#6845-usage) §5, confirmed from the ROM bytes); port 06h ← 80h |
| 4 | C253h | `COLD_FLAG` (0B22h) = FFh forces a cold start (`WARM_FLAG` = 0). A warm start skips the RAM tests (C25Dh, entered inside `LD A,08h`) |
| 5 | C261h | SP = BFFFh, test U0 (C34Dh: fill/compare with AAh, then 55h). On error, A cycles into port 00h (border) forever |
| 6 | C271h | SP = 16ACh, test VRAM at 8000h |
| 7 | C280h | `OUT (2),70h`; test 4000h, then 8000h if U1 passed; `HI_MEM` = HL − 1 ([OS]: 7FFFh or BFFFh) |
| 8 | C291h → 029Bh | back to page 0; `OUT (2),80h` (U3 in page 3); test C000h; A' = 00h/FFh; then C2BBh |
| 9 | C2C5h | `BAS_VERSION` (0EB7h) = 22h (2.1: 21h) |
| 10 | 0340h → EXT E000h | `OUT (2),D0h`; on a cold start `P3RAM` = A' (U3 result); copy the RAM stubs (§2); clear `PROGID` (0E98h–0EABh); set defaults (0B10h = FCh, 0B13h = 80h, 0B1Fh, 0B17h = 0F23h); card scan |
| 11 | E045h– | **card scan**: `IN A,(5Ah)`, two bits per slot; `OUT (3),slot<<6` selects the IOMEM. The name for the slot's table entry: ID 00 → "RS232", 01 → **"VGB"** (not in [OS] ❓), 10 → "DISK" (length-prefixed at E2A3h–E2B1h), 11 → the name in the card's own header after `MOPS` at C000h (E06Eh). For a non-00 ID the card's init is called through **C00Bh** (E109h). Matches [tvc-hardware.md](tvc-hardware.md#boot-os-6) §3 "Boot" step 7 |
| 12 | EXT → C2D1h | hooks and reboot stub (§2); C2FDh: CA3Ah ❓ |
| 13 | C300h / 030Dh | if 0EB5h ≠ 0, start BASIC directly. Otherwise `OUT (2),20h` (CART in page 3) and compare C000h with `MOPS` (0334h); a match jumps to **C004h** |
| 14 | C329h | `OUT (2),70h`, `P_SAVE` = 70h, `EI`, `JP CEC2h` (BASIC cold start) |

## 4. System calls (RST 30h)

Path (R, C):

1. `RST 30h` → 0030h `JP 0B23h`.
2. `INFUNC`: `EX (SP),HL / LD A,(HL) / INC HL / EX (SP),HL` takes the inline code and steps past it. The code goes to A'. It pushes the caller's AF' and `P_SAVE`, then `P_SAVE` = 70h, `OUT (2),70h` (U0, U1, U2, SYS) and `JP C418h`.
3. C418h pushes `OUTFUNC` (0B37h) as the return address. `OR 37h` clears carry and skips C41Eh, the `SCF` entry used by the IRQ path.
4. C41Eh saves IX, IY, HL and the alternate set. Carry clear → C372h; carry set → IRQ service C43Eh.
5. C372h (decode):
   - b7 selects `IN_TABLE` (1) or `OUT_TABLE` (0); b6–4 = class; b3–0 = function.
   - Class 7 (kernel) is never redirected.
   - For classes 0–6 the table byte is read:
     - FFh with function 0–2 → error **FEh**.
     - Byte & 7Fh ≥ 7 → error **FEh**.
     - **Bit 7 set** → `LD HL,E174h / JP FFF5h`: card in the slot given by the byte, for every function.
     - Otherwise the byte replaces the class **only for functions 0–2**; functions 3–15 always run on the class's own device.
     - Device 6 (card class) → `LD HL,E17Ah / JP FFF5h`.
   - This is exactly [OS] §4.3's step list (OS p.36–37).
6. C3CBh: `dev_table` (**C538h**, 8 pointers) → `DEFB count / DEFW handler…`. Function ≥ count → error **FFh**. Otherwise C321h `JP (HL)`.
7. Return: C3EFh (`OR A / POP HL / RET Z`; on error it involves EXT E1F4h, which reloads a default assignment from E259h/E261h ❓). Then `OUTFUNC` restores `P_SAVE`, port 02h and AF'.

**Video, editor, sound and cassette run in EXT**: their SYS tables point every function at a stub `EXX / LD DE,<EXT table> / JP CA29h`. CA29h is `LD HL,F179h / JP FFF5h`. EXT's F179h indexes the plain word table at DE with code & 0Fh, calls it, and returns to SYS through CA19h (`RET`).

**Card class** (EXT E174h/E17Ah; R):

- Slot < 4, else error. `OUT (3),slot<<6` (`PORT03` mirror 0B11h).
- The name entry is at 0040h + 30h·slot (FFh = no card).
- Name `RS232` → EXT table **E2BDh**. Any other name → the card's own table at **C00Dh** (count) / **C00Eh** (words): IRQ, char I/O, block I/O, open, close, verify, … ep128emu's `tvcfileio.s` header has the same layout (C00Bh init, C00Dh = 15).
- Handlers are entered with **IX = 0048h + 30h·slot**, the card's 40-byte buffer in its name-table entry. That is why `tvcfileio.s` can derive the slot from IX.
- Exit is through C3EFh in SYS.

**IRQ** (R): `IT_ENTRY` → C405h saves `P_SAVE`, DE, BC and AF', then C41Eh with carry set → C43Eh:

- `INT_FLAG` = FFh, `INTINC`++, `BORDER` → port 00h, reads port 59h and writes port 07h.
- Function 0 of every device (C48Bh); card IRQs (EXT E235h polls the four slots; the role of `IRQ_STAT` there ❓).
- Exit through `INT_EXIT` (0B41h, ends with `EI / RET`).

**Error codes** (OS p.162; the ROM returns them in A, NZ):

| Code | Source |
|---|---|
| FFh | no such function |
| FEh | assignment |
| FDh | no such card |
| FCh / FAh | bad block I/O parameters |
| FBh | not enough memory |
| F9h–F7h | video |
| F6h | editor |
| F5h | STOP |
| F4h–F2h | serial |
| ECh–E5h | cassette ([tvc-hardware.md](tvc-hardware.md#8-cassette) §8) |
| E3h | video function 0Dh (not in [OS] ❓) |

## 5. Function table (SYS 2.2 / EXT 2.x)

The handler addresses are C; the names and codes are from OS p.149–159 unless marked. One handler serves both directions; it tests b7 itself.

| Code(s) | Name | Dev/fn | Handler | Notes |
|---|---|---|---|---|
| 00 | video IRQ | 0/0 | EXT F1B7h | `RET` |
| 01 | VID_CHOUT | 0/1 | EXT F4D4h | C = character |
| 02 | VID_BKOUT | 0/2 | EXT F4C6h | DE, BC |
| 03 | BTEXT | 0/3 | EXT F79Ch | B = column, C = row; F9h |
| 04 | VMODE | 0/4 | EXT F226h | C = 0/1/2; **C ≥ 3 → F7h** (R) |
| 05 | CLS | 0/5 | EXT F27Bh | |
| 06 | BABS | 0/6 | EXT F311h | DE = Y, BC = X; F9h |
| 07 | BREL | 0/7 | EXT F30Eh | |
| 08 | BON | 0/8 | EXT F42Fh | |
| 09 | BOFF | 0/9 | EXT F43Fh | |
| 0A | FILL | 0/10 | EXT F59Ch | used by `PLOT … PAINT` |
| 0B | DEFC | 0/11 | EXT F77Dh | DE = matrix, C = code; F8h |
| 0C | PAL | 0/12 | EXT F26Ah | DE = 4-byte palette |
| 0D | (polygon) ❓ | 0/13 | EXT F835h | not in [OS]; used by `PLOT … POLYGON` (E137h); parameter block at DE, error E3h |
| 0E | (ellipse) ❓ | 0/14 | EXT FA2Ah | not in [OS]; used by `PLOT … ELLIPSE` (E1B3h) |
| 10 | keyboard IRQ | 1/0 | SYS CB34h | matrix scan |
| 91 | KBD_CHIN | 1/1 | SYS CA91h → EXT EB1Dh | F5h on STOP |
| 92 | (block in) | 1/2 | SYS CAC7h | `RET` |
| 93 | KBD_STATUS | 1/3 | SYS CA84h | C = FFh if a key is waiting |
| 14 | (function key) ❓ | 1/4 | SYS CAC8h | not in [OS]; used by `FKEY` (DAA9h) |
| 20/A0 | ED_INT | 2/0 | EXT EF86h | cursor blink |
| A1 / 21 | ED_CHIN / ED_CHOUT | 2/1 | EXT EBCEh | |
| A2 / 22 | ED_BKIN / ED_BKOUT | 2/2 | EXT EBBDh | [OS] gives ED_BKOUT's decimal as 162, a typo for 34 |
| 23/A3 | CPOS | 2/3 | EXT EB99h | F6h |
| 24/A4 | CFIX | 2/4 | EXT EB8Fh | |
| 30 | sound IRQ | 3/0 | EXT FF48h | |
| 31, 32 | – | 3/1, 3/2 | EXT FF6Ch | `RET` |
| 33 | TONE_SET | 3/3 | EXT FF6Dh | DE = pitch, C = amplitude, B = duration (20 ms) |
| 40 | printer IRQ | 4/0 | SYS CE9Dh | |
| 41 | PAR_CHOUT | 4/1 | SYS CE75h | calls hook 0725h |
| 42 | PAR_BKOUT | 4/2 | SYS CE6Fh | block loop C548h |
| 50 | cassette IRQ | 5/0 | EXT E546h | |
| D1 / 51 | CAS_CHIN / CAS_CHOUT | 5/1 | EXT E5B4h | |
| D2 / 52 | CAS_BKIN / CAS_BKOUT | 5/2 | EXT E651h | |
| D3 / 53 | CAS_OPEN / CAS_CRTE | 5/3 | EXT E421h | WinTVC trap slot 2 is patched here (W) |
| D4 / 54 | CAS_CLOSE | 5/4 | EXT E579h | |
| D5 | CAS_VERIFY | 5/5 | EXT E626h | the built-in cassette has 6 functions, so **D6h+ return FFh** unless the class is assigned to a card |
| 60 | serial IRQ | 6/0 | EXT E336h | card class, name RS232; `RET` (the card has no interrupt) |
| E1 / 61 | SER_CHIN / SER_CHOUT | 6/1 | EXT E369h | calls hook 0728h |
| E2 / 62 | SER_BKIN / SER_BKOUT | 6/2 | EXT E3F4h | |
| E3/63 | RS232_SET | 6/3 | EXT E2D2h | `BAUD`, `FORMAT` |
| E4/64 | RS232_CLOSE | 6/4 | EXT E396h | |
| 70 | kernel IRQ | 7/0 | SYS C404h | `RET` |
| 71/F1 | HI_MEM_SET | 7/1 | SYS C4B4h | **floor 1EACh, FBh** = [OS] §7.1 (1EACh = 7852) |
| 72/F2 | EXP_DIRECT | 7/2 | SYS C4CDh | FDh (OS §7.2) |
| 73/F3 | IO_DIRECT | 7/3 | SYS C4DEh | FEh (OS §7.3) |
| 74/F4 | SLOT_NUM | 7/4 | SYS C501h | FDh (OS §7.4) |

The class-0…4 IRQ functions are listed with their output codes; the dispatcher reaches them as function 0 from the IRQ loop.

**Printer and RS-232 drivers** (Z, checked by runs through them in freeTVC; the emulated port and card are in [machine.md](../design/machine.md) and [cards.md](../design/cards.md)):

- `PAR_CHOUT` (SYS 2.2 CE75h, after hook 0725h): waits for 59h b7 = 1, checking STOP (flag 0B16h) at CAC0h; then `DI`, `OUT (01h),C`, 06h with b7 = 0 then b7 = 1 from the mirror 0B13h, `EI`. The ROM writes 06h only with b7 = 1 elsewhere (SYS C24Fh, EXT E036h/E138h at boot, F25Ch mode changes keep it), so it never strobes by itself. STOP is CTRL+ESC (row 7 b4 and b3).
- `RS232_SET` (EXT 2.x E2D2h; EXT 1.x F2A4h): amplitude bits of 0B13h cleared, pitch from the BAUD (0B69h) table (2.x E403h, 1.x F3C6h: 0C88h, 0D75h, 0EBAh, 0F5Dh, 0FAFh, 0FD7h, 0FECh, 0FF6h, 0FFBh) with the tone on, then for every slot with ID 00: 00h, 00h, 00h, 40h, mode (FORMAT 0B6Ah & B4h) | 4Ah, and **on 2.x only** the command 05h; SER_OK (0B71h) = 0.
- `SER_CHOUT` (2.x E36Ch–E397h, 1.x F339h–F358h): through hook 0728h; waits for status b7 (DSR) and b0, re-runs `RS232_SET` when SER_OK ≠ 0, **writes the command 05h before every character** (2.x E386h, 1.x F347h), waits for b0 again, writes the data.
- `SER_CHIN` (2.x E398h–E3F3h, 1.x F359h–F3B6h): a waiting character is read at once; otherwise `DI`, keyboard row 7, command 25h (RTS on), poll status b1 while checking CTRL+ESC at 58h & 18h (STOP: F5h, sets 0B62h b3), then 05h (RTS off), status, data, `EI`; status bits 38h give F4h (parity), F2h (overrun), F3h (framing) after the command 11h (error reset).
- So SYS 1.2's missing enable in `RS232_SET`, which the OS manual and the card's manual answer with `OUT 17,5` (slot 0), does not stop BASIC: its own character I/O enables the chip. Only a program that drives the 8251A itself after `RS232_SET` on 1.2 needs it. The manuals may describe an earlier EXT 1.x ❓.

## 6. BASIC layer

- **Statement dispatch** (D0E9h; R, C): a token ≥ CAh takes the word at C023h + 2·(FFh − token).
  - The keyword table at **D73Ah** (last character + 80h) uses the same index: entry k = token FFh − k.
  - Statements are tokens FDh (`:`) … CCh (`EXCEPTION`); CBh/CAh are unused placeholders.
  - Pairs that confirm the mapping: `LPRINT` (DEC5h) falls into `PRINT` (DEC8h, also `OUTPUT`), and `LLIST` (D650h) into `LIST` (D655h). `REM` (D126h) also serves tokens FEh (`!`) and FFh.
  - The function and operator tokens (`AND` … `+`, from C9h down) are dispatched elsewhere, not traced yet.
- **RST 18h** (F7DBh; R): a threaded-code interpreter.
  - It swaps the return address with `BAS_IP` (1718h) and reads op bytes. Each op calls the word at **C08Bh** + 2·(op & 7Fh), patched into the `JP` at 0015h.
  - It stops after the first op with **bit 7 set** (carry from `ADD A,A`, `POP AF / JR NC`) and returns after it.
  - **Op 05h** reads one operand byte (E607h, a byte constant), also as the last op (85h): the exit (F810h) returns to `BAS_IP`, after the operand. No other op takes operand bytes: across SYS 2.2, SYS 1.2, EXT 2.x and EXT 1.x only the interpreter and op 05h address `BAS_IP` (1718h) (byte scan; the EXT 1.x hit at 03DBh is a `JR`), and stepping over the RST 18h at EAB8h (ops `05 22 05 22 80`) and EAC7h (`8A`) returns at EABEh and EAC9h (R, D). 1.2 is the same at FB82h–FBB8h and EA82h (R).
  - Ops 00h–2Eh are named `bas_op_xx`; their meanings are not analysed.
- **Output**: `CALL FB78h` is followed by a length-prefixed string (prints it, returns after it). FB99h prints A through the vector at **0712h** (default FBAAh).
- **Errors**: RST 08h n / RST 10h → F9D6h with A = number. Messages at FACAh: number, length, text (bytes ≥ 80h expand to keyword-table words, e.g. 91h → "Bad "), FFh. Error 10h = "Bad file".
- **Program files** (R):
  - `SAVE` (D7h → E519h) builds the 16-byte program header at 19DFh–19EEh: byte 0 = 00h, 1 = 01h, 2–3 = length (end − `TEXT`), 4 = `BAS_AUTO`, the rest 00h. It writes the header with 16 character outputs, then the body with one block output.
  - `LOAD` (E8h → E4E8h → E5C1h) reads 16 bytes and accepts the header only if **byte 0 = 00h and byte 1 = 01h** (otherwise error 10h). It then reads the body with a block input; with byte 4 ≠ 0 it jumps to `RUN` (D6F5h), else to READY (D00Ch).
  - Compare [file-formats.md](file-formats.md#cas--program-file-vt-dos-on-disk-format-de-facto-exchange-format) (CAS program header).
- **PLOT sub-keywords** (E09Ah): `POLYGON` (ABh) → E100h → video 0Dh; `ELLIPSE` (ADh) → E13Bh → video 0Eh; `RECTANGLE` (ACh) → E1B7h; `PAINT` (BEh) → `FILL`.

## 7. Cassette routines (EXT 2.x)

The function entries are in §5. Inner routines, found from the call tree, with the WinTVC trap sites as an independent check (W, found from WinTVC's patch descriptors):

| Address | Routine | Evidence |
|---|---|---|
| E421h | `CAS_OPEN` / `CAS_CRTE` entry | C, W (trap slot 2) |
| E5ADh | `LD HL,0` inside `CAS_CLOSE` (E579h–E5B3h) | R, W (slot 4) |
| E7B1h | leader/sync search of the block read | W (slot 0); R: inside the traced cassette code |
| E965h | bit-read loop of the byte read | W (slot 3); R |
| EA84h | `LD B,8` bit-write loop of the byte write | W (slot 1); R |
| E8C3h / EA4Ch | read / send the CRC, low byte first | [tvc-hardware.md](tvc-hardware.md#8-cassette) §8; R |
| EAFCh–EB12h | CRC update per bit | [tvc-hardware.md](tvc-hardware.md#8-cassette) §8 |

**How the block read times the signal** (Z = z80dasm of `ext2x.bin` at E000h; D = traced in freeTVC with debugger checkpoints at E7DBh, E7E4h, E829h, E830h and E837h while it read a real tape recording, [media.md](../design/media.md)):

- **Measuring.** Two routines measure the tape signal: E984h times a full cycle (it waits for two edges, first to the level that register C names, then back) and E977h the time to the next edge (one edge; it also sets C from the level it finds, E977h–E97Dh). Each starts by waiting for one sound interrupt with `EI` / `HALT`; the end of the previous measurement restarted the sound generator with `IN A,(5Bh)` and cleared the interrupt flip-flop with `OUT (07h)` (E99Eh–E9A0h). After that wait, a 48 T polling loop (E988h–E991h and E993h–E99Ch) reads the tape input bit (59h b5) and counts register E up once per pass, 15.36 µs a pass, until the edge. Each measurement also steps the border colour between 88h and A0h (E9A2h–E9B3h): the ROM's loading stripes. The count is time, not interrupts (D: leader cycles of about 441 µs give E = 16–17, which is (441 µs − the interrupt wait) / 15.36 µs; counting interrupts every 123–184 µs would give 2–4).
- **Finding the sync** (Z, from E7A3h). With the pitch set to DCh (E7ADh), the reader averages the counts of 32 cycles into D (E7B4h–E7C7h), then requires 1024 cycles in a row (HL = 400h) each within ±2 of that average (E7D5h–E7DDh); any other cycle restarts the search at E7A3h (D: 2–4 restarts per half second on a jittery leader). With the pitch set to E8h (E7F7h) it then sums 256 half cycles alternately into two totals, which gives an average for each kind of half (E7FEh–E818h), and compares the following halves with their kind's average in turn (E819h–E82Eh): a half 3 or more counts shorter restarts the search (E829h), one 4 or more longer is the sync cycle (E830h). Back at pitch DCh, E977h takes C from the half after the sync (E834h), and the byte reads at E965h use that level for the whole block (D: C = 00h or 20h at E837h; with unequal halves the wrong choice misreads the block).
- The counts depend on the sound-interrupt wait ([tvc-hardware.md](tvc-hardware.md#6-sound) §6): if the high nibble in 05h is Fh, as in the writer's pitch table (FAF3h–FAF6h), pitches E8h and DCh give 384 T and 576 T (122.9 µs and 184.3 µs), plus the interrupt service before the loop starts (❓ the reader's 05h value and the service length were not checked).

## 8. Data areas (SYS 2.2 unless noted)

| Range | Content |
|---|---|
| 0003h–0021h (page 0 view) | copyright text "Copyright (1987) IS Systems Ltd" |
| C023h–C0E8h | statement table (52 words) + RST 18h op table (47 words) |
| C20Ch–C21Bh | CRTC init values |
| C574h–C8D3h | character bitmaps, 10 bytes per character |
| CD28h–CE67h | keyboard code tables |
| CFA4h | "TV COMPUTER BASIC" (PROGID source) |
| D73Ah–D8E7h | keyword table (113 entries incl. message words " missing", "rgument", "Bad ", "No ", "Cannot") |
| F7B4h–F7DAh | BASIC RAM vectors (copied to 0008h) |
| FACAh–FB77h | error messages |
| EXT E259h–E2C7h | RAM-init blocks (§2), `MOPS` (E29Fh), card names "VGB", "RS232", "DISK" (E2A3h), RS-232 table (E2BDh) |
| EXT ECD3h | editor control-code table: code, handler pairs ❓ (not traced) |

## 9. Results for the other docs

- [tvc-hardware.md](tvc-hardware.md#card-iomem-rom-header-c000h) §3 "Card IOMEM ROM header": **confirmed** from the ROM. C00Bh init, C00Dh function count, C00Eh+ handlers; IX = card buffer; ep128emu's extended cassette codes D6h–DEh only reach a card.
- [tvc-hardware.md](tvc-hardware.md#boot-os-6) §3 "Boot": confirmed; addresses in §3 here.
- [tvc-hardware.md](tvc-hardware.md#6845-usage) §5: ROM CRTC values confirmed from the ROM bytes.
- [file-formats.md](file-formats.md): ROM `LOAD` checks header bytes 0 and 1 (§6).
- `HI_MEM_SET` floor, RAM-variable map: [OS]'s 1.x-era map holds for 2.2. Every variable in `rom-symbols/tvc-ram.sym` tagged "2.2" is referenced by the 2.2 code.

## 10. Coverage and open items

- Trace coverage: SYS 2.2 73.8 % of the bytes as code (6 909 instructions), EXT 2.x 83.0 % (3 678). The rest is data (§8) plus the open items below.
- ❓ / not done:
  - SYS 2.1's BASIC part (SYS 1.2 / EXT 1.x: §11).
  - BASIC function list: a linked list of `next, length, name, type byte` headers followed by code, from E73Bh (DEC, HEX$, PEEK, STR$, USR …). Function/operator token dispatch.
  - RST 18h op meanings.
  - EXT editor control-code table (ECD3h) and some EXT video code (EF07h–EF85h, F043h–F075h, F0C0h–F178h, FD6Ah–FE88h), reached through RAM vectors (`JP (IX)`/`JP (IY)` via 0B84h/0B86h/0B93h/0B94h).
  - SYS C55Ch (block-input loop): no caller found. CA3Ah and C8D4h (boot): purpose not read. The 0EB3h/0EB5h flags.
  - Error path EXT E1F4h.
  - Video functions 0Dh/0Eh and keyboard function 4: named from BASIC usage only.
- Reproduce, from the project root, with the ROMs in `testdata/roms/` (the autofetch, [build.md](../design/build.md#rom-autofetch)) and z80dasm installed. The configs `tools/re/tvc_roms.json` and `tvc_roms12.json` read the images from `testdata/roms` (their `"root"`; the EXT 1.x entries take the upper half of the 8 KiB socket image). The output goes to a work folder `W` outside the repository:

```sh
W=<work folder>
# SYS 2.2 / EXT 2.x: trace, symbol check (246 symbols, 144 table checks, 0 problems), listing
python3 -I tools/re/check_rom_syms.py tools/re/tvc_roms.json "$W"/trace docs/hardware/rom-symbols
z80dasm -a -t -l -g 0xc000 -b "$W"/trace/sys22.blk -S "$W"/trace/sys22.sym -o "$W"/trace/sys22.asm testdata/roms/sys22.bin

# SYS 1.2 / EXT 1.x (§11): symbol check (130 symbols, 69 table checks, 0 problems), overlaps and gaps, listings
python3 -I tools/re/check_rom_syms12.py tools/re/tvc_roms12.json "$W"/trace12 docs/hardware/rom-symbols
python3 -I tools/re/z80trace_audit.py tools/re/tvc_roms12.json "$W"/audit12 sys12 sys12lo ext1x
z80dasm -a -t -l -g 0xc000 -b "$W"/trace12/sys12.blk -S "$W"/trace12/sys12.sym -o "$W"/trace12/sys12.asm testdata/roms/sys12.bin
tail -c 4096 testdata/roms/ext1x.bin > "$W"/ext1x.bin          # the 4 KiB EXT 1.x ROM, CRC-32 59C6B9F9
z80dasm -a -t -l -g 0xf000 -b "$W"/trace12/ext1x.blk -S "$W"/trace12/ext1x.sym -o "$W"/trace12/ext1x.asm "$W"/ext1x.bin
```

## 11. SYS 1.2 and EXT 1.x

Traced with `tools/re/tvc_roms12.json`. The symbols are in `rom-symbols/tvc-sys12.sym`, `tvc-sys12-page0.sym` and `tvc-ext1x.sym`. `tools/re/check_rom_syms12.py` checks them:

- 130 symbols and 69 table checks, 0 problems.
- A mutation test (one label moved inside an instruction, one handler swapped) is reported with exit 1.
- The checks include the five EXT 1.x tape-trap sites from WinTVC's patch descriptors (W). All five are instruction starts with the recorded bytes.

The 1.x set has the same architecture as 2.x:

- the same RAM stubs and `IN_TABLE`/`OUT_TABLE` values;
- the same RST 30h decode rules, BASIC RST 08h/10h/18h vectors and inline-data conventions;
- the same card names and card dispatch.

What moves are the addresses, and the split of work between the two ROMs: **in 1.x, video, editor, keyboard and sound are in SYS, and only the cassette, RS-232 and card code plus BASIC number-output extensions are in EXT**.

**Coverage**: SYS 1.2 74.8 % of the bytes as code (7 053 instructions), EXT 1.x 84.8 % (1 892). Everything that remains is explained:

- Overlaps: all 18 in SYS (17 in page 3, 1 in page 0) and all 4 in EXT are hidden-entry tricks.
  - An operand hides an instruction: `LD BC,553Eh` hides `LD A,55h` in the RAM test; `LD A,08h` hides `EX AF,AF'` on the warm-start path.
  - `OR 37h` hides `SCF`.
  - Never-taken `JP cc` skips at E5F1h (target A0FEh) and EXT FE71h (target 0C48h).
  - A `DJNZ` loop body sits in an operand at CA88h.
- External targets: the BASIC RAM vectors (0015h, 001Bh, 001Eh), the RAM stubs (0B37h, 0B41h), the RAM vector 0E68h, and the two never-taken skips.
- Indirect jumps: the dispatcher (C321h), the statement dispatch (DBADh), the editor control codes (D16Fh), the VRAM-paged call wrapper (C9A0h), the video RAM vectors (CAC3h/CACBh via `JP (IY)`, CBB3h/CBBBh via 0B84h/0B86h), a table search at C58Eh (target not read ❓), and the bridges.
- Data, and code not yet reached: the font, keyboard tables, keywords, messages and RAM-init blocks (symbol files), plus the BASIC function bodies (EB80h onwards, the same open item as 2.2).

| Item | SYS 1.2 / EXT 1.x | SYS 2.2 / EXT 2.x | Evidence |
|---|---|---|---|
| Reset / boot | 0000h `JP 0229h`; `DI / IM 1 / OUT (2),40h`; 0233h `OUT (2),C0h` → EXT **F13Dh** (RAM-intact check) → back via F163h `JP 023Ah` → C241h (palette, CRTC from **C545h**, values as 2.2) | 0000h → 021Ch; E14Bh; C234h | R |
| EXT init | 02D0h → **F000h**: stubs from FB0Eh/FB1Eh/FB2Eh, defaults (0B10h = FCh, 0B13h = 80h, 0B1Fh = 01h, 0B17h = **0F10h**), card scan F050h | E000h; 0B17h = 0F23h | R |
| Device init | table **C555h**, 4 calls through C321h: video C9F2h, keyboard D5ECh, sound D960h, cassette reset D9E2h → EXT F3F1h → F707h | – (not a table in 2.2 ❓) | R, C |
| BASIC start | C329h: `OUT (2),70h`, `P_SAVE` = 70h, `EI`, `JP D9EFh` | C329h → CEC2h | R |
| Bridges | **FFF0h** in both ROMs (SYS: A = F0h, EXT: A = 70h) | FFF5h | R |
| RAM stubs | IRQ → **C412h**, `INFUNC` → **C363h**; layout 0030h/0038h, 0B23h/0B37h/0B41h unchanged | C405h, C418h | R |
| Dispatcher | C37Eh; device table **C55Dh**; cards → EXT F166h / F16Ch. Extra first step: `IN_TABLE[1]` = 2 is turned into 1 (C384h–C38Ch) ❓ purpose | C372h, C538h, E174h/E17Ah | R, C |
| Classes | video C974h (13 functions: no 0Dh/0Eh), keyboard D5E3h (4: no function-key call), editor CF98h (5), sound D92Ah (4), printer D8FFh (3), cassette D9BBh (6, stubs into EXT F3D8h–F3ECh), kernel C4ACh (5); RS-232 table EXT F291h (5) | 15, 5, 5, 4, 3, 6, 5; E2BDh | C |
| Cassette | `CAS_OPEN` F3FAh, `CAS_CLOSE` F558h, `CAS_CH` F593h, `CAS_VERIFY` F605h, `CAS_BK` F630h; sync search F798h, bit read F919h, bit write FA3Dh; buffer clear F707h (0BF0h–0E34h) | E421h, E579h, E5B4h, E626h, E651h | C, W |
| BASIC vectors | FB5Bh → 0008h (27h bytes, copied at DA0Bh): RST 10h → **FD5Ch**, RST 18h → **FB82h** | F7B4h; F9D6h, F7DBh | R |
| RST 18h | op table **C0C7h**, only **15 ops** (00h–0Eh); op 05h EA82h reads one operand byte | C08Bh, 47 ops; E607h | R |
| Statements | dispatch DB92h, table **C067h**, 48 entries (tokens FFh..D0h); keyword table DE6Dh (starts like 2.2's D73Ah) | D0E9h, C023h, 52; D73Ah | R |
| Inline print | `CALL FE79h` + length-prefixed string | FB78h | R |
| Error messages | FDC6h (number, length, text) | FACAh | R |
| Mode vector | 0E68h = `JP` + 4 bytes from table D007h by video mode (0B73h): D3ADh/40h/01h, D3BCh/20h/02h, D3D9h/10h/04h (routine, text columns, bytes per character) | – | R, C |
| Editor control codes | table D170h, 11 × (code, handler): 13h, 04h, 05h, 18h, 16h, 07h, 08h, 19h, 0Eh, 0Bh, 09h; dispatch D157h | EXT ECD3h (not traced) | R |
| Number output | SYS E647h/E66Dh continue in EXT 1.x at FF9Ch/FCB5h with a conditional bridge jump (`JP NZ,FFF0h`); about 800 bytes of EXT code (FCB5h–FFE7h) formatting numbers ❓ (e.g. `PRINT USING`) | in SYS ❓ | R |

Open for 1.x:

- the C58Eh table search;
- the `IN_TABLE[1]` tweak;
- what the EXT block copied to 0740h/0880h is;
- the number-output extension's exact role;
- the BASIC function list;
- the patch areas of WinTVC's Cyrillic preset, which overwrite parts of both ROMs (static analysis of WinTVC).

## 12. Cassette functions and `LOAD`

What the host cassette ([media.md](../design/media.md)) needs to stand in for the cassette class. [OS] chapter 13 (OS p.116–121) gives the ABI; the ROM readings below fill in what it leaves out.

**Functions** (OS p.116–120; Z: EXT 2.x E421h–E6FCh, EXT 1.x F3FAh–F707h):

| Code | Function | In | Out |
|---|---|---|---|
| D1h / 51h | CAS CHIN / CHOUT | – / C = character | C = character |
| D2h / 52h | CAS BKIN / BKOUT | DE = buffer, BC = count | BC = bytes not moved (on error) |
| D3h / 53h | CAS OPEN / CRTE | DE = length-prefixed name | DE = the found / converted name |
| D4h / 54h | CAS CLOSE (read / write) | – | – |
| D5h | CAS VERIFY | DE = memory, BC = length | buffered: DE, BC at the differing byte |

- Success: A = 0. Error: A = code, NZ (the exit path's `OR A`, SYS 2.2 C3EFh, Z). Codes E5h–ECh (OS p.121): ECh end of file, EBh already open, E9h not open, E8h verify, E7h internal (header over 16 bytes, bad byte count), E6h protection.
- Unbuffered files: CHIN/CHOUT reach only the header block's at most 16 data bytes; one BKIN/BKOUT moves the data block. Buffered files go through 256-byte sectors (OS p.113).
- Name conversion (Z: EXT 2.x E547h): at most 16 bytes; 61h–7Ah and 90h–98h lose 20h / 10h.

**Workspace**, the same addresses in EXT 1.x and 2.x (Z: 2.x E42Ah–E4EBh and E4FEh–E543h; 1.x F403h–F525h):

| Address | Content |
|---|---|
| 0BF3h | read file type: 0 = none open, 01h buffered, 11h unbuffered (the tape header's type byte = CAS byte 00h) |
| 0BF4h | read file name, length-prefixed (OPEN returns DE = 0BF4h) |
| 0D14h / 0D15h | the same for the write file (CRTE returns DE = 0D15h) |
| 0B6Bh `BUFFER` | after OPEN: type − 11h, so 0 or F0h (Z: E4B8h); before CRTE: non-zero = buffered (Z: E50Dh) |
| 0B6Dh `PROTECT` | read and cleared by CRTE (Z: E51Dh) |
| 0B6Eh `EOF` | FFh after the last byte (Z: E5DAh, E620h) |
| 0B6Fh `MUDDLE` | cleared by both CLOSEs (Z: E5ADh) |

- An unbuffered BKOUT closes the write file itself (Z: E6FCh `JP E587h`); BASIC's `SAVE` then calls CLOSE 54h, which succeeds on a closed file (Z: E579h–E5A4h).
- Open-file checks read 0BF3h and 0D14h (Z: E77Eh, E787h), so a reset that clears the workspace closes everything.

**Error exit** (Z: SYS 2.2 C3F2h → EXT 2.x E1F4h; EXT 1.x F1E6h): unless the call came from the IRQ service (`INT_FLAG` 0B20h = FFh), the calling class's entry in `IN_TABLE` (0B00h) or `OUT_TABLE` (0B08h) goes back to its default, except when a card holds it (bit 7). A device byte 06h first copies 0B1Ch to the table's entry 7 (E22Ch ❓ purpose). The defaults are identical in both EXTs: IN `FF 01 02 FF FF 05 06 FF` (2.x E259h, 1.x FB0Eh), OUT `00 FF 02 FF 04 05 06 FF` (E261h, FB16h). This is how [OS]'s merge example (OS p.37–38: `OPEN "NAME":POKE 2818,5` feeds a listing to the editor) gets the editor back at end of file. D: reproduced with the host cassette ("ok" twice, the line merged).

**`LOAD`** (Z: SYS 2.2 E4E8h; matches §6):

| Address | Step |
|---|---|
| E4E8h → E5C1h | open; 16 × CHIN into 19DFh–19EEh; error 10h unless byte 0 = 00h and byte 1 = 01h |
| E4EBh → D6E2h | `BAS_TEXT` = `BAS_VLOMEM`, `(TEXT)` = 0, `JP D289h` (NEW) |
| E4F4h → F8F6h | `BAS_TOP` + length against IY and SP − (0B17h); error 06h |
| E4FAh | BKIN (function (1705h) \| 82h through the RAM stub 001Bh: code written to 001Fh, `RST 30h` at 001Eh), DE = `BAS_TEXT`, BC = header length |
| E508h → D289h | `BAS_CHAIN` = ECD7h, 1712h = `BAS_TEXT`, `BAS_TOP` = end of program (D2CBh) + 1 ❓ full role |
| E50Bh → E5E6h | CLOSE ((1705h) \| 04h), then `RST 10h` |
| E50Fh | header byte 4 ≠ 0 → `JP D6F5h` (RUN, which calls D289h again), else `JP D00Ch` (READY) |

- The class comes from the name (E585h → F84Ah): 50h cassette or 60h card.
- BASIC's error handler also closes the cassette: a refused file is closed without its block being read (D).
- Channel `#5` is the cassette class in BASIC (`OPEN OUTPUT "N":PRINT #5:…:CLOSE OUTPUT`, `OPEN "N":INPUT #5:…`; OS p.37; D). `#1` is not.
- SYS 1.2's `LOAD` was not read; its behaviour matched 2.2 on every check (D, all five presets).

**Prompt and logo** (D, all five presets): the start-up logo waits for a key through the keyboard class (first RST 30h 9xh, 2.2 frame 322); the first editor input (A1h, `ED CHIN`) follows about 15 frames after the key, at the BASIC prompt. Without a key no editor input happens within 600 frames.
