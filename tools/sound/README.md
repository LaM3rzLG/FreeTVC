# tools/sound: sound checks

Scripts that check freeTVC's sound end to end: the TVC tone generator through the real GUI audio path, recordings analysed per cycle, a trace of the sound registers, and the SID card. The sound model is described in [machine.md](../../docs/design/machine.md) (section "Sound") and the SID card in [cards.md](../../docs/design/cards.md) (section "SID card").

Run everything from the project root. The Python scripts run with `python3 -I`; `gui_sound.py` and `grab_load.py` need python-xlib and Pillow, `sid_check.py` needs NumPy, the others only the standard library. ROMs come from the usual ROM search: set `FREETVC_ROM_DIR` (for example `$PWD/testdata/roms`) or build with `-DFREETVC_FETCH_ROMS=ON`.

| File | Does |
|---|---|
| `gui_sound.py <freetvc> <out dir> [line]` | starts the `freetvc` GUI, records the sound output with `parec` into `<out dir>/gui_rec.wav` (48 kHz, mono, 16 bit), presses SPACE past the start-up logo, types the line (default `SOUND PITCH 3950,VOLUME 15,DURATION 50`) and RETURN with XTEST keys, and saves five window grabs, `gui_1_logo.png` to `gui_5_after.png`. Prints the times of typing and RETURN |
| `wavinfo.py <wav>` | length, peaks, and each tone burst (10 ms RMS windows above 0.01) with its length, mean frequency and peak to peak. The mean includes the attack, so it reads about 0.3 % high; use `cycles.py` for the pitch |
| `cycles.py <wav>` | the first to last tone window with attack and release trimmed: cycle count, minimum, maximum and mean cycle length in samples, and gaps longer than 40 samples (dropouts). `PITCH 3950` at 48 kHz is 35.881 samples per cycle |
| `grab_load.py <seconds>` | background load for dropout experiments: a 960 × 660 root-window `XGetImage` every 0.35 s |
| `sid_check.py <freetvc-headless> <out dir> [6581\|8580]` | the SID card check, see below |
| `probes/sid.asm`, `probes/common.inc` | the SID card's test program for `sid_check.py` (sjasmplus) |
| `soundtrace.cpp` | boots a preset, types a line (`\|` = RETURN), and prints every change of the tone generator's pitch register P, the amplitude, the tone and interrupt enables, and every `INSTART`, with the PC after the instruction |

## gui_sound.py

```sh
python3 -I tools/sound/gui_sound.py build/gcc-release/src/freetvc <out dir>
python3 -I tools/sound/cycles.py <out dir>/gui_rec.wav
```

Environment:
- `EXTRA`: more `freetvc` arguments;
- `NOGRAB=1`: no window grab while the tone plays (a grab is load that can cause dropouts);
- `MONITOR`: the `parec` source, default `@DEFAULT_MONITOR@`, the monitor of the default sink.

Requirements and quirks:
- It records the real sound output, so it needs a PulseAudio or PipeWire playback sink and `parec`. If no default sink is set, pass the sink's monitor in `MONITOR` (`pactl list short sources` lists the sources).
- Keys go only to a focused freeTVC window: the script activates it with `wmctrl` and checks `_NET_ACTIVE_WINDOW` before every key, so it needs a window manager.
- Typing: upper case letters get Shift; other characters are sent as the bare key, so use letters, digits, space, `,` and `.`.
- It does not set `XDG_DATA_HOME`. The GUI keeps `freetvc.ini` and `layout.ini` in its user data folder, so set `XDG_DATA_HOME=<out dir>/xdg` to keep your own settings out of the run.
- The printed times count from the emulator's start plus 0.5 s. The real `parec` start offset is not measured, so they do not give the audio latency.

For a `PITCH 3950` tone, `cycles.py` should report a mean cycle of 35.881 samples and no gaps.

## sid_check.py

Assembles `probes/sid.asm` with sjasmplus, runs it in `freetvc-headless` with `--sid-card <model>` (loaded at its own address with `--load`, started with `--pc`, no ROM calls and interrupts off), and analyses the recording:
- OSC3 and ENV3 as the program read them at the end (a monitor memory dump line);
- the pitch of the three notes (sawtooth A4, pulse C5, triangle E5) by autocorrelation, in Hz and cents against the expected value (zero crossings misread the 6581's sawtooth);
- the RMS loudness per 100 ms.

The WAV is kept as `<out dir>/sid-<model>.wav`; the model defaults to 6581. sjasmplus 1.24.0 must be at `.tools/sjasmplus-1.24.0/bin/sjasmplus` under the project root (see [tools/dezog/README.md](../dezog/README.md#prerequisites)).

```sh
FREETVC_ROM_DIR=$PWD/testdata/roms python3 -I tools/sound/sid_check.py build/gcc-release/src/freetvc-headless <out dir>
```

Expected output (6581):

```text
OSC3, ENV3: >C:6802  59 25 12 87 70 4C 8A 9C  00 1A 35 50 6A 85 A0 BA  Y%..pL....5Pj...
6.60 s at 48000 Hz, first sound at 0.000 s, peak 32571
sawtooth A4: 440.09 Hz (expected 440.00, +0.4 cents)
pulse C5: 523.09 Hz (expected 523.25, -0.5 cents)
triangle E5: 659.30 Hz (expected 659.26, +0.1 cents)
RMS per 100 ms: 2173 1151 580 314 ...
```

`probes/sid.asm` plays about 7 s of notes through ports E0h–FFh and starts when its `GO` byte is non-zero. The same program was run in WinTVC 1.4.1 under Wine for comparison (WinTVC's SID ports read FFh, so OSC3 and ENV3 are only meaningful in freeTVC): the same pitches, envelopes correlating at 0.93, and one level factor between the two.

## soundtrace.cpp

A small program linked against an existing build's libraries (here `gcc-release`); build it outside the source tree:

```sh
B=build/gcc-release
g++ -std=c++20 -O1 -I src -I $B/src/generated tools/sound/soundtrace.cpp \
    $B/src/libfreetvc_frontend_common.a $B/src/libfreetvc_debug.a $B/src/libfreetvc_core.a \
    $B/third_party/libfreetvc_fatfs.a $B/third_party/libfreetvc_littlefs.a \
    $B/third_party/libfreetvc_resid.a $B/third_party/libfreetvc_ymfm.a -o <scratch>/soundtrace
FREETVC_ROM_DIR=testdata/roms <scratch>/soundtrace 64k+-2.2 'SOUND PITCH 3950,VOLUME 15,DURATION 50|' 700
```

Arguments: the preset name, the line to type, and the number of frames to run (default 700). SPACE is pressed at frame 330 to leave the start-up logo and typing starts at frame 400. The ROMs come from `FREETVC_ROM_DIR` (default `testdata/roms`). Each output line is the time in seconds, the frame, the PC, then `P`, `amp`, `tone`, `irq`, `chain`, and `INSTART` when the generator was restarted with no reload due:

```text
   12.3059 s  f611 pc FFA0  P FAF amp 15 tone 1 irq 0 chain 10
   12.3060 s  f611 pc FFB3  P F6E amp 15 tone 1 irq 0 chain 11
   13.2949 s  f661 pc FF6B  P F6E amp 15 tone 0 irq 0 chain 11
```
