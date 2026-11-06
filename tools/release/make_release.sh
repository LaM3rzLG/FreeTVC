#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the release packages freeTVC-<version>-linux-x86_64.tar.gz and freeTVC-<version>-windows-x64.zip,
# RELEASE-NOTES.md and SHA256SUMS. usage: tools/release/make_release.sh <out dir>   (from the project root)
# Env: JOBS = parallel build jobs (default: CMake's), SKIP_TESTS=1 skips the Linux test run.
set -eu
root=$(pwd)
[ -f "$root/CMakeLists.txt" ] && [ -f "$root/data/roms.txt" ] || { echo "run from the project root" >&2; exit 1; }
out=$(mkdir -p "${1:?out dir}" && cd "$1" && pwd)
version=$(sed -n 's/^ *VERSION \([0-9][0-9.]*\)$/\1/p' CMakeLists.txt | head -n 1)
[ -n "$version" ] || { echo "no project VERSION in CMakeLists.txt" >&2; exit 1; }
jobs=${JOBS:+--parallel $JOBS}
echo "freeTVC $version -> $out"

# Release builds in their own folders; libstdc++ and libgcc linked in, so Linux needs only glibc
cmake --preset gcc-release -B build/release-linux -DCMAKE_EXE_LINKER_FLAGS="-static-libstdc++ -static-libgcc"
cmake --build build/release-linux $jobs
cmake --preset mingw-release -B build/release-windows -DFREETVC_BUILD_TESTS=OFF
cmake --build build/release-windows $jobs
if [ "${SKIP_TESTS:-0}" != 1 ]; then
    ctest --test-dir build/release-linux --output-on-failure -LE slow $jobs
fi

# The ROM list, then only what a release may carry; the rest stays with the download
stage=$out/stage
rm -rf "$stage"
mkdir -p "$stage"
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

# One folder per platform: programs, the GUI's data folders, the ROMs, the docs
package() {  # <build dir> <folder name> <program suffix>
    dir=$stage/$2
    mkdir -p "$dir"
    cp "$1/src/freetvc$3" "$1/src/freetvc-headless$3" "$dir/"
    for d in fonts skins keymaps pictures lang about; do
        cp -R "$1/src/$d" "$dir/$d"
    done
    cp -R "$stage/roms" "$dir/roms"
    cp -R docs "$dir/docs"
    cp README.md LICENSE THIRD_PARTY.md CHANGELOG.md CHANGELOG.hu.md "$dir/"
}
linux=freeTVC-$version-linux-x86_64
windows=freeTVC-$version-windows-x64
package build/release-linux "$linux" ""
package build/release-windows "$windows" ".exe"
strip "$stage/$linux/freetvc" "$stage/$linux/freetvc-headless"
x86_64-w64-mingw32-strip "$stage/$windows/freetvc.exe" "$stage/$windows/freetvc-headless.exe"

rm -f "$out/$linux.tar.gz" "$out/$windows.zip"
tar -C "$stage" --owner=0 --group=0 --numeric-owner --sort=name -czf "$out/$linux.tar.gz" "$linux"
(cd "$stage" && zip -q -r -X "$out/$windows.zip" "$windows")

# The release notes: CHANGELOG.md's section for this version
awk -v v="## $version" 'index($0, v) == 1 { on = 1; next } on && /^## / { exit } on' CHANGELOG.md > "$out/RELEASE-NOTES.md"
[ -s "$out/RELEASE-NOTES.md" ] || { echo "CHANGELOG.md has no \"## $version\" section" >&2; exit 1; }

(cd "$out" && sha256sum "$linux.tar.gz" "$windows.zip" > SHA256SUMS)
rm -rf "$stage"
ls -l "$out"
