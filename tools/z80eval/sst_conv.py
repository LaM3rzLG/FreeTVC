# SPDX-License-Identifier: GPL-3.0-or-later
# Z80 core evaluation: flatten one SingleStepTests/z80 JSON file into the line format read by sst_run
import json
import sys

REGS = ['pc', 'sp', 'a', 'b', 'c', 'd', 'e', 'f', 'h', 'l', 'i', 'r', 'ei', 'wz',
        'ix', 'iy', 'af_', 'bc_', 'de_', 'hl_', 'im', 'p', 'q', 'iff1', 'iff2']


def main():
    with open(sys.argv[1], encoding='utf-8') as fp:
        tests = json.load(fp)
    out = sys.stdout
    for t in tests:
        out.write('T %s\n' % t['name'].replace(' ', '_'))
        for tag, ramtag, key in (('I', 'M', 'initial'), ('F', 'N', 'final')):
            s = t[key]
            out.write(tag + ' ' + ' '.join(str(int(s[r])) for r in REGS) + '\n')
            for a, v in s['ram']:
                out.write('%s %d %d\n' % (ramtag, a, v))
        for a, d, fl in t['cycles']:
            out.write('C %d %d %s\n' % (-1 if a is None else a, -1 if d is None else d, fl))
        for a, v, d in t.get('ports', []):
            out.write('P %d %d %s\n' % (a, v, d))
        out.write('E\n')


main()
