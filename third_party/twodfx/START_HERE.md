# START HERE — Portable 2dfx emulator

For **Károly Kiss and Zoltán Balogh** — the same complete source module is suitable for integration in either ep128emu host implementation.

1. Read `README.md` and `ASSET_LICENSES.md` (the original Elite/Wriggler-derived demonstration content is **not** MIT-licensed).
2. Build the complete C99 library: `cmake -S . -B build && cmake --build build --parallel`.
3. Run its native smoke tests: `ctest --test-dir build --output-on-failure`.
4. Use `src/include/twodfx_emu.h` or the C++11 convenience wrapper `src/adapter/ep128emu_twodfx_adapter.hpp`.
5. Wire VM F8h/F9h port read/write, 50 Hz VSYNC, raw HSYNC scanline, IRQ and the indexed video compositor according to `docs/INTEGRATION.md`.
6. Integrate snapshot format v13 as a distinct peripheral state chunk; see `docs/SNAPSHOT_FORMAT.md` and the mid-egg limitation in `docs/COMPATIBILITY.md`.
7. Run Enterprise and TVC host regressions, including TVC256+++, and compare with physical hardware when practical.

Every production source and asset is under `src/`. No MCU-private firmware or Pico SDK is needed. The original public v1.1.0 egg code and owned artwork are included; the hardware-specific RP2354 renderer, DMA, PIO, multicore workers and timing optimizations are not.
