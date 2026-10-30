# SPDX-License-Identifier: GPL-3.0-or-later
# Usage: python3 -I check_rom_syms12.py <tvc_roms12.json> <trace outdir> <symdir>  -> checks the SYS 1.2 / EXT 1.x symbol files against the trace
import sys, os, re
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import z80trace

# symbol file -> trace image; data labels must lie outside traced code, all others must be instruction starts
FILES = {'tvc-sys12.sym': 'sys12', 'tvc-sys12-page0.sym': 'sys12lo', 'tvc-ext1x.sym': 'ext1x'}
DATA = re.compile(r'(table|_src$|^font$|^copyright$|^mops_signature$|^card_name_|^bas_keywords$|^bas_err_msgs$|^kbd_tables$)')

imgs = z80trace.run(sys.argv[1], sys.argv[2])
bad, syms = 0, {}
for fname, iname in FILES.items():
    img, n = imgs[iname], 0
    for line in open(os.path.join(sys.argv[3], fname)):
        m = re.match(r'\s*(\w+)\s*=\s*0x([0-9A-Fa-f]{4})', line)
        if not m:
            continue
        name, a = m.group(1), int(m.group(2), 16)
        syms[(fname, name)] = a
        n += 1
        ok = img.inside(a) and ((a not in img.owner) if DATA.search(name) else (a in img.code))
        if not ok:
            bad += 1
            print('%s: %s = %04X is %s' % (fname, name, a, 'traced code' if a in img.owner else 'not an instruction start'))
    print('%s: %d symbols checked' % (fname, n))

# handler names must match the ROM's own tables and stub operands
sys12, ext1x = imgs['sys12'], imgs['ext1x']
word = lambda img, a: img.rd(a) | img.rd(a + 1) << 8
S, E = 'tvc-sys12.sym', 'tvc-ext1x.sym'
classes = {0: ['vid_irq', 'VID_CHOUT', 'VID_BKOUT', 'BTEXT', 'VMODE', 'CLS', 'BABS', 'BREL', 'BON', 'BOFF', 'FILL', 'DEFC', 'PAL'],
           1: ['kbd_irq', 'KBD_CHIN', 'kbd_bkin', 'KBD_STATUS'], 2: ['ED_INT', 'ED_CH', 'ED_BK', 'CPOS', 'CFIX'],
           3: ['snd_irq', 'snd_nop', 'snd_nop', 'TONE_SET'], 4: ['prt_irq', 'PAR_CHOUT', 'PAR_BKOUT'],
           5: ['cas_irq', 'cas_ch_stub', 'cas_bk_stub', 'cas_open_stub', 'cas_close_stub', 'cas_verify_stub']}
checks = []
for c, names in classes.items():
    t = word(sys12, syms[(S, 'dev_table')] + 2 * c)
    if sys12.rd(t) != len(names):
        bad += 1; print('class %d count at %04X is %d, expected %d' % (c, t, sys12.rd(t), len(names)))
    checks += [((S, nm), word(sys12, t + 1 + 2 * i)) for i, nm in enumerate(names)]
kt = syms[(S, 'kernel_fn_table')]
checks += [((S, nm), word(sys12, kt + 1 + 2 * i)) for i, nm in enumerate(['kernel_fn0', 'HI_MEM_SET', 'EXP_DIRECT', 'IO_DIRECT', 'SLOT_NUM'])]
it = syms[(S, 'dev_init_table')]
checks += [((S, nm), word(sys12, it + 2 * i)) for i, nm in enumerate(['vid_init', 'kbd_init', 'snd_init', 'cas_reset_stub'])]
mv = syms[(S, 'mode_vec_table')]
checks += [((S, 'chr_put_mode%d' % i), word(sys12, mv + 4 * i)) for i in range(3)]
for stub, entry in [('cas_ch_stub', 'cas_ch_entry'), ('cas_bk_stub', 'cas_bk_entry'), ('cas_open_stub', 'cas_open_entry'),
                    ('cas_close_stub', 'cas_close_entry'), ('cas_verify_stub', 'cas_verify_entry'), ('cas_reset_stub', 'cas_reset_entry')]:
    a = syms[(S, stub)]
    if sys12.rd(a) != 0x21:
        bad += 1; print('%s at %04X is not LD HL,nn' % (stub, a))
    checks.append(((E, entry), word(sys12, a + 1)))
for entry, routine in [('cas_ch_entry', 'CAS_CH'), ('cas_bk_entry', 'CAS_BK'), ('cas_open_entry', 'CAS_OPEN'),
                       ('cas_close_entry', 'CAS_CLOSE'), ('cas_verify_entry', 'CAS_VERIFY'), ('cas_reset_entry', 'cas_clear')]:
    a = syms[(E, entry)]
    if ext1x.rd(a) != 0xCD:
        bad += 1; print('%s at %04X is not CALL nn' % (entry, a))
    checks.append(((E, routine), word(ext1x, a + 1)))
st = syms[(E, 'ser_fn_table')]
if ext1x.rd(st) != 5:
    bad += 1; print('ser_fn_table count is %d' % ext1x.rd(st))
checks += [((E, nm), word(ext1x, st + 1 + 2 * i)) for i, nm in enumerate(['ser_irq', 'SER_CH', 'SER_BK', 'RS232_SET', 'RS232_CLOSE'])]
# independent: WinTVC's EXT 1.x tape-trap sites (found in WinTVC 1.4.1)
checks += [((E, nm), a) for nm, a in [('cas_sync_search', 0xF798), ('cas_bit_write', 0xFA3D), ('CAS_OPEN', 0xF3FA),
                                       ('cas_bit_read', 0xF919), ('cas_close_hl0', 0xF58C)]]
for key, want in checks:
    got = syms.get(key)
    if got != want:
        bad += 1
        print('%s: %s = %s, ROM says %04X' % (key[0], key[1], '%04X' % got if got is not None else 'missing', want))
print('table checks: %d, problems: %d' % (len(checks), bad))
sys.exit(1 if bad else 0)
