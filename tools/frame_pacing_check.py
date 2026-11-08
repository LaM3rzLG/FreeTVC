#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks a GUI frame log (FREETVC_FRAME_LOG, docs/design/ui.md "Frame pacing").

Usage: python3 tools/frame_pacing_check.py LOG [SECONDS]
Reads the last SECONDS (default 20) of the log: the host's refresh rate, how long each TVC frame stayed on screen,
frames that never reached the screen and frames the display wanted before the machine had completed them.
Exit code 0 when no frame was dropped or late, 1 otherwise, 2 on a bad log.
"""

import statistics
import sys
from collections import Counter

FRAME_MS = 62800 / 3.125e6 * 1000  # 20.096 ms


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__.strip())
        return 2
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 20.0
    rows = []
    with open(sys.argv[1], encoding="ascii") as f:
        for line in f:
            parts = line.split()
            if len(parts) == 4:
                rows.append(tuple(int(p) for p in parts))
    if len(rows) < 100:
        print(f"too few lines ({len(rows)}): run the machine for at least {seconds:.0f} s")
        return 2
    end_ns = rows[-1][0]
    rows = [r for r in rows if end_ns - r[0] <= seconds * 1e9]

    present_ms = [(b[0] - a[0]) / 1e6 for a, b in zip(rows, rows[1:])]
    refresh = 1000 / statistics.median(present_ms)

    changes = []  # (host ns, shown frame) at each new picture
    for t, shown, _machine, _late in rows:
        if not changes or shown != changes[-1][1]:
            changes.append((t, shown))
    steps = Counter(b[1] - a[1] for a, b in zip(changes, changes[1:]))
    dropped = sum((step - 1) * n for step, n in steps.items() if 1 < step <= 8)
    jumps = sum(n for step, n in steps.items() if step > 8 or step < 1)
    held_ms = [(b[0] - a[0]) / 1e6 for a, b in zip(changes, changes[1:])]
    late = rows[-1][3] - rows[0][3]
    behind = statistics.mean(m - s for _t, s, m, _l in rows)

    print(f"window:         {(rows[-1][0] - rows[0][0]) / 1e9:.1f} s, {len(rows)} pictures presented")
    print(f"host refresh:   {refresh:.1f} Hz (present interval median {statistics.median(present_ms):.2f} ms, "
          f"min {min(present_ms):.2f}, max {max(present_ms):.2f})")
    print(f"TVC frames:     {len(changes)} shown, {1000 / statistics.mean(held_ms):.2f} per second (machine: {1000 / FRAME_MS:.2f})")
    print(f"on screen for:  " + ", ".join(f"{ms} ms x{n}" for ms, n in sorted(Counter(round(x) for x in held_ms).items())))
    print(f"display lag:    {behind:.2f} frames behind the machine on average")
    print(f"dropped frames: {dropped}")
    print(f"late picks:     {late}")
    if jumps:
        print(f"discontinuities: {jumps} (a load, reset, snapshot or pause: not counted)")
    ok = dropped == 0 and late == 0
    print("RESULT:", "OK" if ok else "PROBLEM")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
