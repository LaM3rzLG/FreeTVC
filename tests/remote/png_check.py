#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Decodes png_test's PNGs with Python's zlib (an independent inflater): chunks, CRCs, header, palette, every pixel.
# usage: python3 -I png_check.py <png_test> <work dir>
import os
import struct
import subprocess
import sys
import zlib

program, work = sys.argv[1], sys.argv[2]
subprocess.run([program, work], check=True)
palette = open(os.path.join(work, "palette.bin"), "rb").read()
failures = 0
for name in ("colours", "noise", "blank", "text", "corner", "pixel", "clamped"):
    png = open(os.path.join(work, name + ".png"), "rb").read()
    want = open(os.path.join(work, name + ".idx"), "rb").read()
    width, height = map(int, open(os.path.join(work, name + ".txt")).read().split())
    problems = []
    if png[:8] != b"\x89PNG\r\n\x1a\n":
        problems.append("signature")
    chunks, at = [], 8
    while at < len(png):
        length, kind = struct.unpack(">I4s", png[at:at + 8])
        data = png[at + 8:at + 8 + length]
        crc = struct.unpack(">I", png[at + 8 + length:at + 12 + length])[0]
        if zlib.crc32(kind + data) != crc:
            problems.append("CRC of " + kind.decode())
        chunks.append((kind, data))
        at += 12 + length
    kinds = [k for k, _ in chunks]
    if kinds != [b"IHDR", b"PLTE", b"IDAT", b"IEND"]:
        problems.append("chunks %s" % kinds)
    ihdr = dict(chunks).get(b"IHDR", b"")
    if ihdr != struct.pack(">IIBBBBB", width, height, 8, 3, 0, 0, 0):
        problems.append("IHDR %r" % ihdr)
    if dict(chunks).get(b"PLTE") != palette:
        problems.append("palette")
    try:
        raw = zlib.decompress(b"".join(d for k, d in chunks if k == b"IDAT"))  # checks the Adler-32 too
    except zlib.error as e:
        raw = b""
        problems.append("zlib: %s" % e)
    rows = [raw[y * (width + 1):(y + 1) * (width + 1)] for y in range(height)]
    if len(raw) != height * (width + 1) or any(r[0] != 0 for r in rows):
        problems.append("scanlines")
    elif b"".join(r[1:] for r in rows) != want:
        problems.append("pixels differ")
    stored = height * (width * 3 + 1) + 6 + 5 * (height * (width * 3 + 1) // 65535 + 1) + 57
    print("%s %s: %dx%d, %d bytes (RGB stored blocks: %d)%s" % ("FAIL" if problems else "PASS", name, width, height, len(png), stored,
                                                              (": " + ", ".join(problems)) if problems else ""))
    failures += 1 if problems else 0
sys.exit(1 if failures else 0)
