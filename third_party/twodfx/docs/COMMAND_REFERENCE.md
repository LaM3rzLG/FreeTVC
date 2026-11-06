# 2dfx Command Reference Guide

This guide documents the public F8h/F9h command interface in **2dfx v1.1.0**, including its 3D extension.
This document specifies the public command interface; no private firmware source is supplied.

## 1. General model

2dfx receives command bytes on port F8h and, when required, parameter/data bytes on port F9h.

Public command families:

- `00h..3Fh`: select sprite and enter sequential SPB write session
- `40h..4Fh`: transparent colour select
- `50h..5Fh`: engine, timing, raster interrupt and resource-definition commands
- `60h..7Fh`: background 2DPT slot commands
- `80h..85h`: resource upload and ZX1 decompression/conversion commands
- `86h..88h`: 3D mesh definition, projection and frame-latched camera control
- `8Eh`: terminal `SHOW_FW` firmware-information page
- `8Fh`: asynchronous `SYS_RESET`
- `90h..91h`: built-in 2dfx logo overlay commands
- `C0h..DFh`: firmware eggs / demos
- `E0h..FFh`: foreground 2DPT slot commands (same commands as `60h..7Fh`, with bit7 set)

Resource RAM is an 8 MiB logical resource address space using 23-bit addresses.
Address byte order is always little-endian LSB-first:

- byte 0 = bits 7..0
- byte 1 = bits 15..8
- byte 2 = bits 22..16 in bits 6..0, bit7 ignored

During an active `80h..82h` upload or `83h..85h` UNZX1 argument session,
ordinary F8h commands are ignored so payload/argument bytes cannot accidentally
start another command. `8Eh SHOW_FW` follows the same strict-session rule.
`8Fh SYS_RESET` is the deliberate exception and is recognized while such a
transaction is active. Once `SHOW_FW` has been accepted, however, all later
F8h/F9h writes—including `SYS_RESET`—are ignored until physical reset or power
cycle.

## 2. Sprite commands and SPB format

Any command `00ssssssb` with bit7..bit6 = `00` selects sprite `s` (`0..63`) and starts a sequential 9-byte SPB write session.
Each subsequent F9h byte writes the next SPB byte immediately. Partial SPB updates are valid and supported.

9-byte SPB layout:

- `BYTE0`: enable, collision enable, X/Y/W/H high bits
- `BYTE1`: X low
- `BYTE2`: Y low
- `BYTE3`: resource address low
- `BYTE4`: resource address mid
- `BYTE5`: resource address high (bits 22..16 in bits 6..0)
- `BYTE6`: sprite flags
- `BYTE7`: width low
- `BYTE8`: height low

Sprite flag bits in `BYTE6`:

- bit7 = X double
- bit6 = Y double
- bit5 = X flip
- bit4 = Y flip

### Collision-detection area

Collision detection is evaluated in the complete boot-selected renderer
framebuffer, not necessarily only in the portion that is physically visible on
the connected display. A machine profile may retain pixels in its framebuffer
that lie outside the practical visible screen area; overlapping
collision-enabled sprites in those stored pixels can therefore set the
collision status even though the overlap cannot be seen on screen.

Applications should enable collision detection only for sprites that are
currently intended to be displayed and to participate in gameplay collision
logic. Disable the collision-enable bit when a sprite is parked or hidden in an
off-screen framebuffer area.

## 3. Transparent colour command

Any command in the range `40h..4Fh` sets the transparent colour directly from the low nibble:

- command value low nibble = transparent colour index `0..15`

## 4. Global / control commands (`50h..5Fh`)

### `50h` — `SET_ENGINE_MODE`

**Cold-boot gate in v0.9.45:** this is the only command recognized before the
engine has been initialized. `50h` is 80 decimal; do not confuse it with `80h`
`UPLOAD_RAW`. All other commands, including egg starts, are discarded until a
nonzero mode byte completes this transaction.
One F9h byte:

- `00h` = engine off; `/EXTC` inactive and EC0..EC3 electrically released
- `01h..FEh` = engine on (normal mode)
- `FFh` = forced on

Cold-start software must send this wake transaction unconditionally rather than
probing bit6 first. It may then poll F8h bit6 until it reads `0`; that transition
is the ready handshake indicating that the engine and normal command parser are
available. Commands sent before bit6 becomes `0` are intentionally discarded.

### `51h` — `SET_FPS`
One F9h byte:

- `00h` = 50 fps update mode
- `01h` = 25 fps update mode

