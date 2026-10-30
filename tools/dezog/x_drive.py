# SPDX-License-Identifier: GPL-3.0-or-later
# Usage: python3 -I tools/dezog/x_drive.py <out dir> STEP...
# Real X input (XTEST) into already running windows, found by title; window grabs go to <out dir>.
# Steps: win REGEX (choose the window by title) | wait S | grab NAME | key KEYSYM[+KEYSYM...] | type TEXT | click X Y
import os, re, subprocess, sys, time
from Xlib import display, X, XK
from Xlib.ext import xtest
from PIL import Image

out_dir, steps = sys.argv[1], sys.argv[2:]
d = display.Display()
root = d.screen().root
name_atom = d.intern_atom('_NET_WM_NAME')
win = None


def title(w):
    p = w.get_full_property(name_atom, X.AnyPropertyType)
    return p.value.decode('utf-8', 'replace') if p else (w.get_wm_name() or '')


def find(pattern):
    for _ in range(100):
        stack = [root]
        while stack:
            w = stack.pop()
            try:
                if re.search(pattern, title(w)) and w.get_attributes().map_state == X.IsViewable:
                    return w
                stack.extend(w.query_tree().children)
            except Exception:
                pass
        time.sleep(0.2)
    sys.exit('no window matching ' + pattern)


def active():
    a = root.get_full_property(d.intern_atom('_NET_ACTIVE_WINDOW'), X.AnyPropertyType)
    return a.value[0] if a else 0


def activate():
    if active() == win.id:
        return
    subprocess.run(['wmctrl', '-i', '-a', hex(win.id)])
    for _ in range(60):
        if active() == win.id:
            return
        time.sleep(0.05)
    raise RuntimeError('cannot focus ' + title(win))


def keycodes(name):
    # A keysym on the shifted level of its key (e.g. '=' on a Hungarian layout) also needs Shift
    sym = XK.string_to_keysym(name)
    found = sorted(d.keysym_to_keycodes(sym), key=lambda ci: ci[1])
    assert found and found[0][1] <= 1, name
    code, level = found[0]
    return [d.keysym_to_keycode(XK.string_to_keysym('Shift_L')), code] if level == 1 else [code]


def key(names, hold=0.08, gap=0.15):
    activate()
    codes = [c for n in names for c in keycodes(n)]
    for c in codes:
        xtest.fake_input(d, X.KeyPress, c)
    d.sync(); time.sleep(hold)
    for c in reversed(codes):
        xtest.fake_input(d, X.KeyRelease, c)
    d.sync(); time.sleep(gap)


def click(x, y):
    activate()
    origin = win.translate_coords(root, 0, 0)
    xtest.fake_input(d, X.MotionNotify, x=x - origin.x, y=y - origin.y)
    d.sync(); time.sleep(0.15)
    xtest.fake_input(d, X.ButtonPress, 1)
    d.sync(); time.sleep(0.08)
    xtest.fake_input(d, X.ButtonRelease, 1)
    d.sync(); time.sleep(0.2)


def grab(name):
    g = win.get_geometry()
    raw = win.get_image(0, 0, g.width, g.height, X.ZPixmap, 0xffffffff)
    Image.frombytes('RGB', (g.width, g.height), raw.data, 'raw', 'BGRX').save(os.path.join(out_dir, name + '.png'))
    print('grab', name, '%dx%d' % (g.width, g.height), flush=True)


keysyms = {' ': 'space', ',': 'comma', '.': 'period', '"': 'quotedbl', '=': 'equal', '-': 'minus', ':': 'colon',
           '/': 'slash', '_': 'underscore', '*': 'asterisk', '(': 'parenleft', ')': 'parenright'}
i = 0
while i < len(steps):
    op = steps[i]
    if op == 'win':
        win = find(steps[i + 1]); print('window', hex(win.id), title(win), flush=True); i += 2
    elif op == 'wait':
        time.sleep(float(steps[i + 1])); i += 2
    elif op == 'grab':
        grab(steps[i + 1]); i += 2
    elif op == 'key':
        key(steps[i + 1].split('+')); i += 2
    elif op == 'type':
        for ch in steps[i + 1]:
            key([keysyms.get(ch, ch)])
        i += 2
    elif op == 'click':
        click(int(steps[i + 1]), int(steps[i + 2])); i += 3
    else:
        sys.exit('unknown step ' + op)
