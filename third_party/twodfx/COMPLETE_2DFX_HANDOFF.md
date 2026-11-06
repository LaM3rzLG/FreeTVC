# 2dfx portable emulator — complete handoff

## Purpose

Provide a standalone, portable C99 implementation of the **2dfx v1.1.0** public device interface for Enterprise and TVC emulators. The library is a guest-visible expansion device, not a USB/HDMI or MCU simulator. It comes with all supported v1.1.0 firmware-resident Easter egg C programs and SHOW_FW, run through a portable public `twodfx_api` implementation.

## What host developers receive

- The library target `twodfx_emu` and its public interface `src/include/twodfx_emu.h`.
- A dependency-free C++11 integration example, `src/adapter/ep128emu_twodfx_adapter.hpp`.
- The 8 MiB resource model; F8/F9 bus parser, uploads/conversion and ZX1, sprite descriptors and transforms, BG/FG 2DPT, 2D primitives, fonts/bitmaps/patterns/tilemaps, indexed colours and transparency, raster interrupt/collision reporting, camera and public 3D mesh functions.
- Original 2dfx v1.1.0 egg sources (23 IDs), original public SHOW_FW, owned assets, the public `twodfx_api.h` and SHA-256 origin information for copied sources.
- A format-v13 snapshot API, command reference, native tests and integration documentation.

## VM responsibilities

Attach F8h/F9h Z80 I/O at the right expansion slot, send every actual 50 Hz VSYNC and raw scanline HSYNC, combine IRQ level with all other interrupt sources, composite per-pixel 2dfx colour indices/mask at the correct NICK/TVC stage, manage power cycle distinctly from host soft reset and serialize a peripheral-specific snapshot chunk. Use `docs/INTEGRATION.md` for exact conventions. Do not disturb existing TVC256+++ functionality.

## Checks performed on this standalone package

The C99 static library and C++11 adapter compile natively. The smoke tests start and tick every applicable original egg on Enterprise/TVC, render SHOW_FW and check C2 public fill/outline 2DPT opcodes. These are functional smoke checks, not a complete public-API conformance suite.

The 2dfx creator reported that all applicable Easter eggs appeared visually identical to physical 2dfx hardware across Enterprise and TVC during a manual check on 22 October 2026. The library does not reproduce MCU execution timing. Edge-case parity, host integration, full frame traces and complex snapshot scenarios require integrator verification.

## Constraints to plan for

- Built-in egg C modules use static state at the process level; avoid running multiple independent active egg instances simultaneously in one process.
- Snapshot v13 stores the portable device state but not every static C variable used by running eggs. Snapshot restore mid-animation therefore is not guaranteed source-faithful.
- No RP2354 PIO/DMA, private fast paths, core worker scheduling or synthetic render-overrun time.
- Some ship/worm showcase artwork is third-party-derived and excluded from the MIT grant; read `ASSET_LICENSES.md` before further distribution.

## Package inventory

- `src/`: **all** production compilation units and original-owned assets.
- `docs/`: integration, command, state format, compatibility, testing and source-provenance reference.
- `tests/`: self-contained native smoke tests.
- `README.md`, `START_HERE.md`, `LICENSE`, `ASSET_LICENSES.md`, `CREDITS.md`.

Contact the module maintainer to coordinate VM-specific wiring and licensing decisions for distribution involving third-party-derived game content.