This changes scene update/render cadence only. Video output timing remains 50 Hz.

### `52h` — `SET_RENDER_OVERRUN_VISUAL`
One F9h byte:

- `00h` = disabled
- nonzero = enabled

When enabled, render overruns are shown by a flashing on-screen exclamation-mark overlay.

### `53h` — `CLEAR_RENDER_OVERRUN`
No F9h bytes. Clears the sticky render-overrun latch.

### `54h..57h` — `SET_RASTER_INT1..4`
Two F9h bytes, high byte first:

- `0000h` = disabled
- `0001h..0137h` (`1..311`) = trigger on that NICK scanline

### `58h` — `CLEAR_RASTER_INT`
No F9h bytes. Clears all four pending raster interrupt latches.

### Resource descriptor commands

- `5Bh` — `DEFINE_FONT`
  - 5 F9h bytes: font id, addr low, addr mid, addr high7, width/height units
  - each 4-bit dimension stores `(8-pixel units - 1)`: `0..15` = `8..128` pixels
  - 128 glyphs per font, 1bpp data
- `5Ch` — `DEFINE_BITMAP`
  - 7 F9h bytes: bitmap id 0..255, addr low, addr mid, addr high7, width/height highbits, width low, height low
- `5Dh` — `DEFINE_PATTERN`
  - 4 F9h bytes: pattern definition id, addr low, addr mid, addr high7
  - the address is the base of a bank containing up to 256 fixed 16×8 patterns
- `5Eh` — `DEFINE_TILESET`
  - 4 F9h bytes: tileset id, addr low, addr mid, addr high7
- `5Fh` — `DEFINE_TILEMAP`
  - 7 F9h bytes: map id, addr low, addr mid, addr high7, encoded width, encoded height, tileset id
  - width/height bytes store `(cells - 1)`: `00h..FFh` = `1..256` cells

## 5. Resource upload / processing commands

All source-format commands produce the same internal 2dfx packed-pixel format.
The command name describes the format of the **source data**, not the currently
selected Enterprise/TVC hardware profile. Therefore NICK and TVC conversion
commands are available on both machines.

### Common upload header (`80h..82h`)

Five F9h header bytes, followed by the declared payload:

- bytes 0..2 = destination resource address
- bytes 3..4 = real upload length, little-endian
- bytes 5.. = payload

Upload length `0001h..FFFFh` means 1..65535 bytes. `0000h` means 65536 bytes.

### `80h` — `UPLOAD_RAW`

Stores payload bytes byte-exact. Use it for:

- ZX1 compressed streams
- already-internal packed graphics
- fonts and text strings
- tilemap cell arrays
- lookup tables and metadata

### `81h` — `UPLOAD_NICK`

Converts uncompressed Enterprise/NICK 16-colour graphics bytes to internal
internal packed format before storage.

NICK source bytes contain the two pixel nibbles interleaved by bit plane:

- left pixel = source bits 7,5,3,1
- right pixel = source bits 6,4,2,0

### `82h` — `UPLOAD_TVC`

Converts uncompressed TVC Graphics-16 bytes to internal packed format before
storage.

A TVC source byte is interleaved `IIGGRRBB`:

- left pixel `IGRB` = source bits 7,5,3,1
- right pixel `IGRB` = source bits 6,4,2,0

Because the TVC motherboard external-colour wiring uses numeric bit order
`BGRI`, conversion swaps the I and B bit positions after extracting each pixel.
The converted left pixel is stored in the internal low nibble and the converted
right pixel in the internal high nibble.

### Common ZX1 argument block (`83h..85h`)

Six F9h bytes:

- bytes 0..2 = compressed source address
- bytes 3..5 = destination address

The compressed stream must first be uploaded byte-exact with `UPLOAD_RAW`.
The operation is logically queued and completes at a frame-safe boundary.
The emulator preserves the front framebuffer while a resource operation is pending.

### `83h` — `UNZX1_RAW`

ZX1-decompresses to byte-exact output. Use this when the compressed data was
already in internal packed format, or when the result is non-pixel raw data.

### `84h` — `UNZX1_NICK`

ZX1-decompresses a stream whose uncompressed bytes are in NICK graphics format
and converts those output bytes to internal packed format while producing the
destination.

### `85h` — `UNZX1_TVC`

ZX1-decompresses a stream whose uncompressed bytes are in TVC Graphics-16
format and converts those output bytes to internal packed format while
producing the destination.

