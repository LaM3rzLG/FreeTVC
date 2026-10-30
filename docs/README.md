# freeTVC documentation

freeTVC emulates the Videoton TV Computer (TVC). These pages describe how to use it, how to build and extend it, how it is designed, and the TVC hardware it models. Everything here describes the current state of the code.

## Start here

| Page | For |
|---|---|
| [user-guide.md](user-guide.md) | running freeTVC: ROMs, presets, Play mode, loading software, cards, settings, Bench, the monitor, MCP, DeZog and GDB, `freetvc-headless` |
| [developer-guide.md](developer-guide.md) | building from a fresh clone, test data, tests, repository layout, coding rules, recipes for extending the emulator |
| [windows.md](windows.md) | building and developing on Windows: MSYS2, Visual Studio, WSL 2, ROMs, test data, running and debugging |
| [status.md](status.md) | what is emulated, how accuracy was checked, known gaps |
| [glossary.md](glossary.md) | TVC and project terms |

## Design

| Page | Covers |
|---|---|
| [design/architecture.md](design/architecture.md) | the source tree, libraries and programs, layering, how a frame runs |
| [design/build.md](design/build.md) | CMake presets and options, dependencies, the ROM autofetch, every test and its data |
| [design/z80-core.md](design/z80-core.md) | the Z80 core (floooh's `z80.h`) and our patches |
| [design/machine.md](design/machine.md) | the base machine: bus, paging, I/O, interrupts, video, sound, keyboard, printer port, presets |
| [design/media.md](design/media.md) | host cassette, autostart, tape deck, cartridges, snapshots, rewind, disk images |
| [design/cards.md](design/cards.md) | the card slots and every expansion card and cartridge-port device |
| [design/debug-and-remote.md](design/debug-and-remote.md) | debugger core, expressions, symbols, disassembler, text monitor, MCP server, DZRP, GDB stub |
| [design/protocols-reference.md](design/protocols-reference.md) | notes on the external protocols (DZRP, GDB RSP, MCP) |
| [design/ui.md](design/ui.md) | the GUI: design direction, Play mode, Bench, settings window, picture model, keyboard editor, languages, skins |

## TVC hardware reference

| Page | Covers |
|---|---|
| [hardware/tvc-hardware.md](hardware/tvc-hardware.md) | the machine as documented and verified: memory map, ports, video, sound, keyboard, cards |
| [hardware/rom.md](hardware/rom.md) | the system ROMs: versions, CRCs, entry points, OS calls |
| [hardware/file-formats.md](hardware/file-formats.md) | CAS, TTP, WAV tapes, cartridge images, disk images, snapshots |
| [hardware/rom-symbols/](hardware/rom-symbols/) | symbol files for the ROMs (compiled into the debugger) |

## Tools

The checks and helpers in `tools/` each have a README: [dezog](../tools/dezog/README.md), [gdb](../tools/gdb/README.md), [lang](../tools/lang/README.md), [mcp](../tools/mcp/README.md), [re](../tools/re/README.md), [release](../tools/release/README.md), [sound](../tools/sound/README.md), [ui](../tools/ui/README.md), [z80eval](../tools/z80eval/README.md).

## Related projects

Other TVC emulators: [WinTVC](https://tvc.homeserver.hu/html/wintvcemulatorrol10.html) (Windows), [ep128emu](https://github.com/istvan-v/ep128emu) (TVC support since 2.0.11), [MAME](https://github.com/mamedev/mame) (`src/mame/videoton/tvc.cpp`), [YATE](https://github.com/hightower70/YATE), [rtvc](https://github.com/teki/rtvc). The TVC community's archive of software, documents and ROMs is [tvc.homeserver.hu](https://tvc.homeserver.hu).
