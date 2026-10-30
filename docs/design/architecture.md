# Architecture

How freeTVC's code is organised: the libraries and programs, the layering rule, how the machine runs a frame at half-T resolution, how sound and picture come out, how the debugger, the remote servers and the front ends drive the machine, and which document describes each part. The build itself is in [build.md](build.md).

## Goals that shape the design

- **Cycle-exact TVC**: VRAM clock stretching at half-T (160 ns) resolution, a 6845 stepped per character, sound and cursor IRQ timing from the hardware documentation ([tvc-hardware.md](../hardware/tvc-hardware.md)).
- **Deterministic and headless**: the machine's result depends only on its inputs and its own clock, never on the host's timing, so headless runs, tests, snapshots and rewind agree with the GUI.
- **One debug API** for the GUI, the text monitor, MCP, DZRP and the GDB stub ([debug-and-remote.md](debug-and-remote.md)).
- **Portable**: C++20 and C11, CMake + Ninja, SDL3 and Dear ImGui for the GUI; Linux and Windows (MinGW; MSVC presets exist), macOS not excluded.
- **Written from documentation**: the code is written from the hardware books, data sheets, card firmware sources and behaviour notes; other emulators were studied for their behaviour only. Reused third-party code is vendored with its licence ([THIRD_PARTY.md](../../THIRD_PARTY.md)).

## Source tree

```
src/
  core/            freetvc_core: the machine; no OS, threads, clocks or I/O
    z80/           the C++ adapter over floooh's z80.h (compiled as C11 in z80_chips.c)
    bus/           MemoryMap (page decoder, RAM, VRAM, ROM images, IOMEM), VRAM stretch tables,
                   cartridge boards with logic (MultiCart v2, SD Dual), SPI SD card, JEDEC flash
    machine/       Machine (run loop, I/O decode, IRQ, keyboard matrix, printer port), MachineConfig,
                   the attachment interfaces (debug hooks, OS call, tape port, printer port)
    video/         MC6845, the TVC video generator, the framebuffer
    audio/         the tone generator, band-limited synthesis (AudioSynth)
    input/         matrix positions, key names, the character map, timed key scripts
    cards/         the card slots and every card and chip: HBF + WD1793, GameCard + SN76489, TVC256k++
                   (overlay, storage, vSID), Multichip (SAA1099, YM3812), NanoSD, serial card + 8251A, SID card
    media/         CAS parsing, the host cassette, autostart, the tape deck and tape signals
    snapshot/      the TVCS format, the machine's chunks, the WinTVC .tvs import
    message.*      msg(): user-facing English messages that a front end can translate
  debug/           freetvc_debug: Debugger (checkpoints, stepping, trace, saved states, heat, event log),
                   expressions, symbols, source maps, disassembler, Z80 decoder, profiler; no OS either
  remote/          freetvc_remote: text monitor + hub, MCP (protocol, tools, stdio/HTTP), DZRP, GDB stub,
                   TCP layer, PNG writer
  frontend/
    common/        freetvc_frontend_common: presets and ROM search, freetvc.ini, HostTape (host files around
                   the machine), card host sides, ROM autofetch, unzip, WAV, printer and serial peers
    headless/      freetvc-headless
    sdl/           freetvc (GUI): Play and Bench, display (OpenGL 3.3 or SDL_Renderer), picture model,
                   host input and key layouts, skins, translations, settings window, keyboard editor
third_party/       vendored z80.h, Dear ImGui, nlohmann/json, FatFs, littlefs, ymfm, reSID (SDL3 is fetched)
data/              skins, key layouts, picture files, translations, the ROM autofetch list (roms.txt)
tests/             CTest programs; test data comes from testdata/ (git-ignored)
tools/             check harnesses and helper scripts (MCP, DeZog, GDB, sound, UI, translations, Z80 evaluation)
```

## Libraries and programs

From `src/CMakeLists.txt`:

| Target | Kind | Contents | Links |
|---|---|---|---|
| `freetvc_core` | static library | `src/core/` | `freetvc_z80h` (private), `freetvc_fatfs`, `freetvc_littlefs`, `freetvc_ymfm` (private), `freetvc_resid` (private) |
| `freetvc_debug` | static library | `src/debug/`, plus `docs/hardware/rom-symbols/*.sym` compiled in at build time (`cmake/EmbedText.cmake`) | `freetvc_core` |
| `freetvc_remote` | static library | `src/remote/` | `freetvc_debug`, `Threads::Threads`, `freetvc_json` (private), `ws2_32` on Windows |
| `freetvc_frontend_common` | static library | `src/frontend/common/`, plus `data/roms.txt` compiled in | `freetvc_core`; `freetvc_debug`, `freetvc_remote` (private) |
| `freetvc_skin` | static library (GUI builds) | skin and picture files, translation catalogs, `tr()`; no SDL or ImGui, so tests can use it | `freetvc_core` |
| `freetvc_keymap` | static library (GUI builds) | host key layouts (SDL scancodes, no window) | `freetvc_core`, `SDL3::SDL3` |
| `freetvc-headless` | program | `src/frontend/headless/main.cpp` | `freetvc_frontend_common`, `freetvc_remote` |
| `freetvc` | program (`FREETVC_BUILD_GUI`, default on) | `src/frontend/sdl/` | the above, `freetvc_skin`, `freetvc_keymap`, `freetvc_imgui`, `SDL3::SDL3` |

The third-party targets are defined in `third_party/CMakeLists.txt`: `freetvc_z80h` and `freetvc_json` (header-only), `freetvc_fatfs`, `freetvc_littlefs`, `freetvc_ymfm`, `freetvc_resid`, `freetvc_imgui`. The GUI's build copies fonts, skins, key layouts, picture files and translations next to the program; with `FREETVC_FETCH_ROMS=ON` both programs get a `roms/` folder too ([build.md](build.md)).

## Layering

- **`core/` and `debug/` have no OS, threads, clocks or I/O.** They take ROM images, card EPROMs and cartridge images as bytes (`RomImages`, `CardConfig`), never read files, never look at the host's time, and print nothing. A copy of a `Machine` is a complete machine state.
- Everything outside the machine reaches it through **attachments**: pointers set on the `Machine` that are not part of its state and survive `restore()` and snapshot loads unchanged:

| Attachment | Set with | Implemented by |
|---|---|---|
| `DebugHooks` | `set_debug_hooks` | `debug::Debugger` |
| `OsCallHandler` (calls at 0030h) | `set_os_call_handler` | `Media` (host cassette, autostart) |
| `TapePort` (59h b5, 50h–57h, motor bits) | `set_tape_port` | `TapeDeck` |
| `PrinterPort` | `set_printer` | the front end's printer (`frontend/common/printer_host.*`) |
| `FloppyDrives` | `set_floppy_drives` | `HostTape`'s drives, written back to host files |
| `Card256Media`, `NanoSdMedia` | `set_card256_media`, `set_nanosd_media` | `frontend/common/card256_host.*`, `HostTape` (host folders, flash images) |
| `SerialLine` | `set_serial_line` | `frontend/common/serial_host.*` (queued bytes, files, TCP) |
| `MultiCartFlash`, `SdCardImage` | `set_multicart_flash`, `set_sd_image` | `HostTape` (image files written in place) |

- Media that a real user would carry from machine to machine (disks, SD cards, flash images, the tape) are attachments, not state, so a snapshot or a rewind does not undo their writes ([media.md](media.md)).
- User-facing messages from `core/` and `frontend/common/` are built with `msg(format, ...)` (`core/message.*`): English text, with a hook through which the GUI records the format and its arguments and rebuilds the message in the UI language ([ui.md](ui.md), "Language").
- `remote/` may use sockets, threads and files; `frontend/` may use everything. Nothing below `frontend/` knows about SDL or ImGui.

## The machine

`Machine` (`core/machine/machine.*`) owns:

- `Z80` (`core/z80/`), the adapter over `z80.h`: one call per T-state, a 64-bit pin word in and out ([z80-core.md](z80-core.md));
- `MemoryMap` (`core/bus/`): RAM U0–U3, four VRAM banks, SYS, EXT and cartridge images, the page decoder, the cartridge boards, and `CardSlots` with the four slots' cards (cloned when the machine is copied);
- `Video` with its `Mc6845` (`core/video/`) and the framebuffers;
- `Sound` (`core/audio/sound.*`), the tone generator;
- an optional `SidCard` outside the slots;
- `AudioSynth` (`core/audio/synth.*`), where every sound source ends;
- the IRQ flip-flop, the keyboard matrix, the printer port flip-flops and the small port latches.

