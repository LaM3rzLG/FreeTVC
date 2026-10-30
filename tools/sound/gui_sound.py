# SPDX-License-Identifier: GPL-3.0-or-later
# Usage: python3 -I tools/sound/gui_sound.py <freetvc exe> <out dir> [BASIC line]   (from the project root)
# Starts the GUI, records the sink monitor with parec, types the line with XTEST keys (only while freeTVC has focus), grabs the window.
# Env: EXTRA = extra freetvc arguments; NOGRAB = skip the grab during the tone; MONITOR = parec source (default @DEFAULT_MONITOR@).
import os, subprocess, sys, time
from Xlib import display, X, XK
from Xlib.ext import xtest
from PIL import Image

exe, out_dir = sys.argv[1], sys.argv[2]
line = sys.argv[3] if len(sys.argv) > 3 else 'SOUND PITCH 3950,VOLUME 15,DURATION 50'
rec = subprocess.Popen(['parec', '-d', os.environ.get('MONITOR', '@DEFAULT_MONITOR@'), '--rate=48000', '--channels=1',
                        '--format=s16le', '--file-format=wav', os.path.join(out_dir, 'gui_rec.wav')])
time.sleep(0.5)
t0 = time.time()
emu = subprocess.Popen([exe, '--rom-dir', os.environ.get('FREETVC_ROM_DIR', 'testdata/roms')] + os.environ.get('EXTRA', '').split(),
                       stderr=open(os.path.join(out_dir, 'gui_stderr.txt'), 'w'))
d = display.Display()
pidatom = d.intern_atom('_NET_WM_PID')
win = None
while win is None and time.time() - t0 < 20:
    stack = [d.screen().root]
    while stack:
        w = stack.pop()
        try:
            p = w.get_full_property(pidatom, X.AnyPropertyType)
            if p and p.value[0] == emu.pid and w.get_attributes().map_state == X.IsViewable:
                win = w
                break
            stack.extend(w.query_tree().children)
        except Exception:
            pass
    time.sleep(0.2)
if win is None:
    emu.terminate(); rec.terminate()
    sys.exit('no freeTVC window after 20 s')
print('window', hex(win.id), 'after %.1f s' % (time.time() - t0))


def ours():
    a = d.screen().root.get_full_property(d.intern_atom('_NET_ACTIVE_WINDOW'), X.AnyPropertyType)
    if not a or not a.value[0]:
        return False
    try:
        p = d.create_resource_object('window', a.value[0]).get_full_property(pidatom, X.AnyPropertyType)
        return bool(p) and p.value[0] == emu.pid
    except Exception:
        return False


def activate():
    if ours():
        return
    subprocess.run(['wmctrl', '-i', '-a', hex(win.id)])
    for _ in range(40):
        if ours():
            return
        time.sleep(0.05)
    raise RuntimeError('cannot focus freeTVC')


def key(names, hold=0.08, gap=0.12):
    activate()
    codes = [d.keysym_to_keycode(XK.string_to_keysym(n)) for n in names]
    assert 0 not in codes, names
    for c in codes:
        xtest.fake_input(d, X.KeyPress, c)
    d.sync(); time.sleep(hold)
    for c in reversed(codes):
        xtest.fake_input(d, X.KeyRelease, c)
    d.sync(); time.sleep(gap)


def grab(name):
    g = win.get_geometry()
    raw = win.get_image(0, 0, g.width, g.height, X.ZPixmap, 0xffffffff)
    Image.frombytes('RGB', (g.width, g.height), raw.data, 'raw', 'BGRX').save(os.path.join(out_dir, name))


keysyms = {' ': 'space', ',': 'comma', '.': 'period'}
time.sleep(max(0, t0 + 8 - time.time()))  # boot + logo
grab('gui_1_logo.png')
key(['space'])
time.sleep(1.5)
grab('gui_2_prompt.png')
t_type = time.time() - t0
for ch in line:
    if ch.isupper():
        key(['Shift_L', ch.lower()])
    else:
        key([keysyms.get(ch, ch)])
grab('gui_3_typed.png')
t_return = time.time() - t0
key(['Return'])
time.sleep(0.5)
if not os.environ.get('NOGRAB'):
    grab('gui_4_during.png')
time.sleep(3)
grab('gui_5_after.png')
print('typing started %.2f s, RETURN at %.2f s (emulator start + 0.5 s; the parec start offset is not measured)'
      % (t_type + 0.5, t_return + 0.5))
emu.terminate(); emu.wait(5)
time.sleep(0.5)
rec.terminate(); rec.wait(5)
print('emulator exit', emu.returncode)
