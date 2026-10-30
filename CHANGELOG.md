# Changelog

## 1.0.1 — 2026-10-31

### New

- An About window (main menu → *About freeTVC...*): the L@M3rZ logo, copper raster bars and a sine scroller in the TVC's colours, the version and build details, the credits, and a button that copies the build details for bug reports.

### Fixed

- Keystrokes shorter than a TVC frame (paste or macro tools, or a moment when the GUI is busy) were lost, merged or reordered. Every key now stays down for at least a frame, a repeated key waits until it has been up for a frame, and new keys reach the TVC one per frame in the order they were pressed.

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
