# tools/z80eval: Z80 core evaluation harness

The test programs used to evaluate floooh's `z80.h` as freeTVC's CPU core; the results and the decision are in [z80-core.md](../../docs/design/z80-core.md). They run the core on its own, outside freeTVC: against SingleStepTests/z80 bus cycles, ZEXDOC/ZEXALL, and interrupt timing. The core is not copied here; every program includes `z80.h` from a folder given with `-I`. The patched core that freeTVC uses is tested by CTest instead (`z80.sst_subset`, `z80.zexdoc`, `z80.zexall`; see [build.md](../../docs/design/build.md)).

| File | Purpose |
|---|---|
| `zex_run.c` | runs ZEXDOC/ZEXALL on a flat 64 KiB RAM and prints the T-states, instructions and speed. `.com`: the CP/M original, loaded at 0100h, BDOS functions 2 and 9 trapped at the fetch from 0005h. `.cas`: YATE's TVC port, payload at 19EFh, entry 1A03h, `RST 30h` functions 21h (character out) and 04h (video mode) trapped at the fetch from 0030h. Both end at a jump to 0000h. Optional second argument: a T-state limit |
| `sst_conv.py <file.json>` | flattens one SingleStepTests/z80 file into the line format `sst_run` reads (standard library only) |
| `sst_run.c [label]` | per test: the final registers (except `ei`, `p` and `q`, which `z80.h` lacks), RAM, the T-state count, the refresh addresses, and every bus access paired in order with the SST bus state. Each access must sit at a fixed offset per kind: SST marks M1 at T2, MR/MW at T2 and IOR/IOW at TW; `z80.h` signals M1 at T1, MR/MW/IOW at T2 and IOR at TW. Prints one summary line per file, the first three failures, and a histogram of the offsets per access kind |
| `int_test.c` | prints the bus timeline for IM 1 with the EI delay, NMI, and the exit from HALT, to compare with the Zilog figures (IM 1 13 T, NMI 11 T) |
| `sst_subset.txt` | the 93 SingleStepTests files used, one per machine-cycle pattern (names such as `00`, `cb 46`, `ed b0`) |

## Data

- `z80.h`: the upstream original from [github.com/floooh/chips](https://github.com/floooh/chips), `chips/z80.h` as last changed in commit `0949766` (2025-01-02), sha256 `6ca70ffd91b1bdbaf00f7a41e7092b326145fd1c95dcdd7bb01183370de734f3` (zlib licence). The results in z80-core.md are for this file. `third_party/floooh-chips/` holds freeTVC's patched copy (see [THIRD_PARTY.md](../../THIRD_PARTY.md)).
- SingleStepTests: the `v1/` JSON files of [github.com/SingleStepTests/z80](https://github.com/SingleStepTests/z80) at commit `ebe1875d48f3` (MIT), as in `testdata/sst-z80/v1/`.
- ZEXDOC/ZEXALL, CP/M originals: `tests/roms/zexdoc.com` and `zexall.com` of [github.com/floooh/chips-test](https://github.com/floooh/chips-test) at commit `3785836e76c4` (GPL-2.0-or-later), as in `testdata/zex/`.
- ZEXDOC/ZEXALL, TVC port: `zexdoc_tvc.cas` and `zexall_tvc.cas` of YATE ([github.com/hightower70/YATE](https://github.com/hightower70/YATE) at commit `8d9eff4db9b0`, GPL-2.0-or-later code in a GPL-3 repository). Not part of `testdata/`; only needed for the TVC port run.

## Build and run

Build into a scratch folder, outside the source tree; `<z80.h folder>` is the folder that holds the upstream `z80.h`:

```sh
B=<scratch>; Z=<z80.h folder>; V=testdata/sst-z80/v1
gcc -std=c11 -O2 -Wall -Wextra -I$Z -o $B/sst_run tools/z80eval/sst_run.c
gcc -std=c11 -O2 -Wall -Wextra -I$Z -o $B/zex_run tools/z80eval/zex_run.c
gcc -std=c11 -O1 -Wall -Wextra -I$Z -o $B/int_test tools/z80eval/int_test.c
while IFS= read -r n; do python3 -I tools/z80eval/sst_conv.py "$V/$n.json" | $B/sst_run "$n"; done < tools/z80eval/sst_subset.txt
$B/zex_run testdata/zex/zexdoc.com; $B/zex_run testdata/zex/zexall.com
$B/zex_run <YATE checkout>/zexdoc_tvc.cas
$B/int_test
```

## Expected results (upstream `z80.h`)

- SST: a line per file such as `02  tests 1000 fail 0 | len 0 ram 0 bus 0 timing 0 rfsh 0 | F 0 (XY only 0)`. Eight files fail by design of the unpatched core: `37` and `3f` (SCF/CCF, X/Y only), `ed b0`, `ed b1`, `ed b8` (repeat steps, X/Y only), `ed b2`, `ed b3` (repeat steps, X/Y, H, P/V) and `76` (HALT's PC). The counts are in z80-core.md.
- ZEXDOC and ZEXALL (CP/M): every test `OK`, then `Tests complete` and a line with `ticks 46734975919` (the instruction count, CPU time and speed follow). One run takes about 2 to 8 minutes of CPU time, depending on the machine.
- ZEX TVC port: 39 tests OK and 28 ERROR, on any correct core. The port moved the `msbt` test area from 0103h to 1A06h but kept the CP/M expected CRCs, so the 28 tests that put `msbt`-based pointers into registers cannot match. Use the CP/M files to test the CPU.
- `int_test`: IM 1 after EI and a NOP: the INTA cycle with M1+IORQ at t18, the pushes at t24/t27, the fetch from 0038h at t29 (13 T); NMI at t5: the fetch from 0066h after 11 T; HALT: return address HALT+1 after the interrupt.

With the patched `third_party/floooh-chips/z80.h`, `sst_run` still reports X/Y differences for SCF/CCF, because the harness does not load SST's `q` value into the core; the patched core passes the CTest test `z80.sst_subset`, which does.

Checks of the harness itself: the SST run under `-fsanitize=address,undefined` gives the same results; a mutated `z80.h` with the `LD (BC),A` write one tick late fails all 1 000 tests of `02.json` as `timing`.
