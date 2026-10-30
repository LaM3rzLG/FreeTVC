# SPDX-License-Identifier: GPL-3.0-or-later
# Usage: python3 -I tools/sound/wavinfo.py <16-bit mono WAV>
# Prints length and peaks, then each tone burst (10 ms RMS windows > 0.01) with its length, mean frequency and peak-to-peak.
import sys, wave, struct, math

w = wave.open(sys.argv[1])
rate = w.getframerate()
n = w.getnframes()
x = [v / 32767 for v in struct.unpack('<%dh' % n, w.readframes(n))]
print('rate', rate, 'samples', n, 'seconds %.3f' % (n / rate), 'peak %.4f min %.4f' % (max(x), min(x)))
win = rate // 100
active = []
for i in range(0, n - win, win):
    seg = x[i:i + win]
    m = sum(seg) / win
    active.append(math.sqrt(sum((v - m) ** 2 for v in seg) / win) > 0.01)
runs, start = [], None
for i, a in enumerate(active + [False]):
    if a and start is None:
        start = i
    if not a and start is not None:
        runs.append((start, i)); start = None
for s, e in runs:
    a, b = s * win, e * win
    seg = x[a:b]
    m = sum(seg) / len(seg)
    # rising zero crossings, linearly interpolated
    cr = [i - 1 + (m - x[i - 1]) / (x[i] - x[i - 1]) for i in range(a + 1, b) if x[i - 1] - m < 0 <= x[i] - m]
    f = (len(cr) - 1) / ((cr[-1] - cr[0]) / rate) if len(cr) > 2 else 0
    print('tone from %.3f s to %.3f s (%.0f ms): f = %.3f Hz (includes the attack), p-p %.3f'
          % (a / rate, b / rate, (b - a) * 1000 / rate, f, max(seg) - min(seg)))
