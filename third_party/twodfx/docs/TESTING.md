# Native build and tests

Requirements: CMake 3.16+, a C99 compiler, a C++11 compiler for adapter smoke testing, and the platform math library.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The included tests check that every applicable original egg can start/tick on Enterprise and TVC, SHOW_FW can be invoked, selected C2 ellipse/rounded rectangle opcodes in the **active** 2DPT correspond to the public API, and the C++11 wrapper compiles and links. These are smoke tests, not an exhaustive conformance suite.

For a separate Clang/sanitizer build:

```sh
cmake -S . -B build-san -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_FLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_CXX_FLAGS='-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer' \
  -DCMAKE_EXE_LINKER_FLAGS='-fsanitize=address,undefined'
cmake --build build-san --parallel
ctest --test-dir build-san --output-on-failure
```

An emulator-host integration should additionally test F8/F9 protocol streams, sprite edge pixel ordering, 2DPT skip/clip, resource upload contention, 3D projection and render order, IRQ line behaviour, NICK/TVC palettes, snapshots and TVC256+++ interactions. Mid-egg snapshot resumption and multi-device concurrent eggs are limited as described in `docs/COMPATIBILITY.md`.

The original creator's visual inspection of all applicable eggs against physical 2dfx hardware is documented separately from the automated tests. No synthetic MCU performance counters or render-overrun deadlines are produced.
