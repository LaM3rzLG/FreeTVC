# freeTVC user guide

This guide covers running and using freeTVC: the window program `freetvc`, the command-line runner `freetvc-headless`, their options, files and keys, and the remote interfaces. Building from source is in [developer-guide.md](developer-guide.md); what is emulated and what is missing is in [status.md](status.md).

## 1. What freeTVC is

freeTVC emulates the Videoton TV Computer (TVC), a Hungarian Z80A home computer of the 1980s: the 32k, 64k and 64k+ boards with their ROM sets, cassette and tape, cartridges, and a set of expansion cards. Besides the window program it has a debugger (Bench), a VICE-style text monitor, an MCP server for AI agents, a DZRP server for DeZog and a GDB stub.

Two programs come out of one build:

| Program | Purpose |
|---|---|
| `freetvc` (`freetvc.exe`) | the window program: Play mode (the TVC's screen and a status strip) and Bench (the debugger) |
| `freetvc-headless` (`freetvc-headless.exe`) | runs the machine without a window for a fixed time or under a remote client; writes screenshots, sound, snapshots; for scripts, tests and agents |

Platforms: Linux and Windows (x86-64). The code also has macOS paths ❓ (not tested). The window program needs SDL3 (fetched by the build); its picture model needs OpenGL 3.3, otherwise it draws with SDL's renderer.

Version: `freetvc --version` and `freetvc-headless --version` print `freeTVC 1.0.1`.

## 2. Getting the ROMs

freeTVC contains no ROMs. It needs the TVC's system ROMs (SYS and EXT) for every preset, and each expansion card needs its own ROM file. Release packages carry them in `roms/` next to the programs: the system ROMs, Videoton's disk-card ROMs and the card firmware whose licence allows it (`roms/ROMS-README.txt` lists what is included). The rest, and every ROM for a build from source, can be downloaded (below).

### Folders and file names

The user data folder is where freeTVC keeps its own files:

| OS | User data folder |
|---|---|
| Linux | `$XDG_DATA_HOME/freeTVC/`, by default `~/.local/share/freeTVC/` |
| Windows | `%APPDATA%\freeTVC\` |
| macOS ❓ | `~/Library/Application Support/freeTVC/` |

The **ROM search** looks in these folders in this order; the first folder that has both of a preset's ROMs wins:

1. `--rom-dir DIR` on the command line;
2. the environment variable `FREETVC_ROM_DIR`;
3. `rom_dir` in `freetvc.ini` (a relative path is read against the folder of `freetvc.ini`; `freetvc-headless` reads this one key too);
4. `roms/` next to the program;
5. `roms/` in the user data folder.

In each folder a ROM is first looked up under its expected name. If that file is missing, every file in the folder that is no larger than the ROM is checked by CRC-32, in name order, so renamed dumps (from MAME or the original EPROM files) work under any name. When nothing is found, the error on stderr lists every place searched.

The CRC-32 column lists the images recognised under another name. `vtdos12.rom` and `card256-flash.bin` are found by name only (their CRC-32 is that of the downloaded file); the VT-DOS cartridges are inserted by path like any cartridge.

| File | Size | CRC-32 | Used for |
|---|---|---|---|
| `sys12.bin` | 16 KiB | 6DA8809A | SYS 1.2 (presets `32k-1.2`, `32k+hbm-1.2`, `64k-1.2`) |
| `sys21.bin` | 16 KiB | 53C8FF98 | SYS 2.1 (preset `64k+-2.1`) |
| `sys22.bin` | 16 KiB | 79FA818C | SYS 2.2 (preset `64k+-2.2`) |
| `ext1x.bin` | 4 KiB, or the 8 KiB socket image with FFh in the first 4 KiB | 59C6B9F9, 1CBBEAC6 | EXT 1.x (with SYS 1.2) |
| `ext2x.bin` | 8 KiB | 05E1C3A8 | EXT 2.x (with SYS 2.1 and 2.2) |
| `hbf.rom` | 16 KiB | AE34982B (VT-DOS 1.1), 1466AED4, B3A567AD (UPM) | the HBF disk card's EPROM (default) |
| `hbf12.rom` | 16 KiB | 2ACF8477, D839417A | the HBF disk card's EPROM with VT-DOS 1.2 |
| `vtdos11.rom`, `vtdos12.rom` | cartridge | 40DDC741, 2C10C420 | the VT-DOS 1.1 and 1.2 cartridges (inserted as a cartridge, see 4.4) |
| `gamecard.rom` | 64 KiB | F782DB9B | GameCard EPROM |
| `multichip.rom` | 32 KiB | 704D1C27 | Multichip sound card EEPROM |
| `nanosd.rom` | 64 KiB | AF3C8EB7 | NanoSD EEPROM |
| `multicart-en.rom`, `multicart-hu.rom`, `multicart-es.rom` | | 7C275018, 50B96903, FFB77D45 | MultiCart v2 firmware 2.1.14, used by the image builder |
| `sddual.rom`, `sddual-gm.rom` | up to 64 KiB | F758C797, 2CC250AF | SD Dual cartridge firmware (plain, gamer mode) |
| `card256.rom` | 12 KiB | F759A66F | TVC256k++ ROM |
| `card256-font.bin`, `card256-picture.bmp` | | 5B6A2B88, EA0740CD | TVC256k++ power-on font and picture (optional) |
| `card256-flash.bin` | | E0FFC216 | TVC256k++ flash drive image (optional) |

### Downloading them

freeTVC can download every file in the table from the original sources: tvc.homeserver.hu (the system ROMs as the original EPROM dumps, joined into the files above, and the Videoton card ROMs), the card authors' GitHub repositories, and the Internet Archive (the SD Dual firmware). The list with a pinned SHA-256 for every download is `data/roms.txt`, compiled into the programs. Each file is checked against its sum before it is written. The download uses the system's `curl` (on PATH; on Windows also `curl.exe` in the system folder, present on Windows 10 and later). A `ROMS-README.txt` with the files' origins and licences is written beside them. The ROMs are not covered by freeTVC's licence; keep them for your own use.

| Way | Where the files go |
|---|---|
| `freetvc-headless --fetch-roms` (then exits) | `--rom-dir DIR` if given, else `roms/` in the user data folder |
| `freetvc` with no ROMs found at start: a message box offers **Download** or **Quit** | `roms/` in the user data folder |
| `freetvc`: **Download ROMs** in the machine menu (the preset name in the strip) or in Settings → Machine | the folder in `rom_dir` (Settings → Machine → Choose folder...) if one is set, else `roms/` in the user data folder; cards that went in without their ROM take it then, and the machine powers on |
| at build time: `-DFREETVC_FETCH_ROMS=ON`, or `cmake -DFREETVC_ROM_OUT=<dir> -P cmake/FreeTVCRoms.cmake` | `roms/` next to the built programs, or `<dir>` ([build.md](design/build.md)) |

## 3. First start and Play mode

### Starting

```sh
freetvc                       # the last preset used, or 64k+-2.2
freetvc --preset 64k-1.2
freetvc --autostart game.cas  # load and run a program
```

The window opens at 1000 × 760 and can be resized; its title is `freeTVC · <preset>`. At start, at power on and when a card changes, a faint photo of the TVC 64k+ covers the TV picture until the machine draws (at least 2.5 s, at most 10 s); the machine runs underneath. Messages go to stderr in English: where the ROMs came from (`freetvc: ROMs from DIR (why)`), which renderer draws (`drawing with ...`), the servers' ports, and every notice the strip shows. After power-on the TVC shows its animated logo and waits for a key; any key leads to the BASIC prompt.

### Machine presets

| Preset | Board | ROMs | BASIC banner |
|---|---|---|---|
| `32k-1.2` | TVC 32k | `sys12.bin` + `ext1x.bin` | TV COMPUTER BASIC 1.2, 25871 bytes free |
| `32k+hbm-1.2` | TVC 32k with the HBM 32 KiB RAM card | `sys12.bin` + `ext1x.bin` | BASIC 1.2, 42255 bytes free |
| `64k-1.2` | TVC 64k | `sys12.bin` + `ext1x.bin` | BASIC 1.2, 42255 bytes free |
| `64k+-2.1` | TVC 64k+ (64 KiB video RAM) | `sys21.bin` + `ext2x.bin` | BASIC 2.1, 42155 bytes free |
| `64k+-2.2` (default) | TVC 64k+ | `sys22.bin` + `ext2x.bin` | BASIC 2.2, 42155 bytes free |

Switching the preset (the machine key's menu or Settings → Machine) powers the machine on with the new board and ROMs; the cartridge and the cards stay in, except the TVC256k++, which the 32k + HBM board cannot hold. The board details are in [tvc-hardware.md](hardware/tvc-hardware.md).

### The screen

Play mode shows the TVC's picture, centred, at the PAL aspect. By default it shows the **TV area**: 650 × 287 of the 800 × 314 framebuffer pixels, what a PAL set shows. **Full raster (debug)** (Settings → Picture, or `--full-raster`) shows every pixel of the beam path, sync and blanking included, with the TV area outlined. How the picture is drawn (sharp pixels or a TV model) is chosen with the picture files ([section 6.3](#63-picture)).

### The status strip

One strip of drawn keys runs along the bottom of the window, in Play and in Bench. A key with something in it is lit; an empty one is faded. Left to right:

| Key | Shows | Click |
|---|---|---|
| four slot ribs | lit for a slot with a card in it | opens the **Cards** menu |
| cartridge and **Cart** | the cartridge's name while one is in (SD Dual for the SD Dual) | opens the **Cartridge** menu |
| a small TVC | the machine; the tooltip names the preset | opens the **machine** menu |
| keyboard | the tooltip names the mapping | opens the **Keyboard** menu |
| speaker | crossed out in red while muted or without a sound device | mutes or unmutes |
| 5.25" floppy | faded without the disk card; in the notice colour with the drive letter while a drive works | opens the **Disk** menu; the tooltip lists the drives |
| cassette | faded without a tape; while the tape moves, the signal under the head as a small wave | opens the **Tape** menu |
| ▶ and the counter | ▶ in the notice colour while playing; grey without a tape | plays the tape |
| ■ | lit while the tape is playing or recording | stops it |
| ◀◀ | lit with a tape in | rewinds it |
| notice | the latest message: 6 s, errors 15 s; a long one is cut with "..." and shows whole on hover | |
| printer | faded without a printer; in the notice colour while it prints | opens the **Printer** menu |
| REC | a blinking dot and the time while recording | starts or stops a video; a right click opens its menu |
| camera | | takes a screenshot; a right click opens its menu |
| speed | `paused`, `running`, or the speed in percent of a real TVC | pauses or resumes |
| **free**TVC | | opens the **main** menu |

A menu opens upward from its key. Clicking another key while a menu is open switches to that menu; Escape or a click elsewhere closes it. While a menu is open the keys go to the menu and every held TVC key is released.

**Main menu** (the **free**TVC nameplate)

| Item | |
|---|---|
| Load program... | CAS, WAV, TTP, DSK, IMG; see [section 4](#4-loading-software). Cartridges go in from the Cart key. |
| Recent programs ▸ | the last ten programs loaded, newest first (kept in `freetvc.ini`, key `recent`); the tooltip shows the path |
| Run NAME again | autostarts the attached cassette again |
| Snapshot ▸ | Quick save (F9), Quick load (Shift+F9), Save snapshot..., Load snapshot... |
| Full screen (F11) | |
| Skin ▸ | every skin found, Reload skin |
| Language ▸ | English and every installed translation, under its own name |
| Download ROMs | see [section 2](#2-getting-the-roms) |
| Settings..., Bench (F12), About freeTVC..., Quit | |

**Machine menu** (the TVC key): under *Version* the four factory machines, TVC 32K, 64K and 64K+ with their BASIC (the 32K + HBM board is the *HBM 32 KiB RAM card* at the bottom of the Cards menu); Pause (the Pause key), Reset (the reset button: memory is kept), Power cycle (RAM cleared).

**Cards menu** (the slot ribs): *Slot 0* to *Slot 3*, each with the card in it. A slot opens the list of cards: Empty, HBF disk card (slot 0 only), GameCard, Multichip sound card, TVC256k++, 2dfx graphics card, NanoSD, Serial card. Choosing one puts it in that slot (taking out what was there, moving it from another slot) and powers the machine on. The NanoSD and the TVC256k++ open a submenu instead, so their storage goes in with them: *SD card folder...*, or *Pendrive folder...* and *Flash drive image...*, or *Insert it empty*. The card in a slot opens its own settings: the HBF card its disk system (VT-DOS 1.1, 1.2), the TVC256k++ its USB port and flash drive, the NanoSD its SD card, the serial card its cable and files.

**Cartridge menu** (Cart): Insert cartridge..., Remove NAME, New MultiCart image..., SD Dual cartridge with its two card sockets, New SD card image....

**Keyboard menu**: Symbolic, Positional, and Key layout ▸ with the layouts found and Edit the layout....

**Disk menu** (the floppy): *Load a program directly...*: a disk image goes into the first empty drive (A: when all are full; the disk card goes in if needed) and its first program starts; then for each drive A: to D: "insert disk..." or "eject NAME", and Fast disk.

**Tape menu** (the cassette): Insert tape..., Run the tape, Play, Stop, Rewind, Record to WAV..., Eject tape, Fast tape.

**Printer menu**: Printer to a file..., No printer ([section 5](#5-expansion-cards)).

**REC** saves an MP4 ([section 4.8](#48-video-recording)) named after the program and the time (`NAME YYYY-MM-DD hh-mm-ss.mp4`) in the Videos folder; its menu starts and stops, picks the folder (`video_dir`) and opens it. While recording, a red dot blinks in the picture's top right corner too (on the window only).

**Camera** saves a PNG of the TV area at the TV's 4:3 aspect (1534 × 1148) on the desktop, named like the video; its menu picks the folder (`screenshot_dir`) and opens it.

### Reserved keys

These host keys never reach the TVC:

| Key | Play | Bench |
|---|---|---|
| F9 | quick save to `quick.tvcs` in the user data folder | checkpoint at the Listing's cursor |
| Shift+F9 | quick load of `quick.tvcs` | |
| F10 (held) | rewind: the machine stands still and steps back one kept second every 200 ms | the same |
| F11 | full screen on or off | the same |
| F12 | Bench | back to Play |
| Pause | pause or resume (a pause is a monitor stop, the same state Bench and remote clients see) | the same |

F1–F8 go to the TVC. Bench adds F4–F8 and Shift+F6/F7 while the Screen panel does not have the keys ([section 7](#7-bench-the-debugger)).

### Keyboard

The TVC has a Hungarian keyboard: QWERTZ, with the letters Ö Ü Ó Ő Ú Ű É Á Í, and keys a PC lacks (@ ; < \ [ ] * ^ as separate keys), plus INS, ESC, DEL, RETURN, LOCK, SHIFT, CTRL and ALT. Its cursor keys are the built-in joystick 1. STOP is CTRL+ESC. The layout is drawn in [tvc-hardware.md](hardware/tvc-hardware.md) §7.

Two **mappings** decide how host keys become TVC keys (`--keymap symbolic|positional`, `keymap` in `freetvc.ini`, the keyboard key's menu, Settings → Keyboard and joysticks):

- **Symbolic** (default): a host key that types a character types the same character on the TVC, pressing the TVC's SHIFT as the TVC needs it. It follows the host's own keyboard layout. Letters keep the user's SHIFT, so the TVC's LOCK still works. Characters the TVC lacks are ignored. Keys that type no character (Enter, Esc, arrows, F1–F8, Ctrl, Alt, the keypad's joystick keys...) follow the key layout below; AltGr only composes characters.
- **Positional**: every host key presses the TVC key the key layout gives it, by position. Use it for games.

A **key layout** is a file `keymaps/<name>.keys` that binds host keys to TVC keys (`--keys NAME|FILE`, `keys` in `freetvc.ini`, Settings → Keyboard and joysticks). Two are shipped:

| Layout | Binds |
|---|---|
| `hungarian` (default) | the keys by their place on a Hungarian PC keyboard (the `0` left of `1` is TVC `0`, `0` is Ö, Y and Z as on QWERTZ) |
| `us` | digits and letters by their legend on a US keyboard; Í on `` ` ``, Ö on `-`, Ü on `=`, Ő on `[`, Ú on `]`, Ű on `\`, É on `;`, Á on `'`, Ó on Tab and on the ISO key left of Z |

Common to both: F1 @, F2 ;, F3 <, F4 \, F5 [, F6 ], F7 *, F8 ^; Enter and keypad Enter RETURN; Backspace DEL (SHIFT up); Delete DEL with SHIFT held; Esc ESC; Insert INS; Caps Lock LOCK; Shift SHIFT; left Ctrl CTRL; left Alt and AltGr ALT; the arrow keys joystick 1, right Ctrl its FIRE; keypad 8, 2, 4, 6 joystick 2, keypad 0 its FIRE.

Layout file format: `;` starts a comment; `name = NAME`; then `HOST = TVC [+shift|-shift]` or `HOST = none`. Host keys by stable names: `A`–`Z`, `0`–`9`, `GRAVE MINUS EQUALS LEFTBRACKET RIGHTBRACKET BACKSLASH NONUSHASH SEMICOLON APOSTROPHE COMMA PERIOD SLASH NONUSBACKSLASH`, `SPACE RETURN ESCAPE BACKSPACE TAB CAPSLOCK`, `F1`–`F8`, `PRINTSCREEN SCROLLLOCK INSERT DELETE HOME END PAGEUP PAGEDOWN UP DOWN LEFT RIGHT NUMLOCK`, `KP_0`–`KP_9 KP_DIVIDE KP_MULTIPLY KP_MINUS KP_PLUS KP_ENTER KP_PERIOD`, `LSHIFT RSHIFT LCTRL RCTRL LALT RALT LGUI RGUI MENU`. TVC keys by their legend (`SEMICOLON` for `;`) or by word: `RETURN DEL ESC INS SHIFT LOCK CTRL ALT SPACE`, `UP DOWN LEFT RIGHT FIRE ACCEL` (joystick 1), `JOY2_UP` ... `JOY4_FIRE`. `+shift` / `-shift` force the TVC's SHIFT down or up while the key is held. Layouts are searched in `keymaps/` in the user data folder first, then `keymaps/` next to the program.

**The keyboard editor**: Settings → Keyboard and joysticks → **Edit the layout...** opens a panel along the bottom with the TVC keyboard and joysticks 1 and 2. Under each TVC key it shows the host keys that press it. Click a TVC key, then press the host key for it; a right click clears a key; F9–F12 and Pause cannot be bound. Held host keys are drawn pressed. Each change is saved at once to `keymaps/<name>.keys` in the user data folder, which then shadows the shipped file; **Back to the shipped layout** removes that copy. While the editor has the keys, the TVC gets none. In symbolic mapping only the keys that type no character use the layout.

### Joysticks and gamepads

| Joystick | Keys | Gamepad |
|---|---|---|
| 1 (the TVC's cursor keys) | arrow keys, FIRE on right Ctrl | the first gamepad connected |
| 2 | keypad 8, 2, 4, 6, FIRE on keypad 0 | the second |
| 3, 4 (GameCard only) | none by default | the third and fourth |

On a gamepad the d-pad or the left stick (past half deflection) gives the directions, the south button (A on an Xbox pad) FIRE and the east button ACCEL. More than four gamepads are ignored.

### Mouse

The TVC256k++ card has a USB port that can hold a mouse (Cards → the TVC256k++'s slot → TVC256k++ → Mouse, `--card256-usb mouse`). Then a left click on the picture (in Play, no menu open) captures the host mouse and sends its motion and buttons to the card once a frame; **Ctrl+Alt** or leaving the window releases it. Keys still reach the TVC while it is captured.

### Sound

The TVC's sound and every sound card are mixed into one 48 kHz stream to the default sound device. With sound on, the emulation keeps time by the sound device; with no device (or `--no-audio`) it keeps time by the clock. The speaker key mutes; Settings → Sound sets the volume (0–100 %, `volume`) and whether sound is opened at the next start (`audio`).

### Full screen and speed

F11 or main menu → Full screen switches full screen; `fullscreen = yes` starts in it. The machine runs at real speed, except: an autostart runs at full speed with the screen blanked until the program starts; **Fast tape** (the cassette's menu, on by default) runs at full speed while the tape moves; **Fast disk** (the floppy's menu, on by default) runs at full speed while a drive works (the machine's own timing stays exact).

## 4. Loading software

### 4.1 CAS files: the host cassette

A CAS file is the TVC's cassette file format ([file-formats.md](hardware/file-formats.md)). freeTVC serves BASIC's `LOAD`, `SAVE` and the OS's cassette calls directly from files, without a tape signal (the **host cassette**):

- `--cas FILE` (or a drop with Ctrl held) attaches a file: `LOAD` with an empty name, or with its own name, reads it. Its folder is readable too (read-only), so a program in several parts finds its next file.
- `--cas-dir DIR` makes a folder the cassette, read and write: `LOAD "NAME"` reads `DIR/NAME.CAS` (case ignored), `SAVE "NAME"` writes it. Without a writable folder, `SAVE` fails with a protection error and the strip says why.
- A name found nowhere returns "end of tape" at once (a real tape would search until STOP). The ROM's "Searching / Found" messages are not printed; the strip shows the file instead.

### 4.2 Autostart

`--autostart FILE`, **Load program...**, and dropping a file on the window load and run it: a cold start, `LOAD` typed at the prompt, `RUN` when the program does not start itself. It runs at full speed with the screen blanked, so the first frame shown is the running program. After the first autostart freeTVC keeps the machine's state at the BASIC prompt (in `prompt/` in the user data folder), so later autostarts on the same configuration start in a few milliseconds. **Run NAME again** repeats it. Reset and power cycle cancel a running autostart.

| File | Autostart does |
|---|---|
| `.cas` | through the host cassette |
| `.wav`, `.ttp` | through the tape deck: `LOAD`, PLAY pressed |
| `.dsk`, `.img` (up to 720 KiB) | into the first empty drive (A: when all are full) with the disk card, then `LOAD` of the first `.CAS` file of its root from that drive |
| `.img` larger than 720 KiB (GUI open or drop) | into the SD Dual's microSD socket, with the cartridge |
| `.crt`, `.rom`, `.bin` (a drop; *Load program...* refuses them) | inserted as a cartridge, machine powered on |
| `.tvcs`, `.tvs` (GUI open or drop) | loaded as a snapshot |

With Ctrl held, a dropped file is only attached or inserted, not run. `--autostart-tape FILE` autostarts through the tape deck, a `.cas` included.

### 4.3 The tape deck

The deck plays a real tape signal to the ROM's own tape routines, for programs whose loaders read the tape themselves. The tape moves only while PLAY or RECORD is down and the TVC has the motor on.

| Input | |
|---|---|
| `.wav` | PCM, 8 or 16 bit, any rate, the first channel |
| `.ttp` | the ROM's byte stream (`TTP0` files) |
| `.cas` played as a tape | encoded with the standard cassette timing |

Recording (`--tape-record FILE`, the cassette's menu → Record to WAV...) writes a 44 100 Hz 16-bit mono WAV at stop, eject or exit. `--tape FILE` puts a tape in and presses PLAY. While a tape is in, the host cassette serves nothing; inserting a tape detaches the cassette's file, and attaching a `.cas` ejects the tape. **Run the tape** types `LOAD` and plays.

### 4.4 Cartridges

`--cart FILE`, Cart → Insert cartridge..., or a dropped `.crt`, `.rom` or `.bin`. Inserting or removing a cartridge powers the machine on; a preset change keeps it.

| Type | Image | Chosen when |
|---|---|---|
| `plain` | up to 16 KiB; a larger image is a board with bank switches, `--cart-bank N` (0–63) picks its 16 KiB window | up to 16 KiB |
| `kilocart` | up to 64 KiB, 4 pages | up to 64 KiB |
| `megacart` | up to 1 MiB, 64 pages | larger |
| `multicart` | MultiCart v2: flash with its own file-select menu, 128 KiB RAM; the flash is written back to the image file | found by its firmware's header |
| `sddual` | the SD Dual cartridge's firmware ([section 5](#5-expansion-cards)) | found by its firmware's header |

`--cart-type TYPE` overrides the rules. Bytes past the image read FFh.

**MultiCart images**: Cart → New MultiCart image... asks for `.cas` programs and `.crt` cartridges (in menu order), then for the image's name; the image goes in with the English firmware. `freetvc-headless --multicart-build OUT --multicart-add FILE ... [--multicart-lang en|hu|es]` builds one without inserting it and lists its contents. Both need `multicart-<lang>.rom`.

**VT-DOS**: the VT-DOS cartridges `vtdos11.rom` and `vtdos12.rom` go in like any cartridge, together with the disk card.

### 4.5 Disks and the disk card

Disks are raw 360 or 720 KiB images (`.dsk`, `.img`) in drives A: to D: of the HBF disk card. `--disk [A-D:]FILE` (repeatable; without a letter the next drive) or the floppy's menu inserts one; a disk going in while the card is out puts the card in and powers on. Written sectors go back to the file; a read-only file is a write-protected disk. With the card in, BASIC's `LOAD` and `SAVE` use drive A:, not the cassette; that is why the card is off by default. A CAS or tape autostart with the card in gives the cassette back for that load. See [section 5](#5-expansion-cards) for the card's ROMs.

### 4.6 Snapshots

A snapshot (`.tvcs`) holds the whole machine: CPU, RAM, video RAM, video and sound state, cards and cartridge, and the tape and cassette by path. It restores the exact moment.

| Way | |
|---|---|
| F9 / Shift+F9 (Play), main menu → Snapshot ▸ Quick save / Quick load | `quick.tvcs` in the user data folder |
| main menu → Snapshot ▸ Save snapshot... / Load snapshot..., a dropped file | any file |
| `--snapshot FILE` | start from it (GUI and headless) |
| `freetvc-headless --save-snapshot FILE` | write one at the end of the run |
| monitor `dump` / `undump`, MCP `snapshot`, DZRP READ/WRITE_STATE | |

Loading follows the snapshot's board and ROM set: the ROMs are found by CRC-32 in the ROM search folders, under any preset. Files that are gone are left out with a message. Take snapshots outside a `LOAD` or `SAVE` through the host cassette: its open file is not kept.

**WinTVC `.tvs` files** can be loaded too. They name no board or ROMs, so the machine powers on with the preset chosen in freeTVC and takes the RAM, the ports, the CRTC, the video RAM and every CPU register from the file. Not carried over: the beam position, a pending interrupt.

### 4.7 Rewind

The machine's state is kept once a second of machine time for the last 60 s. Holding F10 steps back one kept second every 200 ms; Bench's run bar has **Back 5 s**; the monitor has `rewind [seconds]` and MCP `snapshot` `rewind`. Asking again goes further back. `freetvc-headless` keeps these points only while a monitor or a remote client is running.

### 4.8 Video recording

The TV picture (the 650 × 287 TV area at the PAL aspect: every line 4 times, the pixels area-averaged across to 1534 × 1148, 4:3, at the TVC's 49.76 frames per second) and the sound go into an MP4 file (H.264 and AAC) that plays in browsers, QuickTime, VLC and on video sites. Only the running machine is recorded: no Bench panels, menus or picture effects. freeTVC hands the frames to an `ffmpeg` program: the release packages carry a small one in `ffmpeg/` next to freeTVC (GPL, with its sources named in `ffmpeg/README.txt`); without that folder freeTVC looks for an installed ffmpeg (on macOS also in Homebrew's folders), then on `PATH`. While recording, the frames go to `FILE.video.mp4` and the sound to `FILE.audio.raw` beside the output; at the end ffmpeg joins them into `FILE` and both are removed. If ffmpeg fails, its last messages are reported and its log stays in `FILE.ffmpeg.log`.

In the GUI the REC key starts and stops a recording ([section 3](#the-status-strip)). `freetvc-headless --video FILE` records the whole run; after `--autostart` it starts when the program runs.

## 5. Expansion cards

The TVC has four card slots (0–3), each with 16 I/O ports at 10h + 10h·n and an 8 KiB memory window at C000h–DFFFh. Putting a card in or taking it out powers the machine on. Two cards cannot share a slot. The GUI starts with the cards that were in when it closed, in the same slots (the `freetvc.ini` keys below). In the Cards menu each slot lists the cards; choosing one puts it into that slot ([section 3](#the-status-strip)). Settings → Machine has the same choice as slot buttons 0–3 beside each card. Details of each card are in [cards.md](design/cards.md).

| Card | Command line | `freetvc.ini` | GUI | ROM | Default slot |
|---|---|---|---|---|---|
| HBF disk card (Videoton), VT-DOS | `--disk-card`, `--disk-rom 1.1\|1.2\|FILE`, `--disk [A-D:]FILE` | `disk_card`, `disk_rom` | Cards → Slot 0 → HBF disk card; Settings → Machine | `hbf.rom` (VT-DOS 1.1, default) or `hbf12.rom` (VT-DOS 1.2), or a 16 KiB image (UPM, ...) | 0 (fixed) |
| GameCard (dikdom): SN76489 sound, joysticks 3 and 4, boot menu with games | `--gamecard [SLOT]`, `--gamecard-rom FILE` (64 KiB) | `gamecard` | Cards → Slot n → GameCard | `gamecard.rom`; without it no boot menu | 1 |
| Multichip sound card (Vass Sándor): SN76489, SAA1099, YM3812 (OPL2) | `--multichip [SLOT]`, `--multichip-rom FILE` (32 KiB) | `multichip` | Cards → Slot n → Multichip sound card | `multichip.rom`; without it the chips still play; its start-up screen plays a demo on each chip after SPACE | 2 |
| SID card: a MOS 6581 or 8580 on reSID, at ports E0h–FFh outside the slots (WinTVC's layout; no real card's documentation is known) | `--sid-card [6581\|8580]` (default 6581), `--sid-clock HZ` (100 000–4 000 000, default 985 248) | `sid_card` (`off`, `6581`, `8580`) | Settings → Machine: SID card, MOS 6581 / MOS 8580 | none | no slot |
| TVC256k++ (dikdom): 256 KiB fast RAM over U2/U3, 8 MiB paged RAM, overlay graphics and sprites, drives, mouse, vSID sound, `EXT n` command line (n = its slot) | `--card256 [SLOT]`, `--card256-rom FILE` (12 KiB), `--card256-usb DIR\|mouse\|none`, `--card256-flash FILE\|DIR` | `card256`, `card256_usb`, `card256_flash` | Cards → Slot n → TVC256k++, with Pendrive folder..., Mouse, USB port empty, Flash drive image... (none of these powers on) | `card256.rom`; optional `card256-font.bin`, `card256-picture.bmp` (power-on screen) and `card256-flash.bin` (drive 2) | 0, else the first free; not on `32k+hbm-1.2` |
| NanoSD (Vass Sándor): an SD card through an Arduino Nano; file-select menu at power-on (ESC skips it), `EXT 0` command line | `--nanosd [SLOT]`, `--nanosd-rom FILE` (64 KiB), `--nanosd-sd DIR` (a folder as the SD card; brings the card) | `nanosd`, `nanosd_sd`, `nanosd_menu`, `nanosd_sort` | Cards → Slot n → NanoSD, with SD card folder... and Take the SD card out | `nanosd.rom` | 3 |
| Serial card (Videoton, an 8251A): BASIC's device #6 (`LIST #6`, `SAVE #6`, `LOAD #6`) | `--serial-card [SLOT]`, `--serial PEER`, `--serial-flow rts\|none`, `--serial-send FILE`, `--serial-capture FILE` (each brings the card) | `serial_card`, `serial_line`, `serial_flow` | Cards → Slot n → Serial card, with Cable unplugged, Cable to TCP port 6585, Send a file..., Capture to a file... | none | 1, else (its GUI switch) the first free |
| SD Dual cartridge (Sebestyén Pál): two SD sockets, VT-DOS 1.2 and a file manager in flash; in BASIC `EXT 2` starts the VT-DOS command line, `EXT 3` the file manager | `--sddual`, `--sddual-gm`, `--sddual-rom FILE`, `--sddual-flash-write`, `--sd FILE` (microSD), `--sd2 FILE` (SD) | (not kept) | Cart → SD Dual cartridge, with its card images and Take ... out; New SD card image... | `sddual.rom` or `sddual-gm.rom` | the cartridge port |
| Printer on the main board's Centronics port | `--printer FILE\|on` | `printer` (`off`, `on`, a file) | the printer key: Printer to a file..., No printer | none | |

Notes:

- **Printer**: none is plugged in by default, so `LPRINT` waits after its first character until STOP (CTRL+ESC), as on the real machine. A file ending in `.txt` gets UTF-8 text, any other file the raw bytes, appended; give `--printer` twice for both; `on` keeps the output in memory only (for the monitor's `printer read` and MCP).
- **Serial line** (`--serial PEER`): `off` (default, the cable unplugged: sending waits until STOP), `on` (freeTVC itself is the peer: `--serial-send`, `--serial-capture`, the monitor and MCP), `listen[:PORT]` (a TCP client on 127.0.0.1 is the peer, default port 6585) or `connect:PORT`. Two freeTVC instances can talk: one with `--serial listen`, the other with `--serial connect:6585`. `--serial-flow rts` (default) sends to the TVC only while its RTS is on, as the factory cable does.
- **TVC256k++ drives**: `--card256-usb DIR` makes a folder the pendrive (drive 0, written at once); the USB port holds the pendrive or the mouse, not both; the flash drive (drive 2) is a read-only littlefs image or a folder packed into one.
- **NanoSD**: the SD card is a host folder, seen through 8.3 names; files are written at once. `SAVE` onto an existing name appends, as the card's firmware does. `nanosd_menu` (yes/no) and `nanosd_sort` (`none`, `name`, `size`) hold the Nano's EEPROM settings.
- **SD Dual cards** are raw whole-card images (`.img`, FAT12/16/32, MBR or a super floppy, up to 2 TiB), written in place; a read-only file sets the write-protect switch. `--sd` alone brings the cartridge unless `--cart` or `--snapshot` brings the machine. Cart → New SD card image... builds a 64 MiB card with one FAT16 partition from chosen files and puts it into the microSD socket; `freetvc-headless --sd-build OUT --sd-add PATH ... [--sd-size MIB]` builds one (default 64 MiB), `--sd-list FILE` lists one. `--sddual-flash-write` sets the flash write jumper, so its firmware updater can write.
- **Disk card EPROM**: VT-DOS 1.1 (`hbf.rom`) is the default; switching the EPROM in Settings → Machine while the card is in powers the machine on. Full VT-DOS also needs its cartridge (`vtdos11.rom` or `vtdos12.rom`); either works with either EPROM.
- A card whose ROM is missing: from the command line freeTVC exits with the places searched; from `freetvc.ini` the card is left out with a notice. **Download ROMs** fetches them all.

## 6. Settings

### 6.1 The settings window

Main menu → **Settings...** opens a sheet at the right of the window (below Bench's run bar in Bench); the picture or Bench's panels make room for it, so a change shows at once. Escape or **Close** closes it; keys stay out of the TVC while it has them. A value decided on the command line shows "Set by --x for this run; a change here is saved".

| Section | Contents |
|---|---|
| Machine | presets; the ROM folder in use, Choose folder... (writes `rom_dir`; powers on only when the preset's ROMs differ), Default places (removes `rom_dir`), Download ROMs; the disk card and its EPROM (VT-DOS 1.1, VT-DOS 1.2, an image file); the other cards and the SID chip |
| Picture | the picture files and their sliders; the TVC's rear switch (Colour, Black and white); Full screen (F11), Start in full screen, Full raster (debug); what draws, and "Use SDL's renderer" from the next start |
| Sound | Mute, Volume, Sound output (from the next start) |
| Keyboard and joysticks | Symbolic or Positional, the key layout, Edit the layout..., the joystick keys |
| Interface | Language, Skin, Reload skin, the settings file's path and whether it is the portable one |
| Remote | the text monitor, the MCP server (HTTP) with its token and **Copy**, Allow monitor commands over MCP, DZRP for DeZog with TVC banks, the GDB stub: each switched on or off and given a port at once; the port each listens on or why it could not start |

### 6.2 freetvc.ini

**Where**: `freetvc.ini` next to the program if one exists there (**portable mode**: create an empty `freetvc.ini` beside `freetvc` to keep the settings with the program), else `freetvc.ini` in the user data folder. Bench's `layout.ini` lives beside it. In portable mode the quick snapshot, screenshots, prompt states, downloaded ROMs and edited skins, pictures and layouts still go to the user data folder.

**Format**: `key = value` lines; `;` starts a comment; a later line wins. Flags take `yes`, `true`, `1` or `no`, `false`, `0`. An unknown key or a bad value is reported (strip and stderr) and skipped; an unknown preset falls back to `64k+-2.2`.

**Writing**: the GUI reads the file before the command line, so its values are the defaults and options override them for one run. The GUI keeps the file in step with its state: checked once a second (every frame while the settings window is open) and on exit. A value given on the command line is written only if it is changed in the GUI during the run. Writing keeps the file's comments, order and other lines; missing keys are appended; an empty value removes a key. A new file starts with a comment naming the keys. `freetvc-headless` reads only `rom_dir`.

| Key | Values | Default | Notes |
|---|---|---|---|
| `rom_dir` | a folder | (none) | third place of the ROM search; relative to the file's folder |
| `preset` | a preset name | `64k+-2.2` | |
| `skin` | a skin name or a path | `bronze` | |
| `keymap` | `symbolic`, `positional` | `symbolic` | the mapping |
| `keys` | a layout name or a path | `hungarian` | the key layout |
| `language` | `en` or a translation's code (`hu`) | the OS's preferred language | written only after a choice in the menu or the settings window |
| `audio` | flag | yes | open the sound device at start |
| `volume` | 0–100 | 100 | |
| `fullscreen` | flag | no | start in full screen |
| `full_raster` | flag | no | read only, never written |
| `renderer` | `gl`, `sdl` | `gl` | |
| `picture` | a picture name or a path | `sharp` | |
| `colour_switch` | `colour`, `bw` | `colour` | the TVC's rear switch |
| `disk_card` | flag | no | |
| `disk_rom` | `1.1`, `1.2` or a file | `1.1` | |
| `gamecard` | flag or a slot 0–3 | no | `yes` = slot 1 |
| `multichip` | flag or a slot 0–3 | no | `yes` = slot 2 |
| `sid_card` | `off`, `6581`, `8580` | `off` | |
| `card256` | flag or a slot 0–3 | no | `yes` = slot 0, else the first free |
| `card256_usb` | `none`, `mouse` or a folder | `none` | |
| `card256_flash` | a littlefs image or a folder | (`card256-flash.bin` from the ROM search) | |
| `nanosd` | flag or a slot 0–3 | no | `yes` = slot 3 |
| `nanosd_sd` | `none` or a folder | `none` | |
| `nanosd_menu` | flag | yes | the Nano's EEPROM: file-select menu at power-on |
| `nanosd_sort` | `none`, `name`, `size` | `none` | the Nano's EEPROM: menu order |
| `printer` | `off`, `on` or a file | `off` | |
| `serial_card` | flag or a slot 0–3 | no | `yes` = slot 1 |
| `serial_line` | `off`, `on`, `listen[:PORT]`, `connect:PORT` | `off` | `on` is saved as `off` (its files are not saved) |
| `serial_flow` | `rts`, `none` | `rts` | |
| `monitor`, `mcp`, `dzrp`, `gdb` | `off`, `on` (default port) or a port 1–65535 | `off` | the remote servers at start |
| `mcp_allow_monitor` | flag | no | |
| `dzrp_banks` | flag | no | |

### 6.3 Picture

The GUI draws with OpenGL 3.3 core. Without a 3.3 context, or with `--renderer sdl` / `renderer = sdl`, it uses SDL's renderer, which shows sharp pixels only. Bench always shows sharp pixels.

A picture file (`--picture NAME|FILE`, `picture` in `freetvc.ini`, Settings → Picture) chooses how the TVC's signal reaches the screen. Shipped:

| Picture | Signal | Models |
|---|---|---|
| `sharp` (default) | sharp | the TVC's pixels scaled without filtering |
| `rgb-monitor` | rgb | a colour monitor on the TVC's RGB socket |
| `pal-tv` | pal | a colour TV on the RF/composite output: PAL encoding and decoding with its bandwidths, delay line, chroma trap |
| `bw-tv` | mono | a black and white TV: the colour subcarrier shows as fine dots in coloured areas |

The tube model adds beam width by brightness, an aperture grille or shadow mask, glow, curvature and vignette. Settings → Picture has sliders for the luma and chroma bandwidth, saturation, chroma phase error, the PAL delay line, spot width, beam on black and on white, mask type, strength and pitch, glow, curvature, vignette, tube gamma and brightness. They apply at once; when a slider is let go the whole picture is written to `pictures/<name>.picture` in the user data folder, which then shadows the shipped file; **Back to the shipped values** removes that copy. Picture files are searched in `pictures/` in the user data folder, then `pictures/` next to the program; the file in use is watched and reapplied when it changes on disk. `data/pictures/pal-tv.picture` lists every key with a comment. The model is described in [ui.md](design/ui.md).

The **rear switch** (`colour_switch`): on black and white, the TVC sends no colour subcarrier, so a TV shows grey. Programs can read the switch at port 59h.

Screenshots always hold the framebuffer's own pixels, whatever the picture.

### 6.4 Skins

A skin sets the window's colours, fonts and sizes. Shipped: `bronze` (default, the TVC case's colours) and `deck` (light: the putty of the letter keys with dark lettering). `--skin NAME|FILE`, `skin` in `freetvc.ini`, main menu → Skin, Settings → Interface. Skins are searched in `skins/` in the user data folder, then `skins/` next to the program; fonts next to the skin file, then in `fonts/` next to the program. A skin may set only some keys; the rest keep the `bronze` values. The skin in use is watched: saving it in an editor applies it at once; a bad line keeps the last good skin and shows the error in the strip. `data/skins/bronze.skin` lists every key with a comment.

### 6.5 Language

The GUI speaks English and Hungarian (Magyar). The language is `--language en|hu`, else `language` in `freetvc.ini`, else the first of the OS's preferred languages (on Linux `LANGUAGE`, `LC_*`, `LANG`) that has a translation, else English. Main menu → Language and Settings → Interface switch at once. Translations are gettext `.po` files in `lang/` next to the program; an unknown `--language` code exits with code 2.

Translated: menus, the strip and its notices, Bench, tooltips, file dialog filters, the no-ROMs message box. Always English: stderr, the text monitor and the Console's output, MCP, DZRP, GDB, `--help`, and the TVC firmware's own texts.

## 7. Bench: the debugger

F12 (or main menu → Bench) switches the window to Bench and back; it never changes the machine's state, so a running program keeps running. Every Bench action goes through the text monitor, so Bench, monitor clients, MCP, DeZog and GDB all see one stopped or running state. Panels dock and can be rearranged; the layout is saved in `layout.ini` beside `freetvc.ini`, and a missing or broken file gives the default arrangement. The status strip and its menus stay at the bottom.

**Keys in Bench**: the TVC gets keys only while the Screen panel has focus (its title then says "Screen: keys go to the TVC"); entering Bench gives it focus. Otherwise: **F5** run, **F6** step, **F7** step over, **F8** step out, **F4** run to the Listing's cursor, **F9** toggle a checkpoint at the cursor, **Shift+F6** / **Shift+F7** source line in / over (with a debug file loaded). F10, F11, F12 and Pause work as in Play.

**Run bar** (top): Run or Pause, Step, Over (CALL, RST, block repeats and HALT), Out, To cursor, Back 5 s, Reset, the stop reason ("stopped at CAA3: checkpoint #1") or the speed, and Play (back to Play mode).

**Page column** (left): the four 16 KiB pages and what is paged in each (SYS, CART, U0–U3, VID0–3, EXT, CARDn; page 3 split into its halves when it is). A click opens the Listing and Memory there.

| Panel | What it does |
|---|---|
| Screen | the picture (TV area or full raster); while stopped, the beam's line and column; markers at the beam position of each OUT, IN and interrupt of the frame (toggles above the picture), with their port, value and PC on hover |
| Listing | disassembly in sjasmplus syntax with labels and system-call names; PC triangle, checkpoint dots; click the gutter for an exec checkpoint; Go to (address or label), Follow PC; a share column while the profiler has data |
| CPU | all registers, flags, IM, IFF, HALT, beam, T-states; values changed since the last stop in yellow; double-click a value to edit it, click a flag to toggle it |
| Memory | hex and ASCII of the CPU view or one bank; Go to `[BANK:]address`; read, write and execute heat as fading tints; click a byte and type hex to edit RAM (ROM banks and cards are read-only here) |
| Machine | paging, video mode, border, palette, CRTC registers, sound generator, IRQ, the keyboard matrix with held keys lit |
| Console | the text monitor, typed into; it also logs every command Bench sent; Up/Down recall lines |
| Agent | the MCP server's last 256 tool calls: tool, arguments, result, time, frame |
| Checkpoints | every checkpoint, whoever set it; enable, condition, ignore count, hits, delete; an add form (exec, load, store, in, out, irq, line; "Log only") |
| Watch | expressions in the monitor's syntax (`HL`, `{SP}`, `[0003]`, `VID0:0000`, labels) shown as a value, bytes or text; changes in yellow; Stop on write; the list is kept in `layout.ini` |
| History | the last 65 536 instructions while stopped, with the registers each one changed; a filter; a click opens the Listing |
| I/O log | every OUT, IN and interrupt with frame, beam position, port, value, meaning and PC; filters, Follow, Clear |
| Video | a video RAM bank as a 512 × 256 picture in any mode and palette, the area the 6845 shows, the cursor and beam; hover for address and colour, click for Memory |
| Profiler | Record / Stop / Clear; T-states per routine (exclusive and inclusive) and the 500 busiest instructions |
| Source | the source file of a loaded debug file (sjasmplus SLD or listing, SDCC `.noi`/`.cdb`/`.map`, z88dk `.map`) with the PC's line; Load symbols..., Line in, Line over, Follow PC; gutter checkpoints |

The design and the meaning of each colour are in [ui.md](design/ui.md); the debugger underneath in [debug-and-remote.md](design/debug-and-remote.md).

## 8. Remote interfaces

All servers listen on 127.0.0.1 only. In the GUI they start from the command line, from `freetvc.ini` (`monitor`, `mcp`, `dzrp`, `gdb`) or from Settings → Remote; a server the command line asked for ends the program when it cannot listen, one from `freetvc.ini` shows its error in the settings window. They can all run together and share one stopped or running state.

| Server | Default port | GUI and headless options | Headless only |
|---|---|---|---|
| text monitor | 6580 | `--remote-monitor`, `--monitor-port N` (0 = any free port, printed on stderr), `--moncommands FILE` | `--monitor` (stdin/stdout) |
| MCP over HTTP | 6584 | `--mcp-http`, `--mcp-port N`, `--mcp-token T`, `--mcp-allow-monitor` | `--mcp-stdio` |
| DZRP (DeZog) | 6582 | `--dzrp`, `--dzrp-port N` (0 = any), `--dzrp-banks` | |
| GDB stub | 6583 | `--gdb`, `--gdb-port N` (0 = any) | |

### 8.1 Text monitor

A VICE-style command line. Connect with any TCP client:

```sh
freetvc --remote-monitor &
nc 127.0.0.1 6580
```

- Any line sent while the machine runs stops it first (as in VICE): the current instruction completes, a `(stopped by the monitor)` report follows, then the command runs. While the monitor holds the machine, output ends with the prompt `(C:$PC) `, and the GUI shows "paused". `x` or `g` lets it run.
- Several clients may connect: output goes to the client that sent the command, stop reports to all.
- Numbers are hexadecimal by default (`8000`, `$8000`, `0x8000`, `8000h`), `#` marks decimal, `radix d` switches. Checkpoint numbers are decimal.
- `BANK:offset` (`SYS:0405`, `U2:1000`, `VID1:0`, `CART:`, `EXT:`, `CARDn:`) names an offset in a bank wherever it is paged: memory commands read and write the bank, checkpoints fire wherever the bank is paged in.
- Quoted file names are taken raw (Windows backslashes stay); only `keybuf` text has escapes (`\n` and `\r` RETURN, `\t`, `\"`, `\\`, `\xNN`).
- `--moncommands FILE` runs the file's lines first, one after another (`;` starts a comment line); a run command lets the machine go and the script continues after the next stop; if the script never ran the machine, it runs when the script ends.
- `help` lists the commands, `help NAME` explains one. The text monitor is always present inside the GUI (Bench's Console), whether or not the TCP listener is on.

| Command | Does |
|---|---|
| `g [address]` (`goto`) | leave the monitor and run, from `address` if given |
| `x` (`exit`) | leave the monitor and run |
| `z [count]` (`step`) | step into instructions |
| `n [count]` (`next`) | step over CALL, RST (and RST 30h's function byte), block repeats and HALT |
| `lstep [count]`, `lnext [count]` | step into / over source lines (needs a debug file from `ll`) |
| `ret` (`return`) | run until a return leaves the current stack frame |
| `until address` (`un`) | run to an address |
| `reset [hard]` | the reset button; `hard`: power cycle with RAM cleared |
| `quit [code]` | quit the emulator with an exit code (0) |
| `r [reg=value, ...]` (`registers`) | show or set registers (A ... HL', IX, IY, SP, PC, WZ, I, R, IM, IFF1, IFF2) |
| `m [[BANK:]start [end]]` (`mem`) | hex dump |
| `d [start [end]]` (`disass`) | disassemble |
| `> [BANK:]address byte ...` | write bytes |
| `f start end byte ...` (`fill`), `h start end byte ...` (`hunt`), `t start end dest` (`transfer`), `c start end other` (`compare`) | fill, find, copy, compare memory |
| `bank` | what each page maps |
| `io` | video, CRTC, sound, IRQ state |
| `sw [reset]` (`stopwatch`) | T-states since the last reset |
| `chis [count\|on\|off]` | instruction history (last 16 by default; 65 536 kept) |
| `profile on\|off\|clear\|[count]` (`prof`) | time every instruction and routine; list the top routines |
| `snap save\|load\|delete NAME`, `snap list` | machine states kept in memory |
| `break [kind ...] [[BANK:]start [end]] [if cond]` (`bk`) | checkpoint on exec (default), load, store, in, out, irq or line (`break line #100`); no arguments: list |
| `watch [load\|store] [BANK:]start [end] [if cond]` (`w`) | stop on memory access |
| `trace [kind ...] [BANK:]start [end] [if cond]` (`tr`) | report accesses without stopping |
| `delete [n ...]` (`del`), `enable n` (`en`), `disable n` (`dis`) | manage checkpoints |
| `condition n [if] [expression]` (`cond`), `ignore n [count]` | a checkpoint's condition, hits to pass over |
| `ll "file"` | load labels and source lines: `name = value` / `EQU`, sjasmplus `--sym`, `--sld`, `--lst`, SDCC `.noi`, `.cdb`, `.map`, z88dk `.map` |
| `src [address]` | the source line of an address |
| `al address name`, `dl name`, `shl [filter]`, `cl` | add, delete, show, clear labels (the ROM symbols stay) |
| `bload "file" [BANK:]address` (`bl`), `bsave "file" [BANK:]start end` (`bs`) | load or save binary files |
| `scrsh "file.png" [full]` | PNG screenshot of the TV area or the full raster |
| `keybuf "text"` | type text while the machine runs |
| `attach ["file.cas"]`, `detach`, `autostart "file.cas"` | the host cassette; autostart leaves the monitor |
| `tape [insert\|autostart\|record "file" \| play\|stop\|rewind\|eject]` | the tape deck |
| `dump "file"`, `undump "file"` | save or load a TVCS snapshot |
| `rewind [seconds]` | go back in machine time (1 s) |
| `cart ["file" [auto\|plain\|kilocart\|megacart\|multicart [bank]] \| remove \| build "out" en\|hu\|es "file"...]` | cartridges and MultiCart images |
| `disk [insert "file" [A-D] \| eject [A-D] \| autostart "file" \| card on\|off]` | the disk card and drives |
| `gamecard [on [slot] \| off]`, `multichip [on [slot] \| off]`, `sid [6581\|8580\|off]` | sound cards |
| `nanosd [on [slot] \| off \| sd "folder"\|none]` | the NanoSD and its SD card |
| `printer [on \| off \| "file" ["file"] \| read]` | the printer; `read` shows what it printed since the last read |
| `serial [on [slot] \| off \| line ... \| flow rts\|none \| send "text" \| file "file" \| capture "file"\|none \| read]` | the serial card and its cable |
| `sddual [on [gm] \| off \| insert 0\|1 "file" \| eject 0\|1 \| build "out" mib "path"...]` | the SD Dual cartridge and its cards |
| `card256 [on [slot] \| off \| usb "folder"\|mouse\|none \| flash "file" \| mouse dx dy [buttons]]` | the TVC256k++ |
| `radix [h\|d]` | default number base |
| `help [command]` (`?`) | the list, or one command |

The command reference with the expression syntax is in [debug-and-remote.md](design/debug-and-remote.md).

### 8.2 MCP server

The MCP server lets an AI agent drive the emulator. It supports the stateless MCP protocol version 2026-07-28 and the session-free form of the earlier versions 2025-11-25, 2025-06-18, 2025-03-26 and 2024-11-05 (`initialize`).

- **stdio** (`freetvc-headless --mcp-stdio`): one JSON message per line on stdin/stdout, logs on stderr; the machine waits for tool calls; the run ends when stdin closes. It cannot be combined with `--monitor`.
- **HTTP** (`--mcp-http`, GUI and headless): `POST http://127.0.0.1:6584/mcp`, answered with `application/json` (no SSE; GET gets 405). Every request needs `Authorization: Bearer <token>`; the token is `--mcp-token T`, else a random one made at each start and printed on stderr (`MCP on http://127.0.0.1:6584/mcp (Authorization: Bearer ...)`); the GUI also shows it in Settings → Remote with **Copy**. A request with a foreign `Origin` header is refused (only the 127.0.0.1, localhost and [::1] origins of the port are accepted).
- In the GUI, tool calls run on the window's thread: a long `run` call holds the window until it returns.
- The machine moves only when a tool asks: `run`, `step`, `type_text` and `press_keys` leave it paused; `control resume` lets it run.

Example client configuration (the `mcpServers` shape most clients use; paths are examples):

```json
{"mcpServers": {
  "freetvc": {"command": "/path/to/freetvc-headless",
              "args": ["--preset", "64k+-2.2", "--mcp-stdio"],
              "env": {"FREETVC_ROM_DIR": "/path/to/roms"}},
  "freetvc-gui": {"type": "http", "url": "http://127.0.0.1:6584/mcp",
                  "headers": {"Authorization": "Bearer TOKEN"}}
}}
```

| Tool | Does |
|---|---|
| `machine_info` | machine, ROMs, paused or running, PC, frame, beam, paging, checkpoints |
| `control` | pause, resume, reset (memory kept), power_cycle (RAM cleared) |
| `run` | run for frames (default 1), T-states or instructions, or until an address; stops early on a checkpoint; returns why and the next instruction |
| `step` | into, over, out, line, line_over |
| `registers` | read all registers; `set` changes them |
| `memory` | read, write, fill, search; CPU view or `BANK:offset`; hex, base64 or integer arrays |
| `disassemble` | listing with labels, system-call names and source lines |
| `breakpoints` | add (every kind, conditions, log only), list, delete, enable, disable, clear |
| `screenshot` | PNG of the TV area (650 × 287) or the full raster (800 × 314) |
| `screen_text` | the text screen from the OS's copy (what BASIC and the editor print) |
| `type_text` | type text (`\n` = RETURN), wait, return the screen text |
| `press_keys` | hold keys by name or `ROW:COLUMN`, joysticks included |
| `snapshot` | in-memory states, `save_file` / `load_file` TVCS files, `rewind` |
| `symbols` | lookup, list, add, load label and debug files |
| `trace` | the instruction history with registers, the latest checkpoint hits |
| `io_state` | video, CRTC, sound, IRQ, paging |
| `evaluate` | a monitor expression |
| `load_binary` | a host file into memory, optionally jump there |
| `media` | cassette, autostart, tape, cartridges, MultiCart and SD images, disks, every card, printer, serial line |
| `card256` | the TVC256k++ card without side effects: `registers` (all 256, decoded), `sprites` (position, colour, phase, flags, data address; pixels as 24 × 21 rows of hex colours), `tiles` (the overlay's 32 × 30 screen codes and colours), `ram` (fast RAM or PSRAM) |
| `monitor_command` | one text monitor command; refused unless `--mcp-allow-monitor` (or `mcp_allow_monitor = yes`) |

After power-on the TVC's logo waits for a key, so keys typed in the first frames (about 330) are lost; `media autostart` handles this itself. [tools/mcp/README.md](../tools/mcp/README.md) describes checks with the official MCP Inspector; the protocol details are in [debug-and-remote.md](design/debug-and-remote.md).

### 8.3 DZRP for DeZog

`--dzrp` serves DeZog's DZRP 2.2 protocol on port 6582, for source-level debugging in VS Code. One client at a time; while DeZog is connected its breakpoints are ordinary checkpoints (the monitor's `break` lists them). DeZog's generic `dzrp` remote needs DeZog 3.8.0 or later; it was checked with 3.8.0-rc7 built from source ([tools/dezog/README.md](../tools/dezog/README.md)).

- By default the TVC is reported as a flat ZX Spectrum 48K, the only kind of machine DeZog's `dzrp` remote accepts: 0000h–3FFFh and 4000h–FFFFh show whatever the TVC has paged in.
- `--dzrp-banks` (or `dzrp_banks = yes`) reports the TVC's own banks in five slots (0000h, 4000h, 8000h, C000h, E000h) with bank numbers SYS 0, CART 1, EXT 2, U0–U3 3–6, VID0–3 7–10, CARD0–3 11–14, OPEN 15. This needs a patched DeZog: `tools/dezog/dezog-dzrp-custom-memory.patch` and its build script, see [../tools/dezog/README.md](../tools/dezog/README.md).
- A launch configuration: `"remoteType": "dzrp"`, `"dzrp": {"hostname": "127.0.0.1", "port": 6582}` (plus `customMemory` for TVC banks), an sjasmplus SLD file, `loadObjs` and `execAddress`; a complete one is `tools/dezog/project/.vscode/launch.json`. Assemble with `DEVICE ZXSPECTRUM48` in sjasmplus.
- Headless with `--dzrp` runs as fast as it can until DeZog connects.

### 8.4 GDB stub

`--gdb` serves the GDB remote protocol on port 6583, with GDB's `org.gnu.gdb.z80.cpu` register names, for GNU gdb built for the Z80 and DeZog's `gdb` remote. One client at a time; on connect the machine is held; detach, kill or a lost connection let it run on (kill does not end the emulator).

```sh
z80-unknown-elf-gdb -ex 'target remote 127.0.0.1:6583'
```

- Addresses from 10000h up are banks: `(bank + 1) << 16 | offset`, with the bank numbers of 8.3; plain addresses are the CPU's view.
- Breakpoints (`Z0`/`Z1`) and watchpoints (`Z2`–`Z4`) are checkpoints; gdb's `monitor CMD` runs one text monitor command.
- With DeZog: `"remoteType": "gdb"`, `"gdb": {"hostname": "127.0.0.1", "port": 6583}`.
- Building gdb for the Z80: [../tools/gdb/README.md](../tools/gdb/README.md).

## 9. freetvc-headless

`freetvc-headless` runs the machine without a window: for a T-state budget, under a text monitor, or as an MCP, DZRP or GDB server. It reads no settings from `freetvc.ini` except `rom_dir`, so a GUI session never changes a headless run. It needs `--preset` (or `--sys` and `--ext`) to have ROMs. It runs as fast as it can.

At the end of a run it prints the stop reason and the registers on stdout:

```
stop <reason>  tstates N  instructions N  pc XXXX  frames N
af .... bc .... de .... hl .... ix .... iy .... sp .... wz ....
af' .... bc' .... de' .... hl' .... i .. r .. im . iff1 . iff2 .
```

Exit codes: 0 success; 1 an error (a missing file, a failed autostart, an output that could not be written); 2 a bad option. With a monitor, `quit CODE` sets it.

### Options

`ADDR` and `VALUE` are hexadecimal (`8000`, `0x8000`, `8000h`); `N` is decimal.

| Option | Meaning |
|---|---|
| `--preset NAME` | machine and ROM set: `32k-1.2`, `32k+hbm-1.2`, `64k-1.2`, `64k+-2.1`, `64k+-2.2` |
| `--rom-dir DIR` | first place of the ROM search |
| `--fetch-roms` | download the ROMs into `--rom-dir` or `roms/` in the user data folder, then exit |
| `--model NAME` | `32k`, `32k+hbm`, `64k`, `64k+` (default 64k+, or the preset's) |
| `--sys FILE`, `--ext FILE` | a SYS ROM image (16 KiB), an EXT ROM image (4 or 8 KiB) |
| `--port02 VALUE` | write the page register before starting (`B0` = all RAM) |
| `--load FILE@ADDR` | copy a raw binary into RAM U0–U3 at ADDR (repeatable) |
| `--pc ADDR` | start execution at ADDR (default 0000, the reset address) |
| `--tstates N` | T-state budget (default 62 800, one frame) |
| `--frames N` | a budget of N frames (N × 62 800 T-states) |
| `--instructions N` | stop after N instructions |
| `--key KEY[+KEY]@F[:H]` | hold keys from frame F for H frames (default 5); KEY = `ROW:COL` or a name (`A`, `5`, `Á`, `SPACE`, `RETURN`, `SHIFT`, `CTRL`, `DEL`, `UP`, `JOY3_FIRE`...); repeatable |
| `--joy N:DIR[+DIR]@F[:H]` | joystick N (1 = row 8, 2 = row 9, 3 and 4 the GameCard's): `UP DOWN LEFT RIGHT FIRE ACCEL` |
| `--type TEXT@F` | type TEXT from frame F, LOCK off; `\n` = RETURN; repeatable |
| `--type-timing H:G[:R]` | typing: frames held, between keys, after RETURN (default 2:2:25) |
| `--cas FILE`, `--cas-dir DIR` | the host cassette ([4.1](#41-cas-files-the-host-cassette)) |
| `--autostart FILE` | attach, cold start, LOAD and RUN at full speed; frame numbers and budgets then count from the program's start; not with `--load`, `--pc`, `--port02` |
| `--tape FILE`, `--autostart-tape FILE`, `--tape-record FILE` | the tape deck ([4.3](#43-the-tape-deck)) |
| `--cart FILE`, `--cart-type TYPE`, `--cart-bank N` | a cartridge ([4.4](#44-cartridges)) |
| `--multicart-build OUT`, `--multicart-add FILE`, `--multicart-lang L` | build a MultiCart v2 image, then exit |
| `--sddual`, `--sddual-gm`, `--sddual-rom FILE`, `--sddual-flash-write`, `--sd FILE`, `--sd2 FILE` | the SD Dual ([section 5](#5-expansion-cards)) |
| `--sd-build OUT`, `--sd-add PATH`, `--sd-size MIB`, `--sd-list FILE` | build or list an SD card image, then exit |
| `--disk-card`, `--disk-rom ROM`, `--disk [A-D:]FILE` | the disk card ([4.5](#45-disks-and-the-disk-card)) |
| `--gamecard [SLOT]`, `--gamecard-rom FILE`, `--multichip [SLOT]`, `--multichip-rom F`, `--sid-card [MODEL]`, `--sid-clock HZ`, `--nanosd [SLOT]`, `--nanosd-rom FILE`, `--nanosd-sd DIR`, `--card256 [SLOT]`, `--card256-rom FILE`, `--card256-usb WHAT`, `--card256-flash F` | expansion cards ([section 5](#5-expansion-cards)) |
| `--printer FILE`, `--serial-card [SLOT]`, `--serial PEER`, `--serial-flow F`, `--serial-send FILE`, `--serial-capture FILE` | printer and serial card |
| `--screenshot FILE` | write the last complete frame as a binary PPM (800 × 314, the full raster) |
| `--snapshot FILE` | start from a TVCS snapshot; its board and ROM set win over `--preset`; budgets count from there |
| `--save-snapshot FILE` | write a TVCS snapshot at the end |
| `--wav FILE` | write the whole run's sound as a 48 kHz 16-bit mono WAV |
| `--video FILE` | record the TV picture and the sound as an MP4 through ffmpeg ([4.8](#48-video-recording)) |
| `--monitor` | text monitor on stdin/stdout, starting in the monitor |
| `--remote-monitor`, `--monitor-port N`, `--moncommands FILE` | the text monitor ([8.1](#81-text-monitor)) |
| `--mcp-stdio`, `--mcp-http`, `--mcp-port N`, `--mcp-token TOKEN`, `--mcp-allow-monitor` | MCP ([8.2](#82-mcp-server)) |
| `--dzrp`, `--dzrp-banks`, `--dzrp-port N` | DZRP ([8.3](#83-dzrp-for-dezog)) |
| `--gdb`, `--gdb-port N` | GDB stub ([8.4](#84-gdb-stub)) |
| `--version`, `--help` | |

With a monitor (stdin, TCP or script) the run lasts until `quit`, until the machine is stopped with nobody left to resume it, or until an explicit `--tstates` / `--frames` budget; `--instructions` is then ignored. With `--mcp-stdio` the run ends when stdin closes. Opens and saves of the host cassette are logged on stderr.

### Examples

```sh
# Download the ROMs once
freetvc-headless --fetch-roms

# Run a program for 5 s from its start and take a screenshot
freetvc-headless --preset 64k+-2.2 --autostart game.cas --frames 250 --screenshot game.ppm

# Type at the BASIC prompt: SPACE past the logo, then a line (frame numbers ❓, adjust to taste)
freetvc-headless --preset 64k+-2.2 --key SPACE@330 --type 'PRINT 6*7\n@400' --frames 500 --screenshot out.ppm

# Record the sound of a program
freetvc-headless --preset 64k-1.2 --autostart tune.cas --frames 1000 --wav tune.wav

# A test binary at 8000h with all RAM paged in, 100 000 instructions
freetvc-headless --preset 64k+-2.2 --port02 B0 --load test.bin@8000 --pc 8000 --instructions 100000

# Interactive monitor on the console
freetvc-headless --preset 64k+-2.2 --monitor

# A monitor script that runs, stops at a checkpoint and quits with a code
freetvc-headless --preset 64k+-2.2 --moncommands script.txt

# MCP over stdio for an agent
freetvc-headless --preset 64k+-2.2 --mcp-stdio
```

## 10. The GUI's command-line options

`freetvc` has no `--help`; an unknown option prints the usage line and exits with code 2. Options override `freetvc.ini` for one run.

| Option | Meaning |
|---|---|
| `--preset NAME` | machine and ROM set |
| `--rom-dir DIR` | first place of the ROM search |
| `--cas FILE`, `--cas-dir DIR`, `--autostart FILE` | host cassette, autostart |
| `--tape FILE`, `--autostart-tape FILE`, `--tape-record FILE` | the tape deck |
| `--cart FILE`, `--cart-type plain\|kilocart\|megacart\|multicart\|sddual`, `--cart-bank N` | a cartridge |
| `--snapshot FILE` | start from a snapshot |
| `--sddual`, `--sddual-gm`, `--sddual-rom FILE`, `--sddual-flash-write`, `--sd FILE`, `--sd2 FILE` | the SD Dual |
| `--disk-card`, `--disk-rom 1.1\|1.2\|FILE`, `--disk [A-D:]FILE` | the disk card |
| `--gamecard [SLOT]`, `--gamecard-rom FILE` | the GameCard |
| `--multichip [SLOT]`, `--multichip-rom FILE` | the Multichip sound card |
| `--sid-card [6581\|8580]`, `--sid-clock HZ` | the SID card |
| `--nanosd [SLOT]`, `--nanosd-rom FILE`, `--nanosd-sd DIR` | the NanoSD |
| `--printer FILE\|on` | a printer |
| `--serial-card [SLOT]`, `--serial PEER`, `--serial-flow rts\|none`, `--serial-send FILE`, `--serial-capture FILE` | the serial card |
| `--card256 [SLOT]`, `--card256-rom FILE`, `--card256-usb DIR\|mouse\|none`, `--card256-flash FILE\|DIR` | the TVC256k++ |
| `--keymap symbolic\|positional` | the keyboard mapping |
| `--keys NAME\|FILE` | the key layout |
| `--language en\|hu` | the UI language |
| `--skin NAME\|FILE` | the skin |
| `--picture NAME\|FILE` | the picture file |
| `--renderer gl\|sdl` | OpenGL (default) or SDL's renderer |
| `--full-raster` | show the full raster |
| `--no-audio` | open no sound device; the clock keeps time |
| `--frames N` | quit after N drawn frames (for scripted checks) |
| `--remote-monitor`, `--monitor-port N`, `--moncommands FILE` | the text monitor |
| `--mcp-http`, `--mcp-port N`, `--mcp-token T`, `--mcp-allow-monitor` | MCP over HTTP |
| `--dzrp`, `--dzrp-banks`, `--dzrp-port N` | DZRP |
| `--gdb`, `--gdb-port N` | GDB stub |
| `--version` | print the version |

Exit codes: 0 normal exit; 1 an error at start (no ROMs and the download declined, a file that cannot be read, a server from the command line that cannot listen); 2 a bad option, an unknown preset or an unknown `--language`.

## 11. Files freeTVC uses

| Where | File | What |
|---|---|---|
| next to the program | `fonts/`, `skins/`, `lang/`, `pictures/`, `keymaps/` | shipped fonts, skins, translations, pictures, key layouts |
| next to the program | `roms/` | ROMs of a release package or a build with the autofetch |
| next to the program (optional) | `freetvc.ini`, `layout.ini` | portable mode |
| user data folder | `freetvc.ini`, `layout.ini` | settings, Bench layout and watch list |
| user data folder | `roms/` | downloaded ROMs, with `ROMS-README.txt` |
| user data folder | `quick.tvcs` | the quick snapshot |
| user data folder | `screenshots/` | GUI screenshots |
| user data folder | `prompt/` | saved BASIC-prompt states that make autostarts fast |
| user data folder | `skins/`, `pictures/`, `keymaps/` | user skins, and the copies the settings window and the keyboard editor write (they shadow the shipped files) |

## 12. Troubleshooting

| Problem | What to do |
|---|---|
| "no ROMs for PRESET ...; searched: ..." | Use Download ROMs, `freetvc-headless --fetch-roms`, or put the files of [section 2](#2-getting-the-roms) in one of the searched folders; `--rom-dir` or `FREETVC_ROM_DIR` point at a folder. The full list of places is on stderr. |
| The download fails: curl not found | Install curl (Linux: e.g. `sudo apt install curl`) or put `curl.exe` on PATH. A SHA-256 mismatch means the file on the site changed; nothing is written then. |
| A card is "left out" at start | Its ROM is missing; Download ROMs fetches it, or give the file with the card's `-rom` option. |
| The window is black or does not open; graphics driver without OpenGL 3.3 | Start with `--renderer sdl` (or set `renderer = sdl`, Settings → Picture). freeTVC also falls back to SDL's renderer by itself when it gets no OpenGL 3.3 context; stderr says `drawing with ...`. |
| No sound, or the sound device misbehaves | The strip shows `no sound` when no device opened; stderr gives the reason, and the clock keeps time instead. `--no-audio` (or `audio = no`) skips the device. |
| Keys type the wrong characters | Switch between symbolic and positional mapping (the keyboard key), choose the `hungarian` or `us` layout, or rebind keys in the keyboard editor. In symbolic mapping the host's own keyboard layout decides the characters. |
| Keys do nothing in Bench | Click the Screen panel: only it passes keys to the TVC. |
| `LPRINT` hangs | No printer is plugged in; press STOP (CTRL+ESC), or plug one in (Cards → Printer port). |
| `LOAD` reads the disk instead of the cassette | The disk card (or NanoSD, SD Dual, TVC256k++ drives) takes over LOAD and SAVE; take it out in the Cards menu (Empty in its slot), or autostart the CAS, which gives the cassette back for that load. |
| `SAVE` fails with a protection error | The host cassette has no writable folder: start with `--cas-dir DIR`. |
| A server does not start | The port is taken: choose another (`--monitor-port`, Settings → Remote). From `freetvc.ini` the error shows in the settings window; from the command line the program exits. |
| MCP over HTTP answers 401 or 403 | Send `Authorization: Bearer <token>` with the token printed on stderr (or set one with `--mcp-token`); do not send a foreign `Origin` header. |
| A setting comes back after a restart | A command-line option was used for that run; the GUI saves only what changed in the GUI. Check which `freetvc.ini` is in use in Settings → Interface (a `freetvc.ini` next to the program wins). |
| Typed keys are lost right after power-on | The logo waits for a key; the first key only leaves the logo. Autostart handles this. |
