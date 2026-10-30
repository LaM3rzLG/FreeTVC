# tools/re: ROM analysis and data helpers

Scripts that analyse the TVC system ROMs and check our ROM documentation against them, check CAS files, locate ROM images inside other files, and generate one data table for the source tree. They need only Python 3's standard library. Run them with `python3 -I` from the project root; pass downloaded files as arguments, never run a script from inside a download folder.

| Script | Purpose |
|---|---|
| `z80trace.py <config.json> <outdir>` | recursive Z80 code trace of the ROM images a config names, following paging, bridges between ROMs, dispatch tables and inline `RST` data. Per image it writes a z80dasm block file (`.blk`), a symbol file (`.sym`) and a report (`.report.txt`), and prints a summary line |
| `tvc_roms.json` | trace config for SYS 2.2 and EXT 2.x |
| `tvc_roms12.json` | trace config for SYS 1.2 and EXT 1.x |
| `check_rom_syms.py <tvc_roms.json> <trace outdir> <symdir>` | checks the SYS 2.2 / EXT 2.x symbol files in `docs/hardware/rom-symbols/` against the trace and the ROMs' own dispatch tables |
| `check_rom_syms12.py <tvc_roms12.json> <trace outdir> <symdir>` | the same for SYS 1.2 / EXT 1.x, plus the EXT 1.x cassette routines' addresses as found in WinTVC 1.4.1's tape traps; a separate script because the 2.2 checker hard-codes its tables |
| `z80trace_audit.py <config.json> <scratch outdir> [image...]` | lists every overlap (a traced instruction that starts inside another) with the instruction that hides it, and every untraced gap of 24 bytes or more; evidence for [rom.md](../../docs/hardware/rom.md) |
| `cas_check.py <file.cas>...` | checks the UPM and program header fields of CAS files against [file-formats.md](../../docs/hardware/file-formats.md), including whether the ROM's `LOAD` would accept the file |
| `find_roms.py <container> <rom>...` | prints the file offsets where each ROM image is embedded in another file, such as an emulator executable; for images over 8 KiB also those of each 8 KiB part (up to 8 matches each) |
| `vsid_tables.py <tvc-sid-internal.h> <out.cpp>` | writes `src/core/cards/vsid_tables.cpp` from the TVC256k++ firmware's SID tables (data only; see below) |

## ROM trace and symbol checks

The configs read the ROM files from `"root": "testdata/roms"`, relative to the working folder, so run from the project root with the ROMs there (the ROM autofetch's output, see [developer-guide.md](../../docs/developer-guide.md)): `sys22.bin`, `ext2x.bin`, `sys12.bin`, `ext1x.bin`. `ext1x.bin` is the 8 KiB socket image, with the 4 KiB EXT 1.x ROM in its upper half; `tvc_roms12.json` reads it from offset 1000h. A config describes each image by file, offset, length and origin, with its entry points, links to other images, bridge addresses, inline-call lengths, extra names and dispatch tables.

```sh
python3 -I tools/re/check_rom_syms.py tools/re/tvc_roms.json <scratch>/trace docs/hardware/rom-symbols
python3 -I tools/re/check_rom_syms12.py tools/re/tvc_roms12.json <scratch>/trace12 docs/hardware/rom-symbols
```

Each checker runs the trace itself (writing its files into the trace folder), then prints a summary line per image, a line per symbol file and a final `table checks: N, problems: 0`. It exits with 1 when any symbol is wrong. The rules: a data label (tables, fonts, copied blocks) must lie outside traced code; every other label must be an instruction start; table entries must match the ROMs' own dispatch tables. Current counts: 2.2: 169 + 12 + 65 symbols and 144 table checks; 1.2: 87 + 5 + 38 symbols and 69 table checks.

The trace on its own, and a disassembly with z80dasm 1.1.6 from its output:

```sh
python3 -I tools/re/z80trace.py tools/re/tvc_roms.json <scratch>/trace
z80dasm -a -l -t -g 0xc000 -b <scratch>/trace/sys22.blk -S <scratch>/trace/sys22.sym -o <scratch>/sys22.asm testdata/roms/sys22.bin
python3 -I tools/re/z80trace_audit.py tools/re/tvc_roms.json <scratch>/audit sys22
```

`z80trace_audit.py` writes no trace files; its output folder is only needed by the trace run inside it.

## vsid_tables.py

Generates the three tables of the TVC256k++ card's vSID engine from the firmware's `sources/firmware/TVC-IO/tvc-sid-internal.h` in [github.com/dikdom/TVC256kplusplus](https://github.com/dikdom/TVC256kplusplus) at commit `4580b144a9b99388a306b871b4f48f4d4b8f3905` (firmware 0.24):
- `ADSR_LFSR15` and `ADSR_Volume2LFSR5` from Bakisha's STM32-SID-Player (GPL-3.0);
- `AND_mask`, reSID's combined-waveform table by Dag Lem (GPL-2.0-or-later).

Only the table data is copied; the licences are recorded in [THIRD_PARTY.md](../../THIRD_PARTY.md). Rerunning it on that header reproduces the committed file byte for byte:

```sh
python3 -I tools/re/vsid_tables.py <TVC256kplusplus checkout>/sources/firmware/TVC-IO/tvc-sid-internal.h src/core/cards/vsid_tables.cpp
```

## Other tools used for ROM work

z80dasm 1.1.6 (Debian/Ubuntu package `z80dasm`) for disassembly listings from the trace output.
