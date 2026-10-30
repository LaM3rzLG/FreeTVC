# SPDX-License-Identifier: GPL-3.0-or-later
# Usage: python3 -I z80trace.py <config.json> <outdir>  -> recursive Z80 code trace; writes z80dasm block/symbol files and a report per image
import sys, json, os, collections

HLMEM = {0x34, 0x35, 0x36, 0x46, 0x4E, 0x56, 0x5E, 0x66, 0x6E, 0x7E, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x77,
         0x86, 0x8E, 0x96, 0x9E, 0xA6, 0xAE, 0xB6, 0xBE}
ED_RET = {0x45, 0x4D, 0x55, 0x5D, 0x65, 0x6D, 0x75, 0x7D}
A_KEEP = {0x00, 0x02, 0x12, 0x32, 0x77, 0xD3, 0xF3, 0xF5, 0xFB}  # opcodes that leave A unchanged (for paging-value tracking)
HL_KEEP = ({0x00, 0x01, 0x06, 0x08, 0x0E, 0x11, 0x16, 0x1E, 0x32, 0x3A, 0x3C, 0x3D, 0x3E, 0xAF, 0xB7, 0xD3, 0xFE,
            0xC1, 0xC5, 0xD1, 0xD5, 0xF1, 0xF3, 0xF5, 0xFB, 0x18, 0x20, 0x28, 0x30, 0x38, 0xC3}
           | set(range(0x40, 0x60)) | set(range(0x70, 0x76)) | set(range(0x77, 0x80)) | {0xC2 + 8 * i for i in range(8)})  # leave HL unchanged
WORD_OPS = {0x01: 'ld bc', 0x11: 'ld de', 0x21: 'ld hl', 0x31: 'ld sp', 0x22: 'st hl', 0x2A: 'ld hl()', 0x32: 'st a', 0x3A: 'ld a()'}


def base_len(op):
    if op in (0x01, 0x11, 0x21, 0x31, 0x22, 0x2A, 0x32, 0x3A, 0xC3, 0xCD) or op & 0xC7 in (0xC2, 0xC4):
        return 3
    if op & 0xC7 in (0x06, 0xC6) or op in (0x10, 0x18, 0x20, 0x28, 0x30, 0x38, 0xD3, 0xDB, 0xCB):
        return 2
    return 1


def decode(rd, pc):
    """-> (length, kind, target, extra); kind: seq jump cjump call ccall ret cret rst ijump; extra: ('word', op, nn) | ('out', n) | None"""
    op = rd(pc)
    if op == 0xED:
        b = rd(pc + 1)
        if 0x40 <= b < 0x80 and b & 0xC7 == 0x43:
            nn = rd(pc + 2) | rd(pc + 3) << 8
            return 4, 'seq', None, ('word', 'ed%02x' % b, nn)
        return 2, ('ret' if b in ED_RET else 'seq'), None, None
    if op in (0xDD, 0xFD):
        b = rd(pc + 1)
        if b in (0xDD, 0xFD, 0xED):
            return 1, 'seq', None, None
        if b == 0xCB:
            return 4, 'seq', None, None
        if b == 0xE9:
            return 2, 'ijump', None, None
        n, kind, tgt, extra = decode_plain(rd, pc + 1, b)
        return 1 + n + (b in HLMEM), kind, tgt, extra
    return decode_plain(rd, pc, op)


def decode_plain(rd, pc, op):
    n = base_len(op)
    nn = rd(pc + 1) | rd(pc + 2) << 8 if n == 3 else None
    rel = (pc + 2 + ((rd(pc + 1) ^ 0x80) - 0x80)) & 0xFFFF
    if op == 0xC3: return n, 'jump', nn, None
    if op & 0xC7 == 0xC2: return n, 'cjump', nn, None
    if op == 0x18: return n, 'jump', rel, None
    if op in (0x10, 0x20, 0x28, 0x30, 0x38): return n, 'cjump', rel, None
    if op == 0xCD: return n, 'call', nn, None
    if op & 0xC7 == 0xC4: return n, 'ccall', nn, None
    if op == 0xC9: return n, 'ret', None, None
    if op & 0xC7 == 0xC0: return n, 'cret', None, None
    if op & 0xC7 == 0xC7: return n, 'rst', op & 0x38, None
    if op == 0xE9: return n, 'ijump', None, None
    if op in WORD_OPS: return n, 'seq', None, ('word', WORD_OPS[op], nn)
    if op == 0xD3: return n, 'seq', None, ('out', rd(pc + 1))
    return n, 'seq', None, None


