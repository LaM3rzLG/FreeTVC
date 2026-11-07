# Media: program loading, tape, cartridges, snapshots

How freeTVC gets programs and machine states in and out: the host cassette that serves the ROM's cassette calls from files, the fast autostart, the tape deck with the real tape signal, cartridge boards, the TVCS snapshot format with the WinTVC `.tvs` import, and rewind. Disk images belong to the disk card and are summarised at the end. The ROM facts this rests on are in [rom.md](../hardware/rom.md) §12, the file formats in [file-formats.md](../hardware/file-formats.md).

Evidence markers: ✓ verified, ❓ unknown or assumed, ⚑ needs a dynamic check, ⚠ a conflict between sources (the choice made is stated next to it).

## What the ROM does on `LOAD`

[rom.md](../hardware/rom.md) §12 has the steps with addresses. In short: SYS 2.2 `LOAD` opens the file, reads the 16-byte program header with CHIN and checks bytes 0/1 ("Bad file"). It then does a NEW (`BAS_TEXT` = `BAS_VLOMEM`), checks memory, reads the program with one BKIN to `BAS_TEXT`, and rebuilds the BASIC pointers (D289h). Finally it closes the file and runs the program or returns to READY. Every file access is an RST 30h cassette-class call.

## How other emulators load a CAS

| Emulator | Method |
|---|---|
| MAME | quickload: checks byte 00 = 11h, copies from 90h to 19EFh ([file-formats.md](../hardware/file-formats.md)) |
| ep128emu | a FILE I/O card ROM (`tvcfileio.s`) whose functions trap to the host; BASIC `LOAD` reads host files |
| WinTVC 1.4.1 | "Load CAS directly" (Alt+C; its mechanism was not analysed ❓) and a FILE I/O ROM with traps; drag-and-drop autostart |
| devTool | writes the assembled code straight into memory and starts it |

Copying the payload into RAM skips the ROM's NEW, memory check and pointer rebuild, so `BAS_TOP` and the variable chain stay stale until `RUN`. A program area that does not start at 19EFh breaks MAME's method. freeTVC instead lets the ROM's own `LOAD` run and serves its calls.

## Host cassette

`src/core/media/host_cassette.{hpp,cpp}`. The machine offers every instruction boundary at 0030h to an attached `OsCallHandler` (`src/core/machine/os_call.hpp`). The offer comes after the debugger's look; a run that resumes at 0030h after a debugger stop gets it at its start. `Media` (`src/core/media/media.{hpp,cpp}`) is that handler:
- It acts only when 0030h holds the OS's `JP INFUNC` (C3 23 0Bh, every SYS ROM). The function code is the byte at the return address.
- It routes the way the OS does ([rom.md](../hardware/rom.md) §4 step 5): a card byte (bit 7) keeps the whole class; functions 1–2 go to the cassette when the class's table byte is 05h (so `POKE 2818,5` sends editor input to it); functions 3–5 when the class is 5.
- With nothing attached and no folder, nothing is intercepted, and the ROM's tape routines run.