`MachineConfig` chooses the board, ROMs, cartridge, cards, SID card, colour switch and audio rate; `configure()` takes a new one and powers on. The board model, paging, I/O map and every timing rule are in [machine.md](machine.md); the cards in [cards.md](cards.md).

## How a frame runs

- **Clock**: machine time counts half-T ticks of 160 ns (6.25 MHz), the resolution of the VRAM stretch circuit and an integer divisor of the pixel clocks. One T-state is 2 ticks; one CRTC character is 4 ticks (2 T); one line of 100 characters is 400 ticks (200 T); the ROM's frame of 314 lines is 125 600 ticks (62 800 T, 49.76 Hz).
- **`Machine::run(RunLimits)`** runs until a tick budget, an instruction count or a debugger stop, optionally also stopping at the first instruction boundary in a new frame (`to_next_frame`), and returns a `StopInfo`. Each pass of its loop:
  1. sets the CPU's /INT pin from the IRQ flip-flop and the slots' /INT lines;
  2. calls `z80_tick()` once (one T-state);
  3. adds 2 ticks plus the VRAM stretch for that tick (`Machine::stretch()`: a memory cycle in a page that maps VID, its DEB phase taken at T1);
  4. lets the devices catch up to the new time (`advance_devices()`): the video generator steps every character whose CRTC slot has started (setting the IRQ flip-flop on a cursor pulse), the tone generator steps its reloads (setting it on a sound IRQ edge), and cards with a timed event (a chip timer that can pull /INT) run it; the debugger's beam-line watch is checked here;
  5. serves the bus request: memory reads and writes through `MemoryMap` (cartridge registers see CPU cycles only), INTA with FFh, I/O through `io_read()`/`io_write()`; flagged accesses are reported to the debugger;
  6. at an instruction boundary: reports the instruction to the debugger when a checkpoint, step or interrupt watch asks for it, offers the OS call at 0030h to the attached handler, and checks the frame and instruction limits.
