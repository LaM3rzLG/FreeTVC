# freeTVC

An emulator of the Videoton TV Computer (TVC), the Hungarian Z80 home computer of the 1980s, for Linux and Windows. It emulates the 32k, 64k and 64k+ machines with BASIC 1.2, 2.1 and 2.2, the tape, cartridges, the disk card and a range of expansion cards, and it is built for people who debug and write TVC software:

- **Bench**, a debugger in the GUI: checkpoints, stepping, memory, video, I/O log, history, profiler, source-level debugging;
- a **VICE-style text monitor** over TCP;
- an **MCP server**, so AI agents can run, inspect and drive the machine;
- a **DZRP server** for DeZog and a **GDB stub**;
- **`freetvc-headless`** for scripts and tests: typing, screenshots, WAV output, snapshots.

What is emulated and how its accuracy was checked: [docs/status.md](docs/status.md). Ready-to-run packages for Windows and Linux are on the [releases page](https://github.com/LaM3rzLG/FreeTVC/releases); [CHANGELOG.md](CHANGELOG.md) lists what each release contains.

## Running on Linux

Use **Ubuntu 24.04 LTS** or newer (Debian 13, Fedora 40 or newer also work; Ubuntu 22.04 is too old). Download `freeTVC-<version>-linux-x86_64.tar.gz` from the [releases page](https://github.com/LaM3rzLG/FreeTVC/releases), then in a terminal:

```sh
cd ~/Downloads
tar xzf freeTVC-*-linux-x86_64.tar.gz
cd freeTVC-*-linux-x86_64
./freetvc
```

The program is the file `freetvc` in that folder; the ROMs, skins and the rest are beside it, so move the whole folder, not the file alone. A double click on `freetvc` in the Files app does not start it: run it from a terminal as above, or right-click it and choose *Run as a Program* where the file manager offers that.

## Build

On Ubuntu 24.04 or a similar Linux (the full package list is in the [developer guide](docs/developer-guide.md#prerequisites)):

```sh
git clone https://github.com/LaM3rzLG/FreeTVC.git freeTVC
cd freeTVC
cmake --preset gcc-release -DFREETVC_FETCH_ROMS=ON
cmake --build --preset gcc-release
ctest --preset gcc-release
build/gcc-release/src/freetvc
```

The tested Windows build is a MinGW-w64 cross build from Linux (preset `mingw-release`); building on Windows itself (MSYS2, Visual Studio or WSL 2) is described in [docs/windows.md](docs/windows.md).

## ROMs

The repository contains no ROMs. They are freely downloadable from [tvc.homeserver.hu](https://tvc.homeserver.hu): `-DFREETVC_FETCH_ROMS=ON` fetches them at build time, `freetvc-headless --fetch-roms` or the GUI's "Download ROMs" item fetches them later, and the checksums of every file are pinned in `data/roms.txt`. Release packages will include the system ROMs in a `roms/` folder with a notice.

## Documentation

- [User guide](docs/user-guide.md): running freeTVC, loading software, cards, settings, Bench, the remote interfaces, the headless runner.
- [Developer guide](docs/developer-guide.md): from a fresh clone to a build, tests and a first change; extending the emulator.
- [Documentation index](docs/README.md): design docs and the TVC hardware reference.
- [AGENTS.md](AGENTS.md): rules for AI coding agents.

## Licence

GPL-3.0-or-later ([LICENSE](LICENSE)). Third-party code, its licences and provenance: [THIRD_PARTY.md](THIRD_PARTY.md). The TVC ROMs and the cards' firmware are not part of freeTVC and not covered by its licence.