The converted ZX1 modes produce the same observable output as decompressing
the entire stream before performing the corresponding NICK or TVC conversion.
The portable emulator is free to use its own equivalent implementation.

Recommended paths:

| Source representation | Uncompressed path | ZX1-compressed path |
|---|---|---|
| Raw/internal bytes | `UPLOAD_RAW` | `UPLOAD_RAW` + `UNZX1_RAW` |
| NICK graphics | `UPLOAD_NICK` | `UPLOAD_RAW` + `UNZX1_NICK` |
| TVC Graphics-16 | `UPLOAD_TVC` | `UPLOAD_RAW` + `UNZX1_TVC` |

## 6. Firmware information command

### `8Eh` — `SHOW_FW`

No F9h parameters:

```text
OUT 248,142   ; 8Eh SHOW_FW
```

`SHOW_FW` replaces the active 2dfx application with the built-in static
firmware-information screen. It is always compiled in and does not depend on
`TWO_DFX_ENABLE_EGGS` or the optional C0 logo-egg switch.

After the command is accepted, 2dfx:

- aborts all ordinary SPB, 2DPT, descriptor and command-session state;
- stops the active egg/demo and cancels queued-but-not-started rendering or ZX1 work;
- safely completes or cancels pending work at the specified logical boundary;
- sets transparent colour index `0`, clears the scene to background colour `0`, and renders one static framebuffer;
- displays the large 2dfx logo using the boot-selected profile layout;
- displays `FIRMWARE: Vx.y.z` and the boot-selected `MODE: TVC` or
  `MODE: ENTERPRISE` with the built-in VCR/OSD font;
- displays a QR code for `https://2dfx.net/firmware/latest/`;
- enters `ENGINE_ON_FORCED` when the completed page is published at VSYNC.

Logo/text colours are profile-aware:

- TVC: black `00h`, red `03h`, white `0Fh`;
- Enterprise default IS-BASIC text palette: black `00h`, red `03h`, white `04h`.

On TVC the logo and QR frame are adjusted to the corrected BASIC-safe geometry,
with the QR frame made visually square for the 2:1 pixel aspect. On Enterprise
the complete page is kept inside the corrected IS-BASIC-safe rectangle
X=124..763, Y=38..262; the QR panel begins at Y=39.

`SHOW_FW` is a terminal information mode. From the instant it is accepted, all
subsequent F8h and F9h writes are ignored, including `8Fh SYS_RESET`. The page
remains displayed until the emulated peripheral is physically reset or power-cycled.

Like other ordinary F8h commands, `SHOW_FW` is not recognized in the middle of
an active strict upload or UNZX1 argument transaction. `SYS_RESET` remains the
recovery command for such an incomplete transaction before `SHOW_FW` is entered.

## 7. System reset command

### `8Fh` — `SYS_RESET`

No F9h parameters:

```text
OUT 248,143   ; 8Fh SYS_RESET
```

`SYS_RESET` resets the public 2dfx application state; it is not the same
as a peripheral power cycle. The command clears the complete 8 MiB resource
RAM and exposes the documented busy status during that logical operation.

The current engine mode is deliberately preserved. `SYS_RESET` temporarily
hides `/EXTC` while it works, then restores the exact OFF/NORMAL/FORCED mode
that was active when the reset command was accepted. It does not require the
host to emulate the physical clock or voltage-control hardware.

Immediately after the command is accepted:

- `/EXTC` is forced inactive and the host computer picture remains visible;
- new renders and ordinary F8h/F9h commands are blocked;
- all active command sessions are aborted, including partial SPB, 2DPT,
  descriptor, upload and UNZX1-argument sessions;
- a queued-but-not-started render or UNZX1 job is cancelled;
- previously active operations are completed or cancelled consistently.

The device then clears:

- all 8 MiB of resource RAM;
- both internal framebuffers.

After the memory clear, the firmware restores these power-on defaults:

- engine mode = preserved exactly as it was when `SYS_RESET` was requested (`OFF`, `NORMAL` or `FORCED`);
- scene update cadence = 50 fps;
- transparent colour index = 0;
- render-overrun visual = disabled;
- sticky render-overrun status = cleared;
- built-in logo = hidden/default state;
- all four raster comparators = disabled and all pending latches = cleared;
- all 64 shadow/active SPBs = disabled/cleared;
- collision status = no collision;
- background and foreground 2DPTs = `RET` at slot 0 / cleared;
- all font, bitmap, pattern, tileset and tilemap descriptors = cleared;
- active firmware egg/demo mode = stopped.