- At the end of a run every card with sound and the SID card bring their chips up to the run's last tick, and `AudioSynth::end_at()` releases every sample no later step can change.
- **Lockstep instead of an event queue**: devices catch up to the CPU's time before each bus access, so a VRAM, palette, border or mode write lands after every character already fetched, and a port read sees the device at the right tick. Lazy devices (the tape deck, the WD1793, the sound chips on cards) compute their state from tick differences when they are touched.
- **Slices**: front ends call `run()` in slices of at most a frame or two; key events are applied at slice starts by frame number, and during autostart the slices end exactly at frame starts, so the result never depends on how a front end slices time ([media.md](media.md#autostart)).

## Sound

- The tone generator is a 12-bit reloading counter on the 3.125 MHz clock and a ÷16 chain: b0 is the ÷2 stage (the bus TXRXCLK that clocks the serial card), b3 the tone and the sound IRQ source; f = 195 312.5 / (4096 − P) Hz ([machine.md](machine.md#sound-6)).
- Every source gives timed level steps in half-T ticks: the TVC's amplitude DAC (`set_level`) and each card chip (`add_step`: SN76489, SAA1099, YM3812 through ymfm, the TVC256k++'s vSID, the SID card through reSID). The chips clock themselves against the machine's ticks without drift (`core/cards/chip_clock.hpp`).
- `AudioSynth` turns the steps into 48 kHz mono float samples with band-limited (windowed-sinc) steps and a 20 Hz high-pass for the TV's AC coupling, inside the core, so the output is deterministic and the same for every front end. At most 2 s of samples is kept.
- Front ends only read samples: the headless program writes a WAV, the GUI feeds an SDL3 audio stream whose fill level paces the emulation.

## Picture

- The 6845 is stepped once per character; the generator decodes the VRAM byte in the current mode and palette, applies border and blanking, and lets a card overlay (the TVC256k++) mix into the paper. The result goes into an 800 × 314 indexed framebuffer stored as a TV beam traces it (x from HSYNC, y from VSYNC), with index 16 for blanking; mid-line palette, border and mode changes are visible because the decode runs per character.
- At each frame start the working buffer is copied to the front buffer. Front ends crop it (the PAL TV area, 650 × 287 from x 131, y 23, or the whole raster) and scale it; the GUI's OpenGL display can run it through a PAL signal and tube model ([ui.md](ui.md), "Picture"). Monitor and MCP screenshots are indexed PNGs of the same areas (`remote/png.*`).

## Snapshots and state

The TVCS format (`core/snapshot/`): `TVCS`, a format version, then tagged, versioned chunks (`CONF`, `SLOT`, `CARD`, `CPU `, `BUS `, `LPT `, `MCRT`, `SDDC`, `SIDC`, `RAM `, `VRAM`, `VID `, `SND `, plus the front end's `MEDI`). It is byte-exact: a loaded snapshot runs on to the same ticks, registers, memory, picture and samples. It drives rewind, DZRP READ/WRITE_STATE, the MCP and monitor snapshot commands and the autostart prompt state on disk. The layout and the WinTVC `.tvs` import are in [media.md](media.md#snapshots).

## Debug layer

- `debug::Debugger` (`debug/debugger.*`) implements `DebugHooks`: it sets the machine's flag maps (exec/read/write per address, in/out per port, beam rows) for its checkpoints, so with nothing set the cost is one test per bus cycle. It provides run, step, step over/out, run to, checkpoints with banks, conditions, ignore counts and temporaries, a trace ring, saved states, the instruction history, byte heat per frame, and the port/interrupt event and I/O logs.
- Around it: expressions (`expression.*`), symbol tables with the built-in ROM symbols chosen by CRC-32 (`symbols.*`), source maps from sjasmplus, SDCC and z88dk debug files (`source_map.*`), the disassembler in sjasmplus syntax with the TVC's inline-data calls (`disassembler.*`), the Z80 decoder (`z80_decode.*`), and the profiler (`profile.*`).
- Every front end uses this API; the GUI's Bench issues its commands as monitor lines, so they work the same while the machine runs ([ui.md](ui.md), "Bench").

## Remote servers and threading

- The text monitor (`remote/monitor.*`) is one engine for the console, TCP clients, `--moncommands` scripts and the Bench's console. `MonitorHub::run()` is the way the front ends run the machine: it applies scripted keys, services `Media`, runs `Debugger::run()` in slices of at most a frame (two while an autostart is busy, ending at frame starts), and calls `MediaControl::after_slice()` (rewind points, disk write-back).
- Servers, all on 127.0.0.1: text monitor (TCP 6580), DZRP (6582), GDB stub (6583), MCP over HTTP (6584, bearer token and Origin check) or stdio; the serial card's TCP cable uses 6585. Protocols and tools: [debug-and-remote.md](debug-and-remote.md).
- **Threading**: the machine, the debugger, the monitor engine and every protocol handler run on one host thread (the GUI's main thread, or the headless program's thread). Reader and acceptor threads in the remote layer (`net.*`, `monitor_hub.*`, `mcp_host.*`, `dzrp_host.*`, `gdb_host.*`) only cut the byte streams into lines or messages and queue them under a mutex; the host thread drains the queues in `service()` between run slices. `Monitor`, `McpServer`, the DZRP server and `GdbServer` are not thread-safe by design. A long MCP `run` call therefore holds the GUI's frame loop while it runs.
- Other threads: the serial card's TCP peer has a reader thread (`frontend/common/serial_host.*`), the GUI's ROM download runs on its own thread, and the GUI's file dialogs answer through SDL callbacks; none of them touches the machine.

## Front ends

### Common parts

`frontend/common/` is the only place that turns file names into machine inputs:
- `presets.*`: the five presets, ROM files by name or CRC-32, the ROM search order (`--rom-dir`, `FREETVC_ROM_DIR`, `rom_dir` in `freetvc.ini`, `roms/` next to the program, `roms/` in the user data folder) ([machine.md](machine.md#presets));
- `settings.*`: `freetvc.ini`, next to the program (portable) or in the user data folder ([ui.md](ui.md), "Settings file and ROM search");
- `host_tape.*`: `HostTape`, the host files around the machine (cassette folder, tape files, cartridge, disks, SD images, snapshots, prompt states, rewind), implementing `MediaControl` for the monitor and MCP;
- `rom_fetch.*`, `unzip.*`: the ROM autofetch through the system's curl with pinned SHA-256 ([build.md](build.md), "ROM autofetch");
- `card256_host.*`, `multicart_image.*`, `sd_image.*`, `printer_host.*`, `serial_host.*`, `wav.*`: the host sides of cards and ports.

### Headless

`freetvc-headless` builds a `Machine` from the options and runs it unpaced:
- **plain runs**: frame-sized slices of `Machine::run()` up to the T-state budget (`--tstates`, `--frames`, `--instructions`), with key events (`--key`, `--joy`, `--type`) and `Media` serviced at each slice start; then screenshots, WAV, snapshot and register output;
- **with a monitor or a server** (`--monitor`, `--remote-monitor`, `--moncommands`, `--mcp-stdio`, `--mcp-http`, `--dzrp`, `--gdb`): a loop that services the hub and every server, runs the machine through `MonitorHub::run()` one frame at a time while nothing holds it, and otherwise waits for input;
- **autostart**: `Media::run_slice()` until the program runs, with the audio dropped; budgets and frame numbers then count from the program's start.

### GUI

`freetvc` runs everything on SDL's main thread. Each pass of the frame loop:
1. handles SDL events (keyboard, gamepads, drops, window);
2. services the monitor hub, MCP, DZRP and GDB hosts;
3. updates host input and services `Media`;
4. runs the machine through `MonitorHub::run()` in one of these pacing modes, at most two frames of ticks per call:
   - **audio-paced** (default): run until about 60 ms of samples wait in the SDL audio stream, so the device's clock sets the speed;
   - **wall-clock**: with `--no-audio` or no device, run the ticks the elapsed host time owes;
   - **unpaced**, in bursts of up to 12 ms per pass: an autostart (screen blanked, audio dropped), fast tape while the tape moves, fast disk while a drive was active in the last 0.25 s;
5. uploads the front framebuffer and draws the Play or Bench screen with Dear ImGui through the `Display` (OpenGL 3.3 with the picture model, or SDL_Renderer).

Pausing is a monitor stop, so the debugger, Bench and remote clients all see the same stopped machine. UI design, modes and panels are in [ui.md](ui.md).

## Testing

CTest programs in `tests/` cover the Z80 core (SingleStepTests subset, ZEXDOC/ZEXALL), the bus, video, input, sound, every card, the debugger, the remote protocols, ROM boots on every preset, media and snapshots through the real ROMs, golden frames against WinTVC 1.4.1, and the GUI's data files and translations. Tests whose data (ROMs, test suites, captures) is missing from `testdata/` are skipped with exit code 77. Protocol conformance is checked with real clients: the official MCP Inspector, DeZog and GNU gdb for the Z80 (`tools/`). How to run them: [build.md](build.md) and [developer-guide.md](../developer-guide.md).

## Where each part is documented

| Part | Document |
|---|---|
| Build, presets, ROM autofetch, test labels | [build.md](build.md) |
| Z80 core choice and patches | [z80-core.md](z80-core.md) |
| Board: timing, paging, I/O, interrupts, video, keyboard, sound, printer port, presets, golden frames | [machine.md](machine.md) |
| Host cassette, autostart, tape deck, cartridges, snapshots, rewind | [media.md](media.md) |
| Expansion cards and their chips, the SID card | [cards.md](cards.md) |
| Debug core, text monitor, MCP, DZRP, GDB stub | [debug-and-remote.md](debug-and-remote.md) |
| External protocol specifications | [protocols-reference.md](protocols-reference.md) |
| GUI: Play, Bench, skins, picture, language, settings, keyboard editor | [ui.md](ui.md) |
| Hardware reference, ROM analysis, file formats | [tvc-hardware.md](../hardware/tvc-hardware.md), [rom.md](../hardware/rom.md), [file-formats.md](../hardware/file-formats.md) |
