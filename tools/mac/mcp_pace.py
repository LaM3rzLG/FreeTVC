#!/usr/bin/env python3
"""Real-time speed of a running freeTVC GUI over MCP: mcp_pace.py [seconds per step] [steps]."""
import json, os, sys, time, urllib.request

URL = f"http://127.0.0.1:{os.environ.get('FREETVC_MCP_PORT', '6584')}/mcp"
TOKEN = os.environ.get("FREETVC_MCP_TOKEN", "claudeteszt")


def info():
    body = {"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": "machine_info", "arguments": {}}}
    req = urllib.request.Request(URL, method="POST", data=json.dumps(body).encode(),
                                 headers={"Authorization": f"Bearer {TOKEN}", "Content-Type": "application/json"})
    d = json.loads(json.load(urllib.request.urlopen(req))["result"]["content"][0]["text"])
    return time.monotonic(), d["tstates"], d["frame"], d["state"]


secs = float(sys.argv[1]) if len(sys.argv) > 1 else 5
steps = int(sys.argv[2]) if len(sys.argv) > 2 else 12
t0, s0, f0, state = info()
print("allapot:", state)
prev = (t0, s0, f0)
for i in range(steps):
    time.sleep(secs)
    t, s, f, _ = info()
    print(f"{i + 1:2}. szakasz: {(s - prev[1]) / (t - prev[0]) / 1e6:.4f} MHz, {(f - prev[2]) / (t - prev[0]):.3f} kep/s")
    prev = (t, s, f)
print(f"OSSZESEN {t - t0:.1f} s: {(s - s0) / (t - t0) / 1e6:.5f} MHz = {(s - s0) / (t - t0) / 3.125e6 * 100:.3f} %, "
      f"{(f - f0) / (t - t0):.3f} kep/s (elmelet 49.761)")
