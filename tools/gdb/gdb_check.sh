#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Drives freetvc-headless's GDB stub with GNU gdb in batch mode on tools/dezog/project/hello.asm.
# usage: [GDB=path] tools/gdb/gdb_check.sh <work dir>   (from the project root; gcc-release build, ROMs in FREETVC_ROM_DIR, default testdata/roms)
set -eu
work=${1:?work dir}
root=$(cd "$(dirname "$0")/../.." && pwd)
gdb=${GDB:-$(command -v z80-unknown-elf-gdb || echo "$root/.tools/gdb-18.1-z80/bin/z80-unknown-elf-gdb")}
port=6593
mkdir -p "$work"
work=$(cd "$work" && pwd)
cp "$root/tools/dezog/project/hello.asm" "$work/"
(cd "$work" && "${SJASMPLUS:-$(command -v sjasmplus || echo "$root/.tools/sjasmplus-1.24.0/bin/sjasmplus")}" --nologo --msg=err hello.asm)

# SPACE at frame 330 leaves the start-up logo; the BASIC prompt is up well before gdb connects
"$root/build/gcc-release/src/freetvc-headless" --preset 64k+-2.2 --rom-dir "${FREETVC_ROM_DIR:-$root/testdata/roms}" \
    --type ' @330' --gdb --gdb-port $port 2> "$work/headless.err" &
emu=$!
trap 'kill $emu 2>/dev/null || true' EXIT
sleep 2

cat > "$work/check.gdb" <<EOF
set pagination off
set confirm off
set architecture z80
target remote 127.0.0.1:$port
info registers
restore $work/hello.bin binary 0x6000
x/6xb 0x6000
set \$pc = 0x6000
break *0x6015
continue
info registers pc sp
stepi
info registers pc
watch *(unsigned char *) 0x602a
delete 1
continue
info registers pc
delete
monitor m EXT:0000 0003
monitor bank
detach
EOF
"$gdb" -q -batch -x "$work/check.gdb" 2>&1 | tee "$work/gdb.log"
