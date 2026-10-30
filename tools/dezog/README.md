# tools/dezog: DeZog checks for the DZRP server and the GDB stub

These files check freeTVC's DZRP server (`--dzrp`, `--dzrp-banks`, port 6582) and GDB stub (`--gdb`, port 6583) with [DeZog](https://github.com/maziac/DeZog), the Z80 debugger extension for VS Code. The servers are described in [debug-and-remote.md](../../docs/design/debug-and-remote.md) (sections "DZRP server" and "GDB stub"). The check is interactive: DeZog runs in a VS Code window, and `x_drive.py` drives both windows with real X input.

## Two memory views

- **Flat ZX48K** (`--dzrp`): works with any DeZog that has the generic `dzrp` remote. DeZog accepts only ZX Spectrum memory models from a DZRP remote, so freeTVC reports a ZX48K and shows a flat view of the current paging.
- **TVC banks** (`--dzrp-banks`): needs DeZog with our patch `dezog-dzrp-custom-memory.patch`. freeTVC then reports machine type 0 (UNKNOWN), and the patched DeZog takes the TVC memory layout from `dzrp.customMemory` in `launch.json`.
  - Five slots: 0000h, 4000h, 8000h, C000h and E000h. Page 3 is split at E000h, where IOMEM splits it.
  - The bank numbers are freeTVC's `Bank` values: SYS 0, CART 1, EXT 2, U0–U3 3–6, VID0–VID3 7–10, CARD0–CARD3 11–14, OPEN 15.

The generic `dzrp` remote first appears in DeZog 3.8.0. The pinned commit `715f2b35b46e8ef02cffaf65f57d649ba3711a40` (version 3.8.0-rc7, 2026-10-18) is not a release, so DeZog is built from source.

## Files

| File | Purpose |
|---|---|
| `project/hello.asm` | test program at 6000h: prints `HELLO FROM DEZOG` through the OS (`RST 30h`, function 21h), then counts in a loop (the counter byte is at 602Ah) |
| `project/.vscode/launch.json` | three configurations: "freeTVC (DZRP, TVC banks)", "freeTVC (GDB stub)", "freeTVC (DZRP, flat ZX48K)" |
| `dezog-dzrp-custom-memory.patch` | the DeZog change against commit `715f2b35`: code, settings schema, documentation and four unit tests. It is kept only here; it was not submitted to DeZog |
| `LICENSE.DeZog.txt` | DeZog's MIT licence, which covers the patch's context lines |
| `build_dezog.sh` | builds a patched DeZog in a git worktree of a DeZog clone, runs the patch's unit tests and bundles the extension |
| `run_dezog.sh` | copies `project/` into a work folder, assembles it with sjasmplus (`hello.bin`, `hello.sld`), and opens it in VS Code with DeZog loaded as a development extension |
| `x_drive.py` | real X input (XTEST) into already running windows found by title, and window grabs |

`hello.asm` uses `DEVICE ZXSPECTRUM48`. With a custom memory model, DeZog passes `NOSLOT64K` banks through unchanged, which would put every label into bank 0 (SYS). ZX48 labels go onto the slots' initial banks instead (U0, U1, U2, SYS).

## Prerequisites

