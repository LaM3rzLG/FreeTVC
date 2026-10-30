# Build, layout and tests

This page is the reference for building freeTVC: the source tree, the CMake presets and options, the dependencies, the ROM autofetch, the files copied next to the programs, and every test with the data it needs. Day-to-day setup is described in [developer-guide.md](../developer-guide.md); running the programs in [user-guide.md](../user-guide.md).

## Source tree

```
CMakeLists.txt, CMakePresets.json
cmake/                    FreeTVCWarnings.cmake, FreeTVCFonts.cmake, FreeTVCRoms.cmake, EmbedText.cmake,
                          toolchains/mingw-w64-x86_64.cmake
third_party/              vendored z80.h, Dear ImGui, nlohmann/json, FatFs, littlefs, ymfm and reSID; SDL3 is fetched
                          (provenance and licences in THIRD_PARTY.md)
src/core/                 freetvc_core: the machine; no OS, threads, clocks or I/O
  z80/                    z80_chips.c (the only C unit that sees z80.h), z80_bridge.h (C ABI), the C++ Z80 adapter
  bus/                    MemoryMap (page decoder, RAM/VRAM/ROM images), VRAM stretch tables, MultiCart v2,
                          JEDEC flash chips, SD card (SPI mode), SD Dual cartridge
  machine/                Machine: half-T clock, run(limits) → StopInfo, I/O decode, IRQ, keyboard matrix; MachineConfig;
                          debug_hooks.hpp (what an attached debugger is told), the OS-call hook, printer and tape ports
  video/                  MC6845, TVC video generator, Framebuffer (indexed, 800 × 314, whole raster) + RGB palette
  input/                  key positions and names, character map, timed key scripts (typing)
  audio/                  Sound (tone generator, sound IRQ), AudioSynth (band-limited steps → 48 kHz PCM)
  cards/                  expansion slots (Card, CardSlots) and the cards: HBF disk card with WD1793 and disk images,
                          GameCard (SN76489), TVC256k++ (overlay, storage, vSID), Multichip (SN76489, SAA1099,
                          YM3812 on ymfm), NanoSD, serial card (8251A), SID card (on reSID)
  media/                  CAS files, host cassette, autostart, tape signals and the tape deck
  snapshot/               TVCS snapshots, machine state, WinTVC .tvs import
  message.*               msg(): user-facing messages of the shared code, translatable by the GUI
  version.hpp.in          generated into the build tree
src/debug/                freetvc_debug: Debugger (checkpoints, stepping, trace, saved states, profiler), Z80 decoder,
                          expressions, symbol tables, source maps, disassembler; ROM symbol files compiled in
src/remote/               freetvc_remote: text monitor (engine + hub), MCP server, DZRP server, GDB stub, TCP layer,
                          PNG writer; sockets, threads and files are allowed here
src/frontend/common/      freetvc_frontend_common: presets and ROM search, settings file, host tape and media files,
                          ROM autofetch (curl, unzip), MultiCart and SD image builders, printer and serial hosts, WAV writer
src/frontend/headless/    freetvc-headless
src/frontend/sdl/         freetvc: Play mode, Bench, display (OpenGL and SDL_Renderer), host input, key layouts and the
                          keyboard editor, settings window, skins, picture files, translations
data/                     roms.txt (the autofetch list), skins/, keymaps/, pictures/, lang/ (copied next to the GUI)
tests/                    plain test programs registered with CTest (core/, debug/, frontend/, machine/, remote/, sdl/, z80/)
tools/                    check and analysis scripts: dezog/, gdb/, lang/, mcp/, re/, sound/, ui/, z80eval/
```

The module boundaries are described in [architecture.md](architecture.md).

### Z80 integration

- `z80.h` is compiled once, as C11, in `src/core/z80/z80_chips.c`. No C++ file includes it, because its anonymous structs are not valid C++.
- `z80_bridge.h` is a small C interface of freeTVC's own: init, tick, prefetch, opdone and a register struct.
- The C++ class `freetvc::Z80` keeps `z80_t` in a 64-byte aligned buffer. `_Static_assert`s in `z80_chips.c` check the size, the alignment and every pin bit freeTVC uses.
- A change of CPU core touches only `src/core/z80/`. The choice of `z80.h` and its patches are in [z80-core.md](z80-core.md).

### Machine run loop

