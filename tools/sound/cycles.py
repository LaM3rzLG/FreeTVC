# SPDX-License-Identifier: GPL-3.0-or-later
# Usage: python3 -I tools/sound/cycles.py <16-bit mono WAV, 48 kHz>
# Per-cycle check of the first to last tone window (attack and release trimmed): cycle count, min/max length, and gaps (dropouts).
import sys, wave, struct

w = wave.open(sys.argv[1])
rate = w.getframerate()
n = w.getnframes()
x = struct.unpack('<%dh' % n, w.readframes(n))
win = rate // 100
act = []
for i in range(0, n - win, win):
    s = x[i:i + win]
    m = sum(s) / win
    act.append(sum((v - m) ** 2 for v in s) / win > (0.01 * 32767) ** 2)
idx = [i for i, a in enumerate(act) if a]
if not idx:
    sys.exit('no tone')
a, b = (idx[0] + 4) * win, (idx[-1] - 3) * win
seg = x[a:b]
m = sum(seg) / len(seg)
cr = [i - 1 + (m - x[i - 1]) / (x[i] - x[i - 1]) for i in range(a + 1, b) if x[i - 1] - m < 0 <= x[i] - m]
d = [cr[i + 1] - cr[i] for i in range(len(cr) - 1)]
gaps = [(round(cr[i] / rate, 3), round(v, 1)) for i, v in enumerate(d) if v > 40]  # 40 samples: above P = 3950's 35.9
print('tone %.3f-%.3f s, cycles %d, min %.3f max %.3f, mean %.4f samples, gaps (s, samples) %s'
      % (a / rate, b / rate, len(d), min(d), max(d), sum(d) / len(d), gaps))
