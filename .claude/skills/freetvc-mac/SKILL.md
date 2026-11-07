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
- Git is Claude's job; the user never runs git. Repo: https://github.com/LaM3rzLG/FreeTVC (public, remote `origin`,
  local `main` tracks it) - everything goes here (user, 2026-11-01). FreeTVC-Mac (remote `mac`) is no longer used; no new repos.
- **Commits go in the user's name: `LaM3rzLG <lorinczg77@gmail.com>`** (author and committer; set in the repo's git config,
  user's request 2026-11-01). Older commits are by Deybacsi; leave them as they are.
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

## 2dfx card (branch `2dfx`)
- `--2dfx [SLOT]` (GUI and headless, default slot 2), Cards menu, ini `twodfx`; not with the TVC256k++.
- Test programs in the ignored `2DFXCAS/` (cube, wolf, anyshow; from tvc.homeserver.hu `dl.php?file=Cube_2dfx.zip`,
  `Wolf3d_2dfx.zip`, `AnyShow2dfx.zip`). Headless: `--preset 64k+-2.2 --2dfx --autostart COPY/Wolf2dfx.cas --frames 3000`
  (copy the folder to the scratchpad first). Raw `.bin` data files load through the host cassette and the NanoSD.
- NanoSD in the GUI: SD folder = the program's folder, ESC to BASIC, `LOAD"WOLF2DFX"`, `RUN`. Headless typing: start
  with `\n\n` (the first keys are lost), e.g. `--key ESC@400 --type '\n\nLOAD"WOLF2DFX"\n@650' --type 'RUN\n@1700'`.

## GUI windows, video, release (2026-11-06)
- Start the GUI in the background, then move it to the external monitor (the user wants it there):
  `osascript -e "tell application \"System Events\" to tell (first process whose unix id is PID) to set position of window 1 to {-900, -1340}"`.
  Restart after a build: `pkill -f 'mac-release/src/freetvc --preset'`, then start again.
- Video: `freetvc-headless --video OUT.mp4` (after `--autostart` from the program's start); GUI: REC key in the strip.
  Needs `ffmpeg/ffmpeg` next to the program: `sh tools/release/build_ffmpeg.sh native build/ffmpeg-out-macos-arm64` and
  copy it to `build/mac-release/src/ffmpeg/`. Check a video: `ffmpeg -i X` (stream info), `qlmanage -t -s 700 -o DIR X`
  (a frame via macOS), sound: `afconvert -f WAVE -d LEI16 X out.wav`.
- Release (only on request; done this way for 1.2.0, 2026-11-07): version in CMakeLists.txt + a `## X.Y.Z — date`
  section in CHANGELOG.md and CHANGELOG.hu.md (with the Downloads table); commit, fast-forward `main`, push; tag
  `vX.Y.Z` (annotated, `git -c user.name=LaM3rzLG -c user.email=lorinczg77@gmail.com tag -a`) and push it: the tag
  starts `release.yml` (Linux + Windows + ffmpeg/x264 sources, ~8 min; `gh run watch ID`, then
  `gh run download ID -n freeTVC-packages -D build/release-X.Y.Z/actions`). Meanwhile
  `sh tools/mac/make_mac_release.sh build/release-X.Y.Z` (both macOS zips + SHA256SUMS-macos). Check `shasum -a 256 -c`
  on both sums and unzip one package (`freetvc-headless --version`). Notes = English section + Quick start, `---`,
  `### Magyarul` + Hungarian section + Gyors kezdés (see the v1.2.0 release body); `gh release create` from the user's
  account with the four packages, both SHA256SUMS and the `ffmpeg-*.tar.xz` / `x264-*.tar.bz2` sources.
- UPM test: see TERVEK.md (UPM.FDR + the EFCA95FC cartridge, `--key RETURN@250`).

## Checking the GUI without clicks
- osascript cannot click (no Accessibility permission). To see a GUI change working, start it with
  `--mcp-http --mcp-token claudeteszt` and drive it with `tools/mac/mcp.sh` (screenshot, press_keys, io_state,
  machine_info frame count for real-time speed); `screencapture -x -R-900,-600,1000,52 out.png` grabs the strip of a
  1000 × 792 window at {-900, -1340}. Menus cannot be opened this way: say so and let the user look.
- Visual changes: an HTML preview first (scratchpad, `open` it; render with headless Chrome
  `--headless=new --screenshot` to check it), build only after the user picks.
- Card checks headless: `--card256 --card256-usb DIR --autostart Wolf256.cas` (dikdom's Wolf in
  `~/Documents/TVC-256K++/256k++ programok/Wolf`, copy it first), `--2dfx --autostart Wolf2dfx.cas` for comparison.
  Test files: `~/Downloads/TVC/TVC_CEmu/cas/*.cas`, `~/Documents/TVC Projects/*/*.dsk`.

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
- The GUI of 1.2.0 (strip, menus, start picture, recent programs, storage cards) is described in
  `docs/user-guide.md` section 3; hand-off state and open questions in TERVEK.md ("FOLYAMATBAN: GUI-átalakítás").