The boot-selected **Enterprise or TVC profile** is preserved. The portable
emulator does not reproduce MCU-specific clock, PIO or memory-controller setup.

### Reset completion polling

While `SYS_RESET` is running, F8h bit0 / STATUS0 is held at `0`. It returns to
`1` when reset is complete:

```text
OUT 248,143
wait:
    read F8h
    repeat while bit0 = 0
```

After completion, STATUS0 resumes its normal render-time scope-pulse role, so
bit0 is a defined reset-busy/completion indication only during a `SYS_RESET`.
F8h/F9h writes received while reset is busy are ignored.

## 8. Built-in logo overlay commands

### `90h` — `SHOW_BUILTIN_LOGO`
Four F9h bytes:

- byte0 = high bits: X bits 9..8 in bits 2..1, Y bit 8 in bit0
- byte1 = X low byte
- byte2 = Y low byte
- byte3 = colours: high nibble = main/white colour, low nibble = accent/red colour

On Enterprise, byte3 selects both colours through the current NICK/LPT palette.
On TVC, byte3 is still required and consumed to preserve the unified command
format, but its value is ignored. The fixed TVC indexes are always used:

- main/white = `0Fh`
- accent/red = `03h`

The built-in logo source artwork size is **206 × 79 pixels**.
White background pixels are transparent; the logo uses a main colour and an accent colour.

### `91h` — `HIDE_BUILTIN_LOGO`
No F9h bytes.

## 9. 2DPT command model

`60h..7Fh` write the background 2D parameter table.
`E0h..FFh` write the foreground 2D parameter table using the same command numbers with bit7 set.

Each retained 2DPB is **12 bytes** (256 indexed slots per layer, with both shadow and active copies). Legacy 2DPT commands keep their original F8/F9 transfer lengths. In general:

- `BYTE0` = opcode
- `BYTE1` = command-specific bits plus X/Y/W/H high bits
- `BYTE2` = X low
- `BYTE3` = Y low
- `BYTE4` = W low or command-specific low byte
- `BYTE5` = H low or command-specific low byte
- `BYTE6..BYTE11` = command-specific / reserved

Primitive opcodes:

- `60h` NOP
- `61h` SET_COLOR
- `62h` PLOT
- `63h` LINE
- `64h` ERASE_LINE
- `65h` BUCKET_FILL
- `66h` RECT
- `67h` FILL_RECT
- `68h` ROUND_RECT
- `69h` FILL_ROUND_RECT
- `6Ah` ELLIPSE
- `6Bh` FILL_ELLIPSE
- `6Ch` ARC
- `6Dh` PIE
- `6Eh` SET_XY_OFFSET
- `6Fh` SKIP_NEXT
- `70h` DRAW_TEXT
- `71h` BLIT
- `72h` PATTERN_FILL
- `73h` TILEMAP
- `74h` PATTERN_BUCKET_FILL
- `75h` TRIANGLE (all three vertices have full coordinate range)
- `76h` FILL_TRIANGLE (all three vertices have full coordinate range)
- `77h` CROP_BLIT
- `78h` STRETCH_BLIT
- `79h` SET_CLIP_RECT
- `7Ah` DRAW_MESH_WIREFRAME
- `7Bh` DRAW_MESH_FILLED
- `7Ch` DRAW_MESH_FWE (filled with edges)
- `7Dh` unassigned
- `7Eh` RESERVED extension prefix (not implemented in v1.1.0)
- `7Fh` RET

### `6Eh` / `EEh` — `SET_XY_OFFSET`

Parameters after the command byte:

