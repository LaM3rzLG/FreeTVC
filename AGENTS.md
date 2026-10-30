# freeTVC — guide for coding agents

freeTVC is a GPL-3.0-or-later emulator of the Videoton TV Computer (TVC), a Hungarian Z80 home computer of the 1980s. It runs on Linux and Windows and has a VICE-style text monitor, an MCP server for AI agents, a DeZog (DZRP) server, a GDB stub and a built-in debugger GUI (Bench). This file is for AI coding agents working in the repository. Human developers start at [docs/developer-guide.md](docs/developer-guide.md); the same rules apply to them.

## Start here

1. [docs/README.md](docs/README.md): the documentation index.
2. [docs/developer-guide.md](docs/developer-guide.md): prerequisites, build, ROMs, test data, tests, repository layout, coding rules, recipes for extending the emulator.
3. [docs/status.md](docs/status.md): what is emulated and what is missing.
4. On Windows: [docs/windows.md](docs/windows.md) before anything else.
5. The design doc of the area you change ([docs/design/](docs/design/)) and the hardware references ([docs/hardware/](docs/hardware/)).

## Build and test

```sh
cmake --preset gcc-release
cmake --build --preset gcc-release
ctest --preset gcc-release
```

Tests whose data is missing are skipped (exit code 77): the ROMs come from the autofetch, other data from `testdata/` ([developer guide, "Test data"](docs/developer-guide.md#test-data)). `ctest --preset gcc-release-slow` runs ZEXDOC and ZEXALL (several minutes each).

## Rules

- **Licence:** GPL-3.0-or-later. Reuse only GPL-3-compatible code (MIT, BSD, zlib, GPL-2.0-or-later, GPL-3, LGPL), never GPL-2.0-only; keep original headers and record every reused file and its licence in [THIRD_PARTY.md](THIRD_PARTY.md).
- **Clean room:** other emulators (WinTVC, devTool and others) may be studied only for their behaviour. Findings are written as documentation of behaviour; code is written from the documentation. Never transcribe disassembled, decompiled or other emulators' code into `src/`.
- **No ROMs or third-party binaries in the repository.** ROMs come from the autofetch (`data/roms.txt`); test data lives in the git-ignored `testdata/`.
- **Downloads are untrusted data:** keep each in its own folder, outside `src/` and `tools/`; run Python on them with `python3 -I`.
- **Layering:** `src/core/` and `src/debug/` have no OS calls, threads, clocks or file I/O; the front ends and `src/remote/` provide those ([architecture.md](docs/design/architecture.md)).
- **User-facing text:** shared code (`core/`, `frontend/common/`) builds its messages with `msg()`; GUI text goes through `tr()`, `trc()`, `tr_format()` or `N_()`. Every new string gets its entry in `data/lang/hu.po`; the test `ui.translation_coverage` fails otherwise. The monitor, MCP, `--help` and stderr stay English.
- **Comments:** one line at most, and only where the code is not self-evident. No history or narrative in comments.
- **Documentation** is in English and describes the current state: no change history, no "was added in". Hardware and reverse-engineering facts carry their evidence (source document and page, ROM address, data sheet, or "checked against WinTVC 1.4.1") and the markers ✓ verified, ❓ unknown, ⚑ needs a check.
- **UI design:** no stock Dear ImGui look and no generic dark-theme templates. The GUI uses the TVC's own materials and its 16 colours with fixed meanings ([ui.md](docs/design/ui.md#design-direction)); every new screen, colour and grouping needs a reason.
- **Verify, don't assume.** Compiling, starting and exiting with code 0 do not mean a feature works. Check the behaviour end to end through the real path: for the GUI, drive the real window with real X input on a virtual display and read a screen grab ([developer guide, "Real-GUI checks"](docs/developer-guide.md#real-gui-checks)). If something could not be checked, say exactly what was and was not checked.

## Key facts

- TVC: Z80A at 3.125 MHz, IM 1, MC6845; a frame is 314 lines × 200 T = 62 800 T (49.76 Hz); VRAM accesses are clock-stretched at 160 ns resolution ([tvc-hardware.md](docs/hardware/tvc-hardware.md)).
- Presets: 32k, 32k + 32k, 64k, 64k+ boards with SYS 1.2, 2.1 or 2.2 and EXT 1.x or 2.x ([machine.md](docs/design/machine.md#presets)).
- Ports: text monitor 6580, MCP over HTTP 6584, DZRP 6582, GDB 6583, all on 127.0.0.1 ([debug-and-remote.md](docs/design/debug-and-remote.md#ports-and-numbers)).
