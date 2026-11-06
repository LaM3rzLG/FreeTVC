# Portable 2dfx — ep128emu host integration

The C99 core has no dependency on ep128emu. Compile it as the `twodfx_emu` CMake target or include the following **28 production C translation units** in your build:

```text
src/core/twodfx_emu.c
src/core/twodfx_raster.c
src/core/twodfx_3d.c
src/assets/logo_2dfx_asset.c
src/firmware_eggs/firmware_page.c
src/firmware_eggs/egg_C0_logo.c
src/firmware_eggs/egg_C1_benchmark.c
src/firmware_eggs/egg_C2_primitives.c
src/firmware_eggs/egg_C3_text.c
src/firmware_eggs/egg_C4_blit.c
src/firmware_eggs/egg_C5_pattern.c
src/firmware_eggs/egg_C6_tilemap.c
src/firmware_eggs/egg_C7_pong_basic.c
src/firmware_eggs/egg_C8_pong_wow.c
src/firmware_eggs/egg_C9_wriggler.c
src/firmware_eggs/egg_CA_boulder.c
src/firmware_eggs/egg_CB_arcpie_benchmark.c
src/firmware_eggs/egg_CC_diag.c
src/firmware_eggs/egg_CD_blit_full.c
src/firmware_eggs/egg_CE_layer.c
src/firmware_eggs/egg_CF_tvc_bitpattern.c
src/firmware_eggs/egg_D0_tvc_crisp_tearing.c
src/firmware_eggs/egg_D1_ruler.c
src/firmware_eggs/egg_D2_xy_offset.c
src/firmware_eggs/egg_D3_elite_showcase.c
src/firmware_eggs/egg_D4_3d_scope.c
src/firmware_eggs/egg_D5_public_mesh.c
src/firmware_eggs/egg_DF_graphics_test.c
```

Use C99 with include paths `src/include`, `src/core`, `src/assets`, `src/firmware_eggs`, `src/firmware_eggs/portable`; link the C math library (`-lm` on Unix-like hosts). `twodfx_public_api_adapter.inc` and `twodfx_source_eggs.inc` are compiled into `twodfx_emu.c` via local `#include` and **must not be compiled independently**. Assets and original headers reside entirely under `src/`.

The C++11 header-only adapter `src/adapter/ep128emu_twodfx_adapter.hpp` is an optional convenience layer with no ep128emu headers; a host may instead use `src/include/twodfx_emu.h` directly.

## Device event contract

| Host event | Library call / requirement |
|---|---|
| Create Enterprise or TVC device | `twodfx_emu_create(TWODFX_EMU_ENTERPRISE)` or `TWODFX_EMU_TVC` |
| Dispose | `twodfx_emu_destroy()` |
| Z80 OUT F8h | `twodfx_emu_write_f8()` |
| Z80 OUT F9h | `twodfx_emu_write_f9()` |
| Z80 IN F8h | `twodfx_emu_read_f8()`; active-low result |
| Z80 IN F9h | Not a 2dfx result port; use host's normal bus handling |
| Every physical VSYNC | `twodfx_emu_vsync()`, at the computer's 50 Hz cadence including 25-fps engine setting |
| Every raw scanline | `twodfx_emu_hsync(device, raw_scanline)` |
| Interrupt | OR `twodfx_emu_irq_asserted()` with other VM IRQ contributors |
| Scanout | `twodfx_emu_overlay_raw_line()` on raw coordinates, or `twodfx_emu_overlay_line()` for device framebuffer-local coordinates |
| Physical card power cycle | `twodfx_emu_power_cycle()`; distinct from CPU soft reset |
| Device state | `twodfx_emu_snapshot_size/save_snapshot/load_snapshot` as a separate v13 chunk |

### Coordinate and pixel contract

`overlay_line` uses local framebuffer coordinates: Enterprise local (0,0) is raw (88,16), TVC local (0,0) is raw (0,41). `overlay_raw_line` accepts raw host coordinates and handles the stored-frame offset. The C API returns indexed 4-bit colours and a 0/1 coverage mask. Composite the overlay **before** the machine's indexed-to-RGB conversion; preserve Enterprise NICK and TVC palette semantics. Respect OFF, NORMAL (transparent key) and FORCED modes. The VM decides the final viewport and priority relative to unrelated overlays.

### Shadow publication

Sprite, descriptor and BG/FG 2DPT writes update shadow state; publication occurs at accepted VSYNC frames. Camera pose is likewise frame latched. The original eggs use public `twodfx_api.h` against that same model. Resource RAM is a logical 8 MiB address space; no Pico SDK or MCU timing services are needed by the host.

### Snapshots and lifecycle

The saved peripheral chunk uses **format v13 only**. A state of another format is not a valid v13 chunk; reject it explicitly. The original egg modules use file-static internal animation state not completely serialized by the device API; avoid promising mid-egg resumption or concurrently independent egg state until that limitation is resolved. A guest soft reset should not automatically invoke `twodfx_emu_power_cycle()`.

### Integration guidance

Use separate Enterprise and TVC VM-port hooks but the same core. Preserve and regress TVC256+++ and unrelated expansion-card code. The supplied module does **not** alter ep128emu source or require a particular version of the VM; adapt the host to its current I/O, VSYNC/HSYNC, compositor and snapshot abstractions.