- byte0 = 2DPT slot number
- byte1 = signed X offset (`-128..+127`, two's complement)
- byte2 = signed Y offset (`-128..+127`, two's complement)

`SET_XY_OFFSET` sets a persistent destination-coordinate offset for the following
commands in the same playlist run:

```text
effective X = command X + SET_XY_OFFSET X
effective Y = command Y + SET_XY_OFFSET Y
```

The command **sets** the offset; it does not add to the previous offset. A
later `SET_XY_OFFSET 0,0` restores unmodified coordinates. At the beginning of every
background and foreground 2DPT run, the offset is reset independently to
`0,0`.

The offset affects destination/position coordinates of PLOT, BUCKET_FILL,
RECT/FILL_RECT, ROUND_RECT/FILL_ROUND_RECT, ELLIPSE/FILL_ELLIPSE, ARC, PIE,
DRAW_TEXT, BLIT, PATTERN_FILL, TILEMAP and PATTERN_BUCKET_FILL. TRIANGLE/FILL_TRIANGLE translate all three vertices. For LINE and ERASE_LINE it affects
both endpoint coordinate pairs. TILEMAP source-tile coordinates, dimensions,
radii, resource addresses and descriptor numbers are not changed.

Signed results are clipped against the renderer framebuffer; they do not wrap
through the unsigned coordinate range.

### `6Fh` / `EFh` — `SKIP_NEXT`

Parameters after the command byte:

- byte0 = 2DPT slot number
- byte1 = unsigned number of following slots to skip (`0..255`)

A count of 0 continues with the immediately following slot. A count of 1 skips
one following slot, and so on. Skipped slots are not interpreted at all, so a
skipped SET_COLOR, SET_XY_OFFSET, SKIP_NEXT or RET has no effect.

Execution can only move forward. If the calculated next slot is beyond slot
255, the current 2DPT run ends. Therefore `SKIP_NEXT` cannot create an infinite
playlist loop.

Notes:

- stroked primitives use `BYTE1` bits 7..6 as `stroke - 1`
- `ROUND_RECT` / `FILL_ROUND_RECT`: `BYTE6` = radius X, `BYTE7` = radius Y
- `ELLIPSE`, `FILL_ELLIPSE`, `ARC`, `PIE` use centre/radius semantics
- `ARC` / `PIE`: `BYTE6` = start angle, `BYTE7` = end angle, clockwise, where 0=top, 64=right, 128=bottom, 192=left

## 10. Resource-aware 2D commands

### `70h` — `DRAW_TEXT`
Uses a font descriptor and a raw string resource. String terminator is `80h`.

### `71h` — `BLIT`
Uses a bitmap descriptor. Source data is PIO-native packed 4bpp bitmap data.

BLIT uses `BYTE1` as follows:

- bit7 = transparency-aware flag
  - `0`: exact packed-byte copy; this is the original maximum-speed path
  - `1`: source pixels matching the current transparent-colour index preserve the existing framebuffer pixel
- bits5..4 = destination X bits 9..8
- bit3 = destination Y bit 8
- bit6 and bits2..0 are reserved

Exact BLIT deliberately copies all source pixels, including the transparent
colour index. Transparency-aware BLIT is useful for foreground/HUD graphics
that must preserve previously rendered sprites or 2D graphics underneath.
The same bitmap descriptor may be used in either mode.

### `72h` — `PATTERN_FILL`
Uses one fixed 16×8 pattern from a pattern table.

### `73h` — `TILEMAP`
Draws a viewport from a defined fixed 16×8 tilemap.

### `74h` — `PATTERN_BUCKET_FILL`
Fills one four-connected region with a selected fixed 16×8 pattern.

Parameters after the command byte:

- byte0 = 2DPT slot number
- byte1 = seed X/Y high bits (`bits5..4 = X9..8`, `bit3 = Y8`)
- byte2 = seed X low byte
- byte3 = seed Y low byte
- byte4 = pattern-table descriptor number (`0..63`)
- byte5 = pattern number within that table (`0..255`)

The effective seed coordinate first samples the colour already present in the
current framebuffer. Every four-connected pixel of that same colour is then
replaced by the corresponding pixel from the selected pattern. The fill uses an
independent visited bitmap, so it remains correct even when one or more pattern
pixels are the same colour as the sampled target.

Pattern phase is anchored to absolute effective 2dfx coordinates, not to the
seed: pattern X is `effective_pixel_x & 15`, pattern Y is
`effective_pixel_y & 7`. Therefore selecting another seed inside the same
connected region produces the same final pattern alignment. `SET_XY_OFFSET`
affects the seed position but does not add another pattern-phase offset.

Pattern pixels are copied exactly. No transparency test is performed. The
implementation uses transient renderer scratch RAM only; it does not reserve
additional persistent SRAM. If its very large finite internal span stack were
ever exhausted by a deliberately pathological region, the operation degrades
safely to a partial fill rather than risking MCU memory corruption.

### `75h` / `76h` — TRIANGLE / FILL_TRIANGLE

Triangle coordinates are encoded using two highbits bytes, including the
third vertex Y high bit. **Nine F9h bytes after the opcode**:

| F9h byte | Meaning |
|---|---|
| 0 | Slot 0..255 |
| 1 | Vertex highbits 0+1: bits5..4 X0[9:8], bit3 Y0[8], bits2..1 X1[9:8], bit0 Y1[8]; bits7..6 reserved |
| 2 | Vertex 2 highbits: bits5..4 X2[9:8], bit3 Y2[8]; all other bits reserved |
| 3,4 | X0, Y0 low bytes |
| 5,6 | X1, Y1 low bytes |
| 7,8 | X2, Y2 low bytes |

`75h` draws three 1-pixel lines; `76h` fills. Foreground `F5h`/`F6h`.
All coordinates have X=0..1023, Y=0..511 and all three obey the current
`SET_XY_OFFSET`. Both windings accepted, no implicit culling; zero-area fill
is empty. Boundary and edge pixels depend on the rasterizer. 

### `77h` / `F7h` — CROP_BLIT

Copies a bitmap subrectangle at its native size. **Ten F9h bytes**:

| F9h byte | Meaning |
|---|---|
| 0 | Slot 0..255 |
| 1 | Source highbits: bit7 transparency; bit6 reserved; bits5..4 X[9:8], bit3 Y[8], bits2..1 W[9:8], bit0 H[8] |
| 2..5 | Source X, Y, W, H low bytes |
| 6 | Destination highbits: bits5..4 X[9:8], bit3 Y[8], all other bits reserved |
| 7..8 | Destination X, Y low bytes |
| 9 | Bitmap descriptor ID |

The selected source region uses bitmap-relative coordinates. `SET_XY_OFFSET`
changes only the destination; source sampling does not move. Actual width and
height are in pixels, zero means no drawing. Source pixels outside the defined
bitmap are skipped safely, not wrapped or read from unrelated RRAM. Supports
odd source X / destination X / widths and optional transparency.

### `78h` / `F8h` — STRETCH_BLIT (new)

Copies a bitmap subrectangle with nearest-neighbour scaling. **Twelve F9h bytes**:

| F9h byte | Meaning |
|---|---|
| 0 | Slot 0..255 |
| 1 | Source highbits and transparency, exactly as CROP_BLIT |
| 2..5 | Source X, Y, W, H low bytes |
| 6 | Destination highbits: bits5..4 X[9:8], bit3 Y[8], bits2..1 W[9:8], bit0 H[8]; bits7..6 reserved |
| 7..10 | Destination X, Y, W, H low bytes |
| 11 | Bitmap descriptor ID |

All X/W fields are 10 bits, Y/H fields 9 bits. Source and destination
width/height are actual pixel counts; zero means no drawing. Sampling is
`source + floor(destination_relative * source_extent / destination_extent)`
for each axis, with clipping applied **after** determining the sampling phase.
Out-of-source-bitmap samples are skipped, never read outside the descriptor.
Destination-only `SET_XY_OFFSET`, full nibble-accurate precision, and the same
transparency semantics as BLIT/CROP_BLIT.

### Updated `71h` — BLIT pixel precision

Unchanged wire protocol and bitmap descriptor definition. Odd destination X
is no longer rounded down. An odd bitmap width is preserved: rows are now
`ceil(width/2)` bytes, with the final high nibble unused for odd widths.
Even-aligned copies retain the optimized fast copy/transparent-run path.

### `79h` / `F9h` — SET_CLIP_RECT

Seven Z80 OUT instructions: `opcode, slot, highbits, X low, Y low, W low,
H low`. Highbits bits 5:4 carry X9:8; bit 3 Y8; bits 2:1 W9:8;
bit 0 H8; bits 7:6 are reserved and ignored. X/W are 10-bit, Y/H 9-bit.
The corresponding typed API is `twodfx_2d_set_clip_rect(layer, slot, x, y,
width, height)`.

The clip replaces the previous clip for subsequent instructions in the SAME
2DPT replay. It is anchored in physical display coordinates; SET_XY_OFFSET
moves subsequent shapes, **not** this clip. A zero width or height makes an
empty region. Each background and foreground replay resets its clip to the
complete visible framebuffer, with no inheritance across passes/frames.
Clipping applies to all 2DPT drawing, including flood-fill traversal, and
never changes source bitmap coordinates or the phase of an absolute pattern.
It does NOT clip sprites or the firmware's built-in logo overlay. `SKIP_NEXT`
can skip a SET_CLIP_RECT exactly as any other slot. Use a replacement clip
covering the full visible area to restore unrestricted drawing.

## 3D mesh commands (v1.1.0)

These are the **public** 3D commands. The original D3 and D4 demo renderers are
separate demonstration implementations and do not redefine this protocol.
All 16-bit quantities below are **little-endian**. 3D world axes are +X right,
+Y up, +Z forward. Object rotations are applied in ROLL (about Z), then YAW
(about Y), then PITCH (about X) order. Positive yaw points +Z toward +X,
positive pitch points +Z toward -Y, positive roll points +X toward +Y.
The camera uses the inverse of the same pose convention.

### 10-bit 3D angle encoding

Both camera and mesh objects use **three 10-bit unsigned rotation angles**,
with 1024 steps = one complete turn (0.3515625 degrees per step). Their common
four-byte little-endian bitstream is:

```text
bits  0.. 9    ROLL   (0..1023)
bits 10..19    YAW    (0..1023)
bits 20..29    PITCH  (0..1023)
bits 30..31    reserved, send zero
```

Pack as `A = (ROLL & 1023) | ((YAW & 1023) << 10) |
((PITCH & 1023) << 20)` and send low byte first. For ROLL=1023,
YAW=513 and PITCH=772, send `FF 07 48 30`.
The existing 2D ARC/PIE commands **still use 8-bit** rotation units.

### `86h` — DEFINE_MESH (7 F9h bytes)

| F9h byte | Meaning |
|---|---|
| 0 | Mesh ID, 0..255 |
| 1..3 | Resource RAM start address, 23-bit little-endian |
| 4 | Vertex count minus one (00h..FFh = 1..256 vertices) |
| 5..6 | Triangle count, uint16 little-endian (supported 1..512) |

A resource mesh is stored as a consecutive vertex table followed immediately
by a triangle table. Each vertex is 6 bytes: signed `X,Y,Z` (three int16 LE).
Each triangle is 4 bytes: three zero-based vertex indices `A,B,C` and one
attribute byte:

- Bits 0..3: filled face colour index (0..15).
- Bit 4: enable edge A–B in wireframe/FWE rendering.
- Bit 5: enable edge B–C.
- Bit 6: enable edge C–A.
- Bit 7: draw two-sided (disable backface rejection for this face).

Vertex IDs are 8-bit. An invalid mesh definition becomes inactive; individual
triangles with invalid vertex indexes are ignored. Mesh memory must lie inside
the 8 MiB address space **without wrapping**. Multiple draw slots can share
one mesh ID with different object poses.

### `87h` — SET_3D_PROJECTION (8 F9h bytes)

Four uint16 LE values in order: `FX, FY, NEAR, FAR`.
`FX` and `FY` are perspective focal factors in native projected screen units;
`NEAR` and `FAR` are positive camera-space Z distances. The command requires
`FX>0`, `FY>0`, `NEAR>0`, and `FAR>NEAR`, otherwise the previous shadow
projection is retained. Defaults after graphics reset are
`FX=365, FY=300, NEAR=96, FAR=2490`.
The optical centre is calculated from the boot-selected machine profile's
maximum visible rectangle, not supplied by the programmer. It is unaffected
by `SET_CLIP_RECT` or `SET_XY_OFFSET`.

### `88h` — SET_CAMERA_POSE (10 F9h bytes)

| F9h bytes | Meaning |
|---|---|
| 0..1 | X, signed int16 LE |
| 2..3 | Y, signed int16 LE |
| 4..5 | Z, signed int16 LE |
| 6..9 | 10-bit packed ROLL, YAW, PITCH (as above) |

There is one global camera shared by BACKGROUND and FOREGROUND. Camera pose
starts at zero and updates the **shadow** state; the active camera changes
only when the renderer accepts a frame.

### `7Ah/FAh`, `7Bh/FBh`, `7Ch/FCh` — 3D draw commands (12 F9h bytes each)

| Background / foreground opcode | Action |
|---|---|
| `7Ah` / `FAh` | DRAW_MESH_WIREFRAME — enabled face edges |
| `7Bh` / `FBh` | DRAW_MESH_FILLED — solid per-face colours |
| `7Ch` / `FCh` | DRAW_MESH_FWE — filled faces plus enabled edges |

All three commands have the **same** parameter bytes:

| F9h bytes | Meaning |
|---|---|
| 0 | 2DPT slot ID, 0..255 |
| 1 | Mesh ID, 0..255 |
| 2..3 | Mesh world X, int16 LE |
| 4..5 | Mesh world Y, int16 LE |
| 6..7 | Mesh world Z, int16 LE |
| 8..11 | Packed 10-bit ROLL, YAW, PITCH |

The internal 2DPT command block remains exactly 12 bytes, with opcode in byte
0, mesh ID in byte 1, XYZ in bytes 2..7 and packed angles in bytes 8..11.
There is **no drawing-mode data byte** and no extension prefix.

`SET_COLOR` determines wireframe and FWE edge colour; filled faces take their
colours from triangle attribute bytes. `SET_CLIP_RECT` clips projected pixels;
`SET_XY_OFFSET` affects projected X/Y only, not camera/world positions. The
selected 2DPT order determines draw ordering among separate mesh instances;
the renderer sorts triangles within each mesh far to near. No general
Z-buffer, cross-mesh sorting, hidden-line elimination, lighting or textures.

### Frame-latch and Resource RAM rules

Mesh definitions, camera and projection settings have shadow/active state and
are latched at frame acceptance with the existing 2DPTs. A render in progress
uses stable active state. Writes across the acceptance boundary can still
combine an old slot with a new camera (same existing 2DPT snapshot caveat).
The actual **Resource RAM geometry bytes are not latched**: do not overwrite a
mesh region while a render may read it. A safe animation pattern uploads into
an unused resource bank and switches the mesh descriptor after completion.

The extension opcodes `7Eh/FEh` are reserved and unimplemented. The supported
public 3D angle representation is a 10-bit turn (0..1023).

## 11. F8h readback / STATUS bit map

The external F8h readback map is:

- bit7 = collision detected in previous completed frame; collision is evaluated across the complete renderer framebuffer, including any profile-specific stored area outside the physically visible screen
- bit6 = 2dfx awake/ready presence detector: `1` during cold sleep/wake transition, `0` after the complete active engine is ready
- bit5 = render-budget overrun since last clear
- bit4 = raster interrupt 4 pending
- bit3 = raster interrupt 3 pending
- bit2 = raster interrupt 2 pending
- bit1 = raster interrupt 1 pending
- bit0 = render-time measurement pin during normal operation; during `SYS_RESET`, `0` means reset busy and `1` means reset complete

Bits 7, 6 and 5 are active-low at the GPIO/status-pin level. In particular, bit6 is intentionally indistinguishable from an absent board until software sends the mandatory `SET_ENGINE_MODE` wake transaction and the engine finishes initialization. STATUS0 is also active-low for the reset-busy interval.

## 12. Easter eggs / demos

`C0h..DFh` are reserved for firmware-side eggs and diagnostics. They are not
part of the stable application command interface and may change between
firmware releases.

The v1.1.0 registry includes:

| Command | Demo | Profiles |
|---|---|---|
| `C0h` | animated 2dfx logo | Enterprise, TVC |
| `C1h` | sprite benchmark/showcase | Enterprise, TVC |
| `C2h` | 2D primitive showcase | Enterprise, TVC |
| `C3h` | text/font showcase | Enterprise, TVC |
| `C4h` | animated BLIT showcase | Enterprise, TVC |
| `C5h` | PATTERN_FILL showcase | Enterprise, TVC |
| `C6h` | TILEMAP showcase | Enterprise, TVC |
| `C7h` | basic Pong | Enterprise, TVC |
| `C8h` | WOW Pong | Enterprise, TVC |
| `C9h` | Wriggler | Enterprise, TVC |
| `CAh` | Boulder | Enterprise, TVC |
| `CBh` | ARC/PIE benchmark | Enterprise, TVC |
| `CCh` | internal diagnostic stages | Enterprise, TVC |
| `CDh` | full-frame BLIT benchmark | Enterprise, TVC |
| `CEh` | dual-layer proof | Enterprise, TVC |
| `CFh` | EC bit-pattern timing test | TVC only |
| `D0h` | CRISP tearing test | TVC only |
| `D1h` | visible-area coordinate probe | Enterprise, TVC |
| `D2h` | C2 primitives + animated SET_XY_OFFSET whole-scene motion | Enterprise, TVC |
| `D3h` | rotating Elite ship showcase, specialized 2D drawing path | Enterprise, TVC |
| `D4h` | curved tunnel and Asp chase, original specialized renderer | Enterprise, TVC |
| `D5h` | curved tunnel and Asp chase via public 3D mesh commands | Enterprise, TVC |
| `DFh` | temporary five-phase graphics benchmark: pattern bucket, crop, crop+offset, stretch, stretch+offset | Enterprise, TVC |

A valid selection starts from centralized clean public graphics state. Resource
RAM is retained, but sprites, both 2DPTs, descriptors, collision/raster state,
transparent colour and other visible scene producers are reset before the egg
`start()` callback.
