# SPDX-License-Identifier: GPL-3.0-or-later
# Usage: python3 -I tools/sound/grab_load.py <seconds>
# Load for the dropout experiments: grabs a 960 × 660 root-window region every 0.35 s, the same XGetImage call as gui_sound.py.
import sys, time
from Xlib import display, X
from PIL import Image

root = display.Display().screen().root
t0 = time.time()
n = 0
while time.time() - t0 < float(sys.argv[1]):
    raw = root.get_image(0, 0, 960, 660, X.ZPixmap, 0xffffffff)
    Image.frombytes('RGB', (960, 660), raw.data, 'raw', 'BGRX')
    n += 1
    time.sleep(0.35)
print('grabs', n)
