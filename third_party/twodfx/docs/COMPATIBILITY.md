# Supported behavior and validation boundaries

The library implements the publicly documented 2dfx v1.1.0 command semantics, preserving the full original firmware-resident egg source and SHOW_FW content. It is an emulation of externally visible behaviour, not a simulation of RP2354 instruction execution, DMA, PIO or wall-clock renderer speed.

| Subsystem | Scope / qualification |
|---|---|
| F8h/F9h parser, resource RAM and uploads | Supported; normal guest command streams, RAW/NICK/TVC conversion and ZX1 |
| Sprites, descriptors, BG/FG 2DPT, transparency and compositing | Implemented against shared renderer, frame-latched publication and indexed output |
| 2D graphics, bitmap/text/pattern/tilemap, clipping | Implemented; command-level validation and hardware edge-pixel capture tests may improve confidence |
| Camera, indexed meshes and 3D operations | Public 10-bit angles/mesh interface and renderer available; no MCU-specific fast paths |
| Raster interrupts and collision | Exposed as guest-visible logical state; VM must map IRQ/HSYNC correctly |
| SHOW_FW and built-in logo | Original firmware screen builder and artwork present; separate SHOW_FW pixel-diff certification not asserted |
| 23 firmware egg IDs | Original v1.1.0 C program and public resources present for each supported ID; CF and D0 are TVC-specific |
| Elite C3D spacecraft | Five original source models: Cobra Mk III, Asp Mk II, Sidewinder, Viper, Krait; D4 Asp reused |
| Device snapshots | v13 CRC/invariant validation; **not all egg-private static animation state is captured** |
| Multiple cards simultaneously | Engine state is instance-local, but original egg source contains process-global statics; concurrent independently active egg programs require refactoring |
| RP2354 render-overrun and real-time diagnostic timing | Deliberately not modeled; diagnostics use portable substitutions |

## Hardware comparison

On **22 October 2026**, Károly Vaczkó, creator of 2dfx, reported that all applicable Easter eggs in the browser-based Enterprise and TVC emulator appeared **identical to the physical 2dfx hardware** under manual observation. This is empirical visual feedback, **not** an automated framebuffer pixel diff, exhaustive edge-case sweep, bus-cycle timing verification or multi-device validation.

Native smoke testing checks startup/frame progression of supported egg commands, SHOW_FW and a selection of public API opcode mappings. Platform integrators should also validate their own VM's video overlay timing, profile-specific palettes, snapshot/lifecycle wiring and interactions with other expansion cards, including TVC256+++.

## Third-party content

The embedded Elite/Wriggler-derived showcase data is included as authentic demonstration content; **its rights are not conveyed under the MIT license**. See `ASSET_LICENSES.md`.
