# tools/mcp: MCP server checks

Two checks of freeTVC's MCP server ([debug-and-remote.md](../../docs/design/debug-and-remote.md), section "MCP server"), both against `freetvc-headless`:

- `inspector_check.sh` drives the server with the official MCP Inspector CLI, a real third-party client;
- `stress_check.py` sends raw HTTP and stdio traffic: protocol abuse, load, limits, determinism and source-level debugging sessions.

Both need a `gcc-release` build (`build/gcc-release/src/freetvc-headless`) and the system ROMs, for example in `testdata/roms/`.

## inspector_check.sh

What it checks:
- `tools/list --strict` over stdio in each of the Inspector's protocol eras: `legacy`, `modern` and `auto`;
- one emulator over HTTP, driven across several separate Inspector calls: `run` 330 frames, `press_keys` SPACE (past the start-up logo), `type_text` `print 6*7` + RETURN in another era, `screen_text` in a third; the screen must show `print 6*7`, ` 42` and `ok`.

### Install the Inspector

The Inspector needs Node.js 22; Node 18 fails with "CustomEvent is not defined". Install version 2.12.0 into a folder of your choice:

```sh
mkdir -p <inspector dir> && cd <inspector dir>
npm init -y && npm install @modelcontextprotocol/inspector@2.12.0
```

Version 2.12.0 brings `@modelcontextprotocol/client` and `@modelcontextprotocol/core` 2.3.1, which speak the 2026-07-28 protocol as well as the older one.

### Run

From the project root:

```sh
FREETVC_ROM_DIR=$PWD/testdata/roms tools/mcp/inspector_check.sh <inspector dir> <work dir>
```

Node 22 must be the first `node` on `PATH`, or set `NODE22_BIN` to its `bin` folder (the script puts it first on `PATH`). The ROMs come from `FREETVC_ROM_DIR` (default `testdata/roms`). HTTP uses port 6599 with the token `check`. The Inspector's catalog and client configuration go into the work folder (`MCP_CATALOG_PATH`, `MCP_CLIENT_CONFIG_PATH`), not into `~/.mcp-inspector`. Replies and stderr of every step are kept in the work folder (`list-<era>.json`, `server.err`, `screen.json`).

Expected output (the tool count grows when tools are added):

```text
== stdio: tools/list --strict in each protocol era
legacy 20 tools
modern 20 tools
auto 20 tools
== HTTP: one emulator, BASIC typed over MCP
TV COMPUTER BASIC 2.2
Copyright 1988 VIDEOTON

42155  bytes free

ok
print 6*7
 42
ok

inspector_check: PASS
```

The script exits with 1 when the screen text is wrong.

## stress_check.py

Python 3 standard library only. It starts its own server (`--preset 64k+-2.2 --mcp-port 6597 --mcp-token stress`, ROMs through `FREETVC_ROM_DIR`), boots it to the BASIC prompt over MCP and runs these groups:

- protocol abuse: broken JSON (-32700), a JSON-RPC batch, odd ids (a string, 2^53, a fraction), a missing `jsonrpc` field, a wrong content type, invalid UTF-8, lowercase header names, a short body, a 20 MB body, pipelined requests;
- load: a stalled request must not block others (worst latency under 1 s), 200 idle connections, 8 parallel clients × 40 mixed calls;
- limits and errors: bad argument types, `run` with a `timeout_ms`, reading all 64 KiB, disassembling across FFFFh, a SYS ROM patch read back and undone, reads and writes past a bank's end, 1000 breakpoints, bad conditions, 100 000 nested parentheses, division by zero, Hungarian letters and an emoji in `type_text`, random and missing symbol files, 100 000 single steps, screenshots as PNGs under 64 KB that inflate correctly;
- determinism: a snapshot replayed twice with the same input gives the same machine;
- calls while the machine runs freely;
- a source-level session with sjasmplus: an SLD loaded with `symbols load`, a breakpoint on a label, `step` with `line` and `line_over`, source lines in disassembly rows and lookups;
- the same for C with SDCC (`sdcc -mz80 --debug`, a CDB file) when `sdcc` is on `PATH`; otherwise that part is reported as skipped;
- stdio: garbage lines, a 1 MB expression and valid requests on `--mcp-stdio`, then a clean exit on end of input.

```sh
python3 -I tools/mcp/stress_check.py build/gcc-release/src/freetvc-headless testdata/roms <sjasmplus> <work dir> [port]
```

`<sjasmplus>` is the path of an sjasmplus 1.24.0 executable. Each check prints `PASS` or `FAIL` with a detail; the last line is the summary, for example `55 checks, 0 failed`, and the exit code is 1 when any check failed. The server's stderr is in `<work dir>/server.err`.

### Against a running GUI

With `STRESS_ATTACH=1` and a port, the script drives a server that is already listening with the token `stress` instead of starting one, and skips the stdio part. For example, the GUI on a virtual display ([tools/ui/README.md](../ui/README.md#running-on-a-virtual-display)):

```sh
DISPLAY=:99 FREETVC_ROM_DIR=$PWD/testdata/roms build/gcc-release/src/freetvc --no-audio --mcp-http --mcp-port 6596 --mcp-token stress &
STRESS_ATTACH=1 python3 -I tools/mcp/stress_check.py build/gcc-release/src/freetvc-headless testdata/roms <sjasmplus> <work dir> 6596
```

Stop such a GUI with `pkill -x freetvc`; `pkill -f` with the command line would also match the calling shell.
