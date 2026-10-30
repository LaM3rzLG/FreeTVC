# Z80 core

Why freeTVC uses floooh's `z80.h` as its Z80 core, the evidence behind that choice, how the TVC's wait states are mapped onto it, and the three patches freeTVC applies to it. The core must expose per-T-state bus cycles for the TVC's VRAM clock stretch ([tvc-hardware.md](../hardware/tvc-hardware.md) §2).

Evidence tags: **T** = a dynamic test run (the harness in `tools/z80eval/`, or the CTest programs in `tests/z80/`), **S** = source reading with file:line, **U** = upstream metadata (GitHub API, CI configuration), not run by us. Line numbers marked "upstream" refer to the unpatched file at commit `0949766`.

## Choice

freeTVC uses **floooh/chips `z80.h`** (zlib; https://github.com/floooh/chips), vendored in `third_party/floooh-chips/z80.h` from upstream commit `0949766` (2025-01-02), under these conditions:

1. It is compiled as **one C11 translation unit** (`src/core/z80/z80_chips.c`) behind freeTVC's own C++ adapter (`src/core/z80/z80.hpp`, class `Z80`), so the rest of the code does not depend on its struct layout and a later swap stays possible.
2. **Every TVC wait state is machine-side time** in half-T ticks: the VRAM stretch, and (not modelled) the 32k/64k slow-device M1 wait and card `/WAIT`. The core's WAIT pin is not used (see "Why not the WAIT pin").
3. The three known deviations (Q, block-repeat flags, HALT) are patched in the vendored copy, which is marked as altered (zlib clause 2). The patches are gated by SingleStepTests.

Why not an own core: no requirement was found that `z80.h` cannot meet with small patches. Its timing matched every one of the 286 032 bus accesses checked, and it passes the canonical ZEXDOC and ZEXALL. Its decoder alone has 1 721 `case` steps in `z80_tick()` (upstream `z80.h`:1068-2811, mostly generated), and an own core would have to be written and validated to the same level first. The adapter keeps that option open.

## Requirements

| ID | Requirement | Source |
|---|---|---|
| R1 | Every bus cycle (M1, MR, MW, IOR, IOW, INTA, refresh) with its T-state position | [tvc-hardware.md](../hardware/tvc-hardware.md) §2; [architecture.md](architecture.md) "How a frame runs" |
| R2 | Waits of 1–4 half-T (160 ns) on VID accesses, from the DEB phase at T1; +1 T on M1 for slow devices (32k/64k); card `/WAIT` on memory and I/O | [tvc-hardware.md](../hardware/tvc-hardware.md) §2 (table, WAIT circuit) |
| R3 | Documented and undocumented behaviour: flags incl. X/Y, MEMPTR (WZ), R, instruction lengths | cycle-exact emulation goal |
| R4 | Interrupts: IM 1 (TVC), IM 0/2 (software, cards), NMI (expansion bus), EI delay, HALT; INTA reads FFh | [tvc-hardware.md](../hardware/tvc-hardware.md) §2 "Interrupts" |
| R5 | Debug: instruction boundaries, full state read/write, set PC, step, access-level watchpoints, byte-exact deterministic state | [debug-and-remote.md](debug-and-remote.md) |
| R6 | Licence compatible with GPL-3.0-or-later | the project licence |
| R7 | Builds with MSVC and GCC/Clang through CMake, no GCC-only extensions | portability goal (Windows and Linux) |
| R8 | Real time with margin on modest PCs; warp speed for headless and agent runs | [debug-and-remote.md](debug-and-remote.md) |
| R9 | Effort to adopt and maintain | — |

## Candidates

| | Core | Licence | Granularity | Examined |
|---|---|---|---|---|
| A | floooh/chips `z80.h` | zlib | one call per T-state, 64-bit pin word | **tested** (below) |
| B | own T-state core, written from documentation | GPL-3.0-or-later | per T-state, by design | effort estimate only |
| C | instruction core + per-instruction bus-cycle table (rtvc's approach, https://github.com/teki/rtvc; the core could be superzazu/z80, redcode/Z80 or an own one) | per core | instruction; access positions from a table | rtvc's technical notes |
| D | ep128emu Z80 (K. Thacker / I. Varga) | GPL-2.0-or-later | instruction, accesses through virtual callbacks between `updateCycle()` calls | source reading |
| E | redcode/Z80 (https://github.com/redcode/Z80) | LGPL-3.0 | instruction + callbacks | not examined in detail |

## Evidence for A (`z80.h`)

The copy examined (2 838 lines) has git blob `055d581d…`, which was upstream HEAD when it was vendored. The last commit touching the file is `0949766` (2025-01-02, "Z80 code generated decoder structure rework"). U: GitHub API. Hashes of the original and the patched file are in [THIRD_PARTY.md](../../THIRD_PARTY.md).

### Bus cycles against SingleStepTests (T)

Data:
- SingleStepTests/z80 v1 at commit `ebe1875` (https://github.com/SingleStepTests/z80, MIT). The tests come from a translation of Ares' Z80 core plus fixes, not from silicon (its README).
- 93 of the 1 604 files, one per machine-cycle pattern: plain, CB, DD/FD, DD CB, ED incl. all block instructions, I/O, HALT, EI/DI, RETN/RETI, the SCF/CCF/DAA flag cases. The list is `tools/z80eval/sst_subset.txt`.
- 1 000 tests per file, 93 000 tests in all.

Method (`tools/z80eval/`):
- `sst_run.c` executes each test from its initial state.
- It compares the final registers and RAM, the instruction length and the refresh addresses.
- It also pairs every bus access with the SST bus state in order and requires a fixed T-state offset per access kind.

Results:
- **0 failures** in instruction length, RAM, access order/address/write data, access timing and refresh address.
- **0 failures** in all registers except F, and PC on HALT. WZ, R, IFF1/2 and IM all match.
- 286 032 accesses were checked: M1 143 000, MR 93 030, MW 40 002, IOR 5 000, IOW 5 000.
- 5 500 failures, all in 8 files and all in flags or the HALT PC:

| File | Instruction | Failures | What differs | Cause in upstream `z80.h` (S) |
|---|---|---|---|---|
| `37`, `3f` | SCF, CCF | 229, 219 | X/Y only | no Q register; X/Y always from A (upstream `z80.h`:681-687) |
| `ed b0`, `ed b1`, `ed b8` | LDIR, CPIR, LDDR | 749, 727, 721 | X/Y only, repeat case | repeat step only rewinds PC and sets WZ (upstream `z80.h`:2587); flags stay as in LDI/CPI (upstream `z80.h`:726-749) |
| `ed b2`, `ed b3` | INIR, OTIR | 930, 925 | X/Y, H, P/V, repeat case | same, `_z80_ini_ind` / `_z80_outi_outd` (upstream `z80.h`:751-771) |
| `76` | HALT | 1 000 | PC: `z80.h` HALT address, SST HALT+1 | `_z80_halt` does `pc--` and re-executes HALT (upstream `z80.h`:459-462) |

All eight are fixed by the patches ("Deviations and patches" below).

Access position: the tick in which `z80.h` returns the pins, and the T1 it implies. SST marks reads and writes one T-state later, except IOR, which both mark at TW.

| Access | `z80.h` pins appear in | T1 of the machine cycle | SST marks it in |
|---|---|---|---|
| M1 opcode fetch | T1 | this tick | T2 |
| memory read / write | T2 | tick − 1 | T2 |
| I/O read | TW (T1+2) | tick − 2 | TW |
| I/O write | T2 | tick − 1 | TW |
| INTA (M1 + IORQ) | T1+2 | tick − 2 | (not in SST) |
| refresh (MREQ + RFSH, address I:R) | T3 of M1 | — | T3 (address matches) |

Checks on the harness:
- The same run under ASan/UBSan gives the same results.
- A mutated `z80.h` copy, with the `LD (BC),A` write one tick late, fails all 1 000 tests of `02.json` as `timing`.

The CTest program `z80.sst_subset` (`tests/z80/sst_test.cpp`) runs the same comparison through freeTVC's adapter on 97 files (`tests/z80/sst_subset.txt`: the 93 above plus `f1` POP AF and the block repeats `ed b9`, `ed ba`, `ed bb`), with Q loaded and checked in every test. It passes with an empty expected-fail table. Data: `testdata/sst-z80/v1/` (cache variable `FREETVC_SST_DIR`); skipped without it.

### ZEXDOC / ZEXALL (T)

`zex_run.c` on a flat 64 KiB RAM, with the output calls trapped:

| Program | Result | T-states, CPU time |
|---|---|---|
| ZEXDOC, CP/M original (BDOS trapped) | **67/67 OK** | 46 734 975 919, 470 s |
| ZEXALL, CP/M original | **67/67 OK** | 46 734 975 919, 465 s |
| ZEXDOC, YATE's TVC port (`RST 30h` trapped) | 39 OK, 28 ERROR | 46 735 332 879, 470 s |
| ZEXALL, YATE's TVC port | 39 OK, the same 28 ERROR | 46 735 332 851, 469 s |

- The CP/M binaries are the same 8 585 program bytes in two independent repositories: floooh/chips-test (https://github.com/floooh/chips-test, `tests/roms/`) and anotherlin/z80emu (https://github.com/anotherlin/z80emu, `testfiles/`); GPL-2.0-or-later, used as test data only.
- YATE's TVC port (https://github.com/hightower70/YATE) keeps the CP/M expected CRCs, but its `msbt` area moved from 0103h to 1A06h. The 28 failing tests are exactly those with `msbt` pointers in registers, so the failures come from the port's CRC table, not from the core. Its CRCs would have to be regenerated for `msbt` = 1A06h before it can test anything inside an emulated TVC.
- The CTest programs `z80.zexdoc` and `z80.zexall` (`tests/z80/zex_test.cpp`) run the CP/M originals through freeTVC's adapter (BDOS functions 2 and 9 trapped at 0005h) and pass when all 67 groups print OK. Data: `testdata/zex/zexdoc.com`, `zexall.com` (cache variable `FREETVC_ZEX_DIR`); label `slow`.

### Interrupts (T)

`int_test.c` prints the bus timeline (tick = T-state):

| Case | Observed | Expected (Zilog UM0080 figures) |
|---|---|---|
| `IM 1`, `EI`, `NOP` with INT high from t0 | not taken after EI; taken after the NOP: INTA cycle from t16 (M1+IORQ at t18), pushes 00h/04h at t24/t27, fetch at 0038h at **t29 = 13 T** | EI delays one instruction; IM 1 takes 13 T; return address 0004h |
| NOPs, NMI rising at t5 | taken after the current NOP: dummy M1 at 0002h at t8, pushes at t14/t17, fetch at 0066h at **t19 = 11 T** | NMI takes 11 T; return address 0002h |
| `HALT` at 0003h, INT high from t30 | halted M1s at 0003h every 4 T with the HALT pin; INT seen in the last T of the t28 M1; INTA from t32; pushes **0004h**; fetch 0038h at t45 = 13 T | return address HALT+1 |

These are the unpatched results; the HALT row changes with patch 3 (below). IM 0/IM 2 were not tested by us; upstream has `tests/z80-int.c` (U).

### Portability and speed (T, U)

- `gcc -std=c11`, `clang -std=c11` and `x86_64-w64-mingw32-gcc -std=c11`, all with `-Wall -Wextra -pedantic`: **no warnings**.
- With `-std=c99` the only warnings are about anonymous structs/unions (a C11 feature).
- Compiled as C++20, g++ and clang++ warn only about anonymous structs in `z80_t` (upstream `z80.h`:348 says so itself). MinGW g++ without `-pedantic` is clean. Hence condition 1: compile it as C.
- MSVC: `z80.h` has an `_MSC_VER` branch (`__assume(0)`, upstream `z80.h`:391-397), and floooh's chips-test CI builds `win-msvc-debug` and `win-msvc-release` on `windows-latest` (its `.github/workflows/build.yml`:6-18; U, not run by us).
- Speed: **99.4 M T-states/s = 31.8× a TVC** for the bare core on a low-end 2-core PC (gcc 13 `-O2`, flat RAM), measured on the full ZEXDOC TVC-port run: 46 735 332 879 T-states in 470 s CPU. The machine around it costs a share of that.

### API and state (S)

- **Instruction boundary**: `z80_opdone()` (upstream `z80.h`:453-457) is true right after the T1 tick of the next opcode fetch, outside prefixes.
  - At that point the previous instruction is complete, and the address bus holds the next PC.
  - `cpu->pc` is already PC+1 (the fetch incremented it), so freeTVC reports the bus address of that fetch as the instruction's PC (`Machine::pc()`).
- **Set PC**: `z80_prefetch()` (upstream `z80.h`:1033-1038) restarts with a fetch at the new PC (`Machine::jump`, `Machine::return_to`).
- **State**: `z80_t` (upstream `z80.h`:337-366) is plain data. It includes the decoder step, latches and the last pin word, so a snapshot can be taken at any tick. The machine's own pin word is stored too (TVCS chunk `CPU `, [media.md](media.md#snapshots)).
  - Upstream has no `q`, `p` or "halted" fields; HALT is a pin. Patch 1 adds `q` and `qw`.
- **Interrupt sampling**: after every tick, `int_bits` takes the INT level and latches a rising NMI edge (upstream `z80.h`:2803-2810). The next fetch checks it (upstream `z80.h`:946-991), so INT is sampled in the last T of an instruction (confirmed by the HALT case above).

### Why not the WAIT pin (S)

- `_wait()` (upstream `z80.h`:1058) tests WAIT at the start of the step that puts the access on the bus. Example: `case 512: _wait();_mread(cpu->pc++)` (upstream `z80.h`:1584).
- So the machine must raise WAIT *before* it sees the address of that cycle. That suits a fixed clock-phase scheme (floooh's CPC), not the TVC, where the stretch depends on whether the address is in VID.
- A wait state changes nothing inside the Z80 except time. The TVC has no DMA or second bus master, so adding the stretch to the machine clock is equivalent.

## Mapping the TVC's timing onto `z80.h`

As built in `Machine::run()` and `Machine::stretch()` ([machine.md](machine.md#timing)):

- The machine clock counts half-T ticks (6.25 MHz). Each `z80_tick()` adds 2.
- When the returned pins show MREQ + RD or WR (not RFSH) to a page that maps VID:
  1. Take T1 of that machine cycle: this tick for M1, the previous tick for MR/MW.
  2. Find its DEB phase (tick mod 4).
  3. Add the extra half-ticks from the [tvc-hardware.md](../hardware/tvc-hardware.md) §2 table: R/W +2/+1/+4/+3, M1 +3/+2/+1/+4.
  4. Devices catch up to the access time first, so a VRAM write lands in the right CRTC slot.
- I/O is never stretched. Port accesses happen at the tick `z80.h` presents them (T2 for writes, TW for reads), after the devices have caught up, which matters for mid-line palette and border writes.
- INTA: the machine puts FFh on the bus; IM 1 ignores it.
- Refresh: the RFSH pins appear at T3, so if refresh into VID turns out to be stretched ([tvc-hardware.md](../hardware/tvc-hardware.md) §2, ❓), the same path can model it.
- Not modelled, but the same path would carry them: the slow-device M1 wait (32k/64k with `/SLOWEXP`, `/FAST` or the ROM jumpers: +2 half-ticks on that M1) and card `/WAIT` (a card would return extra ticks).

## Deviations and patches

| # | Behaviour | Effect on TVC software | Status |
|---|---|---|---|
| 1 | **Q** (SCF/CCF X/Y): real NMOS gives X/Y = ((Q xor F) or A) and 28h, where Q = F after an instruction that set flags, else 0 | none known; test suites see it (SST, Patrik Rak's `z80ccf`) | patched |
| 2 | **Block-repeat flags** (LDIR/LDDR/CPIR/CPDR X/Y from PC; INIR/INDR/OTIR/OTDR also H, P/V) | only if code inspects these flags after an interrupt in the middle of a repeat | patched |
| 3 | **HALT**: upstream `z80.h` keeps PC at the HALT byte and re-fetches it | the INT return address was already right (HALT+1). The PC a debugger shows and the M1 address while halted differed; the latter matters for the stretch if HALT runs from VID | patched |
| 4 | Not modelled: `p` (the NMOS LD A,I/R P/V behaviour when an INT is accepted right after), the CMOS variant | negligible | not patched |

### What the patches do

The vendored file is marked as altered at the top (zlib clause 2). Every change carries a `freeTVC` tag on or above it, and the full list is in [THIRD_PARTY.md](../../THIRD_PARTY.md) (+101/−20 lines against the original).

1. **Q.**
   - New state: `q` (the Q register) and `qw` ("the current instruction has written F").
   - `qw` is set at every F write: the ALU, rotate/shift, BIT, DAA/CPL/SCF/CCF, 16-bit arithmetic, block and I/O helpers, and LD A,I/R.
   - `_z80_fetch()` runs at every instruction boundary, including EI/RETN/RETI, which call it directly. It sets `q = qw ? F : 0` and clears `qw`.
   - SCF/CCF take X/Y from `((q ^ F) | A)`.
   - `z80_prefetch()` sets `qw = (q != 0)`, so its restart fetch keeps Q. That works because Q is always 0 or F.
   - The adapter exposes `q` in `Z80Registers`, and the SST test loads and checks it in every test.
   - **POP AF and EX AF,AF' do not set Q**: SST `f1` and `08` give final q = 0 (T). SST is generated from Ares' core, not from silicon, so this is Ares' model. ❓ No silicon reference is known to us.
2. **Block-repeat flags.** These are applied in the repeat step, after the PC rewind, so PC is the instruction's address.
   - LDIR/LDDR/CPIR/CPDR: X = PC bit 11, Y = PC bit 13.
   - INIR/INDR/OTIR/OTDR: the same X/Y. If C is set, H and the parity input use B−1 (data bit 7 = 1) or B+1 (bit 7 = 0); H = ((B & 0Fh) == 00h) or ((B & 0Fh) == 0Fh) respectively. P/V is inverted when the 3-bit parity input has odd parity.
   - All 8 block-repeat files pass, 1 000 tests each (T).
3. **HALT.**
   - HALT no longer does `pc--`, so PC = HALT+1 afterwards.
   - While the HALT pin is up, `_z80_fetch()` puts PC on the bus without incrementing it.
   - `Z80_M1_T2` forces opcode 00h (NOP) instead of the bus byte, so each halted cycle is a 4 T M1 with refresh.
   - The INT/NMI exits drop their `pc++`.
   - Evidence for "halted M1 reads HALT+1" (web sources):
     - the redcode/Z80 wiki page "Z80 Special Reset" (https://github.com/redcode/Z80/wiki/Z80-Special-Reset) states that while /HALT is low the PC has already moved past the HALT and the same following address is fetched over and over; it reports tests on real chips by Dave Stevenson (MTX Plus+ board, mainly genuine Zilog parts), and notes that "The Undocumented Z80 Documented" is wrong on this point;
     - Patrik Rak on spectrumcomputing.co.uk (topic "Contention pattern for HALT", 2024) says the same: the halted CPU keeps reading from PC+1.
   - Timeline (T, `tools/z80eval/int_test.c` before and after):
     - the halted M1s move from 0003h to **0004h** at the same ticks (t16/20/24/28);
     - INTA at t34, pushes 00h/04h and the fetch at 0038h at t45 are unchanged.
   - Regression checks in `tests/core/core_test.cpp` (`z80_halt_int_exit`, `z80_halt_nmi_exit`; CTest `core`), with INC A placed after HALT:
     - every halted M1 at HALT+1;
     - INC A not executed;
     - R = 8 at INTA;
     - return address 0004h; INTA t34, vector t45.

Validation (T): the SST subset, ZEXDOC/ZEXALL and the HALT regression checks pass with the patched file. Each check was also run against a deliberately broken copy of `z80.h` and failed as it should: halted fetch incrementing PC, no forced NOP, POP AF setting Q, inverted I/O-repeat parity, no repeat X/Y.

Open: Q after an interrupt response. `_z80_fetch()` at the end of the INT/NMI sequence sets Q = 0, since the response writes no flags. No test covers it (❓).

## Other options, briefly

- **B, own core**: meets R1/R2/R5 by design, but R3/R4 must be built and proven from zero with the same tests. It is the costliest option (R9) and showed no gain over A + patches.
- **C, instruction core + table**:
  - Works: rtvc pairs each access with FUSE bus-cycle data and fails a test when the table drifts (its technical notes).
  - It keeps two sources of truth that must agree, and mid-instruction events (I/O timing, device catch-up) need the table too.
  - A gives the same positions directly.
- **D, ep128emu** (https://github.com/libretro/ep128emu-core):
  - Proven on a TVC with clock stretching; its half-cycle formula is the reference in [tvc-hardware.md](../hardware/tvc-hardware.md) §2 (`tvc64vm.cpp`).
  - But the core is tied to ep128emu: the `Ep128::Z80` base class, `ep128emu.hpp` → `fileio.hpp`, and `Ep128Emu::File` state code (`z80/z80.hpp`).
  - Access timing is implicit in the order of callbacks and `updateCycle()` calls, so it needs the same SST validation after extraction.
- **E, redcode/Z80**:
  - Reportedly accurate on flags, but instruction-granular, so it has C's table problem.
  - LGPL-3.0 is compatible with GPL-3.0-or-later: all of freeTVC's source is published under GPL-3, so relinking is not an obstacle.

## Open items

- The full SingleStepTests v1 set (1 604 files, 1 374.6 MB at `ebe1875`) has not been run; only the 97-file subset.
- ZEXDOC/ZEXALL inside the emulated TVC, through the real ROM's `RST 30h`: needs YATE's port with its 28 CRCs regenerated for `msbt` = 1A06h, using the core that passes the CP/M originals.
- The MSVC build has not been run on Windows.
- Optional: Patrik Rak's z80test 1.0 (`z80full`, `z80ccf`, `z80memptr`; bundled in floooh's chips-test).
- The clock-stretch model has no measurement on real hardware.
