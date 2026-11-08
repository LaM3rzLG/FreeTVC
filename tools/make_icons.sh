#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Renders data/icon/freetvc.svg into the window icon (data/about/freetvc-icon.png) and the Windows icon
# (data/icon/freetvc.ico, 16-256 px). Needs Google Chrome or Chromium (headless render) and python3 with no packages;
# resizing uses macOS sips. Run from the repository root after changing the SVG; the outputs are committed.
set -eu
chrome=${CHROME:-"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"}
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
"$chrome" --headless=new --disable-gpu --hide-scrollbars --default-background-color=00000000 \
    --window-size=1024,1024 --screenshot="$tmp/1024.png" "file://$PWD/data/icon/freetvc.svg" 2>/dev/null
for n in 16 24 32 48 64 128 256; do
    sips -z $n $n "$tmp/1024.png" --out "$tmp/$n.png" >/dev/null
done
cp "$tmp/256.png" data/about/freetvc-icon.png
python3 -I - "$tmp" data/icon/freetvc.ico <<'EOF'
import struct, sys
src, out = sys.argv[1], sys.argv[2]
sizes = [16, 24, 32, 48, 64, 128, 256]
blobs = [open(f"{src}/{n}.png", "rb").read() for n in sizes]
offset = 6 + 16 * len(sizes)
head = struct.pack("<HHH", 0, 1, len(sizes))
for n, b in zip(sizes, blobs):
    head += struct.pack("<BBBBHHII", n % 256, n % 256, 0, 0, 1, 32, len(b), offset)
    offset += len(b)
open(out, "wb").write(head + b"".join(blobs))
EOF
echo "data/about/freetvc-icon.png, data/icon/freetvc.ico"