class Image:
    def __init__(self, name, cfg, root):
        self.name, self.org = name, int(cfg['origin'], 16)
        data = open(os.path.join(root, cfg['file']), 'rb').read()
        off = int(cfg.get('offset', '0'), 16)
        self.data = data[off:off + int(cfg['length'], 16)] if 'length' in cfg else data[off:]
        self.links = {int(s, 16): (t.split(':')[0], int(t.split(':')[1], 16)) for s, t in cfg.get('links', {}).items()}
        self.bridge = {int(a, 16): o for a, o in cfg.get('bridge', {}).items()}  # "jp <addr>" continues at HL in image o
        self.code = {}       # instruction start -> length
        self.owner = {}      # byte addr -> instruction start
        self.data_tables = []  # (addr, len, type)
        self.labels = {}     # addr -> name
        self.rst30 = []      # (site, code)
        self.ext = collections.defaultdict(set)  # external target -> sites
        self.ijump, self.overlap, self.words, self.outs = [], [], collections.defaultdict(set), []
        self.xfer = []       # (site, target, page-3 field, image) for jumps/calls made after paging another image in

    def inside(self, a):
        return self.org <= a < self.org + len(self.data)

    def rd(self, a):
        return self.data[a - self.org] if self.inside(a) else 0

    def resolve(self, t):
        return t if self.inside(t) else None


def bridge_jump(img, pc, hl, cross):
    """'jp <bridge>' with a known HL: record the transfer into the other ROM; True if handled"""
    if hl is None or img.rd(pc) != 0xC3 or (img.rd(pc + 1) | img.rd(pc + 2) << 8) not in img.bridge:
        return False
    other = img.bridge[img.rd(pc + 1) | img.rd(pc + 2) << 8]
    img.xfer.append((pc, hl, 'hl', other))
    if other in cross:
        cross[other].add(hl)
    return True


def trace(img, entries, inline_rst, inline_call, page3, page0, imgorg, cross):
    work = [(a, None, None) for a in entries]
    while work:
        pc, hl, src = work.pop()
        if pc in img.code:
            bridge_jump(img, pc, hl, cross)
            continue
        p3 = p0 = a_imm = None  # page-3 / page-0 fields set by "ld a,n ... out (2),a" in this linear run
        while img.inside(pc):
            if pc in img.code:  # joined already-traced code: a bridge jump there still needs this run's HL
                bridge_jump(img, pc, hl, cross)
                break
            n, kind, tgt, extra = decode(img.rd, pc)
            op = img.rd(pc)
            if op == 0xD3 and img.rd(pc + 1) == 0x02 and a_imm is not None:
                p3, p0 = a_imm >> 6, a_imm >> 3 & 3
            if bridge_jump(img, pc, hl, cross):
                img.code[pc] = n
                for i in range(n):
                    img.owner.setdefault(pc + i, pc)
                break
            a_imm = img.rd(pc + 1) if op == 0x3E else a_imm if op in A_KEEP else None
            hl = img.rd(pc + 1) | img.rd(pc + 2) << 8 if op == 0x21 else hl if op in HL_KEEP else None
            clash = [pc + i for i in range(n) if pc + i in img.owner]
            if clash:  # entry inside another instruction (e.g. jr into the operand of "ld a,08h"): keep both
                img.overlap.append((pc, img.owner[clash[0]], src))
            img.code[pc] = n
            for i in range(n):
                img.owner.setdefault(pc + i, pc)
            nxt = pc + n
            if extra and extra[0] == 'word':
                img.words[extra[2]].add(pc)
            elif extra and extra[0] == 'out':
                img.outs.append((pc, extra[1]))
            if kind == 'rst':
                spec = str(inline_rst.get(tgt, 0))
                if tgt == 0x30 and img.inside(nxt):
                    img.rst30.append((pc, img.rd(nxt)))
                if spec.rstrip('!') != '0':
                    nxt += inline_len(img, nxt, spec.rstrip('!'))
                if spec.endswith('!'):  # e.g. BASIC "raise error n": does not return
                    break
            if kind in ('jump', 'cjump', 'call', 'ccall'):
                r = img.resolve(tgt)
                other = page3.get(p3, '?') if p3 is not None and tgt >= 0xC000 else img.name
                if p0 is not None and tgt < 0x4000 and p0 in page0:  # ROM paged into page 0: image address = org + target
                    other = page0[p0]
                    if other == img.name:
                        r = img.org + tgt
                    else:
                        tgt += imgorg[other]
                if pc in img.links:  # configured cross-ROM link (paging set up outside this linear run)
                    other, tgt = img.links[pc]
                if other != img.name:
                    img.xfer.append((pc, tgt, p3, other))
                    if other in cross:
                        cross[other].add(tgt)
                    if kind == 'jump':
                        break
                    pc = nxt
                    continue
                if r is None:
                    img.ext[tgt].add(pc)
                else:
                    work.append((r, hl if kind in ('jump', 'cjump') else None, pc))
                    if kind in ('call', 'ccall'):
                        img.labels.setdefault(r, 'sub_%04x' % r)
                    spec = inline_call.get(r if r is not None else tgt)
                    if spec is not None and kind in ('call', 'ccall'):
                        nxt += inline_len(img, nxt, spec)
            if kind == 'ijump':
                img.ijump.append(pc)
            if kind in ('jump', 'ret', 'ijump'):
                break
            pc = nxt


