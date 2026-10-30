# Base machine

How freeTVC models the TVC's main board: the clock and VRAM stretch, paging, I/O decode, interrupts, reset, the printer port, video, keyboard and joysticks, sound, the machine presets, and how the boot and the picture were checked against WinTVC 1.4.1. Every behaviour cites a section of [tvc-hardware.md](../hardware/tvc-hardware.md) (§n); where the sources disagree or are silent, the choice made is marked ❓. Expansion cards are in [cards.md](cards.md), program media in [media.md](media.md).

## Files

| Path | Content |
|---|---|
| `src/core/machine/config.hpp` | `Model` (32k, 32k + HBM, 64k, 64k+), `RomImages` (bytes, no file I/O), `CartType`, `CardConfig`, `SidCardConfig`, `MachineConfig` (+ colour switch, audio rate) |
| `src/core/machine/machine.*` | run loop, I/O decode, IRQ flip-flop, keyboard matrix with ghosting, printer port flip-flops, reset |
| `src/core/machine/debug_hooks.hpp` | what the machine reports to an attached debugger (flag maps per address, port and beam line) |
| `src/core/machine/os_call.hpp` | the OS-call hook at 0030h used by the host cassette ([media.md](media.md)) |
| `src/core/machine/tape_port.hpp` | what the machine tells an attached tape deck |
| `src/core/machine/printer_port.hpp` | what the machine hands an attached printer |
| `src/core/bus/memory_map.*` | RAM U0–U3, 4 VRAM banks, SYS/EXT/CART images, the page decoder, the card slots' IOMEM |
| `src/core/bus/video_stretch.hpp` | DEB phase and the two stretch tables |
| `src/core/video/mc6845.*` | the CRTC, stepped once per character |
| `src/core/video/video.*` | TVC generator: addressing, pixel modes, palette, border, blanking, beam, front buffer; the card overlay interface |
| `src/core/video/framebuffer.hpp` | the 800 × 314 indexed framebuffer, the RGB palette, the TV area |
| `src/core/audio/sound.*` | tone generator: 12-bit reloading counter, ÷16 chain, gates, amplitude, sound IRQ edges, TXRXCLK count |
| `src/core/audio/synth.*` | band-limited step synthesis to PCM (windowed sinc), AC-coupling high-pass, sample buffer |
| `src/core/input/keyboard.*` | matrix positions, key names, the character map (Unicode ↔ TVC code ↔ key + SHIFT) |
| `src/core/input/key_script.*` | timed key events: typed text, chords, applying them by frame |
| `src/core/snapshot/machine_state.cpp` | the machine's TVCS chunks ([media.md](media.md#snapshots)) |
| `src/frontend/common/presets.*` | presets and ROM files (front ends only) |
| `src/frontend/common/wav.*` | 16-bit mono WAV writer (headless `--wav`) |
| `src/frontend/sdl/host_input.*`, `keymap.*` | host keyboard (symbolic / positional) and gamepads → matrix; key layouts |

## Timing

- Machine time is in half-T ticks (160 ns, 6.25 MHz). A T-state is 2 ticks plus any stretch.
- **Lockstep:** after every `z80.h` tick the machine adds the tick's length, lets the video, the tone generator and the cards catch up to that time, and only then serves the bus request. A VRAM write or a palette, border or mode write therefore lands after every character already fetched.
- **DEB phase** = tick mod 4. Tick 0 at power-on is the start of a CPU slot (phase 0, DEB rising); the CRTC slot starts at phase 2. The real power-on phase is unknown ❓; the model is deterministic either way.
- **Character clock:** character k is fetched at tick 4k + 2 (its CRTC slot) and drawn then. One character is 4 ticks (2 T), one line of 100 characters 400 ticks (200 T).

### VRAM clock stretch (§2)

| Phase at T1 | 0 | 1 | 2 | 3 |
|---|---|---|---|---|
| Memory read/write, extra half-T | 2 | 1 | 4 | 3 |
| M1, extra half-T | 3 | 2 | 1 | 4 |

- Applies to a memory cycle (MREQ with RD or WR, not RFSH) whose address is in a page that maps VID: page 2 with 02h b5 = 0, or page 1 on the 64k+ with 02h b2 = 1, **whatever the bank**. WinTVC 1.4.1 instead adds a flat +1 T per access, and only when the page shows the same bank as page 2's VID selection (measured with probe programs under Wine).
- **T1 position:** `z80.h` raises the M1 pins in T1 and the MR/MW pins in T2 (`_z80_fetch` for M1; the memory-read steps are `_wait();_mread(...)` one tick after T1). So the phase is taken from the current tick for M1 and from the tick before for MR/MW. Verified by a per-tick trace of `LD A,(8000h)` from U0: the stretched tick runs from tick 22 to 26 of the instruction, which is T2 of the third MR. `core.bus` fails with the T2 phase (checked by mutating the code).
- The extra ticks are added to the tick that carries the request. The hardware stretches T3 (R/W) or T2 (M1), but nothing between those points is observable, so the following cycles see the same phase.
- Not stretched: I/O cycles, refresh cycles (❓ §2), INTA.
- **Not modelled:** the 32k/64k boards' slow-device M1 wait (§2 "WAIT circuit") and card `/WAIT`. No emulated card asserts `/WAIT`.
- Consequences (from the table, not measured on hardware): NOPs running from VRAM settle at 6 T each (phase 3, +4 half-T); `LD A,(8000h)` from RAM settles at 14 T, which happens to equal WinTVC's flat +1 T.

