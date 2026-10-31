---
name: freetvc-mac
description: >
  freeTVC (Videoton TV-Computer emulator, this repo) on macOS / Apple Silicon: building it with the Mac script
  (no ctest!), starting the GUI, loading .cas/.dsk programs, driving and inspecting the running machine through its
  MCP server with curl (screenshots, typing, memory, registers, I/O, breakpoints), and measuring timing (real-time
  speed, RAM/VRAM T-states, frame length). Use it whenever the user wants freeTVC built, started, tested, compared
  with the real TVC, or used to check a TVC program (SIDSoftEMU, WaterPumpGame and others) on the Mac.
---

# freeTVC on the Mac

The state of the Mac port and what was checked: `MAC_ALLAPOT.md` in the repo root (Hungarian). Upstream rules for code
changes: `AGENTS.md`. The user speaks Hungarian; answer in Hungarian, briefly.

## Rules from the user
- **Never run the automatic tests** (`ctest`, `tests/`): the Mac build turns them off (`FREETVC_BUILD_TESTS=OFF`).
- Git is Claude's job; the user never runs git. Repo: https://github.com/LaM3rzLG/FreeTVC-Mac (public, remote `mac`,
  local `main` tracks it; `origin` is the private upstream LaM3rzLG/FreeTVC — do not push there).
- **Commit author is always `Deybacsi <deyprof@gmail.com>`** (author and committer):
  `GIT_COMMITTER_NAME=Deybacsi GIT_COMMITTER_EMAIL=deyprof@gmail.com git -c user.name=Deybacsi -c user.email=deyprof@gmail.com commit ...`
- No ROMs in git (`.gitignore`: `*.rom`, `/roms/`, `/ROMS/`; the `.bin` ROMs live only in ignored folders).
- The user tests in the GUI; commit a GUI change only after the user confirms it.

## Build
```sh
bash tools/mac/build_mac.sh        # brew cmake+ninja if missing; Release; ROMs fetched at build time
```
Output: `build/mac-release/src/freetvc` and `freetvc-headless`, with `roms/ fonts/ skins/ keymaps/ pictures/ lang/
about/` beside them (move the whole folder, never the binary alone). The CMake presets are Linux/Windows only.

## Run
- GUI: `build/mac-release/src/freetvc` (run in the background). Bring it forward:
  `osascript -e 'tell application "System Events" to set frontmost of (first process whose unix id is PID) to true'`.
- Programs: Program menu → *Load program...* (`.cas`, `.dsk`, ...). A `.dsk` inserts the HBF disk card, puts the
  disk in A: and runs its first CAS file. Games that read data files (WaterPumpGame) run only from `.dsk`.
- ROM search: `--rom-dir`, `FREETVC_ROM_DIR`, `rom_dir` in `~/Library/Application Support/freeTVC/freetvc.ini`,
  `roms/` next to the program, `roms/` in the user data folder. *Download ROMs* fills `rom_dir` if set.
- Headless check without a window:
  `freetvc-headless --preset 64k+-2.2 --autostart FILE --frames N --screenshot out.ppm` (PPM; `sips -s format png`).
  Copy a `.dsk` to the scratchpad first (disk images are written in place).

## MCP (drive the user's running GUI)
- Start: `build/mac-release/src/freetvc --mcp-http --mcp-token claudeteszt` (port 6584, 127.0.0.1 only).
- `tools/mac/mcp.sh TOOL 'JSON' [image.png]` (env `FREETVC_MCP_TOKEN`, `FREETVC_MCP_PORT`).
- Tested tools: `machine_info`; `type_text {"text":" \n10 PRINT 1\nRUN\n"}` (first key goes to the logo);
  `screenshot {} out.png`; `screen_text`; `memory {"action":"read","address":"0xC000","length":16}`;
  `disassemble {"address":"0xCAA4","count":6}`; `io_state` (sound.amplitude = DAC level, sound.pitch,
  video.border); `media {"action":"autostart","path":"/abs/x.dsk"}`; `run {"frames":N}` / `{"until":"600D"}`
  (returns `tstates_run`); `press_keys {"keys":["2"],"frames":5}` (an array); `load_binary {"path","address",
  "jump":true}` (`VID0:0100` style banks work); `breakpoints {"action":"add","kind":"irq"}`; `control
  {"action":"pause|resume|reset|power_cycle"}`.
- `run`, `step`, `type_text`, `press_keys` leave the machine paused: send `control resume` afterwards.
- Sound IRQ rate from `io_state` pitch P: 3 125 000 / 16 / (4096 − P) Hz (SIDSoftEMU: P = 0FD9h → 5008 Hz).

## Timing checks
- `python3 tools/mac/mcp_pace.py 5 12`: real-time speed of the running GUI (audio-paced).
- `python3 tools/mac/mcp_cycles.py`: RAM/VRAM loop T-states, code in VRAM, frame length; it overwrites 6000h and
  power-cycles the machine — tell the user first.
- Results 2026-10-31 (M4, macOS 27): 99.97 % (3.1240 MHz, 49.73 frames/s) over 60 s, 100 % with the window hidden,
  85–90 % only in the first ~10 s after start; RAM loop 20.0 T, VRAM read/write loop 22.0 T and NOP+DJNZ in VRAM
  20.0 T (both exactly as `docs/hardware/tvc-hardware.md` section 2 predicts), frame 62 800 T. Not yet compared with a
  real TVC.

## Mac fixes in this fork
- Retina: UI scale = display scale / pixel density (`ui_scale()` in `src/frontend/sdl/main.cpp`); before it every
  text was twice as large.
- *Download ROMs* writes into `rom_dir` when one is set (`RomDownload::dir`, texts in `play_view.cpp`, `hu.po`,
  `docs/user-guide.md`, `docs/design/ui.md`).
