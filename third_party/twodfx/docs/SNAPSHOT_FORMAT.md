# Device snapshot contract — format 13

This document is the **canonical** on-disk / VM-chunk description of `twodfx_emu_save_snapshot()` and `twodfx_emu_load_snapshot()`; `src/core/twodfx_emu.c` is authoritative if any discrepancy is found. Device snapshots contain the **emulated device state**, not entire Enterprise/TVC VM snapshots or all egg-private file-static animation variables.

## Public C ABI

- `size_t twodfx_emu_snapshot_size(void)` — exact size (constant for this ABI/format).
- `int twodfx_emu_save_snapshot(const twodfx_emu *, void *output, size_t capacity)` — writes the complete blob when capacity is sufficient; returns 1 on success.
- `int twodfx_emu_load_snapshot(twodfx_emu *, const void *input, size_t length)` — accepts only **exact** format/size, correct machine profile, CRC and semantically valid state. A failure leaves the current device unchanged.

All integers are encoded little-endian; **never `memcpy` the native device struct** or depend on compiler padding/alignment.

## Fixed 20-byte header

| Byte offset | Size | Format | Meaning |
|---:|---:|---|---|
| 0 | 8 | ASCII | Exactly `2DFXSNAP` |
| 8 | 1 | u8 | Format version `13` |
| 9 | 3 | bytes | Reserved, all zero |
| 12 | 4 | u32 LE | Canonical serialized body length |
| 16 | 4 | u32 LE | CRC-32/IEEE of **body only** (reflected polynomial `0xEDB88320`, initial/final xor `0xFFFFFFFF`) |
| 20 | variable | serialized body | Exact, private field sequence below |

The body holds the emulated **machine profile**; a snapshot for the wrong target is rejected atomically. The host should additionally label its VM state chunk with its own chunk ID and this device format version.

## Device body contents

The serializer `snap_body()` writes a deterministic and architecture-neutral stream with all required runtime state:

- Machine selection, physical wake/session/parser state including partially received commands and F9 upload bytes.
- Resource memory (8 MiB), resource job and descriptors for fonts/bitmaps/patterns/tilesets/tilemaps.
- Background and foreground 2DPT **shadow and active** entries, sprites/collision state, engine modes and frame controls.
- Video/framebuffer pixel colours and per-pixel coverage; logo overrides, SHOW_FW status and device-managed egg input/state. Original egg module file-static animation variables are not all captured.
- 3D resources/descriptors and **shadow/active** camera and projection settings, keeping frame publication deterministic.
- D4/D5 tunnel controls and animation state, saved as: `tunnel_mode` (u8, 0–5), `tunnel_profile` (u8, 0–2), `tunnel_ship` (u8, 0–1), `tunnel_zoom` (u8, 0–1), `tunnel_auto` (u8, 0–1), `tunnel_phase` (u16 LE, 0–399), `tunnel_travel` (u32 LE). These **11 bytes are at the end of the v13 body**. The full field encoding is defined by `snap_body` and is intentionally **not** a supported per-field patch API.

The shared controls are valid only in their associated egg lifecycle. The loader additionally verifies parser boundaries, descriptor geometry, ranges, frame buffer indices, and other invariants even if the CRC is correct.

## VM integration rules

1. Save the complete returned byte sequence as **one versioned 2dfx chunk** beside other VM chunks. Never manually serialize individual C struct fields.
2. Save/restore the host VM time reference consistently; the device chunk does **not** contain the host's NICK/TVC scheduler, palette, display or other interrupt sources.
3. After a successful load, recompute the VM combined IRQ state from all sources including `twodfx_emu_irq_asserted()`.
4. Keep device profile consistent: Enterprise snapshots cannot be loaded into a TVC device and vice versa.
5. Only **format 13** is recognized. Any chunk using a different format identifier is rejected with no partial restore; hosts should report an incompatible peripheral state chunk.
6. Restore at an appropriate host execution boundary so the saved device scene and VM display timeline remain consistent.

Deliberately **not serialized or emulated:** original egg-private process-global variables, physical MCU render-time/overrun duration, RP2354B DMA/PIO/core schedule. A snapshot taken during an egg animation is not guaranteed to resume identically. Device state is versioned independently of ep128emu's own snapshot schema.
