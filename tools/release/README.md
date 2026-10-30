# tools/release: release packages

`make_release.sh` builds the downloadable packages of a freeTVC release from the current source tree.

## What it makes

| File | Contents |
|---|---|
| `freeTVC-<version>-linux-x86_64.tar.gz` | `freetvc`, `freetvc-headless` (libstdc++ and libgcc linked in, so they need only glibc: 2.38 or newer when built on Ubuntu 24.04), the GUI's data folders, `roms/`, `docs/`, `README.md`, `LICENSE`, `THIRD_PARTY.md`, `CHANGELOG.md` |
| `freeTVC-<version>-windows-x64.zip` | the same with `freetvc.exe` and `freetvc-headless.exe` from the MinGW-w64 cross build (static, no DLLs) |
| `RELEASE-NOTES.md` | the `## <version>` section of `CHANGELOG.md` |
| `SHA256SUMS` | the checksums of both archives |

`<version>` is the `VERSION` in `CMakeLists.txt`. Each archive holds one folder named like the archive.

`roms/` comes from the ROM autofetch (`data/roms.txt`) minus the files a release may not carry: `multichip.rom` (CC BY-NC-SA 4.0), `sddual.rom` and `sddual-gm.rom` (no licence stated, they contain VT-DOS), `card256-font.bin` (the Commodore character set) and `card256-flash.bin` (other authors' programs). Its `ROMS-README.txt` starts with what the package includes and what is left to the download, followed by the autofetch notice (origin, copyright and licence of every file). The list of excluded files is `not_bundled` in the script.

## Requirements

- Linux x86-64 with everything the developer guide lists for the `gcc-release` and `mingw-release` presets ([developer-guide.md](../../docs/developer-guide.md#prerequisites)), plus `zip`, `strip` and `x86_64-w64-mingw32-strip` (binutils, MinGW-w64).
- Network access for the ROM download.

## Usage

From the project root:

```sh
JOBS=7 tools/release/make_release.sh <out dir>
```

- It builds into `build/release-linux` and `build/release-windows`, so the development builds are left alone.
- It runs the Linux test set (`ctest -LE slow`) on the release build; `SKIP_TESTS=1` skips it. Tests whose data is missing are skipped as usual ([developer-guide.md](../../docs/developer-guide.md#test-data)).
- `JOBS` sets the parallel build and test jobs; without it CMake's default is used.

## Before publishing

1. Set `VERSION` in `CMakeLists.txt` and add its `## <version> — <date>` section to `CHANGELOG.md` (links there must be absolute, because the section becomes the release page's text).
2. Run the script, then check the packages as a downloader would: unpack each into an empty folder and, with no other ROM folder set (`FREETVC_ROM_DIR` unset, a fresh `HOME`), run from inside it

   ```sh
   ./freetvc-headless --preset 64k+-2.2 --frames 700 --key SPACE@330 --type 'PRINT 6*7\n@400' --screenshot shot.ppm
   ```

   `shot.ppm` must show `42` under `PRINT 6*7`. Then start the GUI (its stderr names the `roms` folder next to the program) and type into it. The Windows programs run the same way under Wine on Linux ([windows.md](../../docs/windows.md)).
3. Commit, tag `v<version>`, push the branch and the tag, then create the GitHub release with the two archives and `SHA256SUMS`:

   ```sh
   gh release create v<version> <out dir>/freeTVC-<version>-*.tar.gz <out dir>/freeTVC-<version>-*.zip <out dir>/SHA256SUMS \
       --title "freeTVC <version>" --notes-file <out dir>/RELEASE-NOTES.md
   ```