def inline_len(img, a, spec):
    if spec == 'z':  # zero-terminated
        n = 0
        while img.inside(a + n) and img.rd(a + n): n += 1
        n += 1
    elif spec.startswith('bit7'):  # last byte has bit 7 set; "bit7/05=1" = op 05h (any bit 7) takes 1 operand byte
        operands = {int(k, 16): int(v) for k, v in (x.split('=') for x in spec.split('/')[1:])}
        n = 0
        while img.inside(a + n):
            b = img.rd(a + n)
            n += 1 + operands.get(b & 0x7F, 0)
            if b & 0x80:
                break
    elif spec == 'len':  # length-prefixed
        n = 1 + img.rd(a)
    else:
        n = int(spec)
    img.data_tables.append((a, n, 'bytedata'))
    return n


def run(cfg_path, out):
    """trace every image of the config, write the outputs, return {name: Image} (code starts in Image.code)"""
    cfg = json.load(open(cfg_path))
    os.makedirs(out, exist_ok=True)
    root = cfg['root']
    inline_rst = {int(k, 16): v for k, v in cfg.get('inline_rst', {}).items()}
    page3 = {int(k): v for k, v in cfg.get('page3', {}).items()}
    imgs, pending, calls = {}, {}, {}
    for name, icfg in cfg['images'].items():
        img = imgs[name] = Image(name, icfg, root)
        calls[name] = {int(k, 16): v for k, v in icfg.get('inline_call', {}).items()}
        entries = pending[name] = []
        for a, lab in icfg.get('entries', {}).items():
            entries.append(int(a, 16)); img.labels[int(a, 16)] = lab
        for t in icfg.get('tables', []):  # {"addr", "count", "name"}; kind "devtab" = pointers to "defb n / defw x n" tables, "counted" = one such table
            a = int(t['addr'], 16)
            cnt = img.rd(a) if t.get('kind') == 'counted' else t['count']
            img.labels.setdefault(a, t['name'])
            if t.get('kind') == 'counted':
                img.data_tables.append((a, 1, 'bytedata'))
                a += 1
            img.data_tables.append((a, 2 * cnt, 'pointers'))
            subs = [(a, cnt, t['name'])]
            if t.get('kind') == 'devtab':
                subs = []
                for i in range(cnt):
                    p = img.rd(a + 2 * i) | img.rd(a + 2 * i + 1) << 8
                    if img.inside(p):
                        img.data_tables.append((p, 1, 'bytedata'))
                        img.data_tables.append((p + 1, 2 * img.rd(p), 'pointers'))
                        img.labels.setdefault(p, '%s%d' % (t['name'], i))
                        subs.append((p + 1, img.rd(p), '%s%d' % (t['name'], i)))
            for b, n, nm in subs:
                for i in range(n):
                    w = img.rd(b + 2 * i) | img.rd(b + 2 * i + 1) << 8
                    if img.inside(w):
                        entries.append(w); img.labels.setdefault(w, '%s_%d' % (nm, i))
        for a, lab in icfg.get('names', {}).items():
            img.labels[int(a, 16)] = lab
    page0 = {int(k): v for k, v in cfg.get('page0', {}).items()}
    imgorg = {n: img.org for n, img in imgs.items()}
    while any(pending.values()):
        cross = {n: set() for n in imgs}
        for name, img in imgs.items():
            trace(img, pending[name], inline_rst, calls[name], page3, page0, imgorg, cross)
        for n, img in imgs.items():
            pending[n] = [a for a in cross[n] if a not in img.code and img.inside(a)]
            for a in pending[n]:
                img.labels.setdefault(a, 'x_%04x' % a)
    for name, img in imgs.items():
        write(img, out, cfg['images'][name])
    return imgs


