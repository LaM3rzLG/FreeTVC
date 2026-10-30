# SPDX-License-Identifier: GPL-3.0-or-later
# Usage: python3 -I find_roms.py <container-binary> <rom> [<rom> ...]  -> file offsets where each ROM (or its 8K halves) is embedded
import sys

blob = open(sys.argv[1], 'rb').read()

def find_all(needle):
    r, i = [], blob.find(needle)
    while i >= 0 and len(r) < 8:
        r.append(i); i = blob.find(needle, i + 1)
    return r

for path in sys.argv[2:]:
    rom = open(path, 'rb').read()
    parts = [(0, len(rom))] + [(o, min(o + 0x2000, len(rom))) for o in range(0, len(rom), 0x2000)] if len(rom) > 0x2000 else [(0, len(rom))]
    for a, b in parts:
        chunk = rom[a:b]
        if len(set(chunk)) == 1:
            print(f'{path} [{a:#06x}:{b:#06x}] constant fill {chunk[0]:#04x}, skipped'); continue
        hits = [h for h in find_all(chunk[:256]) if blob[h:h + len(chunk)] == chunk]
        print(f'{path} [{a:#06x}:{b:#06x}] full matches at {[hex(h) for h in hits]}')
