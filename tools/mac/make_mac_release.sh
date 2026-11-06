#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# With ffmpeg/ for video recording (tools/release/build_ffmpeg.sh; needs Homebrew's nasm and pkgconf).
# Builds freeTVC-<version>-macos-arm64.zip and freeTVC-<version>-macos-x86_64.zip (macOS 13 or newer, not
# notarized: ad-hoc signed by the linker). usage: tools/mac/make_mac_release.sh <out dir>   (from the project root)
set -eu
root=$(pwd)
[ -f "$root/CMakeLists.txt" ] && [ -f "$root/data/roms.txt" ] || { echo "run from the project root" >&2; exit 1; }
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
version=$(sed -n 's/^ *VERSION \([0-9][0-9.]*\)$/\1/p' CMakeLists.txt | head -n 1)
jobs=$(sysctl -n hw.ncpu)
for tool in cmake ninja nasm pkg-config; do
    command -v $tool >/dev/null || brew install "$([ $tool = pkg-config ] && echo pkgconf || echo $tool)"
done

stage=$out/stage-mac
rm -rf "$stage"
mkdir -p "$stage"
# The same ROM set as the Linux and Windows packages (tools/release/make_release.sh)
cmake -DFREETVC_ROM_OUT="$stage/roms" -P cmake/FreeTVCRoms.cmake
not_bundled="multichip.rom sddual.rom sddual-gm.rom card256-font.bin card256-flash.bin"
for f in $not_bundled; do
    rm -f "$stage/roms/$f"
done
{
    echo "This freeTVC $version package includes these ROM files:"
    (cd "$stage/roms" && ls | grep -v '^ROMS-README.txt$' | tr '\n' ' ')
    echo
    echo "Not included (their licences do not allow it, or none is stated): $not_bundled."
    echo "Download them with the GUI's \"Download ROMs\" item or with freetvc-headless --fetch-roms."
    echo
    cat "$stage/roms/ROMS-README.txt"
} > "$stage/notice.txt"
mv "$stage/notice.txt" "$stage/roms/ROMS-README.txt"

for arch in arm64 x86_64; do
    ffmpeg_dir=build/ffmpeg-out-macos-$arch
    [ -x "$ffmpeg_dir/ffmpeg" ] || sh tools/release/build_ffmpeg.sh "$([ $arch = arm64 ] && echo native || echo macos-x86_64)" "$ffmpeg_dir"
    build=build/release-macos-$arch
    cmake -S . -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DFREETVC_BUILD_TESTS=OFF -DFREETVC_FETCH_ROMS=OFF \
        -DCMAKE_OSX_ARCHITECTURES=$arch -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
    cmake --build "$build" -j"$jobs"
    name=freeTVC-$version-macos-$arch
    dir=$stage/$name
    mkdir -p "$dir"
    cp "$build/src/freetvc" "$build/src/freetvc-headless" "$dir/"
    for d in fonts skins keymaps pictures lang about; do
        cp -R "$build/src/$d" "$dir/$d"
    done
    cp -R "$stage/roms" "$dir/roms"
    cp -R docs "$dir/docs"
    cp README.md LICENSE THIRD_PARTY.md CHANGELOG.md CHANGELOG.hu.md "$dir/"
    cp -R "$ffmpeg_dir" "$dir/ffmpeg"
    strip -x "$dir/freetvc" "$dir/freetvc-headless"
    codesign -f -s - "$dir/freetvc" "$dir/freetvc-headless" "$dir/ffmpeg/ffmpeg"
    rm -f "$out/$name.zip"
    (cd "$stage" && ditto -c -k --keepParent "$name" "$out/$name.zip")
done
rm -rf "$stage"
(cd "$out" && shasum -a 256 freeTVC-$version-macos-*.zip > SHA256SUMS-macos)
ls -l "$out"