The host performs the call and returns past the function byte (`Machine::return_to`, which the debugger treats as an ordinary next instruction). It returns A and flags as the OS exit's `OR A` leaves them, and BC/DE per [rom.md](../hardware/rom.md) §12, with HL, IX, IY and the alternate set kept. Memory moves use the OS paging (port 02h = 70h during the call). The ROM workspace is kept as the ROM keeps it: the file types at 0BF3h/0D14h decide what is open, so a reset or the ROM's own close also closes the host's file. Names go to 0BF4h/0D15h, plus `BUFFER`, `PROTECT`, `EOF` and `MUDDLE`. An error return also runs the OS's error exit (the class's default assignment back, [rom.md](../hardware/rom.md) §12).

Served bytes: the file from offset 80h. Unbuffered: CHIN reads the 16 header bytes, one BKIN the rest. A BKIN asking for fewer bytes than the file holds succeeds (TVBALL.CAS and two other corpus files declare fewer bytes than they carry); asking for more returns E7h.

Sources (both are supported):
- **attached file** (`--cas`, monitor `attach`): `OPEN` with an empty name, or its own name, reads it. When a name is found nowhere else it answers that too (a one-program cassette).
- **folder**: `LOAD "NAME"` opens `NAME.CAS` (ASCII case ignored; a name with a dot is taken whole). A file that is not `.CAS` (such as the raw `.bin` data of the 2dfx Wolf3D) is read byte for byte from its start as a buffered file, as the [NanoSD](cards.md#nanosd) serves it. `--cas-dir DIR` makes it read/write; without it the attached file's own directory is read only, so multi-part programs find their next file.
- `SAVE` and `OPEN OUTPUT` write `NAME.CAS` with an exact header (`build_cas`: type, records = floor(size / 128), byte 04h = size mod 128). With no writable folder, CRTE returns E6h (protection) and the strip / stderr says why. An unbuffered header shorter than 16 bytes is padded with zeros (CAS cannot express it).

Choices where the ROM has no counterpart (❓ against a real tape):
- a name found nowhere returns ECh (end of tape); a real tape searches until STOP;
- the "Searching / Found / Reading" messages are not printed; the strip and stderr show the open instead;
- calls take no emulated time; CHIN leaves B unchanged (the ROM leaves the high byte of its count there).

CAS parsing (`src/core/media/cas.{hpp,cpp}`), as a survey of 112 CAS files requires: the file size decides the length. The record count, byte 01h and a disagreeing program-header length are reported as warnings, never trusted. Type 01h is buffered; 11h, and anything else (with a warning), unbuffered.

## Autostart

`Media::autostart` (cold start, then `LOAD`, then `RUN` when needed), driven by the OS calls it sees ([rom.md](../hardware/rom.md) §12 "Prompt and logo"):
1. Booting: at the first keyboard-class input (the logo's wait) it holds SPACE for 2 frames (up to three tries).
2. At the first editor input (A1h/A2h), the BASIC prompt: it types `LOAD` + RETURN.
3. At the cassette's CLOSE (D4h): success needs a block read in full first, since BASIC's error handler closes the file too. With header byte 84h ≠ 0 the ROM runs the program and the autostart is done; otherwise it types `RUN` at the next prompt.
4. Failure: the prompt comes back before the close, or the close comes without the block ("Bad file"), or 3000 frames pass (`kAutostartFrames`).
5. With the disk card in, a CAS or tape autostart first gives the cassette class back to the cassette, as `POKE 2821,5: POKE 2829,5` does; a disk image autostarts from A: (see "Disk images").

**Fast start:**
- Front ends run the autostart unpaced, with audio discarded and the screen blanked, so the logo never shows. The first frame shown is the running program. A cold autostart takes 300–390 frames of machine time.
- **Prompt state**: at the first prompt after a cold boot `Media` keeps a copy of the machine. Later autostarts on the same configuration (board, ROM images, cartridge, cards and their ROMs, colour switch, audio rate) restore it instead of booting: 50 frames, a few ms. The ROM is not patched (unlike WinTVC's `FastBoot`).
- The prompt state is also kept on disk, as a TVCS snapshot in the user data folder: `prompt/prompt-<board>-<SYS CRC>-<EXT CRC>-<cart CRC>-<cart type>-<cart bank>[-bw][-s<slot><card><ROM CRC>…].tvcs`. It is written after the autostart that captured it and read before the next one, so the first autostart of a new run is fast too (a headless autostart on a test machine: 0.37 s cold, then 0.10 s, with the same frame and screen).
- **Deterministic**: while busy, slices end at the first instruction boundary of each new frame (`RunLimits::to_next_frame`, `Media::run_slice`; the monitor does the same), and scripted keys are applied there. So the result does not depend on how a front end slices time, and the warm path ends in exactly the cold path's state (`media.rom.*` compares RAM, VRAM, registers and ticks).

## Front ends

| Front end | Options and commands |
|---|---|
| headless | `--cas FILE`, `--cas-dir DIR`, `--autostart FILE` (not with `--load`/`--pc`/`--port02`). After an autostart, `--frames`/`--tstates` budgets and `--key`/`--type` frame numbers count from the program's start; a failed autostart exits with 1 and the reason on stderr. Opens and saves are logged on stderr. |
| GUI | the same options; the main menu's **Program** section: "Load program..." (SDL3 file dialog, `*.cas` filter), "Run NAME again" (uses the prompt state), "Eject NAME"; a dropped file autostarts, with Ctrl it is only attached. Reset and power cycle cancel a running autostart. Notices show opens, saves and the result ([ui.md](ui.md)). |
| text monitor | `attach ["file"]` (no argument: show the media), `detach`, `autostart "file"` (leaves the monitor) |
| MCP | tool `media`: `attach`, `detach`, `status`, `autostart` (waits until the program runs, returns the result and the screen text, leaves the machine paused) |

The front-end side is `src/frontend/common/host_tape.{hpp,cpp}`: `HostTape` reads the files and owns the `Media`; `CasFolder` maps names to files. The monitor and MCP reach it through `MediaControl` (`src/core/media/media_control.hpp`), an interface with no file I/O in the core.

## Checks: host cassette and autostart

- `core.cas`: parsing and writing, warnings, name conversion.
- `media.rom.<preset>` (all five presets):
  - autostart without autorun (`RUN` typed) and with autorun;
  - the warm path ends in the cold path's state;
  - a refused header fails instead of hanging;
  - `SAVE "Copy"` / `NEW` / `LOAD "COPY"` / `LIST` through a folder, with the saved file byte-compared;
  - `OPEN "SRC":POKE 2818,5` merging a line, and the editor assignment back to 02h after end of file.
- Headless, a corpus of 115 CAS files (games and demos, YATE's ZEXDOC/ZEXALL ports, rtvc's TVBALL) on 64k+-2.2 and 64k-1.2: all 115 load and start on both. Screens of 26 were checked by eye: title screens, menus and pictures.
  - Two of them stop with "Not understood" after `RUN` on every ROM, and a third shows nothing in 300 frames. The first two start by poking BASIC's own pointers (`POKE 5927,116:POKE 5926,45:…`, `BAS_TOP`/`BAS_CHAIN`) to absolute values for one memory layout, so they need a loader that injects them, not a plain `LOAD`.
  - WinTVC 1.4.1 behaves the same: loaded with its "Load CAS directly" (Alt+C) on preset 5 with `RUN` typed, it prints "*** Not understood." for the first two and shows an empty screen for 19 s (60 M T) for the third (run under Wine).
- Headless typed BASIC: `SAVE`/`LOAD` round trip, buffered `PRINT #5` / `INPUT #5` (`DATA.CAS` type 01h, exact records), the OS book's merge example.
- Real GUI (XTEST clicks, window grabs):
  - `--autostart` of a game: blank screen at 0.8 s, the game running at 2.3 s, notice "running NAME";
  - menu → "Run NAME again": restarted within 0.45 s;
  - menu → "Load program..." opened the GTK "Open File" dialog with the `*.cas` filter, and the file chosen in it (typed with xdotool) autostarted ZEXDOC.
- Monitor script (`attach`, `autostart`) and MCP over stdio (`media autostart` → `done: true`).

Not checked: drag and drop (no XDND source to drive it), the Windows GUI, a real tape.

## Tape deck

The real tape signal, for loaders that do not go through the OS's cassette class, and for checking the ROM's own bit-level routines.

### Where the deck sits

`src/core/media/tape_deck.*`: a `TapePort` (`core/machine/tape_port.hpp`) the machine calls at the hardware's points, with the machine's half-T tick (`Machine::set_tape_port`):
- **input**: `IN` 59h b5 reads the signal level at the tape position ([tvc-hardware.md](../hardware/tvc-hardware.md) §8);
- **output**: every access (read or write) to 50h–57h toggles the flip-flop; while recording, each toggle is an edge at the tape position;
- **motor**: port 05h b6 or b7; the two sockets share their data lines, so either bit moves the one deck.

The tape moves only while PLAY (or RECORD) is down and a motor bit is on. The position advances lazily from tick differences, so it depends only on the machine's time, never on how a front end slices it.

A tape is a list of level runs in ticks (`TapeSignal`, `core/media/tape_signal.*`), whatever it came from. `Media` owns the deck, steps aside for it and autostarts from it (`autostart_tape`).

### What goes in and out

- **CAS played as a tape**: the bytes the ROM would write ([tvc-hardware.md](../hardware/tvc-hardware.md) §8), with the timing of the `cas2wav` tool from tvc.homeserver.hu (`/bin/util/cas2wav.zip`, read as documentation): 2 s of silence and 10 240 leader cycles before the header block, 1 s and 5 120 before a data block, one sync cycle, the bytes LSB first, 5 trailing leader cycles; one cycle per bit, high half first: "0" 552 µs, "1" 388 µs, leader 470 µs, sync 736 µs. MAME's CAS-to-tape code agrees on the silences, leaders, sync and trailing cycles.
- **TTP**: `TTP0` and then the ROM's byte stream without leader, sync or bit encoding (confirmed from `cas2ttp.pas` in tvc.homeserver.hu's `/bin/util/casttpwav.zip`, read as documentation). Its blocks are parsed and encoded like a CAS. `cas2ttp` writes 257 data bytes in a full sector (`for i:=0 to 256`), so a sector is taken as 256 or 257 bytes, whichever matches its CRC ([file-formats.md](../hardware/file-formats.md), "Tape signals").
- **WAV in**: PCM, 8 or 16 bit, any rate, the first channel; a comparator with hysteresis (±0.04 of full scale) around the file's mean level. Each edge is placed where the mean is crossed, interpolated between the two samples around it: whole-sample edges add up to half a sample of jitter (±11 µs at 44.1 kHz), enough to break the ROM's leader check on real recordings (Space Sapper, below).
- **WAV out**: recording; the edges are written as a square wave, 44 100 Hz, 16 bit, mono.
- `decode_tape` decodes a signal by the ROM's cycle classes (used by the tests).

### The ROM's reader

From EXT 2.x E965h–E9BDh and E7A3h–E83Fh (z80dasm disassembly, traced over MCP; details in [rom.md](../hardware/rom.md) §7): the reader does **not** count sound interrupts. Each measurement waits once for the sound IRQ (`EI`/`HALT` after the `IN A,(5Bh)` that restarted the generator at the end of the previous one), then counts a busy loop of 48 T (15.36 µs: `INC E`, `IN A,(59h)`, test) until the edge it waits for. A bit is a full cycle between two edges of the same direction; which direction is fixed per block, right after the sync, so a recording with unequal halves decodes only with one of the two pairings. The sound-IRQ model ([machine.md](machine.md#sound-6)) still matters: the IRQ wait is part of every measurement.

The sync search per block: 32 leader cycles give an average, then 1024 cycles in a row must each be within ±2 loop counts of it (else it starts over); then the generator is set faster (pitch E8h), 256 half-cycles give separate averages for the two kinds of half, and the first half 4 or more counts longer than its kind's average is taken as the sync; a half 3 or more counts shorter starts over.

### With the host cassette and autostart

- While a tape is in the deck, the host cassette serves nothing, so the ROM's real tape routines run.
- `.wav` and `.ttp` always go to the deck. A `.cas` keeps the host cassette, unless it is played as a tape on purpose (menu, `--autostart-tape`, `--tape`).
- Autostart from a tape types `LOAD`, presses PLAY and runs unpaced. A close of the cassette ends the load: a known autorun header (CAS, TTP) means running; otherwise a BASIC prompt gets `RUN`, and no prompt within 50 frames (`kTapeRunFrames`) means the program started itself.
- A load through the tape counts as done only if the deck passed the end of the program and, for a tape made from a CAS or TTP, the program's bytes are in RAM unchanged at the close: BASIC's error handler also closes the cassette, so a close alone does not mean success.
- From a CAS, the data block is cut to the program header's length (as MAME does): three corpus files carry more bytes after it, and the ROM reads only the announced length.
- **Fast tape**: the front ends run unpaced while the tape moves (GUI: on by default, a menu toggle; the screen is shown, the sound dropped).

### Controls

- GUI: the Tape menu (insert, run the tape, play, stop, rewind, record to a WAV, eject, fast tape); the strip shows "tape play/rec/stop m:ss", in the notice colour while the tape moves, and a click plays or stops ([ui.md](ui.md), "Strip menus"). "Load program..." and drag and drop also take `.wav` and `.ttp`.
- Headless and GUI: `--tape FILE` (in the deck, PLAY down), `--tape-record FILE`, `--autostart-tape FILE`; `--autostart` sends `.wav` and `.ttp` to the deck.
- Monitor `tape [insert|autostart|record "file" | play|stop|rewind|eject]` and MCP `media` (`tape_insert`, `tape_autostart`, `tape_record`, `tape_play`, `tape_stop`, `tape_rewind`, `tape_eject`).
- Putting a tape in detaches the host cassette's file; attaching a `.cas` to the host cassette ejects the tape. A recording is written to its WAV at stop, eject or exit.

### Checks: tape deck

- `core.tape`: encode/decode round trip, TTP (freeTVC's blocks and cas2ttp's 257-byte sectors), a damaged byte, WAV out and in at 44.1 and 22.05 kHz, interpolated edges (a 441 µs square wave sampled at 44.1 kHz keeps every cycle within 4 µs), the deck's motor, position, end of tape and recording.
- `media.tape.64k-1.2`, `media.tape.64k+-2.2`: the ROM's own LOAD through 59h (no autorun with RUN, autorun, three sectors in RAM), a flipped bit fails, `SAVE "REC"` recorded by the deck decodes to exactly the encoder's header and data blocks and loads again.
- Headless corpus sweep (`--autostart-tape`): all 115 CAS files load and start through the deck on 2.2 and on 1.2, as through the host cassette. TVBALL and the other files compared gave the same screen both ways (one shows a randomly chosen joke page either way).
- Real GUI (window grabs, XTEST keys): `--tape GAME.cas`, `LOAD` typed: "Searching / Reading: NAME", the ROM's border stripes, "tape play" in the notice colour at 1742 % speed with fast tape, "ok" after it; the Tape menu; `--tape-record rec.wav` and `SAVE`, the WAV written at exit, and loaded headless it prints 12345 after RUN.
- `tape_test.exe` (MinGW build) under Wine.
- Not checked: loaders that bypass the ROM other than the recordings below, a buffered file through the deck, the file dialogs for Insert and Record (opened by the same code as "Load program..."), the Windows GUI.

### Real recordings

From the tvc.homeserver.hu file archive (`/cas/`, its `dir_list.php` listing, 6 241 files): 10 WAV files (9 recordings; one Renegade is there twice; 8-bit and 16-bit, 44.1 kHz, 7 s to 6 min) and 4 TTP files. Headless `--autostart-tape`, 64k+ 2.2, window grabs read by eye:

| Recording | Result |
|---|---|
| `Crossfire.wav` (Zka, 0:43) | running: the title screen with its key help (also in the real GUI) |
| `PUCKMAN_TVC.wav` (Doberdo Brothers, 3:35) | its own loader shows the picture with border stripes, then the game runs its demo |
| `Renegade 64k+ tape.wav` (two copies), `RENEGADE.WAV` (5:23) | the loading picture, then the title, all three |
| `Flipper mono 2.wav` (16-bit, 3:54) | the pinball table |
| `kastely.wav`, `kisertet8.wav` | the castle picture of both |
| `trukkos.wav` (0:07) | a short BASIC program; ends at "ok" |
| `spacesapper.wav` (Molmix, 2:20) | **two of five files** ⚑ (see below). WinTVC 1.4.1 (with its disk card's cassette redirection undone by `POKE 2821,5: POKE 2829,5`) gets to the title picture and goes on reading |
| 4 Molmix `.ttp` files | refused: "CRC error in sector 0". Their header block has type 00h and a 17-byte name, and its CRC matches no coverage of the ROM's; WinTVC 1.4.1 stays at "Searching" with them too. The tool that made them is not known |

So the deck works with real recordings and with the loaders of Puckman, Renegade and Flipper, which bypass the OS.

#### Space Sapper ⚑

Five files: headers at 5.3, 17.5, 53.1, 70.2 and 98.7 s (the last three unnamed). The recording runs about 6 % fast (leader 441–454 µs), every sync is a long low half then a long high one (16 and 14 samples against the leader's 10), and only the low halves carry the bit (8 or 12 samples); the high halves stay at 9–11 whatever the bit, so the low-then-high pairing gives two clean cycle lengths and the other pairing four.

- **First cause, fixed by interpolated edges**: with whole-sample edges the leader cycles jump between 19 and 20 samples (430.8 and 453.5 µs). In loop counts that is 1.5 counts of jitter on top of the loop's own ±1, so about one leader cycle in 70 was off by 3 or more and the 1024-in-a-row check restarted all the way through file 2's header leader (traced at E7DBh: counts 16–17, 2–4 restarts every half second). With interpolated edges no leader cycle is more than 2 counts off (128 → 0 on that leader), and files 1 and 2 load (the Novotrade logo). The other nine WAV files load as before.
- **Left** ⚑: the high half just before every sync is 11–12 samples, about 3 counts over its kind's average in real time, against the sync threshold of 4. On file 3's data block (56.35 s) it reaches 4, the sync is taken one half early, the block is read with the wrong pairing (phase register C = 20h at E837h instead of 00h), and the program crashes into a striped screen. This is deterministic (24 runs with `LOAD` typed at frames 450–473 all fail the same way: the tape starts with the motor, so the timing moves with it). Re-renderings of the same edges at 441 kHz load all five files, so it is a matter of a microsecond or two.
- **Why WinTVC loads it**: WinTVC 1.4.1's tape input reads at most one WAV sample per `IN (59h)` and stalls while the CPU is halted (static analysis, confirmed by a run under Wine), so the ROM ends up counting samples, not time: the leader jitters by 1 count and the stretched half is +2, under both thresholds. A real TVC samples a live signal in real time, so freeTVC keeps real time. Whether a real TVC loads this recording is unknown.

## Cartridges

The cartridge sits in page 0 (02h page-0 field `01`) or page 3 (field `00`, the reset value); the ROM boots one that starts with `MOPS` at C004h ([tvc-hardware.md](../hardware/tvc-hardware.md) §3). The boards are `CartType` in `core/machine/config.hpp`, decoded by `MemoryMap` (`core/bus/`).

| Type | Image | Paging |
|---|---|---|
| plain | up to 16 KiB; a larger image is a home-built board with bank switches (`--cart-bank N`, the 16 KiB window) | none |
| KiloCart | up to 64 KiB, 4 pages | a **read** at offset 3FFCh–3FFFh selects page (offset − 3FFCh); the byte read comes from the new page (from YATE's `Expansions/KiloCart/TVCKiloCart.cs`, github.com/hightower70/YATE, read as documentation; ❓ for the hardware) |
| MegaCart | up to 1 MiB, 64 pages | a **write** at offset 3C00h–3FFFh selects page (value & 3Fh) (YATE's `Expansions/MegaCart/TVCMegaCart.cs`, read as documentation) |
| MultiCart v2 | a flash image (two 512 KiB chips) plus 128 KiB RAM | its own register and firmware ([cards.md](cards.md), "MultiCart v2") |
| SD Dual | its firmware in flash, up to 64 KiB, plus RAM and two SD sockets | its own registers ([cards.md](cards.md), "SD Dual") |

- Type rules when no `--cart-type` is given: a MultiCart v2 image is found by its firmware's header, SD Dual firmware by its header; otherwise by size: up to 16 KiB plain, up to 64 KiB KiloCart, larger MegaCart. The MultiCart's flash is written back to the image file; the SD Dual's SD cards are raw images written in place.
- Bytes past the image read FFh: the factory HBR board has 4 KiB EPROM sockets behind a 74LS138, and the dumps of 8 KiB carts (Tetris, the test cart as two 4 KiB EPROMs) and of the 4 KiB Debugger leave the rest FFh. A single-chip home-built board may mirror instead ❓.
- The page survives the reset button (no reset line on the cart ❓, as in YATE); power-on and inserting set page 0 (a plain board keeps its switches).
- Debugger reads and writes never page (`MemoryMap::read`/`write`); only CPU bus cycles do (`cpu_read`/`cpu_write`). The `CART` bank is the page shown; a page past the image has no bytes.
- Inserting or removing a cartridge powers the machine on; a preset change keeps it.
- Front ends: `--cart FILE` with `--cart-type plain|kilocart|megacart|multicart|sddual` and `--cart-bank N` (GUI and headless); the GUI's Cards menu, Cartridge section (insert, remove, "New MultiCart image...", the SD Dual) and drag and drop of `.crt`, `.rom`, `.bin`; monitor `cart ["file" [type [bank]] | remove]`; MCP `media` `cart_insert`/`cart_remove`.
- Not built: YATE's SDCart (no hardware is known). `sakk.crt` (32 KiB, tvc.homeserver.hu `/cas/odor/`) pages by snooping port 00h writes (its loader steps `OUT (00h)` until C000h reads `Z`); no such board type exists in freeTVC ❓.

### Checks: cartridges

- `core.bus`: plain windows, FFh past the image, bank switches, the KiloCart read register in page 3 and page 0, the MegaCart write register, page kept by reset and cleared by power-on, debugger accesses that do not page.
- Headless, 64k+ 2.2: Space Invaders (#49201), Tetris, Pack Man, Bridzs and Turbó Rudi from the tvc.homeserver.hu cartridge dumps (`/bin/rom/`) and Crossfire from its archive boot to their title or game (window grabs read by eye); the other dumps were not run one by one. Zozo's 32 compiled carts (archive `2019/Zozo/`) start on 2.2 after the logo key, except **Batman**, which stays inside its decompressor on 2.2 and runs on 1.2. WinTVC 1.4.1 does the same (the same striped border on 2.2, the title on 1.2, run under Wine), so it is the cartridge's behaviour.
- Real GUI (XTEST, window grabs): `--cart Tetris.crt` starts Tetris and the 5 key starts the game; menu → "Remove Tetris.crt" powers on into BASIC's logo; "Insert cartridge..." opens the GTK dialog and the file chosen there (Pack Man) boots.
- Not checked: a real MegaCart or KiloCart image (none found; the paging is tested with synthetic images), drag and drop.

## Snapshots

A TVCS file (`src/core/snapshot/`): `TVCS`, a 16-bit format version (1), then chunks of a 4-character tag, a 16-bit chunk version, a 32-bit length and the payload, all little-endian; large arrays are PackBits-packed. A reader skips chunks it does not know, and a missing optional chunk gets its default.

| Chunk | Holds |
|---|---|
| `CONF` | board, colour switch, audio rate, SYS and EXT CRC-32, cartridge type, bank and image |
| `SLOT` | each slot's card kind and EPROM image (embedded like a cartridge) |
| `CARD` | each card's state (registers, RAM, chips), in slot order |
| `CPU ` | every register with Q, the decoder's state (`z80.h`'s step, address, latch, opcode, prefix, pins, interrupt bits), the machine's tick counter and instruction-boundary state |
| `BUS ` | 02h, the video paging, 03h's slot, the cartridge page, the IRQ flip-flop, the keyboard matrix and row, 05h, the printer latch, the tape output, the slot bits |
| `LPT ` | the printer port's /STROBE and ACK flip-flops and the pending /ACK ([machine.md](machine.md#printer-port)) |
| `MCRT` | the MultiCart v2 board: register, RAM, the flash chips' command state (the flash contents are media) |
| `SDDC` | the SD Dual board: flash, RAM, registers, the two SD cards' protocol state (the card images are media) |
| `SIDC` | the SID card: model, clock and the chip's complete state |
| `RAM `, `VRAM` | 64 KiB each |
| `VID ` | the 6845's registers and counters, border, palette, mode, the beam, both framebuffers |
| `SND ` | the tone generator, the band-limited synthesis state, the TXRXCLK edge count |
| `MEDI` | (front end) the tape's path, state, motor and position; the cassette's and folder's paths; the cartridge's file name; the four disk images' paths; the two SD images' paths; the NanoSD's SD folder (while the card is in); the TVC256k++'s USB port and pendrive folder (while the card is in) |
| `GAME` | (front end) the program the snapshot belongs to, for game saves |

- **Byte-exact**: a snapshot taken mid-frame, written, read back and loaded into a freshly powered machine runs on to the same ticks, registers, RAM, VRAM, framebuffer and audio samples; saving right after a load gives the same bytes (`snapshot.rom.<preset>`, all five presets). About 26 KB at a BASIC prompt, 48 KB in the middle of a game.
- Loading follows the file's board, ROM set, cartridge and cards: the machine's own ROMs if their CRCs match, else every preset of that board from the ROM search folders ([ui.md](ui.md), "Settings file and ROM search"), by CRC-32. The tape comes back by path at its position (a tape being recorded comes back stopped); disks and SD images come back by path; files that are gone are left out with a message. The host cassette's open file is not kept: take snapshots outside a LOAD or SAVE through it.
- Media outside the machine state (disks, SD images, the MultiCart flash, the TVC256k++'s pendrive and flash drive, the NanoSD's SD folder) are not in the snapshot, so loading one does not undo their writes.
- Front ends: the GUI's game saves (F9, Shift+F9 and the strip's game save key; "Game saves" below), drag and drop of `.tvcs` and `.tvs`, `--snapshot FILE` (GUI and headless; budgets count from there), `--save-snapshot FILE` (headless, at the end), monitor `dump "file"` / `undump "file"` (VICE's names), MCP `snapshot` `save_file`/`load_file`, DZRP READ_STATE/WRITE_STATE (the TVCS bytes; DeZog gzips and stores them).
- The autostart prompt state on disk is a TVCS file too (see "Autostart").

### Game saves

`frontend/common/game_saves.*` and `HostTape`: snapshots kept per program, so a game can be continued later.

- **Where**: `<root>/<game>/<name>.tvcs`, each with a PNG of the TV area beside it. The GUI asks for the name (the game's name in the field) and asks before replacing a save of that name; MCP without a name numbers it, `<game> NNN.tvcs`, one past the highest number in the folder. The time shown in the lists is the file's own modification time. The root is `SaveState` in the user data folder, or the `save_dir` setting. The game folder name replaces `/\:*?"<>|` and control characters with `_`, drops leading and trailing dots and spaces, and appends `_` to Windows' reserved names (`CON`, `COM1`, …), so the same folder works on Linux, Windows and macOS.
- **The game**: the file name (without extension) of the program last started (autostart of a CAS, tape or disk), the disk last inserted or the cartridge last plugged in; after a load, the one the snapshot's `GAME` chunk names; else the NanoSD's or the pendrive's folder name while that card is in (the front end keeps a folder attached while its card is out); else `GameCard` with the GameCard in (its boot menu copies one of four games into RAM, which the card's state does not tell). A card going in or out, a power cycle or another preset forgets the program. A save can name another.
- **What is saved**: the whole snapshot (memory, board, ROM set by CRC, the cards in their slots with their EPROMs and state) with the media by path. Disk, SD, flash and folder contents are not copied: a game loaded into memory needs only the memory, and a game that reads its media later needs the same files.
- **Checking before a load**: `MediaControl::snapshot_check` lists every file and folder a snapshot names and whether it is there, without touching the machine. A load with moved paths (`snapshot_load_moved`) takes old path → new path pairs; a moved folder carries every file under it (the cassette and the pendrive of one game folder follow one new folder). What is still missing is left out with a message: an empty NanoSD socket, an unplugged pendrive, an ejected disk.
- **Loading** follows the save's board, ROM set and cards as any snapshot load does.
- Front ends: MCP `game_saves` (`list`, `save`, `check`, `load` with `moved`, `delete`, which only deletes `.tvcs` files under the root). GUI: F9 and a click on the strip's game save key (a small screen with an arrow, beside the machine key) stop the machine, dim the window and ask for the save's name in its middle (`frontend/sdl/save_prompt.*`); Shift+F9 loads the running game's newest save; the key's right-click menu has the last save, the running game's saves (newest first, with their times), every game's saves in a submenu, and the saves folder. The check before a load with its browse dialog is not built yet.

**Checks** (headless with MCP, 2026-11-07):
- Wolf256 from a pendrive folder on the TVC256k++, saved at the title screen. The folder was renamed, `check` reported the cassette and the pendrive missing, and `load` with the folder moved restored them from the new place: the same picture, PC and memory.
- The same save loaded into a 32k 1.2 machine without cards, with only the folder given as moved: the machine switched to the 64k+ with the TVC256k++, the pendrive and the cassette came from the new folder, and the picture was the same.
- NanoSD: with its folder renamed, a plain load left the socket empty with a message, and a load with the folder moved put the new folder in.
- A `quick.tvcs` saved before `GAME` and the new `MEDI` fields loads and runs on.

### WinTVC `.tvs` import

`src/core/snapshot/tvs_import.*`, from the layout in [file-formats.md](../hardware/file-formats.md#wintvc-tvs):

- the machine powers on with the board and ROMs chosen in freeTVC (a `.tvs` names neither), takes RAM, the ports from `IOUT` (paging, video paging, mode, palette, border, sound), R0–R15 from the `CRTC` section and the 6845 address, VRAM into the displayed bank and the one after it (64k+), and every register with IFF1/IFF2 and IM from the `CPU ` section; execution goes on at PC (a halted WinTVC CPU sits on its HALT, so it halts again);
- not carried over: the beam position (starts at a frame start; WinTVC saves at its own 20 ms boundary anyway), a pending IRQ (the next cursor or sound IRQ sets it again), `EVTS` (WinTVC's own event list, no machine state), the 4 bytes after `IOUT` ❓; cards, disks and cartridges (a `.tvs` stores none of them);
- checks: `core.bus` with a synthetic file; snapshots of probe programs saved in WinTVC import with every register intact (still in the probe's loop after 20 frames); a snapshot saved in WinTVC during the logo carries on in freeTVC to the BASIC 1.2 prompt, and `PRINT 6*7` typed there prints 42.

### Checks: snapshots

- `snapshot.rom.<preset>` (above); `core.bus`: a MegaCart snapshot brings back its image and page, another cartridge is refused; PackBits round trip; the card tests (`core.floppy`, `core.card256`, `core.multicart`, `core.sddual`, `core.sid_card` and the others; [cards.md](cards.md)) save and load their cards' state; `remote.dzrp`: READ_STATE returns TVCS bytes and WRITE_STATE brings back a changed register; `headless.snapshot_no_preset`: a snapshot saved with `--preset` loads in a run without one.
- Headless: Puckman's own tape loader snapshotted mid-load with `--save-snapshot`, resumed with `--snapshot` under `--preset 32k-1.2` (the snapshot switched to the 64k+ 2.2 board and ROMs by CRC and put the tape back at its position): the same frame number, registers and screenshot as an uninterrupted run.
- Real GUI: Tetris from the cartridge, F9 at score 0, 12 s of play (score 95), Shift+F9: the saved moment again.

## Rewind

- `HostTape` keeps a TVCS snapshot (with `MEDI`) once a second of machine time, the last 60 s (61 points, `kRewindSeconds`), taken after run slices through the monitor (`MediaControl::after_slice`, which also writes idle disks back), so the GUI, headless and MCP runs all feed it. About 17 KB a point headless and 55 KB in the GUI's runs (the framebuffers): 1–3.3 MB for a minute.
- Going back N seconds restores the newest point at least N seconds old, or the oldest one when less is kept, and drops the newer points: asking again goes further back. A point taken after a power-on or another load replaces the ring (the machine's clock went backwards).
- Front ends: hold **F10** (Play and Bench): the machine stands still and steps back one kept second every 200 ms; Bench's run bar "Back 5 s"; monitor `rewind [seconds]`; MCP `snapshot` `rewind` (seconds). Headless keeps points only with a monitor or a remote client.
- Not built: stepping back by instructions (it would need an input log and replay). The Bench's History panel shows the recent instruction history instead ([ui.md](ui.md)).
- Rewind does not undo writes to media outside the machine state (disks, SD images, flash).
- Checks: MCP stdio, BASIC typing: frame 1180, back 5 s to frame 900, back 1 s to 850, back 30 s to the oldest point (frame 0, power-on) with only 17 s kept. Real GUI: F10 held 1.5 s in a Tetris game stepped back 8 s (score 75 → 0); Bench "Back 5 s" twice went back to score 0 and then to before the game drew its screen.

## Disk images

Disk images belong to the HBF disk card ([cards.md](cards.md), "Disks" and "Host cassette and autostart with the card"); in short:

- Raw sector images (`.dsk`, `.img`), 360 KiB (80 × 1 × 9 × 512) or 720 KiB (80 × 2 × 9 × 512), geometry from the FAT12 boot sector or the size, in four drives A–D (`core/cards/floppy.*`).
- The disks are not machine state: `FloppyDrives` is attached to the machine (`Machine::set_floppy_drives`), and `HostTape` writes a written disk back to its file once the drive has been idle for half a second of machine time, on eject and on exit, through a `.tmp` file and a rename. A file that cannot be opened for writing is a write-protected disk.
- Snapshots keep the card's state in `CARD` and the drives' paths in `MEDI`.
- Disk autostart (`--autostart FILE.dsk`, GUI open and drop, monitor `disk autostart`, MCP `media autostart`): the disk goes into A: with the card, the machine boots, and `LOAD "NAME"` is typed with the first `.CAS` file of the root directory.
- Options: `--disk-card`, `--disk-rom 1.1|1.2|FILE`, `--disk [A-D:]FILE`; GUI Disk menu with fast disk; monitor `disk`; MCP `media` disk actions.