- A freeTVC build, here the `gcc-release` preset (`build/gcc-release/src/freetvc`), and the system ROMs (for example `testdata/roms/`, see [developer-guide.md](../../docs/developer-guide.md)).
- git, VS Code (`code` on `PATH`), and Node.js 22 with npm. Node 22 must be the first `node` on `PATH`, or set `NODE22_BIN` to its `bin` folder (`build_dezog.sh` puts it first on `PATH`).
- sjasmplus 1.24.0 ([github.com/z00m128/sjasmplus](https://github.com/z00m128/sjasmplus), tag `v1.24.0`). `run_dezog.sh` takes it from `SJASMPLUS`, else `PATH`, else `.tools/sjasmplus-1.24.0/bin/sjasmplus` under the project root (`.tools/` is git-ignored). One way to put it there:

  ```sh
  cmake -S <sjasmplus source> -B <build folder> -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX=$PWD/.tools/sjasmplus-1.24.0
  cmake --build <build folder> && cmake --install <build folder>
  ```

- For `x_drive.py`: an X11 session with a window manager, `wmctrl`, and Python 3 with python-xlib and Pillow.

## Build DeZog

Clone DeZog into a folder of your choice, then build the patched copy into another folder (for example under the git-ignored `.tools/`):

```sh
git clone https://github.com/maziac/DeZog.git <DeZog clone>
tools/dezog/build_dezog.sh <DeZog clone> .tools/dezog-tvc
```

`build_dezog.sh` adds a detached git worktree at the pinned commit (sparse checkout without DeZog's large `releases/` folder), applies the patch, then runs `npm ci --ignore-scripts`, `npx tsc`, the patch's tests (`npx mocha -u tdd out/tests/dzrpgenericremote.tests.js`), `npm run esbuild-node` and `npm run esbuild-browser`. The clone must contain the pinned commit (a `--depth 1` clone of a later master does not).

An unpatched DeZog (flat ZX48K and the GDB stub only): in a clone at the same commit, run `npm ci --ignore-scripts`, `npm run esbuild-node` and `npm run esbuild-browser`.

## Run the check

From the project root:

```sh
build/gcc-release/src/freetvc --rom-dir testdata/roms --no-audio --dzrp-banks --remote-monitor &
python3 -I tools/dezog/x_drive.py <out dir> win '^freeTVC' wait 6 key space wait 1.5 grab prompt
tools/dezog/run_dezog.sh .tools/dezog-tvc <work dir>
python3 -I tools/dezog/x_drive.py <out dir> win 'Extension Development Host' wait 4 click 127 139 wait 1 key F5 wait 7 grab started
```

The first `x_drive.py` call presses SPACE past the start-up logo and grabs the BASIC prompt. The second focuses the VS Code window, clicks into it and presses F5, which starts the selected configuration: the first one, "TVC banks". The click position depends on the VS Code window layout; read it off a grab.

Other configurations:
- flat ZX48K: start freeTVC with `--dzrp` and select "freeTVC (DZRP, flat ZX48K)" in the Run and Debug view;
- GDB stub: start freeTVC with `--gdb` and select "freeTVC (GDB stub)" (DeZog's `gdb` remote; any DeZog 3.8 build).

Then, in VS Code:
- step with F10 / F11 / Shift+F11, set breakpoints with F9, continue with F5, pause with F6;
- add a watchpoint in the debug console: `-wpadd 0x602a 1 w`;
- dump a bank that is not paged in: `-md bank=2 0 16` (EXT);
- see the paging in Variables → Memory Banks.

[debug-and-remote.md](../../docs/design/debug-and-remote.md) lists what was checked this way.

`run_dezog.sh` gives VS Code its own `--user-data-dir` and `--extensions-dir` inside the work folder, so your own VS Code profile is not touched. Its settings turn off telemetry and updates and turn on DeZog's logs. The DZRP traffic is logged to `<work dir>/user/logs/*/window1/exthost/output_logging_*/3-DeZog Transport.log`.

## x_drive.py steps

`python3 -I tools/dezog/x_drive.py <out dir> STEP...`

| Step | Does |
|---|---|
| `win REGEX` | chooses the visible window whose title matches (waits up to 20 s) |
| `wait S` | sleeps S seconds |
| `grab NAME` | saves the chosen window as `<out dir>/NAME.png` |
| `key A[+B…]` | presses the keysyms together, then releases them (`key F5`, `key Shift_L+F11`) |
| `type TEXT` | types TEXT key by key; letters, digits, space and `,."=-:/_*()` |
| `click X Y` | left click at window pixel X, Y |

Keys go only to the chosen window: `x_drive.py` activates it with `wmctrl` and waits until it is the active window. A keysym that sits on the shifted level of its key in the current X keyboard layout (for example `=` on a Hungarian layout) is sent with Shift.

## Known DeZog limits

- Unpatched DeZog 3.8.0-rc7 offers only the flat ZX48K view, and its "Break on Interrupt" checkbox does nothing (the command is never sent to the remote). The patch fixes both.
- Paging cannot be changed from DeZog: freeTVC does not support DZRP's `SET_SLOT`.
