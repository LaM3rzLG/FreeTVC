# Building and developing on Windows

Everything a developer (or an AI coding agent) working on Windows needs to know beyond the [developer guide](developer-guide.md), which stays the main reference for the code, the tests and the rules. Read this page first, then the guide.

## What is tested and what is not

| | State |
|---|---|
| Windows `.exe` files cross-built on Linux with MinGW-w64 (preset `mingw-release`) | built with 0 warnings; the test programs and the GUI run under Wine |
| A checkout with Windows line endings (CRLF) | checked on Linux: every text file converted to CRLF, then configure, build, all tests, the GUI loading its translation, skin, picture and key layout files, and the ROM autofetch script; all pass |
| A native build on Windows with MSYS2 (GCC) | **not tested.** It is the same compiler family as the tested cross build, so it is the route most likely to work unchanged |
| A native build with Visual Studio (MSVC) | **not tested.** The presets `msvc-debug` and `msvc-release` exist; MSVC has never compiled this code, so expect warnings and possibly small fixes (see [MSVC: what to expect](#msvc-what-to-expect)) |
| Running the programs on real Windows | **not tested**; only under Wine |

When you build natively for the first time, treat it as a porting task: keep the full configure and build logs and the `ctest` output, fix what breaks, and update this page with what you found.

## Choosing a route

| Route | Use it when | Notes |
|---|---|---|
| [A. MSYS2 UCRT64 (GCC)](#route-a-msys2-ucrt64-gcc) | you want a native Windows build with the least friction | GCC 13+ like the tested cross build; static `.exe` files |
| [B. Visual Studio (MSVC)](#route-b-visual-studio-msvc) | you work in Visual Studio or need MSVC | the `msvc-*` presets; untested code paths in MSVC |
| [C. WSL 2](#route-c-wsl-2) | you want the fully tested setup | the Linux instructions work unchanged inside WSL; the `mingw-release` `.exe` files then run directly on Windows |

All three share the steps in [Getting the source](#getting-the-source), [ROMs](#roms) and [Running and debugging](#running-and-debugging).

## Getting the source

```bat
git clone https://github.com/LaM3rzLG/FreeTVC.git C:\src\freeTVC
cd /d C:\src\freeTVC
```

- **Git for Windows** (https://git-scm.com/download/win) or MSYS2's `git`.
- **Line endings:** `.gitattributes` sets `* text=auto` with `*.sh`, `*.py` and `*.patch` always LF. Keep Git's default line-ending handling; every parser in freeTVC and the CMake scripts accept CRLF (checked as described above). Do not convert the `.sh` and `.py` files to CRLF: bash and some Python tools reject them.
- **Short path:** clone close to the drive root (for example `C:\src\freeTVC`). The SDL3 build creates deep folders under `build\<preset>\_deps\`, and very long paths can hit Windows' 260-character limit in older tools ❓.
- The repository has no symbolic links and no submodules.

## Route A: MSYS2 UCRT64 (GCC)

1. Install MSYS2 from https://www.msys2.org (default folder `C:\msys64`) and update it as its page describes (`pacman -Syu`, repeated until nothing is left).
2. Open the **"MSYS2 UCRT64"** shell (not "MSYS2 MSYS": its CMake reports the host as `MSYS`, not `Windows`, and builds for the POSIX layer). Install the toolchain:

   ```sh
   pacman -S --needed git mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja mingw-w64-ucrt-x86_64-python
   ```

   Package names follow MSYS2's `mingw-w64-ucrt-x86_64-*` scheme ([MSYS2: CMake](https://www.msys2.org/docs/cmake/)); `python` is needed for two tests (`remote.png`, `ui.translation_coverage`).
3. `CMakePresets.json` has no MSYS2 preset. Put this file in the repository root as **`CMakeUserPresets.json`** (git-ignored); it inherits the project's own `base` and `test-base` presets. It was checked to parse and to configure all 109 tests (on Linux, with its host condition switched; the Windows build itself is untested):

   ```json
   {
     "version": 6,
     "configurePresets": [
       {
         "name": "msys2-base",
         "hidden": true,
         "inherits": "base",
         "condition": { "type": "equals", "lhs": "${hostSystemName}", "rhs": "Windows" },
         "cacheVariables": { "CMAKE_C_COMPILER": "gcc", "CMAKE_CXX_COMPILER": "g++" }
       },
       { "name": "msys2-debug", "displayName": "Windows MSYS2 UCRT64 GCC, Debug", "inherits": "msys2-base", "cacheVariables": { "CMAKE_BUILD_TYPE": "Debug" } },
       { "name": "msys2-release", "displayName": "Windows MSYS2 UCRT64 GCC, Release", "inherits": "msys2-base", "cacheVariables": { "CMAKE_BUILD_TYPE": "Release" } }
     ],
     "buildPresets": [
       { "name": "msys2-debug", "configurePreset": "msys2-debug" },
       { "name": "msys2-release", "configurePreset": "msys2-release" }
     ],
     "testPresets": [
       { "name": "msys2-debug", "inherits": "test-base", "configurePreset": "msys2-debug" },
       { "name": "msys2-release", "inherits": "test-base", "configurePreset": "msys2-release" }
     ]
   }
   ```

4. Build and test, in the UCRT64 shell from the repository root:

   ```sh
   cmake --preset msys2-release -DFREETVC_FETCH_ROMS=ON
   cmake --build --preset msys2-release
   ctest --preset msys2-release
   ./build/msys2-release/src/freetvc.exe
   ```

- `CMakeLists.txt` links every program with `-static` when the compiler is MinGW, so `freetvc.exe`, `freetvc-headless.exe` and the test programs need no MinGW DLLs and run outside the MSYS2 shell too (as the cross-built ones do).
- The warning flags are the GCC ones (`-Wall -Wextra -Wpedantic -Wshadow`); the cross build gives 0 warnings with GCC 13.
- MSYS2 also offers Clang (`mingw-w64-ucrt-x86_64-clang`); not tried.

## Route B: Visual Studio (MSVC)

1. Install **Visual Studio 2022** (Community is enough) with the **"Desktop development with C++"** workload. It includes MSVC, CMake and Ninja. Visual Studio 2019 works only from version 16.8 ([MSVC C11 support](https://learn.microsoft.com/en-us/cpp/build/reference/std-specify-language-standard-version)).
2. Make sure a **Windows SDK 10.0.20348.0 or later** is installed (Visual Studio Installer, "Individual components"; any Windows 11 SDK qualifies). The one C file (`src/core/z80/z80_chips.c`, the Z80 core) is compiled as C11, and MSVC's C11 mode needs that SDK's updated C runtime headers ([Microsoft: install C11 and C17 support](https://learn.microsoft.com/en-us/cpp/overview/install-c17-support)).
3. Install **Python 3** (https://www.python.org, or `winget install Python.Python.3.12`) for the two Python-based tests; CMake finds it with `find_package(Python3)`.
4. Open **"x64 Native Tools Command Prompt for VS 2022"** (the presets use the Ninja generator with `"architecture": { "strategy": "external" }`, so the compiler environment must come from this prompt), then:

   ```bat
   cd /d C:\src\freeTVC
   cmake --preset msvc-debug -DFREETVC_FETCH_ROMS=ON
   cmake --build --preset msvc-debug
   ctest --preset msvc-debug
   build\msvc-debug\src\freetvc.exe
   ```

   In the Visual Studio IDE: *File → Open → Folder* on the repository; Visual Studio reads `CMakePresets.json` and offers `msvc-debug` and `msvc-release` in its configuration list. Visual Studio Code with the CMake Tools extension works the same way from the Native Tools prompt (`code .`).

### MSVC: what to expect

These points come from reading the code and the build files; none has been seen on a real MSVC build.

- **Flags:** freeTVC's own targets get `/W4 /utf-8`, and for C++ `/permissive- /Zc:__cplusplus` (`cmake/FreeTVCWarnings.cmake`). `/utf-8` matters: many sources and tests contain UTF-8 text (Hungarian strings, ❓ in comments and messages). `-DFREETVC_WERROR=ON` adds `/WX`; leave it off until the warnings are cleared. Third-party targets keep MSVC's defaults; their GCC-only flags are behind compiler checks in `third_party/CMakeLists.txt`.
- **Warnings likely at `/W4`** that GCC does not give: C4996 ("unsafe" CRT functions such as `getenv`, `fopen`, `strerror`), C4244/C4267 (narrowing conversions, e.g. `size_t` to `int`), C4100 (unused parameters). Fix them in the code (`static_cast`, `[[maybe_unused]]`), not with project-wide suppressions; if a warning class has to be silenced, add it to `freetvc_warnings()` with a one-line reason.
- **printf-style checking:** `FREETVC_PRINTF` (`src/core/message.hpp`) is empty on MSVC, so wrong `msg()`/`tr_format()` arguments are not caught there; the Linux build still checks them.
- **Windows code paths** exist in five files, all already compiled by MinGW: `src/remote/net.cpp` (Winsock; `ws2_32` is linked in `src/CMakeLists.txt`), `src/remote/mcp_host.cpp`, `src/frontend/common/rom_fetch.cpp` (`CreateProcessW` to run `curl.exe`), `src/frontend/common/presets.cpp` (`%APPDATA%`), `src/frontend/sdl/gl_api.hpp` (`__stdcall` for the OpenGL function pointers).
- **Third-party code:** Dear ImGui, nlohmann/json, FatFs, littlefs (it has non-GCC fallbacks for its intrinsics), ymfm and floooh's `z80.h` all support MSVC upstream; reSID is plain C++ ❓.
- **Runtime library:** CMake's default for MSVC is the DLL runtime (`/MD`, `/MDd`), so copying `freetvc.exe` to another PC needs the Visual C++ Redistributable there. SDL3 is linked statically (`SDL_STATIC` in `third_party/CMakeLists.txt`). For self-contained `.exe` files configure with `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>` ❓ (not tried).
- **Tests:** the `msvc-debug` and `msvc-release` test presets match the Linux ones (the `slow` label excluded). The slow ZEXDOC/ZEXALL set has only a Linux test preset; on Windows run it directly: `ctest --test-dir build\msvc-release -L slow` (or `build\msys2-release`).

## Route C: WSL 2

Install WSL 2 with an Ubuntu 24.04 distribution (`wsl --install -d Ubuntu-24.04`), clone the repository **inside** the Linux file system (for example `~/src/freeTVC`, not under `/mnt/c`, which is much slower), and follow the [developer guide](developer-guide.md) as written: every command, test and tool there works unchanged.

- The `mingw-release` preset cross-builds the Windows programs inside WSL; the `.exe` files under `build/mingw-release/src/` run directly on Windows (open `\\wsl$\Ubuntu-24.04\home\<user>\src\freeTVC\build\mingw-release\src\` in Explorer, or copy them out together with the `fonts`, `skins`, `keymaps`, `pictures` and `lang` folders next to them).
- The Linux GUI also runs in WSL through WSLg ❓ (not tried); sound goes through WSLg's PulseAudio.
- The real-input GUI checks (`tools/ui/ui_drive.py` on Xvfb) work inside WSL like on any Linux.

## ROMs

The repository holds no ROMs ([developer guide, "ROMs"](developer-guide.md#roms)). On Windows:

- `-DFREETVC_FETCH_ROMS=ON` downloads them at configure time with CMake's own `file(DOWNLOAD)`: no curl needed. They land in `build\<preset>\src\roms\`, next to the programs.
- Into any folder: `cmake -DFREETVC_ROM_OUT=C:/src/freeTVC/testdata/roms -P cmake/FreeTVCRoms.cmake`.
- From the programs: `freetvc-headless.exe --fetch-roms`, or the GUI's download box. These run `curl.exe`, which Windows 10 (since version 1803) and Windows 11 have in `C:\Windows\System32`; freeTVC looks there and on `PATH`.
- The ROM search ([developer guide, "The ROM search"](developer-guide.md#the-rom-search)) uses `%APPDATA%\freeTVC\roms` as the user data folder's `roms\`, and the environment variable `FREETVC_ROM_DIR` (`set FREETVC_ROM_DIR=C:\path\to\roms` in cmd, `$env:FREETVC_ROM_DIR = 'C:\path\to\roms'` in PowerShell).

## Test data

The test data and its sources are in the [developer guide, "Test data"](developer-guide.md#test-data). Its download block is bash: run it in the MSYS2 shell, Git Bash or WSL as it is. In PowerShell (5.1 or 7), from the repository root, the same steps are ❓ (written from the bash block, not run):

```powershell
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'   # Windows PowerShell 5.1 downloads very slowly with the progress bar on

# ROMs
cmake "-DFREETVC_ROM_OUT=$PWD/testdata/roms" -P cmake/FreeTVCRoms.cmake

# SingleStepTests/z80 subset (names may contain spaces: "cb 00")
New-Item -ItemType Directory -Force testdata/sst-z80/v1 | Out-Null
$sst = 'https://raw.githubusercontent.com/SingleStepTests/z80/ebe1875d48f374bcfd4b505d8eb8ee751568b5f7/v1'
Get-Content tests/z80/sst_subset.txt | Where-Object { $_ -and -not $_.StartsWith('#') } | ForEach-Object {
    Invoke-WebRequest -UseBasicParsing -Uri "$sst/$($_.Replace(' ', '%20')).json" -OutFile "testdata/sst-z80/v1/$_.json"
}

# ZEXDOC and ZEXALL
New-Item -ItemType Directory -Force testdata/zex | Out-Null
foreach ($f in 'zexdoc.com', 'zexall.com') {
    Invoke-WebRequest -UseBasicParsing -OutFile "testdata/zex/$f" `
        -Uri "https://raw.githubusercontent.com/floooh/chips-test/3785836e76c43922f78a50e1f8adfed259ab9672/tests/roms/$f"
}
Get-FileHash -Algorithm SHA256 testdata/zex/*.com   # the hashes in the developer guide, in upper case

# MultiCart v2 (the two files the tests read)
$m = 'https://raw.githubusercontent.com/dikdom/TVC-Multicart-v2/631b251d131e6d0782aa7192fd18f08c0134527e'
New-Item -ItemType Directory -Force testdata/TVC-Multicart-v2/software/tvcpla, testdata/TVC-Multicart-v2/software/firmware | Out-Null
Invoke-WebRequest -UseBasicParsing -Uri "$m/software/tvcpla/tvcpla.bin" -OutFile testdata/TVC-Multicart-v2/software/tvcpla/tvcpla.bin
Invoke-WebRequest -UseBasicParsing -Uri "$m/software/firmware/multicart_v2.1.14en.bin" -OutFile testdata/TVC-Multicart-v2/software/firmware/multicart_v2.1.14en.bin

# SD Dual files from the Internet Archive
New-Item -ItemType Directory -Force testdata/downloads | Out-Null
Invoke-WebRequest -UseBasicParsing -OutFile testdata/downloads/sd_dual_2022_11_09_gm.zip `
    -Uri 'https://web.archive.org/web/20250516092433id_/http://users.atw.hu/sdinterface/Firmware/sd_dual_2022_11_09_gm.zip'
Invoke-WebRequest -UseBasicParsing -OutFile testdata/downloads/system_f_v2.zip `
    -Uri 'https://web.archive.org/web/20250516092435id_/http://users.atw.hu/sdinterface/Tools/system_f_v2.zip'
Get-FileHash -Algorithm SHA256 testdata/downloads/*.zip
Expand-Archive testdata/downloads/sd_dual_2022_11_09_gm.zip -DestinationPath testdata/sdinterface/sd_dual_2022_11_09_gm -Force
Expand-Archive testdata/downloads/system_f_v2.zip -DestinationPath testdata/sdinterface/system_f_v2 -Force

# Then configure again, so the configure-time registrations see the data
cmake --preset msvc-debug
```

Test data cache variables are set on the first configure of a build folder; after moving data, delete `build\<preset>\CMakeCache.txt` or set the variables (`-DFREETVC_TEST_DATA=...`).

## Running and debugging

- **`freetvc.exe` is a GUI-subsystem program** (`add_executable(freetvc WIN32 ...)`): started from a console it returns at once and its stderr, where it prints the ROMs found, the renderer and every status-strip notice, is not shown. To keep it, start it from `cmd` with a redirection: `build\msvc-debug\src\freetvc.exe 2> freetvc-stderr.txt` ❓. In PowerShell use `Start-Process -Wait -RedirectStandardError freetvc-stderr.txt build\msvc-debug\src\freetvc.exe`.
- **`freetvc-headless.exe` is a console program**: output, exit codes and `--help` work as on Linux.
- **User data folder:** `%APPDATA%\freeTVC` (`freetvc.ini`, `layout.ini`, `roms\`, the user copies of key layouts and pictures, `SaveState\` with the game saves). A `freetvc.ini` next to `freetvc.exe` makes that copy portable: settings are then read from and written next to the program.
- **Data folders:** the build copies `fonts`, `skins`, `keymaps`, `pictures` and `lang` next to `freetvc.exe`; the GUI needs them there.
- **Renderer:** OpenGL 3.3 core by default; with an old or missing OpenGL driver (some virtual machines, remote desktops) start with `--renderer sdl`, which uses SDL's own renderer (normally Direct3D on Windows).
- **Debugger:** in Visual Studio, pick `freetvc.exe` or `freetvc-headless.exe` as the startup item and add arguments in its launch settings (`launch.vs.json`); with MSYS2 use `gdb` from `mingw-w64-ucrt-x86_64-gdb`.
- **Remote servers** (monitor 6580, DZRP 6582, GDB 6583, MCP 6584) listen on 127.0.0.1 only. A local connection needs no firewall rule ❓ (Windows may still ask once).
- **No icon, version resource or application manifest** is built into the `.exe` files yet; Windows shows a generic icon, and high-DPI behaviour comes from SDL alone ❓.

## Tools on Windows

| Tool | On Windows |
|---|---|
| Python scripts in `tools/` | run with `py -3 -I tools\...` (or `python -I` in MSYS2); the ROM tracing and translation tools are plain Python |
| `tools/lang/po_check.py` | works anywhere Python runs; `msgfmt`/`msgmerge` come from MSYS2's gettext tools package ❓ |
| Shell scripts (`tools/gdb/gdb_check.sh`, `tools/mcp/inspector_check.sh`, `tools/dezog/*.sh`) | bash: run them in MSYS2, Git Bash or WSL |
| `tools/ui/ui_drive.py` (real-input GUI checks) | **X11 only**: run it in WSL 2. On native Windows, check the GUI by hand, or let an agent drive the emulated machine through the MCP server (`screenshot`, `type_text`, `press_keys`, `screen_text`); that sees the TVC's screen, not the GUI's menus and panels |
| DeZog (VS Code) | runs natively on Windows; build it from source with our patch as [tools/dezog/README.md](../tools/dezog/README.md) describes |
| GNU gdb for the Z80 | built on Linux in [tools/gdb/README.md](../tools/gdb/README.md); building it for Windows is not described ❓ |
| sjasmplus | Windows binaries are on its releases page (https://github.com/z00m128/sjasmplus/releases); put it on `PATH` or pass `-DFREETVC_SJASMPLUS=C:/path/to/sjasmplus.exe` |

## Checklist for a first native build

1. Configure, build and test with one route; keep `cmake` and `ctest` output.
2. Start `freetvc.exe`, see the TVC logo, press SPACE, type `PRINT 6*7` and RETURN: `42` must appear. Check sound, full screen (F11), Bench (F12), the settings window, a CAS file loaded by drag and drop, and `--renderer sdl`.
3. Start `freetvc.exe --remote-monitor` and connect with any TCP client to `127.0.0.1:6580` (for example PuTTY in "Raw" mode, or `ncat 127.0.0.1 6580`); `r` prints the registers.
4. Write down what failed or needed changing (compiler messages, missing packages, path problems) and update this page and the tables above.
