# External debug and remote protocols — reference

Notes on the debug and remote protocols of other emulators and tools that freeTVC's own front ends follow or learned from: the VICE monitors, DZRP (DeZog), the GDB Remote Serial Protocol, ZEsarUX's ZRCP, Fuse, Emulicious, Mesen, emulators with MCP servers, and the MCP specification. freeTVC's own design is in [debug-and-remote.md](debug-and-remote.md).

Tags: **[v]** read from the primary source (linked); **[u]** not checked against the primary source, re-check before relying on it.

## 1. VICE text remote monitor [v]

Source: the VICE manual, [chapter 12, "Monitor"](https://vice-emu.sourceforge.io/vice_12.html).

- `-remotemonitor`, `-remotemonitoraddress ip4://127.0.0.1:6510` (resources `MonitorServer`, `MonitorServerAddress`); also `-moncommands <file>`, `-initbreak <addr|reset|ready>`, `-keepmonopen`, `-refreshonbreak`, `-debugcart` (regression exit code).
- One client at a time, polled from the emulator loop; first incoming data halts the machine (like the monitor hotkey). Line-based (`\r`, `\n` or `\r\n`, 260-byte buffer). Output ends with the prompt `(C:$xxxx) ` [u]; clients wait for the prompt. `x` resumes; the socket stays open but silent until the next break prints again.
- Commands: execution `g [addr]`, `z [n]`, `n [n]`, `ret`, `x`, `q`, `reset [type]`, `stopwatch`, `warp`, `chis`, `bt`; state `r [reg=val]`, `m [radix] [range]`, `mc ms i ii`, `> addr data`, `f h c t`, `d`, `a addr instr : instr`, `io`, `sc`, `bank`, `device`, `sidefx`, `mmsh/mmzap/mmsave`; checkpoints `break [load|store|exec] [a [b]] [if cond]`, `watch`, `trace`, `until`, `enable/disable/delete n`, `condition n if …`, `ignore n [count]`, `command n "cmd"`, `dummy`; conditions over registers, `RL`/`CY` raster, `@bank:$addr`, operators `== != < > <= >= + - * / & | && ||` (left to right); files `l s bl bs v`, `dump/undump`, `attach detach autostart autoload @ list`, `scrsh "f" [fmt]` (PNG = 2), `keybuf "text\x0d"`; labels `al dl ll sl shl cl`; scripting `pb`, `rec`; `resget/resset`; `profile`; `~`, `radix`.

freeTVC's text monitor uses VICE's command names, its stop-on-input behaviour and its prompt, but C operator precedence ([debug-and-remote.md](debug-and-remote.md#text-monitor)).

## 2. VICE binary monitor (port 6502, API version 0x02) [v]

Source: the VICE manual, [chapter 13, "Binary monitor"](https://vice-emu.sourceforge.io/vice_13.html). freeTVC does not implement it.

Little-endian, length-delimited; **any command halts the emulator**.

Request header (11 B): STX 0x02 · API 0x02 · body length u32 · request id u32 · command u8 · body.
Response header (12 B): STX · API · body length u32 · response type u8 · error u8 · request id u32 (`0xFFFFFFFF` = unsolicited event) · body.

Errors: 00 ok, 01 object missing, 02 bad memspace, 80 bad length, 81 bad parameter, 82 API version, 83 unknown command, 8F general failure. Memspace 0 = main, 1–4 = drives 8–11.

| ID | Command | Request | Response |
|---|---|---|---|
| 01 | memory get | sidefx u8, start u16, end u16, memspace u8, bank u16 | len u16 + data |
| 02 | memory set | same + data | – |
| 11 | checkpoint get | id u32 | checkpoint info |
| 12 | checkpoint set | start, end, stop u8, enabled u8, op u8 (1 load 2 store 4 exec), temp u8, [memspace] | checkpoint info |
| 13 / 14 / 15 | delete / list / toggle | id / – / id + enabled | – / infos + count u32 / – |
| 22 | condition set | id u32, len u8, expr | – |
| 31 / 32 | registers get / set | memspace / memspace, count, items (size, id, value u16) | register info |
| 41 / 42 | dump / undump | save roms, save disks, name | – / PC |
| 51 / 52 | resource get / set | name / type, name, value | type, value |
| 71 | advance instructions | step-over u8, count u16 | – |
| 72 | keyboard feed | text | – |
| 73 | execute until return | – | – |
| 81 | ping | – | – |
| 82 | banks available | – | count, (size, id u16, name) |
| 83 | registers available | memspace | count, (size, id, bits, name) |
| 84 | display get | vc u8, format u8 | header len, full w/h, inner x/y offset, inner w/h, bpp 8, buffer len, pixels |
| 85 | VICE info | – | version, svn rev |
| 86 | CPU history | memspace, count u32 | items: regs, clock u64, instr bytes |
| 91 | palette get | vc | count, RGB entries |
| A2 / B2 | joyport / userport set | port, value | – |
| AA / BB / CC | exit (resume) / quit / reset(type) | | |
| DD | autostart | run u8, file index u16, name | – |

Events: 0x11 checkpoint hit (id, hit, start, end, stop, enabled, op, temp, hit count, ignore count, has condition, memspace), 0x31 registers, 0x61 JAM(PC), 0x62 stopped(PC), 0x63 resumed(PC). Lessons: self-describing registers and banks (82/83) are good; halting on every command is intrusive.

## 3. DeZog / DZRP 2.2 — the main Z80 debugger front end [v]

Sources: [DeZogProtocol.md](https://github.com/maziac/DeZog/blob/main/design/DeZogProtocol.md) (DZRP 2.2.0) and DeZog's code, [github.com/maziac/DeZog](https://github.com/maziac/DeZog) at commit `715f2b35` (version 3.8.0-rc7, MIT).

DeZog remotes: `zsim`, `zrcp` (ZEsarUX, 10000), `cspect` (11000), `zxnext` (serial / 14000), `mame` (gdbstub, 12000), `gdb` (12100), **`dzrp` (generic, socket or serial, requires DZRP ≥ 2.2.0, experimental)**. `memoryModel: "CUSTOM"` describes slots/banks; an optional `ioMmu` JavaScript snippet runs on OUT; it is unnecessary if GET_REGISTERS reports slots.

Framing (little-endian): command = length u32 (payload only) · seq u8 (1–15) · cmd u8 · payload; response = length u32 (seq + payload) · seq · payload; notification = seq 0 + notification id. Long address = addr u16 + (bank+1) u8, 0 = plain 64K.

| ID | Command | Request → response |
|---|---|---|
| 1 | INIT | version[3], program name\0 → error, version[3], machine type (0 unknown), string |
| 2 | CLOSE | |
| 3 | GET_REGISTERS | → PC SP AF BC DE HL IX IY AF' BC' DE' HL' (u16), R, I, IM, reserved, slot count, slot banks |
| 4 | SET_REGISTER | reg u8, value u16 |
| 6 | CONTINUE | 2× (enable, addr) temp breakpoints, alt command (0), range start/end |
| 7 | PAUSE | → response then NTF_PAUSE |
| 9 | WRITE_MEM | reserved, addr, data |
| 10 | SET_SLOT | slot, bank → error |
| 15 | LOOPBACK | echo |
| 20 / 21 | READ_PORT / WRITE_PORT | port u16 (, value) |
| 22 | EXEC_ASM | context, code → error, AF BC DE HL |
| 23 | INTERRUPT_ON_OFF | |
| **24** | **GET_SUPPORTED_COMMANDS** | → bitfield ≤32 B (required for 2.2) |
| 25 / 26 | READ_BANK_MEM / WRITE_BANK_MEM | bank, offset, size / data |
| 28 | READ_MEM_BLOCKS | total len u32, N × (addr, size) → data |
| 39 | ENABLE_BREAK_ON_INTERRUPT | |
| **40 / 41** | ADD / REMOVE_BREAKPOINT | addr, bank+1, condition\0 → id u16 (0 = none free) / id |
| 42 / 43 | ADD / REMOVE_WATCHPOINT | addr, bank+1, size u16, access (bit0 read, bit1 write) |
| 50 / 51 | READ_STATE / WRITE_STATE | opaque snapshot blob |

NTF_PAUSE (1): reason (0 step, 1 manual, 2 breakpoint, 3 watch read, 4 watch write, 255 other), addr, bank+1, text. ZX Next only: 11, 13, 14, 16–19, 27 (the "simple mode" that patches RSTs into memory; avoid it and use 40/41).

Found in DeZog's source (locations in [debug-and-remote.md](debug-and-remote.md#what-dezog-accepts)):
- the `dzrp` and `gdb` remotes are new in 3.8.0, not in the 3.7.0 release;
- CMD_INIT's machine type must be 1–4 (ZX models; 0 is refused);
- conditions are checked by DeZog, and a breakpoint NTF_PAUSE must carry the bank byte DeZog used;
- CMD_ENABLE_BREAK_ON_INTERRUPT is never sent by the `dzrp` remote.

freeTVC's DeZog patch (`tools/dezog/`) makes machine type 0 use `dzrp.customMemory` and sends CMD_ENABLE_BREAK_ON_INTERRUPT ([debug-and-remote.md](debug-and-remote.md#tvc-banks)).

## 4. GDB Remote Serial Protocol (Z80) [v]

Sources: the GDB manual's [Remote Serial Protocol](https://sourceware.org/gdb/current/onlinedocs/gdb.html/Remote-Protocol.html) and [Target Descriptions](https://sourceware.org/gdb/current/onlinedocs/gdb.html/Target-Descriptions.html) appendices; `gdb/z80-tdep.c` in [binutils-gdb](https://sourceware.org/git/?p=binutils-gdb.git;a=blob;f=gdb/z80-tdep.c); MAME's stub, [`src/osd/modules/debugger/debuggdbstub.cpp`](https://github.com/mamedev/mame/blob/master/src/osd/modules/debugger/debuggdbstub.cpp).

- GDB ≥ 11 has `z80-tdep.c`; feature `org.gnu.gdb.z80.cpu`; default order af bc de hl sp pc ix iy af' bc' de' hl' ir. MAME uses a different order and makes it work by serving `target.xml` via `qXfer:features:read`, so a stub should **always serve target.xml**.
- MAME gdbstub packets: `? g G p P m M c s z/Z (0–1 bp, 2–4 wp) D k H T !`, `qSupported` (`PacketSize=4000;qXfer:features:read+;qOffsets+`), `qC`, `qfThreadInfo`, `qOffsets`, `qRcmd` (hex-encoded console passthrough; this is how MAME exposes banking to DeZog). 0x03 = interrupt. No `vCont`. Stop reply `T05` + `watch/rwatch/awatch:addr;` + SP/PC. One connection.
- Works with gdb, IDA, Ghidra, DeZog `gdb`/`mame` modes.
- freeTVC's stub serves GDB's register names in MAME's order. DeZog's `gdb` remote reads only one packet per TCP read, so the stub sends one per write ([debug-and-remote.md](debug-and-remote.md#gdb-stub)).

## 5. ZEsarUX ZRCP (telnet, port 10000) [v]

Source: [ZEsarUX](https://github.com/chernandezba/zesarux) (its `help` command lists the ZRCP commands).

Prompts `command> ` / `command@cpu-step> `. About 150 commands; the most useful: `enter-cpu-step`, `cpu-step(-over)`, `run [limit]`, `get-registers`, `set-register`, `read/write-memory`, `hexdump`, `disassemble`, `assemble`, `evaluate`, `find-label`, `set-breakpoint idx cond`, `set-breakpointaction`, `set-membreakpoint`, `cpu-history` (DeZog reverse debugging), `cpu-code-coverage`, `cpu-transaction-log`, `get-stack-backtrace`, `get-tstates(-partial)`, `get-memory-pages`, `get-paging-state`, `get-io-ports`, `snapshot-save/load`, `get/put-snapshot`, `smartload`, `load/save-binary`, `save-screen`, **`get-ocr`** (screen text via ROM-font OCR), `send-keys-string ms text`, `send-keys-event`, `hard-reset-cpu`, `exit-emulator`.

## 6. Fuse debugger [v]

Source: the [Fuse manual](https://fuse-emulator.sourceforge.net/fuse.html), section "Monitor/Debugger".

Breakpoints: exec, `read|write addr`, `port read|write port`, `time T` (T-state in frame), `event area:detail` (`tape:play`, `rzx:end`…), all with `if cond`; `tbreak`, `condition`, `ignore`, `commands id`, `finish`, `out port val`, `set area:var val`, **`exit [expr]`** (CI exit code). Bank-qualified addresses `RAM:5:0x0100`, `ROM:1:0x1234`. Expression variables `z80:<reg>` (incl. MEMPTR, im, iff1), `ula:tstates`, `spectrum:frames`; `[addr]` dereference. `--debugger-command` at startup.

## 7. Emulicious — DAP inside the emulator [v]

Source: [Emulicious](https://emulicious.net/) and its VS Code extension.

A built-in Debug Adapter Protocol server over TCP (default 58870); the VS Code extension just connects (`DebugAdapterServer(port)`). This would be a path to VS Code source debugging without DeZog; freeTVC does not implement DAP.

## 8. Mesen / Mesen2 — UI reference [v, Lua details u]

Source: [Mesen2](https://www.mesen.ca/) documentation.

Debugger with labels, comments, call stack, function list; **event viewer** (scanline × cycle grid with register-access/IRQ dots, previous-frame option); hex editor with fading read/write/exec highlighting, code/data colouring, freeze, access counters incl. uninitialised reads; trace logger; profiler; assembler; video viewers; Lua (`emu.addMemoryCallback`, `emu.addEventCallback`, …) [u]; headless `--testrunner` with `emu.stop(code)`. Expressions: `[addr]` byte, `{addr}` word, `Address Value Cycle Scanline Frame IsRead IsWrite IRQ NMI`, labels. [ares](https://ares-emu.net/) has a built-in GDB server (port 9123).

## 9. Emulators that embed or bridge MCP [v]

| Project | Shape | Lessons |
|---|---|---|
| [barryw/vice-mcp](https://github.com/barryw/vice-mcp) | embedded in a VICE fork, HTTP JSON-RPC `127.0.0.1:6510/mcp`, token option | 61 dotted tools (`vice.execution.run`, `run_until`, `frame.advance`, `memory.search`, `checkpoint.*`, `display.screenshot` base64 PNG, `keyboard.type/matrix`, `symbols.*`, `snapshot.*`); requests marshalled to the emulator thread; `timed_out`/`stopped_early`; no push events |
| [RetroArch](https://github.com/libretro/RetroArch) (built in) | Streamable HTTP `127.0.0.1:55357/mcp` + `--mcp-stdio`; **dual-era** (2026-07-28 + 2025-x) | automatic bearer token; rejects non-local web origins; read-only/destructive hints; `outputSchema`/`structuredContent`; PNG ≤ 4 MiB; waits capped (600 frames / 10 s over HTTP) |
| [Parisoft/mame-mcp](https://github.com/Parisoft/mame-mcp) | stdio MCP via a Node supervisor ↔ MAME Lua over NDJSON | 67 tools incl. an `exec.wait_for_stop` long-poll and a `debug.command` escape hatch; **stdout noise corrupts stdio MCP; never block the emulator loop; reads while running are torn** |
| [nataliapc/mcp-openmsx](https://github.com/nataliapc/mcp-openmsx) (Z80) | external, stdio or HTTP | about 17 tools, each with an `action` enum (keeps the tool count low): `emu_control`, `emu_replay`, `debug_run`, `debug_cpu`, `debug_memory`, `debug_vram`, `debug_breakpoints`, `emu_keyboard` (sendText), `emu_savestates`, `screen_shot`, `screenGetFullText`, `basic_programming`; 60+ documentation resources; raw Tcl off by default |
| [ml6719/zx84](https://github.com/ml6719/zx84) | browser Z80 machines, stdio | generic `Machine` tools plus per-machine extensions |

## 10. MCP specification essentials

Source: [MCP specification 2026-07-28](https://modelcontextprotocol.io/specification/2026-07-28) [v]; earlier revisions, e.g. [2025-11-25](https://modelcontextprotocol.io/specification/2025-11-25).

- Previous revisions still widely deployed: 2025-11-25, 2025-06-18, 2025-03-26, 2024-11-05.
- 2026-07-28 is **stateless**: no `initialize` handshake; each request carries `params._meta` keys `io.modelcontextprotocol/protocolVersion`, `…/clientCapabilities`, `…/clientInfo`; server identity in each result's `_meta` (`…/serverInfo`); mandatory `server/discover`; no `Mcp-Session-Id`, GET stream or SSE resumption; `ping` and `logging/setLevel` removed; `subscriptions/listen` replaces `resources/subscribe`; results carry `resultType` (`complete` | `input_required`), list results `ttlMs`/`cacheScope`; `tools/list` in a deterministic order; server→client requests replaced by the multi-round-trip `InputRequiredResult`; roots/sampling/logging deprecated; tasks moved to an extension. Errors −32020 HeaderMismatch, −32021 MissingRequiredClientCapability, −32022 UnsupportedProtocolVersion, −32601.
- Transports: **stdio** (newline-delimited JSON on stdin/stdout, logs only on stderr) and **Streamable HTTP** (single endpoint, one message per POST, `Accept: application/json, text/event-stream`, reply JSON or request-scoped SSE, notifications → 202, headers `MCP-Protocol-Version` + `Mcp-Method` + `Mcp-Name` must match the body or 400/−32020, closing the stream cancels, `X-Accel-Buffering: no`). The old HTTP+SSE transport should not be implemented.
- **Dual-era** servers serve modern `_meta` requests statelessly and fall back to legacy behaviour when they receive `initialize` (legacy: `initialize` → `{protocolVersion, capabilities, serverInfo}`, `notifications/initialized`, optional `Mcp-Session-Id`; `tools/list`, `tools/call` → `{content:[text|image], structuredContent?, isError?}`, `resources/list|read`, `prompts/list|get`) [u for the legacy shapes].
- Security: validate `Origin` (403), bind 127.0.0.1, authenticate (bearer token), treat file and monitor tools as code execution (opt-in).
- SDKs: official TypeScript, Python, C#, Go, **Rust (`rmcp`)**, Ruby, Java, Swift, PHP, Kotlin; **no official C/C++ SDK** [u]; the community C++ libraries seen target older protocol revisions [u]. In C++ the protocol is hand-written (a few hundred lines with a JSON library and a small HTTP server), as freeTVC does. The [MCP Inspector](https://github.com/modelcontextprotocol/inspector) is a real client to test against; freeTVC's checks use version 2.12.0 ([debug-and-remote.md](debug-and-remote.md#mcp-server)).

## Not researched

bsnes-plus; exact Mesen2 Lua signatures; which revisions `rmcp` supports; VICE's prompt string in its source.
