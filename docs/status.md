# Status

What freeTVC emulates today, how its accuracy was checked, and what is still missing. For how to use it see the [user guide](user-guide.md); for how to build and change it see the [developer guide](developer-guide.md).

## What is emulated

| Area | State | Reference |
|---|---|---|
| Z80A CPU | floooh's `z80.h` with our patches (Q register, block-repeat flags, HALT); passes ZEXDOC and ZEXALL and a SingleStepTests/z80 subset | [z80-core.md](design/z80-core.md) |
| Base machine | the 32k, 32k + 32k (HBM), 64k and 64k+ boards; SYS 1.2 / 2.1 / 2.2 with EXT 1.x / 2.x; paging, I/O decode, IRQ flip-flop, VRAM clock stretch at half-T resolution | [machine.md](design/machine.md) |
| Video | MC6845 with the TVC video generator, every mode and palette, a beam-ordered 800 × 314 framebuffer | [machine.md](design/machine.md#video-5) |
| Sound | the tone generator, sound IRQ and DAC with band-limited synthesis to 48 kHz | [machine.md](design/machine.md#sound-6) |
| Keyboard and joysticks | the 10 × 8 matrix with ghosting, symbolic and positional host mapping, editable key layouts, gamepads | [machine.md](design/machine.md), [ui.md](design/ui.md#keyboard-editor) |
| Printer port | the Centronics port; output to raw or UTF-8 text files | [machine.md](design/machine.md#printer-port) |
| Program loading | a host cassette serving the ROM's own `LOAD`/`SAVE` from CAS files and folders, fast autostart | [media.md](design/media.md) |
| Tape deck | the real tape signal through the ROM's tape routines: CAS as tape, TTP, WAV in and out | [media.md](design/media.md#tape-deck) |
| Cartridges | plain (with bank switches), KiloCart, MegaCart, MultiCart v2, SD Dual | [media.md](design/media.md#cartridges), [cards.md](design/cards.md) |
| Snapshots and rewind | freeTVC's own TVCS (byte-exact), WinTVC `.tvs` import, 60 s rewind | [media.md](design/media.md#snapshots), [file-formats.md](hardware/file-formats.md#wintvc-tvs) |
| Expansion cards | HBF disk card (WD1793, four drives, VT-DOS 1.1 / 1.2), GameCard, TVC256k++ (memory, overlay, storage, mouse, vSID), Multichip sound card, NanoSD, SID card, serial card, 2dfx graphics card (the creator's library) | [cards.md](design/cards.md) |
| Debugger | checkpoints with conditions and banks, stepping, trace, history, saved states, expressions, symbols, disassembler, source-level debugging, profiler | [debug-and-remote.md](design/debug-and-remote.md) |
| Remote interfaces | VICE-style text monitor, MCP server for AI agents, DZRP for DeZog, GDB stub | [debug-and-remote.md](design/debug-and-remote.md) |
| GUI | Play mode, the Bench debugger with its panels, skins, a settings window, OpenGL picture model (PAL signal and tube) with an SDL fallback, keyboard editor, English and Hungarian | [ui.md](design/ui.md) |
| Headless runner | `freetvc-headless` for scripts and tests: typing, screenshots, WAV output, snapshots | [user-guide.md](user-guide.md) |

## How accuracy was checked

- **CPU:** ZEXDOC and ZEXALL pass (67/67 each); a SingleStepTests/z80 subset passes with per-T-state bus checks.
- **Video and boot:** frames captured from WinTVC 1.4.1 (logo, BASIC prompt, a 2-colour and a 16-colour mode screen on four presets) match freeTVC pixel for pixel. Boot timing equals WinTVC's except for VRAM accesses, where freeTVC models the clock stretch and WinTVC does not.
- **ROM behaviour:** all presets boot to the BASIC prompt; 115 CAS programs load through both the host cassette and the tape deck on SYS 2.2 and 1.2; real tape recordings load (8 of 9 tested).
- **Cards:** each card's own firmware runs: its menus, demos, file functions and loaders, as listed in [cards.md](design/cards.md).
- Hardware facts carry evidence markers in the docs: ✓ verified, ❓ unknown, ⚑ needs a check on real hardware or a dynamic run.

## Platforms

| Platform | State |
|---|---|
| Linux x86-64, GCC 13 and Clang 18 | built and tested |
| Windows x86-64 | cross-built with MinGW-w64 from Linux; tests and the GUI run under Wine |
| Windows, native build (MSYS2 GCC or MSVC) | described in [windows.md](windows.md), not built or tested |
| macOS (Apple Silicon) | built with `tools/mac/build_mac.sh` and used daily (GUI, headless, MCP); no tests run ([MAC_ALLAPOT.md](../MAC_ALLAPOT.md)) |

## Known gaps

- **Releases:** a Windows zip and a Linux tarball (glibc 2.38 or newer), built by `tools/release/make_release.sh`; no AppImage, no installer, no code signing, no CI.
- **Hardware questions:** the open ❓ items in [tvc-hardware.md](hardware/tvc-hardware.md) and [cards.md](design/cards.md) need a real machine: for example the disk card's READY, disk-change and reset wiring.
- **Tape:** one real recording (Space Sapper, third file) does not load: a sync half-wave of 3 counts falls under the ROM's 4-count threshold.
- **Printer and serial:** no host serial port (COM, `/dev/tty*`) and no Epson ESC/P rendering; printer output is a byte or text file.
- **Debugger:** monitor checkpoint commands (VICE `command`), MCP resources, progress notifications and cancelling a running call are not built; DZRP `SET_SLOT` is not built.
- **ROM documentation:** the BASIC function list, operator dispatch, RST 18h operation meanings and SYS 2.1 differences are not fully documented in [rom.md](hardware/rom.md).
- **Not planned:** the Cyrillic TVC (WinTVC patches SYS 1.2 for it; no original ROM is known), SoundQuartett, the 6-channel SAA1099 modification, UPM on the disk card, YATE's SDCart.
