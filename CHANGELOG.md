# Changelog

## 1.3.2 — 2026-11-08

### New

- **Frame blending** for displays that are not 50 Hz: the TVC draws 49.76 frames a second, so a 60 Hz display must show every fifth frame twice and a scroll jerks ten times a second (less visibly at 120, 144 or 180 Hz). Blending mixes the two frames around each refresh, so motion is even. *Automatic* (default) blends unless the display runs at 50 or 100 Hz, where there is no jerk. Settings → Picture → Frame blending, `frame_blend` in `freetvc.ini`.
- **Hz key** in the strip, beside the speaker: bright while blending is on, dim while off; a click turns it on or off. While it is on, a right click opens the **Sharpness** slider (0 %: smoothest; higher: only the change is mixed and most pictures stay sharp; `blend_sharpness`, default 50 %).

### Fixed

- **Strip keys did not react to clicks** after the strip was made smaller by dragging its groove: the groove's invisible handle had the UI's minimum window size and covered the upper part of the keys.

### Linux

- Use **Ubuntu 24.04 LTS** or newer (or another Linux of 2024 or later: Debian 13, Fedora 40). Ubuntu 22.04 is too old.
- The program is the file `freetvc` in the unpacked folder. A double click in the Files app does not start it; open a terminal in the folder and run `./freetvc` (step by step in the Quick start below and in [README.md](README.md#running-on-linux)).

### Downloads

| File | For |
|---|---|
| `freeTVC-1.3.2-windows-x64.zip` | 64-bit Windows: unpack anywhere and run `freetvc.exe`. The programs are not signed, so Windows SmartScreen may warn on the first start. |
| `freeTVC-1.3.2-linux-x86_64.tar.gz` | 64-bit Linux: Ubuntu 24.04 LTS or newer (Debian 13, Fedora 40 or newer also work): unpack and run `./freetvc` in a terminal. |
| `freeTVC-1.3.2-macos-arm64.zip` | macOS 13 or newer on Apple Silicon (M1 and later): unpack and run `freetvc`. |
| `freeTVC-1.3.2-macos-x86_64.zip` | macOS 13 or newer on Intel Macs. |
| `SHA256SUMS`, `SHA256SUMS-macos` | checksums of the archives |
| `ffmpeg-*.tar.xz`, `x264-*.tar.bz2` | the sources of the bundled ffmpeg (GPL) |

## 1.3.1 — 2026-11-08

### New

- **Icon**: the TV Computer's photo on a dark rounded tile, in the Dock, the taskbar, the Alt+Tab / Cmd+Tab switcher and on the Windows `freetvc.exe`.

### Changed

- **Smooth display**: fine scrolling and motion are even, without jumps. About every 16th TVC frame never reached the screen, because the machine runs in slices paced by the audio; now every frame is queued and the display takes them on an even clock about 30 ms behind. The machine's speed is unchanged (49.76 frames/s, as on the real TVC).
- Works at any refresh rate (60, 75, 120, 144, 180 Hz...): at start and after a display change the GUI checks for 1 s whether vsync follows the display; when it does not (macOS OpenGL holds even a 180 Hz display at 60 Hz), it times presents at the display's rate itself, with the picture effects kept.
- Measuring: with `FREETVC_FRAME_LOG=file` the GUI logs every present, and `tools/frame_pacing_check.py file` reports dropped frames.

### Downloads

| File | For |
|---|---|
| `freeTVC-1.3.1-windows-x64.zip` | 64-bit Windows: unpack anywhere and run `freetvc.exe`. The programs are not signed, so Windows SmartScreen may warn on the first start. |
| `freeTVC-1.3.1-linux-x86_64.tar.gz` | 64-bit Linux with glibc 2.39 or newer (Ubuntu 24.04, Debian 13, Fedora 40 or newer): unpack and run `./freetvc`. |
| `freeTVC-1.3.1-macos-arm64.zip` | macOS 13 or newer on Apple Silicon (M1 and later): unpack and run `freetvc`. |
| `freeTVC-1.3.1-macos-x86_64.zip` | macOS 13 or newer on Intel Macs. |
| `SHA256SUMS`, `SHA256SUMS-macos` | checksums of the archives |
| `ffmpeg-*.tar.xz`, `x264-*.tar.bz2` | the sources of the bundled ffmpeg (GPL) |

## 1.3.0 — 2026-11-07

### New

- **Game saves**: F9, or the new game save key beside the TVC key, stops the machine, dims the picture and asks for the save's name; the save goes to `SaveState/<game>/<name>.tvcs` in the user data folder with a picture of the screen, and a name already there is replaced only after a question. The game is the program started last (or the disk, the cartridge, the NanoSD's or the pendrive's folder, the GameCard). Shift+F9 loads the game's newest save; a right click on the key lists the game's saves with their times, every game's saves, and opens the folder. A save holds the whole machine: the board, the ROMs, the cards in their slots and the media by path, the NanoSD's folder and the TVC256k++'s pendrive included, so it comes back on a machine set up differently. MCP `game_saves`: list, save, check (which files a save needs and whether they are there), load (with moved folders), delete.
- The status strip's size: drag the groove above it, or Settings → Interface → Status strip (75–250 %); the keys grow with it, and the window widens when they would not fit.
- The text size of the menus, the settings window and the strip: Settings → Interface → Text (12–36 pt).

### Changed

- Notices appear in yellow on the TV picture's top border instead of the strip.
- The speed (or *paused*) is in the window title: `freeTVC · <machine> · 100 %`.
- The main menu's Snapshot submenu and the single `quick.tvcs` quick save are gone: F9 and Shift+F9 are game saves. A dropped `.tvcs` file still loads.
- Settings → Machine keeps only the ROM folder; the machine and the cards are chosen in the strip's menus. Settings → Picture names the pictures by what they show (Black and white TV, CRT TV emulation, RGB monitor emulation, Sharp picture). The settings window's text is as large as the menus'.

### Downloads

| File | For |
|---|---|
| `freeTVC-1.3.0-windows-x64.zip` | 64-bit Windows: unpack anywhere and run `freetvc.exe`. The programs are not signed, so Windows SmartScreen may warn on the first start. |
| `freeTVC-1.3.0-linux-x86_64.tar.gz` | 64-bit Linux with glibc 2.39 or newer (Ubuntu 24.04, Debian 13, Fedora 40 or newer): unpack and run `./freetvc`. |
| `freeTVC-1.3.0-macos-arm64.zip` | macOS 13 or newer on Apple Silicon (M1 and later): unpack and run `freetvc`. |
| `freeTVC-1.3.0-macos-x86_64.zip` | macOS 13 or newer on Intel Macs. |
| `SHA256SUMS`, `SHA256SUMS-macos` | checksums of the archives |
| `ffmpeg-*.tar.xz`, `x264-*.tar.bz2` | the sources of the bundled ffmpeg (GPL) |

## 1.2.0 — 2026-11-07

### New

- Video recording ([#1](https://github.com/LaM3rzLG/FreeTVC/issues/1)): the TV picture at the TV's 4:3 aspect (1534 × 1148) and the sound into an MP4 (H.264, AAC) through ffmpeg: the REC key in the status strip (a click starts and stops, a right click picks the folder), `freetvc-headless --video FILE`. The packages carry a small GPL ffmpeg in `ffmpeg/` (`tools/release/build_ffmpeg.sh`), so nothing has to be installed; its sources are among the release files.
- A new status strip of drawn keys: the cartridge with its name, the machine (versions and power), the keyboard (symbolic, positional, the key layout), the speaker (crossed out when muted), the 5.25" floppy, the cassette with play, counter, stop and rewind, the printer, REC, the camera (a 4:3 PNG), the speed and the freeTVC nameplate with the main menu. While the tape moves, the cassette shows the signal under the head as a small wave.
- A faint photo of the TVC 64k+ covers the TV picture at start, at power on and when a card changes, until the machine draws.
- The main menu: *Recent programs* (the last ten, kept in `freetvc.ini`), the snapshot, skin and language submenus, *Download ROMs*.
- The floppy menu starts with *Load a program directly*: the disk goes into the first empty drive and its first program starts.
- In a slot's list the NanoSD and the TVC256k++ take their SD card folder, pendrive folder or flash image as they go in.

### Changed

- *Load program* no longer takes cartridges: they go in from the Cart key.
- The HBF card's own menu keeps only its disk system; the disks are on the floppy key.

### Fixed

- The TVC256k++ overlay showed only every other pixel in 16-colour mode, which broke checkerboard dithers (dikdom's Wolf256 title) into black lines; every pixel shows now, as on the 2dfx card.

### Downloads

| File | For |
|---|---|
| `freeTVC-1.2.0-windows-x64.zip` | 64-bit Windows: unpack anywhere and run `freetvc.exe`. The programs are not signed, so Windows SmartScreen may warn on the first start. |
| `freeTVC-1.2.0-linux-x86_64.tar.gz` | 64-bit Linux with glibc 2.39 or newer (Ubuntu 24.04, Debian 13, Fedora 40 or newer): unpack and run `./freetvc`. |
| `freeTVC-1.2.0-macos-arm64.zip` | macOS 13 or newer on Apple Silicon (M1 and later): unpack and run `freetvc`. |
| `freeTVC-1.2.0-macos-x86_64.zip` | macOS 13 or newer on Intel Macs. |
| `SHA256SUMS`, `SHA256SUMS-macos` | checksums of the archives |
| `ffmpeg-*.tar.xz`, `x264-*.tar.bz2` | the sources of the bundled ffmpeg (GPL) |

## 1.1.0 — 2026-11-06

### New

- The slot of each expansion card can be chosen in the GUI ([#1](https://github.com/LaM3rzLG/FreeTVC/issues/1)): in the Cards menu, *Put in slot* (or *Move to slot* while the card is in) under the GameCard, the Multichip sound card, the TVC256k++, the NanoSD and the serial card lists slots 0–3 with their ports, a slot holding another card greyed out with its name; Settings → Machine has slot buttons 0–3 beside each card. `freetvc.ini` keeps the slot (`gamecard = 3`; `yes` is the card's own slot). The HBF disk card stays in slot 0.
- The 2dfx graphics card (Vaczkó Károly's: sprites, 2D and 3D graphics over the paper, ports F8h/F9h), emulated with the creator's own MIT library (2dfx v1.1.0, `third_party/twodfx/`): any slot (default 2), Cards menu and Settings, `twodfx` in `freetvc.ini`, `--2dfx [SLOT]` in the GUI and headless, monitor `2dfx`, MCP `media` `twodfx_on`/`twodfx_off`. Every other card fits beside it except the TVC256k++; beside it the SID card keeps E0h–F7h. Checked with AnyStone's Cube, Wolfenstein 3D and AnyShow from tvc.homeserver.hu. Its state is kept in snapshots and rewind.
- macOS packages for Apple Silicon and Intel (macOS 13 or newer), built with `tools/mac/make_mac_release.sh`; the Linux and Windows packages are built by GitHub Actions (`.github/workflows/release.yml`).
- MCP tool `card256` ([#1](https://github.com/LaM3rzLG/FreeTVC/issues/1)): the TVC256k++ card's state for debugging, read without side effects: every register, decoded; the 16 sprites with their pixels; the overlay's 32 × 30 tile codes and colours; the card's fast RAM and PSRAM.

### Fixed

- The host cassette read data files that are not `.CAS` (such as the 2dfx Wolfenstein 3D's `.bin` files) as CAS files and dropped their first 144 bytes; it now serves them byte for byte, as the NanoSD card does.

### Downloads

| File | For |
|---|---|
| `freeTVC-1.1.0-windows-x64.zip` | 64-bit Windows: unpack anywhere and run `freetvc.exe`. The programs are not signed, so Windows SmartScreen may warn on the first start. |
| `freeTVC-1.1.0-linux-x86_64.tar.gz` | 64-bit Linux with glibc 2.39 or newer (Ubuntu 24.04, Debian 13, Fedora 40 or newer): unpack and run `./freetvc`. |
| `freeTVC-1.1.0-macos-arm64.zip` | macOS 13 or newer on Apple Silicon (M1 and later): unpack and run `freetvc`. |
| `freeTVC-1.1.0-macos-x86_64.zip` | macOS 13 or newer on Intel Macs. |
| `SHA256SUMS`, `SHA256SUMS-macos` | checksums of the archives |

The macOS programs are not notarized by Apple. On the first start macOS refuses to open them: in Finder, Control-click `freetvc` and choose *Open*, or run `xattr -dr com.apple.quarantine` on the unpacked folder. A Hungarian changelog is in `CHANGELOG.hu.md`.

## 1.0.1 — 2026-10-31

### New

- An About window (main menu → *About freeTVC...*): the L@M3rZ logo, copper raster bars and a sine scroller in the TVC's colours, the version and build details, the credits, and a button that copies the build details for bug reports.

### Downloads

| File | For |
|---|---|
| `freeTVC-1.0.1-windows-x64.zip` | 64-bit Windows: unpack anywhere and run `freetvc.exe`. The programs are not signed, so Windows SmartScreen may warn on the first start. |
| `freeTVC-1.0.1-linux-x86_64.tar.gz` | 64-bit Linux with glibc 2.38 or newer (Ubuntu 24.04, Debian 13, Fedora 39 or newer): unpack and run `./freetvc`. Older distributions: build from source ([docs/developer-guide.md](https://github.com/LaM3rzLG/FreeTVC/blob/main/docs/developer-guide.md)). |
| `SHA256SUMS` | checksums of both archives |

The packages contain the same ROM set as 1.0.0; `roms/ROMS-README.txt` lists what is included and what the GUI's "Download ROMs" item fetches.

## 1.0.0 — 2026-10-30

The first release of freeTVC, an emulator of the Videoton TV Computer (TVC) for Linux and Windows.

### What it emulates

- The 32k, 32k + 32k, 64k and 64k+ machines with BASIC 1.2, 2.1 and 2.2: Z80 (passes ZEXDOC and ZEXALL), MC6845 video with every mode, VRAM clock stretching, the tone generator and sound interrupt, the keyboard matrix, joysticks and gamepads, the printer port.
- Loading software: CAS files and folders through the ROM's own `LOAD` with fast autostart, the tape deck with the real tape signal (CAS, TTP, WAV), cartridges (plain, KiloCart, MegaCart, MultiCart v2, SD Dual), snapshots (freeTVC's own and WinTVC `.tvs`), 60 s rewind.
- Expansion cards: the HBF disk card with VT-DOS 1.1 and 1.2, GameCard, TVC256k++ (memory, overlay graphics, storage, mouse, vSID), Multichip sound card, NanoSD, SID card, serial card.

### For debugging and automation

- Bench, the debugger in the GUI: checkpoints, stepping, memory, video, I/O log, history, profiler, source-level debugging (sjasmplus, SDCC, z88dk).
- A VICE-style text monitor, an MCP server for AI agents, a DZRP server for DeZog and a GDB stub.
- `freetvc-headless` for scripts and tests: typing, screenshots, WAV output, snapshots.

### GUI

- Play mode with a status strip and menus, a settings window, an OpenGL picture model of a PAL TV or monitor (with a plain SDL renderer as fallback), an editable host key layout, English and Hungarian.

### Downloads

| File | For |
|---|---|
| `freeTVC-1.0.0-windows-x64.zip` | 64-bit Windows: unpack anywhere and run `freetvc.exe`. The programs are not signed, so Windows SmartScreen may warn on the first start. Tested under Wine; feedback from real Windows machines is welcome. |
| `freeTVC-1.0.0-linux-x86_64.tar.gz` | 64-bit Linux with glibc 2.38 or newer (Ubuntu 24.04, Debian 13, Fedora 39 or newer): unpack and run `./freetvc`. Older distributions: build from source ([docs/developer-guide.md](https://github.com/LaM3rzLG/FreeTVC/blob/main/docs/developer-guide.md)). |
| `SHA256SUMS` | checksums of both archives |

Both packages contain the programs, the GUI's data folders, the documentation and a `roms/` folder with the system ROMs, Videoton's disk-card ROMs and the card firmware whose licence allows it; `roms/ROMS-README.txt` lists the origin and copyright of each file. The Multichip and SD Dual firmware and two TVC256k++ files are not included: the GUI's "Download ROMs" item or `freetvc-headless --fetch-roms` downloads them.

### Known limits

See [docs/status.md](https://github.com/LaM3rzLG/FreeTVC/blob/main/docs/status.md): among them, no native Windows build has been tested yet, one real tape recording does not load, and there is no host serial port or ESC/P printer rendering.
