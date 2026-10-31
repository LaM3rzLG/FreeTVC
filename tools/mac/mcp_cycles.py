#!/usr/bin/env python3
"""T-state checks on a running freeTVC over MCP: RAM vs VRAM access, code in VRAM, frame length.
Overwrites 6000h and VID0:0100, then power-cycles the machine."""
import json, os, tempfile, urllib.request

URL = f"http://127.0.0.1:{os.environ.get('FREETVC_MCP_PORT', '6584')}/mcp"
TOKEN = os.environ.get("FREETVC_MCP_TOKEN", "claudeteszt")
TMP = tempfile.mkdtemp(prefix="freetvc_cyc_")


def call(name, args=None):
    body = {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": name, "arguments": args or {}}}
    req = urllib.request.Request(URL, method="POST", data=json.dumps(body).encode(),
                                 headers={"Authorization": f"Bearer {TOKEN}", "Content-Type": "application/json"})
    text = json.load(urllib.request.urlopen(req))["result"]["content"][0]["text"]
    try:
        return json.loads(text)
    except ValueError:
        return text


def load(code, address, jump=False):
    path = os.path.join(TMP, "t.bin")
    with open(path, "wb") as f:
        f.write(bytes(code))
    call("load_binary", {"path": path, "address": address, "jump": jump})


def loop_test(name, hl, op, expect):
    # DI; OUT (02h),D0h (VID at 8000h); LD HL,hl; LD B,0; loop: op; DJNZ loop; JR $
    load([0xF3, 0x3E, 0xD0, 0xD3, 0x02, 0x21, hl & 0xFF, hl >> 8, 0x06, 0x00, op, 0x10, 0xFD, 0x18, 0xFE], "6000", True)
    t = call("run", {"until": "600D"})["tstates_run"]
    print(f"{name:22} mert {t:5} T  vart {expect:5}  elteres {t - expect:+3d}  ciklus {(t - 39) / 256:.3f} T")


call("control", {"action": "pause"})
loop_test("RAM olvasas 7000h", 0x7000, 0x7E, 5154)
loop_test("RAM iras 7000h", 0x7000, 0x77, 5154)
loop_test("VRAM olvasas 8000h", 0x8000, 0x7E, 5154 + 512)  # steady state +2 T per access (tvc-hardware.md 2.)
loop_test("VRAM iras 8000h", 0x8000, 0x77, 5154 + 512)
load([0x00, 0x10, 0xFD, 0x18, 0xFE], "VID0:0100")  # NOP; DJNZ; JR $ in VRAM
load([0xF3, 0x3E, 0xD0, 0xD3, 0x02, 0x06, 0x00, 0xC3, 0x00, 0x81], "6000", True)
t = call("run", {"until": "8103"})["tstates_run"]
print(f"{'kod VRAM-bol':22} mert {t:5} T  ciklus {(t - 39) / 256:.3f} T (RAM-ban 17, elmelet VRAM-ban 20)")
call("control", {"action": "power_cycle"})
call("run", {"frames": 300})
call("breakpoints", {"action": "clear"})
call("breakpoints", {"action": "add", "kind": "irq"})
ts = []
for _ in range(8):
    call("run", {"frames": 2})
    ts.append(call("machine_info")["tstates"])
d = [b - a for a, b in zip(ts, ts[1:])]
print("ket video-IT kozti utemek:", d, f"atlag {sum(d) / len(d):.1f} (elmelet 62800)")
call("breakpoints", {"action": "clear"})
call("control", {"action": "power_cycle"})
call("control", {"action": "resume"})
