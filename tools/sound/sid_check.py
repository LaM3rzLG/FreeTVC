# SPDX-License-Identifier: GPL-3.0-or-later
"""SID card check: runs tools/sound/probes/sid.asm in freetvc-headless with the SID card and measures the WAV.

Usage: python3 -I tools/sound/sid_check.py <freetvc-headless> <out dir> [6581|8580]
Writes <out dir>/sid-<model>.wav, prints the pitch of the three notes, the loudness per 100 ms and OSC3/ENV3 as read.
"""
import os, re, struct, subprocess, sys, wave
import shutil

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
SJASM = os.environ.get('SJASMPLUS') or shutil.which('sjasmplus') or os.path.join(ROOT, '.tools', 'sjasmplus-1.24.0', 'bin', 'sjasmplus')
PROBE = os.path.join(ROOT, 'tools', 'sound', 'probes', 'sid.asm')
# (name, start s, end s, expected Hz) relative to the first sound
NOTES = [('sawtooth A4', 0.05, 0.55, 440.0), ('pulse C5', 0.85, 1.25, 523.25), ('triangle E5', 1.45, 1.85, 659.26)]


def assemble(out):
    img, sym = os.path.join(out, 'sid.bin'), os.path.join(out, 'sid.sym')
    subprocess.run([SJASM, '--nologo', '--msg=war', f'--sym={sym}', f'-DIMAGE="{img}"', PROBE], check=True,
                   cwd=os.path.dirname(PROBE))
    syms = {}
    for line in open(sym):
        k, v = re.match(r'(\S+): EQU 0x([0-9A-Fa-f]+)', line).groups()
        syms[k] = int(v, 16)
    return bytearray(open(img, 'rb').read()), syms


def samples(path):
    w = wave.open(path)
    n = w.getnframes()
    return w.getframerate(), struct.unpack(f'<{n}h', w.readframes(n))


def pitch(x, rate, hz):
    """Autocorrelation peak near the expected period, refined by a parabola; zero crossings miscount the 6581's sawtooth."""
    import numpy as np
    n = round(4 * rate / hz)  # the local mean over four periods: the 6581's envelope moves the DC level
    y = np.asarray(x, dtype=float)
    y = (y - np.convolve(y, np.ones(n) / n, 'same'))[n:-n]
    ac = np.correlate(y, y, 'full')[len(y) - 1:]
    lo, hi = int(rate / hz * 0.8), int(rate / hz * 1.25) + 2
    lag = lo + int(np.argmax(ac[lo:hi]))
    a, b, c = ac[lag - 1], ac[lag], ac[lag + 1]
    return rate / (lag + 0.5 * (a - c) / (a - 2 * b + c))


def analyse(wav):
    """Prints the length, the three notes' pitch and the loudness per 100 ms from the first sound; returns the RMS list."""
    import math
    rate, x = samples(wav)
    first = next(i for i, v in enumerate(x) if abs(v) > 300)
    print(f'{len(x) / rate:.2f} s at {rate} Hz, first sound at {first / rate:.3f} s, peak {max(map(abs, x))}')
    for name, a, b, hz in NOTES:
        f = pitch(x[first + int(a * rate):first + int(b * rate)], rate, hz)
        print(f'{name}: {f:.2f} Hz (expected {hz:.2f}, {1200 * math.log2(f / hz):+.1f} cents)')
    step = rate // 10
    rms = []
    for i in range(first, len(x) - step, step):
        seg = x[i:i + step]
        m = sum(seg) / len(seg)
        rms.append(int((sum((v - m) ** 2 for v in seg) / len(seg)) ** 0.5))
    print('RMS per 100 ms:', ' '.join(map(str, rms)))
    return rms


def main():
    exe, out = sys.argv[1], sys.argv[2]
    model = sys.argv[3] if len(sys.argv) > 3 else '6581'
    os.makedirs(out, exist_ok=True)
    img, syms = assemble(out)
    img[syms['GO']] = 1
    start, end = syms['R_ENTRY_START'], syms['R_RES_END']
    code = os.path.join(out, 'sid-6000.bin')
    open(code, 'wb').write(img[start:end])
    cmds = os.path.join(out, 'sid.mon')
    open(cmds, 'w').write(f"break {syms['ENDSEQ']:04X}\ng\nm {syms['RES']:04X} {syms['RES_END'] - 1:04X}\nquit\n")
    wav = os.path.join(out, f'sid-{model}.wav')
    run = subprocess.run([exe, '--sid-card', model, '--load', f'{code}@{start:04X}', '--pc', f'{syms["ENTRY"]:04X}',
                          '--moncommands', cmds, '--wav', wav], capture_output=True, text=True, timeout=120)
    if run.returncode != 0:
        print(run.stdout.strip(), run.stderr.strip())
        sys.exit(1)
    print('OSC3, ENV3:', next(line for line in run.stdout.splitlines() if line.startswith('>C:')))
    analyse(wav)


if __name__ == '__main__':
    main()
