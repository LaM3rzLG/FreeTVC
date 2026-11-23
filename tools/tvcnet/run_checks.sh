#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-3.0-or-later
# TVCnet end to end on the headless runner with the TVC programs of the firmware repository (tvc/): PING and ECHO,
# a GET and an OPEN/SEND/NEXT/CLOSE against local servers, and a scripted Hayes session through TERM.
# Usage: tools/tvcnet/run_checks.sh [build dir] [sjasmplus] [firmware checkout]   (from the project root)
set -euo pipefail
build=${1:-build/gcc-release}
sjasmplus=$(realpath "${2:-.tools/sjasmplus-1.24.0/bin/sjasmplus}")
firmware=${3:-$(sed -n 's/^FETCHCONTENT_SOURCE_DIR_TVCNET:PATH=//p' "$build/CMakeCache.txt")}
firmware=$(realpath "${firmware:-$build/_deps/tvcnet-src}")
headless=$(realpath "$build/src/freetvc-headless")
here=$(cd "$(dirname "$0")" && pwd)
work=$(mktemp -d)
trap 'kill ${http:-0} ${echo:-0} 2>/dev/null || true; rm -rf "$work"' EXIT

cd "$work"
for program in ping get tcp hayes; do
    "$sjasmplus" --nologo --sym="$program.sym" "$firmware/tvc/$program.asm" >"$program.asm.log" 2>&1 ||
        { cat "$program.asm.log"; exit 1; }
done
"$headless" --preset 64k+-2.2 --frames 150 --save-snapshot boot.tvcs >/dev/null 2>&1

mkdir www
python3 -I -c "open('www/hello.txt','w').write(''.join('Line %03d of the TVCnet test body.\n' % i for i in range(20)))"
python3 -I -m http.server 8765 --bind 127.0.0.1 --directory www >/dev/null 2>&1 &
http=$!
python3 -I "$here/echo_server.py" 8766 &
echo=$!
sleep 0.5

# Runs a program to its `done` loop and saves 6FF0h-7FFFh (results and log) as <program>.mem
run() {
    local end
    end=$(sed -n 's/^done: EQU 0x0000//p' "$1.sym")
    printf 'bload "%s.bin" 5000\nbreak %s\ng 5000\nbsave "%s.mem" 6ff0 7fff\nquit\n' "$1" "$end" "$1" >"$1.mon"
    "$headless" --snapshot boot.tvcs --tvcnet --moncommands "$1.mon" >"$1.out" 2>&1 || { cat "$1.out"; exit 1; }
}
for program in ping get tcp hayes; do
    run "$program"
done
python3 -I "$here/check_results.py" "$work"