- Time is counted in half-T ticks: 2 per T-state, plus the VRAM stretch ([architecture.md](architecture.md#how-a-frame-runs)).
- `Machine::run({max_ticks, max_instructions})` stops at the tick budget; a stretched last cycle can overrun it by up to 4 ticks. It returns a StopInfo: the reason, the ticks and instructions run, and the address of the last opcode fetch.
- An instruction counts when `z80.h` reports the T1 of the next fetch. The first fetch after `jump()` or `power_on()` ends no instruction.
- The bus is the TVC's: paging per model, I/O decode, the IRQ flip-flop and the video generator in lockstep ([machine.md](machine.md)). Test programs that want plain RAM write 02h = B0h first (`core_test`).
- `run` also stops with `StopReason::Debugger` when an attached debugger asks. A copy of a `Machine` is a full machine state (`MemoryMap` rebuilds its page table on copy); see [debug-and-remote.md](debug-and-remote.md).

## Requirements

- CMake 3.25 or newer (`cmake_minimum_required(VERSION 3.25)`; `CMakePresets.json` is version 6), Ninja, and a C11 + C++20 compiler (GCC, Clang or MSVC).
- **GUI on Linux:** the X11/Wayland development headers SDL3 needs (https://wiki.libsdl.org/SDL3/README-linux).
  - On Ubuntu 24.04 the packages SDL2's `libsdl2-dev` depends on cover all of them except XTest.
  - SDL 3.4 fails at configure when an enabled extension is missing: install `libxtst-dev`, or configure with `-DSDL_X11_XTEST=OFF` (freeTVC does not use XTest).
  - With `libpipewire-0.3-dev` installed, SDL3 also builds its PipeWire audio backend.
- **First configure with the GUI:** network access for the SDL3 tarball and the fonts, or local copies (`FREETVC_SDL3_URL`, `FREETVC_FONT_DIR`, below).
- **Optional:** Python 3 (tests `remote.png` and `ui.translation_coverage`), sjasmplus (test `debug.disasm_sjasmplus`), curl at run time (the programs' ROM download), MinGW-w64 and Wine (the Windows cross build and its tests).

## Presets

The configure presets put each build in `build/<preset>/`; the programs land in `build/<preset>/src/`, the test programs in `build/<preset>/tests/`.

| Preset | Host | Notes |
|---|---|---|
| `gcc-debug`, `gcc-release` | Linux | GCC |
| `clang-debug` | Linux | Clang |
| `mingw-release` | Linux | cross build of Windows x64 `.exe` files with MinGW-w64 (`cmake/toolchains/mingw-w64-x86_64.cmake`, Debian/Ubuntu tool names `x86_64-w64-mingw32-gcc`/`-g++`/`-windres`), linked with `-static`, so the executables need no MinGW DLLs. No test preset: the tests run under Wine by hand (below) |
| `msvc-debug`, `msvc-release` | Windows | run from an "x64 Native Tools" prompt, or open the folder in Visual Studio. These presets have not been built or tested |

```sh
cmake --preset gcc-release
cmake --build --preset gcc-release
ctest --preset gcc-release            # every test except the label slow
ctest --preset gcc-release-slow       # ZEXDOC and ZEXALL (needs the gcc-release build)
```

Every test preset excludes the label `slow`; `gcc-release-slow` runs only it. All four Linux presets build with no warnings.

## Options and cache variables

| Variable | Default | Effect |
|---|---|---|
| `FREETVC_BUILD_GUI` | ON | build `freetvc` (SDL3 + Dear ImGui). OFF builds the headless runner and the tests only, with no SDL3 or font download |
| `FREETVC_BUILD_TESTS` | ON | build and register the test programs |
| `FREETVC_WERROR` | OFF | treat warnings in freeTVC's own code as errors (`-Werror`, `/WX`) |
| `FREETVC_FETCH_ROMS` | OFF | the ROM autofetch at configure time, [below](#rom-autofetch) |
| `FREETVC_USE_SYSTEM_SDL3` | OFF | use an installed SDL3 (`find_package(SDL3 3.2 CONFIG)`) instead of fetching it |
| `FREETVC_SDL3_URL` | the SDL 3.4.18 release tarball on GitHub | another location of the same tarball, also `file:///path/SDL3-3.4.18.tar.gz`; its SHA-256 is checked either way |
| `FREETVC_FONT_DIR` | empty | a local folder with the font files instead of downloading them; their SHA-256 is checked either way |
| `FREETVC_TEST_DATA` and the other test-data variables | `testdata/` | [Test data](#test-data) |

Warnings: GCC and Clang get `-Wall -Wextra -Wpedantic -Wshadow`; MSVC gets `/W4 /utf-8` and, for C++, `/permissive- /Zc:__cplusplus` (`cmake/FreeTVCWarnings.cmake`). Third-party targets keep their defaults; two vendored libraries get one warning switched off each (ymfm `-Wno-stringop-overflow` on GCC, reSID `-Wno-deprecated-enum-float-conversion` on GCC and Clang, both explained in `third_party/CMakeLists.txt`).

## Dependencies

Vendored in `third_party/` (versions, hashes and changes in `THIRD_PARTY.md`):

| Library | Version | Licence | Used by |
|---|---|---|---|
| floooh/chips `z80.h` | file last changed in upstream commit `0949766`, with freeTVC's patches ([z80-core.md](z80-core.md)) | zlib | the CPU |
| Dear ImGui, docking branch | `v1.92.9b-docking`; demo window not vendored (`IMGUI_DISABLE_DEMO_WINDOWS`) | MIT | the GUI, with its SDL3, SDL_Renderer and OpenGL3 backends |
| nlohmann/json | 3.12.0 | MIT | the MCP server |
| FatFs (ChaN) | R0.16 | 1-clause BSD style | the TVC256k++ DSK drive, FAT image builders |
| littlefs | v2.11.3 | BSD-3-Clause | the TVC256k++ PSRAM and flash drives |
| ymfm (Aaron Giles), OPL family only | upstream commit `81aec25` | BSD-3-Clause | the Multichip card's YM3812 |
| reSID (Dag Lem) | upstream commit `ef7873f`, with a patch that makes `SID::State` complete | GPL-2.0-or-later | the SID card |

Fetched at configure time (GUI builds only):

- **SDL3** 3.4.18: `https://github.com/libsdl-org/SDL/releases/download/release-3.4.18/SDL3-3.4.18.tar.gz`, SHA-256 `9c75cf16330322c217dedd2e0609f1124f1b54b8633e763467b4684d0f4334a3`, through `FetchContent`, built as a static library without its tests, examples and install rules.
- **Fonts** (`cmake/FreeTVCFonts.cmake`, SIL OFL 1.1): Atkinson Hyperlegible Next Regular and Medium from `googlefonts/atkinson-hyperlegible-next` at commit `7925f50f649b3813257faf2f4c0b381011f434f1`, Atkinson Hyperlegible Mono Regular from `googlefonts/atkinson-hyperlegible-next-mono` at commit `154d50362016cc3e873eb21d242cd0772384c8f9`, and both repositories' `OFL.txt` (as `OFL-Next.txt`, `OFL-Mono.txt`). Each file's SHA-256 is pinned. They go into `build/<preset>/fonts/`; with `FREETVC_FONT_DIR` they are copied from there instead.

The GUI draws through OpenGL 3.3 core with Dear ImGui's OpenGL3 backend, or through SDL_Renderer (`--renderer sdl`, [ui.md](ui.md#picture)). The OpenGL functions come from `SDL_GL_GetProcAddress`, so no OpenGL library is linked; only `CMAKE_DL_LIBS`, for ImGui's own loader.

Generated sources: `cmake/EmbedText.cmake` turns text files into a C++ lookup function. It compiles `docs/hardware/rom-symbols/*.sym` into `freetvc_debug` (the built-in ROM symbols) and `data/roms.txt` into `freetvc_frontend_common` (the autofetch list), so the files in the repository stay the only copy.

## Files next to the programs

Post-build steps copy data folders next to the executables, where the programs look for them:

| Folder | From | Next to | Copied |
|---|---|---|---|
| `fonts/` | `build/<preset>/fonts/` (fetched) | `freetvc` | after each link |
| `skins/` | `data/skins/*.skin` | `freetvc` | whenever a skin file changes (a stamp target), not only on relink |
| `keymaps/` | `data/keymaps/*.keys` | `freetvc` | whenever one changes |
| `pictures/` | `data/pictures/*.picture` | `freetvc` | whenever one changes |
| `lang/` | `data/lang/*.po` | `freetvc` | whenever one changes |
| `roms/` | `build/<preset>/roms/` (the autofetch) | `freetvc` and `freetvc-headless` | after each link, only with `FREETVC_FETCH_ROMS=ON` |

The file formats are described in [ui.md](ui.md) (skins, key layouts, picture files, language catalogs).

## ROM autofetch

The TVC's system ROMs and the card ROMs are not in the repository. They are freely downloadable: the system and Videoton card ROMs from tvc.homeserver.hu, the open-source card firmware from its authors' GitHub repositories, and the SD Dual firmware from the Internet Archive's copy of its author's site. freeTVC can download them in three ways, all from one list, `data/roms.txt`.

### The list

`data/roms.txt` is read by `cmake/FreeTVCRoms.cmake` and compiled into the programs. Lines starting with `#` are comments; the others are:

| Line | Meaning |
|---|---|
| `site URL` | the base URL for relative piece paths (`https://tvc.homeserver.hu`) |
| `piece NAME PATH-or-URL SHA256` | one downloaded file: a path below the site, or a full `https://` URL; pinned by SHA-256 |
| `image FILE PIECE[+PIECE...] SHA256 CRC32 DESCRIPTION` | one ROM file written into the output folder: its pieces joined in socket order, pinned by SHA-256, with its CRC-32 and a description for the notice. A piece written `ARCHIVE#MEMBER` is a file inside a downloaded ZIP |
| `notice TEXT` | a line of `ROMS-README.txt` (origin, copyright holders, licences) |

The output folder gets the images and `ROMS-README.txt`, which repeats the notice and lists each file with its CRC-32 and its sources. The file names are the ones the ROM search expects (the presets' `sys12.bin`, `ext2x.bin`, the cards' `hbf.rom`, `card256.rom` and so on; [ui.md](ui.md#settings-file-and-rom-search)).

| File | Pieces (in socket order) | CRC-32 |
|---|---|---|
| `sys12.bin` | site `cas/odor/TVC12.024` + `TVC12.025` (MAME `tvc12_d4` + `tvc12_d3`) | 6DA8809A |
| `ext1x.bin` | site `cas/odor/TVC12.026` (MAME `tvc12_d7`, the 8 KiB socket image) | 1CBBEAC6 |
| `sys21.bin` | site `cas/odor/TVC21.020` + `TVC21.021` (MAME `tvc21_d6` + `tvc21_d4`) | 53C8FF98 |
| `sys22.bin` | site `cas/odor/TVC22.D6` + `TVC22.D4` (MAME `tvc22_d6` + `tvc22_d4`) | 79FA818C |
| `ext2x.bin` | site `cas/odor/TVC22.D7` (MAME `tvc22_d7`) | 05E1C3A8 |
| `hbf.rom` | site `bin/rom/hbf_eprom.rom`: the HBF disk card EPROM with VT-DOS 1.1 (the same image WinTVC ships) | AE34982B |
| `hbf12.rom` | site `cas/odor/FLPDOS12.PGM`: the disk card EPROM with VT-DOS 1.2 | 2ACF8477 |
| `vtdos11.rom` | site `bin/rom/vtdosrom.zip`, member `VT-DOS_1.1/VT-DOS11-CART.ROM`: the VT-DOS 1.1 cartridge | 40DDC741 |
| `vtdos12.rom` | site `bin/rom/vtdos12.rom`: the VT-DOS 1.2 cartridge | 2C10C420 |
| `gamecard.rom` | github.com/dikdom/TVC-GameCard at `56e9c2b`, `bin/TVC-GameCard-r1229.BIN` (MIT) | F782DB9B |
| `multichip.rom` | github.com/dikdom/TVC-Multichip-Soundcard at `fa23e43`, `source/firmware/sndmx-fw-v1.0.1.bin` (CC BY-NC-SA 4.0) | 704D1C27 |
| `nanosd.rom` | github.com/dikdom/TVC-NanoSD at `3fb88f2`, `bin/NanoSDROM-v0.33.bin` (BSD-3-Clause) | AF3C8EB7 |
| `multicart-en.rom`, `-hu.rom`, `-es.rom` | github.com/dikdom/TVC-Multicart-v2 at `631b251`, `software/firmware/multicart_v2.1.14{en,hu,es}.bin` (BSD-style) | 7C275018, 50B96903, FFB77D45 |
| `card256.rom` | github.com/dikdom/TVC256kplusplus at `4580b14`, `binaries/tvc-256k++.bin`, firmware 0.24 (MIT) | F759A66F |
| `card256-font.bin` | the same repository, `sources/firmware/TVC-IO/binaries/c16-hungarian.bin` | 5B6A2B88 |
| `card256-picture.bmp` | the same repository, `sources/firmware/TVC-IO/binaries/tvc-16cimage.bmp` | EA0740CD |
| `card256-flash.bin` | the same repository, `binaries/flash-root-1.3-bin.zip`, member `flash-root-1.3.bin` | E0FFC216 |
| `sddual.rom` | the Internet Archive's copy of `users.atw.hu/sdinterface/Firmware/sd_dual_2022_11_09.zip`, member `sd_dual_2022_11_09/vtdos_cr.bin` (no licence stated) | F758C797 |
| `sddual-gm.rom` | the same for `sd_dual_2022_11_09_gm.zip`, member `sd_dual_2022_11_09_gm/vtdos_cr.bin` (the "gamer mode" build) | 2CC250AF |

Notes on the list:
- The system images are the site's original EPROM dumps joined in socket order; they equal MAME's chips byte for byte, SYS 2.1 included. `cas/odor/TVC21.022`, an EXT found next to SYS 2.1, differs from EXT 2.x only in its last byte (FFFFh) ❓; like MAME, the 2.1 preset uses EXT 2.x.
- EXT 1.x arrives as the 8 KiB socket image: FFh in E000–EFFFh, the 4 KiB ROM in F000–FFFFh. `normalise_ext()` (`src/frontend/common/presets.cpp`) turns an 8 KiB EXT whose lower half is all FFh into the 4 KiB ROM (CRC-32 59C6B9F9) when it is loaded, so every source gives the same bytes and CRC-32 for snapshots and prompt states (`frontend.rom_search` checks it).
- VT-DOS versions follow the ROMs' own text ("VT-DOS CLI 1.1" / "1.2" at 1E6Bh): AE34982B is 1.1 and 2ACF8477 is 1.2. MAME (`d_tvcdos.128`) and the site's `bin/rom/romgyujtes/` collection label 2ACF8477 as 1.1; the site's `FLPDOS11.PGM`, `FLPDOS12.PGM` and `vtdosrom.zip` names agree with the text. MAME's "1.2" card EPROM F5C35597 and its cartridge FFD453DA repeat every even 2 KiB block in the odd one (address line A11 stuck) and are not used.
- Also on the site and not fetched: the UPM card EPROM (`cas/odor/UPM.FDR`, MAME `d_tvcupm.128`, B3A567AD; the ROM search accepts it as `hbf.rom`), `DOS1.FDR` (D839417A, VT-DOS 1.2 with 4 bytes changed at 2CC1h–2D1Eh; accepted as `hbf12.rom`) and the ZIP files of `bin/rom/romgyujtes/`.
- Every piece and every joined file is pinned by SHA-256; a file that does not match is downloaded or joined again. The site's certificate chain is incomplete, so the downloads do not verify TLS; the hashes vouch for the files. Plain HTTP on the site redirects to HTTPS.

### At configure time

`cmake/FreeTVCRoms.cmake`, off by default. With `-DFREETVC_FETCH_ROMS=ON` the configure step downloads the pieces into `build/<preset>/rom-downloads/` (with `file(DOWNLOAD … EXPECTED_HASH SHA256=…)`, ZIP members through `file(ARCHIVE_EXTRACT)`), joins the images into `build/<preset>/roms/`, and a post-build step copies that folder as `roms/` next to `freetvc-headless` and `freetvc`, where the ROM search finds it.

The same script fills any folder in script mode, for example `testdata/roms/` for the tests or `roms/` in the user data folder:

```sh
cmake -DFREETVC_ROM_OUT=$PWD/testdata/roms -P cmake/FreeTVCRoms.cmake
```

It keeps its downloads in `<dir>/downloads/` while it runs and removes them at the end.

### In the programs

`src/frontend/common/rom_fetch.{hpp,cpp}`, with the list compiled in:
- `find_curl()` looks for `curl` on PATH (on Windows `curl.exe`, also in the Windows system folder, where Windows 10 and later keep it), runs `curl --version` and takes it only if the answer starts with `curl `. Otherwise the message says what to install.
- `fetch_roms()` runs curl once per piece (`--fail --silent --show-error --location --insecure --max-time 60 --output`, no shell: `posix_spawn`, or `CreateProcessW` with `CREATE_NO_WINDOW`), unpacks ZIP members with `src/frontend/common/unzip.cpp` (the central directory, stored or deflated members through freeTVC's own inflate after RFC 1951, the member's CRC-32 checked), checks every piece and every joined image with freeTVC's own SHA-256, writes the images through a `.tmp` file and a rename, then `ROMS-README.txt`. A piece that does not match stops the fetch with nothing written.
- Headless `--fetch-roms` fetches into `--rom-dir` if given, else `roms/` in the user data folder, then exits. The user data folder is `$XDG_DATA_HOME/freeTVC` (else `~/.local/share/freeTVC`) on Linux, `%APPDATA%\freeTVC` on Windows, `~/Library/Application Support/freeTVC` on macOS.
- GUI: when the ROM search finds nothing at start, a message box offers *Download* into the user data folder's `roms/` (or says curl is missing) instead of quitting, then the search runs again. The menu item *Download ROMs* does the same on a thread while the machine runs, with a notice when done.

Verified: both paths write the same bytes and the same notice; with no curl, a program named `curl` that is not curl, and a curl that delivers wrong bytes, the fetch is refused and nothing is written; the Windows build under Wine with no curl ("not found") and with the official curl 8.23.0 for Windows (https://curl.se/windows/) into a folder with a space in its name gives the same bytes; the GUI's start-up box, then the machine booting from the downloaded ROMs; the menu item with the machine running; a fresh build tree with the option on boots every preset to `ok` and answers `PRINT 6*7` with those ROMs alone (no `--rom-dir`, no `FREETVC_ROM_DIR`, an empty user data folder), and autostarts a Flipper disk with the fetched `hbf.rom`; a corrupted piece and a corrupted image are replaced on the next configure; `hbf.rom` + `vtdos11.rom` and `hbf12.rom` + `vtdos12.rom` boot VT-DOS from the site's `vtdos12.dsk` and list it with `DIR`. `frontend.rom_fetch` checks SHA-256 against the FIPS 180-2 answers, the list's completeness, and its image hashes against the ROMs in `FREETVC_ROM_DIR`.

### Release packages

The repository holds no ROMs, and source builds fetch them only on request (the option or `--fetch-roms`). Release packages are to include the system ROMs and Videoton's card ROMs (HBF, VT-DOS) as separate files in `roms/` next to the programs, with the notice above (origin, copyright holders, CRC-32s), never compiled into a program. Card firmware under a licence that allows redistribution (MIT, BSD) ships under that licence; the Multichip firmware (CC BY-NC-SA 4.0, non-commercial) and the SD Dual firmware (no licence stated, contains VT-DOS) are not bundled: users fetch them. `tools/release/make_release.sh` builds the packages: a Linux tarball and a Windows zip with the programs, their data folders, `roms/` and the docs ([tools/release/README.md](../../tools/release/README.md)).

## Windows builds

`mingw-release` cross-builds `freetvc.exe`, `freetvc-headless.exe` and the test programs on Linux. The executables import only Windows system DLLs (checked with objdump). It was built with Ubuntu 24.04's MinGW-w64 GCC 13 (its default win32 thread model).

There is no test preset for it. The test programs run under Wine, in a Wine prefix of their own; `ctest -N -V` prints each test's command line and arguments:

```sh
ctest --test-dir build/mingw-release -N -V            # the commands, without running them
WINEPREFIX=$PWD/build/wineprefix wine build/mingw-release/tests/core_test.exe
WINEPREFIX=$PWD/build/wineprefix wine build/mingw-release/tests/boot_test.exe testdata/roms 64k+-2.2
```

Results under Wine: `core_test`, `bus_test`, `video_test`, `input_test`, `keymap_rom_test`, `host_input_test`, `sound_test`, `debug_test`, `expression_test`, `disasm_test`, `skin_test`, `floppy_test`, `golden_test` (one scene) and the remote tests (`monitor_test`, `mcp_test`, `dzrp_test`, `gdb_test`, with Winsock) pass; `sst_test` (97 000 tests), `typing_test` and `boot_test` (all five presets) give output byte-identical to Linux, `sound_rom_test` the same numbers; headless output, PPM screenshots and WAV files are byte-identical to Linux; `freetvc-headless.exe --moncommands` and an MCP stdio session (binary stdout, `\n` line ends) work; the GUI window looks the same as on Linux. `remote.png` is not registered in a cross build (it runs the test program under the host's Python).

The MSVC presets have not been built.

## Tests

Tests are plain programs: exit code 0 passes, non-zero fails, and 77 means skipped because the test data is missing (CTest `SKIP_RETURN_CODE 77`). There is no test framework.

### Labels

| Label | Tests | Data |
|---|---|---|
| `core` | units and machine checks on hand-made programs | none, or the shipped `data/` files |
| `z80` | the CPU against SingleStepTests and ZEX | `FREETVC_SST_DIR`, `FREETVC_ZEX_DIR` |
| `slow` | ZEXDOC and ZEXALL (also `z80`); several minutes each in a release build | `FREETVC_ZEX_DIR` |
| `rom` | boots and programs on the real system and card ROMs | `FREETVC_ROM_DIR` and others |
| `golden` | frames compared with WinTVC's | `FREETVC_GOLDEN_DIR` |

`ctest -L rom` (or `-LE rom`) selects by label.

### Test data

Test data is never committed. It lives in the git-ignored `testdata/` folder (`FREETVC_TEST_DATA`, default `<source>/testdata`). The variables below are set from `FREETVC_TEST_DATA` on the first configure of a build tree; each can also point elsewhere (set it, or delete `build/<preset>/CMakeCache.txt` to start over). Tests whose data is missing are skipped. [developer-guide.md](../developer-guide.md) has the download steps.

| Cache variable | Default | Expects | Public source |
|---|---|---|---|
| `FREETVC_ROM_DIR` | `testdata/roms`; `build/<preset>/roms` when `FREETVC_FETCH_ROMS=ON` and `testdata/roms` does not exist | the autofetch's output: `sys12.bin`, `sys21.bin`, `sys22.bin`, `ext1x.bin`, `ext2x.bin` and the card ROMs | `cmake -DFREETVC_ROM_OUT=$PWD/testdata/roms -P cmake/FreeTVCRoms.cmake`, [above](#rom-autofetch) |
| `FREETVC_DISK_ROM` | `${FREETVC_ROM_DIR}/hbf.rom` | the HBF card EPROM, VT-DOS 1.1 | the autofetch |
| `FREETVC_DISK_IMAGE` | `testdata/Flipper.dsk` | a 360 KiB VT-DOS disk image whose root holds `FLIPPER.CAS` as its first CAS file and `FLIPKOD.CAS` (the Flipper pinball game's loader and code) | no download is pinned; any VT-DOS image of the game with those files works |
| `FREETVC_CARD256_ROM` | `${FREETVC_ROM_DIR}/card256.rom` | the TVC256k++ card ROM, firmware 0.24 | the autofetch |
| `FREETVC_NANOSD_ROM` | `${FREETVC_ROM_DIR}/nanosd.rom` | the NanoSD EEPROM, ROM v0.33 | the autofetch |
| `FREETVC_SDDUAL_ROM` | `${FREETVC_ROM_DIR}/sddual.rom` | the SD Dual firmware 2022.11.09 | the autofetch |
| `FREETVC_SDDUAL_FILES` | `testdata/sdinterface` | optional: `sd_dual_2022_11_09_gm/sd_dual_2022_11_09_gm/SDFLASH.CAS` (that ZIP unpacked into a folder of its name) and `system_f_v2/` (`system_f_v2.zip` unpacked: `AUTOEXEC.BAT`, `BDOS.SYS`, `COMMAND.COM`, `sdc/`) | the Internet Archive's copy of the author's site: `sd_dual_2022_11_09_gm.zip` at the URL in `data/roms.txt`, and https://web.archive.org/web/20250516092435id_/http://users.atw.hu/sdinterface/Tools/system_f_v2.zip. No licence stated |
| `FREETVC_MULTICART_DIR` | `testdata/TVC-Multicart-v2` | the repository's `software/tvcpla/tvcpla.bin` and `software/firmware/multicart_v2.1.14en.bin` | https://github.com/dikdom/TVC-Multicart-v2 at commit `631b251d131e6d0782aa7192fd18f08c0134527e` (BSD-style licence) |
| `FREETVC_SST_DIR` | `testdata/sst-z80/v1` | `NAME.json` for the 97 names in `tests/z80/sst_subset.txt` (about 77 MiB) | https://github.com/SingleStepTests/z80 at commit `ebe1875d48f374bcfd4b505d8eb8ee751568b5f7`, folder `v1/` (MIT) |
| `FREETVC_ZEX_DIR` | `testdata/zex` | the CP/M `zexdoc.com` and `zexall.com` (8 585 program bytes each) | https://github.com/floooh/chips-test at commit `3785836e76c43922f78a50e1f8adfed259ab9672`, `tests/roms/` (ZEX by Frank D. Cringle, GPL-2.0-or-later) |
| `FREETVC_GOLDEN_DIR` | `testdata/golden` | `<preset>/<scene>.json` (WinTVC's video state), `.vram` (16 KiB), `.pgm` (WinTVC's F2 screenshot as colour indexes) | captured from WinTVC 1.4.1; the captures and the capture tool are not published, so the `golden.*` tests are skipped elsewhere |
| `FREETVC_SJASMPLUS` | `find_program(sjasmplus)` on `PATH` (or a project-local `.tools/sjasmplus-1.24.0/bin`, git-ignored) | sjasmplus 1.24.0 | https://github.com/z00m128/sjasmplus |

`remote.headless_script` is registered only when `${FREETVC_ROM_DIR}/sys22.bin` exists at configure time, so re-run the configure after fetching the ROMs.

### Test list

Preset lists: "(5)" runs on `32k-1.2`, `32k+hbm-1.2`, `64k-1.2`, `64k+-2.1` and `64k+-2.2`; "(3)" on `32k-1.2`, `64k-1.2`, `64k+-2.2`; "(2)" on the two named.

**CPU and core** (label `core` unless noted; no data):

| CTest name | Program | What it checks |
|---|---|---|
| `core` | `core_test` | Z80 adapter power-on state and pin helpers; HALT exit by INT (IM 1) and NMI (halted M1 at HALT+1, forced NOP, R, return address, INTA/vector ticks); Machine stop at an instruction count and a tick budget with Zilog T-state sums; determinism; `power_on()` |
| `core.bus` | `bus_test` | paging fields and the page-0 `11` / U2 / U3 rules per model, VID in page 1, 0Ch–0Fh mirrors, IOMEM + EXT 4/8 KiB, ROM writes, I/O read values (59h, 5Ah, A7, CRTC readback), keyboard matrix, VRAM stretch (M1 and MR timing, the first instruction included), IRQ flip-flop clear by 07h; cartridges (plain windows and bank switches, KiloCart read register, MegaCart write register, reset vs power-on, debugger accesses that do not page); a MegaCart snapshot round trip, PackBits, a synthetic WinTVC `.tvs` import |
| `core.video` | `video_test` | frame period 62 800 T, cursor IRQ once per frame at row 59 / raster 3, R10 cursor off, 2/4/16-colour decode, palette mirrors, the VRAM address formula, border, HSYNC/VSYNC blanking, all with the ROM's CRTC values |
| `core.input` | `input_test` | ghosting (the OS book's A + D + E → Q), joystick rows, rows 10–15, key names, UTF-8, typing event order (SHIFT a frame early and late, the pause after RETURN) |
| `core.sound` | `sound_test` | the first sound IRQ on the 8th reload after `INSTART`, then every 16; b5 gating, and no IRQ when b5 is turned on with b3 high; P = 4095 stop and restart; pitch change at the reload; power-on count; levels and DAC; synth step centre and exact DC, square-wave area and period, high-pass decay, the 2 s cap, chunked and whole runs bit-identical; a Z80 program through the bus (IRQ timing after `IN A,(5Bh)`, 07h clear, the 5Fh mirror, reset); DAC output and `audio_rate` 0 |
| `core.cas` | `cas_test` | CAS parsing (warnings for a wrong record count and length, short files), exact headers from `build_cas`, the ROM's file-name conversion |
| `core.tape` | `tape_test` | tape signals and the deck without a ROM, including TTP files with 256 or 257 bytes per full sector |
| `core.floppy` | `floppy_test` | disk image geometry and FAT12 reading; every WD1793 command through the HBF card's ports with its timing, lost data, write protection, not ready, side compare, multiple records, write track, force interrupt, a seek out at TR00; a copy and a snapshot of a card mid-read; port 5Ah, IOMEM paging, card snapshots |
| `core.gamecard` | `gamecard_test` | the SN76489A measured in the machine's audio (pitch, level, 2 dB step, period 0, the clock gate, periodic and white noise), joystick ports and rows 10–11, EPROM paging, reset, an empty socket, a snapshot and a copy mid-tone |
| `core.card256` | `card256_test` | the TVC256k++ register file and its ports, fast RAM in place of U2/U3, PSRAM paging and shared pages, ROM banks, the border snoop, reset and reboot, the memory functions, a snapshot |
| `core.card256_overlay` | `card256_overlay_test` | power-on pictures, mixing and colours, border and 16-colour latching, A8h/A9h timing and the held pixel, sprites and priority, collisions with the firmware's quirks, the slot IRQ into 59h and the CPU, 16-colour text, extended border and scroll, the text and drawing functions, a snapshot mid-frame, reset |
| `core.card256_storage` | `card256_storage_test` | the D6h commands on card RAM, the four drives, the C0h file functions, function 15, the mouse registers, an open file across a snapshot (the pendrive as a FAT image) |
| `core.card256_vsid` | `card256_vsid_test` | the vSID engine's sample pipeline (blocks, the write queue and its overflow, the one-block delay), waveforms, envelope timing, read-back registers; the card on the machine's audio (pitch, a warm reset that keeps playing, a reboot that stops, a snapshot mid-note) |
| `core.multichip` | `multichip_test` | the chip-clock conversion; the SN at 3.579545 MHz; the SAA1099's pitch, buffered octave, sync, noise, envelopes (internal and external clock, 3-bit); the YM3812's pitch and timer on the slot /INT line (port 59h), status, reset; the bank latch; a snapshot and a copy with all three chips and a running timer |
| `core.nanosd` | `nanosd_test` | the latches, the Nano's commands as the ROM sends them, the SD library's view of a FAT volume, the firmware's quirks, card detect, snapshots |
| `core.multicart` | `multicart_test` | the PLA against the board's `tvcpla.bin` (all 131 072 inputs; skipped with a note when `FREETVC_MULTICART_DIR` lacks it), the register and its reset-on-read latch, the RAM, the SST39SF040's commands and status, snapshots, the image builder |
| `core.sddual` | `sddual_test` | the SD card's SPI protocol (CRCs, initialisation, CSD and CID, single and multiple block reads and writes), the board's registers and windows, its flash with and without the jumper, snapshots and copies, the image builder |
| `core.serial` | `serial_test` | the printer port's flip-flops, the 8251A's timing on TXRXCLK, the handshake, copies and snapshots |
| `core.sid_card` | `sid_card_test` | the ports with and without the card; OSC3, ENV3 and the fading data bus; a 440 Hz sawtooth on the 6581 and the 8580; identical samples from a copy and from differently sliced runs; an 8580 snapshot that continues sample for sample and saves again byte-identical; reset |
| `z80.sst_subset` (`z80`) | `sst_test` | the 97-file SingleStepTests subset: final registers (Q included), RAM, length, refresh addresses, and each bus access's order, address, data and T-state offset. Known deviations of `z80.h` go into a strict expected-fail table, where a deviation that disappears fails the test too; the table is empty. Data `FREETVC_SST_DIR` |
| `z80.zexdoc`, `z80.zexall` (`z80`, `slow`) | `zex_test` | the CP/M originals on a flat 64 KiB RAM with BDOS functions 2 and 9 trapped at 0005h; passes on 67 × "OK", no "ERROR" and "Tests complete". Each takes 46 734 975 919 T. Data `FREETVC_ZEX_DIR` |

**Debugger and remote** (label `core`):

| CTest name | Program | What it checks |
|---|---|---|
| `debug.core` | `debug_test` | the debug core on hand-assembled programs in all-RAM mode: checkpoints, stepping, trace, saved states |
| `debug.expression` | `expression_test` | numbers in each radix, C precedence, registers, memory, banks, hits, symbol files |
| `debug.source_map` | `source_map_test` | source maps from real debug files: sjasmplus 1.24.0 SLD and listing, SDCC 4.2.0 NoICE/CDB/map, z88dk 2.4 maps |
| `debug.disasm` | `disasm_test` | disassembler spot checks |
| `debug.disasm_sjasmplus` | `disasm_test` | every opcode family disassembled, assembled back with sjasmplus and compared byte for byte; skipped without `FREETVC_SJASMPLUS` |
| `remote.monitor` | `monitor_test` | text monitor commands on a RAM program, a script through the hub, a real TCP client |
| `remote.mcp` | `mcp_test` | both MCP protocol eras and the tools on a RAM program, HTTP over a real socket, and BASIC on the ROM (that part skipped without the 2.2 ROMs in `FREETVC_ROM_DIR`) |
| `remote.dzrp` | `dzrp_test` | every supported DZRP command, then a session over TCP |
| `remote.gdb` | `gdb_test` | GDB packets, acks, registers, memory and banks, breakpoints, watchpoints, Ctrl-C, `qRcmd`, then real TCP |
| `remote.png` | `png_test` + `tests/remote/png_check.py` | PNG screenshots of test framebuffers decoded by Python's zlib (an independent inflater): chunks, CRCs, header, palette, every pixel. Needs Python 3; not registered in cross builds |

**Front end** (label `core`):

| CTest name | Program | What it checks |
|---|---|---|
| `frontend.rom_search` | `rom_search_test` | the order of the ROM places, first match wins, file names first, then CRC-32 under any name, `normalise_ext()`; skipped without ROMs in `FREETVC_ROM_DIR` |
| `frontend.rom_fetch` | `rom_fetch_test` | SHA-256 against its known answers, the built-in `data/roms.txt` is whole, and its image hashes equal those of the ROMs in `FREETVC_ROM_DIR` (that part left out without them). No network |
| `frontend.card256_host` | `card256_host_test` | the TVC256k++ pendrive as a host folder: names without case, 8.3 aliases, code page 437, the file operations |
| `frontend.settings` | `settings_test` | `freetvc.ini` parsing, the portable place, rewriting keys in place |
| `headless.version` | `freetvc-headless --version` | prints exactly `freeTVC <version>` |
| `headless.bad_option` | `freetvc-headless --no-such-option` | must fail |
| `ui.skin` (GUI builds) | `skin_test` | `data/skins/bronze.skin` equals the built-in skin, every value form parses, bad lines are reported with their line number |
| `ui.picture` (GUI builds) | `picture_test` | the shipped picture files in `data/pictures` load and are named after their files, `pal-tv.picture` names every key, `picture_text()` reads back, out-of-range values and unknown keys are reported with their line number |
| `ui.keymap` (GUI builds) | `keymap_test` | `data/keymaps/hungarian.keys` equals the built-in layout, `us.keys` loads, every host key and TVC key name round-trips, `keymap_text()` reads back (`SEMICOLON`, joystick 2, unnamed keys left out), bad lines are reported, rebinding, unbinding and a forced SHIFT. `keymap_test --print` writes the built-in layout as a file |
| `ui.translation` (GUI builds) | `translation_test` | the `.po` reader, the language choice, `tr()` and `tr_format()`, `msg()` against printf, messages rebuilt in the UI language, and the shipped catalogs in `data/lang`, which must read without errors |
| `ui.translation_coverage` (GUI builds with Python 3) | `tools/lang/po_check.py check src data/lang/*.po` | every translatable string in `src/` (`tr()`, `trc()`, `tr_format()`, `N_()`, `msg()`, the strip's notices) has a translation in every catalog, with the same printf conversions. A new user-facing string therefore needs its `data/lang/hu.po` entry ([tools/lang/README.md](../../tools/lang/README.md)) |

**On the ROMs** (label `rom`; data `FREETVC_ROM_DIR` unless noted):

| CTest name | Program | What it checks |
|---|---|---|
| `boot.<preset>` (5) | `boot_test` | cold boot, SPACE at frame 330, then the OS screen copy (0100h) must show "TV COMPUTER BASIC x.y" and "ok", and the frame must hold drawn text |
| `debug.rom.<preset>` (5) | `debug_rom_test` | an attached debugger changes nothing (same ticks, RAM and frame), checkpoints see the OS at work, step out and step over follow the RST 30h convention; prints boot times with and without the debugger |
| `snapshot.rom.<preset>` (5) | `snapshot_rom_test` | a TVCS snapshot taken mid-frame with a key held and a tone on, written and decoded, loaded into a freshly powered machine: the same registers, RAM, VRAM, framebuffer and audio samples over 187 more frames; saving after the load gives the same bytes; another board and a truncated file are refused |
| `input.keymap_rom` | `keymap_rom_test` | the character map against the key tables of SYS 1.2, 2.1 and 2.2, both directions |
| `input.typing.<preset>` (5) | `typing_test` | types all 113 printable map characters at the BASIC prompt, compares the screen copy byte for byte, then `PRINT 6*7` must answer 42 |
| `input.host` (GUI builds) | `host_input_test` | SDL's dummy video driver with its US keymap: symbolic typing of all 95 printable ASCII characters plus Backspace, positional keys, SHIFT ordering, virtual gamepads on rows 8, 9 and 10–11 |
| `sound.basic.<preset>` (5) | `sound_rom_test` | types `SOUND PITCH 3950,VOLUME 15,DURATION 50`; the audio must hold one burst of 970–1040 ms after the typing, its cycles averaging 35.881 samples (16 × 146 T at 48 kHz, within 0.005), every cycle within 0.2; P = F6Eh and the tone gate off at the end |
| `media.rom.<preset>` (5) | `media_rom_test` | autostart with and without autorun; the restored prompt state ends in the cold boot's exact state; a refused header fails; `SAVE`/`LOAD` through a folder with the saved bytes compared; the OS book's editor merge and the error exit's default assignment |
| `media.tape.<preset>` (2: `64k-1.2`, `64k+-2.2`) | `tape_rom_test` | the ROM's own `LOAD` reads a CAS played as a tape (autorun, RUN, several sectors), a damaged tape fails, `SAVE` records the signal freeTVC's encoder writes |
| `media.disk.<preset>` (3) | `disk_rom_test` | disk autostart of Flipper with its code (`FLIPKOD.CAS`) found in RAM; a snapshot taken during an FDC transfer runs on identically; `SAVE` to a blank disk, read back through FAT12 from the host file, then `LOAD`. Data also `FREETVC_DISK_ROM`, `FREETVC_DISK_IMAGE` |
| `card256.rom.<preset>` (3) | `card256_rom_test` | the card ROM's `EXT 0`, `VER`, `DIR`, `HELP` (ROM bank 1) and `X`; a 32k board with a 64k board's free RAM. Data also `FREETVC_CARD256_ROM` |
| `nanosd.rom.<preset>` (3) | `nanosd_rom_test` | the NanoSD ROM v0.33's menu, LOAD/SAVE and `EXT 0` on a host folder. Data also `FREETVC_NANOSD_ROM` |
| `serial.rom.<preset>` (3) | `serial_rom_test` | `LPRINT` and `LLIST`, an unplugged printer stopped with STOP, `LIST #6`, and `SAVE #6` into `LOAD #6` on a second machine |
| `sddual.rom.<preset>` (2: `64k+-2.2`, `64k-1.2`) | `sddual_rom_test` | SAVE from BASIC onto a FAT card freeTVC's builder made, LOAD and RUN after a power-on, the VT-DOS CLI's `DIR` on both sockets with an SDHC card in the second; with `FREETVC_SDDUAL_FILES` also the firmware updater with and without the flash jumper and a VT-DOS boot from the card. Data also `FREETVC_SDDUAL_ROM` |
| `multicart.rom.<preset>` (2: `64k-1.2`, `64k+-2.2`) | `multicart_rom_test` | the MultiCart v2 firmware 2.1.14's menu with the chips' IDs and the RAM size, a cart started from it, the reset-on-read latch both ways, SAVE through `EXT 0` into the flash, the saved program run, a delete. Data `FREETVC_MULTICART_DIR` (the firmware file) |
| `headless.snapshot_no_preset` | `tests/frontend/headless_snapshot.cmake` | a snapshot saved with `--preset` loads in a run without one (the ROM search exists without a preset); skipped (`SKIP:`) without ROMs |
| `remote.headless_script` | `freetvc-headless --moncommands tests/remote/boot_call.mon` | the command-line monitor path on the 2.2 ROM: stop at the boot's first output system call, show it (`RST 30h : DB 71h`), quit with code 0 |

**Golden frames** (label `golden`; data `FREETVC_GOLDEN_DIR`):

| CTest name | Program | What it checks |
|---|---|---|
| `golden.<preset>.<scene>` (16) | `golden_test` | rebuilds WinTVC's frozen video state (VRAM bank, CRTC R0–R15, palette, border, mode) in a machine whose CPU spins on `DI / JR $`, renders 3 frames and compares colour indexes with WinTVC's F2 framebuffer at the fixed crop offset x 136, y 24. Presets `32k-1.2`, `32k+hbm-1.2`, `64k-1.2`, `64k+-2.2`; not `64k+-2.1`, because WinTVC's 2.1 preset does not reach BASIC ([machine.md](machine.md)). Scenes `logo` (GIF rows 193–204, which the OS rewrites while the beam scans them, are reported but not judged: `--skip-rows 193-204`), `prompt`, `mode2`, `mode16`. On a failure it prints the best offset, the area and the colour pairs |

### Not used as tests

- YATE's TVC build of ZEXDOC/ZEXALL (`zexdoc_tvc.cas`, `zexall_tvc.cas`, github.com/hightower70/YATE): its 69 expected CRCs are the CP/M ones, but its memory operand `msbt` sits at 1A06h instead of 0103h, so the 28 tests that load `msbt`-based pointers into a register cannot match on any correct Z80. It could run inside the emulated TVC once its CRCs are regenerated for `msbt` = 1A06h.
- The full SingleStepTests set (1 604 files, 1 374.6 MB in `v1/` at the commit above); the 97-file subset has one file per machine-cycle pattern.

### How the tests were checked

Each test was checked against deliberate defects (mutations applied to a scratch copy, built, run and reverted). Each of these fails:
- `z80.h` with the `LD (BC),A` write one tick late: all 1 000 tests of SST file `02` fail as timing.
- A fake expected-fail entry for `00` fails as "no longer occurs". With the unpatched `z80.h`, removing its expected-fail entry for `37` gave 229 unexpected failures.
- A halted fetch with `pc++`: `core_test`, 5 checks. No forced NOP: `core_test`, INC A executed. POP AF setting Q: SST `f1`, 994 failures. Inverted I/O-repeat parity: `ed b2` 995, `ed bb` 1 000. No repeat X/Y flags: `ed b0` 749, `ed b1` 727.
- MR/MW stretch phase taken at T2: `bus_test`, first instruction 32 instead of 30 ticks (both variants settle at 28 ticks per instruction, so the test checks the first instruction). The 64k page-0 `11` giving U3: `bus_test`. Only 0Fh decoded for video paging: `bus_test`, 6 checks.
- 4-colour bit order swapped: `video_test`, 6 pixels. One extra adjust line: `video_test`, frame and cursor periods 126 000 ticks.
- No ghost propagation: `input_test` (row 2 FDh, expected BDh). Symbolic mode never forcing SHIFT: `host_input_test`, 9 wrong characters and the SHIFT order. "@" on the wrong key: `keymap_rom_test` on all 3 ROMs, `typing_test` 1 wrong character. SHIFT pressed on the key's frame in `type_text`: `input_test`. A positional Y/Z key moved: `host_input_test`, 1 wrong character. SHIFT applied in the same step as the key fails only the SHIFT-order check (the 95-character typing still passes, because the update phase never lands mid-scan).
- The sound IRQ on the falling edge of b3: `sound_test`, 7 checks. A period of 4095 − P: `sound_test`, 14 checks, and `sound_rom_test` (35.635 samples). `INSTART` not clearing the chain: `sound_test`. A pitch write taking effect at once: `sound_test`, 3 checks. The synth kernel without its half-sample offset: `sound_test`, the step centre. `available()` ignoring the read position: `sound_test`, the cap.
- Altered golden captures: the picture shifted right by 1 pixel, 2 550 pixels; a wrong border index, 57 984; one VRAM bit flipped, 2; a wrong mode, 6 525; a capture that hit the logo race judged without `--skip-rows`, 8. The crop offset came out of the search on every capture, so the test uses it as a constant and a shifted picture fails.
- `boot_test` without the key: the screen copy stays empty after 450 frames, so the test cannot pass on a stale screen.
- `debug.disasm_sjasmplus` with a deliberately wrong mnemonic: 24 failed checks.

Findings from the test runs that shaped the code:
- Typing: 1 frame held and 1 frame between keys types correctly at the prompt. The key after RETURN is lost below about 15 frames for a 47-character program line, hence the default of 25 frames after RETURN (`--type-timing 2:2:25`).
- HostInput waits one frame period of machine time between SHIFT and the key, not for frame flips: with an unprogrammed CRTC no frame flips, and the key would never go down.

### Checks outside CTest

Some behaviour can only be checked through the real programs. The scripts are in `tools/`:
- GUI: `tools/ui/ui_drive.py` drives the real window with XTEST keys and clicks and grabs it, on a virtual X display ([tools/ui/README.md](../../tools/ui/README.md)).
- Sound: `tools/sound/` records the GUI's audio while typing into it, and analyses WAV cycles and dropouts ([tools/sound/README.md](../../tools/sound/README.md)). The GUI's `SOUND PITCH 3950,VOLUME 15,DURATION 50` gives one burst of 1010–1020 ms with every cycle 35.83–35.91 samples (1337.76 Hz), the same as the headless WAV; with `--no-audio` the sink gets exact silence.
- MCP: `tools/mcp/inspector_check.sh` with the official MCP Inspector, and `tools/mcp/stress_check.py` ([tools/mcp/README.md](../../tools/mcp/README.md)).
- DZRP and the GDB stub: `tools/dezog/` (DeZog built from source with a patch) and `tools/gdb/gdb_check.sh` (GNU gdb built for the Z80).
- `tools/z80eval/`: the stand-alone CPU harnesses used to choose the Z80 core ([z80-core.md](z80-core.md)).

There is no CI configuration.

## The programs

The command-line options of both programs are in [user-guide.md](../user-guide.md) and their `--help` text. Facts that concern the build and the tests:
- `freetvc-headless` runs a budget (`--tstates`, `--frames`, `--instructions`) in frame-sized slices and prints the StopInfo, the completed frame count and all registers. Exit codes: 0 OK, 1 runtime error, 2 usage error. Addresses and values are hexadecimal (`8000`, `0x8000`, `8000h`). It needs `--preset` (or `--sys`/`--ext`) to have ROMs; without one the machine is a 64k+ with no ROMs (FFh). `--screenshot` writes an 800 × 314 binary PPM, `--wav` a 48 kHz 16-bit mono WAV.
- `freetvc` (the GUI) is paced by the audio device: each pass tops the SDL audio stream up to about 60 ms of samples. With `--no-audio`, or when no device opens, the wall clock paces it (6.25 M ticks per second). Either way there are at most two frames of catch-up per host frame ([machine.md](machine.md)). `--frames N` exits after N host frames, for smoke checks.

Example: `freetvc-headless --preset 64k+-2.2 --rom-dir testdata/roms --frames 900 --key SPACE@330 --type 'PRINT "Árvíztűrő"\n@400' --screenshot basic.ppm`.
