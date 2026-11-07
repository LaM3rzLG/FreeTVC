# Debug core, remote monitor and MCP — design

This document describes freeTVC's debugging stack: the debugger core (`src/debug/`), expressions, symbols, the disassembler, source-level debugging and the profiler, and the remote front ends in `src/remote/`: the text monitor, the MCP server, the DZRP server for DeZog and the GDB stub. Notes on the external protocols themselves are in [protocols-reference.md](protocols-reference.md); the GUI's debugging mode, Bench, is in [ui.md](ui.md#bench).

## Principles

1. **One core debug API, many front ends.** Bench, the text monitor, MCP, DZRP and the GDB stub are thin adapters over the same `Debugger` and the text monitor's `Monitor`; none of them touches emulator internals directly. They share one hold state: a stop made by one is seen by all.
2. **One thread owns the machine**: the GUI or headless loop. Network threads only accept, parse and queue commands; the queue is drained between run slices, or at once while the machine is held. Nothing blocks the emulation loop waiting for a client. There is no separate emulator thread.
3. **Everything bounded and deterministic.** A run always has a budget (T-states, frames, instructions or wall time) and returns why it stopped. The core never reads the wall clock; scripted input (`--key`, `--type`, MCP `type_text`) is applied by frame number, so a scripted run repeats exactly.
4. **Headless first.** Every capability works without a window; scripts, CI and AI agents are first-class users.
5. **Secure by default.** Every listener binds 127.0.0.1 only and is off unless asked for. MCP over HTTP checks `Origin` and requires a bearer token. The raw monitor passthrough (`monitor_command`) is opt-in. Other MCP tools and monitor commands read and write host files named by the client (`load_binary`, `bload`/`bsave`, snapshot and media files), so a connected client has the rights of the user running freeTVC.

## Layering

```
                 ┌─ Bench (Dear ImGui): Listing, Memory, CPU/Machine cards, Screen as timeline, Console, Agent lane, ...
                 ├─ Text monitor: VICE-style commands — TCP, stdin console, Bench Console, --moncommands
 Debugger +      ├─ MCP server: stdio + Streamable HTTP, 2026-07-28 and legacy (2025-11-25 ...) clients
 text monitor ───┼─ DZRP 2.2 server — DeZog "dzrp" remote — TCP
 (hold state,    └─ GDB RSP stub (target.xml, qRcmd → text monitor) — TCP
  command queue)
```

## Ports and numbers

| Port (127.0.0.1) | Server | Options |
|---|---|---|
| 6580 | text monitor | `--remote-monitor`, `--monitor-port N` |
| 6582 | DZRP (DeZog) | `--dzrp`, `--dzrp-port N`, `--dzrp-banks` |
| 6583 | GDB stub | `--gdb`, `--gdb-port N` |
| 6584 | MCP over HTTP (`/mcp`) | `--mcp-http`, `--mcp-port N`, `--mcp-token T`, `--mcp-allow-monitor` |

- All are off by default. In the GUI they can also be started from `freetvc.ini` (`monitor`, `mcp`, `dzrp`, `gdb`: `off`, `on` or a port) and switched or moved at run time in the settings window ([ui.md](ui.md#settings-window)). Port 0 (headless) means any free port, printed on stderr.
- The defaults avoid VICE's 6502/6510 and MAME's 12000. 6581 is unused. 6585 is the serial card's default TCP cable ([cards.md](cards.md), section "Serial card"), not a debug port.
- **Monitor numbers**: hex by default; `$`, `0x` and an `h` suffix are accepted; `#` marks decimal, `%` binary. MCP takes JSON numbers (decimal) or monitor expression strings.
- **Networking**: our own thin socket layer (`net.{hpp,cpp}`, POSIX and Winsock) and a minimal HTTP/1.1 server for `POST /mcp` that always answers with plain JSON (no SSE). JSON is the vendored nlohmann/json 3.12.0 (MIT; `THIRD_PARTY.md`).

## Debug core

Code: `src/core/machine/debug_hooks.hpp` (interface and flag maps), the hook calls in `Machine::run`, `MemoryMap::locate`/`bank_bytes`, and `src/debug/` (library `freetvc_debug`, with no OS, threads or clocks, like the core): `z80_decode.{hpp,cpp}`, `debugger.{hpp,cpp}`, `expression.*`, `symbols.*`, `disassembler.*`, `profile.*`, `source_map.*`. Tests: `debug.core` (hand-assembled programs in all-RAM mode) and `debug.rom.<preset>` (all 5 ROM sets).

**Hooks.** A `Debugger` attaches itself to a `Machine` by pointer. `DebugHooks` holds a 64 K flag map (exec/read/write per CPU address) and a 256-entry port map (in/out). The machine calls `on_access` only for a flagged address or port, and `on_instruction` only for a flagged PC or when `every_instruction` is set (trace, step out, profile). A copied `Machine` has no attachment; `restore()` keeps the target's own.

**Instruction boundary.** z80.h's `opdone` tick, which holds T1 of the next opcode fetch (`Machine::at_boundary()`). `Machine::pc()` is the start of the instruction in progress, or of the next one at a boundary; z80.h's internal PC is already one further. Registers can be written only at a boundary; a new PC restarts the fetch there (`jump`).

**When a checkpoint stops.**
- Exec: before the instruction. A run never stops on the exec checkpoint of the instruction it starts at: after a stop the fetch is already under way, and after `jump()` the first fetch is a restart. The first instruction after a reset or power-on does stop, so an exec checkpoint at 0000h is "break at reset".
- Read, write, in, out: at the boundary after the instruction that made the access. The hit carries the address (port = A0–A7), the value, the bank and offset, the instruction's PC and the tick.
- Reads exclude opcode fetches (M1) but include operand bytes, as in VICE.
- While the CPU is halted, its refetches at HALT+1 every 4 T are not instructions for the hooks or the trace (they are still counted by `RunLimits::max_instructions`).

**Checkpoint fields.** Access kinds, inclusive range, optional bank (memory kinds; or a bank-offset checkpoint, `Checkpoint::offsets`, that fires wherever the bank is paged), enabled, stop or log only, temporary (deleted after it stops), ignore count, hit count, and a condition (an expression, "Expressions" below; `Checkpoint::condition_text` keeps its text). Every counted hit that is not ignored goes to a 1024-entry hit log.

**Banks.** `SYS CART EXT U0–U3 VID0–VID3 CARD0–CARD3 OPEN`. CARDn is C000–DFFF of the IOMEM slot; EXT offsets start at E000. Banks a board lacks have no bytes (U2/U3 on the 32k, VID1–3 below the 64k+). Seen on the ROMs: the OS runs with U0 in page 0 (`P_SAVE` = 70h, [rom.md](../hardware/rom.md) §4), so its RST vectors execute from a U0 copy, not from SYS (`debug.rom`, all presets).

**Stepping.**
- `step(n)`: mid-instruction, the first step completes the current instruction.
- `step_over`: a CALL, CALL cc or RST runs until SP is back at the call's level, wherever the code returns to. This covers RST 30h's function byte ([rom.md](../hardware/rom.md) §4), RST 18h's BASIC ops and the ROM's inline-print strings, which all return further on than the next address (a step over the RST 18h at 2.2 EAB8h would never stop if the target were the next address). Block repeats and HALT run to `pc + length`. Anything else is one step. Every front end (`n`, `lnext`, DZRP, GDB, MCP) uses it.
- `step_out`: stops at the first boundary after a RET/RET cc/RETI/RETN that left SP above its starting value.
- `run_to(address, bank)`.
- `start_step_over/out/run_to` keep their target across `run()` slices, so a step over a call that waits for a key just keeps running. Any stop other than the budget ends them.
- Checked on the ROMs: stepping out of the OS handler from 0030h lands after the function byte with SP + 2 (2.x: CA6Bh function 71h, 1.2: DA1Ch function 0Ch). Stepping over the same RST 30h returns at the same SP.

**Decoder.** Length and flow class (next, jump, branch, call, return, repeat, halt) from the x/y/z opcode fields. A DD/FD before DD, FD or ED joins the next instruction, because z80.h makes no boundary between chained prefixes. `debug.core` checks all 1647 fall-through opcodes (plain, CB, ED, DD, FD, DD CB, FD CB) against the PC advance in z80.h.

**Trace.** A ring of `TraceEntry` (tick, bank/offset, 4 bytes, registers before the instruction, entered through an interrupt). It is off by default: `set_trace_capacity(n)` turns it on. The text monitor turns it on with 65 536 entries (`chis`).

**Event log and I/O log.** While events are on (`set_events`), port accesses and interrupt entries are recorded with the beam position (`BeamEvent`) for Bench's Screen markers, two frames at a time. Every such event also goes into `io_log()`, a ring of 65 536 `IoEvent`s (tick, frame, the `BeamEvent`); it is kept when events go off and emptied by `clear_io_log()`.

**Heat.** `debug::HeatMap` stamps every byte's last exec, read and write with the frame number while Bench's Memory panel is visible ([ui.md](ui.md#memory)).

### Profile

`src/debug/profile.*`. `set_profile(true)` times every instruction start (it joins `every_instruction`): the ticks until the next start belong to the instruction, so wait states count and a HALT collects the time it waits; an interrupt acknowledge counts to the instruction it follows ❓.

- Per byte (bank, offset): executions and ticks; an LDIR repeat is one execution.
- Per routine, through a shadow call stack: a CALL or RST whose SP went down by 2 (by 4 when an interrupt came right after it) pushes a frame for its target, an interrupt entry one for the handler; any instruction with SP above a frame's SP pops it (RET, RETI, a dropped return address, a stack switch). Exclusive ticks go to the routine on top (or "outside any call"), inclusive ticks to a routine's outermost frame when it pops (recursion counted once). Over 64 frames the oldest is dropped without credit.
- Off keeps the counts and forgets the calls in progress; time going back (snapshot, rewind) restarts the timing.
- The monitor's `profile on|off|clear|[count]` lists the top routines; Bench's Profiler panel shows routines, instructions and a Listing column ([ui.md](ui.md#profiler)). Cost: about +11 % while recording.
- Tests: `debug.core` (a hand-timed call pattern), `debug.rom.*` (50 frames at the prompt: every tick in an address and a routine, the interrupt entry 50 times, the stack back to depth 0).

### Source level

`src/debug/source_map.*`. The monitor's `ll` recognises the file and loads its labels into the symbol table and its source lines into a `SourceMap` (CPU addresses, not banked ❓; relative names are taken from the debug file's folder):

| Format | Made by (checked with) | Lines | Labels |
|---|---|---|---|
| SLD (`\|SLD.data.version\|1`) | sjasmplus `--sld` in a DEVICE (1.24.0) | `T` records, one per instruction, exact; a macro's code on its call line | `L` records: module, main, local joined with `.`; not module/struct/macro markers |
| listing (`# file opened:`) | sjasmplus `--lst [--lstlab]` (1.24.0); this is also what YATE calls an "SJASM list" | lines that emit bytes, exact; the file stack from `# file opened/closed` | the `Value Label` table |
| `.noi` (`DEF name 0xADDR`), `.cdb` (`L:name:ADDR`), `.map` (`ASxxxx Linker`) | SDCC 4.2.0 `--debug` | `C$file$line$…` starts; `A$module$line` starts in `module.asm` (rank 1) | plain names, `G$name` globals, `Fmodule$name` statics; not `XG$`, `L…$`, `s__`, `l__` |
| map (`name = $ADDR ; kind, scope, , module, section, location`) | z88dk 2.4 `zcc -debug -m`, `z80asm -debug -m` (its Windows build, run under Wine) | `__C_LINE_` starts (location `file::function::…:line`; not those without a section, the preprocessor's) and `__ASM_LINE_` starts (labels only) | every other `addr`/`const` name up to FFFFh |

- Lookup: exact records answer only their own address; a line start lasts until the same file's next record, at most 256 bytes. Where several files have a line, the source (rank 0) beats a generated listing; at one address the record written last wins (SDCC writes `C$main.c$5` then `$7` for main's first instruction: line 7).
- A newer load wins over an older one at the same addresses, in both `SourceMap` and `SymbolTable`, so reloading a rebuilt program never keeps stale labels or lines.
- `lstep [n]` steps instructions until the PC is on another source line (code without lines, such as the ROM, is stepped through); `lnext [n]` does the same with step over. Both stop on checkpoints and give up after 100 frames. `src [addr]` names the line. Output lines read `main.c:7  counter = twice(counter);`.
- MCP: `symbols load` takes the same files, `step` has the modes `line` and `line_over`, and listings carry each row's source line. Bench: the Source panel ([ui.md](ui.md#source)).
- Tests: `debug.source_map` (each format's real output from a sample, lines and labels checked by hand), `debug.disasm` and its sjasmplus round trip.

### Saved states

Named in-memory copies of the whole `Machine` (about 1 MB each; ROM images and framebuffers included): the monitor's `snap`, MCP's `snapshot save|load`. Loading drops audio not yet read. The test runs a ROM boot after a load and gets the same ticks, RAM and frame as the first time. Snapshot files on disk (TVCS) and rewind are described in [media.md](media.md), sections "Snapshots" and "Rewind".

### Cost

gcc-release, a 450-frame boot of about 330 ms: an attached debugger with no checkpoints is within the noise (0–3 %). Trace plus 3 logging checkpoints adds 10–15 %. With the debugger attached, boots give the same ticks, RAM and frame as without it, on every preset.

## Expressions

Code: `src/debug/expression.{hpp,cpp}`. Test: `debug.expression`.

- **Numbers.** Hex by default. `$1F`, `0x1F` and `1Fh` are hex, `#31` is decimal, `%101` is binary. `ParseOptions::radix` changes the default (10 for front ends that want it; the monitor's `radix d`).
  - A token that starts with a letter is a name, so hex that starts with a letter needs `$` or a leading 0 (`0FFh`). The error message says so.
- **Operators.** C precedence (not VICE's left to right), from lowest: `||`, `&&`, `|`, `^`, `&`, `== != =`, `< <= > >=`, `<< >>`, `+ -`, `* / %`; unary `- ! ~ +`; parentheses.
  - Arithmetic is 64-bit signed. Comparisons give 0/1. Division or modulo by zero gives 0. Logical operators short-circuit.
  - Limits against hostile input: at most 100 nested brackets or prefix signs, and a tree at most 1000 operators deep (a chain of 1000 additions still parses). Past them the parse fails with a message; evaluating and freeing a tree never recurse deeper. Without them, one MCP request with 100 000 nested parentheses overflowed the stack.
- **Memory.** `[a]` is a byte and `{a}` a little-endian word, both in the current CPU view. `[BANK:a]` / `{BANK:a}` read a bank at an offset (`[U2:1000]`, `{VID1:0}`); a bank the board lacks reads FFh.
- **Names** are case-insensitive:
  - registers: `A F B C D E H L I R AF BC DE HL IX IY SP PC WZ/MEMPTR AF' BC' DE' HL' IXH IXL IYH IYL IM IFF1 IFF2`;
  - flags as 0/1: `SF ZF HF PF/VF NF CF`;
  - time and beam: `TICKS` (half-T), `CYCLE` (T), `FRAME`, `LINE`, `COLUMN` (the beam's framebuffer row and character column);
  - from the checkpoint hit: `VALUE`, `ADDRESS`/`PORT` (0 without a hit).

  `PC` is `Machine::pc()`. Anything else is looked up in the symbol table when parsing (exact name, then a unique case-insensitive match). A bank name followed by `:` is a bank, not a symbol.
- **Address arguments.** `parse_address("U2:start")` gives an optional bank plus an expression; front ends use it for checkpoint and memory addresses.
- **Conditions.** `make_condition(expr)` turns an expression into a checkpoint condition (true where non-zero). The ROM test stops at the first output system call with `U0:OS_ENTRY` and `([{SP}] & 80h) == 0`.

## Symbols

Code: `src/debug/symbols.{hpp,cpp}`, `rom_symbol_files.hpp` plus a generated `rom_symbol_files.cpp` (`cmake/EmbedText.cmake`).

- **One loader for every label format.** Lines are `name = value` (the `docs/hardware/rom-symbols/` format), `name EQU value` or `name: EQU value`. That covers sjasmplus `--sym` and `--exp`, checked on real 1.24.0 output, including `module.label` and `label.local`. Debug files with source lines go through the same `ll` ("Source level").
  - `;` starts a comment.
  - Values above FFFFh are skipped: sjasmplus lists every EQU.
  - Bad lines are reported as "line N: …" and loading continues.
- **Banked and unbanked symbols.** A file can be loaded with a `Placement` (bank, the CPU address of offset 0, and an address range). Its symbols then live at a bank offset, so a ROM routine keeps its name wherever the ROM is paged.
  - Reverse lookup (`at`) tries the banked symbols at the current bank and offset first, preferring one written at this CPU address. So SYS offset 0 is `reset` at 0000h and the page-3 name at C000h.
  - Unbanked symbols then match by CPU address.
- **Built-in ROM symbols** (`load_rom_symbols`). The files in `docs/hardware/rom-symbols/` are compiled in at build time, so the docs stay the only copy. They are chosen by the CRC-32 of the SYS and EXT images (the same CRC-32s as MAME's, [rom.md](../hardware/rom.md) §1):

  | Image | CRC-32 | Files and ranges |
  |---|---|---|
  | SYS 2.2 | 79FA818Ch | `tvc-sys22.sym` (page 3), `tvc-sys22-page0.sym` |
  | SYS 2.1 | 53C8FF98h | the same two files, but only C0E8–CF01 and 021C–034F, where 2.1 equals 2.2 ([rom.md](../hardware/rom.md) §1) |
  | SYS 1.2 | 6DA8809Ah | `tvc-sys12.sym`, `tvc-sys12-page0.sym` |
  | EXT 2.x | 05E1C3A8h | `tvc-ext2x.sym` |
  | EXT 1.x | 59C6B9F9h (the 4 KiB image) | `tvc-ext1x.sym` |
  | any known SYS | – | `tvc-ram.sym` in U0 |

  Counts on the ROM test: 340 names for 2.2, 222 for 2.1, 224 for 1.2. A preset switch reloads them.

## Disassembler

Code: `src/debug/disassembler.{hpp,cpp}`. Tests: `debug.disasm`, `debug.disasm_sjasmplus`.

- **Syntax.** Zilog mnemonics in upper case, sjasmplus syntax, and the manuals' hex style (`1Fh`, `0C000h`). Displacements look like `(IX+05h)` and `(IY-03h)`; relative jumps show the absolute target.
- **Undocumented forms.**
  - Printed as sjasmplus writes them: `SLL`, `IXH`/`IXL`, `IN F,(C)`, `OUT (C),0`, and `RLC (IX+d),B` for DD CB with a register copy.
  - Undefined `ED xx` becomes `DB 0EDh,xxh`, with a comment that it is an 8 T no-op.
  - A prefix that changes nothing becomes `DB 0DDh : <instruction>`, and chained prefixes are written the same way.
  - Second encodings (the NEG/RETN/IM copies, `ED 63/6B`, BIT (IX+d) with z ≠ 6) are flagged `alias`: their text assembles to the usual bytes.
- **TVC inline data** (`tvc_inline`, on by default):
  - `RST 30h` takes the next byte as its function code: `RST 30h : DB 21h`, length 2. The comment is the [OS] name and class/function from [rom.md](../hardware/rom.md) §5 (`ED_CHOUT (class 2, function 1)`).
  - `RST 08h` takes the BASIC error number ([rom.md](../hardware/rom.md) §2).
  - `RST 18h` takes the BASIC ops after it: bytes up to and including the first with bit 7 set, op 05h (also as the last op, 85h) with one operand byte: `RST 18h : DB 05h,22h,05h,22h,80h`. Evidence ([rom.md](../hardware/rom.md) §6): the interpreter (2.2 F7DBh–F816h, 1.2 FB82h–FBB8h) and op 05h (E607h, EA82h) disassembled; across all four ROM images only these address `BAS_IP` (1718h) directly (byte scan); stepping over the RST 18h at 2.2 EAB8h and EAC7h returns at EABEh and EAC9h. More than 32 bytes without an end op, or ops past the bytes given (a trace entry), leave a plain `RST 18h`.
  - A CALL to an address in `string_calls` takes a length byte and that many characters: `CALL print_inline : DB 03h,"ok",0Bh`, with the text in the comment; quotes, backslashes and control codes are written as numbers, so sjasmplus reads it back. `tvc_disasm_options(table, machine)` fills the list from the symbol `print_inline` (2.2 FB78h, 1.2 FE79h) and gives the labels; every front end uses it. The machine form reads 260 bytes for a CALL or RST 18h.
- **Labels.** Only for jump/call targets and `(nn)` memory operands, never for immediates (a counter in `LD BC,0010h` should not be called `RST10_ERRNZ`). `symbol_labels(table, machine)` gives names under the current paging.
- **Checked.**
  - `debug.disasm_sjasmplus` writes every opcode family (plain, CB, ED, DD, FD, DD CB, FD CB with sample operands; 1792 instructions), each in its own 8-byte slot, and assembles the file with sjasmplus 1.24.0. 1660 are byte-exact; the 132 aliases read back as the same text. RST 18h ops and inline strings are part of the round trip too (the test prints its own counts).
  - A deliberate mutation (SLA printed as SRA) makes 24 checks fail, so the comparison works.
  - On the ROMs, 0030h reads `JP INFUNC`, 0038h reads `PUSH AF / LD A,70h / OUT (02h),A / JP irq_handler` (2.2; `irq_entry` on 1.2), and the first output calls read `RST 30h : DB 71h ; HI_MEM_SET` (2.x) and `RST 30h : DB 0Ch ; PAL` (1.2).

## Event checkpoints

- **IRQ** (`kAccessIrq`, range = handler addresses): stops before the first handler instruction after an interrupt acknowledge, with the bank of the handler.
- **Raster row** (`kAccessLine`, range = framebuffer rows 0–511): reported when the beam starts the row. It stops at the next instruction boundary. With the ROM's CRTC values, two stops at row 100 are 125 608–125 610 half-T apart: one frame plus at most one instruction.
- **Break at reset**: see "When a checkpoint stops" above.
- Paging and video mode changes are port checkpoints (out 02h, 0Ch–0Fh, 06h) and need no kind of their own.
- Tests: the event part of `debug.core`, and `debug.rom.<preset>`.

## Text monitor

Code: `src/remote/` (library `freetvc_remote`; sockets, threads and files are allowed here):
- `monitor.{hpp,cpp}`: the command engine, called only on the host thread;
- `monitor_hub.{hpp,cpp}`: the sources (TCP clients, stdin, script, Bench's Console as a local client) and the queue;
- `net.{hpp,cpp}`: TCP over POSIX/Winsock;
- `png.{hpp,cpp}`: screenshots for `scrsh` and MCP: indexed PNGs (the 16 IGRB colours plus blanking as palette entries) compressed with our own deflate (greedy LZ77 over hash chains, fixed Huffman codes) and our own CRC-32/Adler-32. A text screen is 2–4 KB (an unindexed, uncompressed RGB image would be 560–750 KB), encoded in about 1.3 ms; random noise, the worst case, is 213 KB in 6 ms. `remote.png` decodes test frames with Python's zlib pixel by pixel; a real frame matches the headless PPM exactly, and the Windows build writes the same bytes.

`ScreenArea`/`kTvArea` live in `core/video/framebuffer.hpp`, so the GUI and the monitor crop the same TV area.

### Starting it

| Front end | Options |
|---|---|
| `freetvc-headless` | `--monitor` (stdin/stdout, starts in the monitor), `--remote-monitor` (127.0.0.1:6580), `--monitor-port N` (0 = any free port, printed on stderr), `--moncommands FILE` |
| `freetvc` (GUI) | `--remote-monitor`, `--monitor-port N`, `--moncommands FILE`; `monitor` in `freetvc.ini`; the settings window. The monitor engine always exists in the GUI (Bench and its Console use it); these options only open its TCP listener or run a script |

Listeners bind 127.0.0.1 only. Several TCP clients may be connected: command output goes to the client that sent it, stop reports go to all. With no client, output goes to stdout (in the GUI, to Bench's Console).

### Behaviour

- **Entering stops the machine (as in VICE).** Any line from a client while the machine runs first stops it: the current instruction completes, a `(stopped by the monitor)` report and its listing line follow, then the command runs.
  - Output ends with VICE's prompt `(C:$PC) ` while the monitor holds the machine.
  - `x`, `g`, an `n` over a call, `ret` and `until` print nothing, like VICE. The next stop prints its report and the prompt.
- **Steps across slices.** `n`, `ret`, `until` and the line steps use `Debugger::start_step_over/out/run_to`, whose targets persist across run slices.
- **Scripts** (`--moncommands`):
  - Lines run in the monitor, one after another; `;` starts a comment line.
  - A run command lets the machine go, and the script continues after the next stop.
  - If the script never ran the machine, the machine runs when the script ends.
- **When a headless run ends:** on `quit [code]` (the exit code); at an explicit `--tstates`/`--frames` budget; or when the monitor holds the machine and nobody can resume it (no listener, console at end of file, script done). `--instructions` is ignored with a monitor.
- **GUI.** While the monitor holds the machine, Play shows "paused" and a notice "monitor: stopped at XXXX". The Pause key or menu item resumes a held machine (`x`).
  - Keys typed into the window while it is held are lost, as on a paused machine; `keybuf` types while it runs.
  - A preset switch reloads the ROM symbols.
- **History.** The monitor turns the instruction history on (65 536 entries; `chis off` saves the 10–15 %). Hits of `trace` checkpoints are printed after each run slice, at most 64 lines per slice plus a "(N more trace hits)" line.

### Commands

`help` lists them all with their usage; `help NAME` shows one. Numbers are hex by default, `#` decimal, `radix d` switches the default. Checkpoint numbers are decimal.

| Group | Commands (aliases in brackets) |
|---|---|
| execution | `g [addr]` (`goto`), `x` (`exit`), `z [n]` (`step`), `n [n]` (`next`), `ret` (`return`), `until addr` (`un`), `lstep [n]`, `lnext [n]`, `reset [hard]`, `quit [code]` |
| state | `r [reg=value, …]` (`registers`), `m [[BANK:]start [end]]` (`mem`), `d [start [end]]` (`disass`), `> addr bytes`, `f` (`fill`), `h` (`hunt`), `t` (`transfer`), `c` (`compare`), `bank`, `io`, `sw [reset]` (`stopwatch`), `chis [n\|on\|off]`, `profile [n\|on\|off\|clear]` (`prof`), `snap save\|load\|delete\|list` |
| checkpoints | `break [kinds] [[BANK:]start [end]] [if cond]` (`bk`; no arguments: list), `watch` (`w`; load+store), `trace` (`tr`; log only), `delete [n…]` (`del`), `enable` (`en`), `disable` (`dis`), `condition n [if] expr` (`cond`), `ignore n [count]` |
| symbols, files, input | `ll "file"` (labels and source lines), `src [addr]`, `al addr name`, `dl name`, `shl [filter]`, `cl`, `bload "file" addr` (`bl`), `bsave "file" start end` (`bs`), `scrsh "file.png" [full]`, `keybuf "text"`, `radix [h\|d]`, `help` (`?`) |
| media and snapshots | `attach ["file.cas"]`, `detach`, `autostart "file.cas"`, `tape [insert\|autostart\|record "file" \| play\|stop\|rewind\|eject]`, `dump "file"`, `undump "file"`, `rewind [seconds]`, `cart [...]`, `disk [...]` ([media.md](media.md), [cards.md](cards.md)) |
| cards and ports | `gamecard`, `multichip`, `sid`, `card256`, `nanosd`, `sddual`, `printer`, `serial` (each with no argument shows the device; `help NAME` gives its forms; [cards.md](cards.md)) |

- **Checkpoint kinds:** `exec load store in out irq line`. Rows for `line` are written in decimal with `#` (`break line #100`).
- **`BANK:x` means an offset in that bank everywhere.**
  - In memory commands it reads and writes the bank wherever it is (`m U2:1000`, `bload "f" VID1:0`).
  - In checkpoints it becomes a bank-offset checkpoint that fires wherever the bank is paged. For example, `break SYS:0405` stops at C405h, and at 0405h if SYS is in page 0.
- **ROM writes.** `>` and `bload` into a ROM bank patch the loaded image (Bench's Memory panel alone keeps ROM read-only).
- **Quoting.** Quoted file names are taken raw, so Windows paths keep their backslashes. Only `keybuf` text has escapes: `\n` / `\r` = RETURN, `\t`, `\"`, `\\`, `\xNN`.

### Checked

- **`remote.monitor`** (Linux, and the MinGW build under Wine): every command group on a RAM program; a script through the hub (no doubled prompt); a real TCP client thread that sends `r\r\n`, `break`, `g 8000`, `quit`.
- **`remote.headless_script`**: the command-line path on the 2.2 ROM; `boot_call.mon` stops at the first output call and `chis 2` shows `RST 30h : DB 71h`.
- **By hand:**
  - a Python TCP client against the headless runner;
  - the stdin console;
  - the GUI with real X input (`tools/ui/ui_drive.py`): a TCP `r` holds it ("paused" plus notice), keys typed while held are lost, Pause resumes it, typing `6` stops it at 0030h before the echo, `delete` + `x` lets the `6` appear, and `quit 0` exits the GUI with code 0;
  - the `scrsh` PNG decodes with PIL (650 × 287) and matches the window.
- **Not checked:** the Windows GUI, and the Windows build against a real Windows TCP stack (only under Wine).

## MCP server

Code, all in `src/remote/`:
- `mcp.{hpp,cpp}`: the protocol, both eras;
- `mcp_tools.{hpp,cpp}`: the 20 tools;
- `mcp_host.{hpp,cpp}`: the stdio and HTTP transports.

The tools work through the text monitor's `Monitor` (shared hold state, keybuf queue, symbols) and the `Debugger`. Specification: [MCP 2026-07-28](https://modelcontextprotocol.io/specification/2026-07-28) and, for legacy clients, [2025-11-25](https://modelcontextprotocol.io/specification/2025-11-25); summary in [protocols-reference.md](protocols-reference.md#10-mcp-specification-essentials).

### Starting it

| Front end | Options |
|---|---|
| `freetvc-headless` | `--mcp-stdio` (stdin/stdout; logs and monitor output go to stderr; the machine waits for tool calls and the run ends when stdin closes), `--mcp-http`, `--mcp-port N`, `--mcp-token T`, `--mcp-allow-monitor` |
| `freetvc` (GUI) | `--mcp-http`, `--mcp-port N`, `--mcp-token T`, `--mcp-allow-monitor`; `mcp` and `mcp_allow_monitor` in `freetvc.ini`; the settings window (which shows the token with a Copy button) |

- **HTTP** listens on 127.0.0.1:6584. Every request needs `Authorization: Bearer <token>`. Without `--mcp-token` a random token is made and printed on stderr.
- **Text monitor alongside.** MCP, the text monitor, DZRP and GDB can run together: all use the same hold state.
- **Client configuration**, in the `mcpServers` JSON shape that many MCP clients read (for example from a project's `.mcp.json`); paths are examples:

```json
{"mcpServers": {
  "freetvc": {"command": "/path/to/freetvc-headless", "args": ["--preset", "64k+-2.2", "--mcp-stdio"],
              "env": {"FREETVC_ROM_DIR": "/path/to/roms"}},
  "freetvc-gui": {"type": "http", "url": "http://127.0.0.1:6584/mcp", "headers": {"Authorization": "Bearer TOKEN"}}
}}
```

### Protocol (dual era)

- **Modern requests** carry `_meta["io.modelcontextprotocol/protocolVersion"]`. They are served statelessly:
  - `server/discover`, `tools/list`, `tools/call`; everything else is −32601 (HTTP 404);
  - version 2026-07-28 only; another version gets −32022 with `supported` (modern plus the four legacy versions) and `requested`;
  - `clientCapabilities` is required (−32602, HTTP 400 without it);
  - results carry `resultType` and `_meta` serverInfo;
  - `server/discover` and `tools/list` carry the required caching hints `ttlMs` 3 600 000 and `cacheScope` "public", since the tool set never changes while the program runs.
- **HTTP header checks (modern).** `MCP-Protocol-Version` and `Mcp-Method` must equal the body, and `Mcp-Name` (base64 sentinel decoded) must equal `params.name` for `tools/call`. A mismatch gets HTTP 400 with −32020.
- **Legacy requests.** `initialize` answers with the client's version if it is one of 2025-11-25, 2025-06-18, 2025-03-26 or 2024-11-05, else 2025-11-25.
  - Later requests without `_meta` are served the same way, with no session (no `Mcp-Session-Id`); `ping` works there.
  - Legacy results have no `resultType`.
- **HTTP transport.** Only `POST /mcp`, always answered with `application/json` (no SSE).
  - GET/DELETE get 405, other paths 404.
  - A foreign `Origin` gets 403; only the 127.0.0.1, localhost and [::1] origins of the port are accepted.
  - A bad or missing token gets 401.
  - Notifications get 202. `Expect: 100-continue` works; chunked bodies get 411.
  - Connections are kept alive.
- **stdio transport.** One message per line, replies in order. Notifications and client responses get no reply. stdout is binary on Windows (no `\r`).
- **Errors.** Unknown tools are protocol errors (−32602). Bad arguments and failures inside a tool are tool results with `isError: true` and a message the model can act on (the `$FF` hint, for example).
- **Not implemented:** resources, prompts, `subscriptions/listen`, progress notifications, cancelling a running tool call.

### Tools and the time model

The machine moves only when a tool asks for it. `run`, `step`, `type_text` and `press_keys` leave it paused. `control resume` lets it run until a checkpoint stops it: in the GUI in real time, in the headless runner as fast as it can.

Tool calls run on the host thread. In the GUI a long `run` holds the window until it returns (at most `timeout_ms`, default 10 s, capped at 60 s; at most 30 000 frames).

| Tool | What it does |
|---|---|
| `machine_info` | machine/preset, SYS and EXT ROM by CRC-32, paused/running, PC, frame, T-states, beam, page map, checkpoint count |
| `control` | pause, resume, reset (the reset button, memory kept), power_cycle (RAM cleared) |
| `run` | frames (default 1), tstates or instructions, optional `until` address; returns `stopped_by` (budget, step, checkpoint, timeout), hits and the next instruction |
| `step` | into / over / out, with a count; line / line_over by source lines |
| `registers` | all registers; `set` changes them (values as integers or expressions) |
| `memory` | read / write / fill / search, CPU view or `BANK:offset`, hex, base64 or an integer array. A banked range past the bank's end is refused. Writes into ROM banks patch the loaded image, as the monitor's `>` does |
| `disassemble` | listing with labels, system-call names, RST 18h ops and inline strings; each row's `source` line when a debug file is loaded (also in every `next_instruction`) |
| `breakpoints` | add (all checkpoint kinds, bank offsets, conditions, log only, temporary, ignore), list, delete, enable, disable, clear |
| `screenshot` | PNG image content (TV area 650 × 287 or full raster 800 × 314) plus a text line; the indexed, deflate-compressed PNG of `remote/png.cpp` ("Text monitor" above). The GUI's own screenshot key uses SDL's writer |
| `screen_text` | the text screen from the OS's ASCII copy at U0:0100 (64 × 24, TVC accented letters mapped back to Unicode) |
| `type_text` | `\n` = RETURN; waits until typed plus `settle_frames` and returns the screen rows |
| `press_keys` | key names or ROW:COLUMN, held together for `hold_frames`; `JOY3_*`/`JOY4_*` with the GameCard |
| `snapshot` | save / load / delete / list in-memory states; save_file / load_file for TVCS files; rewind (seconds) |
| `game_saves` | game saves per program: list, save, check (the media a save names and whether each is there), load (with moved paths), delete ([media.md](media.md#game-saves)) |
| `symbols` | lookup (by name, or by address with its `source_line`), list, add, load: label files and the debug files of "Source level", through the monitor's loader |
| `trace` | instruction history with registers, and the latest checkpoint hits |
| `io_state` | video, CRTC, sound, IRQ, paging |
| `evaluate` | a monitor expression |
| `load_binary` | a host file into memory, optionally jump there |
| `media` | cassette, tape deck, cartridges (including `cart_build`), disks, every card and its media, the printer and the serial line ([media.md](media.md), [cards.md](cards.md)) |
| `card256` | the TVC256k++ card without side effects: `registers` (all 256, decoded), `sprites` (position, colour, phase, flags, data address; pixels as 24 × 21 rows of hex colours), `tiles` (the overlay's 32 × 30 screen codes and colours), `ram` (fast RAM or PSRAM) |
| `monitor_command` | one text monitor command; refused unless `--mcp-allow-monitor` (passthrough is opt-in) |

- **Arguments.** Address arguments are JSON integers (decimal) or monitor expression strings (hex default, symbols, `BANK:offset`).
- **Schemas.** They use `anyOf` instead of type arrays, because the MCP Inspector's `--strict` portability check flags type arrays.
- **Annotations.** Every tool carries read-only, destructive and idempotent hints.
- **Instructions.** The `instructions` text tells the model about the time model, the address syntax and the start-up logo: after power-on it waits for a key, so keys typed in the first 330 frames are lost.

### Checked

- **`remote.mcp` (CTest)**, also as the MinGW build under Wine:
  - both eras and their errors, on a RAM program: discover with the caching hints, initialize version choice, unknown method/tool, bad version, missing capabilities, parse error, ignored client responses;
  - most tools, including their error results;
  - 8 real HTTP requests over our socket layer (200, 401, 403, 400/−32020, the base64 `Mcp-Name`, 405, 202, legacy initialize);
  - on the 2.2 ROM: run 330 frames, press SPACE, type `print 6*7` → `screen_text` shows ` 42` then `ok`.
- **Harder checks** (`tools/mcp/stress_check.py`, raw HTTP and stdio; 55 checks headless and 54 against the GUI with `STRESS_ATTACH=1`): broken JSON, batches, odd ids, missing `jsonrpc`, wrong content type, invalid UTF-8, a short body, a 20 MB body (the connection is cut), pipelining, lowercase headers; a stalled half-sent request (others wait at most 17 ms in the GUI), 200 idle connections, 8 parallel clients × 40 mixed calls; wrong argument types, a 30 000-frame run cut at 200 ms, a full 64 KiB read, 512 lines across FFFFh, a ROM patch read back, bank ends, 1000 breakpoints, deep expressions, Hungarian letters and an emoji, random bytes as a symbol file, 100 000 steps, both screenshot sizes; snapshot determinism (same input twice, same SHA-256 of memory and registers); calls while running freely; a source-level session with sjasmplus (SLD, label breakpoint, line steps in and over, source lines in listings) and one with SDCC (C lines, a static by name) loaded over it; stdio garbage, a 1 MB line and EOF. Its cases cover expression nesting, reads and writes past a bank's end, a newer program's labels and lines replacing an older one's, and `symbols load` with every debug-file kind. The GUI shows every call in the Agent lane.
- **The official [MCP Inspector](https://github.com/modelcontextprotocol/inspector) 2.12.0** (npm `@modelcontextprotocol/inspector`, client SDK 2.3.1), a real third-party client, run by `tools/mcp/inspector_check.sh` ([tools/mcp/README.md](../../tools/mcp/README.md)):
  - stdio `tools/list --strict` in the legacy, modern and auto eras with 0 portability warnings (run with the first 19 tools, before `media` was added);
  - one HTTP emulator driven across calls in mixed eras to the `print 6*7` → 42 screen;
  - by hand: a decodable 650 × 287 PNG, a bank-offset conditional breakpoint, `evaluate`, and `monitor_command` refused.
  - It checks the caching hints the modern protocol era requires and the tool schemas' type arrays.
- **GUI with real X input** (`tools/ui/ui_drive.py`) while the Inspector drove it over HTTP:
  - `control pause` shows "paused";
  - `type_text print 6*7` appears with 42 while still paused;
  - `control resume` brings back 100 %;
  - XTEST typing (`print 1` → `1`) lands below it, and `screen_text` returns both;
  - the text monitor ran alongside and `quit 0` ended the GUI.
- **curl** against the headless HTTP server: the same status codes.
- **Not checked:** the Windows GUI; the client configuration above in a desktop MCP client.

## DZRP server

Code: `src/remote/dzrp.{hpp,cpp}` (the protocol engine, `DzrpServer`) and `src/remote/dzrp_host.{hpp,cpp}` (TCP, `DzrpHost`). Test: `remote.dzrp`. Real-client check: `tools/dezog/` (DeZog built from source, plain and with our patch; see [tools/dezog/README.md](../../tools/dezog/README.md)). Specification: [DeZogProtocol.md](https://github.com/maziac/DeZog/blob/main/design/DeZogProtocol.md) (DZRP 2.2.0). Two memory views: a flat ZX48K for any DeZog with the `dzrp` remote, and the TVC's own banks for DeZog with our patch ("TVC banks" below).

### What DeZog accepts

Read in DeZog's source, [github.com/maziac/DeZog](https://github.com/maziac/DeZog) at commit `715f2b35` (master, 2026-10-18, version 3.8.0-rc7, MIT); file and line references are to that commit.
- **The generic `dzrp` remote is unreleased.** It is new in 3.8.0 (`CHANGELOG.md` "3.8.0"). The latest release, 3.7.0 (2025-12-19), has only zrcp, cspect, zxnext, zsim and mame (`src/remotes/remotefactory.ts` at tag `v3.7.0`).
- **Only ZX memory models.** CMD_INIT's machine type must be 1–4 (ZX16K, ZX48K, ZX128K, ZXNEXT). 0 (unknown) makes DeZog fail with "Unknown machine type" (`src/remotes/dzrp/dzrpremote.ts:250–270`, the same in 3.7.0). `customMemory` works only for zsim (`src/settings/settings.ts:1329`).
- **DeZog owns conditions and steps.** It sends the condition text with CMD_ADD_BREAKPOINT, but checks it itself after the stop and continues if it is false (`dzrpremote.ts:820`). Step into/over/out are CMD_CONTINUE with up to two temporary breakpoints that DeZog computes (`remotebase.ts:1501` `calcStepBp`). The alternate step commands are never sent.
- **Breakpoints are matched by long address.** A breakpoint NTF_PAUSE must carry the bank+1 byte DeZog used, or DeZog finds no breakpoint and continues (`dzrpremote.ts:994`). Reason 0 (no reason) always stops, so it is right for step targets.
- **Step over `RST nn`** puts the second temporary breakpoint at pc + 2 (`remotebase.ts:1573`, "just in case the return address is manipulated"), so stepping over the TVC's `RST 30h : DB fn` system call returns after the function byte.
- **"Break on Interrupt" never reaches a DZRP remote** in unpatched DeZog. `DzrpRemote` does not override `enableBreakOnInterrupt`, so the base class answers "disabled" and CMD_ENABLE_BREAK_ON_INTERRUPT is never sent (`remotebase.ts:1158`; seen in the GUI check). Our server implements the command anyway; our DeZog patch passes it on.
- **Labels and banks.** With a custom memory model, sjasmplus `NOSLOT64K` banks are passed through unchanged, so every label would get bank 0 (`src/labels/sjasmplussldlabelparser.ts:450–461`; source reading, not run). `ZXSPECTRUM48` sources take the generic mapping instead: each address gets the initial bank of its slot (`src/labels/labelparserbase.ts:681–694`).
- **Unsupported commands** (from CMD_GET_SUPPORTED_COMMANDS) are turned into errors on DeZog's side (`dzrptransportremote.ts:148`). Without CMD_ADD_BREAKPOINT it would switch to the RST-patching simple mode.

### Design

- **The TVC is reported as a ZX48K, a flat view** (the default). Two slots: slot 0 = 0000–3FFF (bank 0), slot 1 = 4000–FFFF (bank 1). Both show whatever the TVC has paged in right now. GET_REGISTERS always reports banks 0 and 1, and the bank byte of incoming long addresses is ignored.
- **One run/hold state.** `DzrpServer` works through the text monitor's `Monitor`, like MCP. A DZRP breakpoint is a normal checkpoint, so the text monitor's `break` lists it.
- **Session.** CMD_INIT ends any earlier session and holds the machine ("DeZog"). CMD_CLOSE or a lost connection removes the session's breakpoints, watchpoints and interrupt checkpoint, and lets the machine run on. One client at a time: a second connection is closed at once.
- **CONTINUE** adds the two temporary breakpoints as temporary exec checkpoints and leaves the monitor. After every run slice the host asks `poll()`. Once the monitor holds the machine again, one NTF_PAUSE goes out:

| Stop | Reason | Address |
|---|---|---|
| a CONTINUE temporary breakpoint (also when a breakpoint shares the address) | 0 | PC |
| a DZRP breakpoint | 2 | PC |
| a DZRP watchpoint | 3 read / 4 write | the accessed address |
| the break-on-interrupt checkpoint | 255, "interrupt, handler at …h" | the handler |
| a checkpoint from the text monitor or MCP | 255, "freeTVC checkpoint #n at …h" (with reason 2 DeZog would not find it and continue) | PC |
| no new stop: CMD_PAUSE, the monitor, MCP | 1 | PC |

  The bank byte is bank+1 as DeZog's own long addresses have it: with the ZX48K view the slot's (1 below 4000h, 2 above), with TVC banks the bank of the hit. The remaining temporary breakpoint is removed.
- **PAUSE** while a CONTINUE runs sends its response, then NTF_PAUSE in the same write (the spec wants that order). Otherwise only the response.
- **Commands** (all reported by CMD_GET_SUPPORTED_COMMANDS):
  - INIT (protocol 2.2.0, "freeTVC <version>"), CLOSE, GET_REGISTERS, SET_REGISTER (all 35 numbers; refused between instructions);
  - CONTINUE, PAUSE, WRITE_MEM, LOOPBACK, READ_PORT / WRITE_PORT (real I/O with side effects, port = low byte);
  - INTERRUPT_ON_OFF (IFF1 = IFF2), READ_BANK_MEM / WRITE_BANK_MEM (ZX48K or TVC banks; FFh past the bank's end), READ_MEM_BLOCKS (wraps at 64 K);
  - ENABLE_BREAK_ON_INTERRUPT (an IRQ checkpoint), ADD / REMOVE_BREAKPOINT (the debugger's checkpoint id), ADD / REMOVE_WATCHPOINT (matched by address, size and access);
  - READ_STATE / WRITE_STATE: the TVCS snapshot bytes of [media.md](media.md) "Snapshots" (machine chunks only; WRITE_STATE has no error field, so a snapshot from another board or ROM set leaves the machine as it was). Checked in `remote.dzrp`.
- **Not supported:** SET_SLOT (no TVC paging through DeZog), EXEC_ASM, the ZX Next commands and the simple mode.
- **Framing.** A command's length counts only the payload after the sequence number and command id; a response's length counts the sequence number too (spec, `dzrptransportremote.ts:336`). Commands over 1 MiB drop the client.

### TVC banks

- **The DeZog patch** `tools/dezog/dezog-dzrp-custom-memory.patch`, against commit `715f2b35` (9 files, +231 / −33 lines, including the CHANGELOG and a 4-test file):
  - machine type 0 (UNKNOWN, already in the spec's CMD_INIT table) makes the generic `dzrp` remote build its memory model from `dzrp.customMemory` in `launch.json`. DeZog's `MemoryModel(customMemory)` existed already, but only zsim used it;
  - the zsim `customMemory` conversion moved into one function for both, which now also converts `bankSize` and `bankOffset`;
  - `enableBreakOnInterrupt` is passed to the remote;
  - schema, `DeZogProtocol.md`, `Usage.md` and `CHANGELOG.md` updated.

  The patch changes DeZog's MIT-licensed sources (its licence is in `tools/dezog/LICENSE.DeZog.txt`); it is not part of upstream DeZog. `tools/dezog/build_dezog.sh` builds a patched DeZog from a clone.
- **freeTVC with `--dzrp-banks`** answers CMD_INIT with machine type 0, and uses five slots:
  - 0000, 4000 and 8000 are whole pages; C000 and E000 are the halves of page 3, because IOMEM splits page 3 into CARDn and EXT;
  - each slot reports its `MemoryMap::locate()` bank as the `Bank` number: SYS 0, CART 1, EXT 2, U0–U3 3–6, VID0–3 7–10, CARD0–3 11–14, OPEN 15.
- **The matching `customMemory`** is in `tools/dezog/project/.vscode/launch.json`:
  - the 16 K banks that page 3 can hold (SYS, CART, U3, OPEN) have `bankSize` 4000h, with `bankOffset` 2000h in the E000 slot;
  - SYS, CART and EXT are marked `rom`;
  - the initial banks are U0, U1, U2, SYS and SYS, the paging of a program started from BASIC.
- **Long addresses.** Breakpoints and watchpoints with a bank byte become checkpoints on the CPU address that hit only while that bank is paged there. A bank byte of 0 means any bank. READ/WRITE_BANK_MEM take the bank's own offsets (EXT from 0 = E000h).

### Starting it

| Front end | Options |
|---|---|
| `freetvc-headless` | `--dzrp`, `--dzrp-port N` (0 = any free port), `--dzrp-banks` (TVC banks). The machine runs as fast as it can until DeZog connects |
| `freetvc` (GUI) | `--dzrp`, `--dzrp-port N`, `--dzrp-banks`; `dzrp`, `dzrp_banks` in `freetvc.ini`; the settings window |

The launch configurations DeZog needs are in `tools/dezog/project/.vscode/launch.json` ("TVC banks" with `customMemory`, and "flat ZX48K"): `"remoteType": "dzrp"`, `"dzrp": {"hostname": "127.0.0.1", "port": 6582}`, an sjasmplus SLD, `loadObjs` and `execAddress`. Use `DEVICE ZXSPECTRUM48` in sjasmplus sources, because `NOSLOT64K` labels would all land in bank 0 with TVC banks (see above).

### Checked

- **`remote.dzrp` (CTest)**, also as `dzrp_test.exe` under Wine (Winsock), on a RAM program:
  - every supported command, the framing, the sequence numbers, the slots;
  - each NTF_PAUSE reason: a breakpoint, a step target after a CALL, a write watchpoint, PAUSE (response first), a monitor checkpoint (255 with text), the sound IRQ with break on interrupt;
  - CLOSE drops the checkpoints and resumes;
  - a session over a real TCP connection;
  - TVC banks: type 0, the five slots for port 02h B0h and D0h (VID0, CARD0, EXT), bank memory by offset, a VID0-qualified breakpoint that does not fire while U2 is paged while the U2 one does, and watchpoint removal by bank.
- **DeZog 3.8.0-rc7 from source in its own VS Code profile, driving the GUI on the 2.2 ROM** (`tools/dezog/`, XTEST keys, window grabs), flat view with a `NOSLOT64K` source:
  - "'freeTVC 1.0.1' initialized."; the program loaded at 6000h, and the editor stopped on `start`;
  - step over shows HL = 6019h, A = 48h 'H' and the flags;
  - a breakpoint on line 17 is hit ("Breakpoint hit @6011h"), and the freeTVC status strip shows "paused, monitor: stopped at 6011";
  - Continue and Pause give "Manual break.";
  - `-wpadd 0x602a 1 w` stops after `INC (HL)` with "Watchpoint write access at address 602Ah";
  - PC set to 6000h in the Variables pane;
  - step into `call print`, step over `rst 30h` lands on `pop hl` (6017h), step out returns to 600Bh with SP restored;
  - disconnect: the GUI runs on, and the monitor lists no checkpoints;
  - DeZog's transport log for the session: 13 command kinds, 17 CONTINUEs answered by 17 NTF_PAUSEs, no errors or timeouts;
  - the screen showed "HELLO FROM DEZOG" once the program ran. At the first breakpoint only "HELL" was drawn, although all 16 calls had been made (the monitor's `chis` shows it): the editor finishes drawing later ❓.
- **Headless over raw TCP** (Python): INIT, registers, a breakpoint at 0038h hit with reason 2, PAUSE with reason 1, CLOSE.
- **The DeZog patch**: its 4 tests pass and 3 of them fail on unpatched DeZog; DeZog's suite gives 1006 passing (1002 without the patch); eslint shows no new errors; the `launch.json` layout loaded into the patched `MemoryModel` gives the intended slots, sizes, ROM flags and offsets.
- **Patched DeZog against the GUI with `--dzrp-banks`** (2.2 ROM, XTEST, window grabs):
  - "Memory Banks" shows 0000 = U0, 4000 = U1, 8000 = U2, C000 = SYS, E000 = SYS;
  - a source breakpoint on `rst 30h` is hit as "Breakpoint hit @6015h (bank=4)" (U1);
  - step into lands on `0030.U0 jp 0B23h`, and the disassembly marks "0C418h is in a different bank";
  - `-md bank=2 0 16` and `-md bank=0 0x0030 8` give the EXT and SYS bytes that the text monitor shows, while neither bank is paged there;
  - "Break on Interrupt: enabled" stops at `0038.U0 push af` with "interrupt, handler at 0038h";
  - a text-monitor checkpoint at `EXT:0BCE` stops in ED_CHOUT as "freeTVC checkpoint #28 at EBCEh", and Memory Banks shows C000 = CARD0, E000 = EXT, with `EBCE.EXT call SUB_F00E`;
  - disconnect leaves no checkpoints and the machine running;
  - the transport log has CMD_ENABLE_BREAK_ON_INTERRUPT ×2 and CMD_READ_BANK_MEM ×2, with no errors.
- **Not checked:** DeZog's reverse debugging and code coverage (both on DeZog's side), the Windows GUI, a released DeZog (none has the `dzrp` remote yet), the flat ZX48K configuration with a `DEVICE ZXSPECTRUM48` source in the GUI.

## GDB stub

Code: `src/remote/gdb.{hpp,cpp}` (`GdbServer`: packet framing, acks, Ctrl-C and the commands) and `src/remote/gdb_host.{hpp,cpp}` (TCP, `GdbHost`). Test: `remote.gdb`. Real-client checks: DeZog's `gdb` remote (`tools/dezog/`, configuration "freeTVC (GDB stub)") and GNU gdb 18.1 built for the Z80 ([tools/gdb/README.md](../../tools/gdb/README.md)). Protocol: [GDB Remote Serial Protocol](https://sourceware.org/gdb/current/onlinedocs/gdb.html/Remote-Protocol.html).

### What the clients need

- **GNU gdb**: `gdb/z80-tdep.c` in [binutils-gdb](https://sourceware.org/git/?p=binutils-gdb.git;a=blob;f=gdb/z80-tdep.c) (master as of October 2026; GPL-3.0-or-later, read as documentation, only the register names are used):
  - the target description must have the feature `org.gnu.gdb.z80.cpu` with 13 registers named af, bc, de, hl, sp, pc, ix, iy, af', bc', de', hl', ir (`z80-tdep.c:155–164`, checked with `tdesc_numbered_register` at 1117–1129);
  - only the names are checked, so the wire order is ours, set by `regnum`. GDB's own `features/z80-cpu.xml` has a 32-bit pc for the eZ80; ours is 16 bit.
- **DeZog's `gdb` remote** (`src/remotes/gdb/gdbremote.ts`, 3.8.0-rc7, commit `715f2b35`):
  - reads `qXfer:features:read:target.xml` and requires `<architecture>z80</architecture>` (`:224–231`);
  - takes the `g` packet in MAME's order: AF BC DE HL AF' BC' DE' HL' IX IY SP PC (`z80registersgdbdecoder.ts`), and sets registers with `P` in that numbering (`gdbremote.ts:576`);
  - takes the stop reply's reason up to the first `:`, so `watch` / `rwatch` / `awatch` must come right after `T05`, and it finds the PC as `0b:` (`:380–408`);
  - pauses with Ctrl-C followed by `p0b` (`:706`);
  - reads only one packet per TCP read: `dataReceived` handles the first `$…#cs` and leaves the rest in its buffer until more data comes (`:254–318`; seen in the GUI check).
- **Both at once**: `target.xml` lists GDB's names with `regnum` in MAME's order (0 af … 7 hl', 8 ix, 9 iy, 10 sp, 11 pc, 12 ir).

### Design

- **Session.** One client at a time on 127.0.0.1:6583; a second connection is closed at once. On connect the machine is held ("GDB"). `D` (detach), `k` (kill) or a lost connection remove the session's breakpoints and watchpoints and let the machine run on. `k` does not end the emulator.
- **Packets.** `$payload#checksum` with acks until `QStartNoAckMode`. A bad checksum gets `-`, and a `-` from the client resends the last reply. Replies escape `$ # } *` and are never run-length encoded.
- **One packet per write.** The host sends at most one reply packet (with its ack) per service pass, and queues the rest for the next passes. Found with DeZog: a Ctrl-C answered by the stop reply and the `p0b` reply in one write left DeZog waiting until its 5 s timeout.
- **Commands:**
  - `qSupported` → `PacketSize=4000;qXfer:features:read+;QStartNoAckMode+;swbreak+;hwbreak+`; `qXfer:features:read:target.xml`;
  - `?`; `g`, `G`, `p`, `P` (writes only between instructions, as after any stop);
  - `m`, `M` (at most 1F00h bytes per read);
  - `c`, `C` and `s`, `S` with an optional address (the signal is ignored). `s` steps one instruction at once;
  - `Z0`–`Z4` / `z0`–`z4`: 0 and 1 are exec breakpoints, 2 write, 3 read, 4 access watchpoints over the length;
  - `D`, `k`, `H`, `T`, `qC`, `qfThreadInfo` / `qsThreadInfo` (one thread), `qAttached`, `qOffsets`, `qSymbol`;
  - `qRcmd` (gdb's `monitor`) runs one text monitor command and returns its text. A command that runs the machine is followed by holding it again, so GDB's stopped state stays true.
  - Everything else (`vCont`, `X`, `!`, …) gets the empty reply, so gdb falls back to `c`, `s` and `M`.
- **Banks (our extension).** Addresses from 10000h up mean `(bank + 1) << 16 | offset`, the same form as DeZog's long addresses, with the `Bank` numbers (SYS 0 … OPEN 15):
  - `m` / `M` there read or write the bank directly (FFh past its end);
  - `Z` there is a bank-offset checkpoint that fires wherever the bank is paged (`Checkpoint::offsets`).
  - Plain 64 K addresses are the CPU's view.
- **Stop replies** carry `0a:` (SP) and `0b:` (PC), with the reason first:
  - a watchpoint gives `T05watch:addr;` (`rwatch`, `awatch`). For a bank watchpoint, addr is in the bank form it was set in;
  - a breakpoint gives `T05swbreak:;` if the client listed `swbreak+` in `qSupported`, else `T05`;
  - a step gives `T05`;
  - Ctrl-C, or a hold by the text monitor or MCP, gives `T02`.

### Starting it

| Front end | Options |
|---|---|
| `freetvc-headless` | `--gdb`, `--gdb-port N` (0 = any free port). The machine runs as fast as it can until a client connects |
| `freetvc` (GUI) | `--gdb`, `--gdb-port N`; `gdb` in `freetvc.ini`; the settings window |

With gdb (built for `z80-unknown-elf`, [tools/gdb/README.md](../../tools/gdb/README.md)): `z80-unknown-elf-gdb -ex 'target remote 127.0.0.1:6583'`. With DeZog: `"remoteType": "gdb"`, `"gdb": {"hostname": "127.0.0.1", "port": 6583}` (in `tools/dezog/project/.vscode/launch.json`).

### Checked

- **`remote.gdb` (CTest)**, also as `gdb_test.exe` under Wine (Winsock), on a RAM program:
  - framing, bad checksum, resend, an incomplete packet, no-ack mode;
  - `target.xml` read in 80h pieces;
  - `g` (SP and PC at their offsets), `P` / `p`;
  - memory in the CPU view and in banks (U2, VID0, EXT's end);
  - a breakpoint (continue answers `+`, then `T05swbreak:;…0b:1080;`), a step, a write watchpoint (`T05watch:9000;`), a bank-offset breakpoint;
  - Ctrl-C (`T02`), `qRcmd`, detach;
  - a session over real TCP.
- **DeZog 3.8.0-rc7's `gdb` remote against the GUI** (2.2 ROM, XTEST, grabs):
  - "GdbRemote connected!"; the program loaded through `M` and started at `start` through `P`;
  - "Breakpoint hit @6015h"; step over `rst 30h` lands on `pop hl` (6017h);
  - `-wpadd 0x602a 1 w` gives "Watchpoint write access at address 602Ah";
  - Continue and Pause give "Manual break." at 600Eh (this is the exchange that hung on the `p0b` reply before the one-packet-per-write rule);
  - disconnect leaves no checkpoints and the machine running;
  - the transport log has no warnings or errors.
- **GNU gdb 18.1 against headless `--gdb`** (`tools/gdb/gdb_check.sh`, gdb built for `z80-unknown-elf` from [ftp.gnu.org/gnu/gdb/gdb-18.1.tar.xz](https://ftp.gnu.org/gnu/gdb/gdb-18.1.tar.xz), signature checked). Ubuntu's `gdb-multiarch` 15.1 has no Z80 target ("Undefined item: z80"; none of its 217 architectures).
  - `target remote` reads `target.xml`; `info registers` shows all 13 by name, with the flags decoded (`af 0x44 [ P/V Z ]`);
  - `restore hello.bin binary 0x6000` writes through `M`, and `x/6xb` reads it back;
  - `break *0x6015` is a `Z0` (the monitor lists the checkpoint), and `continue` stops there;
  - `stepi` on `rst 30h` lands at 0030h. gdb's Z80 target steps by itself with a temporary `Z0` at the next PC, not with `s`;
  - `watch` on 602Ah is a hardware watchpoint (`Z2`), with "Old value = 0, New value = 1" at 6012h;
  - `monitor m EXT:0000 0003` and `monitor bank` print the text monitor's output; detach works.
  - **gdb cuts addresses to 16 bits**: `x/4xb 0x30000` read 0000h. The bank addresses from 10000h therefore serve other clients; in gdb, use `monitor` with `BANK:offset`.
  - The warning "couldn't find `_break_handler`… RST 0x08" is harmless: gdb would patch RST 08h into memory only if `Z0` failed.
- **Not checked:** Ghidra or IDA as clients, the Windows GUI.

## Headless and CI use

`freetvc-headless` has no window and no audio device and runs as fast as it can. The options that matter for scripted debugging (full list: `freetvc-headless --help`; [build.md](build.md), section "Headless runner"):

| Need | Options |
|---|---|
| a budget | `--tstates N`, `--frames N`, `--instructions N` (ignored with a monitor) |
| scripted input | `--key KEY[+KEY]@F[:H]`, `--joy N:DIR@F[:H]`, `--type TEXT@F`, `--type-timing H:G[:R]` |
| programs | `--load FILE@ADDR`, `--pc ADDR`, `--port02 VALUE`, `--cas`, `--autostart`, `--tape`, `--cart`, `--disk`, `--snapshot` |
| results | `--screenshot FILE` (PPM, 800 × 314), `--save-snapshot FILE`, `--wav FILE`, `scrsh` from a script, monitor output on stdout |
| control | `--monitor`, `--moncommands FILE` (with `quit [code]` as the exit code), `--remote-monitor`, `--mcp-stdio`, `--mcp-http`, `--dzrp`, `--gdb` |

## Not implemented

- A DAP server, Lua or other scripting beyond monitor scripts, input recording and replay, a full trace to file, step-back (rewind goes back by whole seconds only; [media.md](media.md), section "Rewind"), the rtvc-style JSON symbol format.
- Checkpoint command lists (VICE's `command n "..."`), and event checkpoints for the tape motor, FDC commands or reset as their own kinds (reset is an exec checkpoint at 0000h; paging and mode changes are port checkpoints).
- MCP resources, prompts, progress notifications and cancellation.
- Screen text from the screen's pixels: `screen_text` reads the OS's ASCII copy at U0:0100, so text written straight to video RAM does not appear.
