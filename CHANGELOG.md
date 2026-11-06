# Changelog

## Unreleased

### New

- Video recording ([#1](https://github.com/LaM3rzLG/FreeTVC/issues/1)): the TV picture, pixels doubled, and the sound into an MP4 (H.264, AAC) through ffmpeg; `freetvc-headless --video FILE`. The packages carry a small GPL ffmpeg in `ffmpeg/` (`tools/release/build_ffmpeg.sh`), so nothing has to be installed.

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