## Memory map (§1, §3)

| Port 02h field | 32k | 32k + HBM | 64k | 64k+ |
|---|---|---|---|---|
| page 0 `00` / `01` / `10` | SYS / CART / U0 | same | same | same |
| page 0 `11` | **nothing** (FFh) | U3 | U0 | U3 |
| page 1 b2 = 1 | U1 | U1 | U1 | VID, bank 0Fh b1–0 |
| page 2 b5 = 0 / 1 | VID / **nothing** | VID / U2 | VID / U2 | VID (bank 0Fh b3–2) / U2 |
| page 3 `00` / `01` / `10` / `11` | CART / SYS / **nothing** / IOMEM+EXT | CART / SYS / U3 / IOMEM+EXT | same | same |

- A bare 32k board has no U2/U3, so those selections read FFh and drop writes. WinTVC 1.4.1 instead keeps the old page pointer (U2) or gives a readable U3 (checked under Wine); that is an emulator's choice, not [HW].
- **IOMEM:** C000–DFFF belongs to the card in the slot chosen by 03h b7–6; an empty slot reads FFh and ignores writes. E000–FFFF is the EXT ROM. A 4 KiB EXT image (1.x) sits at F000–FFFF and E000–EFFF reads FFh: MAME's 8 KiB socket image of EXT 1.x is 4 KiB of FFh followed by the ROM. The front ends turn that socket image into the 4 KiB ROM (`normalise_ext()`), so every source gives the same bytes and CRC-32.
- A card can also serve 16 KiB in place of RAM unit U2 or U3 (the bus `/PHANTOM` line; the TVC256k++'s fast RAM). The page decoder asks the slots after every card access that can change it ([cards.md](cards.md)).
- **Video paging** 0Ch–0Fh is one register on the 64k+ ([HW] p.125: A2/A3 are not decoded; MAME, YATE and WinTVC decode only 0Fh). It is ignored on the other boards, which have one VRAM bank.
- ROM and empty areas: reads FFh, writes ignored. No cartridge = FFh. Cartridge boards and their registers are in [media.md](media.md#cartridges).
- `Machine::load()` and `ram()` address U0–U3 directly (64 KiB back to back), bypassing the decoder. `peek()`/`poke()` go through the current map without timing and without side effects (no cartridge paging).

## I/O (§4)

- Only A0–A7 are decoded. Ports 80h–FFh (A7 = 1) are not the machine's: they read FFh, except E0h–FFh when the SID card is in ([cards.md](cards.md)). INTA reads FFh (❓ §2).

Writes:

| Port | Effect |
|---|---|
| 00h | border colour; every card also sees the write (the TVC256k++ uses it) |
| 01h | printer data latch (see "Printer port") |
| 02h | page register |
| 03h | b7–6 IOMEM slot, b3–0 keyboard row |
| 04h | pitch b7–0 |
| 05h | b3–0 pitch b11–8, b4 tone gate, b5 sound IRQ gate, b7–6 tape motors (to the tape deck) |
| 06h | b1–0 video mode, b5–2 amplitude, b7 printer /STROBE |
| 07h | any value clears the IRQ flip-flop |
| 08h–0Bh | unused |
| 0Ch–0Fh | video paging (64k+) |
| 10h–4Fh | slot (port >> 4) − 1, its port & 0Fh; an empty slot ignores writes |
| 50h–57h | toggle the tape output flip-flop |
| 58h–5Bh (mirrored at 5Ch–5Fh) | slot (port & 3)'s /CLRINT strobe, with the data, to the card |
| 60h–6Fh | palette entry port & 3 |
| 70h–7Fh | even: CRTC address; odd: CRTC data |

Reads:

| Port | Value | Note |
|---|---|---|
| 10h–4Fh | the slot's card | an empty slot reads FFh |
| 50h–57h | FFh | reads toggle the tape flip-flop too ([HW] p.37) |
| 58h/5Ch | keyboard row (03h b3–0), active low, with ghosting (see "Keyboard and joysticks") | rows 10–11 are the GameCard's joysticks 3/4 when it is in; 12–15 read FFh |
| 59h/5Dh | b3–0 the four slots' /INT lines (0 = pending), b4 = 0 when the flip-flop is set, b5 tape input, b6 colour switch, b7 the printer's ACK flip-flop | idle with no card = DFh; WinTVC gave CFh with the cursor IRQ pending (checked under Wine), i.e. the same bits |
| 5Ah/5Eh | card IDs, 2 bits per slot (slot n in b2n+1..b2n) | 11 = empty or a card named by its own ROM header; FFh with no cards |
| 5Bh/5Fh | FFh ❓ | `INSTART`: reloads the sound counter and clears the ÷16 chain (see "Sound"); data undriven, WinTVC gives 00h |
| 71h (odd) | R14–R17 | MC6845: other registers read 00h. WinTVC also reads R12 (& 3Fh) back, HD46505-style; variant ❓ §5 |
| others | FFh | write-only ports and unused ports (WinTVC gives 00h) |

## Interrupts (§2)

- One flip-flop for cursor and sound; any write to 07h clears it. The CPU's /INT line is the flip-flop OR any slot's /INT line (HBA-2 schematic sheets 7 and 10), so a card's interrupt is level-triggered and stays until the card releases it.
- The board has no slot interrupt latch or enable. A write to 58h + n is slot n's /CLRINT strobe; what its data bits mean is the card's (the OS writes b7). The machine also keeps b7 per slot (saved in snapshots), but it gates nothing.
- **Cursor:** set on the character where the 6845 cursor output is active: MA = R14:R15, RA within R10–R11, cursor mode not "off", and display enable (as MAME's rising edge and WinTVC). With the ROM's values that is row 59, raster 3, column 63, once per frame, every 125 600 ticks (`core.video`).
- **Sound IRQ:** the 1→0 edge of the ÷16 chain's b3 while 05h b5 = 1 (see "Sound").
- At power-on the unprogrammed 6845 gives one cursor pulse at tick 2, so the flip-flop starts set; the ROM clears it. `core.sound` clears it with `OUT (07h)` first.

## Reset

- `power_on()`: CPU power-on state, RAM and VRAM cleared, every register and latch zero, keyboard released, clock at 0; the cards and the cartridge board power on too.
- `reset()` (button): CPU reset, ports 02h, 03h and 0Fh, 05h (both sound gates off), the IRQ flip-flop, the slot bits, the printer port (latch cleared, flip-flops set), the cards' reset line. RAM, the video state and the cartridge page are kept. Whether the 6845 sees the reset is unknown ❓; it keeps its registers.

## Printer port

The Centronics interface on the main board ([HW] §3.3.4 and p.133, [OS] ch. 12, MAME `tvc.cpp`):

- **01h** write: the data latch (C3, a 74LS273 ❓ type); reset clears it, so the printer sees NUL.
- **06h b7** write: the /STROBE flip-flop (B2/9); 0 = data valid. Reset sets it to 1. The printer's /ACK pulse sets it back to 1 by itself.
- **59h b7** read: the ACK flip-flop (B2/5). /STROBE low clears it; the rising edge at the end of the printer's /ACK pulse sets it; reset sets it ("READY after RESET", [HW] p.133).
- The ROM (SYS 2.2 `PAR_CHOUT` CE75h–CE9Dh, z80dasm disassembly): wait for 59h b7 = 1 (checking STOP through CAC0h), `DI`, `OUT (01h)`, 06h with b7 = 0 then 1 from the mirror 0B13h (about 20 T apart), `EI`. No printer interrupt: function 40h is `RET`.
- **No printer attached** (the default): /ACK never comes, so after the first character the flip-flop stays 0 and the next `PAR_CHOUT` waits until STOP, CTRL+ESC (error F5h; BASIC shows "STOP"). The OS manual describes the same wait for an off-line printer ([OS] 12.2.1). Whether the board's /ACK input has a pull-up is not in our sources ❓; an open input that floated could set the flip-flop by noise.
- **A printer attached**: the byte is taken on /STROBE's falling edge, and the printer's /ACK pulse ends 10 µs (63 ticks) later ❓ (YATE uses the same delay; real printers take from a few µs to a whole line). The core sees the printer through `PrinterPort` (`core/machine/printer_port.hpp`), an attachment outside the machine state like the tape port. The front end's printer writes raw bytes or UTF-8 text to a file (`frontend/common/printer_host.*`).
- State: chunk `LPT ` (strobe, ACK flip-flop, the pending /ACK); the latch is in `BUS `. Snapshots without `LPT ` load with both flip-flops set.
- Front ends (`--printer FILE`, the Cards menu's printer items, monitor `printer`, MCP `printer_*`) and checks: [cards.md](cards.md), "Serial card".

## Video (§5)

### 6845

- Write masks per the MC6845 data sheet; R16/R17 (light pen) are read-only.
- Each step outputs the current character (MA, RA, display enable, HSYNC, VSYNC, cursor) and then advances.
- End of line at h = R0. Display enable switches off at h = R1. On the last raster of a row the MA at h = R1 is latched as the next row's start. HSYNC starts at h = R2 for R3 b3–0 characters (0 = no pulse ❓).
- Rows end at RA = R9; after row R4 come R5 adjust lines, then a new frame (start address R12:R13). Vertical display ends at the start of row R6, and VSYNC starts at the start of row R7 for 16 lines (fixed on the MC6845; R3 b7–4 ignored).
- Cursor blink modes (R10 b6–5): 00 steady, 01 off, 10/11 half of a 16/32-field period.
- Not modelled: interlace (R8), skew, light pen.
- With the ROM's values the frame is (77 + 1) × 4 + 2 = 314 lines × 100 characters = **62 800 T** (49.76 Hz; `core.video`: frame flips 125 600 ticks apart).

### Generator

- VRAM address = `(MA & 3Fh) | (RA & 3) << 6 | ((MA >> 6) & 3Fh) << 8` in the displayed bank (0Fh b5–4 on the 64k+).
- Modes (06h b1–0): 2-colour (bit 7 leftmost, palette 0/1); 4-colour (pixel k = palette[bit 3−k : bit 7−k], two dots wide); 16-colour (I G R B from b7 b5 b3 b1, then b6 b4 b2 b0, four dots each).
- Palette 60h–63h `-I-G-R-B`; border 00h `I-G-R-B-`; stored as IGRB indexes 0–15.
- **Blanking:** the VSYNC start sets a blank flip-flop, and MA9 rising clears it. With the ROM's values MA9 rises at h = 64 of the last raster of row 71, so the first border line (y 19) shows border only from x 712 (a step at the top border). HSYNC characters are also blank: the real pulse width comes from a 74LS123 ❓.
- Otherwise: display enable → pixels, else the border colour.
- **Card overlay:** a card that drives the colour bus (the TVC256k++) is called for every character with display enable, VSYNC, the mode and the 8 pixels while the beam draws paper (`VideoOverlay` in `video.hpp`; [cards.md](cards.md), "Overlay").
- `Video::decode()` turns any VRAM byte into pixels for a mode and palette; the Bench's Video panel uses the same function ([ui.md](ui.md)).

### Framebuffer and beam

- `Framebuffer`: 800 × 314 indexes, 0–15 colours plus 16 = blank (100 characters × 8 pixels at the 12.5 MHz 2-colour dot clock). `kRgbPalette`: component = 146 (I = 0) or 255 (I = 1), after [OS] ❓ levels. The GUI's picture model turns the indexes into a PAL signal or a monitor picture ([ui.md](ui.md), "Picture").
- The picture is stored **as a TV beam would trace it**, not in CRTC coordinates: x counts from the start of HSYNC, y from the first HSYNC after a VSYNC began. Front ends crop and scale.
- The beam follows a sync pulse only inside a capture window: HSYNC after ≥ 90 characters (free-run at 104), VSYNC after ≥ 250 lines (free-run at 330). Characters and lines beyond 100 × 314 are not stored. This is a TV model, not hardware; it keeps an unprogrammed or odd CRTC from producing thousands of frames.
- Positions with the ROM's values: display character (0, 0) at **x 200, y 49**; lines 0–18 are blank, line 19 partly, then border; x 0–15 (HSYNC, R3 = 2) is blank on every line.
- **TV area** (`kTvArea`): 650 × 287 from x 131, y 23, what a PAL TV shows (52 µs of each line from 10.5 µs after the HSYNC edge, lines 23–309). The GUI shows it, or the whole raster in its "Full raster" debug view; monitor and MCP screenshots use the same areas.
- Frame flip: at the line start that begins a new frame, the working buffer is copied to the front buffer and `frame_count()` increments. `Machine::framebuffer()` always returns a complete frame.

## Keyboard and joysticks (§7)

### Matrix (core)

- `Machine::set_key(row, column, down)` holds a switch, and `key_down()` reads it back. Joystick 1 is row 8, joystick 2 is row 9. `joystick_key(n, JoyLine)` names the lines: up b1, down b2, fire b3, accel b4, right b5, left b6. The GameCard's joysticks 3 and 4 are rows 10 and 11 ([cards.md](cards.md), "GameCard").
- **Ghosting:** the switches have no diodes ([OS] p.87). Reading 58h starts from the selected row and follows pressed keys alternately to columns and rows until nothing new is reached. All reached columns read 0. This reproduces the OS manual's example: A, D and E held also read Q (`core.input`). Two keys never ghost.
- Port 58h with a row of 12–15 reads FFh (no decoder output).

### Characters and names (core)

- `char_keys()`: 115 characters with their TVC code, key and SHIFT state, LOCK off. That is printable ASCII, the 18 accented letters, RETURN (`\r`) and ESC. Checked against the key tables in every SYS ROM in both directions (`input.keymap_rom`; [tvc-hardware.md](../hardware/tvc-hardware.md) §7 "ROM key tables").
- `find_key(name)`: the unshifted legend ("A", "5", "Á" or "á", ";") or a word: SPACE, RETURN/ENTER, DEL, INS, ESC/ESCAPE, SHIFT, LOCK, CTRL/CONTROL, ALT, UP, DOWN, LEFT, RIGHT, FIRE, ACCEL, JOY2_UP … JOY2_LEFT, JOY3_*/JOY4_*.
- `type_text(text, frame, timing)`: text to key events. A shifted character gets SHIFT one frame before its key and releases it one frame after, so no scan sees the key with the wrong SHIFT. Defaults: 2 frames held, 2 frames apart, **25 frames after RETURN**. BASIC drops keys while it handles a line; 10 frames lost a key after storing a 47-character line, 15 did not. A command that runs longer needs its next text scheduled later. Every front end types with these fixed delays (MCP `type_text` included); `input.typing.<preset>` types every character of the map at the BASIC prompt and reads it back from the OS screen copy.

### Host input (SDL front end)

- `--keymap symbolic` (default):
  - The character the host key gives under the host's own layout, with SHIFT and AltGr (SDL `SDL_GetKeyFromScancode`), is typed on the TVC.
  - SHIFT is forced to what that character needs while the key is down. So US "=" (unshifted) presses SHIFT+7, and US SHIFT+6 "^" releases SHIFT.
  - Letters keep the user's SHIFT, so the TVC's LOCK keeps working. Host CapsLock is the TVC LOCK key.
  - Characters the TVC lacks are ignored.
  - SDL3 does not apply modifiers to event keycodes by default, which is why the mapping asks for the shifted character itself.
- `--keymap positional`: every host key follows the **key layout** ([ui.md](ui.md), "Keyboard editor"): `--keys NAME|FILE` or `keys` in `freetvc.ini`, by default `hungarian` (`data/keymaps/hungarian.keys`, equal to the built-in table, checked by `ui.keymap`); `us` puts digits and letters on their US legends. The editor saves a changed layout as a copy in the user data folder. The `hungarian` layout:
  - Each host key is the TVC key with the same legend on a Hungarian PC keyboard, since the TVC layout is a Hungarian typewriter layout. Grave = 0, Digit0 = Ö, Minus = Ü, Equal = Ó, [ = Ő, ] = Ú, ; = É, ' = Á, \ = Ű, the 102nd key = Í, / = "-", and the US "Y" position = Z.
  - The eight TVC keys a PC lacks are on **F1–F8** in TVC order: @ ; < \ [ ] * ^.
  - Right Alt is ALT, like the TVC's ALT to the right of SPACE.
- Keys that type no character follow the layout in both modes: in symbolic mode a key goes by its character when it types one, else by the layout, so F1–F8 work there too; AltGr only composes characters in symbolic mode. In `hungarian`:
  - Enter / keypad Enter = RETURN;
  - Backspace = DEL;
  - Delete = SHIFT+DEL;
  - Esc = ESC, Insert = INS;
  - Shift = SHIFT, Caps Lock = LOCK;
  - left Ctrl = CTRL, left Alt = ALT.
- Joysticks:
  - The arrows are row 8, which is the TVC's cursor keys and built-in stick, and Right Ctrl is joystick 1 fire.
  - Keypad 8/2/4/6 is joystick 2, with keypad 0 as its fire.
  - Gamepads 1 and 2 are joysticks 1 and 2, gamepads 3 and 4 the GameCard's joysticks 3 and 4: d-pad or left stick (half deflection) for directions, A (south) = fire, B (east) = accel.
- Ordering: SHIFT changes get a frame period (62 800 T of machine time) of their own, after releases and before presses. Machine time rather than frame flips, because an unprogrammed CRTC never flips a frame (found by `input.host`). Window focus loss releases every key.
- Timing: the ROM's keyboard interrupt registers a key only on its press edge, one new key per scan, and only after the editor has taken the previous one ([tvc-hardware.md](../hardware/tvc-hardware.md), "Editor behaviour"). So new presses reach the matrix **one per frame period, in press order**; each key stays down for at least a frame period, and a key tapped again waits until it has been up for a frame period. Taps shorter than a frame (paste and macro tools, `xdotool type`, a GUI frame that took long) therefore arrive complete and in order; a chord's second key starts one frame (20 ms) after the first. Test `input.host` ("quick taps"); checked in the GUI with `xdotool type` at about 10 ms per key (`print 112233`, a 19-character program line, `print 6+7`).
- Tests use SDL's built-in US keymap only; other host layouts are not tested automatically.

### Headless

- `--key KEY[+KEY]@F[:H]`: KEY is ROW:COL or a name; H defaults to 5 frames.
- `--joy N:DIR[+DIR]@F[:H]`: N = 1–4.
- `--type TEXT@F`: `\n` is RETURN; UTF-8 accents are accepted.
- `--type-timing H:G[:R]` (default 2:2:25).
- Every event is sorted by frame and applied at the start of that frame's slice.

## Sound (§6)

### Generator (core)

- **Counter:** 12 bits, clocked at 3.125 MHz on even half-T ticks (tick 0 = power-on), so stretched CPU cycles do not skew it. It counts up from P and reloads from P when full, so one reload takes 4096 − P T.
  - A new P is loaded at the next reload; it never cuts the current count short.
- **Chain:** ÷16. Its b0 is the ÷2 stage, the bus TXRXCLK that clocks the serial card (`Sound::txc_edges()`); its b3 is the tone and the IRQ source. f = 195 312.5 / (4096 − P) Hz.
- **`INSTART`** (read 5Bh/5Fh): the counter restarts from P on the next clock edge and the chain clears. The sound IRQ comes on b3's 1→0 edge, so the first one comes 16·(4096 − P) T later, then one every 16·(4096 − P) T. Evidence: the ROM's tape writer (EXT 1.x FA4Fh–FA78h) produces the documented cycle lengths only with a full period first, checked by a recorded `SAVE` on 2.2 and 1.2 (`media.tape.*`); WinTVC 1.4.1 also gives the first IRQ one full period after `INSTART` (measured under Wine).
- **P = 4095** stops the oscillator ([HW] p.128): no reloads, no IRQ, the chain keeps its state. `INSTART` does not restart it.
- **Output level:** 0 while 05h b4 = 1 and b3 = 0; otherwise the amplitude (06h b5–2), so with b4 = 0 port 06h is a 4-bit DAC.
- **Speed:** when both gates are off nothing is observable, so whole reloads are skipped at once (the TXRXCLK count still advances). Otherwise the core steps reload by reload; the worst case is P = 4094, about 31 400 steps per frame.
- **❓ defaults (no hardware evidence either way):**
  - Turning on 05h b5 gives no IRQ at once, only at b3's next 1→0 edge; the literal gate logic might give an edge at once.
  - Leaving P = 4095 restarts the count from the new P on the next clock edge.
  - Power-on: counter and chain at 0, pitch 0, gates off, amplitude 0. Reset clears 05h (both gates off) and keeps the counter running.
  - The 16 amplitude steps are linear, because [HW] calls them "nearly evenly spaced" (p.102). EP's table ([tvc-hardware.md](../hardware/tvc-hardware.md) §6) is the alternative.
  - The 20 Hz high-pass stands in for AC coupling in the TV/monitor path; no component values are known.

### Output (core)

- `AudioSynth` gets every level change as a timed step and outputs 48 kHz mono float (`MachineConfig::audio_rate`; 0 = none):
  - **steps:** band-limited, from a windowed-sinc impulse (Blackman, 32 taps, 64 sub-sample phases, cutoff 0.41 × rate ≈ 19.7 kHz), taken mid-sample, with each phase normalised to sum 1, so DC levels are exact;
  - **latency:** 15.5 samples;
  - **ringing:** about 11 % at the sample points, near the cutoff, which any steep band-limited edge has (`core.sound`);
  - **gain:** amplitude 15 = 0.75 before the high-pass, so the TVC's own tone stays inside ±0.84.
- **Other sources:** the sound chips on cards (SN76489, SAA1099, YM3812, the vSID, the SID card) add their own steps to the same synthesis (`AudioSynth::add_step`), the bus SOUND line; each card brings its chips up to the end of a run before the samples are released ([cards.md](cards.md)). The tape signal is not mixed into the sound.
- A sample becomes readable once no later step can reach it; `Machine::run()` releases everything up to its end. At most 2 s is kept (oldest dropped and counted), so a front end that never reads cannot grow memory.
- The output is deterministic: the headless WAV is byte-identical for gcc debug/release, clang and MinGW (run under Wine).

### OS use (dynamic, freeTVC; `--type`, every preset)

- PCs below are the addresses right after the `OUT` that made the change.
- At boot EXT sets P = FAFh with the tone gate on and amplitude 0, so the output is silent: EXT 2.x at E304h/E307h (frame 246), EXT 1.x at F2D6h/F2D9h (frame 244 on 64k-1.2 and 32k+hbm-1.2, frame 159 on 32k-1.2, whose RAM test is shorter).
- `SOUND PITCH 3950,VOLUME 15,DURATION 50` writes the amplitude, then P = F6Eh: `PITCH` is the raw P. Then it turns the tone on (2.x: EXT FFA0h, FFB3h; 1.2: SYS D996h, D9AAh, matching [rom.md](../hardware/rom.md) §11: sound is in SYS in 1.x).
  - About 50 cursor ticks later (0.989 s) it clears the tone gate and leaves the amplitude at 15 (2.x: FF6Bh; 1.2: D95Fh), so the output steps to a DC level: an audible click. By the §6 model the hardware does the same (not checked on hardware ❓).
  - `INSTART` and the sound IRQ are not used. Source: a per-instruction trace of the sound state (`tools/sound/soundtrace.cpp`) with the PCs above. ROM disassembly not cross-checked ❓.
- `sound.basic.<preset>` types that line and checks the output: silence before, one burst of 50 OS ticks, period 16 × (4096 − 3950) T = 35.881 samples at 48 kHz.
- WinTVC 1.4.1's tone period, measured under Wine, follows 4095 − P instead of 4096 − P, so it is not a pitch reference; the sound IRQ and DAC timing have no hardware reference.

### Front ends

- **Headless:** `--wav FILE` writes the whole run, 48 kHz 16-bit mono.
- **GUI:** an SDL3 audio stream, F32 mono at 48 kHz, on the default playback device.
  - Each loop pass runs the machine until about 60 ms of samples wait in the stream, so the device's clock paces the emulation.
  - `--no-audio`, or a device that fails to open, falls back to wall-clock pacing; samples are read and dropped.
  - On Linux SDL3 uses its native PipeWire backend when `libpipewire-0.3` development files are present at build time, else PulseAudio (also through pipewire-pulse).
  - **Known issue (PulseAudio path):** on a low-end 2-core PC, with SDL's PulseAudio backend through pipewire-pulse and a 768-frame device buffer, 4 of 10 runs that grabbed the window image (`XGetImage`) during a tone lost one or two PipeWire quanta (256 frames each, filled with zeros), although freeTVC's queue never fell below 10 ms. Without grabs 4 of 4 runs were clean; a 1536-frame device buffer still dropped once in 4; `pw-play` of the same samples under grabs was clean 3 of 3; `PULSE_LATENCY_MSEC=60` was clean 5 of 5 (latency not measured). So the drops sit in the PulseAudio path under load. With the native PipeWire backend a recorded tone showed no dropouts; that path has not been tested under the same load ❓.

## Presets

A preset is a board (`Model`) plus a SYS and an EXT ROM (`src/frontend/common/presets.*`). They follow WinTVC 1.4.1's machine presets 0–2, 4 and 5:

| freeTVC preset | Board | SYS | EXT | WinTVC 1.4.1 preset |
|---|---|---|---|---|
| `32k-1.2` | 32k (HBA: U0, U1) | `sys12.bin` (BASIC 1.2) | `ext1x.bin` (4 KiB at F000h) | 0, TVC 32 / BASIC 1.2 |
| `32k+hbm-1.2` | 32k + HBM 32 KiB card (U2, U3; U3 can map to page 0) | `sys12.bin` | `ext1x.bin` | 1, TVC 32 + 32 / BASIC 1.2 |
| `64k-1.2` | 64k (HBA-1) | `sys12.bin` | `ext1x.bin` | 2, TVC 64 / BASIC 1.2 |
| `64k+-2.1` | 64k+ (HBA-2: 4 VRAM banks) | `sys21.bin` (BASIC 2.1) | `ext2x.bin` (8 KiB at E000h) | 4, TVC 64+ / BASIC 2.1 |
| `64k+-2.2` | 64k+ | `sys22.bin` (BASIC 2.2) | `ext2x.bin` | 5, TVC 64+ / BASIC 2.2 |

- WinTVC's preset 3, "TVC 64+ / Cirill BASIC 1.2", is left out: WinTVC builds it by patching SYS 1.2 (323 bytes: keyboard tables and code) and EXT 1.x (1016 bytes: Cyrillic character bitmaps and code in the empty E000h–E521h, plus a few bytes near F00Fh and F542h), and runs it on a 64k board, not a 64k+ (static analysis of WinTVC 1.4.1, the running SYS image compared under Wine). WinTVC embeds no Cyrillic ROM image (none of MAME's `tvcru_*` images matches any part of its program file).
- With its `FastBoot` option on, WinTVC patches a few SYS bytes on presets 2 and 5 to shorten the boot (17 bytes at 0275h–029Ah and 1A19h–1A1Bh on preset 2, one at 0F21h on preset 5); with it off, the running images equal the embedded ones (compared under Wine). freeTVC never patches a ROM.
- ROMs are found by file name or by CRC-32 under any name, in the ROM search folders ([ui.md](ui.md), "Settings file and ROM search"; [build.md](build.md), "ROM autofetch"). Known images: `sys12.bin` 6DA8809A, `sys21.bin` 53C8FF98, `sys22.bin` 79FA818C, `ext1x.bin` 59C6B9F9 (4 KiB) or 1CBBEAC6 (the 8 KiB socket image), `ext2x.bin` 05E1C3A8.
- `--model` with `--sys`/`--ext` builds a machine outside the presets.

## Boot results

- All five presets reach the BASIC prompt (`boot.<preset>`). Evidence: the OS screen copy at 0100h and screenshots read back.

| Preset | Banner (screen copy rows 0, 3) |
|---|---|
| `32k-1.2` | TV COMPUTER BASIC 1.2, 25871 bytes free |
| `32k+hbm-1.2`, `64k-1.2` | TV COMPUTER BASIC 1.2, 42255 bytes free |
| `64k+-2.1` | TV COMPUTER BASIC 2.1, 42155 bytes free |
| `64k+-2.2` | TV COMPUTER BASIC 2.2, 42155 bytes free |

- **Start-up logo:** after the RAM test the ROM shows the animated VIDEOTON / TV COMPUTER logo and waits for a key. `boot_test` presses SPACE (row 7, column 5) at frame 330 for 5 frames; without the key the screen copy stays empty after 450 frames.
- **Boot time:** the IRQ stub's `JP` at 003Dh is written at 15.36 M T (64k, SYS 1.2), 15.49 M T (64k+, 2.2) and 10.02 M T (32k). The jump targets match WinTVC 1.4.1 (C412h on SYS 1.2, C405h on 2.2).
  - The time fits the cold-start RAM tests ([rom.md](../hardware/rom.md) §3, steps 5–8; the stubs are copied in step 10). Each 16 KiB test is an LDIR fill (21 T per byte) plus the compare loop at C367h (`DEC (HL)`, `CPI`, `DEC HL`, `LD (HL),0`, `RET NZ`, `INC HL`, `RET PO`, `JR`: 71 T per byte), for AAh and then 55h: about 3.0 M T per page. A 64k tests U0, VRAM, U1, U2, U3: about 15.1 M T plus the VRAM stretch (source: SYS 2.2 disassembly, C34Dh–C370h, and a freeTVC run).
  - WinTVC 1.4.1 with `FastBoot` off writes the 64k stub between 15.256 M and 15.313 M T (its memory polled under Wine about once per emulated 62.8 k T); with `FastBoot` on it gets there after about 2 M T.
- **Against WinTVC 1.4.1 (64k-1.2, `FastBoot` off):** WinTVC's T counter and registers were read about once per 62.8 k T through the boot (under Wine, from its process memory), and each state (HL, DE, BC, A) was looked up in a freeTVC run of the same preset:

| Phase | T | WinTVC T − freeTVC T at the same state |
|---|---|---|
| U0 RAM test | 0 – 3.0 M | −8 … +5 (one instruction) |
| VRAM test (8000h) | 3.0 – 6.3 M | grows steadily to −65.5 k |
| U1, U2, U3 tests | 6.3 – 15.2 M | −65.5 k, constant within 60 T |
| after the RAM tests | 15.3 M – | +120 k … +220 k: WinTVC runs its default disk card's init |

  - So the CPU timing outside VRAM matches WinTVC over 12 M T. The whole gap comes from the VRAM test: about 196 600 VRAM accesses (2 passes × 16 KiB × about 6 accesses per byte: LDIR read + write, `DEC (HL)` read + write, `CPI` read, `LD (HL),0` write), so freeTVC stretches about **+0.33 T more per access** than WinTVC's flat +1 T. That is the difference between the 160 ns phase model (§2) and WinTVC's flat model (its own notes call its stretch "not fully accurate"); which matches the hardware needs a hardware measurement (§2 ❓).
  - 15.36 M T (freeTVC) − 65.5 k = 15.29 M T, inside WinTVC's stub bracket above.
  - After the RAM tests WinTVC executes the HBF card ROM's init, because its default configuration has the disk card in slot 0: an LDIR at C244h over D1DEh–DD8Eh, which is `LD BC,0FF2h / LD (HL),L / LDIR` in `hbf.rom` at offset 0240h. freeTVC's default has no card, so the comparison ends at the RAM tests; a comparison past them with both machines configured alike has not been made.

## Golden frames

- **Method:** a capture harness (not part of the repository) pauses WinTVC 1.4.1 under Wine where the beam is past the visible rows (the Pause key stops its T counter), saves its F2 framebuffer GIF and dumps the displayed VRAM bank, CRTC R0–R15 and the decoded border, mode and palette from its memory. `golden_test` rebuilds that state in freeTVC, renders it and compares colour indexes. The F2 GIF's indexes are IGRB colour numbers, so WinTVC's TV colour settings do not matter.
- **Data:** `testdata/golden/<preset>/<scene>.{json,vram,pgm}` (state dump, 16 KiB VRAM, the GIF as colour indexes), found through the CMake cache variable `FREETVC_GOLDEN_DIR`. The captures are not distributed; without them the `golden.*` tests are skipped (exit code 77).
- **Result:** 16 scenes (logo, BASIC prompt, `GRAPHICS 2`, `GRAPHICS 16` on 32k-1.2, 32k+hbm-1.2, 64k-1.2, 64k+-2.2) match WinTVC **pixel for pixel**: 640 × 285 visible pixels each, 2-, 4- and 16-colour modes, border and palette. WinTVC's visible area is freeTVC's raster from x 136, y 24, so display (0, 0) is at its (64, 25).
- **The logo race:** while the logo waits for a key, the OS rewrites the "TV COMPUTER" letter colours (character rows 42–44) in every frame, at a beam line that changes from frame to frame (SYS 1.2 from CAD2h, 2.2 from F309h; traced in freeTVC with a VRAM write tracer, not cross-checked in the disassembly ❓). A write after the beam passed those rows but before the pause is in the dumped VRAM and not in the GIF. Of 11 logo captures paused past the visible rows, 7 showed this (8–38 pixels), always inside GIF rows 193–204 and nowhere else, so `golden.<preset>.logo` reports those rows but does not judge them (`--skip-rows 193-204`). The same race exists on hardware: which colour a row shows depends on the beam.
- **Not covered:**
  - The 16-colour scenes hold only colours 0 and 12; drawing all 16 needs shifted characters, which the capture harness could not type.
  - 64k+-2.1: WinTVC 1.4.1's preset 4 does not reach BASIC in our tests: a yellow box (palette 0 = 14) replaces the logo, SPACE leads nowhere, and after 30 s its PC was at F3FCh (two runs). That preset also applies EXT 1.x tape-hook patches inside the EXT 2.x image; whether that causes it was not traced ❓. freeTVC's `64k+-2.1` shows the logo and boots.
  - Timing-dependent pictures (raster effects, mid-frame changes): these need a full-run comparison, which the boot gap above rules out.

## Known gaps

- VRAM stretch: freeTVC averages about 1.33 T per VRAM access in the boot's RAM test, WinTVC 1 T; no hardware measurement decides it (§2 ❓). The slow-device M1 wait and card `/WAIT` are not modelled.
- Golden frames: no 16-colour scene with all 16 colours; no boot comparison past the RAM tests.
- Sound: the ROM `SOUND` addresses come from a dynamic trace only (❓); the sound IRQ and DAC timing have no hardware reference; the PulseAudio dropouts above.
- Input: the CTRL/ALT key tables (❓ which is which) and the Cyrillic matrix are not used; typing has fixed delays instead of waiting for the OS to take each key.
- The MSVC build and a native Windows GUI have not been run (the MinGW build runs under Wine).
