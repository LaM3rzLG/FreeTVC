# SPDX-License-Identifier: GPL-3.0-or-later
# Usage: python3 -I cas_check.py <file.cas>...  -> UPM and program header fields checked against docs/hardware/file-formats.md (CAS)
import sys

for p in sys.argv[1:]:
    d = open(p, 'rb').read()
    n = len(d)
    if n < 0x90:
        print('%s: %d bytes, too short for UPM + program header' % (p, n)); continue
    rec, last, plen = d[2] | d[3] << 8, d[4], d[0x82] | d[0x83] << 8
    print('%s %d type=%02X recs_ok=%s hdr80=%02X t81=%02X len=%d %s autorun=%02X ver=%02X rom_load=%s' % (
        p, n, d[0], rec * 128 + last == n, d[0x80], d[0x81], plen, 'len_ok' if plen == n - 144 else 'LEN!=%d' % (n - 144),
        d[0x84], d[0x8f], 'yes' if d[0] == 0x11 and d[0x80] == 0 and d[0x81] == 1 else 'NO'))
