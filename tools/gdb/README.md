# tools/gdb: GDB stub check with GNU gdb

`gdb_check.sh` checks freeTVC's GDB stub ([debug-and-remote.md](../../docs/design/debug-and-remote.md), section "GDB stub") with a real GNU gdb built for the Z80. It starts `freetvc-headless --gdb` on the 64k+ preset with SYS 2.2, types SPACE at frame 330 to leave the start-up logo, and runs a gdb batch session against it:

- connect (`set architecture z80`, `target remote`) and `info registers`;
- load `tools/dezog/project/hello.asm`, assembled with sjasmplus, with `restore … binary 0x6000`, and read it back with `x/6xb 0x6000`;
- a breakpoint on its `RST 30h` at 6015h, `continue`, `stepi` (into the OS entry at 0030h);
- a write watchpoint on the counter byte at 602Ah, `continue`;
- a read of the EXT ROM with `monitor m EXT:0000 0003`, `monitor bank`, `detach`.

GNU gdb cuts Z80 addresses to 16 bits (`x/4xb 0x30000` reads 0000h), so the stub's bank addresses from 10000h up are out of its reach. Use `monitor` with the text monitor's `BANK:offset` syntax instead.

## Prerequisites

- A freeTVC build of the `gcc-release` preset (`build/gcc-release/src/freetvc-headless`).
- The system ROMs: set `FREETVC_ROM_DIR` to their folder (for example `$PWD/testdata/roms`). Without it the script uses `testdata/roms`.
- sjasmplus 1.24.0 on `PATH`, or in `SJASMPLUS`, or at `.tools/sjasmplus-1.24.0/bin/sjasmplus` under the project root (see [tools/dezog/README.md](../dezog/README.md#prerequisites)).
- GNU gdb with the Z80 target, see below. The script uses `.tools/gdb-18.1-z80/bin/z80-unknown-elf-gdb` unless `GDB` names another one.

## Build a gdb with the Z80 target

Distribution packages such as Ubuntu 24.04's `gdb-multiarch` 15.1 have no Z80 target (`set architecture z80` fails with "Undefined item: z80"), so gdb is built from GNU's source for `z80-unknown-elf`. GDB 18.1 was used:

1. Download `https://ftp.gnu.org/gnu/gdb/gdb-18.1.tar.xz`, its `.sig`, and `https://ftp.gnu.org/gnu/gnu-keyring.gpg` into a folder of your choice. Check the signature with `gpgv --keyring ./gnu-keyring.gpg gdb-18.1.tar.xz.sig gdb-18.1.tar.xz`; it reports "Good signature from Joel Brobecker", key `F40ADB902B24264AA42E50BF92EDB04BFF325CF3`. Unpack the tarball.
2. Install the build dependencies (Debian/Ubuntu: `sudo apt install libgmp-dev libmpfr-dev`), then configure and build in a separate folder:

```sh
mkdir -p <build folder> && cd <build folder>
<gdb-18.1 source>/configure --target=z80-unknown-elf --prefix=<install folder> --disable-nls --disable-werror \
    --disable-sim --disable-gdbserver --with-python=no --without-guile --without-debuginfod
make -j8 all-gdb && make install-gdb
```

The result is `<install folder>/bin/z80-unknown-elf-gdb`. With `--prefix=<project root>/.tools/gdb-18.1-z80` the script finds it without `GDB`.

## Run

From the project root:

```sh
FREETVC_ROM_DIR=$PWD/testdata/roms tools/gdb/gdb_check.sh <work dir>
GDB=<path to z80-unknown-elf-gdb> FREETVC_ROM_DIR=$PWD/testdata/roms tools/gdb/gdb_check.sh <work dir>
```

The stub listens on port 6593 for this check. The gdb output goes to the terminal and to `<work dir>/gdb.log`, freeTVC's stderr to `<work dir>/headless.err`; the gdb command file is `<work dir>/check.gdb`.

## Expected output

gdb warns once that it cannot find `_break_handler` and uses `RST 0x08` as the software breakpoint instruction; that is harmless, since the stub handles breakpoints itself. The monitor's stop messages are passed through as gdb console output. The end of a good run:

```text
Breakpoint 1, 0x00006015 in ?? ()
pc             0x6015              0x6015
sp             0x1668              0x1668
#2 (Stop on exec 0030)  U0:0030
OS_ENTRY:
.C:0030  C3 23 0B     JP INFUNC
(C:$0030) 0x00000030 in ?? ()
pc             0x30                0x30
Hardware watchpoint 2: *(unsigned char *) 0x602a
...
Old value = 0 '\000'
New value = 1 '\001'
0x00006012 in ?? ()
pc             0x6012              0x6012
>EXT:0000  3E D0 D3 02                                       >...
page 0  0000-3FFF  U0
page 1  4000-7FFF  U1
page 2  8000-BFFF  U2
page 3  C000-FFFF  SYS
port 02h = 70  IOMEM slot 0  video paging = 00  displayed VID0
[Inferior 1 (Remote target) detached]
```
