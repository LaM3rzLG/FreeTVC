# SPDX-License-Identifier: GPL-3.0-or-later
# Usage: python3 -I check_rom_syms.py <tvc_roms.json> <trace outdir> <symdir>  -> checks docs/hardware/rom-symbols against the ROM trace
import sys, os, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import z80trace

# symbol file -> trace image; data labels must lie outside traced code, all others must be instruction starts
FILES = {'tvc-sys22.sym': 'sys22', 'tvc-sys22-page0.sym': 'sys22lo', 'tvc-ext2x.sym': 'ext2x'}
DATA = re.compile(r'(table|_dev$|^font$|^kbd_tables$|_src$|^bas_keywords$|^bas_err_msgs$|^card_name_|^copyright$|^mops_signature$|^ram_init_tables$)')

imgs = z80trace.run(sys.argv[1], sys.argv[2])
bad = 0
for fname, iname in FILES.items():
    img, n = imgs[iname], 0
    for line in open(os.path.join(sys.argv[3], fname)):
        m = re.match(r'\s*(\w+)\s*=\s*0x([0-9A-Fa-f]{4})', line)
        if not m:
            continue
        name, a = m.group(1), int(m.group(2), 16)
        n += 1
        is_data = bool(DATA.search(name))
        ok = img.inside(a) and ((a not in img.owner) if is_data else (a in img.code))
        if not ok:
            bad += 1
            print('%s: %s = %04X is %s' % (fname, name, a, 'traced code' if a in img.owner else 'not an instruction start'))
    print('%s: %d symbols checked' % (fname, n))

# statement and op handlers must match the ROM's table words; device handlers must match the dispatch tables
sys22, ext2x = imgs['sys22'], imgs['ext2x']
word = lambda img, a: img.rd(a) | img.rd(a + 1) << 8
syms = {}
for fname in FILES:
    for line in open(os.path.join(sys.argv[3], fname)):
        m = re.match(r'\s*(\w+)\s*=\s*0x([0-9A-Fa-f]{4})', line)
        if m:
            syms[(fname, m.group(1))] = int(m.group(2), 16)
checks = [(('tvc-sys22.sym', 'stmt_colon'), word(sys22, 0xC023 + 2 * 2))]
for (f, name), a in syms.items():
    m = re.match(r'bas_op_([0-9a-f]{2})$', name)
    if m:
        checks.append(((f, name), word(sys22, 0xC08B + 2 * int(m.group(1), 16))))
ext_tables = {0xF192: ['vid_irq', 'VID_CHOUT', 'VID_BKOUT', 'BTEXT', 'VMODE', 'CLS', 'BABS', 'BREL', 'BON', 'BOFF', 'FILL', 'DEFC', 'PAL', 'vid_polygon', 'vid_ellipse'],
              0xEB13: ['ED_INT', 'ED_CH', 'ED_BK', 'CPOS', 'CFIX'], 0xFF40: ['snd_irq', 'snd_nop', 'snd_nop', 'TONE_SET'],
              0xE415: ['cas_irq', 'CAS_CH', 'CAS_BK', 'CAS_OPEN', 'CAS_CLOSE', 'CAS_VERIFY']}
for base, names in ext_tables.items():
    checks += [(('tvc-ext2x.sym', nm), word(ext2x, base + 2 * i)) for i, nm in enumerate(names)]
checks += [(('tvc-ext2x.sym', nm), word(ext2x, 0xE2BE + 2 * i)) for i, nm in enumerate(['ser_irq', 'SER_CH', 'SER_BK', 'RS232_SET', 'RS232_CLOSE'])]
sys_tables = {0xC201: ['kernel_fn0', 'HI_MEM_SET', 'EXP_DIRECT', 'IO_DIRECT', 'SLOT_NUM'], 0xCA2F: ['kbd_irq', 'KBD_CHIN', 'kbd_bkin', 'KBD_STATUS', 'kbd_fkey'],
              0xCE68: ['prt_irq', 'PAR_CHOUT', 'PAR_BKOUT']}
for base, names in sys_tables.items():
    if sys22.rd(base) != len(names):
        bad += 1; print('count at %04X is %d, expected %d' % (base, sys22.rd(base), len(names)))
    checks += [(('tvc-sys22.sym', nm), word(sys22, base + 1 + 2 * i)) for i, nm in enumerate(names)]
kw, k, cur, o = [], 0, b'', 0xD73A  # statements: keyword entry k <-> table word k
while len(kw) < 52:
    b = sys22.rd(o); o += 1; cur += bytes([b & 0x7F])
    if b & 0x80:
        kw.append(cur.decode('latin-1')); cur = b''
for k in range(3, 52):
    if kw[k] != '!' and kw[k] not in ('OUTPUT',):
        checks.append((('tvc-sys22.sym', 'stmt_' + kw[k]), word(sys22, 0xC023 + 2 * k)))
for key, want in checks:
    got = syms.get(key)
    if got != want:
        bad += 1
        print('%s: %s = %s, ROM table says %04X' % (key[0], key[1], '%04X' % got if got is not None else 'missing', want))
print('table checks: %d, problems: %d' % (len(checks), bad))
sys.exit(1 if bad else 0)
