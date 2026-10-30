# Developer guide

This guide takes a developer (or an AI coding agent) from a fresh clone of freeTVC to a working build, passing tests and a first change. It lists the prerequisites, every build and test command, the test data and where to get it, the repository layout, the coding rules, and step-by-step recipes for extending the emulator.

All commands run from the repository root unless a step says otherwise. Commands marked ❓ could not be confirmed on a clean machine.

## Contents

- [Prerequisites](#prerequisites)
- [Getting the source and building](#getting-the-source-and-building)
- [ROMs](#roms)
- [Running the tests](#running-the-tests)
- [Test data](#test-data)
- [Repository layout](#repository-layout)
- [Coding rules](#coding-rules)
- [Your first change](#your-first-change)
- [Extending freeTVC](#extending-freetvc)
- [Verifying changes](#verifying-changes)
- [Making a release](#making-a-release)
- [Debugging the emulator with its own tools](#debugging-the-emulator-with-its-own-tools)
- [Where to read next](#where-to-read-next)

## Prerequisites

### What the build needs

| Need | Version | Where it is set |
|---|---|---|
| CMake | 3.25 or newer | `cmake_minimum_required(VERSION 3.25)` in `CMakeLists.txt`; `CMakePresets.json` is format version 6 |
| Ninja | any recent | every preset uses the Ninja generator |
| C and C++ compiler | C11 and C++20 | `CMAKE_C_STANDARD 11`, `CMAKE_CXX_STANDARD 20`, extensions off |
| Python 3 | 3.x, standard library only | tests `remote.png` and `ui.translation_coverage` (skipped from the test list when CMake finds no interpreter) |
| Network at the first configure | | SDL3 source tarball and the GUI fonts (see [Offline builds](#offline-builds)); the optional ROM autofetch |

The tested combination is an Ubuntu 24.04-based Linux with GCC 13.3, Clang 18.1, CMake 3.28.3, Ninja 1.11.1 and Python 3.12. Older compilers are not tested ❓: Ubuntu 22.04's CMake 3.22 is too old for the presets.

### Linux (Ubuntu, Debian, Linux Mint)

The build and the headless runner:

```sh
sudo apt install build-essential cmake ninja-build git curl python3 pkg-config unzip
```

- `curl` is used by the programs' own ROM download (`--fetch-roms`, the GUI's download box) and by the test-data commands below; `unzip` only by the test-data commands.
- `clang` adds the `clang-debug` preset.

The GUI builds SDL3 from source, so it needs SDL's development packages. The list below follows SDL's own Linux build list (`docs/README-linux.md` in the SDL 3.4.18 source, "Build Dependencies") for the features freeTVC uses:

```sh
sudo apt install libx11-dev libxext-dev libxcursor-dev libxi-dev libxfixes-dev libxrandr-dev libxss-dev libxtst-dev \
    libgl-dev libegl-dev \
    libwayland-dev libxkbcommon-dev wayland-protocols libdecor-0-dev \
    libasound2-dev libpulse-dev libpipewire-0.3-dev \
    libudev-dev libdbus-1-dev libibus-1.0-dev
```

| Packages | What they give | Without them |
|---|---|---|
| `libx11-dev libxext-dev` | SDL's X11 video driver | X11 is left out silently; on an X11 desktop the GUI then has no window ❓ |
| `libxcursor-dev libxi-dev libxfixes-dev libxrandr-dev libxss-dev libxtst-dev` (Xdbe, XShape and XSync come with `libxext-dev`) | the X11 extensions SDL enables by default, XTEST included | **the configure stops** once X11 is found: SDL 3.4 reports `Couldn't find dependency package for XTEST. Please install the needed packages or configure with -DSDL_X11_XTEST=OFF` (and the same for each extension). freeTVC itself does not use XTEST, so `-DSDL_X11_XTEST=OFF` also works |
| `libgl-dev libegl-dev` (`libgl1-mesa-dev` pulls in the first) | SDL's OpenGL, which the GUI's OpenGL 3.3 renderer and picture model use | SDL is built without OpenGL; the GUI is meant to fall back to SDL's renderer (`--renderer sdl`) ❓ not checked with a build that lacks the headers |
| `libwayland-dev libxkbcommon-dev wayland-protocols libdecor-0-dev` | SDL's Wayland video driver | Wayland is left out; X11 (or XWayland) still works |
| `libasound2-dev libpulse-dev libpipewire-0.3-dev` | ALSA, PulseAudio and PipeWire audio | no sound device opens; the GUI then paces itself by the wall clock, silent (as with `--no-audio`) |
| `libudev-dev` | gamepad hot-plugging | gamepads may not be found ❓ |
| `libdbus-1-dev` | the desktop portal (file dialogs) and screensaver control | file dialogs need another driver, such as `SDL_FILE_DIALOG_DRIVER=zenity` ❓ |
| `libibus-1.0-dev` | input methods | none that freeTVC needs |

Debian 12 has the same package names and CMake 3.25.1, but freeTVC has not been built there ❓.

### Windows

- **Tested path: the MinGW-w64 cross build from Linux** (preset `mingw-release`). Install `mingw-w64` (Ubuntu's GCC 13 MinGW-w64 with its default win32 thread model was used). The toolchain file `cmake/toolchains/mingw-w64-x86_64.cmake` expects `x86_64-w64-mingw32-gcc`, `-g++` and `-windres` and the sysroot `/usr/x86_64-w64-mingw32`. The programs are linked with `-static`, so the `.exe` files need no DLLs next to them.
- The `.exe` files have been run under Wine on Linux (tests and the GUI). A run on real Windows is not documented ❓.
- **A native Windows build is not tested.** **Developing on Windows** (MSYS2 with GCC, Visual Studio with the `msvc-*` presets, or WSL 2), with the ROMs, the test data in PowerShell, running and debugging there, and what to expect from MSVC: [windows.md](windows.md).

### macOS

Not supported and not tested. There is no macOS preset (the Linux presets are conditioned on a Linux host), and nothing has been built on macOS. The code has a macOS user-data path (`~/Library/Application Support/freeTVC`), never exercised.

### Optional tools

| Tool | Package or source | Used for |
|---|---|---|
| sjasmplus 1.24.0 | built from https://github.com/z00m128/sjasmplus (tag `v1.24.0`, `make` in its source tree per its README ❓) | test `debug.disasm_sjasmplus` (the disassembler round trip; skipped without it), `tools/mcp/stress_check.py`, the DeZog and GDB checks. CMake finds it with `find_program` on `PATH` and in `.tools/sjasmplus-1.24.0/bin/` (git-ignored); or pass `-DFREETVC_SJASMPLUS=/path/to/sjasmplus` |
| SDCC | `sdcc` | the SDCC part of `tools/mcp/stress_check.py` (source-level sessions); the tests use committed fixtures and do not need it |
| gettext | `gettext` (`msgmerge`, `msgfmt`) | updating `data/lang/*.po` ([tools/lang/README.md](../tools/lang/README.md)) |
| Wine | `wine` | running the MinGW `.exe` tests and programs on Linux |
| Virtual display and X input | `xvfb metacity wmctrl xdotool zenity dbus python3-xlib python3-pil` | real-input GUI checks with `tools/ui/ui_drive.py` ([Real-GUI checks](#real-gui-checks)) |
| Node.js 22 | the official binaries from https://nodejs.org (Ubuntu 24.04's Node 18 is too old) | the MCP Inspector check `tools/mcp/inspector_check.sh` |
| GNU gdb for the Z80 | built from GNU's source for `z80-unknown-elf` ([tools/gdb/README.md](../tools/gdb/README.md)) | the GDB stub check |
| DeZog | built from source ([tools/dezog/README.md](../tools/dezog/README.md)) | the DZRP and GDB checks in VS Code |

## Getting the source and building

```sh
git clone https://github.com/LaM3rzLG/FreeTVC.git freeTVC
cd freeTVC
```

### Configure and build with a preset

```sh
cmake --list-presets
cmake --preset gcc-release
cmake --build --preset gcc-release
```

| Configure preset | Host | Compiler, type | Test preset |
|---|---|---|---|
| `gcc-debug` | Linux | `gcc`/`g++`, Debug | `gcc-debug` |
| `gcc-release` | Linux | `gcc`/`g++`, Release | `gcc-release`, and `gcc-release-slow` for ZEXDOC/ZEXALL |
| `clang-debug` | Linux | `clang`/`clang++`, Debug | `clang-debug` |
| `mingw-release` | Linux | MinGW-w64 cross build for Windows x64, Release | none (the tests are Windows programs: run them under Wine) |
| `msvc-debug`, `msvc-release` | Windows | MSVC `cl`, x64 | `msvc-debug`, `msvc-release` (not tested) |

Every preset builds into `build/<preset>/` (git-ignored) and writes `compile_commands.json` there.

The first build of SDL3 prints `WARNING: XML failed validation against built-in DTD` for some of SDL's Wayland protocol files (`wayland-scanner` output); it is harmless and comes from SDL, not from freeTVC.

### Where the programs land

| Path | What |
|---|---|
| `build/<preset>/src/freetvc` | the GUI (`freetvc.exe` for MinGW, a Windows GUI-subsystem program) |
| `build/<preset>/src/freetvc-headless` | the headless runner (`freetvc-headless.exe`) |
| `build/<preset>/src/fonts/`, `skins/`, `keymaps/`, `pictures/`, `lang/` | copied next to the GUI after each build: the fonts from the configure download, and `data/skins`, `data/keymaps`, `data/pictures`, `data/lang` (each folder is copied again whenever one of its files changes) |
| `build/<preset>/src/roms/` | only with `-DFREETVC_FETCH_ROMS=ON` ([ROMs](#roms)) |
| `build/<preset>/tests/` | the test programs |

There is no install step. To move the GUI elsewhere, copy the program together with those folders. A `freetvc.ini` next to the program makes it portable (settings are then read from and written there instead of the user data folder).

Quick checks after a build:

```sh
build/gcc-release/src/freetvc-headless --version
build/gcc-release/src/freetvc-headless --help
```

The first prints `freeTVC 1.0.1` (the project version in `CMakeLists.txt`).

### Build options

| Option | Default | Effect |
|---|---|---|
| `FREETVC_BUILD_GUI` | `ON` | the SDL3 + Dear ImGui GUI. `OFF` builds the headless runner and the tests only: no SDL3, no fonts, no network at configure, and no `ui.*` or `input.host` tests |
| `FREETVC_BUILD_TESTS` | `ON` | the test programs and CTest registration |
| `FREETVC_WERROR` | `OFF` | warnings in freeTVC's own targets become errors (`-Werror`, `/WX`) |
| `FREETVC_FETCH_ROMS` | `OFF` | the ROM autofetch at configure time ([ROMs](#roms)) |
| `FREETVC_USE_SYSTEM_SDL3` | `OFF` | use an installed SDL3 (`find_package(SDL3 3.2 CONFIG)`) instead of fetching it. Ubuntu 24.04 has no SDL3 package ❓ |
| `FREETVC_SDL3_URL` | the GitHub release tarball | where to fetch SDL3 from; may be a `file://` URL |
| `FREETVC_FONT_DIR` | empty | a local folder with the font files instead of downloading them |

Pass them at configure time, for example `cmake --preset gcc-release -DFREETVC_WERROR=ON`.

### What is fetched at configure time

- **SDL3 3.4.18** (zlib licence): `https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz`, SHA-256 `9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3`, through CMake's `FetchContent` into `build/<preset>/_deps/`, built as a static library (`third_party/CMakeLists.txt`). Only with the GUI.
- **Fonts**: Atkinson Hyperlegible Next (Regular, Medium) and Atkinson Hyperlegible Mono (Regular) with their SIL OFL 1.1 texts, from pinned commits of `googlefonts/atkinson-hyperlegible-next` and `googlefonts/atkinson-hyperlegible-next-mono` on `raw.githubusercontent.com`, each checked by SHA-256 (`cmake/FreeTVCFonts.cmake`), into `build/<preset>/fonts/`. Only with the GUI.
- **ROMs**, only with `-DFREETVC_FETCH_ROMS=ON`.

Nothing else is downloaded. Dear ImGui, nlohmann/json, FatFs, littlefs, ymfm, reSID and floooh's `z80.h` are vendored in `third_party/` ([THIRD_PARTY.md](../THIRD_PARTY.md)).

### Offline builds

- SDL3: download the tarball once and pass it as a file URL; the SHA-256 is checked either way:

  ```sh
  cmake --preset gcc-release -DFREETVC_SDL3_URL=file:///absolute/path/SDL3-3.4.18.tar.gz
  ```

  Alternatively `-DFREETVC_USE_SYSTEM_SDL3=ON` with an installed SDL3 3.2 or newer, or CMake's own `-DFETCHCONTENT_SOURCE_DIR_SDL3=<unpacked SDL3 source>`.
- Fonts: `-DFREETVC_FONT_DIR=<dir>`, where `<dir>` holds exactly `AtkinsonHyperlegibleNext-Regular.ttf`, `AtkinsonHyperlegibleNext-Medium.ttf`, `AtkinsonHyperlegibleMono-Regular.ttf`, `OFL-Next.txt` and `OFL-Mono.txt` (the two upstream `OFL.txt` files renamed). The SHA-256 of each is checked. The easiest copy is `build/<preset>/fonts/` of an earlier online configure; the URLs and hashes are in `cmake/FreeTVCFonts.cmake`.
- Or configure with `-DFREETVC_BUILD_GUI=OFF`, which needs no download at all.

A later configure of the same build tree does not download again: the SDL3 source stays in `_deps/`, and font files whose hash matches are kept.

## ROMs

The emulator needs the TVC's system ROMs; it does not include them, and **ROMs are never committed** (`.gitignore` ignores `*.rom`, `/testdata/` and `/build*/`). They are copyright Videoton / IS Systems; the expansion-card firmware belongs to its authors under the licences named in `ROMS-README.txt` (written next to the fetched files).

### What each preset and card needs

| File | What | Needed by |
|---|---|---|
| `sys12.bin` + `ext1x.bin` | SYS 1.2 and EXT 1.x | presets `32k-1.2`, `32k+hbm-1.2`, `64k-1.2` |
| `sys21.bin` + `ext2x.bin` | SYS 2.1 and EXT 2.x | preset `64k+-2.1` |
| `sys22.bin` + `ext2x.bin` | SYS 2.2 and EXT 2.x | preset `64k+-2.2` (the GUI's default) |
| `hbf.rom`, `hbf12.rom` | HBF disk card EPROM with VT-DOS 1.1, with VT-DOS 1.2 | `--disk-card` (`--disk-rom 1.1` or `1.2`) |
| `vtdos11.rom`, `vtdos12.rom` | VT-DOS 1.1 and 1.2 cartridges | `--cart` |
| `gamecard.rom` | the GameCard's EPROM | `--gamecard` |
| `multichip.rom` | the Multichip sound card's EEPROM | `--multichip` |
| `nanosd.rom` | the NanoSD's EEPROM | `--nanosd` |
| `multicart-en.rom`, `multicart-hu.rom`, `multicart-es.rom` | MultiCart v2 firmware 2.1.14 | the MultiCart image builder (`--multicart-build`) |
| `sddual.rom`, `sddual-gm.rom` | SD Dual firmware 2022.11.09, plain and gamer mode | `--sddual`, `--sddual-gm` |
| `card256.rom`, `card256-font.bin`, `card256-picture.bmp`, `card256-flash.bin` | the TVC256k++'s ROM, power-on font and picture, flash drive image | `--card256` |

The list, with every piece's URL and SHA-256 and every joined image's SHA-256 and CRC-32, is `data/roms.txt`. The pieces come from tvc.homeserver.hu, from the card authors' GitHub repositories and from the Internet Archive. The site's TLS certificate chain is incomplete, so the downloads do not verify TLS; the pinned SHA-256 of every piece and image vouches for the bytes.

### Three ways to get them

All three read `data/roms.txt` (it is also compiled into the programs):

1. **Into any folder with CMake's script mode** (the one the tests use):

   ```sh
   cmake -DFREETVC_ROM_OUT=$PWD/testdata/roms -P cmake/FreeTVCRoms.cmake
   ```

2. **At configure time**: `cmake --preset gcc-release -DFREETVC_FETCH_ROMS=ON` downloads into `build/<preset>/rom-downloads/`, joins the images into `build/<preset>/roms/`, and a post-build step copies that folder as `roms/` next to `freetvc-headless` and `freetvc`.
3. **From the programs**, through the system `curl`: `freetvc-headless --fetch-roms` writes into `--rom-dir` if given, else `roms/` in the user data folder, then exits; the GUI offers a download when it finds no ROMs at start, and has a "Download ROMs" item in the Machine menu.

   ```sh
   build/gcc-release/src/freetvc-headless --fetch-roms --rom-dir testdata/roms
   ```

EXT 1.x arrives as its 8 KiB socket image; the programs turn it into the 4 KiB ROM when they load it (`normalise_ext()` in `src/frontend/common/presets.cpp`).

### The ROM search

Both programs look in these folders, first match wins (`rom_places()` in `src/frontend/common/presets.hpp`):

1. `--rom-dir DIR`;
2. the environment variable `FREETVC_ROM_DIR`;
3. `rom_dir` in `freetvc.ini`;
4. `roms/` next to the program;
5. `roms/` in the user data folder: `$XDG_DATA_HOME/freeTVC/roms` (else `~/.local/share/freeTVC/roms`) on Linux, `%APPDATA%\freeTVC\roms` on Windows.

In each folder a file is taken by its expected name, else any file whose CRC-32 is a known image of that ROM (the table `kKnownRoms` in `src/frontend/common/presets.cpp`). Card ROMs are found the same way. When nothing is found, the error names every place searched.

The environment variable `FREETVC_ROM_DIR` is for the programs; the CMake cache variable of the same name is for the tests ([Test data](#test-data)).

Smoke run from the fetched ROMs, reading the BASIC prompt back from a screenshot:

```sh
build/gcc-release/src/freetvc-headless --preset 64k+-2.2 --rom-dir testdata/roms --frames 900 \
    --key SPACE@330 --type 'PRINT 6*7\n@400' --screenshot basic.ppm
```

## Running the tests

Tests are plain programs registered with CTest, without a test framework: exit 0 passes, any other code fails, and **77 means skipped** (its test data is missing).

```sh
ctest --preset gcc-release                 # every test except the "slow" label
ctest --preset gcc-release-slow            # ZEXDOC and ZEXALL only (needs the gcc-release build)
ctest --preset gcc-debug -L rom            # one label
ctest --preset gcc-debug -R '^core\.bus$'  # one test
ctest --preset gcc-debug -j 8              # in parallel
ctest --preset gcc-debug -N                # list without running
```

The test presets print the output of failing tests. With the full test data and the GUI built, the fast set has 109 tests and the slow set 2.

| Label | Tests | Needs |
|---|---|---|
| `core` | the unit tests of the core, debug, remote, front-end helpers and UI data (40) | nothing, except `debug.disasm_sjasmplus` (sjasmplus) and `remote.png`, `ui.translation_coverage` (Python 3) |
| `rom` | boots, typing, sound, media, snapshots, cards and the command line on the real ROMs (52) | `testdata/roms`, some also other test data |
| `golden` | frames compared pixel by pixel with WinTVC 1.4.1's (16) | `testdata/golden` |
| `z80` | the SingleStepTests subset, ZEXDOC, ZEXALL (3) | `testdata/sst-z80/v1`, `testdata/zex` |
| `slow` | `z80.zexdoc`, `z80.zexall`: several minutes each (timeout 3600 s) | `testdata/zex` |

Without any test data every `rom`, `golden` and `z80` test reports *Skipped* (or passes its data-free part: `remote.mcp` and `frontend.rom_fetch` then skip their ROM checks, `core.multicart` its PLA table check). Two registrations depend on what exists **at configure time**: `remote.headless_script` is registered only when `sys22.bin` is in the ROM folder, and `sddual.rom.*` runs its updater and VT-DOS-boot parts only when `testdata/sdinterface/` holds the files. So configure again after adding test data:

```sh
cmake --preset gcc-release
```

### MinGW tests under Wine

The `mingw-release` preset has no test preset. Run the test programs directly, with paths relative to the repository root and a Wine prefix of your own:

```sh
export WINEPREFIX=$HOME/.wine-freetvc
wineboot -i
wine build/mingw-release/tests/core_test.exe
wine build/mingw-release/tests/boot_test.exe testdata/roms 64k+-2.2
wine build/mingw-release/src/freetvc-headless.exe --version
```

The arguments of each test are in `tests/CMakeLists.txt` (`add_test(... COMMAND ...)`). `remote.png` is not built when cross-compiling.

## Test data

Test data is never committed. It lives in the git-ignored `testdata/` folder, the CMake cache variable `FREETVC_TEST_DATA` (default `<repository>/testdata`). Each data set has its own cache variable, set from `FREETVC_TEST_DATA` on the **first** configure of a build tree; to move the data later, set the specific variables or delete `build/<preset>/CMakeCache.txt`.

| `testdata/` entry | Holds | Public source | Cache variable | Tests |
|---|---|---|---|---|
| `roms/` | the ROM autofetch's output ([ROMs](#roms)) | `cmake -DFREETVC_ROM_OUT=$PWD/testdata/roms -P cmake/FreeTVCRoms.cmake` | `FREETVC_ROM_DIR`; with `-DFREETVC_FETCH_ROMS=ON` and no `testdata/roms`, `build/<preset>/roms`. Also `FREETVC_DISK_ROM` (`hbf.rom`), `FREETVC_CARD256_ROM`, `FREETVC_NANOSD_ROM`, `FREETVC_SDDUAL_ROM` | `boot.*`, `debug.rom.*`, `snapshot.rom.*`, `input.keymap_rom`, `input.typing.*`, `input.host`, `sound.basic.*`, `media.rom.*`, `media.tape.*`, `media.disk.*`, `card256.rom.*`, `nanosd.rom.*`, `serial.rom.*`, `sddual.rom.*`, `multicart.rom.*`, `remote.headless_script`, `headless.snapshot_no_preset`, `frontend.rom_search`; the ROM parts of `remote.mcp` and `frontend.rom_fetch` |
| `sst-z80/v1/` | the 97 JSON files listed in `tests/z80/sst_subset.txt` (about 77 MB; MIT) | SingleStepTests/z80, commit `ebe1875d48f374bcfd4b505d8eb8ee751568b5f7`, folder `v1/` | `FREETVC_SST_DIR` | `z80.sst_subset` |
| `zex/` | `zexdoc.com`, `zexall.com`: the CP/M originals (GPL-2.0-or-later) | floooh/chips-test, commit `3785836e76c43922f78a50e1f8adfed259ab9672`, `tests/roms/` | `FREETVC_ZEX_DIR` | `z80.zexdoc`, `z80.zexall` |
| `golden/<preset>/` | `<scene>.json` (the frozen video state), `.vram` (16 KiB), `.pgm` (the frame as colour indexes) for the scenes `logo`, `prompt`, `mode2`, `mode16`, captured from WinTVC 1.4.1; the `golden.*` tests are skipped without them | none: the capture tool is not published | `FREETVC_GOLDEN_DIR` | `golden.*` (16; presets `32k-1.2`, `32k+hbm-1.2`, `64k-1.2`, `64k+-2.2`) |
| `Flipper.dsk` | a 360 KiB VT-DOS disk image whose root holds `FLIPPER.CAS` and `FLIPKOD.CAS` | ❓ no public download confirmed; any VT-DOS image with those two files works | `FREETVC_DISK_IMAGE` | `media.disk.*` (3) |
| `TVC-Multicart-v2/` | dikdom's MultiCart v2 repository; the tests read `software/tvcpla/tvcpla.bin` and `software/firmware/multicart_v2.1.14en.bin` | https://github.com/dikdom/TVC-Multicart-v2, commit `631b251d131e6d0782aa7192fd18f08c0134527e` | `FREETVC_MULTICART_DIR` | `core.multicart` (the PLA table check), `multicart.rom.*` |
| `sdinterface/` | `sd_dual_2022_11_09_gm/sd_dual_2022_11_09_gm/` (`SDFLASH.CAS`, `vtdos_cr.bin`, ...) and `system_f_v2/` (`BDOS.SYS`, `COMMAND.COM`, ...): the SD Dual author's ZIPs, unpacked; no licence stated | the Internet Archive's copy of `users.atw.hu/sdinterface/` | `FREETVC_SDDUAL_FILES` | `sddual.rom.*` (the firmware updater and booting VT-DOS from the card; the rest runs without them) |

Commands to fill `testdata/` (bash; each download is checked by the test or by the hashes given here):

```bash
# ROMs
cmake -DFREETVC_ROM_OUT=$PWD/testdata/roms -P cmake/FreeTVCRoms.cmake

# SingleStepTests/z80 subset
mkdir -p testdata/sst-z80/v1
grep -v '^#' tests/z80/sst_subset.txt | while IFS= read -r n; do
    curl -fsSL -o "testdata/sst-z80/v1/$n.json" \
        "https://raw.githubusercontent.com/SingleStepTests/z80/ebe1875d48f374bcfd4b505d8eb8ee751568b5f7/v1/${n// /%20}.json"
done

# ZEXDOC and ZEXALL
mkdir -p testdata/zex
for f in zexdoc.com zexall.com; do
    curl -fsSL -o testdata/zex/$f "https://raw.githubusercontent.com/floooh/chips-test/3785836e76c43922f78a50e1f8adfed259ab9672/tests/roms/$f"
done
sha256sum testdata/zex/*.com
# 07f72770b73273799c681925b04d8f50848ebd3a530add01b577e0f41d38f99f  testdata/zex/zexall.com
# 9983008770347bcbb8ebe103fc27b1edcb52a0c39932d4c38797481bf40a9924  testdata/zex/zexdoc.com

# MultiCart v2 (the two files the tests read)
m=https://raw.githubusercontent.com/dikdom/TVC-Multicart-v2/631b251d131e6d0782aa7192fd18f08c0134527e
mkdir -p testdata/TVC-Multicart-v2/software/tvcpla testdata/TVC-Multicart-v2/software/firmware
curl -fsSL -o testdata/TVC-Multicart-v2/software/tvcpla/tvcpla.bin $m/software/tvcpla/tvcpla.bin
curl -fsSL -o testdata/TVC-Multicart-v2/software/firmware/multicart_v2.1.14en.bin $m/software/firmware/multicart_v2.1.14en.bin

# SD Dual files from the Internet Archive
mkdir -p testdata/downloads testdata/sdinterface/sd_dual_2022_11_09_gm testdata/sdinterface/system_f_v2
curl -fsSL -o testdata/downloads/sd_dual_2022_11_09_gm.zip \
    https://web.archive.org/web/20250516092433id_/http://users.atw.hu/sdinterface/Firmware/sd_dual_2022_11_09_gm.zip
curl -fsSL -o testdata/downloads/system_f_v2.zip \
    https://web.archive.org/web/20250516092435id_/http://users.atw.hu/sdinterface/Tools/system_f_v2.zip
sha256sum testdata/downloads/*.zip
# 9d70080086a22f87347284459b3ac104ff26081e2353dada83c1382d1591f31f  testdata/downloads/sd_dual_2022_11_09_gm.zip
# 064996ecd0da3ba6d59e3b2445bbcce20cbdec517a40d539ce49c5cb9c50da16  testdata/downloads/system_f_v2.zip
unzip -q testdata/downloads/sd_dual_2022_11_09_gm.zip -d testdata/sdinterface/sd_dual_2022_11_09_gm
unzip -q testdata/downloads/system_f_v2.zip -d testdata/sdinterface/system_f_v2

# Then configure again, so the configure-time registrations see the data
cmake --preset gcc-release
```

Instead of the two MultiCart files, a full clone at that commit works too: `git clone https://github.com/dikdom/TVC-Multicart-v2.git testdata/TVC-Multicart-v2` and `git -C testdata/TVC-Multicart-v2 checkout 631b251d131e6d0782aa7192fd18f08c0134527e`.

Treat downloaded files as untrusted data: keep them under `testdata/`, and do not run scripts from inside it.

## Repository layout

| Path | What |
|---|---|
| `CMakeLists.txt`, `CMakePresets.json` | the top-level build and its presets |
| `cmake/` | warning flags (`FreeTVCWarnings.cmake`), the font fetch (`FreeTVCFonts.cmake`), the ROM autofetch (`FreeTVCRoms.cmake`), the text embedder (`EmbedText.cmake`), the MinGW-w64 toolchain file (`toolchains/`) |
| `src/` | freeTVC's sources (below) |
| `tests/` | the test programs: `core/`, `debug/`, `frontend/`, `machine/` (ROM-based), `remote/`, `sdl/` (UI data and host input), `z80/`, `support/` (a small JSON reader) |
| `data/` | files that ship next to the GUI: `skins/`, `pictures/`, `keymaps/`, `lang/` (gettext `.po`); `roms.txt` (the ROM list, compiled into the programs) |
| `third_party/` | vendored code: floooh `z80.h` (patched), Dear ImGui (docking), nlohmann/json, FatFs, littlefs, ymfm, reSID (patched); provenance in [THIRD_PARTY.md](../THIRD_PARTY.md) |
| `tools/` | developer tools: `ui/` (real-input GUI checks), `lang/` (translation template and check), `mcp/` (MCP Inspector and stress checks), `dezog/` (DeZog patch and DZRP checks), `gdb/` (GDB stub check), `sound/` (sound measurements), `re/` (ROM trace and symbol checks, table generators), `z80eval/` (the Z80 core evaluation harness) |
| `docs/` | the documentation; `docs/hardware/rom-symbols/*.sym` are compiled into the program |
| `LICENSE`, `THIRD_PARTY.md`, `README.md` | GPL-3.0 text, third-party list, short introduction |
| `build/`, `testdata/` | git-ignored: build trees and local test data |

### `src/`

| Folder | What |
|---|---|
| `core/z80/` | the Z80 adapter: `z80_chips.c` is the only file that includes `z80.h` (compiled as C11), `z80_bridge.h` its C interface, `z80.cpp` the C++ class |
| `core/bus/` | `MemoryMap` (paging per model, RAM/VRAM/ROM images, the cartridge port, the card slots), VRAM stretch tables, MultiCart v2, SD Dual, the SPI SD card, JEDEC flash chips |
| `core/machine/` | `Machine` (half-T clock, `run()`, I/O decode, IRQ, keyboard matrix), `MachineConfig`, the debug, OS-call, tape and printer hooks |
| `core/video/` | MC6845, the TVC video generator, the framebuffer and palette |
| `core/input/` | key names and positions, the character map, timed key scripts |
| `core/audio/` | `Sound` (tone generator, sound IRQ, DAC) and `AudioSynth` (band-limited synthesis to 48 kHz) |
| `core/cards/` | `Card` and `CardSlots`, every expansion card and its chips (HBF + WD1793, GameCard, TVC256k++ with overlay, storage and vSID, Multichip with SN76489, SAA1099 and YM3812, NanoSD, serial card with 8251A, the SID card on reSID) |
| `core/media/` | CAS files, the host cassette, autostart, the tape deck and signal, the `MediaControl` interface |
| `core/snapshot/` | TVCS snapshot chunks, the machine state, WinTVC `.tvs` import |
| `core/message.*` | `msg()`: user-facing English messages a front end can translate |
| `debug/` | `Debugger` (checkpoints, stepping, trace, saved states), Z80 decoder, disassembler, expressions, symbols (with the built-in ROM symbols), source maps, profiler |
| `remote/` | TCP layer, PNG writer, the text monitor and its hub, the MCP server (protocol, tools, transports), the DZRP server, the GDB stub |
| `frontend/common/` | presets and the ROM search, `freetvc.ini`, WAV, `HostTape` (the host files around the machine; implements `MediaControl`), TVC256k++ host media, the curl ROM download, unzip, MultiCart and SD image builders, printer and serial host ends |
| `frontend/headless/` | `freetvc-headless` |
| `frontend/sdl/` | the GUI: `main.cpp` (main loop and wiring), Play view, Bench, settings sheet, display (OpenGL and SDL renderer, picture shaders), host input, key layouts and their editor, skins, translations and `tr()` |

### Targets and the dependency rule

| Target | Kind | Links |
|---|---|---|
| `freetvc_core` | static library | `freetvc_z80h`, `freetvc_fatfs`, `freetvc_littlefs`, `freetvc_ymfm`, `freetvc_resid` |
| `freetvc_debug` | static library | `freetvc_core` |
| `freetvc_remote` | static library | `freetvc_debug`, threads, `freetvc_json` (and `ws2_32` on Windows) |
| `freetvc_frontend_common` | static library | `freetvc_core` (public), `freetvc_debug`, `freetvc_remote` |
| `freetvc-headless` | program | `freetvc_frontend_common`, `freetvc_remote` |
| `freetvc_skin` | static library (GUI builds) | `freetvc_core`: skin and picture files, translation catalogs, `tr()`, without SDL or ImGui |
| `freetvc_keymap` | static library (GUI builds) | `freetvc_core`, SDL3 |
| `freetvc` | program (GUI builds) | `freetvc_frontend_common`, `freetvc_remote`, `freetvc_skin`, `freetvc_keymap`, `freetvc_imgui`, SDL3 |

**`freetvc_core` and `freetvc_debug` use no OS services, threads, clocks or I/O.** The core never reads a file: ROMs, cartridges and card images reach it as bytes (`RomImages`, `CardConfig::rom`), and host media (disk drives, SD cards, the TVC256k++'s pendrive, the serial line) are host objects attached through pointers, kept outside the machine state. Time is the machine's own half-T tick count. A copy of a `Machine` is the complete machine state, and snapshots are byte-exact; new state must keep both true. Sockets, threads and files belong in `freetvc_remote` and the front ends.

## Coding rules

- **Languages**: C11 for the one C file, C++20 for everything else, compiler extensions off. GCC and Clang build freeTVC's own targets with `-Wall -Wextra -Wpedantic -Wshadow`, MSVC with `/W4 /permissive- /utf-8`; third-party targets keep their defaults. Keep the build warning-free (check with `-DFREETVC_WERROR=ON`).
- **Files** start with `// SPDX-License-Identifier: GPL-3.0-or-later`. Code lives in `namespace freetvc` (`freetvc::debug`, `freetvc::remote`, `freetvc::sdl` in those libraries).
- **Comments**: one line, short and descriptive, only where the code is not self-evident. No multi-line comment blocks and no history in comments; design reasoning goes into `docs/`.
- **Licence**: freeTVC is GPL-3.0-or-later. Reuse only GPL-3-compatible code (MIT, BSD, zlib, GPL-2.0-or-later, GPL-3, LGPL), never GPL-2.0-only. Keep the original headers, mark local changes in the file, and record every reused file in [THIRD_PARTY.md](../THIRD_PARTY.md) with its origin, version, licence and changes. Code written from someone else's sources without copying is listed there too ("Written from third-party sources").
- **Clean room**: other TVC emulators and tools (WinTVC and the like) may be studied only for their behaviour. What is learned goes into `docs/` as a description with its evidence; the code is then written from the docs. Never transcribe disassembled, decompiled or other third-party emulator code into `src/`.
- **User-facing text**:
  - shared code (`core/`, `frontend/common/`) builds messages with `msg(format, ...)` (`src/core/message.hpp`), in English; the GUI rebuilds them in the UI language through a hook;
  - GUI text goes through `tr()`, `trc(context, text)` where one English word needs two translations, `tr_format()` for printf-style text, and `N_()` to mark a literal that reaches `tr()` or `msg()` later (`src/frontend/sdl/text.hpp`); the status strip's notices are the second argument of `.show()`/`.show_text()`;
  - the monitor, MCP, `--help` and stderr stay English and are not translated;
  - **every new string needs its entry in `data/lang/hu.po`** (and in any other catalog). The test `ui.translation_coverage` runs `tools/lang/po_check.py check` over `src/` and every `data/lang/*.po` and fails on a missing, fuzzy or stale entry, or on a translation whose printf conversions differ ([tools/lang/README.md](../tools/lang/README.md)).
- **No ROMs or third-party binaries in the repository.** Tests find their data through cache variables ([Test data](#test-data)); the programs through the ROM search.
- **UI design**: the GUI's look is deliberate (case materials, the TVC's 16 colours with fixed meanings, Atkinson Hyperlegible); new screens follow [design/ui.md](design/ui.md), not the stock Dear ImGui look or another emulator's layout.
- **Docs**: English; findings about hardware or third-party behaviour carry their evidence (a source file and line, a data sheet, a ROM address, a run), marked ✓ (verified), ❓ (unknown) or ⚑ (needs a dynamic check).

## Your first change

1. Configure and build a debug tree: `cmake --preset gcc-debug` and `cmake --build --preset gcc-debug`.
2. Run `ctest --preset gcc-debug` once before changing anything, so you know which tests pass or skip with your test data.
3. Make the change. Add or extend a test program in `tests/` and register it in `tests/CMakeLists.txt` ([Tests for new code](#tests-for-new-code)).
4. Add `hu.po` entries for new user-facing strings, then run the translation check:

   ```sh
   python3 -I tools/lang/po_check.py check src data/lang/hu.po
   ```

5. Build and run the fast set again on `gcc-debug`, and on `gcc-release` and `clang-debug` if the change touches timing, the core or compiler-sensitive code.
6. For anything the user sees in the GUI, check the real window ([Real-GUI checks](#real-gui-checks)): a passing test with simulated input is not proof that the GUI works.
7. Update the design doc that describes the area (and [status.md](status.md) when a feature or gap changes).

## Extending freeTVC

The recipes name the real files and functions; read the existing code next to each step before copying its pattern. The Multichip sound card (`MultichipCard`, `CardKind::Multichip`) is the simplest complete card to follow from end to end: `grep -rn -i multichip src tests data`.

### Adding an expansion card

A slot card has the slot's 16 ports (slot *n* at 10h + 10h·*n* to 1Fh + 10h·*n*), the 8 KiB IOMEM window at C000h–DFFFh when port 03h bits 7–6 select its slot, a 2-bit ID at port 5Ah, and the slot's /INT line ([design/cards.md](design/cards.md), [hardware/tvc-hardware.md](hardware/tvc-hardware.md)).

**Core** (`src/core/`):

1. Add a `CardKind` value at the end of the enum in `core/machine/config.hpp`, and a default slot constant beside `kGameCardSlot` and the others.
2. Write the card as a class derived from `Card` (`core/cards/card.hpp`) in `core/cards/<name>.{hpp,cpp}`, and add the `.cpp` to `freetvc_core` in `src/CMakeLists.txt`. The interface:
   - `clone()` returns a full copy (a machine copy clones its cards); `kind()`; `id()` (port 5Ah: 00 serial, 01 unnamed, 10 floppy, 11 none or a card named by its ROM header);
   - `power_on()`, `reset(tick)`;
   - `io_read(port, bus)`, `io_write(port, value, bus)` for ports 0–15 of the slot; `CardBus` (same header) gives the tick, the bus SOUND line (`audio`), the keyboard matrix, the tone generator (`sound`, whose ÷2 stage is TXRXCLK) and the host media pointers;
   - `mem_read(offset)` (no side effects), `mem_write(offset, value)` and `window()` (the 8 KiB the debugger shows) for IOMEM;
   - `save(ChunkWriter&)` and `load(ChunkReader&)` for snapshots (`core/snapshot/snapshot.hpp`), byte-exact;
   - optional: `advance_audio(tick, AudioSynth&)` for a card with sound (bring the chip up to `tick` and feed `AudioSynth::add_step()`, which mixes card steps into the machine's band-limited output), `phantom_page(unit)` to replace RAM unit 2 or 3, `border_write()` to snoop port 00h, `overlay()` for a card that drives the video overlay lines, `irq()` and `clear_interrupt()` for the slot /INT line and its /CLRINT strobe (58h + slot), and `next_event()`/`run_events(bus)` for a timer that can pull /INT (the machine calls `run_events` once its tick reaches the earliest `next_event()`; see `MultichipCard` and its YM3812).
3. Create it in `CardSlots::configure()` and name it in `card_kind_name()` (`core/cards/card.cpp`).
4. Snapshots store each slot's kind and ROM: raise the upper bound of the kind check in `core/snapshot/machine_state.cpp` (`kind > static_cast<std::uint8_t>(CardKind::Serial)`) to the new last value.
5. If the card needs a host resource (a file, a folder, a network line), add a host-side object outside the machine state and a `CardBus` pointer to it, filled in `Machine::card_bus()` (`core/machine/machine.hpp`), as `FloppyDrives`, `Card256Media`, `NanoSdMedia` and `SerialLine` are.
6. A device outside the slots (as the SID card at E0h–FFh) is a `Machine` member decoded in `Machine::io_read`/`io_write` instead.

**Front ends:**

7. ROM: add `piece`, `image` and `notice` lines to `data/roms.txt`; a name constant beside `kMultichipRom` in `frontend/common/presets.hpp`; its CRC-32 in `kKnownRoms` in `frontend/common/presets.cpp`; then update the piece and image counts that `tests/frontend/rom_fetch_test.cpp` checks (`list.pieces.size() == 24 && list.images.size() == 21`). Only add firmware whose licence allows the download; say what it is in the notice.
8. `MediaControl` (`core/media/media_control.hpp`): add the card's methods (on/off with a slot, its slot, a describe line), and implement them in `HostTape` (`frontend/common/host_tape.{hpp,cpp}`): the ROM through `find_card_rom()`, placement through `place_card()`, and the ROM reload in the function that refreshes card images after the ROM folder changes (the `CardKind::Multichip` branch near `multichip_rom()`).
9. Headless (`frontend/headless/main.cpp`): the option (`--<name> [SLOT]`, `--<name>-rom FILE`), its `--help` text, and the `config.cards[slot]` setup.
10. GUI (`frontend/sdl/`): the same options and the usage line in `main.cpp`; the `freetvc.ini` key read at start and written on exit; a `PlayActions` toggle and a `PlayStatus` field with the Cards menu item in `play_view.{hpp,cpp}`; the slot rib name (the `slot_cards` switch in `main.cpp`); a row in `settings_view.cpp`.
11. `freetvc.ini`: the field in `Settings` (`frontend/common/settings.hpp`), its parse in `parse_settings()` and the key list in the file header text (`kHeader`) in `frontend/common/settings.cpp`; extend `tests/frontend/settings_test.cpp`.
12. Monitor (`remote/monitor.{hpp,cpp}`): a command in `Monitor::commands()` and its `cmd_<name>()`; a card that needs only its image can use `switch_card()`.
13. MCP (`remote/mcp_tools.cpp`): `<name>_on`/`<name>_off` actions in the `media` tool (its handler, the `enum` in its schema and its description).
14. Translations: `hu.po` entries for every new `msg()` and `tr()` string.

**Tests and docs:**

15. A core test `tests/core/<name>_test.cpp` (ports, timing, sound measured in the machine's audio, reset, a snapshot round trip and a machine copy mid-activity), and if it has firmware a ROM test `tests/machine/<name>_rom_test.cpp` that exits 77 without its ROM ([Tests for new code](#tests-for-new-code)).
16. [design/cards.md](design/cards.md) (the card, its sources and evidence), [hardware/tvc-hardware.md](hardware/tvc-hardware.md) for hardware facts, [THIRD_PARTY.md](../THIRD_PARTY.md) for anything reused or followed, [status.md](status.md), and [user-guide.md](user-guide.md) for the options.

### Adding a cartridge type

1. Add a `CartType` value at the end of the enum in `core/machine/config.hpp`, and raise the cartridge-type bound of the `CONF` check in `core/snapshot/machine_state.cpp` (`type > static_cast<std::uint8_t>(CartType::SdDual)`).
2. Bank switching: in `MemoryMap` (`core/bus/memory_map.{hpp,cpp}`), the cartridge page's read and write paths (as the KiloCart's read register and the MegaCart's write register) and `select_cart_page()`. A board with chips of its own (flash, RAM, registers) is a class in `core/bus/` held by `MemoryMap` as a `std::optional`, as `MultiCart` and `SdDual` are, with its power-on, reset and snapshot state.
3. Loading (`frontend/common/host_tape.cpp`): `cart_type_name()`, `parse_cart_type()` and the detection in `load_cart()` (a header test such as `is_multicart_image()`, else by size), and the error message that lists the types.
4. The type name in every help text: headless `--cart-type`, the GUI usage line, the monitor's `cart` usage, the MCP `media` tool's `cart_type` description.
5. Tests: the cartridge part of `tests/core/bus_test.cpp` (paging, reset against power-on, debugger accesses that must not page, a snapshot round trip), and a ROM test if real software exists.
6. Docs: [design/media.md](design/media.md) for cartridges, or [design/cards.md](design/cards.md) for a board with chips.

### Adding a monitor command

1. Declare `std::string cmd_<name>(Args& a);` in `src/remote/monitor.hpp`.
2. Add a row to the table in `Monitor::commands()` (`src/remote/monitor.cpp`): names (first name, then aliases, space separated), usage, one-line help, `&Monitor::cmd_<name>`. `help` lists the table, so the command documents itself.
3. Implement it: arguments in `a`, failures with `fail("message")` (an English line), output as lines ending in `'\n'`. Program media go through `media_` (a front end without media leaves it null: fail with "this front end has no program media"). After a change of ROMs or cards call `machine_changed()`.
4. Test it in `tests/remote/monitor_test.cpp` (`EXPECT_IN(mon.execute("..."), "...")` on a small RAM program), or through a `--moncommands` script as `remote.headless_script` does with `tests/remote/boot_call.mon`.
5. Document it in [design/debug-and-remote.md](design/debug-and-remote.md) and [user-guide.md](user-guide.md).

MCP clients can reach every monitor command through the `monitor_command` tool when the emulator runs with `--mcp-allow-monitor`.

### Adding an MCP tool

1. Write the handler in `src/remote/mcp_tools.cpp` as `json <name>(Ctx& c, const json& args)`: `Ctx` gives the monitor, the debugger, the machine and the options; read arguments with the existing helpers (`string_arg()`, `int_arg()`), report argument errors with `fail()` (a `ToolError`, returned with `isError` so the model can correct itself), and return `structured({...})` or text content as the other tools do.
2. Register it in `make_tools()` with `tools.push_back({name, title, description, schema({...}, {required...}), annotations(read_only, destructive, idempotent), bind(<name>)})`. `tools/list` returns the tools in this order.
3. `tests/remote/mcp_test.cpp` checks the tool count (`s.tool_count() == 20`): update it and add calls for the new tool.
4. Optionally run `tools/mcp/inspector_check.sh` and `tools/mcp/stress_check.py` ([tools/mcp/README.md](../tools/mcp/README.md)).
5. Document it in [design/debug-and-remote.md](design/debug-and-remote.md).

### Adding a GUI panel or setting

- **A Bench panel** (`src/frontend/sdl/bench_view.{hpp,cpp}`): a window ID constant (`constexpr const char* kProfiler = "###profiler";` is the pattern: the visible title comes from `tr()`, the `###` ID keeps the saved layout stable across languages), a `draw_<name>()` member called from the Bench frame next to the others, and its dock in the `DockBuilder` block. The default dock is applied only when no saved layout exists (`layout.ini` in the user data folder); with a saved one the new panel opens undocked. Take colours and fonts from the skin (`Skin`, `SkinFonts`) and follow [design/ui.md](design/ui.md).
- **A setting**: the `freetvc.ini` key as in step 11 of the card recipe; a row in the right section of `settings_view.cpp` (`SettingsSection`: Machine, Picture, Sound, Keyboard, Interface, Remote), with its state in `SettingsStatus` and its change as an action back to `main.cpp`, which applies it and saves the key with `save_settings()`. A value given on the command line wins for that run and is not saved (the `*_given` flags).
- **Strings**: every label through `tr()`, with its `hu.po` entry. Hungarian labels are often longer; check narrow fields in both languages with a window grab.

### Adding a UI language

1. Write the template and start the catalog with gettext:

   ```sh
   python3 -I tools/lang/po_check.py pot src freetvc.pot
   msginit -i freetvc.pot -o data/lang/<code>.po -l <code> --no-translator
   ```

2. In its header set `Language: <code>` and `X-Language-Name: <the language's own name>`; translate every entry, keeping printf conversions in the same order and leading or trailing spaces and line breaks.
3. Configure again (the test globs `data/lang/*.po` at configure time) and run `ctest --preset gcc-debug -R '^ui\.translation'`. An incomplete catalog fails `ui.translation_coverage`.
4. The GUI lists the catalog in the main menu's Language section; `--language <code>` and `language = <code>` in `freetvc.ini` select it.

Keeping a catalog current: `msgmerge -U --backup=none data/lang/hu.po freetvc.pot`, then translate what it marks.

### Adding a skin, picture file or key layout

All three are text files of `key = value` lines with `;` comments, copied next to the GUI at build time and also looked for in the same-named folder of the user data folder (which comes first, and where the GUI keeps edited copies).

| Kind | Folder | Start from | Selected by | Test |
|---|---|---|---|---|
| Skin | `data/skins/*.skin` | `bronze.skin`, which lists every key with its built-in value; a skin may set only some | `--skin NAME\|FILE`, `skin` in `freetvc.ini`; saving the skin in use applies it at once | `ui.skin` (`bronze.skin` must equal the built-in skin) |
| Picture | `data/pictures/*.picture` | `pal-tv.picture`, which names every key | `--picture NAME\|FILE`, `picture` | `ui.picture`: update the shipped-file count in `tests/sdl/picture_test.cpp` (`entries.size() == 4`) |
| Key layout | `data/keymaps/*.keys` | `hungarian.keys` (the built-in layout) or `us.keys`; `build/<preset>/tests/keymap_test --print` writes the built-in layout | `--keys NAME\|FILE`, `keys`, or the GUI's keyboard editor | `ui.keymap`: update the layout count in `tests/sdl/keymap_test.cpp` (`list_keymaps({dir}).size() == 2`) |

The `name` key must match the file name. The formats are described in [design/ui.md](design/ui.md).

### Tests for new code

- A test is a program in `tests/<area>/` that prints `FAIL: ...` lines and exits non-zero on failure, 0 on success, and **77 when its data is missing**.
- Register it in `tests/CMakeLists.txt`: `freetvc_test_program(<exe> <area>/<file>.cpp)` (links `freetvc_core`; add libraries with `target_link_libraries`), `add_test(NAME <area>.<name> COMMAND <exe> <args>)`, then `set_tests_properties(... PROPERTIES LABELS "core")` or `LABELS "rom" SKIP_RETURN_CODE 77 TIMEOUT 600` for a ROM test. Pass data paths as arguments from the cache variables, never as hard-coded paths.
- A test that needs new test data gets its own cache variable under `FREETVC_TEST_DATA`, and a row in the [Test data](#test-data) table with a public source.

## Verifying changes

Compiling, starting and exiting with code 0 do not show that a change works. Check the behaviour itself:

1. **Unit tests**: the `core` label, on `gcc-debug` and `clang-debug`.
2. **ROM tests**: the `rom` label with `testdata/roms` present; they boot the real ROMs and check the screen, sound and media through the ROM's own routines.
3. **Headless checks by hand**: `freetvc-headless` with `--screenshot` (an 800 × 314 PPM of the last frame), `--wav` (48 kHz mono) and `--moncommands`, then read the output back.
4. **The real GUI** for anything visible or interactive.

### Real-GUI checks

`tools/ui/ui_drive.py` starts the GUI, sends real X key and mouse events (XTEST, only while the freeTVC window has focus) and saves window grabs as PNG files ([tools/ui/README.md](../tools/ui/README.md) lists its steps: `wait`, `grab`, `key`, `type`, `click`, `move`, `dclick`, `rclick`, `sh`). Run it on a virtual display, so no events reach your own desktop:

```sh
Xvfb :99 -screen 0 1280x1024x24 -nolisten tcp &
DISPLAY=:99 metacity &
mkdir -p build/ui-check
DISPLAY=:99 EXTRA="--rom-dir testdata/roms --no-audio" \
    python3 -I tools/ui/ui_drive.py build/gcc-release/src/freetvc build/ui-check \
    wait 7 key space wait 1 type 'print 6' wait 1 grab prompt
```

Then open `build/ui-check/prompt.png` and look at it. Notes:

- Without `EXTRA` the script passes `--rom-dir testdata/roms --no-audio`; set `EXTRA` to add other options.
- `metacity` (or another window manager) is needed: `wmctrl` focuses the window through it. The virtual display has no sound, so pass `--no-audio`.
- The GUI draws through OpenGL. Grab the freeTVC window itself (the script does, by its window ID); a grab of the root window shows a GL window as black.
- The script gives the GUI `<out dir>/xdg` as `XDG_DATA_HOME`, so the GUI's `freetvc.ini` and `layout.ini` go there and your own settings stay untouched. Set `XDG_DATA_HOME` to a scratch folder for any other GUI run you script.
- File dialogs: SDL asks the desktop portal over the session bus, which would open the dialog on your real desktop. Run the check under its own bus with the zenity driver: `DISPLAY=:99 SDL_FILE_DIALOG_DRIVER=zenity dbus-run-session -- python3 -I tools/ui/ui_drive.py ...`. The dialog is then a zenity window on `:99`; type into it with `xdotool` real key events after `xdotool windowactivate --sync` (details in [tools/ui/README.md](../tools/ui/README.md)).
- Click coordinates depend on the window size (1000 × 760 by default), the skin and the language (Hungarian strip items sit further right): read them off a grab.
- Stop the display afterwards with `pkill -x metacity; pkill -x Xvfb`.

Sound can be checked headless (`--wav`, then `tools/sound/wavinfo.py` and `tools/sound/cycles.py`, [tools/sound/README.md](../tools/sound/README.md)) or from the GUI's own output with `tools/sound/gui_sound.py`.

## Making a release

`tools/release/make_release.sh <out dir>` builds the release packages (a Linux tarball and a Windows zip) from the current tree, runs the Linux tests and writes the release notes and checksums; [tools/release/README.md](../tools/release/README.md) has the requirements and the publishing steps.

## Debugging the emulator with its own tools

freeTVC's debug servers work on its own development as well as on TVC programs. All listen on 127.0.0.1 only.

| Server | Option | Default port | Client |
|---|---|---|---|
| Text monitor (VICE-style) | `--monitor` (stdin), `--remote-monitor`, `--moncommands FILE` | 6580 | any terminal or TCP client; `help` lists the commands |
| MCP server | `--mcp-stdio`, `--mcp-http` (bearer token printed on stderr, or `--mcp-token`) | 6584 | AI agents and MCP clients |
| DZRP | `--dzrp`, `--dzrp-banks` | 6582 | DeZog in VS Code ([tools/dezog/README.md](../tools/dezog/README.md)) |
| GDB stub | `--gdb` | 6583 | GNU gdb for `z80-unknown-elf`, DeZog's `gdb` remote |

For example:

```sh
build/gcc-debug/src/freetvc-headless --preset 64k+-2.2 --rom-dir testdata/roms --monitor
```

The GUI has the monitor built in (Bench, F12) and starts the same servers from its options or the settings window. The commands, tools and protocols are in [user-guide.md](user-guide.md) and [design/debug-and-remote.md](design/debug-and-remote.md); the wire protocols in [design/protocols-reference.md](design/protocols-reference.md). For the C++ side, the Debug presets work with any native debugger (`gdb build/gcc-debug/src/freetvc-headless`).

## Where to read next

| Document | What it covers |
|---|---|
| [README.md](README.md) | the documentation index |
| [status.md](status.md) | what is emulated and the known gaps |
| [user-guide.md](user-guide.md) | running and using the emulator |
| [glossary.md](glossary.md) | TVC terms, with the Hungarian originals |
| [design/architecture.md](design/architecture.md) | modules, threading and the timing model |
| [design/build.md](design/build.md) | the build system, the ROM autofetch and the test list in detail |
| [design/machine.md](design/machine.md) | the base machine: bus, video, keyboard, sound, printer port, golden frames |
| [design/media.md](design/media.md) | the host cassette, autostart, tape deck, cartridges, snapshots, rewind |
| [design/cards.md](design/cards.md) | the expansion cards and cartridge boards, with their sources and checks |
| [design/ui.md](design/ui.md) | the GUI: Play, Bench, skins, picture, language, settings, keyboard editor |
| [design/debug-and-remote.md](design/debug-and-remote.md) | the debugger, monitor, MCP server, DZRP server and GDB stub |
| [design/protocols-reference.md](design/protocols-reference.md) | the external protocols freeTVC speaks |
| [design/z80-core.md](design/z80-core.md) | the Z80 core: why `z80.h`, its patches and its tests |
| [hardware/tvc-hardware.md](hardware/tvc-hardware.md) | the TVC hardware reference: memory, I/O ports, video, sound, slots |
| [hardware/rom.md](hardware/rom.md) | the system ROMs: boot, system calls, BASIC, the cassette interface |
| [hardware/file-formats.md](hardware/file-formats.md) | CAS, TTP, `.tvs` and the other file formats |
| [hardware/rom-symbols/](hardware/rom-symbols/) | the ROM symbol files compiled into the debugger |
| [../THIRD_PARTY.md](../THIRD_PARTY.md) | every third-party component, its licence and local changes |
