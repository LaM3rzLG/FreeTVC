#!/bin/bash
# Builds freeTVC on macOS (Apple Silicon) into build/mac-release; tests are not built
set -e
cd "$(dirname "$0")/../.."
command -v cmake >/dev/null && command -v ninja >/dev/null || brew install cmake ninja
cmake -S . -B build/mac-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DFREETVC_BUILD_TESTS=OFF -DFREETVC_FETCH_ROMS=ON
cmake --build build/mac-release -j"$(sysctl -n hw.ncpu)"
echo "kesz: build/mac-release/src/freetvc"
