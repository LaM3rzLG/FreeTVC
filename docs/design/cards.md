# Expansion cards and cartridge-port devices

This document is the reference for every expansion card and cartridge-port device freeTVC emulates: the card slot model, then each device with its hardware, public sources, port and memory map, behaviour and quirks, how freeTVC models it, its front-end options, ROM or firmware, tests and known limits. The bus itself is described in [tvc-hardware.md](../hardware/tvc-hardware.md) (§9 "Expansion"); plain cartridges, snapshots and the host cassette are in [media.md](media.md); the main board's printer port is in [machine.md](machine.md) ("Printer port").

Evidence markers: ✓ verified, ❓ unknown or assumed, ⚑ needs a dynamic check, ⚠ a conflict between sources (the choice made is stated next to it).

## Overview

| Device | Kind | Default place | ID (port 5Ah) | Model | ROM file(s) |
|---|---|---|---|---|---|
| [HBF disk card](#hbf-disk-card) | slot card | slot 0 (10h–1Fh) | 10 | `src/core/cards/hbf.*`, `wd1793.*`, `floppy.*` | `hbf.rom` (VT-DOS 1.1), `hbf12.rom` (VT-DOS 1.2) |
| [GameCard](#gamecard) | slot card | slot 1 (20h–2Fh) | 11 | `gamecard.*`, `sn76489.*` | `gamecard.rom` |
| [TVC256k++](#tvc256k) | slot card | slot 0, else the first free slot | 11 | `card256*.*`, `vsid*.*` | `card256.rom`, `card256-font.bin`, `card256-picture.bmp`, `card256-flash.bin` |
| [Multichip sound card](#multichip-sound-card) | slot card | slot 2 (30h–3Fh) | 11 | `multichip.*`, `sn76489.*`, `saa1099.*`, `ym3812.*`, `chip_clock.hpp` | `multichip.rom` |
| [NanoSD](#nanosd) | slot card | slot 3 (40h–4Fh) | 11 | `nanosd.*` | `nanosd.rom` |
| [Serial card](#serial-card) | slot card | slot 1, else the first free slot | 00 | `i8251.*`, `serial_card.*` | none (the driver is in EXT) |
| [SID card](#sid-card) | ports E0h–FFh, outside the slots | – | – | `sid_card.*` on `third_party/resid/` | none |
| [MultiCart v2](#multicart-v2) | cartridge | cartridge port | – | `src/core/bus/multicart.*`, `jedec_flash.*` | `multicart-en.rom`, `multicart-hu.rom`, `multicart-es.rom` |
| [SD Dual](#sd-dual) | cartridge | cartridge port | – | `src/core/bus/sddual.*`, `sd_card.*`, `jedec_flash.*` | `sddual.rom`, `sddual-gm.rom` |

Paths without a folder are in `src/core/cards/`. Every ROM file is found by the ROM search (by name, or by CRC-32 under any name) and can be downloaded by the ROM autofetch from the list in `data/roms.txt` ([build.md](build.md), "ROM autofetch"; [ui.md](ui.md), "Settings file and ROM search"). None is in the repository. A card or cartridge without its ROM still has its hardware (ports, chips, RAM), but the machine never calls a card that has no `MOPS` header in its IOMEM window.

How the checks below were made: the unit tests run in CTest (`ctest`, tests labelled `core` need no ROMs; tests labelled `rom` need the ROMs and are skipped with exit code 77 when they are missing; see [developer-guide.md](../developer-guide.md)). "Checked by hand" means runs of `freetvc-headless` (screenshots, WAV output, `--moncommands`, MCP over stdio) and of the GUI on a virtual X display driven by synthetic key and mouse input with window captures (`tools/ui/ui_drive.py`, [tools/ui/README.md](../../tools/ui/README.md)). Where a test is said to pass under Wine, the MinGW build of that test program was run under Wine.

## Slot model

`src/core/cards/card.{hpp,cpp}`. A `Card` is one board in one of the four slots. Slot n (0–3) has:
- the slot's 16 ports at 10h + 10h·n to 1Fh + 10h·n, dispatched by `Machine::io_read`/`io_write`;
- the IOMEM window C000–DFFF while port 03h b7–6 = n and page 3 is IOMEM (port 02h b7–6 = 11); E000–FFFF stays the EXT ROM;
- a 2-bit ID at port 5Ah, slot n in bits 2n+1..2n: 00 serial, 01 (unnamed), 10 floppy, 11 none or a card named by its own ROM header;
- its /INT line: port 59h reads the four lines directly, and a write to 58h + n is the slot's /CLRINT strobe. The main board has no latch and no enable for these lines (HBA-2 schematics, see [TVC256k++ overlay](#overlay)).

`Card`'s interface (`card.hpp`):

| Member | Meaning |
|---|---|
| `kind()`, `id()` | the `CardKind` and the 2-bit ID |
| `power_on()`, `reset(tick)` | the machine's power-on and its reset button |
| `io_read`, `io_write` | the slot's ports 0–15, with a `CardBus` (machine tick, floppy drives, the audio synth for the bus SOUND line, the keyboard matrix, the TVC256k++ and NanoSD media, the tone generator for `TXRXCLK`, the serial line) |
| `mem_read`, `mem_write`, `window()` | IOMEM (reads have no side effects); `window()` is the 8 KiB the CPU sees at C000–DFFF, for debuggers |
| `save`, `load`, `clone` | the card's state in snapshots and machine copies |
| `advance_audio(tick, out)` | a card with sound brings it up to `tick` before the machine's audio is read out |
| `phantom_page(unit)` | 16 KiB served in place of RAM unit 2 or 3 (the card asserts /PHANTOM) |
| `border_write(value)` | port 00h writes, which every card sees on the bus |
| `overlay()` | the card that drives /EXTC and EC0–3 (only the first one is used: two would fight over the lines) |
| `irq()`, `clear_interrupt(value)` | the slot's /INT line and its /CLRINT strobe |
| `next_event()`, `run_events(bus)` | the tick of the card's next timed event (a chip timer that can pull /INT) and the catch-up to it |

`CardSlots` holds the four slots inside `MemoryMap` (which decodes the IOMEM window) and clones the cards when the machine is copied, so the prompt state, rewind points and snapshots copy cards like any other part. It keeps the earliest `next_event()` of all cards up to date after every call that can move it, and `Machine::advance_devices()` compares it once per T-state; cards without events are never called. `MachineConfig::cards` says what is in each slot (`CardKind` plus the card's EPROM image); `card_kind_name()` gives the names `none`, `hbf`, `gamecard`, `card256`, `multichip`, `nanosd`, `serial`.

**Empty slots** read FFh for their ports and their IOMEM window, and ignore writes, like other unmapped reads (open bus; YATE and MAME also read FFh) ❓. WinTVC 1.4.1 (observed under Wine) reads 00h there: slots 1–3 read 00h at ports x0h–x4h and x8h and at C000h/D000h. WinTVC 1.4.1 also puts the HBF disk card in slot 0 when it has no card configuration; freeTVC starts with no cards.

**Common behaviour of all cards and cartridges:**
- Switching a card in or out, from any front end, powers the machine on, like a cartridge. Inserting media into a card that is out (a disk, for example) puts the card in. The preset menu keeps the cards across a change of board.
- Snapshots carry the card kind with its EPROM image in the `SLOT` chunk (embedded like a cartridge) and the card's state in `CARD`, so a snapshot brings its cards along when it is loaded into a machine without them. Media that stand for host files (disks, SD cards, flash images, the pendrive folder) are **outside the machine state**: snapshots name them by path in `MEDI`, and rewind never undoes writes to them ([media.md](media.md), "Snapshots").
- The GUI's strip lights a slot rib for every inserted card; the Cards menu (opened from the slot ribs) switches the cards and the cartridge and chooses each slot card's slot ([ui.md](ui.md), "Strip menus"). The settings window lists them too, with slot buttons. The HBF card stays in slot 0.
- In the GUI, card keys in `freetvc.ini` (`yes` for the card's own slot, else the slot number) are written when they change, unless the command line set them for this run ([ui.md](ui.md), "Settings file and ROM search").
- The MCP `media` tool's `status` action reports every card.

## HBF disk card

Videoton's floppy disk card (HBF, 49111): an FD1793 (WD1793) at 1 MHz with a WD9216 data separator, up to four drives, 250 kbit/s MFM, a 16 KiB EPROM and 4 KiB of RAM. Hardware summary: [tvc-hardware.md](../hardware/tvc-hardware.md) §9.

Sources: the card's documentation and schematics (`floppydoc.zip` on tvc.homeserver.hu); the FD179X-02 data sheet (Western Digital, May 1980; http://www.bitsavers.org/components/westernDigital/FD179X-02_Data_Sheet_May1980.pdf, a scan, cited below as [DS] with its page numbers); for cross-checks, MAME's `src/devices/bus/tvc/hbf.cpp` (BSD-3-Clause; port roles, FD1793 at 16 MHz / 16) and the constants part of YATE's `Expansions/HBF/HBFCard.cs` (https://github.com/hightower70/YATE, LGPL-2.1-or-later; status bits, step delays, 4 ms index pulse, track layout), both read as documentation, no code copied. MAME's `wd_fdc.cpp` and ep128emu's `wd177x.cpp` were not read. The card ROM was read by disassembly.

`src/core/cards/hbf.{hpp,cpp}`, `CardKind::Hbf`, **slot 0** (ports 10h–1Fh), ID 10 (port 5Ah = FEh).

### Ports and memory

| Port | Write | Read |
|---|---|---|
| +0 to +3 | WD1793 command, track, sector, data | WD1793 status, track, sector, data |
| +4 to +7 | drive latch: b3–0 DS0–DS3 (the lowest set bit selects), b5 DDEN, b6 motor, b7 side | b7 DRQ, b0 INTRQ, **every other bit 0** |
| +8 to +11 | b5–4 EPROM page | FFh |
| +12 to +15 | nothing | FFh |

- Decoding is by A3–A2, as in MAME's `hbf.cpp`.
- IOMEM: C000–CFFF is a 4 KiB page of the 16 KiB EPROM, D000–DFFF 4 KiB of RAM. EPROM writes are ignored.
- The reset line clears the page register and the drive latch ❓ (not read off the schematic); RAM is cleared only at power-on.
- +4 read: the card ROM's transfer loops need the other bits to read 0. In `hbf.rom` (CRC-32 AE34982B), page 2, the read loop at CCE0h is `IN A,(C)` / `JR Z` / `JP M`: a 0 waits, b7 reads a byte, anything else ends the transfer as INTRQ (ROM disassembly; the loops run unchanged in freeTVC). MAME also reads 0 there. The disk-change and head-load bits are not known ❓ ([tvc-hardware.md](../hardware/tvc-hardware.md) §9).
- The same ROM's drive latch writes always set b6 (motor) together with one select bit (CC29h–CC31h), and it polls status b7 (not ready) before each type II command with a time-out (CDB6h–CDDBh). So *not ready* must clear when a disk is in: here it is set only while no drive is selected or the selected drive is empty. How the real card wires READY is not known ❓.

### WD1793

`src/core/cards/wd1793.{hpp,cpp}`, written from [DS]. FD1793 at 1 MHz with a 250 kbit/s MFM data separator, so:

| Quantity | Model |
|---|---|
| a byte | 32 µs = 200 half-T ticks |
| a revolution | 200 ms (300 rpm) = 6 250 bytes; the index pulse is the first 4 ms |
| step rates r1 r0 | 6, 12, 20, 30 ms ([DS] p. 6 Table 1: 1 MHz doubles the 2 MHz rates) |
| head settle (V), E delay | 30 ms ([DS] p. 6, p. 8 Table 4, p. 10–11 flow notes: 15 ms at 2 MHz, 30 ms at 1 MHz) |
| record not found, seek error on verify | at the fifth index pulse after the search starts ([DS] p. 10–11 flowcharts; the p. 9 and p. 11 text says four revolutions) |
| head unload | after 15 idle revolutions ([DS] p. 6, p. 9) |
| track layout | IBM System 34: gap 4a 80, sync, index mark, gap 1 50; per sector sync 12, A1 A1 A1 FE C H R N CRC, gap 2 22, sync 12, A1 A1 A1 FB, 512 bytes, CRC, gap 3; sector i's ID mark at byte 161 + 654·i (9 sectors) |

- **Lazy timing**: the controller keeps one pending event and catches up to the machine's tick whenever the CPU reads or writes it. It has no interrupt line to the machine (the HBF card polls INTRQ), so nothing is lost by not ticking it per T-state, and a machine without disk activity pays nothing.
- Every command: restore, seek, step, step in, step out (with T, h, V and the rate); read sector and write sector (m, S, E, C, a0); read address; read track (synthesised from the image, CRCs included); write track (F5/F6/F7 handled, ID and data fields decoded into the image); force interrupt (I3 held until the next command, I2 at the next index pulse, D0 without interrupt).
- Status in the type I layout after type I commands and after a force interrupt while idle; *index* is the index hole under the sensor, *track 0* the selected drive's head at cylinder 0, *head loaded* until 15 idle revolutions pass.
- Type I stepping ([DS] p. 9–10): restore loads FFh into the track register and seeks 0, at most 255 pulses, then seek error; any step out while TR00 is active sets the track register to 0 and ends the stepping without a pulse.
- Verify ⚠ CONFLICT in [DS]: the p. 9 text ends with seek error at the first ID whose track differs, the p. 10 flowchart reads on until five index holes. The flowchart is followed (a wrong track is only an error once no ID matches).
- Type II and III check READY first and end at once with an interrupt when it is false ([DS] p. 8, p. 11); write protection ends a write sector or write track with the WP bit.
- **Lost data** as [DS] p. 6 has it: a byte the CPU has not taken when the next one arrives; a write sector that gets no first byte within 22 bytes is cut off.
- A multiple-record command goes on with sector + 1 until a sector is not found (RNF ends it, as on the chip).
- Not modelled: CRC errors (images have none), deleted data marks on read (record type 0), write fault, sector sizes other than 512 in images, the HLT input.
- The drive mechanics (head cylinder per drive, 0–83) are part of the card's state; the motor line spins every drive from the moment it is set (no spin-up time ❓).

### Disks

`src/core/cards/floppy.{hpp,cpp}`. **The disks are not machine state**, like a real disk or the tape: `FloppyDrives` (drives A–D) is attached to the machine (`Machine::set_floppy_drives`) and given to the card at every access.
- Images are raw sector dumps (`.dsk`, `.img`): track by track, side 0 then side 1, sectors 1–9 of 512 bytes. The geometry comes from the FAT12 boot sector when it describes the file's size, else from the size: up to 84 tracks of 9 sectors is single-sided (VT-DOS's 360 KiB is 80 × 1 × 9), more is double-sided (720 KiB = 80 × 2 × 9). No other disk image format is known for the TVC.
- The IDs on a track are (physical cylinder, side, 1–9, 2), in order, no interleave. That is what VT-DOS's own `FORMAT` writes: its Write Track streams carry the IDs 1–9 in order on every track (captured from port 13h writes), so real VT-DOS disks are not interleaved either.
- Write Track on a cylinder past the image's end grows it, up to 84 tracks; a side the image does not have keeps nothing ❓.
- Writes: `HostTape` writes a written disk back to its file once the drive has been idle for half a second of machine time (`MediaControl::after_slice`), on eject and on exit, through a `.tmp` file and a rename. A file the host does not let freeTVC open for writing is a write-protected disk.
- Snapshots keep the card's state (FDC registers, the command in progress, page, latch, RAM) in `CARD` and the card kind with its EPROM image in `SLOT`; the drives go into `MEDI` by path and come back by path. Rewind therefore does not undo disk writes.
- `DiskImage::root_files()` and `root_file()` read a FAT12 root directory (used by the disk autostart and the tests).

### Speed and fast disk

Measured on the VT-DOS 1.2 boot by logging ports 10h/14h: power-on to the date prompt is 12 s of machine time, of which the first 5 s are the TVC's own boot. Between 5 and 11 s the driver issues 73 commands: 38 single-sector reads take 3.4 s, and most of the rest are restores and seeks (it goes back to track 0 and seeks again before each read on track 0). Between two such reads the driver spends 10–12 ms, while the next sector's ID passes 3 ms after the previous one ends, so each read waits a whole revolution (about 194 ms); where it reads in a tight loop (0.1 ms between commands) a sector costs 4.3 ms. With 1:1 disks and this driver a real TVC loses the same revolutions, so the timing is kept. An instant-disk mode (no rotational or step delays) is not offered: it would change what programs see.

**Fast disk** (GUI, Disk menu, on by default, not saved, like fast tape): while a drive was active in the last 0.25 s of machine time, the GUI runs unpaced, with the screen shown and the sound dropped. The machine's own timing does not change, so snapshots, determinism and programs that time the disk are unaffected; only the wall clock moves faster (headless, the VT-DOS boot ran 12.4 s of machine time in 0.49 s). Activity is every command start, step, byte and command end of the selected drive (`Wd1793::touch`), so long seeks keep it going, and the drive light shows them too. `FloppyDrives::active()` does not count a time after the machine's clock (it restarts at power-on, snapshot load and rewind). Headless runs and autostarts are unpaced anyway.

### Cassette class and autostart

The card's init takes the cassette class (`IN_TABLE`/`OUT_TABLE` entry 5 gets the card byte, [rom.md](../hardware/rom.md) §4), so with the card in, BASIC's `LOAD` and `SAVE` go to drive A: and the host cassette sees nothing (a card byte keeps the whole class, [media.md](media.md) "Host cassette"). That is why the card is off by default.
- **Disk autostart** (`--autostart FILE.dsk`, GUI open and drop, monitor `disk autostart`, MCP `media autostart`): the disk goes into A: with the card, the machine boots, and `LOAD "NAME"` is typed with the first `.CAS` file of the root directory; the close of the file (D4h) ends the load, then `RUN` at the next prompt, or done after 50 frames without one. Flipper and Expedíció (`teszt.img`, from tvc.homeserver.hu's `teszt.zip`) start this way; Flipper's cold autostart takes 0.7 s wall time headless.
- A **CAS or tape autostart while the card is in** gives the cassette class back before typing `LOAD`, as `POKE 2821,5: POKE 2829,5` does by hand (the same was found to work in WinTVC 1.4.1).

### Card ROM

Videoton's card ROMs are handled like the system ROMs: not in the repository, fetched by the autofetch from tvc.homeserver.hu. The card ROM comes through the ROM search, by name or by CRC-32:

| File | CRC-32 | What it is |
|---|---|---|
| `hbf.rom` | AE34982B | the card EPROM WinTVC 1.4.1 ships (the site's `hbf_eprom.rom`, `FLPDOS11.PGM`), header `MOPS` 04h `DISK`, **VT-DOS 1.1** by its own text ("VT-DOS CLI 1.1" at 1E6Bh); MAME calls it the "BASIC" BIOS |
| `hbf.rom` | 1466AED4 | another revision, differing in page 0 |
| `hbf.rom` | B3A567AD | UPM (MAME's `d_tvcupm.128`) |
| `hbf12.rom` | 2ACF8477 | **VT-DOS 1.2** (the site's `FLPDOS12.PGM`; MAME's `d_tvcdos.128`, which MAME calls 1.1) |
| `hbf12.rom` | D839417A | the site's `DOS1.FDR`, VT-DOS 1.2 with 4 bytes changed |

MAME's F5C35597 is a bad dump (A11 stuck) and is not accepted. `--disk-rom 1.1|1.2|FILE` picks `hbf.rom` (the default), `hbf12.rom` or any 16 KiB image. The VT-DOS *cartridges* (`vtdos11.rom`, CRC-32 40DDC741, and `vtdos12.rom`, 2C10C420, both autofetched; the DOS command shell) are ordinary cartridges (`--cart`); either card ROM runs with either cartridge. VT-DOS and UPM also need U3 in page 0 ([tvc-hardware.md](../hardware/tvc-hardware.md) §9).

### Front ends

| Front end | Disk card |
|---|---|
| headless | `--disk-card`, `--disk-rom 1.1\|1.2\|FILE` (implies the card), `--disk [A-D:]FILE` (repeatable; a bare file takes the next drive not named yet), `--autostart FILE.dsk` |
| GUI | the same options; Cards menu item *Disk card in slot 0* (powers on); Disk menu: *A: insert disk...* / *A: eject NAME* for A–D, *Fast disk*; a dropped `.dsk`/`.img` autostarts from A: (Ctrl: insert only); the strip shows **disk** while the card is in and **disk A:** in the notice colour while the drive is active (0.25 s after its last step, byte or command), with the drives in its tooltip; the settings window's Machine section switches the card and picks its EPROM (VT-DOS 1.1, VT-DOS 1.2 or an image file; a card that is in takes it at once and powers on); `disk_card` and `disk_rom` (`1.1`, `1.2` or a file) in `freetvc.ini` |
| monitor | `disk [insert "file" [A-D] \| eject [A-D] \| autostart "file" \| card on\|off]`; no argument shows the card and drives |
| MCP | `media` actions `disk_insert` (path, drive), `disk_eject` (drive), `disk_card_on`, `disk_card_off`; `autostart` with a disk image; `status` reports the drives |

### Tests and checks

- `core.floppy` (no ROMs): image geometry, FAT12 reading, every command through the card's ports with its timing, lost data, write protection, not ready, side compare, multiple records, write track, force interrupt, a copy and a snapshot of a card in the middle of a read, port 5Ah, IOMEM paging, a card snapshot refused by a machine without the card.
- `media.disk.<preset>` (32k-1.2, 64k-1.2, 64k+-2.2; cache variables `FREETVC_DISK_ROM`, default `hbf.rom` in the ROM folder, and `FREETVC_DISK_IMAGE`, default `testdata/Flipper.dsk`, a VT-DOS disk with `FLIPPER.CAS`): disk autostart with FLIPKOD's code found in RAM; a snapshot taken while the FDC transfers, loaded into a second machine, equal after 300 frames; `SAVE` onto a blank disk, read back from the host file through FAT12, then `LOAD` in a new machine.
- Checked by hand, headless: Flipper on all five presets with both `hbf.rom` revisions; Expedíció from `teszt.img`; VT-DOS 1.2 from its cartridge and boot disk, `DIR B:` of the 360 KiB Flipper disk; `FORMAT B:` of a zeroed 720 KiB image (all 160 tracks through Write Track, VT-DOS's boot sector), which mtools then reads and BASIC `SAVE`s onto; a BASIC `SAVE` read back by mtools; the monitor's `disk` command; MCP over stdio.
- Checked by hand, GUI: `LOAD "FLIPPER"` typed at the prompt with the drive light lit during the load; the Disk menu; *Disk card in slot 0* from the Cards menu (notice, strip, `disk_card = yes` written); `--autostart teszt.img`; the card and VT-DOS 1.2 switched from the settings window. Fast disk: VT-DOS from its cartridge and disk shows the date prompt 7.9 s after start with it on; with it off, the drive light is still lit and the screen blank at 8.3 s and the prompt comes by 14.3 s.

### Known limits

- Not checked: drag and drop of disk images (the file dialog is the same code as the cartridge's), a real HBF card, UPM with the card, the Windows GUI.
- Hardware questions open ❓: READY, disk change and head load wiring, what the reset line clears, the drives' spin-up time ([tvc-hardware.md](../hardware/tvc-hardware.md) §9).

## GameCard

dikdom's (Vass Sándor's) TVC-GameCard: an SN76489A sound chip, two more joystick ports and a 64 KiB EPROM with a boot menu and four games. Hardware facts with their evidence: [tvc-hardware.md](../hardware/tvc-hardware.md) §9 "GameCard".

Sources: https://github.com/dikdom/TVC-GameCard at commit `56e9c2b` (MIT): the GAL equations (`src/gal/*.PLD`), the Eagle schematic `src/hw/tvc-gamecard-GAL-v2.2.1.sch`, `doc/TVC GameCard.pdf`, the boot menu and TVC4Snake sources, and TI's SN76489AN manual; MAME's `src/devices/sound/sn76496.cpp` (BSD-3-Clause, master, October 2026) for the chip variants. All read as documentation; no code copied.

`src/core/cards/gamecard.{hpp,cpp}`, `CardKind::GameCard`, default **slot 1** (ports 20h–2Fh); any slot works (the card's software finds itself by its `JOY+SN` name).

| Port | Write | Read |
|---|---|---|
| +0 | SN76489A | FFh |
| +2, +3 | – | joystick 3 / 4: b0 up, b1 down, b2 left, b3 right, b4 fire (0 = pressed), b7–5 = 1 (not driven) |
| +Fh | b2–0 EPROM page, **b3 = 1 runs the sound clock** | FFh |
| others | – | FFh |

- IOMEM C000–DFFF: the selected 8 KiB page of the 64 KiB EPROM, read only; an empty or missing image reads FFh. ID 11 (port 5Ah shows "named by its ROM header"), no interrupts, no wait states.
- Reset (and power-on) loads page 7, the boot menu, and stops the sound clock; the chip's registers survive a reset (the card has no reset line to it).
- **The b3 polarity, checked against the software**: BattleField writes 08h (4A05h) and RasterRunner 0Fh (1ACAh) to +Fh before their first sound writes, then silence all four channels (9F BF DF FF); the boot menu writes pages with b3 = 0 (`AND $07` in `BANK_ON_HL`). That agrees with the GAL equations and with YATE.
- **Joysticks 3 and 4** are keyboard matrix rows 10 and 11, which the TVC itself reads as FFh (`core/input/keyboard.hpp` `joystick_key`, `kMatrixRows` 12, `kKeyboardRows` 10); the card reads them through `CardBus::keyboard`. So snapshots, typing scripts, headless `--joy 3:…`/`4:…`, `--key JOY3_UP` … `JOY4_FIRE` (and `10:0` style), MCP `press_keys` and the GUI's gamepads 3 and 4 all reach them without card-specific code.

### SN76489

`src/core/cards/sn76489.{hpp,cpp}`, written from TI's SN76489AN manual and the variant notes of MAME's `sn76496.cpp`: the counters step every 16 clocks (3.125 MHz on this card; the clock is a parameter); a tone toggles every `period` steps (0 counts as 400h); noise shifts every 32/64/128 steps or at twice tone 3's period, through a 15-bit register (taps b2 and b3 in MAME's 17-bit form, output b0) cleared by every noise write; attenuation in 2 dB steps, 15 off; the output is not inverted. Power-on registers are 0 (MAME's choice; real chips start undefined ❓), which is silent on the GameCard because the clock is stopped until software sets b3, and the games silence the channels first.

- **Sound into the machine**: the chip steps lazily (when its ports are touched and at the end of each `Machine::run`) and gives each change of its summed output to `AudioSynth::add_step()`, a band-limited step like the TVC's own tone; steps from several sources add up in any order until `end_at()` passes them. No per-T-state cost.
- **Level** ❓ (no measurement): all four channels at full volume together equal the TVC tone at amplitude 15 (each channel 0.1875 at the synth input); level set by listening tests (BattleField's music in the GUI).

### ROM

`gamecard.rom` (CRC-32 F782DB9B, dikdom's `bin/TVC-GameCard-r1229.BIN`, 64 KiB) through the ROM search, `--gamecard-rom FILE`, and the autofetch: `data/roms.txt` names it by its pinned GitHub URL (commit `56e9c2b`), with the MIT notice and the game credits in `ROMS-README.txt`. Without it the card still has its joysticks and sound, and the boot goes on as usual.

### Front ends

| Front end | GameCard |
|---|---|
| headless | `--gamecard [SLOT]` (default 1), `--gamecard-rom FILE`, `--joy 3:…`, `--joy 4:…`, `--key JOY3_FIRE` … |
| GUI | the same options; Cards menu item *GameCard* (powers on; the first free slot when slot 1 is taken); gamepads 3 and 4; `gamecard` in `freetvc.ini` |
| monitor | `gamecard [on [slot] \| off]`; no argument: slot, EPROM CRC, page, sound clock |
| MCP | `media` actions `gamecard_on` (slot) and `gamecard_off`; `status` reports it; `press_keys` with `JOY3_*`/`JOY4_*` or rows 10–11 |

Snapshots carry the card in `SLOT` (with its EPROM image) and its page and chip state in `CARD`.

### Tests and checks

- `core.gamecard` (no ROMs): tone pitch (period 222 → 439.9 Hz within 1 Hz), RMS of a full-volume channel, the 4 dB step, period 0 = 400h, the stopped clock (silent, writes lost), periodic noise (one pulse in 15 shifts: 406.9 Hz) and white noise, port 5Ah, the joystick ports and rows 10–11 being invisible to the TVC, paging, read-only EPROM, reset to page 7 with the clock stopped, a socket without an image, a snapshot and a copy sounding the same afterwards. `input.host` drives four virtual gamepads.
- Checked by hand, headless: the boot menu on 64k+-2.2 from dikdom's repository's image and from the fetched one; all four games start from it (BattleField, Bomberman, RasterRunner, TVC4Snake); joystick 3's fire acts like joystick 1's in TVC4Snake; a BASIC `OUT` sequence plays 440 Hz in the WAV (440 crossings a second; RMS 0.094 as designed); BattleField and RasterRunner make sound from their titles (Bomberman's title music is the TVC's own tone; TVC4Snake has no SN sound in its source); the monitor shows the clock running in BattleField; RasterRunner also starts on 64k-1.2 and 64k+-2.1, not on 32k-1.2 (the menu highlights it; probably too little RAM ❓).
- Checked by hand, GUI: the card's menu, key 1, BattleField's title; freeTVC's own audio stream recorded silent until the key and then the music at the headless WAV's level; the menu item powers on into the card's menu and saves `gamecard = yes`.
- MCP over stdio: `gamecard_on` in slot 2, `press_keys` `JOY3_FIRE` and `10:0`, `gamecard_off`.

### Known limits

- Not checked: a real GameCard, more than two physical gamepads; not compared with WinTVC 1.4.1's `JOY+SN` card.
- The SN level against the TVC tone is set by listening, not measured; the chip's power-on state ❓; whether writes closer than about 32 chip clocks are lost on the real card (READY is not wired to /WAIT) ❓.

## TVC256k++

dikdom's TVC256k++: an RP2350 MCU that serves the card's ROM, RAM and registers on the bus, with 256 KiB of fast RAM, 8 MiB of PSRAM, a graphics overlay with sprites, USB storage or a mouse, a flash drive and a virtual SID ("vSID"). Hardware summary: [tvc-hardware.md](../hardware/tvc-hardware.md) §9.

Sources: https://github.com/dikdom/TVC256kplusplus at commit `4580b144a9b9` (firmware v0.24; the author's own files MIT, the repository as a whole GPL-3.0): the firmware sources, cited as **[FW]** = `sources/firmware/TVC-IO/`; the card ROM's Z80 sources, **[Z80]** = `sources/firmware/TVC-IO/Z80/`; the manual `doc/TVC256k++ kártya-20261011.pdf`; the rev3 schematic `doc/Schematic_TVC-rev3-smd.pdf`. The firmware is followed step by step in freeTVC's own C++, with its quirks; copied as data are four power-on sprite tables (`tvc-graphics.c` balloon and rectangle, `videoton_sprites.h`, `tvc-hat.h`, MIT) and the three vSID tables ([vSID](#vsid)). Licences in `THIRD_PARTY.md`.

`src/core/cards/card256.{hpp,cpp}`, `card256_functions.cpp`, `card256_overlay.cpp`, `card256_graphics.cpp`, `card256_storage.{hpp,cpp}`, `vsid.{hpp,cpp}`, `vsid_tables.cpp`; host side `src/frontend/common/card256_host.{hpp,cpp}`. `CardKind::Card256`.

### Bus

| Access | Effect |
|---|---|
| ports +0/+4 | register index of set 0/1 (read back) |
| ports +1/+5 | data of the selected register; after every data read or write the index grows by the set's increment |
| ports +2/+6 | the increment (power-on 1) |
| ports +3/+7 | read 00h, writes ignored |
| ports +8…+15 | mirror +0…+7 (only A0–A2 decoded) |
| IOMEM C000–D7FF | the selected 6 KiB bank of the 12 KiB card ROM; writes ignored |
| IOMEM D800–DFFF | 2 KiB of card RAM (the ROM's variables) |
| U2 / U3 | while B0h / B1h is not FFh: fast RAM page n (the card asserts /PHANTOM); FFh gives the machine's own RAM |

[FW] `TVC-IO-main-sys.c` `irq_handler_slotn` (257ff.), `irq_handler_mreq_p` (219ff.). ID 11: on the rev3 schematic (connector B36/B38) IDA_N and IDB_N are not connected, so software finds the card by its `MOPS` 06h `256k++` header; /INT_N (B37) is wired (the collision interrupt, [Overlay](#overlay)), /CLRINT_N (B35) is not. The card answers wherever port 02h maps U2 or U3, also U3 in page 0 (static reading of the paging circuit ⚑). On a bare 32k board U2/U3 come from the card alone, so with the power-on pages 4/5 a 32k machine boots as a 64k one (manual p. 12). The debugger's U2/U3 banks show what answers there. The card is refused on the 32k + HBM board.

### Registers

Index 00h–FFh through ports +1/+5; [FW] `initRegisterPointers` (486ff.) gives each register a read and a write place, `process_queue_cycle` (722ff.) the follow-up of a write. The firmware runs follow-ups on its main loop, microseconds later; here they take effect at once.

| Index | Read | Write |
|---|---|---|
| 00h–1Fh, 20h–3Fh | sprite X, Y (16-bit, low/high) | stored |
| 40h–4Fh, 50h–5Fh | sprite colour, phase | stored |
| 60h–6Fh, 70h–7Fh, 80h–8Fh | sprite foreground, enable, colour mode (0/1) | stored, then all 16 made 0/1 and the bit words rebuilt |
| 90h–95h | enable, foreground, colour-mode bit words | the 8 sprites' flags from the bits |
| 96h/97h, 98h/99h | sprite–sprite, sprite–background collisions; **reading 96h (98h) clears its word and releases /INT** | ignored |
| 9Ah–9Dh | collision interrupt masks | stored |
| 9Eh | sprite base (b2–0) | stored, masked |
| A0h–A4h | video mode, screen/colour/font/bitmap bases (reset 0, 01h, 02h, 02h, 01h) | stored, masked (3, 7Fh, 7Fh, 3Fh, 3) |
| A5h, A6h | scroll X, Y | stored, masked 87h |
| A7h | border (IRGB) | stored, masked 0Fh; **port 00h writes land here too**, IGRB turned into IRGB ([FW] `irq_handler_border_io`) |
| A8h | the overlay's line counter, low byte: 21 after power-on, then the line being built ([Overlay](#overlay)) | ignored |
| A9h | screen height (240) | 2–239 sets it |
| B0h/B1h | U2/U3 fast-RAM page (power-on 04h/05h), 10h/11h while PSRAM is mapped | FFh, or the value masked to 0Fh |
| B2h–B5h | the PSRAM page mapped at U2 / U3 (16-bit, FFFFh none) | B2h/B4h map PSRAM page n (0–255) at U2/U3; B3h/B5h only latch |
| B6h | PSRAM size in MiB: 8 | ignored |
| B7h | ROM bank | 0/1 selects the bank |
| C0h | the function running, FFh when done | runs function n |
| C1h | parameter block: b7 = 0 in U2's page, 1 in U3's; b6–0 × 128 bytes into it | stored |
| C2h | the last function's result | stored |
| C3h/C4h | firmware version 00h / 18h (0.24) | ignored |
| C5h | drive (= card RAM D810h) | stored, above 3 made 3 |
| C6h | multi-function chain, FFh when done | runs n functions from the parameter block |
| C7h | the functions' bitmap base (FFh = follow A4h) | stored, masked 3 unless FFh |
| C8h–CAh | PSRAM / flash / DSK drive valid (C8h = D824h, C9h the flash drive mounted, CAh = D825h) | ignored |
| D0h | USB device: b0 a mouse, b1 a pendrive ([Mouse](#mouse), [Storage](#storage)) | ignored |
| D1h–D4h | mouse buttons, dX, dY (cleared by a read), wheel (never set) | ignored |
| D5h | mouse speed (10h on a fresh card) | b6–0 kept across power cycles (the card's flash) |
| D6h | storage command status: 00h done, result \| 80h failed, FFh unknown ([Storage](#storage)) | runs command 01h–13h |
| DCh | debug byte | stored |
| E0h–FFh | vSID registers as the engine last applied them; 1Bh/1Ch (FBh/FCh) voice 3's oscillator and envelope ([vSID](#vsid)) | queued with the write's vSID clock; the read-back changes when its block is computed |
| others | 00h | ignored |

PSRAM mapping ([FW] `processPSRAMMapping`, 675ff.): mapping page n at U2 first copies the page mapped there before back from the window into the PSRAM, then copies page n into the window (fast-RAM page 16; U3: 17) and points U2 at it. Mapping the page already mapped keeps the window as it is. The same PSRAM page in both windows gives two independent copies, as on the card.

### Memory functions

The parameter block lies in fast RAM ([FW] `getFunctionParamAddress`, 706ff.); without a fast-RAM page in the chosen unit the result is FFh. A function code without a function also gives FFh; in a chain (C6h) a failing function leaves its 1-based index in C2h (80h + index when unknown) and its error in the block's first byte. Chain entries are a code byte and that function's parameters; the parameter size comes from [FW] `getFunctionParamSize` (2789ff.).

[FW] `tvc-routines.c`; 24-bit addresses, bit 23 = PSRAM where the firmware allows it:

| Code | Function | Notes |
|---|---|---|
| 11 | move within one 16 KiB page (pages 0–17) | `memmove` |
| 12 | move within fast RAM | |
| 13, 14 | fast RAM ← PSRAM, PSRAM ← fast RAM | |
| 17 | copy a block into chunks with a destination step | source in fast RAM or PSRAM |
| 18, 41 | chunked copy with one / two steps | backwards when the areas overlap the firmware's way |
| 19 | mirror sprite phases (2- or 16-colour, horizontal or vertical) | |
| 20 | ZX7 decompression; the size goes to parameter bytes 6–9 | freeTVC's own decoder from the ZX7 format (Einar Saukas), checked against `zx7Compress.c` |
| 37 | evaluate a postfix expression (bytes, int16, int32, TVC BCD floats, floats, variable references, π and decade constants; + − × ÷ min max abs sqr sqrt sin cos tan int float round) | single precision like the MCU; sin/cos/tan may differ from its library in the last bit ❓ |

The text and drawing functions (0–10, 16, 21–33, 38–40) are in [Drawing functions](#drawing-functions), the drive functions (15, 34–36, 81h–93h) in [Storage](#storage). Where the firmware would read or write past its RAM (unchecked lengths, ZX7 streams, variable references up to 2 MiB), the emulation stops at the memory's end: reads give 0, writes are dropped.

Firmware behaviour kept as it is: the math evaluator returns the type of the top stack entry with the value of the bottom one, unsigned min/max on integers, integer division by zero gives 0 (Cortex-M33 `SDIV`), float-to-int conversions saturate (`VCVT`).

### Reset and power-on

- **Reset button** ([FW] `tvc-graphics.c` `reset_handler`, 763ff.; manual p. 10–11): video mode 0, sprites off, scroll 0, vSID register 24 reads 0. Paging, RAM, the ROM bank and the registers otherwise stay.
  - ⚠ **CONFLICT**: the manual (p. 10–11) has the reset silence the sound by writing 0 to vSID register 24; the handler writes only the read-back array `SID[24]`, not the engine's master volume, so a note goes on playing. freeTVC follows the firmware (`core.card256_vsid` checks it).
  - ⚠ **CONFLICT**: the manual (p. 11) also has the screen height go back to 240; the v0.24 handler leaves it. freeTVC follows the firmware.
  - A second reset within 1 s of machine time after the last one reboots the MCU: the card powers on. A reset within 1 s of the MCU's start (power-on included) counts as the second press.
- **Power-on**: fast RAM and PSRAM 0, ROM bank 0, U2/U3 = pages 4/5, the register reset values above, then the power-on pictures ([Overlay](#overlay)).
- The reset also ends the overlay's frame and syncs it again.

### Overlay

[FW] `tvc-graphics.c`, `tvc-overlay.pio`, `TVC-IO-main-gfx.c`; manual pp. 13–17; `src/core/cards/card256_overlay.cpp`. The core's interface is `VideoOverlay`: `Video` calls `VideoOverlay::video_character` for every CRTC character.

**How it reaches the picture.** From the HBA-2 main board schematics (`TVC_HBA2_rajzok.pdf`, tvc.homeserver.hu "Tervrajzok"), read off the rendered sheets, static ⚑:
- Sheet 9 (PDF page 11): EC0–EC3 from the connector enter the second half of B11 (74LS244, 2A1–2A4). Its outputs share the colour bus with B11's first half (the 16-colour bits) and the palette RAM B7 (74LS670). /EXTC (pulled up by R7, through J1 and the A4 gates beside the mode latch F6) hands that bus to the card.
- The bus is input 1 of B9 (74LS298). During the border, WS (WSE, from the CRTC's DE, sheet 7) picks input 2, the border register B8. B9 is clocked by H5 (74S153) at the mode's pixel clock, as the hardware book says ([HW] p. 110).
- So the card's pixels replace the TVC's only in the paper. B9 samples the card's 6.25 MHz pixels at 12.5 MHz in 2-colour mode and at 6.25 MHz in 4-colour mode (all 256 pixels show). In 16-colour mode it samples at 3.125 MHz, so every other pixel shows; which one of a pair is ❓, freeTVC shows the even one.
- Sheets 7 and 10 (PDF pages 9 and 12): the slots' /INT0–3 (pull-ups R1–R4) go through J2 to /EXPINT and, with the cursor/sound flip-flop, to the CPU's /INT (J18, J16). Port 59h reads the four lines directly (E1, /RDINT). The board has no latch and no enable; a write to 58h–5Bh is that slot's /CLRINT strobe ("IT kérésének törlése", [HW] port table, pp. 125–126).

**Timing.** `draw_screen_cycle` and the PIO program `draw_overlay_new`, modelled on beam events:
- After a start (power-on, reset) the core syncs: the first frame starts at the end of the second VSYNC it sees. After that, every VSYNC end starts a frame, and line 21 is built at once.
- At every paper start (DE rising, the bus BORDER), the PIO starts the queued line, the counter (A8h) steps, and the next line is built from the registers and RAM of that moment. A change therefore shows from the line after the next one (manual pp. 16–17).
- When the counter reaches A9h + 21 the frame ends. The collision words are checked against the masks (/INT low), and the core waits for VSYNC. VSYNC is not watched during a frame, so a CRTC with fewer paper lines than A9h makes the overlay roll.
- Pixel p of a line shows at paper character p / 4, two framebuffer dots wide. After 256 pixels, and on paper lines with nothing queued, the PIO's pins keep their last value: the next line's first pixel, or the frame's last pixel. After a reset that last pixel stays until the overlay starts again (two frames; not verified on a card ❓).
- Not modelled: the firmware's compute time inside a line (a register written then may or may not land in it), the 74LS123 that stretches VSYNC on the connector, and any delay of BORDER against DE (a pixel offset against the TVC's own pixels is possible ❓).

**A line** (`calculate_one_line`). Colours are IRGB (b3 I, b2 R, b1 G, b0 B; the hat's red 4/C confirms the order); colour 8 is transparent.
1. With A6h b7 set, lines 21–28 and 254–260 are the border colour A7h and nothing else (no sprites, no collisions). That is 8 lines on top and 7 below (the firmware's `y > maxY - 8`).
2. Otherwise: transparent, then the back sprites (foreground 0), the picture of the mode, the front sprites, and with A5h b7 the 8-pixel strips left and right in A7h. A0h: 1 2-colour text, 2 16-colour text, 3 bitmap; 0 none, and then 98h is cleared on every line. Higher sprite numbers cover lower ones.
3. The picture moves right by A5h & 7 and down by A6h & 7 pixels.

Addresses in fast RAM:
- Screen and colour RAM: A1h/A2h × 400h, 32 × 30 bytes.
- 2-colour font: A3h × 800h, 8 bytes a glyph, MSB left. 2-colour text is transparent where the glyph has no pixel.
- 16-colour font: (A3h × 800h) & 1E000h, 32 bytes a glyph, 4 a row, high nibble left. ⚠ The manual's 1F800h case is not what v0.24 does; freeTVC follows v0.24.
- Bitmap: A4h × 8000h, 128 bytes a line.
- Sprites: 9Eh × 4000h + phase × 64 (2-colour, 3 bytes a row, colour from 40h + n) or × 256 (16-colour, 12 bytes a row); 24 × 21 pixels, (24; 21) is the paper's top left; skipped from x = 280.

**Collisions** (the firmware's buffers, quirks kept):
- Sprite–sprite (96h): per line, each sprite's pixels against those of the sprites drawn before it, back and front alike. 2-colour sprites compare in groups of four pixels. 16-colour sprites mark and test only the left pixel of each byte; the right one is only looked at.
- Sprite–background (98h, modes 1–3): every enabled sprite against the line's background mask:
  - 2-colour text: the glyph, with its 4-pixel halves swapped (the firmware writes the 64-bit mask's 32-bit halves the other way round);
  - 16-colour text: the non-8 pixels;
  - bitmap: the pixel's colour, so black never collides and colour 8 always does (it is stored as 10h).
- The mask is never cleared except 7 bytes on the left. For a sprite beyond x = 280 the firmware reads past the mask's 304 bytes into its other variables; freeTVC reads 0 ❓.
- The frame-end check pulls the slot's /INT line low (`Card::irq()`); reading 96h or 98h releases it. 58h–5Bh writes do not reach the card (its /CLRINT is not wired).

**Power-on pictures** (`preinit_core_gfx`, `init_graphics`, `clear_text_screen`):
- the font at 1000h and the picture's rows, turned, at 8000h (both from the card image, [ROM and data files](#rom-and-data-files));
- the balloon at 0, the rectangle at 40h, eight Videoton letters at 80h, the 16-colour hat at phase 40h of sprite base 0 (4000h), rows turned;
- screen RAM at 400h spaces and colour RAM at 800h colour 0Fh, for 32 × height / 8 bytes (the 64 after keep the init's pattern);
- sprites at x = 24 + 12n, y = 21, colour 15, phase 40h, front, 16-colour, off.

The overlay starts in mode 0: the picture shows when a program selects mode 3 (the manual's snowy landscape).

### Drawing functions

[FW] `tvc-routines.c`; `src/core/cards/card256_graphics.cpp`. They draw into the bitmap at C7h × 8000h (A4h's while C7h is FFh), 4 bits a pixel, high nibble left. Where the firmware passes a dot's X and Y as bytes, freeTVC's do too.

| Code | Function | Notes |
|---|---|---|
| 00h–0Ah | clear text screen, clear bitmap (88h), print a screen code, hex byte, hex word, screen-code string, ASCII string, get/set text colour, get/set XY | cursor and text colour live in the card (snapshot). ASCII: `A`–`]`, `a`–`z`, 32–64, `_ ^ ~`, CR, LF, cursor codes 04h/13h/05h/18h, accents 80h–88h/90h–98h, A0h–DFh; F0h–FFh set the colour. The text scrolls at height / 8 rows. Printing and scrolling mask A1h/A2h with 3Fh, clearing does not |
| 10h | replace a colour in a fast-RAM range | nibble by nibble; past 256 KiB: error 1 |
| 15h–18h | get/set pen colour (power-on 0), get/set a dot | Y at or below the height: error 1 |
| 19h, 1Ah, 1Ch | line (Bresenham, clipped per pixel), rectangle, poly-line | |
| 1Bh | filled box | leaves out its last line; off-limit boxes are dropped |
| 1Dh, 28h | flood fill with the pen colour, or checkerboard with two colours | the firmware's scanline fill, 1024-entry stack (error 2 when full); it also tests and fills the line just below the screen height |
| 1Eh, 1Fh | ellipse, filled ellipse | midpoint algorithm in the firmware's 32-bit arithmetic |
| 20h | copy an image block from fast RAM or PSRAM | `copy_image_block_fast`: colour 8 keeps the bitmap's pixel |
| 21h | copy part of a larger image | outside the source the default colour; colour 8 skipped when b7–4 of it are set |
| 26h | filled triangle, checkerboard | counter-clockwise only; outline in the pen colour unless it is 8; clipped to 240 lines whatever A9h |
| 27h | filled box, checkerboard | includes its last line; with off-limit corners it still draws a dot at (0, 0), like the firmware |

### Storage

`src/core/cards/card256_storage.{hpp,cpp}`, written from [FW] `TVC-IO-main-sys.c` (REG_USB_MSC_CMD), `tvc-fatfs.c`, `tvc-lfs.c`, `tvc-lfs-flash.c`, `tvc-lfs-psram.c` and `tvc-routines.c` (functions 15, 34–36, `tvcfunc_*`). The host side is `src/frontend/common/card256_host.{hpp,cpp}`. The file system libraries are the vendored FatFs R0.16 and littlefs v2.11.3 (`third_party/`, `THIRD_PARTY.md`).

**How the TVC reaches it.** The card ROM's init writes its card byte into the cassette class (IN_TABLE/OUT_TABLE entry 5) on every reset, warm or cold, without looking at what is there ([Z80] `tvc-init.asm` lines 37–41); with the HBF disk card in a lower slot, the TVC256k++ wins. Every cassette-class call then runs the card ROM's own handlers (open, CHIN/CHOUT, BKIN/BKOUT, close, verify, cd, dir, mkdir, delete, rename, seek, its base port 5Dh, DSK mount), which write a command's input at card RAM D900h, a code into register D6h, poll D6h and read the output at DA00h. The `EXT n` command line uses the same calls. Commands finish at once here; the ROM accepts 00h at its first poll.

**Drives** (C5h = D810h selects; the D6h dispatch and the file functions follow it):

| # | Drive | In freeTVC | Library |
|---|---|---|---|
| 0 | pendrive (USB mass storage) | a host folder (`PendriveFolder`), written to at once, outside the machine state | a FatFs-like layer of freeTVC's own |
| 1 | PSRAM drive | littlefs on the card's PSRAM bytes 0 … n × 4 KiB, machine state; overlaps the B2h–B5h paging as on the card | littlefs |
| 2 | flash drive | a read-only littlefs image (15 MiB on the card), outside the machine state | littlefs |
| 3 | DSK | a FAT image file on drive 0, 1 or 2, mounted by name | FatFs |

The firmware's two file layers are followed step by step: the FatFs layer (drives 0 and 3, one pool of 4 files and 4 directories) and the littlefs layer (drives 1 and 2, another pool), with their result codes (FRESULT, littlefs errors as bytes, 20 a bad handle, 21 a bad CAS type byte, 40 read-only, 41 end of file), their card-RAM side effects (names terminated and lower-cased in place, `strncpy` padding, `getcwd`'s left-over tail, the FILINFO or `lfs_info` at DC00h) and their quirks, among them:
- open for read: an empty name opens the first `.cas` of the working directory (by its 8.3 name on FatFs), a trailing `*` matches a prefix in lower case, a name without an extension tries `.cas`; a CAS file must start with 01h or 11h, is read from 80h and its size reported without the header; SAVE (mode 06h, `@` = 0Ah) writes the type byte and 127 zeros first;
- both littlefs drives open read-only, so **SAVE to the PSRAM drive fails** (A8h) though the manual says it is writable; neither renames; a bad CAS type on littlefs leaks a handle;
- littlefs `chdir` refuses a trailing `/`; `mkdir`, `delete` and `stat` measure the name against the previous command's path (`temp_path`); a short littlefs read is reported as an error carrying its length;
- function 84h copies DA00h into the parameter block and writes card RAM (the data pointer lands at D906h); function 34 takes its block count from the parameter block's MCU address; 35 changes nothing the Z80 sees; 36 would clear about 4 GiB: here it fills the 8 MiB with FFh and stops ⚑;
- function 15 copies the pendrive folder's files (flat, lower case) into a new PSRAM drive of max(512 KiB, 2 × their size), stops at the first file that fails, and with its flag set shows progress in the overlay's text mode.

**freeTVC's own modelling** (no firmware equivalent): files and directories are reopened for every command at the handle's place (the card keeps them open), so the state is plain data and copies, rewind and snapshots need nothing else; handles are made-up MCU addresses ❓; FatFs is R0.16 (the firmware's version is not in its sources ❓) with code page 437 ❓, one FatFs drive per mounted image; the pendrive's directory order is by name without case (a FAT stick lists in creation order ❓), its 8.3 aliases are `NAME~1.EXT` without collision numbering, its names are CP437 bytes on the TVC and UTF-8 on the host ❓; FatFs dates are fixed (2026-01-01, `FF_FS_NORTC`) for the DSK, host files keep their own; the DSK image on the flash drive cannot be written here (the card would program its flash ❓). No pendrive gives FR_NOT_ENABLED (8Ch); a bare LOAD then reports 84h, as the firmware's directory scan fails first.

**DSK.** VT-DOS 360/720 KiB images mount: their boot sectors start with EBh and have no 55AAh signature, which FatFs R0.16 accepts as an early MS-DOS volume (`check_fs`); an older FatFs in the card may not ❓.

**Flash image.** `card256-flash.bin` is the author's `binaries/flash-root-1.3-bin.zip` (member `flash-root-1.3.bin`, 15 MiB, CRC-32 E0FFC216) at the pinned commit, through the autofetch (an `ARCHIVE#MEMBER` piece, unpacked by `src/frontend/common/unzip.cpp`, freeTVC's own inflate from RFC 1951) and the ROM search; `--card256-flash` takes another image or a folder, packed into a littlefs image at load (names lower-cased, as the manual's `littlefs-python` step wants). It holds `launcher.cas` (Sanyi's launcher), `tvc256k++/` with the card's demos (cube, demo, gfxdemo, jukebox, lim, math, pics, setmousespd, triangle, vsid) and `games/` with TVC programs.

### Mouse

One USB-A port and no hub (`CFG_TUH_DEVICE_MAX` 1, manual p. 36): a pendrive or a mouse, never both, so D0h is 01h or 02h as on the card (setMouseSpeed requires 01h). Each host report goes through [FW] `tvc-usb.c` `process_mouse_report`: buttons to D1h (1 left, 2 middle, 4 right), the movement summed since the last read of D2h/D3h, times the speed in D5h (3.5 fixed point: 10h = 0.5), rounded, clamped to −254…255, stored as a byte; reading D2h or D3h clears it and starts a new sum. The wheel (D4h) is never set. The card ROM has no mouse code. The front ends send one report per GUI frame (at most ±127 a report, as the boot protocol).

### vSID

`src/core/cards/vsid.{hpp,cpp}`, written from [FW] `tvc-sid.c` and `tvc-sound.c`. `tvc-sid.c` is Sanyi's code built on Bakisha's STM32-SID-Player (https://github.com/Bakisha/STM32-SID-PLAYER, GPL-3.0) and Sgw32's UltiSID (no licence stated: only its behaviour was followed, no code or data taken from it). The three tables in `vsid_tables.cpp` (generated by `tools/re/vsid_tables.py` from the firmware's `tvc-sid-internal.h`) are copied as data: the ADSR rate and volume-divider tables `ADSR_LFSR15` and `ADSR_Volume2LFSR5` from STM32-SID-Player (GPL-3.0), and reSID's combined-waveform AND mask by Dag Lem (GPL-2.0-or-later). The vSID is not a real SID (the manual says so, p. 11) and reSID is not used for it; the [SID card](#sid-card) is the reSID one.

**Pipeline** ([FW] `tvc-sound.c`): the vSID clock is 336 MHz / 341 = 985 337 Hz (a C64 PAL SID runs at 985 248 Hz), counted from the MCU's start; machine ticks convert exactly (1344/8525). A data-port write to E0h–FFh is queued with its vSID clock (256 entries, 8-bit positions: a 257th pending write makes the queue look empty and everything pending is lost). Every 3072 clocks the firmware's DMA interrupt computes the past 128 samples (one every 24 clocks, about 41 kHz), applying each queued write at its own clock between samples; the data port reads the registers as those blocks left them, so a write reads back up to 3 ms later. The samples play one block later (3.1 ms). Until the first block the output is 0.

**Engine** ([FW] `calculate_SID_voices`): per call it advances all three voices by the clocks since the last one: 24-bit accumulators (the test bit holds them at 0), noise from a 23-bit LFSR clocked by the accumulator's bit-19 rises (output bits 11–22, not the SID's tap selection), sync and ring modulation from the previous voice in the order 1←3, 2←1, 3←2 (each seeing the ones already changed), triangle, sawtooth, a pulse averaged over the accumulator's movement since the last call ("pseudo multi-sampling"), the combined waveforms through the AND mask, noise with anything else 0, no waveform keeping the last output. The envelope steps by a rate counter (`ADSR_LFSR15`, reSID's rates less one) and a volume-dependent divider (`ADSR_Volume2LFSR5`); quirks kept: the gate's previous state starts high, sustain switches the rate counter to the release rate, the attack overshoot turns into decay at once, division by zero gives 0 (the MCU's `UDIV`).

**Output** ([FW] `prepare_unfiltered_output`): (the three voices' mean + 80000h) × the master volume, clamped to 24 bits; the top 16 bits scaled by 65335/65535 in a 32-bit `int` that wraps above 32869 (a ±1 difference, kept), plus 100: the 16-bit value of the two 8-bit PWM outputs. No filter: the filter registers (21–23) and OFF3 change nothing. Registers 27 and 28 take voice 3's oscillator top byte and envelope at every sample.

**Sound line** (rev3 schematic): SNDHI through 1 kΩ and SNDLO through 256 kΩ sum into an op-amp with a 6.8 nF low-pass (about 23.5 kHz), then 100 Ω, 10 µF and 2.2 kΩ into the bus SOUND line (slot pin A16), like the GameCard's SN. Here the 16-bit value goes into the machine's band-limited audio as steps at the sample clock, full scale 0.75 like the TVC tone at amplitude 15 (❓, not measured). The RC low-pass sits above what the 48 kHz output carries and is not modelled. A reboot of the card (second reset press) drops the output to 0 at once.

### State and snapshots

Fast RAM is 288 KiB (16 pages + the two PSRAM windows). The 8 MiB PSRAM is kept as 512 pages of 16 KiB shared between copies of the machine and copied only when one is written, so rewind points, saved states and the prompt state stay small. Snapshots store the registers, card RAM, fast RAM and the PSRAM pages that are not all zero, behind a version byte: 2 adds the overlay (state, shown and queued lines, the held pixel, the background mask, the text cursor and colours); 3 adds storage (handle pools with paths and positions, working directories, the DSK, the PSRAM drive) and the mouse; 4 adds the vSID (the voices, the pending writes, the computed samples not yet played, the MCU's start); older snapshots load with the missing parts as after power-on. The pendrive folder, the flash image and what is plugged into the USB port stay with the front end; the card follows the front end's port at every access.

### ROM and data files

| File | Source (dikdom's repository, commit `4580b14`) | CRC-32 | Licence |
|---|---|---|---|
| `card256.rom` | `binaries/tvc-256k++.bin`, 12 KiB, firmware 0.24 (SHA-256 `4fc47a80…11942b9`) | F759A66F | MIT; the author published it for emulator developers |
| `card256-font.bin` | `sources/firmware/TVC-IO/binaries/c16-hungarian.bin`, 2048 bytes | 5B6A2B88 | the Commodore 16/Plus/4 character set with Hungarian letters added; the original character set's copyright stays with its holders |
| `card256-picture.bmp` | `sources/firmware/TVC-IO/binaries/tvc-16cimage.bmp`, 30838 bytes | EA0740CD | MIT |
| `card256-flash.bin` | `binaries/flash-root-1.3-bin.zip#flash-root-1.3.bin`, 15 MiB | E0FFC216 | dikdom's launcher and demos (MIT) and TVC programs by their authors |

All four are fetched by the autofetch, not in the repository, and found by the ROM search; `--card256-rom FILE` and `--card256-flash FILE|DIR` take others. Without the ROM the card still pages its RAM, but the machine never calls it (no `MOPS` header). The raw GitHub file at the pinned commit matches the fetched ROM byte for byte.

The picture is a bottom-up 256 × 240 4-bit BMP whose pixels the firmware takes from offset 118 (palette ignored). The front ends join the font and picture after the ROM (`card256_image()` in `src/frontend/common/presets.cpp`): the ROM padded with FFh to 12 KiB, the font, the 30720 pixel bytes; a missing file is zeros and a notice ("TVC256k++ without its font, so text modes stay empty until a program loads one", and the same for the picture); neither file leaves the ROM alone. Another BMP size is refused with a notice. So snapshots (the `SLOT` chunk) and prompt-state keys carry them.

### Front ends

| Front end | TVC256k++ |
|---|---|
| headless | `--card256 [SLOT]` (default slot 0, else the first free slot), `--card256-rom FILE`, `--card256-usb DIR\|mouse`, `--card256-flash FILE\|DIR` (both imply the card); refused on the 32k + HBM board |
| GUI | the same options; Cards menu item **TVC256k++** (powers on; the notice names the slot and `EXT n`), its slot rib lit; under it **Pendrive folder…** (a folder dialog), **Mouse**, **USB port empty**, **Flash drive …** (a file dialog), none of which powers on; with the mouse plugged in, a left click on the picture captures the host mouse (relative mode), Ctrl+Alt or losing focus lets it go; `card256`, `card256_usb` (`none`, `mouse` or a folder) and `card256_flash` in `freetvc.ini`; switching the preset to 32k + HBM drops the card with a notice |
| monitor | `card256 [on [slot] \| off \| usb "folder"\|mouse\|none \| flash "file" \| mouse dx dy [buttons]]`; no argument: slot, ports, ROM CRC, font and picture, U2/U3 pages, ROM bank, PSRAM pages in use; the overlay's mode, line counter, enabled sprites, collision words and /INT; the drive selected, the PSRAM drive, the DSK, the USB port and the flash image |
| MCP | `media` actions `card256_on` (slot), `card256_off`, `card256_usb` (path, or `mouse`/`none`), `card256_flash` (path), `card256_mouse` (dx, dy, buttons); `status` reports it with the monitor's text. The `card256` tool reads the card without side effects (`peek_register`, so the collision words and mouse movement stay): `registers` (all 256 and the decoded overlay, sprite, memory, function and USB registers), `sprites` (the 16 sprites with their data address at 9Eh × 4000h + phase × 64 or × 256; `pixels` gives 24 × 21 rows, a hex digit per colour, `.` transparent, and the raw bytes), `tiles` (32 × 30 screen codes at A1h × 400h and colour bytes at A2h × 400h), `ram` (fast RAM 0–47FFFh, or the PSRAM with `psram`) |

Debuggers see the card's pages as the U2/U3 banks (what answers there) and its IOMEM as `CARDn`; the other fast-RAM pages and the PSRAM have no bank of their own.

### Tests and checks

- `core.card256` (no ROMs): ports, mirrors and increments, sprite flags and bit words, masks, read-only registers, collision masks, the border snoop, fast RAM in place of U2/U3 (and on a bare 32k board), FFh for the machine's own RAM, `Machine::load` into the card's pages, ROM banks and card RAM in IOMEM, PSRAM mapping with copy-back, copies sharing PSRAM pages, warm reset and the double-reset reboot, functions 11–14, 17–20, 37, 41 (including ZX7 and the expression quirks), the block in U3, chains with an unknown and a failing function, function 15's answers, the vSID read-back following the queue, and a snapshot with fast RAM, PSRAM and registers.
- `core.card256_overlay` (no ROMs, the CRTC as the ROM sets it): the power-on pictures with and without the font and picture; colours (IRGB to IGRB), two dots a pixel, colour 8, the 256th pixel, the border on both sides, the 16-colour mode's even pixels; A8h at the frame's start and during the paper; A9h and the held pixel; 2- and 16-colour sprites (the hat pixel for pixel), priority against the bitmap; sprite–sprite collisions, the IRQ through 59h and into the CPU (IM 1), 58h not reaching the card, 98h with the bitmap's colour-8 and black quirks, mode 0 clearing 98h, the 2-colour text mask's swapped halves; 16-colour text and its 1E000h font base; extended border X/Y (8 + 7 lines), scroll X; functions 01h, 04h, 06h, 08h–0Ah, 15h–19h, 1Bh, 1Dh (with its extra line), 1Eh/1Fh (a circle's extremes, filled), 20h (aligned, to an odd X, clipped at X = −1, colour 8), 21h (default colour, colour 8 written or skipped), 26h (both windings), 27h and 28h; a snapshot taken mid-frame going on identically; a reset holding the last pixel, then no overlay.
- `core.card256_storage` (no ROMs; the pendrive is a FAT image here): bare-LOAD open, reads to the end, close, handle 0, stale and unknown handles, `.cas` added, a prefix, SAVE with its header, EXIST, bad CAS type, mkdir/chdir/getcwd (with its tail), `..`, prefix chdir, opendir/readdir order, stat, rename, delete, unknown commands, no pendrive (8Ch, and 84h for a bare LOAD); the flash drive (first `.cas`, end of file 41, write 10, mkdir 40, read-only opens, prefix, the trailing-slash quirk, absolute paths, the 4-handle pool, `./`, no image 83h); function 15 into a PSRAM drive, SAVE refused there, function 36; a DSK on the pendrive (read, SAVE into it, the bytes on the pendrive afterwards, unmount, 80h cases); the C0h functions 81h, 83h, 8Bh, 92h; the mouse (scaling, rounding, clearing reads, unread sums, clamping, unplugged); an open file across a snapshot and a machine copy; 8.3 aliases.
- `core.card256_vsid` (no ROMs): no sample before the first block, 128 a block at the PWM floor of 100, the one-block delay, a write read back only with its block, 256 pending writes lost; a sawtooth's period (4096 clocks) and one voice's full swing (to 20380), a 50 % pulse, the test bit holding the output, OSC3/ENV3 read-back; attack 9 reaching the top in about 250 ms and release 0 emptying the envelope; on the machine: silence at power-on, A440 on the bus SOUND line within 2 Hz, a snapshot mid-note playing on sample for sample, a warm reset leaving the note playing with register 24 reading 0, a reboot silencing it.
- `frontend.card256_host`: the host folder (case, aliases, CP437 names, sorted listing, reads, writes past the end, create, mkdir, rename across directories and in case only, remove), the USB port, a folder packed into a flash image. `frontend.rom_search`: `card256_image` padding, font by name, a BMP of another size refused, neither file. `frontend.rom_fetch`: the `ARCHIVE#MEMBER` pieces and a ZIP made by Python (deflate, a damaged member refused).
- `card256.rom.<preset>` (32k-1.2, 64k-1.2, 64k+-2.2; cache variable `FREETVC_CARD256_ROM`, default `card256.rom` in the ROM folder): the card's init, `EXT 0`, `VER` (v0.24), `DIR` (ED:83), `HELP` from ROM bank 1, `X`; the 32k board boots with 42255 bytes free (25871 without the card, as a 64k board has).
- The MinGW builds of `card256_test`, `card256_overlay_test`, `card256_storage_test`, `card256_vsid_test`, `card256_host_test` and `rom_fetch_test` pass under Wine.
- Checked by hand, headless: the disk card in slot 0 with the TVC256k++ in slot 1 and `EXT 1`; a snapshot taken in the command line, loaded into a run started without the card, carries on (`VER`); the monitor's `card256` command; MCP over stdio (`card256_on` in slot 3, `card256_off`); the HBM refusal; both autofetch paths (`--fetch-roms`, `cmake -P cmake/FreeTVCRoms.cmake`) write the four files with the pinned SHA-256s and identical notices. The triangles demo (`trd.cas`) draws its filled and outlined triangles; typed BASIC turns on bitmap mode (the snowy picture) and the power-on sprites (eight hats), and prints `Hi!` through function 06h in 2-colour text mode, transparent over BASIC's text. With the card ROM and the fetched flash image: a bare `LOAD` loads `launcher.cas` and the launcher lists the flash drive and enters `games/`; `LOAD "games/aircraft"` and `RUN`; the main demo (`tvc256k++/demo`, its BMP, sprites and scroller); the imagecopy demo (`tvc256k++/lim`, its base port through `RST 30h` 5Dh, the clown moving over the picture); BASIC `SAVE`/`LOAD`/`RUN` on a pendrive folder (the file on the host with its header); the CLI's `DIR`, `CD` (canonical case in `PWD`), `MKDIR`, `DS F` without an image (ED:83), `VER`; `MDSK FLIPPER.DSK` (a VT-DOS image) on the pendrive, `DIR` of it, Flipper loaded and running from the DSK drive. Sound: the launcher's background music ("Sng player by Noel", from about 9 s), the Tiny SIS Jukebox (`tvc256k++/jukebox`, AnyStone) playing `aoe2` (RMS about 3000–4000 of 32767; the spectrogram shows notes, arpeggios, vibrato and noise drums), the `vsid` register editor finding the card at port 16.
- Checked by hand, GUI: `EXT 0`, `VER`, `DIR` typed at the prompt; the Cards menu with **TVC256k++ in slot 0** ticked and its rib lit; switched on from the menu with the disk card in, it goes to slot 1 (notice "EXT 1") and `card256 = yes` is written; `--card256 --autostart trd.cas` animating; BASIC typed in Play mode turning on bitmap mode and a sprite over the picture, then 2-colour text mode; the launcher from a bare `LOAD`, navigated to `tvc256k++/setmousespd/sms-as.cas` and run; a click captures the mouse (notice), 120 px right moves the sprite X from 80h to BCh and 120 px down Y from 80h to BCh (× 0.5), STAT 1, SPD 16; Ctrl+Alt releases it; SAVE and LOAD on a pendrive folder; Mouse picked from the menu and `card256_usb = mouse` written; the launcher to `tvc256k++/jukebox`, its sort keys, a tune picked.
- **AnyStone's Wolfenstein 3D** (https://downloads.anystone.games/wolf3d-tvc-enterprise, `Wolf.zip`, no readme or licence in the archive): `Wolf256.cas`, `Wolf2dfx.cas` and `WolfSID.cas` look for the card (`256k++`) and the GameCard (`JOY+SN`) and load their data files by name from the card's drive. Headless on 64k+-2.2 with the files as the pendrive: `LOAD "WOLF256"` loads it, its title is drawn by the overlay in bitmap mode (video RAM all zeros), SPACE starts level 1 (the 3D view, health 100, ammo 8, the face), joystick 1 and the cursor keys walk to the door and turn. A TVCS snapshot at the title resumes into the game, also in the GUI with sound; nothing is written to the pendrive.
- Cost: 3000 frames headless take 2.95 s without the card, 3.20 s with it and the overlay, 3.31 s with the vSID too.

### Known limits

- Not checked: a real card; the pendrive and flash dialogs in the GUI (the options and the menu's direct items were); a DSK on the PSRAM or flash drive with the card ROM (unit level on the pendrive only); the mouse's buttons in a program; rewind with storage in the GUI (unit level: snapshot and copy); a SID tune against a recording of a real card; Wolfenstein 3D's `Wolf2dfx`, `WolfSID`, the GameCard's sound in it, later levels.
- Wolfenstein 3D's title and some 3D surfaces show every other raster line dark ❓. Neither `title.bin` nor `titlep.bin` has alternating rows, so either the game draws them so (a scanline shading) or the overlay path differs from the card; no screenshot from the author or a real card was found to compare.
- The overlay's ❓ items: which pixel of a pair 16-colour mode shows, the pixel offset against the TVC's own, the pins after a reset, the firmware's compute time inside a line. The firmware's main-loop delays are not modelled (follow-ups take effect at once).
- Storage's ❓ items: the FatFs version and code page, handle values, the pendrive's directory order and alias numbering, DSK writes on the flash drive; function 36's overrun ⚑.
- The vSID's level against the TVC tone ❓ (not measured), and whether the card's output latency (one block, 3 ms) matters to anything; `sin`/`cos`/`tan` may differ from the MCU's library in the last bit ❓.
- Debugger bank views exist only for U2/U3 and `CARDn`, not for the other fast-RAM pages or the PSRAM.

## Multichip sound card

Vass Sándor's (dikdom) TVC Multichip Soundcard v1.3, "SndMx": an SN76489, an SAA1099 and a YM3812 (OPL2) with a 32 KiB EEPROM. Hardware facts with their evidence: [tvc-hardware.md](../hardware/tvc-hardware.md) §9 "Multichip".

Sources: https://github.com/dikdom/TVC-Multichip-Soundcard at commit `fa23e43` (CC BY-NC-SA 4.0, read as documentation; no code or data copied): the Hungarian manual `doc/TVC Multichip Soundcard-v1.0.pdf`, the Eagle schematic `hardware/pcb-eaglev8.3.1/tvc-soundmagic-sch-v1.3.pdf`, the firmware 1.0.1 sources, and in the same repository the SAA1099 data sheet, the SAM Coupé technical manual and the YM3812 and SN76489AN manuals.

`src/core/cards/multichip.{hpp,cpp}`, `CardKind::Multichip`, default **slot 2** (ports 30h–3Fh, clear of the disk card, the TVC256k++ and the GameCard); any slot works (the firmware scans the four name areas for `05 "SndMx"`).

| Port | Write | Read |
|---|---|---|
| +0, +1 | SN76489 | FFh |
| +2 | SAA1099 data | FFh |
| +3 | SAA1099 register address | FFh |
| +4 | YM3812 register address | YM3812 status |
| +5 | YM3812 data | FFh (the chip's "inhibit") |
| +6, +7 | bank latch b1–0 | FFh |
| +8…+15 | as +0…+7 (A3 is not decoded) | |

- IOMEM C000–DFFF: the 8 KiB bank of the 32 KiB EEPROM that the latch selects, read only (/WE tied high); reset and power-on select bank 3 (the 74HCT74's /PRE), where the `MOPS` header and the init live; a short image reads FFh. ID 11. No wait states: the CPU never waits for a chip.
- **Reset**: /RESET presets the latch and drives the YM's /IC. The SN76489 and the SAA1099 have no reset pin and keep sounding through it, as on the card.
- **Interrupt**: the YM3812's /IRQ is wired to the slot's /INT (schematic), so its two timers can interrupt the TVC through port 59h and the CPU's INT like the TVC256k++'s overlay. The timers are card events (`Card::next_event()`, `run_events()`, see [Slot model](#slot-model)).
- **Clocks**: `src/core/cards/chip_clock.hpp` converts between machine ticks (6.25 MHz) and a chip's own clock counted from power-on, exactly (no drift; a unit test checks it after 30 days of emulated time).
- **Mix and level** ❓: the card's TV jumper position sends both mixer outputs, through 1k each, to the bus SOUND line, so the TV gets the mean of left and right. Mixer gains from the schematic: 22k inputs for the SN and the YM, 12k for each SAA side, 22k feedback (LM358s). Chip levels on one volts scale: the YM3014B's full scale is ±VDD/4 = 1.25 V; a full SAA channel sinks about Iref (data sheet typical 250 µA, 10k reference resistor) through the card's 1k loads, 0.25 V a side; the SN keeps the GameCard's level (all four channels at full volume = 0.75), for no data sheet gives its output voltage. On top of that scale the SAA and the YM are 3 dB lower (level set by listening tests: the YM's full scale at 0.75 was too loud, −6 dB too quiet), so the YM's full scale is 0.53 at the synth input; the SN was not changed. The SAA's stereo is summed (the TVC is mono); the stereo jack is not modelled.
- **SN76489** (`sn76489.{hpp,cpp}`, the GameCard's chip with a clock parameter): here 3.579545 MHz from the card's own oscillator (the manual's N = 3579545 / (32 f)), always running. Step times keep a fraction of a tick (`save_phase()`, outside the GameCard's snapshot layout). Its power-on registers are all muted on this card ❓: the clock runs from power-on, so MAME's all-zero start would sound four full-volume tones until the firmware's init mutes them; real chips start with random registers.

### SAA1099

`src/core/cards/saa1099.{hpp,cpp}`, freeTVC's own, event-driven in the chip's 8 MHz clocks (CLK-8 from its own oscillator), from the Philips data sheet (November 1986) and the SAM Coupé technical manual's appendix; where they are silent, from the notes in SAASound (Dave Hooper's emulator, tuned on real chips; https://github.com/stripwax/SAASound commit `3d92322`, no licence found, read only), with MAME's `src/devices/sound/saa1099.cpp` (BSD-3-Clause, read only) as a second reading. No code copied.

- **Tones**: a half cycle is (511 − offset) · 2^(8 − octave) clocks, so f = 15625 · 2^octave / (511 − offset) Hz (the SAM table's A: offset 227, octave 3, 440.141 Hz). New offset and octave act at the end of a half cycle (the data sheet's buffered controls); the order rule SAASound describes for an offset written after an octave is not modelled ❓. Generators start high.
- **Noise**: two 18-bit Galois LFSRs, x^18 + x^11 + x (Jepael's recording of an SAA1099P, quoted by both), all ones at power-on; clocked at 31.25, 15.6 or 7.8 kHz from a free-running 8 MHz divider ❓, or by every half cycle of generator 0 / 3.
- **Mixers**: tone, noise, both (the tone at half level while the noise is high: SAASound's table), or neither; amplitudes 0–15 a side. SE (register 28 b0) off at power-up (data sheet); RST (b1) holds every generator high with its new data and silences the output, and releasing it starts every half cycle afresh.
- **Envelopes**: two controllers, on channels 2 and 5 only (data sheet: "two of the six tone generators"; MAME applies them to all three of a group), clocked by every half cycle of generator 1 / 4 or, with the external clock, by an address write of register 24 / 25 (the data sheet says "address write pulse"; SAASound and MAME both take only 24/25). The eight shapes of the data sheet's Fig. 5; 3-bit resolution steps by 2, so a ramp takes 8 clocks (the data sheet's 1.95 kHz against 977 Hz); enable and resolution act at once (switching resolution keeps the position's upper bits), the rest only at point (3) or (4); a single shape ends at 0; the right side can be the inverse (15 − level, or 14 − level at 3 bits). An envelope shows where the mixer output is low, so a channel with both mixers off sounds the envelope itself (the data sheet's "non-square tone"); its level is the PDM AND of half the amplitude and the envelope, (a · e + 1) / 2 (SAASound's notes).
- The output is the sum of the channels' averaged pulse-density levels: the PDM carrier (62.5 kHz and up) is above the band and the card's 1k / 10 nF filters, so it is not modelled. No DTACK timing: writes act at once (the manual's 2/7 T rule is not enforced).

### YM3812

`src/core/cards/ym3812.{hpp,cpp}` hosts the vendored **ymfm** (Aaron Giles, BSD-3-Clause, https://github.com/aaronsgiles/ymfm commit `81aec25`, `third_party/ymfm/`): `ymfm::ym3812` at 3.579545 MHz (CLK-3.579), one sample every 72 clocks (49 716 Hz) with ymfm's YM3014B rounding, each sample a band-limited step at its own tick. Its host interface keeps the chip clock of the operation in progress, so the two timers run out at their exact clock: a timer is processed before a sample at the same clock (CSM key-on reaches it), raises the status flags and /IRQ, and re-arms itself as ymfm asks. Busy time follows ymfm's 12/84 clocks; writes are never dropped. Because ymfm keeps a reference to its host, a copy (rewind, saved states, machine clones) builds a new chip and restores ymfm's own save state into it; snapshots carry that state too.

### ROM

`multichip.rom` (CRC-32 704D1C27, `source/firmware/sndmx-fw-v1.0.1.bin`, 32 KiB, bank n at offset n · 2000h) through the ROM search, `--multichip-rom FILE`, and the autofetch from GitHub at the pinned commit with a notice naming its licence (CC BY-NC-SA 4.0, non-commercial) and credits. It is not in the repository. Without it the chips still play and the boot goes on.

The firmware 1.0.1: at a cold start it scans the name areas, mutes the SN, disables the SAA, zeroes every YM register, and shows its screen (slot, base port, "v1.3+ board detected" once a switch to bank 2 and back has worked). SPACE while its three dots appear plays the TVC tone and the SN (C-D-E each), the SAA song (Sean Bernard's Pop 'N' Twin Bee through M. Tarasov's E-Tracker player, from bank 2) and the YM's OutZone ending (a VGM stream timed by the CPU, 71 T a sample); otherwise the boot goes on.

### Front ends

| Front end | Multichip |
|---|---|
| headless | `--multichip [SLOT]` (default 2), `--multichip-rom FILE` |
| GUI | the same options; **Multichip sound card** in the Cards menu (powers on; the tooltip names the chips), its slot rib; `multichip` in `freetvc.ini` |
| monitor | `multichip [on [slot] \| off]`; no argument: slot, EEPROM CRC, bank, SAA enable, YM /IRQ (shares its code with `gamecard`) |
| MCP | `media` actions `multichip_on` (slot) and `multichip_off`; `status` reports it |

Snapshots carry the card in `SLOT` (with its EEPROM image) and its bank and three chips in `CARD` (version 1).

### Tests and checks

- `core.multichip` (no ROMs): the chip clock conversion both ways, also after 30 days; the SN at 3.579545 MHz (period 254 → 440.4 Hz within 1 Hz) and at +1 and +8; the SAA off at power-up, A at 440.14 Hz, the octave buffered and doubling it, sync silencing, noise, a repetitive decay on channel 2 repeating every 16 clocks of generator 1 and every 8 at 3 bits, the external clock stepped by two address writes of 24 (46/53 of the level, the PDM rule); the YM's sample rate, a sine at block 4 F-number 580 (440.0 Hz within 1 Hz), timer 1 at 16 counts raising port 59h b2 after 1287 µs within 10 µs with status C0h, reads of +5, IRQ reset, the timer firing again, a stopped timer, reset clearing it; port 5Ah, only the YM driving reads, the banks, read-only EEPROM, reset to bank 3; a snapshot and a copy with all three chips and a running timer sounding the same afterwards, the timer agreeing.
- Both autofetch paths write `multichip.rom` byte-identical to dikdom's repository's file, with the same notice.
- Checked by hand, headless on 64k+-2.2: the card's screen ("Used slot: #2", "Base port address: $30", "v1.3+ board detected"); SPACE at frame 400, then 40 s of demos recorded: the TVC tone and the SN both play C3 130.8, D3 146.8, E3 164.8 Hz; the SAA and YM songs' notes sit on the equal-tempered scale (median 9 cents off, 86–87 % within 20 cents, against 25 cents and 40 % for random pitches), so the SAA's octave/offset formula and the YM's sample rate are right; the spectrogram shows notes, arpeggios and vibrato. At the chosen level the SAA song's RMS is 0.131 (peak 0.59) and the YM song's 0.094 (peak 0.50), the TVC tone 0.28–0.31 and the SN 0.08, no sample clipped. A snapshot saved at frame 1500 and resumed gives the uninterrupted run's audio sample for sample (385 843 samples). Cost: 2400 frames (48 s) in 2.3–2.5 s with or without the card.
- Checked by hand, GUI: the Cards menu item switching the card in (notice "Multichip sound card in slot 2; powered on"), the card's screen, SPACE starting the demos, the item checked with its tooltip, `multichip = yes` saved and the card back from the ini at the next start with its slot rib lit. Monitor (`--moncommands`): `multichip`, `off`, `on 3`, `on 0`. MCP over stdio: `multichip_on` in slot 3, `status`, `multichip_off`.

### Known limits

- Not checked: a real card; other software for the card (dikdom's repository's VGM and SAA players come as Intel HEX and are not built); not compared with WinTVC 1.4.1's `SndMx` card.
- The three chips' levels against the TVC tone ❓ (set by listening); the SN's power-on registers on a running clock ❓; the SAA's offset-after-octave rule and its noise divider's phase ❓; DTACK and YM busy timing are not enforced; the stereo jack is not modelled.

## NanoSD

Vass Sándor's (dikdom) NanoSD IO Card, rev 2: an Arduino Nano with an SD card socket behind a pair of latches, and an EEPROM with the card ROM. Hardware facts with their evidence: [tvc-hardware.md](../hardware/tvc-hardware.md) §9 "NanoSD".

Sources: https://github.com/dikdom/TVC-NanoSD at commit `3fb88f2` (BSD-3-Clause, Copyright (c) 2021, dikdom): the rev 2 schematic, the manual v0.3, the Arduino Nano firmware v0.32 (**[NFW]**: `TVC_NanoCartSD.ino`, `CircularQueue.cpp`, `PathStack.cpp`) and the ROM v0.33 sources; the Arduino SD library in the same repository (`SD.cpp`, `SdFile.cpp`, GPL-3.0) was read as documentation of what the Nano sees. freeTVC's own C++; nothing copied.

`src/core/cards/nanosd.{hpp,cpp}`, `CardKind::NanoSd`, default **slot 3** (ports 40h–4Fh, the only slot clear of the other cards' defaults); any slot works.

**The card.** Every port of the slot is the one register. A write goes to the Nano's input queue; a read returns the byte the Nano latched after the previous read, then the Nano latches the next one: its status while that has b7 set, else the next byte of its output queue, else BUFFER_UNDERRUN (89h). So a status below 80h (OK, OPEN_OK) is never seen itself: the ROM reads the data behind it. IOMEM C000–DFFF is the EEPROM's 8 KiB at offset C000h + 2000h · bank, read only; the bank is the Nano's A13 line, which a BANKTO0/1 command sets but which reaches the pin only at the next read (the firmware writes it with the latched byte). ID 11, no interrupt; the reset button does nothing to the card (RESET is not wired), power-on resets the Nano.

**The Nano** (`NanoSd`, plain data so snapshots and copies carry it): the firmware's main loop as a state machine. A command collects its input bytes as they arrive, then runs at once; the Nano is a 16 MHz AVR and the ROM waits at least 100 µs after a command (27 T between bytes, 1 ms before the first poll of OPEN), so no ROM path can tell. The one wait kept is the card-detect handler's ≈ 300 ms debounce loop, during which the status stays DATA_NOT_READY_YET; it counts from the moment the front end put the card in or took it out (`NanoSdMedia::sd_changed`, machine time), since the Nano was idle until the TVC's next access. Both queues hold 80 bytes ⚑ (the firmware builds them with `CircularQueue q = new CircularQueue(80)`, which `-fpermissive` turns into a pointer cast; whatever size results, GETDATA's 65-byte pushes work on the card, so 80 is kept). A write into a full input queue is dropped with BUFFER_OVERRUN; an output push that does not fit hangs the Nano until power-on, as on the card (every push happens while the status has b7 set, so no read can drain it).

Kept from [NFW]:
- OPEN (00h): closes an open file, takes a name up to 00h; an empty name gives the next `.CAS` file of the current directory (its own iterator, restarted by CHDIR and card insert); otherwise the name is prefixed with the current directory, upper-cased, and `.CAS` appended when the name does not exist and lacks it. The reply is the name without its last 4 characters (whatever they are), then the size as 4 bytes; a `.CAS` file whose first byte is 01h/11h skips its 128-byte header.
- GETDATA (05h): up to 64 bytes behind a count byte; 0 bytes left is END_OF_FILE. CLOSE (01h) clears the output queue.
- LIST (03h): each entry is 00h + 12-byte space-padded 8.3 name + type (00h directory, 01h file) + 4-byte size (`.CAS` names minus 128 without looking at the first byte); the Nano then waits for ACK_LIST (04h); any other byte ends the list (READ_OUT_OF_SYNC, then END_OF_LIST) and is consumed.
- FINFO (06h): the entry of a name, as the TVC spelled it (not upper-cased, so a lower-case `.cas` keeps its 128 bytes), and only 17 of its 18 bytes: the size's top byte is cut (`buffer[17]` holds 18).
- CHDIR (02h), MKDIR (0Fh, makes every missing directory on the way), RMDIR (10h, empty directories only), DELETE (11h, files only, not read-only): the path is made absolute with the firmware's `PathStack` (at most 8 levels, `../` pops, `/../` from the root empties the path, which then works as the root).
- CREATE (0Ah): a length byte (at most 63 characters taken; the rest are read as what follows), the name, a type byte; no dot appends `.CAS`, a name not ending in `.CAS` writes a plain file; 01h/11h write the type byte and 127 zeros first. ⚠ **CONFLICT**: the manual says CAS_CRTE overwrites ("korábbi tartalom törlődik"); the firmware opens with `FILE_WRITE`, which in its SD library is `O_CREAT | O_APPEND`, so saving onto an existing name appends to it. freeTVC follows the firmware.
- PUTDATA (0Bh): a count and the bytes; CLOSEWRITE (0Ch) pads a type-01h file to a multiple of 128 bytes; the CAS header's block counts stay zero (the firmware's seek back is commented out: "does not work on SD filesystem during write").
- GETPARAMETER (0Dh) / SETPARAMETER (0Eh): menu at boot (0), sort (1), SD present (2, read only), the firmware's version string (3); the EEPROM gets its ID and the defaults (menu on, no sort) at the first parameter call. SETPARAMETER with an unknown ID takes no value byte, so the ROM's value byte is read as the next command.
- Without an SD card only GETPARAMETER, the card-detect event and command FEh (answers 75) are served; anything else gives STATUS_NO_SD (FFh) and empties the input queue.

**The SD card** is a host folder (`PendriveFolder`, shared with the TVC256k++, outside the machine state), seen through `NanoSdCard`, a reading of what the Arduino SD library does: 8.3 names only (a long name shows as its alias), case ignored, path components of at most 12 characters, `make83Name`'s rules (printable ASCII, one dot, 8 + 3, none of `|<>^+=?/[];,*"\`), `.` and `..` hidden; entries in the folder's order (sorted by name ❓: the card shows FAT directory order). A folder plugged in or out flips the card-detect pin; the Nano's pin-change interrupt pushes event 12h into its input queue (disabled until the event has been handled, as in [NFW]). FatFs-style volumes work too (the unit test uses an in-memory FAT image); SD.begin is "the root can be listed".

**The Nano's EEPROM** (menu at boot, sort) lives with the host (`NanoSdMedia`), not in the machine state, like a disk: snapshots and rewind leave it alone, `freetvc.ini` keeps it as `nanosd_menu` and `nanosd_sort` (written once the ROM has asked for a parameter, which gives the EEPROM its ID).

**Autostart with the card in** (a host shortcut, like the disk card's cassette-class handover): the menu reads the OS's copy of the keyboard matrix (`PICTURE`, 0B51h + row), not the keyboard class, so an autostart would wait in it for ever. For an autostart's boot the Nano answers parameter 0 with 0 (`NanoSdMedia::skip_menu`, cleared once the autostart is over); the ROM then shows "Press ESC to enter file-select menu!" for 2 s and boots on. The EEPROM is not touched. The CAS autostart then gives the cassette class back to the host cassette as for any card.

### ROM

`nanosd.rom` (CRC-32 AF3C8EB7, `bin/NanoSDROM-v0.33.bin`, 64 KiB, the code at C000h–DD2Fh) through the ROM search, `--nanosd-rom FILE`, and the autofetch from GitHub at the pinned commit (BSD-3-Clause, so it may be redistributed with its notice). Without it the Nano still answers but the machine never calls the card.

The ROM v0.33: the init takes the cassette class, writes a fake `EXT 0`, asks the Nano for parameter 0 and shows its file-select menu (or "Press ESC to enter file-select menu!" for 2 s); RETURN on a `.CAS` file types `LOAD "name"` through its interrupt hook once BASIC is up; `EXT 0` (always 0: the ROM hooks 0021h whatever its slot) starts its command line (`DIR [/W]`, `CD`, `PWD`, `MKDIR`, `RMDIR`, `DEL`, `VER`, `CLS`, `HELP`, `EXIT`).

### Front ends

| Front end | NanoSD |
|---|---|
| headless | `--nanosd [SLOT]` (default 3), `--nanosd-rom FILE`, `--nanosd-sd DIR` (brings the card too, unless `--snapshot` brings the machine) |
| GUI | the same options; **NanoSD** and **NanoSD card folder...** in the Cards menu (with "Take the SD card out"), its slot rib; `nanosd`, `nanosd_sd`, `nanosd_menu`, `nanosd_sort` in `freetvc.ini` |
| monitor | `nanosd [on [slot] \| off \| sd "folder"\|none]`; no argument: slot, ROM CRC, bank, the Nano's status, queues, current directory, open file, the SD folder |
| MCP | `media` actions `nanosd_on` (slot), `nanosd_off`, `nanosd_sd` (path, or none); `status` reports it |

Snapshots carry the card in `SLOT` (with its ROM image) and the Nano's state in `CARD` (version 1).

### Tests and checks

- `core.nanosd` (no ROMs, an in-memory FAT volume): port 5Ah, the latch one read behind, the 16 ports aliasing, BANKTO1/0 reaching A13 only at the next read; the parameters (EEPROM ID and defaults at the first call, the version string without "-dbg", an unknown ID, SETPARAMETER with an unknown ID leaving its value byte as the next command); OPEN with upper-casing and `.CAS` added, the reply's name and size, GETDATA 64 + 64 + 22 then END_OF_FILE, a plain file, FILE_NOT_FOUND, OPEN "" walking the `.CAS` files; LIST with a directory and a long name's alias, a non-ACK byte ending it; FINFO's lost byte and its case; CREATE/PUTDATA/CLOSEWRITE with padding, the append onto an existing name, plain files, lower-case `.cas`, "A.B"; MKDIR on two levels, CHDIR with `../..` and `/../`, RMDIR and DELETE rules; no card (FFh, FEh → 75), insert and debounce, removal, BUFFER_OVERRUN in the debounce; the hang on a full output queue, reset not reaching it, power-on restarting it; a copy and a snapshot going on mid-block with the same bytes.
- `nanosd.rom.<preset>` (32k-1.2, 64k-1.2, 64k+-2.2; cache variable `FREETVC_NANOSD_ROM`, default `nanosd.rom` in the ROM folder) on a host folder: the start-up line and the menu (a long name as `SOMELO~1.CAS`, a directory), the EEPROM written by the boot's parameter call, ESC to BASIC, SAVE writing the firmware's header, LOAD finding it and RUN, `EXT 0`, `VER` (the Nano's version), MKDIR/CD/PWD and the host folder made. `frontend.settings` covers the ini keys. The MinGW builds of `nanosd_test`, `settings_test` and `nanosd_rom_test` (64k+-2.2) pass under Wine.
- Both autofetch paths write `nanosd.rom` byte-identical to dikdom's repository's `bin/NanoSDROM-v0.33.bin` with the same notice; the ROM search finds it by name; without it a notice says so.
- Checked by hand, headless on 64k+-2.2: the menu (`BOINGB~1.CAS 12628`, `GAMES <DIR>`, `NUPOGODI.CAS 7507`), RETURN loading and running BoingBall; ESC, then `EXT 0` with `VER`, `DIR`, `MKDIR TEST`, `CD TEST`, `PWD` (`/TEST/`), `CD ..`, `DIR /W`; SAVE/NEW/LOAD/LIST/RUN through the card (HELLO.CAS: type 11h, zero header, the program); a CAS autostart (NuPogodi) with the card in, which needs the menu shortcut above; a TVCS snapshot saved in the menu and resumed, RETURN then loading BoingBall; the monitor's `nanosd` in every form and MCP over stdio (`nanosd_on`, `nanosd_sd`, a run into the menu, `status`, `nanosd_off`); over MCP, the SD card taken out while the menu runs (within 6 s the list empties and "PWD read failed!" shows, the manual's "PWD failed") and put back (the list returns).
- Checked by hand, GUI: the card's menu, SPACE switching "Enter menu on boot" off, ESC, typing, SAVE onto the host folder, `EXT 0` and `DIR`; the Cards menu with "NanoSD in slot 3", the SD folder and "Take the SD card out"; `nanosd_menu = no` saved; the next start putting the card in from the Cards menu (`nanosd = yes` saved) and the ROM's "Press ESC to enter file-select menu!" at boot.
- The card also serves the MultiCart tools MCRead, MCFlash and MCVerify ([MultiCart v2](#multicart-v2)).

### Known limits

- Not checked: a real card; the GUI's folder dialog and its "Take the SD card out" item (the same call as MCP's `nanosd_sd none`).
- Open ❓: the queue sizes ⚑ (`new CircularQueue(80)` through `-fpermissive`; the AVR binary is not disassembled); the debounce's real length (≈ 300 ms by the firmware's comment); how long the Nano takes to boot against the TVC's first card access (taken as ready at once); the directory order (sorted by name; the card shows FAT order); OPEN of a directory (here empty; on the card the library reads its raw entries); a CAS file shorter than its header (a negative count overruns the Nano's buffer on the card; here END_OF_FILE); the SD card's FAT version and partitions (the manual: the first FAT16/FAT32 partition); an SD image file through FatFs.

## MultiCart v2

Vass Sándor's (dikdom) MultiCart v2, board `seleprom6-7` ("v2.2" in its docs): a cartridge in the program-module port, not a slot card, with two 512 KiB flash chips, 128 KiB of RAM, a flash used as a PLA, and a menu firmware that starts carts and CAS files stored in the flash.

Sources: https://github.com/dikdom/TVC-Multicart-v2 at commit `631b251` (BSD-style licence in the README, Copyright (C) 2017-2023 Sandor Vass): **[MSCH]** the Eagle schematic `hardware/eagle/seleprom6-7.sch` (netlist read from its XML by script); **[PLA]** `software/tvcpla/tvcpla.pas` and the image it generated, `tvcpla.bin`; **[MFW]** the firmware 2.1.14 sources and binaries (`software/firmware/`); **[MRB]** the ROM builder `software/rom-builder/unit1.pas` (MC21RM); **[MCT]** the TRSE tools `software/mc-tools/mcv2.tru`. **[SST]** the SST39SF010A/020A/040 data sheet DS20005022F (Microchip, https://ww1.microchip.com/downloads/aemDocuments/documents/MPD/ProductDocuments/DataSheets/SST39SF010A-SST39SF020A-SST39SF040-Data-Sheet-DS20005022.pdf). freeTVC's own C++; nothing copied.

`src/core/bus/multicart.{hpp,cpp}` (the board), `jedec_flash.{hpp,cpp}` (the flash chip: `JedecFlash` with `kSst39sf040`), `CartType::MultiCart`; image builder `src/frontend/common/multicart_image.{hpp,cpp}`. Plain cartridges, KiloCart and MegaCart are in [media.md](media.md) ("Cartridges").

**The board** [MSCH]:
- The cartridge port has no /WR: a cycle with /CART low and /BRD high is a write. A 74HCT123 pair delays and stretches /CART into the PLA's /OE window; outside it the PLA's outputs float and RN1 pulls them inactive.
- The PLA is a 128 KiB flash used as a look-up table. Its address pins: A1–A4 from the CPU, A5–A8 from a 74HCT32 (A9|A10, A6|A11, A7|A8, A5|A12), A9 = CPU A13, A10 = /BRD, A11 = the window, A12–A14 = the register's b7 (REGREL), b6 (RAMSEL) and b5 (A19), A15 = RESENA, A16 = /RST; A0 is grounded. Outputs: the two flash /CE, the SRAM /CS1, /OE, /WE, the register's clock /REGWR and its clear (D6, through D2). So the register is exactly C000h–C003h (A0 ignored, A1–A12 all decoded), or E000h–E003h while b7 is set.
- A 74HCT273 is the register: b4–0 the 16 KiB page (flash A14–A18), b5 the second flash chip, b6 the RAM instead of the flash, b7 the register at E000h. A write at C000h/C001h also sets RESENA through a 74HCT74 (D = A1, clocked by /REGWR): a later read of C000h/C001h clears the register; a write at C002h/C003h clears RESENA. Register writes never reach the memory.
- /RST comes from an RC circuit (C1, R1, D1) only: power-on clears the register and sets RESENA; **the reset button does not reach the cart** (the port has no reset line). With RESENA the ROM's own `MOPS` read at C000h after a reset clears the register, so the reset button brings back the menu; with "Reset ... NO" (C002h) the selected cart boots again.
- ⚠ **CONFLICT** [PLA]: `tvcpla.pas` tests input bit 7 for the read reset, which can never be set at C000h; `tvcpla.bin`, the image actually burned, clears on a read whenever RESENA is high. freeTVC follows the binary; `MultiCart::pla()` equals it for all 131 072 inputs (`core.multicart`). The clear is asynchronous, so the read returns page 0 of chip 0 (C000h–C003h: the cleared register's map).
- RAM: a 628128 (128 KiB, the BOM's), A14–A16 from b2–0; the jumper JP1 at VCC, so pages 8–31 repeat 0–7 (the firmware's size test finds 128 kB). The 512 KiB fit-out (jumper to A18) is not emulated.
- Flash: two SST39SF040 (the BOM's), 512 KiB each, A14–A18 from b4–0, chip 1 by b5. Both always present: a shorter image leaves the rest erased.

**The flash chips** [SST]: freeTVC's own `JedecFlash` with the `kSst39sf040` figures. Byte program (AAh/55h/A0h, then the byte), sector erase (4 KiB, AAh/55h/80h/AAh/55h, 30h at the sector), chip erase (… 10h at 5555h), software ID (… 90h: BFh at address 0, B7h at 1; ❓ A0 alone decides elsewhere), ID exit (F0h anywhere, or AAh/55h/F0h); command addresses compare A14–A0; an invalid cycle aborts to read mode (and may start a new sequence). The data sheet's typical times (14 µs, 18 ms, 70 ms) at machine ticks; meanwhile reads give Data# polling on DQ7 (the complement of the final bit 7; 0 while erasing) and the toggle bit on DQ6 (high first), other bits 0 ❓, and commands are ignored. A program only clears bits ❓ (the data sheet says to erase first). ⚠ The table's maximum for byte program (10 µs) is below the first page's typical 14 µs in DS20005022F; 14 µs is used. The AM29F0x0 and AT29C0x0 the firmware also knows are not offered (the firmware can only save and delete on SST chips), nor are other flash chips.

**The flash is media**: its bytes stay outside the machine state like a disk, in `MultiCartFlash` owned by `HostTape` and attached to the memory map; a copied machine has none, an assignment keeps its own. Snapshots (`MCRT` chunk: register, RESENA, RAM, both chips' command states) name the image by path in `MEDI`; loading one reloads the image only when the path differs; rewind never undoes flash writes. A changed flash goes back to its file half a second of machine time after the last write, and when the cart is removed, replaced, or the program ends; the file keeps its length while the flash past it is still erased (a one-chip image stays 512 KiB).

**Images** (`src/frontend/common/multicart_image.{hpp,cpp}`, freeTVC's own builder after [MRB]'s layout as the firmware reads it):
- 1 MiB, chip 0 then chip 1, 64 banks of 16 KiB; blocks of 256 bytes. Bank 0 holds the firmware (its first 12 KiB, `MOPS`, version at +6, the BAM's address at +8, F000h), the BAM at F000h (chip 0's 2048 blocks then chip 1's, one bit each, the lowest block in bit 0, set = used) and the directory at F200h: 20-byte entries (type 1 BASIC, 2 CART, 3 CAS, 4 BIN, 5 DAT; bank; first block; size in blocks; 16 characters, Hungarian letters as the TVC's 80h–88h/90h–98h), the first one "EXIT to BASIC" from the firmware, a type 0 ending the list.
- A cart (`MOPS` at its start) takes the first bank whose first blocks are all free (up to 16 KiB). A CAS file's payload (from 90h, as long as its 82h–83h length, else the rest of the file as BIN, as [MRB] does) goes into a chain of blocks taken from the first free one: each block is the next block's bank and block, then 254 bytes; the last has FFh and its byte count.
- [MRB] starts from zeroed chips, and so does freeTVC; the firmware erases a sector before it writes into it. Differences from MC21RM: a bank is checked for all of a cart's blocks (MC21RM skips the last one), a CAS's block count comes from the payload written, a full directory or a file that does not fit is an error, other non-ASCII characters become `?`.
- Detection: `MOPS`, `JR` at +4, version 21h or later at +6, the BAM inside the window, more than 64 KiB; otherwise the size rules of plain cartridges apply ([media.md](media.md), "Cartridges").

**The firmware 2.1.14** runs unchanged; observed behaviour:
- ⚠ **A CAS file does not start on SYS 1.2**: `EXEC_CAS` does `CP 2; CALL NZ,CONFIGURE_EXT0; JP NZ,MEMORY_PAGING` (CA4Bh in the English binary), but 2.1.14's `CONFIGURE_EXT0` (the new NanoSD search) returns with Z set, so the 1.2 path falls into the 2.x exit, which clears the program area; BASIC comes up empty after the logo. Dynamic run: at CA4Eh F = 68h (Z set), SYS 1.2. The older routine, kept in the source as comments, used only LD/LDIR. Carts, the menu, SAVE and delete work on 1.2.
- ⚠ **Delete clears the BAM bit below the freed block**: `L_ALLOCATION_BYTE_FOUND` turns block d of a byte into mask bit d − 1 (d = 0 clears nothing), so a deleted CAS file keeps one block per BAM byte allocated, and freeing its first block can free the block before it, which may belong to another file. Seen in the flash after a delete (bank 1, blocks 0–49 of BoingBall: bit 7 of each byte stays set, the last byte clears bit 0 only), equal to the code's arithmetic.
- The start-up logo (SYS 1.2 and 2.2) takes the first key after "EXIT to BASIC", as at any boot ([tvc-hardware.md](../hardware/tvc-hardware.md) §7); on SYS 1.2 a CAS start goes through it too.
- SAVE goes through `EXT 0` (`EXT 1` when 2.1.14's NanoSD check finds one): the BASIC program into free blocks, then the BAM's sector rewritten (read into TVC RAM, erased, programmed byte by byte).

### ROM

The firmware 2.1.14 in English, Hungarian and Spanish (`multicart_v2.1.14{en,hu,es}.bin`, CRC-32 7C275018, 50B96903, FFB77D45) as `multicart-en.rom`, `multicart-hu.rom`, `multicart-es.rom` through the ROM search and the autofetch from GitHub at the pinned commit (BSD-style licence, so they may be redistributed with their notice). Only the image builder needs one; an image carries its own copy.

### Front ends

| Front end | MultiCart v2 |
|---|---|
| headless | `--cart FILE` (detected, or `--cart-type multicart`); `--multicart-build OUT` with `--multicart-add FILE` (repeatable, in menu order) and `--multicart-lang en\|hu\|es`, then exit (the directory is printed) |
| GUI | `--cart`; drop or "Insert cartridge..."; **New MultiCart image...** in the Cards menu (the programs, then where the image goes; English firmware; it goes in and the machine powers on) |
| monitor | `cart "file" [multicart]`; `cart build "out" en\|hu\|es "file"...`; `cart remove`; `cart` shows the register, RESENA, the number of files and unsaved flash |
| MCP | `media` `cart_insert` (`cart_type` multicart), `cart_remove`, `cart_build` (path, files, language); `status` reports it |

Autostart with a MultiCart in times out ("timed out booting"): its menu reads the keyboard itself, so the autostart never sees BASIC. Every booting MOPS cart does the same (Tetris: "timed out waiting for the BASIC prompt").

### Tests and checks

- `core.multicart` (no system ROMs; cache variable `FREETVC_MULTICART_DIR`, default `testdata/TVC-Multicart-v2`, the repository at commit `631b251`, for `tvcpla.bin`): the PLA against `tvcpla.bin` (131 072 inputs, 0 different); the register at C000h–C003h only (C004h and C200h not), RESENA both ways, the read reset and its page-0 read, relocation to E000h; RAM pages and the 128 KiB repeat; ID mode on both chips, byte program with Data# polling and toggle bit, ignored commands while busy, bits only cleared, an aborted sequence, A14–A0 decoding, sector and chip erase with their times; in a `Machine`: CPU writes to the register and RAM, debugger reads without the reset, debugger writes into RAM, the reset button not reaching the cart, a snapshot with the flash still attached, a copy without it, power-on; the builder (accents, chains, cart bank, BIN, BAM, zeroed chips, refusals).
- `multicart.rom.<preset>` (64k-1.2, 64k+-2.2) with the English firmware from the same folder: the menu with both chips' IDs and 128 kB RAM, a cart from bank 1, the reset button back to the menu, SPACE and the reset button starting the cart again, `EXT 0` saving a BASIC program into the flash, it running from the menu (on SYS 1.2: BASIC empty, as above), the delete with its leaked block. The MinGW builds of `multicart_test` and `multicart_rom_test` (64k+-2.2) pass under Wine.
- Both autofetch paths write the three firmware files with the pinned SHA-256s.
- Checked by hand, headless, with BoingBall, NuPogodi, BUEK-Demo and the Tetris cartridge dump: the builder's directory; the menu ("Free space on cart: 3850 blocks"); BoingBall and Tetris started from it; a delete and an `EXT 0` SAVE on SYS 2.2, the saved program run after power-on; SYS 1.2: carts start, CAS files stop at an empty BASIC (traced to CA4Eh as above); the Hungarian firmware through `cart build` in the monitor; MCP over stdio `cart_build` (Spanish) and `cart_insert`; a TVCS snapshot saved in the menu and resumed from the snapshot alone (the image by path; the selection kept; RETURN started NuPogodi); a missing image gives "MultiCart image left out".
- dikdom's own `mcdetect.cas` ([MCT]), loaded through the host cassette after "EXIT to BASIC": "MOPS jelzés megvan - valid cart", "RAM: 128kB", "EEPROM #1:BFB7 (SST39SF040-512)" and the same for #2, an independent reading of the ID mode and the RAM.
- [MCT]'s tools on the emulated cart, with the NanoSD in slot 3 serving the files (their readme names tape, floppy or the NanoSD; through the host cassette or a tape MCRead stops after its first 16 KiB block with E9h, because an unbuffered block write ends the file there, EXT E6FCh): **MCRead** dumped both chips (32/32 blocks each) into `DUMP.CAS`, whose payload equals the image byte for byte; **MCFlash** erased and wrote both chips from a raw 1 MiB `NEW.BIN` built by freeTVC's builder (64/64 blocks each, "File lezárva"), and the image written back to the cart's file equals `NEW.BIN`; **MCVerify** passed both chips against `NEW.BIN` ("megegyezik") and against the old dump reported the first difference at #12309, where `cmp` finds it. The menu's RAM test (M) passed at 128 kB.
- Checked by hand, GUI: the firmware's menu, BoingBall started with synthetic keys, a delete written back to the image on exit (the same bytes as the headless run), the Cards menu with "New MultiCart image...", and through it the file dialogs building an image of NuPogodi that went in and ran.

### Known limits

- Not checked: a real cart; the menu's rename and sort keys; BIN and DAT saves.
- Open ❓: the ID bytes away from addresses 0 and 1 and the status bits besides DQ7/DQ6; whether programming an unerased cell ANDs; autostart with any booting cart in (times out, above). The firmware's SYS 1.2 CAS start and its delete behave as described above.
- Not emulated: the 512 KiB RAM fit-out and other flash chips.

## SD Dual

Sebestyén Pál's SD Dual cartridge (2015; "TVC SD illesztő Dual verzió"), the fourth of his SD boards after the slot-card SD interface (2009), the SD cartridge (2011) and the microSD cartridge (2013). It sits in the cartridge port and holds a Xilinx CPLD, a TSOP48 flash with his firmware and an adapted VT-DOS 1.2, an 8 KiB SRAM (CY7C185) and two sockets, microSD and full-size SD.

Sources (no schematic or CPLD equations were published): **[SDW]** the author's site `sdinterface.atw.hu`, now offline, read from the Internet Archive's copy of http://users.atw.hu/sdinterface/ (snapshots of May 2025): the firmware ZIPs with `vtdos_cr.bin`, `SDFLASH.CAS`, `HASZNAL.TXT` and `ROM_VER.TXT`, the VT-DOS system files (`system_f_v2.zip`), sources of his file manager and demos, board photos, and `tvcemu.zip`, his Windows emulator MinTVC (`emu.ini` with `SDcartridge`, `.img` card images); **[SDM]** the user manual on szergitata.blog.hu (2016-11-17); **[JTV]** jtvcemu (https://github.com/erno-szabados/jtvcemu, Apache-2.0; Hoffer Gábor, Szabados Ernő, Sebestyén Pál), `emulator/tvc/SDcartridge.java` and `SDcard.java` at its May 2025 head, the designer's own model of the board, read for behaviour only; **[SFW]** a disassembly of the firmware 2022.11.09 (z80dasm, page by page; addresses are C000h-based within a page). The SD card follows the SD Association's Physical Layer Simplified Specification (SPI mode). freeTVC's own C++; nothing copied.

`src/core/bus/sddual.{hpp,cpp}` (the board), `sd_card.{hpp,cpp}` (an SD card in SPI mode), `jedec_flash.{hpp,cpp}` (the flash), `CartType::SdDual`; host side `src/frontend/common/sd_image.{hpp,cpp}`.

**Not built: YATE's "SDCart".** YATE's `Expansions/SDCart` (https://github.com/hightower70/YATE) is an emulator-side design: a 578-byte ROM of its own and a C# "co-processor" serving CAS files from a host folder through windows at E000h/E800h/F800h. No such hardware is documented; its job is what freeTVC's host cassette does ([media.md](media.md), "Host cassette").

**The board** ([JTV], cross-checked against [SFW]):
- The cartridge window (C000h–FFFFh when page 3 is CART, 0000h–3FFFh when page 0 is): C000h–DFFFh the flash in 8 KiB pages, E000h–FBFFh the SRAM (its top 1 KiB is hidden), FC00h–FFFFh the CPLD's registers, A1–A0 decoded (they repeat every 4 bytes).
- +0: SPI data. A write sends the byte to the selected cards; a read returns the byte the last transfer brought in (MISO floats high, FFh, with nothing selected). Transfers are instant here: the CPLD clocks SPI from its own crystal, and [SFW] waits one `NOP` between a write and a read (D200h–D209h).
- +1 read, status: b7 the SD socket's write-protect switch (set = protected or no card; [SFW] D449h checks it only when socket 1 is selected: a microSD socket has no switch), b6 no card in either socket (D31Dh skips the scan), b5 a card went in or out; b4–b0 read 0 ❓. +1 write, control: b7 selects socket 0 (microSD), b6 socket 1 (SD), b5 clears the change latch (D31Bh writes 20h). The firmware scans socket 0 first and gives its partitions the first drive letters ([SDM]; [SFW] D591h).
- +2 write: the flash page in b7–5 (8 pages, 64 KiB); b4–0 are ignored ([SFW]'s updater writes the target's address bits there, SDFLASH 1BE0h). Every page starts with `MOPS`, `XOR A`, `LD (FC02h),A`, so whatever page is selected, the ROM's `MOPS` check at reset lands in page 0: the board needs no reset line, and has none here (power-on clears the page).
- +3 write: b7 fast mode. Then every register read returns the byte in and clocks FFh out, so `LD HL,FC00h; LDIR` reads a block (D234h–D244h: one priming read, 512 reads, one more for the CRC); reads of +2/+3 otherwise give 00h ❓.
- The far call at EC99h (a RAM copy of C0F1h) switches pages by the target's high byte: b7–5 the page, then the address with b5 cleared and b7–6 set.

**The flash** ([SFW] SDFLASH.CAS; [JTV] has none): AMD-style commands at AAAh/555h within the window (a x16 part in byte mode), Data# polling on DQ7 and the toggle bit on DQ6. The updater erases once (80h … 30h at C000h of the current page) and then programs and verifies all eight pages, so one sector holds the 64 KiB: `kSdDualFlash` has a single 64 KiB sector ❓, no ID (FFh ❓, never read) and the Am29LV family's typical 9 µs program and 0.7 s erase ❓. Writes need the jumper the manual describes (off as shipped, `--sddual-flash-write`); the flash is part of the machine state (snapshots carry it) and is never written back to the firmware file, whose CRC-32 the ROM search and snapshots match.

**The SD card** (`SdCard`, freeTVC's own after the Simplified Specification's SPI chapter; every path the firmware takes was checked in runs): CMD0 with a valid CRC moves it to SPI mode; CMD8 (R7), CMD55 + ACMD41 and CMD1 (ready on the second call ❓), CMD58 (OCR, CCS), CMD59, CMD9/10 (CSD/CID as data blocks), CMD16, CMD17/18 (one byte before the FEh token, CRC16), CMD12 (a stuff byte, then R1b), CMD24/25 (FEh / FCh tokens, FDh to stop, data response 05h/0Bh/0Dh, then busy), CMD13; illegal commands in the idle state; CRC7 checked on CMD0 and CMD8 (and on everything after CMD59). Up to 2 GiB it is SDSC with CSD 1.0 (READ_BL_LEN 9, 10 above 1 GiB) and byte addresses; larger images are SDHC/SDXC with CSD 2.0 and block addresses, up to 2 TiB. The CID names no manufacturer (MID 0, OID "FT", "FTVC ").
- ⚠ The firmware sends HCS only on the first ACMD41 of its loop: `sub_d288h` returns with DE = the argument's low word, so the loop at D2C7h pushes D = 0 after the first try. A card following the strict rule (an SDHC card never gets ready without HCS) stays idle for all 16 384 tries; freeTVC's card takes the HCS of the call that starts its initialisation ❓, as real SDHC cards must, since the author reports SDHC and SDXC cards up to 2 TB working ([SDW], [SDM]).
- ⚠ On FAT32 the firmware shows 0K free (the CLI's `DIR`, the file manager's F key): it checks FSInfo's signatures (C324h, page 1) and reads only the next-free hint at 1ECh (C29Fh, C2F8h); no page reads the free count at 1E8h, and a trace shows no FAT scan. Dynamic: the same on FatFs and mkfs.vfat volumes, 300 MiB SDSC and 3 GiB SDHC. FAT12/16 show the real figure.

**Card images are media**, like disks: a host file holding a whole card (MBR partitions or a super floppy; MinTVC's and [JTV]'s `.img`), opened read-write in place (`SdImageFile`; a read-only file sets socket 1's write-protect switch), served to the core through `SdCardImage`. They stay attached to the memory map when the cartridge goes in or out or the machine powers on; a copied machine has none, an assignment keeps its own. A new image, or none, is a card going in or out: that card powers up from nothing and the change latch is set. Snapshots (`SDDC`: flash, RAM, registers, both cards' states) name the images by path in `MEDI`; rewind never undoes card writes.

**Images** (`write_sd_image`, on the vendored FatFs): a new file of the size given (64 MiB by default) with one FAT partition behind an MBR (FatFs chooses FAT16 up to 2 GiB and FAT32 above), host files and folders copied into its root; long names get FatFs's 8.3 aliases, which is what the firmware shows ("8.3 short names", [SDW]). `sd_image_listing` lists one. The FatFs disk interface (`FatDisk`, `fat_format`, `fat_disk_volume` in `card256_storage.hpp`) is shared with the TVC256k++'s DSK code.

**Not emulated**: the single-socket SD cartridges (`cart_eprom/vtdos_cr_*.bin`, 32 KiB; platforms 2.0 sd_cr_v1 and 2.1 sd_cr_v2 in `ROM_VER.TXT`) boot on this board but never set +1's b7/b6 (only 00h is written there), so their card select is wired another way ⚑ and they see no card; the 2009 slot-card SD interface (`if_eprom/`, I/O ports); the CPLD's real SPI clock. The SD Dual runs newer firmware with the same functions.

### ROM

The firmware 2022.11.09 (`sd_dual_2022_11_09.zip`, member `sd_dual_2022_11_09/vtdos_cr.bin`, 40 KiB, CRC-32 F758C797) as `sddual.rom`, and its "gamer mode" build (`…_gm`, RESET starts the file manager, CRC-32 2CC250AF) as `sddual-gm.rom`, through the ROM search and the autofetch from the Internet Archive's copy of the author's site (pinned SHA-256 of the ZIPs and members). The ZIPs state no licence and the firmware contains Videoton's VT-DOS: both files are fetched by the autofetch, not in the repository. Detected anywhere by `MOPS`, `XOR A`, `LD (FC02h),A` at its start and at most 64 KiB; the flash past the image reads FFh. Its build date is at C020h (`cart` shows it).

### Front ends

| Front end | SD Dual |
|---|---|
| headless | `--sddual`, `--sddual-gm`, `--sddual-rom FILE`, `--sddual-flash-write`; `--sd FILE` (microSD socket) and `--sd2 FILE` (SD socket), which bring the cartridge unless `--cart` or `--snapshot` does; `--cart FILE` with the firmware; `--sd-build OUT` with `--sd-add PATH` (repeatable) and `--sd-size MIB`, then exit (listed); `--sd-list FILE` |
| GUI | the same options; Cards menu, under Cartridge: **SD Dual cartridge** (in or out, powers on), with it in **microSD card image...**, **SD card image...** and their "Take ... out" (no power-on), **New SD card image...** (files, then where the 64 MiB image goes; it goes into the microSD socket, bringing the cartridge); dropping an `.img` larger than 720 KiB puts it in the microSD socket |
| monitor | `sddual [on [gm] \| off \| insert 0\|1 "file" \| eject 0\|1 \| build "out" mib "path"...]`; no argument: the cartridge and both sockets |
| MCP | `media` `sddual_on` / `sddual_off` (`firmware` plain or gamer), `sd_insert` / `sd_eject` (`socket`, `path`), `sd_build` (`path`, `size_mib`, `files`); `status` reports it |

Cartridges are not saved in `freetvc.ini`, so neither is the SD Dual nor its cards. In BASIC, `EXT 2` starts the VT-DOS CLI and `EXT 3` the file manager; LOAD and SAVE use the card; the CLI's `DOS` (or V in the file manager) boots VT-DOS from the card when `BDOS.SYS` and `COMMAND.COM` are in its root ([SDW]'s `system_f_v2.zip`, not fetched).

### Tests and checks

- `core.sddual` (no system ROMs): CRC7 of CMD0/CMD8 (95h, 87h) and CRC16 of 512 × FFh (7FA1h); SDSC: SD mode before CMD0 and a bad-CRC CMD0 ignored, idle-state illegal commands, R7, ACMD41 idle then ready, OCR, CSD 1.0 capacity and CRC7, CID, byte-addressed single, partial (CMD16) and multiple reads with CRC16 and CMD12, misaligned and out-of-range reads, single and multiple block writes with data responses and busy, a read-only image's write error; SDHC: never ready without HCS, ready with HCS on the first call only, CCS, CSD 2.0 capacity, block addresses, 512 bytes after CMD16; the board: pages by b7–5, RAM, peek, status bits per socket, register mirrors, the change latch, SPI to the selected card only, fast mode, MISO high with nothing selected, the flash with and without the jumper (program, DQ7, a 64 KiB sector erase with DQ6 toggling, program after it); in a `Machine`: the board, CART at C000h, the `SDDC` chunk, a copy without images, a load keeping them; the builder: listing, MBR, size, no temporary file left, a too-small image failing cleanly.
- `sddual.rom.<preset>` (64k+-2.2, 64k-1.2; cache variables `FREETVC_SDDUAL_ROM`, default `sddual.rom` in the ROM folder, and `FREETVC_SDDUAL_FILES`, default `testdata/sdinterface`, the site's unpacked ZIPs, optional): SAVE from BASIC writing `HELLO.CAS` into the FAT16 volume (seen by FatFs), LOAD and RUN printing 42 after a power-on, `EXT 2` and `DIR` on A: (both files), B: an SDHC FAT32 3 GiB card ("1 file", "0K free"); with [SDW]'s files present: `SDFLASH.CAS` without the jumper ("Hiba", flash unchanged) and with it (the gamer build flashed and verified, pages 5–7 given the `MOPS` stub), and VT-DOS 2.0's `COMMAND.COM` booted from the card (in RAM, its banner shown). The MinGW builds of `sddual_test` and `sddual_rom_test` (64k+-2.2, with the updater and the system files) pass under Wine.
- Both autofetch paths fetch the two ZIPs from the Internet Archive with the pinned hashes.
- Checked by hand, headless: the start-up logo, `EXT 3`'s file manager listing a FatFs card (folders, sizes), TVBALL started from it, both sockets in the DOS file manager (drives A: and B:), `AUTOEXEC.BAT` running (date and time prompts, its banner), the gamer build, SYS 1.2; a TVCS snapshot taken in the file manager resumed from the snapshot alone (the cards by path; a folder opened afterwards).
- [SDW]'s streaming demos (`Demok/demo1-4.zip`) from a card made by freeTVC's builder, started from the file manager: demos 1–3 show their `.kep` picture and stream `.rw4` 4-bit sound, paced by the TVC's sound interrupt at the pitch in the file's header (the player's source `Source/source.zip` `demo/demo.asm` sets the interrupt; the SD reads only refill its buffer); demo 1 (pitch 0FE7h, 7812 samples/s by that source) plays for 156 s, the 157 s its 613 548 bytes hold, then its end prompt; demo 3 first asks for the rear colour switch on black and white (its picture is made for it); demo 4 streams `.vid` video (frames 1100, 1150, 1200 and 1400 all differ). Demo 5 (12 MB) was not run.
- Checked by hand, GUI: `--sd` and `EXT 3` typed with synthetic keys, the Cards menu with the SD Dual items, **SD Dual cartridge** from the menu, **microSD card image...** and **New SD card image...** through the file dialogs (the image built, inserted and listed).

### Known limits

- Not checked: real hardware; SDXC sizes above 32 GiB; MMC cards (CMD1 path; freeTVC's card answers as an SD card); dropping an `.img` on the GUI window.
- Open ❓: the CPLD's unread status bits and +2/+3 reads; the flash part, its sectors and times; the HCS rule (a real SDHC card's behaviour); a real SPI clock (transfers are instant). FAT32 free space shows 0K, as the firmware computes it (⚠ above).

## SID card

No real card's documentation is known: the English Wikipedia says a SID card "was created" for the TVC, with no source, schematic, ports or software, and no known program writes the ports below. WinTVC 1.4.1's SID device is the only definition, so freeTVC follows its layout, with a real chip's reads. `src/core/cards/sid_card.{hpp,cpp}` on the vendored reSID; a `Machine` member, not a slot card.

**WinTVC 1.4.1 (observed under Wine):** ports E0h–F8h are SID registers, register = port − E0h; the chip is a MOS 6581 at 985 248 Hz (a PAL C64's clock), through `winresid.dll`; reads of E0h–FFh return FFh (its OSC3 and ENV3 reads gave FFh in a probe run); the SID is reset with the machine ⚑ (not checked in a run). winresid (https://github.com/calmopyrin/winresid, GPL-2.0, read only; the same reSID code one commit older than freeTVC's) clocks the chip only when the audio buffer is filled, per 48 kHz sample, and applies writes at once, so in WinTVC a write lands at the start of the buffer being computed ⚑ (static reading of winresid's source). No WinTVC code was used.

- **Ports**: E0h–FFh, decoded outside the slots (A7–A5 high; A4–A0 reach the chip). Writes to E0h–F8h are registers 00h–18h, as in WinTVC. F9h–FCh read POTX, POTY (no paddles: FFh), OSC3 and ENV3; every other read gives the chip's data bus, the last value written or read, which fades to 0 after 1D00h clocks on a 6581 and A2000h on an 8580 (reSID's bus TTL). ⚠ WinTVC reads FFh at all of them; a chip on a real bus would drive them ❓. Without the card E0h–FFh read FFh and writes go nowhere.
- No slot, ID, ROM or interrupt, so the strip's slot ribs do not show it; it combines with every card and cartridge.
- **Chip**: reSID (Dag Lem, https://github.com/daglem/reSID commit `ef7873f`, GPL-2.0-or-later, `third_party/resid/`, patched as below), a MOS 6581 (default, WinTVC's model) or 8580; clock 985 248 Hz unless `--sid-clock` gives another ❓. reSID's external filter (the C64's output stage: 16 Hz high-pass, 16 kHz low-pass) stays in ❓. Filter bias 0 mV (`adjust_filter_bias(0.0)`, VICE's and sidplayfp's default): reSID leaves it uninitialised otherwise, which made machine copies differ in the MinGW build. winresid never calls `adjust_filter_bias()` either, so WinTVC's 6581 filter cut-off depends on uninitialised memory ⚑; in the comparison run below it behaved as 0.
- **Timing**: an access clocks the chip to its own machine tick (`ChipClock`), so a write lands within one chip clock of its OUT. Between accesses reSID runs in steps of 4 chip clocks and its output is taken after each (about 246 kHz) as a step into the band-limited synthesis. The steps depend only on that grid and the accesses, not on how runs are sliced (tested).
- **8580 writes**: reSID's non-cycle-exact mode holds an 8580 write for one clock (its "fake pipeline", kept); a second write in the same chip clock, which only the host can make (an OUT takes 11 T), first clocks the chip once so the held one lands.
- **Reset**: the reset button resets the chip (registers 0, voices released), as WinTVC does. Power-on builds a new chip.
- **Level** ❓: reSID's 16-bit output, ±32 768 = ±1.06 at the synth's input: the TVC tone's loudest square (0.75) plus 3 dB (level set by listening tests, after the WinTVC comparison below). On the bus SOUND line. The probe's resonant three-voice chord at volume 15 peaks at 32 571 of 32 767 in the 16-bit WAV, so a full-volume SID with the TVC tone on top can reach the output's clipping.
- **State**: chunk `SIDC` (model, clock, then the chip): reSID's `SID::State` completed by freeTVC's patch (waveform pipelines, filter integrators, external filter), so snapshots and machine copies continue sample for sample. A snapshot needs the same model and clock (`snapshot_needs`).

reSID findings (upstream `ef7873f`; patched where it mattered, every change listed in `THIRD_PARTY.md`): `Filter::Vw_bias` is set only by `adjust_filter_bias()` (`filter.cc` 432, used at 523), which neither the library nor winresid calls; `read_state()` changed the data bus through `read()`; `write_state()` lost all but one register on an 8580 through `write()`'s fake pipeline; C++20 rejects `DAC<bits>()` as a constructor name.

### Front ends

| Front end | SID card |
|---|---|
| headless | `--sid-card [6581\|8580]` (default 6581), `--sid-clock HZ` (100 000–4 000 000) |
| GUI | the same options; **SID card, MOS 6581** and **SID card, MOS 8580** in the Cards menu under the Multichip card (one at a time; choosing the checked one takes it out; powers on); the settings window's Machine section picks the chip; `sid_card = off\|6581\|8580` in `freetvc.ini` |
| monitor | `sid [6581 \| 8580 \| off]`; no argument: model, ports, clock, volume, filter registers, gated voices |
| MCP | `media` action `sid_card` with `model` 6581, 8580 or off; `status` reports `sid_card` |

Test program: `tools/sound/probes/sid.asm` (interrupts off, no ROM calls: a plucked sawtooth A4, pulse C5, triangle E5, noise, a three-voice chord under a resonant low-pass sweep, ring modulation, hard sync, then OSC3 and ENV3 into `RES`). `tools/sound/sid_check.py` runs it headless and measures the WAV ([tools/sound/README.md](../../tools/sound/README.md)).

### Tests and checks

- `core.sid_card` (no ROMs): no card → FFh and no effect; OSC3 follows voice 3's noise; a write-only register reads back the bus, faded 20 ms later; 1Fh reads the bus; ENV3 reaches FFh after the attack and 0 after the release; a 440 Hz sawtooth on both models (F = 7493) within 2 Hz at a sane level; a copied machine and a differently sliced run give identical samples; an 8580 snapshot taken right after a write restores OSC3, ENV3 and every sample, and saves again byte-identical; a machine without the card refuses the snapshot; reset silences the chip. The MinGW build of `sid_card_test` passes under Wine.
- `sid_check.py` (headless, 64k+ board without ROM use): both models play the probe; autocorrelation pitch of the three notes within 0.5 cents (zero crossings misread the 6581's sawtooth by 3 %; an FFT agreed with the autocorrelation, 440.05 Hz); ENV3 rises by about 26 per 4 ms (attack 4 = 38 ms); OSC3 gives noise. The MinGW build under Wine writes a byte-identical WAV.
- **Against WinTVC 1.4.1** (the same probe recorded in WinTVC under Wine, with its sound on): its OSC3/ENV3 reads give FFh; the probe's three notes are within 1.4 cents of their pitch (sawtooth 440.13 Hz, pulse 522.84 Hz, triangle 659.28 Hz); aligned at 10 ms, the loudness envelopes of the whole probe correlate at 0.933 with freeTVC's 6581, with a steady level ratio in every section, so the two differ by one scale factor; the resonant sweep's spectral centroid rises alike (WinTVC 959 → 5467 Hz, freeTVC 982 → 5107 Hz in 200 ms steps). Against each emulator's own TVC tone, WinTVC's SID was 6–7 dB louder than freeTVC's before freeTVC's +3 dB: WinTVC's own mixing, not a hardware fact.
- Checked by hand, GUI: `OUT 248,15` … `OUT 228,33` typed at the BASIC prompt reach the 6581 (the remote monitor's `sid`: volume 15, voice 1 gated); the Cards menu shows the chosen chip checked; choosing the 8580 there powers on with the notice, boots to the logo and prompt, and typed OUTs reach it; `sid_card = 8580` is saved. Monitor (`--moncommands`): `sid`, `sid 8580`, a bad model, `sid off`, `help sid`; MCP over stdio: `sid_card` 8580 and a bad model; `tools/mcp/inspector_check.sh` passes (strict schemas).
- Cost: 30 s of machine time takes 1.24 s headless with the card against 1.00 s without (the probe: 1.40 s against 1.03 s).

### Known limits

- No real hardware is known to compare with.
- Open ❓: the external filter; clipping when a loud SID and the TVC tone sound together; whether a real card drives reads and resets the chip; its clock.

## Serial card

Videoton's serial line interface (HBS, 49103; "soros vonali interfész", order number 210-07554-01-00A): an Intel 8251A USART with V.24 line drivers. The main board's Centronics printer port, which shares the front ends below, is described in [machine.md](machine.md) ("Printer port").

Sources: the card's user manual (`sorosvonaliif.pdf`) and warranty booklet (`jotallasi_fuzet.pdf`), and Csontos András' four redrawn schematics from three surviving boards (`tvcser_1.jpg` … `tvcser_4.jpg`, with `tvcser_4` the factory circuit reconstructed, drawn from altered boards and marked "not built, may contain errors" ❓), all on tvc.homeserver.hu; the OS manual ch. 14 and appendix 21 ([OS] in [tvc-hardware.md](../hardware/tvc-hardware.md)); Intel's 8251A data sheet (205222-002, November 1986); a disassembly of the EXT 2.x and EXT 1.x drivers (z80dasm). `src/core/cards/i8251.{hpp,cpp}` (the chip) and `serial_card.{hpp,cpp}` (the board), `CardKind::Serial`, default **slot 1**, else the first free slot.

- **Board** (`tvcser_4`): an 8251A-P5, its /CS on the slot's /SLOTX, so it answers on all 16 ports, A0 to C/D: even ports data, odd ports command and status ([OS]: X0h/X1h). TxC and RxC both on the bus `TXRXCLK`; its CLK on A41 (drawn "!BC" ❓), which only paces the chip's internals. RESET on the bus RESET line, so the reset button resets the chip. IDA and IDB grounded through jumper TB1 (ID 00, "RS232"). TxRDY, RxRDY and SYNDET are not wired to the bus: no interrupt (the OS's serial IRQ function 60h is `RET`, EXT E336h). Line drivers 75150/75154 (V.24). No ROM: the driver is in EXT.
- **Clock**: `TXRXCLK` is the sound generator's ÷2 stage, the chain's b0 ([tvc-hardware.md](../hardware/tvc-hardware.md) §6), so the baud rate is 97 656.25 / (4096 − P) at ×16. The card counts the chain's b0 rising edges (`Sound::txc_edges()`); a pitch change or `INSTART` mid-character changes the timing as on the hardware.
- **8251A** (freeTVC's own, from the data sheet):
  - after reset the next control write is the mode; a sync mode (b1–0 = 00) then takes one or two sync characters; then commands. Command b6 returns to the mode. The ROM's "00h, 00h, 00h, 40h" sequence works from any of these states.
  - async ×1, ×16, ×64; 5–8 data bits, parity, 1/1.5/2 stop bits. A character takes factor × (1 + bits + parity + stops) clocks; the receiver needs only one stop bit (data sheet), so a received character takes that many.
  - transmitter: a written byte waits in the buffer (status b0 TxRDY = buffer empty, not masked by TxEN or /CTS, data sheet note 1) and starts when TxEN and /CTS allow; TxEMPTY (b2) while nothing is buffered or shifting. A finished character goes to the line.
  - receiver: starts a character from the line only while RxE; at its end RxRDY (b1) is set, an unread character before it is lost and OE (b4) set. Parity and framing never fail on bytes from the host. ER (command b4) clears PE, OE and FE.
  - status b7 DSR, b6 SYNDET/BRKDET always 0. RTS (command b5) and DTR (b1) are outputs to the line; SBRK holds the line in break (bytes still go out after it ❓).
  - sync mode sends and receives nothing ❓: only the army's rebuilt boards used it, and their software is lost.
- **The line** (`SerialLine`, `core/cards/serial_card.hpp`, an attachment outside the machine state like the drives): `connected()` is the cable with a peer, which turns on /CTS and /DSR (the factory cable loops 106 and 109 at each end, and the peer's RTS comes in as DSR; freeTVC's peers are always ready to receive). Unplugged, both are off, so `SER_CHOUT` waits, as `PAR_CHOUT` does without a printer. Incoming bytes start only while the receiver is empty and, by default, while the TVC's RTS is on (the handshake cable: `SER_CHIN` raises RTS while it waits); `flow none` drops the RTS condition. Pacing to the empty receiver is the host peer's courtesy: a real peer could overrun the TVC. While the receiver listens the card looks at the line once a bit time (a card event), so a byte starts within one bit of arriving even when the CPU is not polling.
- **The ROM's driver** (EXT 2.x E2D2h–E3F4h; [OS] 14.3): `RS232_SET` sets the pitch from the BAUD table at E403h (0C88h … 0FFBh: 110, 150, 300, 600, 1200, 2400, 4800, 9600, 19 200 baud, the last 19 531), turns the tone on with amplitude 0, then for every slot with ID 00 writes 00h, 00h, 00h, 40h, the mode (FORMAT & B4h) | 4Ah and the command 05h. `SER_CHOUT` waits for status b7 and b0, `SER_CHIN` writes 25h (RTS on), polls b1 with STOP checked through port 58h, writes 05h, reads status and data, and turns error bits 38h into F4h/F3h/F2h. EXT 1.x's `RS232_SET` (F2A4h–F305h) leaves out the 05h command, as the card's manual and the OS manual say (they prescribe `OUT 17,5` once), but its `SER_CHOUT` sends 05h before every character (F347h) and `SER_CHIN` 25h (F37Ah), so BASIC's `#6` works on 1.2 without it ([rom.md](../hardware/rom.md), "Printer and RS-232 drivers"); only a program driving the chip itself needs the OUT. Nothing to emulate: the chip does what the ROM tells it.
- **State**: in the `CARD` chunk (mode, command, buffers, the characters in flight with their clock counts); `Sound::txc_edges()` is appended to `SND ` (older snapshots start it at 0).
- **Host side** (`src/frontend/common/serial_host.*`, `printer_host.*`): the line's peer is "off" (unplugged), "on" (the host: bytes queued by the user, a file sent in, a capture file), `listen[:PORT]` (127.0.0.1, default 6585, one client at a time; the cable counts as unplugged until one connects) or `connect:PORT` (127.0.0.1 only, like every freeTVC socket); a reader thread queues what the peer sends. The last 64 KiB the TVC sent are kept for the monitor and MCP. The printer appends raw bytes and/or UTF-8 text (CR or CR LF ends a line, 80h–98h become the accented letters, other control codes and ESC with the byte after it are left out ❓ for ESC sequences with parameters).

### Front ends

| Front end | Printer port | Serial card |
|---|---|---|
| headless | `--printer FILE` (raw, or text for `.txt`; twice for both; `on` = memory only) | `--serial-card [SLOT]` (default 1), `--serial off\|on\|listen[:PORT]\|connect:PORT`, `--serial-flow rts\|none`, `--serial-send FILE`, `--serial-capture FILE` (the last three bring the card and a host peer) |
| GUI | the same options; Cards menu "Printer port": "Printer to a file...", "No printer"; `printer = off\|on\|FILE` in `freetvc.ini` | the same options; Cards menu "Serial card (RS-232)" (slot 1, else the first free), with "Cable unplugged", "Cable to TCP port 6585", "Send a file...", "Capture to a file..." under it; `serial_card`, `serial_line` (a host peer is saved as off: its files are not), `serial_flow` in `freetvc.ini` |
| monitor | `printer [on \| off \| "file" ["file"] \| read]` | `serial [on [slot] \| off \| line off\|on\|listen[:port]\|connect:port \| flow rts\|none \| send "text" \| file "file" \| capture "file"\|none \| read]`; `send` takes `\r \n \t \xNN` |
| MCP | `media` actions `printer_on` (path, text_path), `printer_off`, `printer_read` | `serial_on` (slot), `serial_off`, `serial_line` (peer), `serial_flow` (flow), `serial_send` (text or hex), `serial_send_file`, `serial_capture`, `serial_read`; `status` reports `printer` and `serial` |

There is no printer by default, so an `LPRINT` waits after its first character until STOP (CTRL+ESC), as on the hardware.

### Tests and checks

- `core.serial` (no ROMs): the printer's flip-flops after power-on and reset, the latch cleared by reset, unplugged = ACK stays low, a printer takes the byte on the falling edge and is ready 10 µs later, a /STROBE left low is ended by /ACK, a snapshot between strobe and /ACK; port 5Ah ID 00; unplugged = DSR off and nothing leaves; at all nine ROM pitches a character takes 176 TXRXCLK clocks (within a clock); a second byte waits in the buffer (TxRDY/TxEMPTY), two take 352 clocks; 7 bits; receiving waits for RTS, takes 152 clocks, waits while a character is unread, no errors; flow none; the ROM's reset sequence from a sync mode; a machine copy and a snapshot taken mid-character send it at the same tick and stay byte-identical; reset. The MinGW build of `serial_test` passes under Wine.
- `serial.rom.<preset>` (32k-1.2, 64k-1.2, 64k+-2.2): `LLIST` and `LPRINT "ÁRVÍZTŰRŐ";42` reach the printer as the TVC's codes; unplugged, `LPRINT` waits in the ROM until CTRL+ESC gives STOP; `LIST #6` through the 8251A; `SAVE #6` captured, then `LOAD #6` and `RUN` on a second machine.
- Checked by hand, headless: the same on 2.2 with the text file (`HELLO ÁRVÍZTŰRŐ 42`); two freeTVC processes linked over TCP (`listen:6599` / `connect:6599`): one's `SAVE #6` became the other's program, which ran; `LIST #6` on SYS 1.2 with and without `OUT 33,5` (both work, see above). Monitor (`--moncommands`): every `printer` and `serial` form, a refused connect, a bad flow; MCP over stdio: `printer_on`/`printer_read`, `serial_on`, `serial_line on`, `serial_send` read by `GET #6`, `serial_read` with `LIST #6`, a bad hex, `status`; `tools/mcp/inspector_check.sh` passes.
- Checked by hand, GUI: `--printer x.txt --serial-card --serial listen:6597`: a typed `LPRINT "GUI"` lands in the text file, `LIST #6` reaches a TCP client, `GET #6` receives the client's byte; the Cards menu's serial switch (notice, lit rib, `serial_card = yes` saved), its cable item ("TCP port 6585, connected" with a client, `serial_line` saved); "Printer to a file..." through a save dialog, `LPRINT 123` in that file, the path saved and plugged in again at the next start; a cancelled dialog changes nothing.

### Known limits

- Not checked: real hardware, a real printer or a PC's serial port; WinTVC 1.4.1's printer output (`printer.txt`) has not been compared.
- Not supported: a host serial port (COM, `/dev/tty*`), Epson ESC/P rendering of printer output.
- Open ❓: the printer's /ACK delay (10 µs) and whether the board's /ACK input floats when unplugged; ESC sequences in the text output; the 8251A's SBRK and a transmitter disabled mid-buffer; the 8251A's CLK source on the card.

## Devices not emulated

- Original Videoton cards beyond the HBF and HBS: HBU parallel (49105), HBI-2 IEC-625 (49106), HBE EPROM programmer, TVC-NET ([tvc-hardware.md](../hardware/tvc-hardware.md) §9). The HBM 32 KiB RAM card (49102) is part of the 32k + HBM preset ([machine.md](machine.md)).
- SoundQuartett, the Hanák 6-channel SAA1099 modification, UPM on the disk card.
- YATE's SDCart (no hardware known, [SD Dual](#sd-dual)), the single-socket SD cartridges and the 2009 slot-card SD interface.
- The MultiCart's 512 KiB RAM fit-out and other flash chips.
