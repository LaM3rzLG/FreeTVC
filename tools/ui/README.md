# tools/ui: GUI checks with real X input

`ui_drive.py` starts the `freetvc` GUI and runs a list of steps with real X input: XTEST key presses and mouse clicks, sent only while the freeTVC window has focus, and window grabs saved as PNG. It is how GUI behaviour is checked end to end, through the same input path a user's keyboard and mouse take. The GUI itself is described in [ui.md](../../docs/design/ui.md).

## Prerequisites

- An X11 display with a window manager (focus goes through `_NET_ACTIVE_WINDOW`), and `wmctrl`.
- Python 3 with python-xlib and Pillow (Debian/Ubuntu: `python3-xlib`, `python3-pil`).
- A GUI build, for example `build/gcc-release/src/freetvc`, and the system ROMs (set `FREETVC_ROM_DIR`, for example to `$PWD/testdata/roms`, or build with `-DFREETVC_FETCH_ROMS=ON`).
- For the virtual display: Xvfb and metacity (Debian/Ubuntu: `xvfb`, `metacity`); for file dialogs also `dbus-run-session` (package `dbus`), `zenity` and `xdotool`.

## Usage

From the project root:

```sh
python3 -I tools/ui/ui_drive.py build/gcc-release/src/freetvc <out dir> wait 7 key space wait 1 type 'print 6' grab prompt
```

The script prints the window id when the window appears, a line per grab with its size and time, and `emulator exit <code>` at the end. It stops the emulator when a step fails. The GUI's stderr, including the status strip's notices, goes to `<out dir>/stderr.txt`.

| Step | Does |
|---|---|
| `wait S` | sleeps S seconds |
| `grab NAME` | saves the window as `<out dir>/NAME.png` |
| `key A[+B…]` | presses the X keysyms together, then releases them (`key Shift_L+a`, `key Pause`, `key F12`) |
| `type TEXT` | types TEXT key by key: letters, digits, space and `,.="-`; upper case letters add Shift |
| `click X Y`, `move X Y` | left click at, or move the pointer to, window pixel X, Y (read the coordinates off a grab) |
| `dclick X Y`, `rclick X Y` | a double click (two clicks 0.1 s apart), a right click |
| `sh CMD` | runs a shell command, for example appending a line to the skin in use to test its live reload |

Environment:
- `EXTRA` replaces the `freetvc` arguments. The default is `--rom-dir testdata/roms --no-audio`; when that folder is missing, the ROM search goes on to `FREETVC_ROM_DIR`. Example: `EXTRA="--no-audio --language en --skin <folder>/try.skin"`.
- `XDG_DATA_HOME`: the GUI's user data folder. The GUI writes `freetvc.ini` and `layout.ini` there on exit, so `ui_drive.py` sets it to `<out dir>/xdg` unless it is already set: your own settings stay untouched.

### Typing quirks

- `type` sends punctuation as the bare key, without Shift. A character that sits on a shifted level of the X keyboard layout arrives as that key's unshifted character: on a Hungarian layout `"` arrives as `2` and `=` as `7`. Send such characters with `key`, for example `key Shift_L+2`.
- Any other punctuation (for example `:` or `+`) has no keysym name in the script, so `type` stops with an error. Use `key` with the key's keysym, plus `Shift_L` where the layout needs it.

### Window grabs

The GUI draws with OpenGL by default. A grab of the root window shows a black rectangle where an OpenGL window is, so always grab the freeTVC window itself by its window id, as `grab` does; in `sh` steps use the window id, never the root window.

### Coordinates

Coordinates depend on the window size (1000 × 760 by default), the skin and the UI language. With the default `bronze` skin and the English UI, the status strip's items are at about: slot ribs (27, 745), nameplate (83, 745), preset (144, 745), the play key (396, 745) and **no tape** (437, 745) ([ui.md](../../docs/design/ui.md), section "Strip menus"). The GUI picks the UI language from the operating system's preferred languages when neither `--language` nor `freetvc.ini` sets one; Hungarian strip items sit further right, so pass `--language en` in `EXTRA` when using these coordinates.

## Running on a virtual display

Real key events go to whatever has the focus, so run checks on a virtual X display to keep them away from your desktop:

```sh
Xvfb :99 -screen 0 1280x1024x24 -nolisten tcp &
DISPLAY=:99 metacity &
DISPLAY=:99 FREETVC_ROM_DIR=$PWD/testdata/roms python3 -I tools/ui/ui_drive.py build/gcc-release/src/freetvc <out dir> wait 7 key space wait 1 type 'print 6' grab prompt
pkill -x metacity; pkill -x Xvfb
```

metacity is needed because `wmctrl` and the focus check need a window manager. Keep `--no-audio` (the default `EXTRA`) on the virtual display. Stop the servers with `pkill -x`: `pkill -f 'Xvfb :99'` also matches the calling shell's own command line and kills it.

## File dialogs

SDL opens file dialogs through the desktop portal over the D-Bus session bus. On a virtual display that would open the dialog on your real desktop instead. Run `ui_drive.py` under `dbus-run-session --` with `SDL_FILE_DIALOG_DRIVER=zenity`:

```sh
DISPLAY=:99 SDL_FILE_DIALOG_DRIVER=zenity dbus-run-session -- python3 -I tools/ui/ui_drive.py build/gcc-release/src/freetvc <out dir> ...
```

The dialog is then a zenity window (class `zenity`) on `:99`. To fill it from `sh` steps:
- find it with `xdotool search --onlyvisible --class zenity`, activate it with `xdotool windowactivate --sync <id>`, and check that `xdotool getactivewindow` is that window before typing, so keys never land in another window;
- send real XTEST keys: `xdotool key` and `xdotool type` without `--window` (`--window` sends synthetic events, which GTK ignores);
- in an open dialog, `ctrl+l` opens the location field: type the path, then `Return`;
- in a save dialog the name field already has the focus: type the whole path there. A `Return` sent at once after `xdotool type` can be lost, so wait a second, send `Return`, and send it again only if a zenity window is still the active one;
- a dialog left open when the GUI exits keeps running: check `pgrep -a zenity` after a run and close leftovers, or the next search finds the stale window first.

SDL's zenity driver reports a cancelled dialog as one empty file name, which the GUI treats as a cancel.

On a normal desktop session without these settings, the portal's dialogs are GTK windows titled "Open File" (not class `zenity`): find one with `xdotool search --onlyvisible --name "^Open File$"`, check `xdotool getactivewindow` before typing, and grab only that window.
