# SPDX-License-Identifier: GPL-3.0-or-later
# Usage: python3 -I tools/ui/ui_drive.py <freetvc exe> <out dir> STEP...   (from the project root)
# Starts the GUI and runs the steps with real X input (XTEST, only while freeTVC has focus); window grabs go to <out dir>.
# Steps: wait SECONDS | grab NAME | key KEYSYM[+KEYSYM...] | type TEXT | click X Y | dclick X Y | rclick X Y | move X Y (window pixels) | sh COMMAND
# Env: EXTRA = extra freetvc arguments (default: --rom-dir testdata/roms --no-audio).
#      XDG_DATA_HOME = the GUI's user data folder (default: <out dir>/xdg, so freetvc.ini and layout.ini of the user stay untouched).
import os, subprocess, sys, time
from Xlib import display, X, XK
from Xlib.ext import xtest
from PIL import Image

exe, out_dir, steps = sys.argv[1], sys.argv[2], sys.argv[3:]
args = os.environ.get('EXTRA', '--rom-dir testdata/roms --no-audio').split()
env = dict(os.environ)
env.setdefault('XDG_DATA_HOME', os.path.join(os.path.abspath(out_dir), 'xdg'))
t0 = time.time()
emu = subprocess.Popen([exe] + args, stderr=open(os.path.join(out_dir, 'stderr.txt'), 'w'), env=env)
d = display.Display()
root = d.screen().root
pidatom = d.intern_atom('_NET_WM_PID')
win = None
while win is None and time.time() - t0 < 20:
    stack = [root]
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
    emu.terminate()
    sys.exit('no freeTVC window after 20 s')
print('window', hex(win.id), 'after %.1f s' % (time.time() - t0))


def ours():
    a = root.get_full_property(d.intern_atom('_NET_ACTIVE_WINDOW'), X.AnyPropertyType)
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


def move(x, y):
    activate()
    origin = win.translate_coords(root, 0, 0)
    xtest.fake_input(d, X.MotionNotify, x=x - origin.x, y=y - origin.y)
    d.sync(); time.sleep(0.15)


def click(x, y, button=1, count=1):
    move(x, y)
    for _ in range(count):
        xtest.fake_input(d, X.ButtonPress, button)
        d.sync(); time.sleep(0.05)
        xtest.fake_input(d, X.ButtonRelease, button)
        d.sync(); time.sleep(0.05)
    time.sleep(0.15)


def grab(name):
    g = win.get_geometry()
    raw = win.get_image(0, 0, g.width, g.height, X.ZPixmap, 0xffffffff)
    Image.frombytes('RGB', (g.width, g.height), raw.data, 'raw', 'BGRX').save(os.path.join(out_dir, name + '.png'))
    print('grab', name, '%dx%d' % (g.width, g.height), 'at %.1f s' % (time.time() - t0))


keysyms = {' ': 'space', ',': 'comma', '.': 'period', '"': 'quotedbl', '=': 'equal', '-': 'minus'}
i = 0
try:
    while i < len(steps):
        op = steps[i]
        if op == 'wait':
            time.sleep(float(steps[i + 1])); i += 2
        elif op == 'grab':
            grab(steps[i + 1]); i += 2
        elif op == 'key':
            key(steps[i + 1].split('+')); i += 2
        elif op == 'type':
            for ch in steps[i + 1]:
                key(['Shift_L', ch.lower()] if ch.isupper() else [keysyms.get(ch, ch)])
            i += 2
        elif op == 'sh':
            subprocess.run(steps[i + 1], shell=True, check=True); i += 2
        elif op in ('click', 'move'):
            (click if op == 'click' else move)(int(steps[i + 1]), int(steps[i + 2])); i += 3
        elif op in ('dclick', 'rclick'):
            click(int(steps[i + 1]), int(steps[i + 2]), 3 if op == 'rclick' else 1, 2 if op == 'dclick' else 1); i += 3
        else:
            sys.exit('unknown step ' + op)
finally:
    # A failed step must not leave the emulator window open
    emu.terminate(); emu.wait(5)
    print('emulator exit', emu.returncode)
