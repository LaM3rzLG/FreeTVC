# Portable 2dfx emulator — Enterprise and TVC

This package contains a complete, portable **C99 behavioural emulator** of the public 2dfx expansion-card interface, with a small optional C++11 connector for emulator hosts such as **ep128emu**. The publicly supported 2dfx v1.1.0 C Easter eggs and SHOW_FW use the portable implementation of `twodfx_api.h` and the same raster engine as host commands.

**Author:** Károly Vaczkó, original 2dfx hardware/firmware creator. Portable emulator produced with AI-assisted development. MIT license applies to original-owned and independently implemented code only; see `src/firmware_eggs/LICENSE_SCOPE.md` for the egg-source grant and `ASSET_LICENSES.md` for restrictions affecting third-party game artwork.

## Package contents

| Location | Contents |
|---|---|
| `src/include/twodfx_emu.h` | Public, stable C99 host ABI |
| `src/core/` | Command parser, resources, sprites, drawing, 3D, public API adapter, renderer |
| `src/firmware_eggs/` | All 23 original v1.1.0 egg C implementations and SHOW_FW, original headers/assets, public `twodfx_api.h`, provenance manifest, portable dependency shims |
| `src/assets/` | Logo and original artwork used by the module |
| `src/adapter/` | Optional C++11 header-only VM connector |
| `docs/` | Integration, public commands, snapshots, compatibility, testing and provenance |
| `tests/` | Native smoke tests, not compiled into the production library |

**Every source or asset required to compile the production library is contained in `src/`.** No Pico SDK, RP2354 hardware source, generated code download, or external firmware archive is needed.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The production target is `twodfx_emu` (a static C99 library). When embedded with CMake `add_subdirectory`, tests are off by default; use `-DTWODFX_BUILD_TESTS=ON` to enable them explicitly. For non-CMake builds, see `docs/INTEGRATION.md` for the exact source list and include paths. C99 and the C math library are required; the adapter requires C++11 but the engine itself does not.

## Emulator interface

The host routes Z80 F8h/F9h port accesses, 50 Hz VSYNC, scanline HSYNC, IRQ levels and output colour indices/coverage through the documented API. Shadow state is published at accepted frame boundaries. Device state snapshots use format v13. The VM is responsible for scanout timing, palette/NICK/TVC compositing and reset-lifecycle wiring; see `docs/INTEGRATION.md`.

## Fidelity and scope

The source-controlled eggs are the original 2dfx **v1.1.0 C programs** (including Cobra Mk III, Asp Mk II, Sidewinder, Viper and Krait data). Their public `twodfx_api` calls are implemented over the normal emulator state. On **22 October 2026**, the original hardware creator reported a manual visual comparison of all applicable egg commands on browser-emulated Enterprise and TVC against physical 2dfx hardware and found the visuals identical. This is a manual visual observation, not an automated pixel-diff or timing certification.

The emulator models **behaviour**, not RP2354 CPU execution time. Real-time MCU PIO, DMA, core scheduling, fast paths and render-overrun duration are intentionally absent. Running eggs use process-global module static state; concurrent independent cards and snapshot restores in the middle of an egg animation are not guaranteed to preserve all egg-private state. Details: `docs/COMPATIBILITY.md`.

This is a standalone device-library source distribution. The host developers integrate the documented connector into their own Enterprise/TVC VM branches; the library does not replace or modify TVC256+++ or unrelated peripherals.

Read `START_HERE.md`, then `COMPLETE_2DFX_HANDOFF.md` for integration steps and deliverable details.
