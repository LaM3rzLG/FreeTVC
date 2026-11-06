# Built-in firmware C API, egg source and asset provenance

The 2dfx v1.1.0 firmware provides a public source-level `twodfx_api.h` for firmware-resident Easter eggs. This module contains the **original v1.1.0 C egg implementation files and SHOW_FW**, along with their headers and artwork, where the original creator authorizes use. The source's public API calls are serviced by `src/core/twodfx_public_api_adapter.inc`, which operates on the emulator's ordinary memory, descriptor, shadow BG/FG 2DPT, sprite and 3D state.

The 23 egg C files are located in `src/firmware_eggs/egg_*.c`. The list and SHA-256 values of exact original source/asset copies appear in `src/firmware_eggs/ORIGIN_SHA256.json`. The portable `src/firmware_eggs/portable/pico/` shims provide clock/entropy placeholders with documented logical behaviour; they do not reproduce MCU implementation details.

**Rights boundary:** Károly Vaczkó permits original-owned egg/SHOW_FW/public API and supporting artwork to be distributed under MIT. No proprietary RP2354 firmware rendering/scheduling fast paths, scanout, DMA, PIO, multicore worker implementation or private `twodfx_api.c` are included. The original Elite ship data and Wriggler imagery may contain third-party rights and are expressly outside the MIT grant: see `ASSET_LICENSES.md`.

SHA-256 digests identify original copied files, not an invitation to copy the full private firmware. The host ABI uses `src/include/twodfx_emu.h`; egg C code uses `src/firmware_eggs/twodfx_api.h`. These are intentionally different interfaces for different integration layers.
