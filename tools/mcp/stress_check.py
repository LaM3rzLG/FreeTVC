#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Harder MCP checks against freetvc-headless: protocol abuse, load, limits, determinism, source-level agent sessions.
# usage: python3 -I tools/mcp/stress_check.py <freetvc-headless> <rom dir> <sjasmplus> <work dir> [port]
# With STRESS_ATTACH=1 it drives a server already listening on the port with token "stress" (e.g. the GUI) and skips stdio.
import base64
import hashlib
import http.client
import json
import os
import shutil
import socket
import struct
import subprocess
import sys
import threading
import time
import zlib

HEADLESS, ROMS, SJASMPLUS, WORK = (os.path.abspath(a) for a in sys.argv[1:5])
PORT = int(sys.argv[5]) if len(sys.argv) > 5 else 6597
TOKEN = "stress"
ATTACH = os.environ.get("STRESS_ATTACH") == "1"
os.makedirs(WORK, exist_ok=True)
results = []


def check(name, ok, detail=""):
    results.append((name, bool(ok), detail))
    print(("PASS " if ok else "FAIL ") + name + (": " + detail if detail else ""), flush=True)


def body(method, params=None, rid=1):
    msg = {"jsonrpc": "2.0", "id": rid, "method": method}
    if params is not None:
        msg["params"] = params
    return json.dumps(msg).encode()


def post(raw, headers=None, conn=None, timeout=60):
    own = conn is None
    conn = conn or http.client.HTTPConnection("127.0.0.1", PORT, timeout=timeout)
    h = {"Authorization": "Bearer " + TOKEN, "Content-Type": "application/json", "Accept": "application/json, text/event-stream"}
    h.update(headers or {})
    conn.request("POST", "/mcp", raw, h)
    r = conn.getresponse()
    data = r.read()
    if own:
        conn.close()
    return r.status, data


def call(tool, args=None, conn=None, timeout=60):
    status, data = post(body("tools/call", {"name": tool, "arguments": args or {}}), conn=conn, timeout=timeout)
    reply = json.loads(data)
    if "error" in reply:
        return None, reply["error"]
    r = reply["result"]
    text = r["content"][0].get("text", "") if r.get("content") else ""
    structured = r.get("structuredContent")
    return {"error": r.get("isError", False), "text": text, "data": structured, "content": r.get("content", [])}, None


def alive():
    try:
        r, err = call("machine_info", timeout=10)
        return r is not None and not r["error"]
    except Exception:
        return False


def raw_exchange(payload, wait=5.0, chunks=None):
    s = socket.create_connection(("127.0.0.1", PORT), timeout=wait)
    for part in (chunks or [payload]):
        s.sendall(part)
    data = b""
    try:
        while True:
            got = s.recv(65536)
            if not got:
                break
            data += got
            if b"\r\n\r\n" in data:
                head, _, rest = data.partition(b"\r\n\r\n")
                length = [l for l in head.split(b"\r\n") if l.lower().startswith(b"content-length:")]
                if length and len(rest) >= int(length[0].split(b":")[1]):
                    break
    except socket.timeout:
        pass
    s.close()
    return data


def http_request(raw_body, extra=b""):
    return (b"POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " + TOKEN.encode() +
            b"\r\nContent-Type: application/json\r\n" + extra + b"Content-Length: " + str(len(raw_body)).encode() + b"\r\n\r\n" + raw_body)


# --- start the server ---
env = dict(os.environ, FREETVC_ROM_DIR=ROMS)
server_err = open(os.path.join(WORK, "server.err"), "w")
server = None if ATTACH else subprocess.Popen([HEADLESS, "--preset", "64k+-2.2", "--mcp-port", str(PORT), "--mcp-token", TOKEN],
                                              stdout=subprocess.DEVNULL, stderr=server_err, env=env)
for _ in range(50):
    if alive():
        break
    time.sleep(0.1)
check("server answers", alive())

