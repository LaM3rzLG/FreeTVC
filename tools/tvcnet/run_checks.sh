#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# TVCnet end to end on the headless runner: PING and ECHO, then a GET from a local HTTP server.
# Usage: tools/tvcnet/run_checks.sh [build dir] [sjasmplus]   (from the project root)
set -euo pipefail
build=${1:-build/gcc-release}
sjasmplus=$(realpath "${2:-.tools/sjasmplus-1.24.0/bin/sjasmplus}")
headless=$(realpath "$build/src/freetvc-headless")
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'kill ${server:-0} 2>/dev/null || true; rm -rf "$work"' EXIT

cd "$work"
"$sjasmplus" --sym=test.sym "$here/tvcnet_test.asm" >/dev/null 2>&1
"$sjasmplus" --sym=get.sym "$here/tvcnet_get.asm" >/dev/null 2>&1
"$headless" --preset 64k+-2.2 --frames 150 --save-snapshot boot.tvcs >/dev/null 2>&1

end=$(sed -n 's/^done: EQU 0x0000//p' test.sym)
printf 'bload "tvcnet_test.bin" 5000\nbreak %s\ng 5000\nm 7000 7003\nm 7040 7047\nquit\n' "$end" > test.mon
out=$("$headless" --snapshot boot.tvcs --tvcnet --moncommands test.mon 2>&1)
grep -q '>C:7000  01 00 01 02' <<<"$out" || { echo "PING: wrong reply"; echo "$out"; exit 1; }
grep -q '>C:7040  05 80 48 45 4C 4C 4F F9' <<<"$out" || { echo "ECHO: wrong reply"; echo "$out"; exit 1; }
echo "PING and ECHO: ok"

mkdir www
python3 -I -c "open('www/hello.txt','w').write(''.join('Line %03d of the TVCnet test body.\n' % i for i in range(20)))"
python3 -I -m http.server 8765 --bind 127.0.0.1 --directory www >/dev/null 2>&1 &
server=$!
sleep 0.5
end=$(sed -n 's/^done: EQU 0x0000//p' get.sym)
printf 'bload "tvcnet_get.bin" 5000\nbreak %s\ng 5000\nm 70f0 70f0\nbsave "got.bin" 7600 78a7\nquit\n' "$end" > get.mon
out=$("$headless" --snapshot boot.tvcs --tvcnet --moncommands get.mon 2>&1)
grep -q '>C:70F0  04' <<<"$out" || { echo "GET: did not end with END"; echo "$out"; exit 1; }
cmp -s got.bin www/hello.txt || { echo "GET: the body differs"; exit 1; }
echo "GET: ok ($(stat -c %s got.bin) bytes)"
