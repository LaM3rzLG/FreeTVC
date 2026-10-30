# SPDX-License-Identifier: GPL-3.0-or-later
# Usage: python3 -I z80trace_audit.py <config.json> <scratch outdir> [image...]  -> per image: every overlap with the hiding instruction, and untraced gaps >= 24 bytes
import sys, os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import z80trace

z80trace.write = lambda img, out, icfg: None  # audit only, keep the real trace outputs untouched
imgs = z80trace.run(sys.argv[1], sys.argv[2])
for n in sys.argv[3:] or list(imgs):
    img = imgs[n]
    for new, old, src in img.overlap:
        ln_old = z80trace.decode(img.rd, old)[0]
        print('%-8s overlap new %04x %-6s old %04x [%s] | new bytes [%s] from %s' % (
            n, new, 'inside' if old < new < old + ln_old else 'covers', old, img.data[old - img.org:old - img.org + ln_old].hex(' '),
            img.data[new - img.org:new - img.org + 3].hex(' '), '%04x' % src if src is not None else 'entry'))
    cov, tab = set(img.owner), set()
    for a, l, t in img.data_tables:
        tab.update(range(a, a + l))
    a, end, tot = img.org, img.org + len(img.data), 0
    while a < end:
        if a in cov or a in tab:
            a += 1; continue
        b = a
        while b < end and b not in cov and b not in tab:
            b += 1
        if b - a >= 24:
            blk = img.data[a - img.org:b - img.org]
            pr = sum(32 <= (c & 0x7f) < 127 for c in blk) / len(blk)
            print('%-8s gap %04x-%04x %5d  text%%=%3d  ff%%=%3d  %s' % (n, a, b - 1, b - a, 100 * pr, 100 * blk.count(255) // len(blk), blk[:12].hex(' ')))
            tot += b - a
        a = b
    print('%-8s gaps >= 24 bytes: %d' % (n, tot))