try:
    # Boot to the BASIC prompt
    call("run", {"frames": 330})
    r, _ = call("press_keys", {"keys": ["SPACE"], "settle_frames": 100})
    check("boot to the prompt", r and "ok" in (r["data"] or {}).get("screen", []), str((r["data"] or {}).get("screen")))

    # --- protocol abuse ---
    reply = raw_exchange(http_request(b'{"jsonrpc":"2.0","id":1,"method":'))
    check("broken JSON: an error reply", b"-32700" in reply, reply[:120].decode(errors="replace"))
    reply = raw_exchange(http_request(b'[' + body("tools/list") + b',' + body("tools/list", rid=2) + b']'))
    check("JSON-RPC batch: an error, not a crash", b'"error"' in reply and alive(), reply.split(b"\r\n\r\n")[-1][:160].decode(errors="replace"))
    for rid in ("abc", 2**53):
        status, data = post(json.dumps({"jsonrpc": "2.0", "id": rid, "method": "tools/list"}).encode())
        echoed = json.loads(data).get("id") if data else None
        check("id %r echoed" % (rid,), status == 200 and echoed == rid, "status %d id %r" % (status, echoed))
    # JSON-RPC: ids SHOULD NOT have fractions; echoing it or refusing the request are both fine
    status, data = post(json.dumps({"jsonrpc": "2.0", "id": 1.5, "method": "tools/list"}).encode())
    reply = json.loads(data) if data else {}
    check("fractional id: echoed or refused cleanly", reply.get("id") == 1.5 or reply.get("error", {}).get("code") == -32600,
          "status %d %s" % (status, data[:80]))
    status, data = post(json.dumps({"id": 5, "method": "tools/list"}).encode())
    check("missing jsonrpc field: an error", b"error" in data and alive(), "status %d %s" % (status, data[:100]))
    status, data = post(body("tools/list"), headers={"Content-Type": "text/plain"})
    check("wrong content type: refused or served, server fine", status in (200, 400, 415) and alive(), "status %d" % status)
    status, data = post(b'{"jsonrpc":"2.0","id":7,"method":"tools/call","params":{"name":"evaluate","arguments":{"expression":"\xff\xfe"}}}')
    check("invalid UTF-8 in a string: an error, server fine", alive() and (status != 200 or b"error" in data.lower()), "status %d %s" % (status, data[:120]))
    lower = body("tools/list", rid=31)
    reply = raw_exchange(b"POST /mcp HTTP/1.1\r\nhost: 127.0.0.1\r\nauthorization: Bearer " + TOKEN.encode() +
                         b"\r\ncontent-type: application/json\r\ncontent-length: " + str(len(lower)).encode() + b"\r\n\r\n" + lower)
    check("lowercase header names", reply.startswith(b"HTTP/1.1 200") and b'"id":31' in reply, reply[:40].decode(errors="replace"))
    # A body shorter than its Content-Length, then the client gives up
    t0 = time.time()
    raw_exchange(http_request(b'{"jsonrpc":"2.0"')[:-5].replace(b"Content-Length: 16", b"Content-Length: 500"), wait=1.0)
    check("short body: server still answers", alive(), "%.1f s" % (time.time() - t0))
    # 20 MB
    big = b'{"jsonrpc":"2.0","id":9,"method":"tools/call","params":{"name":"machine_info","arguments":{},"pad":"' + b"x" * (20 << 20) + b'"}}'
    t0 = time.time()
    try:
        status, data = post(big, timeout=60)
        outcome = "status %d" % status
    except Exception as e:  # a server may cut the connection on an oversized body
        outcome = "connection: %s" % type(e).__name__
    check("20 MB body: answered or refused, server fine", alive(), "%s in %.1f s" % (outcome, time.time() - t0))
    # Two requests in one write: two replies in order
    two = http_request(body("tools/list", rid=21)) + http_request(body("tools/list", rid=22))
    s = socket.create_connection(("127.0.0.1", PORT), timeout=10)
    s.sendall(two)
    data = b""
    deadline = time.time() + 10
    while data.count(b'"id":2') < 2 and time.time() < deadline:
        got = s.recv(1 << 20)
        if not got:
            break
        data += got
    s.close()
    check("pipelined requests: both answered, in order", data.find(b'"id":21') != -1 and data.find(b'"id":21') < data.find(b'"id":22'),
          "%d bytes" % len(data))

    # --- load ---
    stall = socket.create_connection(("127.0.0.1", PORT), timeout=10)
    stall.sendall(b"POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\n")
    latencies = []
    for _ in range(5):
        t0 = time.time()
        ok = alive()
        latencies.append(time.time() - t0)
        stall.sendall(b"X-Slow: 1\r\n")
    stall.close()
    check("a stalled request does not block others", ok and max(latencies) < 1.0, "worst %.0f ms" % (max(latencies) * 1000))
    idle = []
    try:
        for _ in range(200):
            idle.append(socket.create_connection(("127.0.0.1", PORT), timeout=5))
        t0 = time.time()
        ok = alive()
        check("200 idle connections: a call still works", ok, "%.0f ms" % ((time.time() - t0) * 1000))
    finally:
        for c in idle:
            c.close()
    errors = []
    counts = [0] * 8

    def worker(n):
        conn = http.client.HTTPConnection("127.0.0.1", PORT, timeout=60)
        plan = [("registers", {}), ("memory", {"action": "read", "address": "0C000", "length": 256}),
                ("disassemble", {"address": "0CAA3", "count": 32}), ("machine_info", {}), ("run", {"frames": 1})]
        for i in range(40):
            tool, args = plan[(i + n) % len(plan)]
            try:
                r, err = call(tool, args, conn=conn)
                if err or r["error"]:
                    errors.append("%s: %s" % (tool, err or r["text"][:80]))
                counts[n] += 1
            except Exception as e:
                errors.append("%s: %r" % (tool, e))
                conn = http.client.HTTPConnection("127.0.0.1", PORT, timeout=60)
        conn.close()

    t0 = time.time()
    threads = [threading.Thread(target=worker, args=(n,)) for n in range(8)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    check("8 parallel clients x 40 mixed calls", not errors and sum(counts) == 320 and alive(),
          "%d calls in %.1f s; %s" % (sum(counts), time.time() - t0, errors[:3]))

    # --- limits and bad arguments ---
    r, err = call("run", {"frames": "10"})
    check("frames as a string: a tool error", err or (r and r["error"]), (err or r)["message" if err else "text"][:100] if (err or r) else "")
    r, err = call("run", {"frames": -1})
    check("frames -1: a tool error", err or (r and r["error"]), "")
    t0 = time.time()
    r, _ = call("run", {"frames": 30000, "timeout_ms": 200})
    took = time.time() - t0
    check("run 30000 frames with timeout_ms 200 stops on time", r and (r["data"] or {}).get("stopped_by") == "timeout" and took < 1.5,
          "stopped_by %s after %.2f s" % ((r["data"] or {}).get("stopped_by") if r else None, took))
    r, _ = call("memory", {"action": "read", "address": 0, "length": 65536, "format": "base64"})
    raw = base64.b64decode((r["data"] or {}).get("data", "")) if r else b""
    check("read all 64 KiB as base64", len(raw) == 65536, "%d bytes" % len(raw))
    r, _ = call("disassemble", {"address": "0FFF0", "count": 512})
    rows = (r["data"] or {}).get("lines", []) if r else []
    wraps = any(row["address"] < "0100" for row in rows)
    check("disassemble 512 lines across FFFFh", len(rows) == 512 and wraps, "%d rows, wraps %s" % (len(rows), wraps))
    # The monitor and MCP patch ROM images on purpose (Bench's panel does not); the patch must read back
    before, _ = call("memory", {"action": "read", "address": "SYS:0000", "length": 1})
    call("memory", {"action": "write", "address": "SYS:0000", "data": "5A"})
    after, _ = call("memory", {"action": "read", "address": "SYS:0000", "length": 1})
    call("memory", {"action": "write", "address": "SYS:0000", "data": before["data"]["data"]})
    check("a SYS ROM patch reads back (and is undone)", after and after["data"]["data"] == "5A", "%s -> %s" % (before["data"]["data"], after["data"]["data"] if after else None))
    r, _ = call("memory", {"action": "write", "address": "U3:3FFF", "data": "12 34"})
    check("a write past a bank's end: a tool error", r and r["error"] and "runs past its end" in r["text"], r["text"][:120] if r else "")
    r, _ = call("memory", {"action": "read", "address": "U3:7FFF", "length": 4})
    check("read past a bank: a tool error", r and r["error"], r["text"][:100] if r else "")
    t0 = time.time()
    added = 0
    for i in range(1000):
        r, _ = call("breakpoints", {"action": "add", "address": 0x9000 + i, "kind": "store"})
        added += 1 if r and not r["error"] else 0
    r, _ = call("breakpoints", {"action": "list"})
    listed = len((r["data"] or {}).get("checkpoints", [])) if r else -1
    call("breakpoints", {"action": "clear"})
    check("1000 breakpoints added, listed, cleared", added == 1000 and listed == 1000, "%d added, %d listed, %.1f s" % (added, listed, time.time() - t0))
    r, _ = call("run", {"frames": 2})
    check("the machine runs after the clear", r and r["data"].get("stopped_by") == "budget", "")
    r, _ = call("breakpoints", {"action": "add", "address": "8000", "condition": "A == == 3"})
    check("a bad condition: a tool error", r and r["error"], r["text"][:100] if r else "")
    deep = "(" * 100000 + "1" + ")" * 100000
    r, err = call("evaluate", {"expression": deep})
    check("100 000 nested parentheses: an answer, not a crash", alive(), (err and err.get("message")) or (r and r["text"][:80]) or "")
    r, err = call("breakpoints", {"action": "add", "address": "8000", "condition": deep})
    check("the same as a condition: server fine", alive(), "")
    call("breakpoints", {"action": "clear"})
    r, _ = call("evaluate", {"expression": "1/0"})
    check("1/0: an answer, server fine", alive(), r["text"][:80] if r else "")
    r, _ = call("type_text", {"text": "\n10 REM árvíztűrő tükörfúrógép\n", "settle_frames": 30})
    screen = (r["data"] or {}).get("screen", []) if r else []
    check("Hungarian letters typed through", any("árvíztűrő" in row for row in screen), str(screen[-3:]))
    r, _ = call("type_text", {"text": "a\U0001F600b"})
    check("an emoji: a tool error naming it", r and r["error"], r["text"][:100] if r else "")
    garbage = os.path.join(WORK, "garbage.bin")
    with open(garbage, "wb") as f:
        f.write(os.urandom(100000))
    r, _ = call("symbols", {"action": "load", "path": garbage})
    check("random bytes as a symbol file: an answer, server fine", r is not None and alive(), r["text"][:100] if r else "")
    r, _ = call("symbols", {"action": "load", "path": os.path.join(WORK, "missing.sld")})
    check("a missing symbol file: a tool error", r and r["error"], r["text"][:100] if r else "")
    t0 = time.time()
    r, _ = call("step", {"mode": "into", "count": 100000})
    check("step into 100 000 instructions", r and not r["error"], "%.2f s" % (time.time() - t0))
    for area, size in (("tv", (650, 287)), ("full", (800, 314))):
        r, _ = call("screenshot", {"area": area})
        png = next((base64.b64decode(c["data"]) for c in (r or {}).get("content", []) if c.get("type") == "image"), b"")
        w, h = struct.unpack(">II", png[16:24]) if png[:8] == b"\x89PNG\r\n\x1a\n" else (0, 0)
        idat, at = b"", 8
        while at + 8 <= len(png):
            length, kind = struct.unpack(">I4s", png[at:at + 8])
            idat += png[at + 8:at + 8 + length] if kind == b"IDAT" else b""
            at += 12 + length
        try:
            rows = len(zlib.decompress(idat)) // (w + 1) if w else 0
        except zlib.error:
            rows = -1
        check("screenshot %s is a %dx%d PNG that inflates, under 64 KB" % (area, *size), (w, h) == size and rows == h and len(png) < 65536,
              "%dx%d, %d rows, %d bytes" % (w, h, rows, len(png)))

    # --- determinism through in-memory snapshots ---
    def fingerprint():
        r, _ = call("memory", {"action": "read", "address": 0, "length": 65536, "format": "base64"})
        g, _ = call("registers")
        return hashlib.sha256(base64.b64decode(r["data"]["data"]) + json.dumps(g["data"], sort_keys=True).encode()).hexdigest()[:16]

    call("snapshot", {"action": "save", "name": "s"})
    call("type_text", {"text": "\nPRINT SIN(2)*3.5\n", "settle_frames": 20})
    call("run", {"frames": 100})
    first = fingerprint()
    call("snapshot", {"action": "load", "name": "s"})
    call("type_text", {"text": "\nPRINT SIN(2)*3.5\n", "settle_frames": 20})
    call("run", {"frames": 100})
    second = fingerprint()
    check("snapshot, the same input twice: the same machine", first == second, "%s %s" % (first, second))
    call("control", {"action": "resume"})
    seen = []
    for _ in range(30):
        r, _ = call("machine_info")
        seen.append((r["data"] or {}).get("frame", 0) if r else -1)
    call("control", {"action": "pause"})
    check("calls while the machine runs freely", all(f >= 0 for f in seen) and seen == sorted(seen) and alive(), "frames %s..%s" % (seen[0], seen[-1]))

    # --- an agent session with source lines: sjasmplus ---
    src = os.path.join(WORK, "prog.asm")
    with open(src, "w") as f:
        f.write("    DEVICE NOSLOT64K\n    ORG 8000h\nstart:\n    ld hl,count\n    ld (hl),0\nloop:\n    call bump\n"
                "    ld a,(count)\n    cp 10\n    jr nz,loop\ndone:\n    jr done\nbump:\n    inc (hl)\n    ret\ncount:\n    db 0\n"
                "    SAVEBIN \"prog.bin\", start, $-start\n")
    subprocess.run([SJASMPLUS, "--nologo", "--msg=err", "--sld=prog.sld", "prog.asm"], cwd=WORK, check=True)
    r, _ = call("symbols", {"action": "load", "path": os.path.join(WORK, "prog.sld")})
    d = r["data"] if r else {}
    check("symbols load: an SLD with labels and lines", d.get("format") == "sjasmplus SLD" and d.get("source_lines", 0) >= 9 and d.get("loaded") == 5, json.dumps(d))
    call("load_binary", {"path": os.path.join(WORK, "prog.bin"), "address": "8000", "jump": True})
    call("breakpoints", {"action": "add", "address": "bump"})
    r, _ = call("run", {"frames": 5})
    nxt = (r["data"] or {}).get("next_instruction", {}) if r else {}
    check("a breakpoint on a label, the stop has its source line", nxt.get("label") == "bump" and "prog.asm:14" in nxt.get("source", ""), json.dumps(nxt))
    call("breakpoints", {"action": "clear"})
    r, _ = call("step", {"mode": "line"})
    nxt = (r["data"] or {}).get("next_instruction", {}) if r else {}
    check("step line: the next source line", "prog.asm:15" in nxt.get("source", ""), nxt.get("source", ""))
    r, _ = call("step", {"mode": "line", "count": 2})
    nxt = (r["data"] or {}).get("next_instruction", {}) if r else {}
    check("step line twice: back in the caller", "prog.asm:9" in nxt.get("source", ""), nxt.get("source", ""))
    for _ in range(3):
        r, _ = call("step", {"mode": "line"})
        if "prog.asm:7" in ((r["data"] or {}).get("next_instruction", {}).get("source", "")):
            break
    r, _ = call("step", {"mode": "line_over"})
    nxt = (r["data"] or {}).get("next_instruction", {}) if r else {}
    check("step line_over a call", "prog.asm:8" in nxt.get("source", ""), nxt.get("source", ""))
    r, _ = call("disassemble", {"address": "start", "count": 6})
    rows = (r["data"] or {}).get("lines", []) if r else []
    check("disassembly rows carry source lines", len(rows) == 6 and all("source" in row for row in rows), str([row.get("source") for row in rows]))
    r, _ = call("evaluate", {"expression": "[count]"})
    check("a variable by its label", r and not r["error"], r["text"] if r else "")
    r, _ = call("symbols", {"action": "lookup", "address": "8005"})
    check("lookup by address gives the source line", "prog.asm:7" in ((r["data"] or {}).get("source_line", "") if r else ""), json.dumps(r["data"] if r else {}))

    # --- the same with SDCC, if it is installed ---
    if shutil.which("sdcc"):
        with open(os.path.join(WORK, "main.c"), "w") as f:
            f.write("static unsigned char counter;\nunsigned char twice(unsigned char x) {\n    return x + x;\n}\n"
                    "void main(void) {\n    for (;;) {\n        counter = twice(counter);\n        counter++;\n    }\n}\n")
        subprocess.run(["sdcc", "-mz80", "--debug", "--code-loc", "0x8000", "--data-loc", "0x9000", "--no-std-crt0", "main.c"],
                       cwd=WORK, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        mem = {}
        for line in open(os.path.join(WORK, "main.ihx")):
            if line.startswith(":") and int(line[7:9], 16) == 0:
                n, a = int(line[1:3], 16), int(line[3:7], 16)
                for i in range(n):
                    mem[a + i] = int(line[9 + 2 * i:11 + 2 * i], 16)
        code = bytes(mem.get(a, 0) for a in range(0x8000, max(k for k in mem if k < 0x9000) + 1))
        with open(os.path.join(WORK, "main.bin"), "wb") as f:
            f.write(code)
        r, _ = call("symbols", {"action": "load", "path": os.path.join(WORK, "main.cdb")})
        check("symbols load: SDCC CDB", r and r["data"].get("format") == "SDCC CDB", json.dumps(r["data"] if r else {}))
        call("load_binary", {"path": os.path.join(WORK, "main.bin"), "address": "8000"})
        r, _ = call("symbols", {"action": "lookup", "address": "8000"})
        check("C: the new program's label wins over the old one's", r and (r["data"] or {}).get("name") == "twice", json.dumps(r["data"] if r else {}))
        call("registers", {"set": {"pc": "main"}})
        r, _ = call("step", {"mode": "line"})
        src_line = ((r["data"] or {}).get("next_instruction", {}).get("source", "")) if r else ""
        check("C: step line into twice()", "main.c:3" in src_line, src_line)
        for _ in range(3):
            r, _ = call("step", {"mode": "line"})
        r, _ = call("step", {"mode": "line_over", "count": 3})
        src_line = ((r["data"] or {}).get("next_instruction", {}).get("source", "")) if r else ""
        check("C: line_over three times", src_line.startswith("main.c:"), src_line)
        r, _ = call("evaluate", {"expression": "[counter]"})
        check("C: a static by its name", r and not r["error"], r["text"] if r else "")
    else:
        check("SDCC session skipped (no sdcc)", True)

    check("server still alive at the end", alive())
except Exception as e:
    check("no exception in the client (the server may have died)", False, repr(e)[:200])
finally:
    if server:
        server.terminate()
        try:
            server.wait(5)
        except subprocess.TimeoutExpired:
            server.kill()
    server_err.close()

# --- stdio: garbage, a long line, then EOF ---
lines = [] if ATTACH else [b"not json at all", b"\xff\xfe\x00garbage",
         body("initialize", {"protocolVersion": "2025-11-25", "capabilities": {}, "clientInfo": {"name": "s", "version": "1"}}, 1),
         b'{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"evaluate","arguments":{"expression":"' + b"1+" * 500000 + b'1"}}}',
         body("tools/call", {"name": "machine_info", "arguments": {}}, 3)]
t0 = time.time()
p = None if ATTACH else subprocess.run([HEADLESS, "--preset", "64k+-2.2", "--mcp-stdio"], input=b"\n".join(lines) + b"\n", capture_output=True, env=env, timeout=60)
if p:
    replies = [json.loads(l) for l in p.stdout.splitlines() if l.strip()]
    ids = [r.get("id") for r in replies]
    check("stdio: garbage answered with errors, valid requests answered, clean exit on EOF",
          p.returncode == 0 and 1 in ids and 2 in ids and 3 in ids, "exit %d, ids %s, %.1f s" % (p.returncode, ids, time.time() - t0))

failed = [n for n, ok, _ in results if not ok]
print("\n%d checks, %d failed%s" % (len(results), len(failed), (": " + "; ".join(failed)) if failed else ""))
sys.exit(1 if failed else 0)
