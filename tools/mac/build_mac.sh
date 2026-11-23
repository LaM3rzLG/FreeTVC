#!/bin/bash
# Builds freeTVC on macOS (Apple Silicon) into build/mac-release; tests are not built
# TVCnet's firmware code comes from its pinned GitHub commit; TVCNET_DIR=<checkout> uses a local copy instead
set -e
cd "$(dirname "$0")/../.."
command -v cmake >/dev/null && command -v ninja >/dev/null || brew install cmake ninja
cmake -S . -B build/mac-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DFREETVC_BUILD_TESTS=OFF -DFREETVC_FETCH_ROMS=ON \
    -DFETCHCONTENT_SOURCE_DIR_TVCNET="${TVCNET_DIR:-}"
cmake --build build/mac-release -j"$(sysctl -n hw.ncpu)"
echo "kesz: build/mac-release/src/freetvc"