def write(img, out, icfg):
    # blocks: merge code runs; explicit data tables override; everything else is bytedata
    kind = ['data'] * len(img.data)
    for a, n in img.code.items():
        for i in range(n): kind[a - img.org + i] = 'code'
    for a, n, t in img.data_tables:
        for i in range(n):
            if img.inside(a + i): kind[a - img.org + i] = t
    blocks, start = [], 0
    for i in range(1, len(kind) + 1):
        if i == len(kind) or kind[i] != kind[start]:
            blocks.append((start + img.org, i + img.org, 'bytedata' if kind[start] == 'data' else kind[start])); start = i
    with open(os.path.join(out, img.name + '.blk'), 'w') as f:
        for k, (a, b, t) in enumerate(blocks):
            f.write('b%03d_%04x: unlabeled start 0x%04x unlabeled last 0x%04x type %s\n' % (k, a, a, b - 1, t))
    with open(os.path.join(out, img.name + '.sym'), 'w') as f:
        for a in sorted(img.labels):
            f.write('%s: equ 0x%04x\n' % (img.labels[a], a))
    code_bytes = sum(img.code.values())
    with open(os.path.join(out, img.name + '.report.txt'), 'w') as f:
        f.write('image %s origin %04x len %04x: %d instructions, %d code bytes (%.1f%%)\n' % (
            img.name, img.org, len(img.data), len(img.code), code_bytes, 100.0 * code_bytes / len(img.data)))
        f.write('\n[rst30] %d sites; codes used: %s\n' % (len(img.rst30), ' '.join('%02x' % c for c in sorted({c for _, c in img.rst30}))))
        for s, c in sorted(img.rst30): f.write('  %04x %02x\n' % (s, c))
        f.write('\n[external targets] target <- sites\n')
        for t in sorted(img.ext): f.write('  %04x <- %s\n' % (t, ' '.join('%04x' % s for s in sorted(img.ext[t]))))
        f.write('\n[xfer] site target page3-field image (jump/call after paging another ROM in)\n')
        f.write(''.join('  %04x %04x %s %s\n' % (s, t, p, o) for s, t, p, o in sorted(img.xfer)))
        f.write('\n[indirect jumps]\n' + ''.join('  %04x\n' % s for s in sorted(img.ijump)))
        f.write('\n[overlaps] new-start existing-start queued-by\n' + ''.join('  %04x %04x %s\n' % (a, b, '%04x' % s if s is not None else 'entry') for a, b, s in img.overlap))
        f.write('\n[out (n),a] site port\n' + ''.join('  %04x %02x\n' % o for o in sorted(img.outs)))
        f.write('\n[16-bit immediates] value <- sites\n')
        for v in sorted(img.words): f.write('  %04x <- %s\n' % (v, ' '.join('%04x' % s for s in sorted(img.words[v]))))
    print('%s: %d instr, %d code bytes (%.1f%%), %d rst30, %d ext targets, %d ijumps, %d overlaps' % (
        img.name, len(img.code), code_bytes, 100.0 * code_bytes / len(img.data), len(img.rst30), len(img.ext), len(img.ijump), len(img.overlap)))


if __name__ == '__main__':
    run(sys.argv[1], sys.argv[2])
