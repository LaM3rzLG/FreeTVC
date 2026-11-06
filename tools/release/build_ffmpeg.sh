#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Builds the small ffmpeg the release packages carry for video recording (raw RGB and float sound in, H.264 + AAC
# MP4 out; GPL, with x264). usage: tools/release/build_ffmpeg.sh native|macos-x86_64|mingw <out dir>
# Writes <out dir>/ffmpeg[.exe], the licences and README.txt with the sources. Needs nasm and pkg-config.
set -eu
target=${1:?target: native, macos-x86_64 or mingw}
root=$(pwd)
out=$(mkdir -p "${2:?out dir}" && cd "$2" && pwd)
ffmpeg_version=7.1.1
x264_commit=b35605ace3ddf7c1a5d67a2eb553f034aef41d55   # x264 branch "stable", 2025-06-08
ffmpeg_url=https://ffmpeg.org/releases/ffmpeg-$ffmpeg_version.tar.xz
x264_url=https://code.videolan.org/videolan/x264/-/archive/$x264_commit/x264-$x264_commit.tar.bz2
jobs=$( (nproc || sysctl -n hw.ncpu) 2>/dev/null)

src=$root/build/ffmpeg-src
work=$root/build/ffmpeg-$target
mkdir -p "$src"
[ -f "$src/ffmpeg-$ffmpeg_version.tar.xz" ] || curl -fsSL -o "$src/ffmpeg-$ffmpeg_version.tar.xz" "$ffmpeg_url"
[ -f "$src/x264-$x264_commit.tar.bz2" ] || curl -fsSL -o "$src/x264-$x264_commit.tar.bz2" "$x264_url"
rm -rf "$work"
mkdir -p "$work/prefix"
prefix=$work/prefix
tar -C "$work" -xJf "$src/ffmpeg-$ffmpeg_version.tar.xz"
tar -C "$work" -xjf "$src/x264-$x264_commit.tar.bz2"

x264_flags=""
ffmpeg_flags=""
exe=""
case $target in
native)
    [ "$(uname)" = Darwin ] && export MACOSX_DEPLOYMENT_TARGET=13.0
    [ "$(uname)" = Linux ] && ffmpeg_flags="--extra-ldflags=-static --pkg-config-flags=--static"
    ;;
macos-x86_64)
    export MACOSX_DEPLOYMENT_TARGET=13.0 CFLAGS="-arch x86_64" LDFLAGS="-arch x86_64"
    x264_flags="--host=x86_64-apple-darwin"
    ffmpeg_flags="--enable-cross-compile --arch=x86_64 --target-os=darwin"
    ;;
mingw)
    x264_flags="--host=x86_64-w64-mingw32 --cross-prefix=x86_64-w64-mingw32-"
    ffmpeg_flags="--enable-cross-compile --arch=x86_64 --target-os=mingw32 --cross-prefix=x86_64-w64-mingw32- --extra-ldflags=-static --pkg-config=pkg-config --pkg-config-flags=--static"
    exe=.exe
    ;;
*) echo "unknown target $target" >&2; exit 2 ;;
esac

cd "$work/x264-$x264_commit"
# shellcheck disable=SC2086
./configure --prefix="$prefix" --enable-static --disable-cli --enable-pic --bit-depth=8 --chroma-format=420 $x264_flags
make -j"$jobs" install

cd "$work/ffmpeg-$ffmpeg_version"
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig" PKG_CONFIG_LIBDIR="$prefix/lib/pkgconfig"
extra_c="-I$prefix/include ${CFLAGS:-}"
extra_ld="-L$prefix/lib ${LDFLAGS:-}"
unset CFLAGS LDFLAGS
# shellcheck disable=SC2086
./configure --prefix="$prefix" --extra-cflags="$extra_c" --extra-ldflags="$extra_ld" $ffmpeg_flags \
    --disable-everything --disable-autodetect --disable-network --disable-doc --disable-debug \
    --disable-ffplay --disable-ffprobe --enable-gpl --enable-libx264 \
    --enable-encoder=libx264,aac --enable-decoder=rawvideo,pcm_f32le \
    --enable-demuxer=rawvideo,pcm_f32le,mov --enable-muxer=mp4 \
    --enable-protocol=file,pipe,fd --enable-filter=scale,format,aformat,aresample,null,anull
make -j"$jobs"
cp "ffmpeg$exe" "$out/"
case $target in mingw) x86_64-w64-mingw32-strip "$out/ffmpeg.exe" ;; *) strip "$out/ffmpeg" 2>/dev/null || true ;; esac
cp COPYING.GPLv2 "$out/COPYING.GPLv2"
cp "$work/x264-$x264_commit/COPYING" "$out/COPYING.x264"
cat > "$out/README.txt" <<EOF
ffmpeg for freeTVC's video recording: FFmpeg $ffmpeg_version with x264 (GPL version 2 or later, COPYING.GPLv2,
COPYING.x264), built by freeTVC's tools/release/build_ffmpeg.sh with only what the recording needs.
Sources: $ffmpeg_url
         $x264_url
The same source archives are attached to the freeTVC release on https://github.com/LaM3rzLG/FreeTVC/releases.
EOF
ls -l "$out"
