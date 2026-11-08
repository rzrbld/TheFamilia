#!/bin/sh
# build_ffmpeg.sh - the minimal FFmpeg the port ships (LGPL, shared): AVI and
# Bink demuxers; Indeo 5, MS ADPCM and Bink video/audio decoders; swscale and
# swresample. Output: build/ffmpeg/{include,lib} (tools/package.sh copies the
# libraries into libs.aarch64).
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
VER=6.1.2
SRC=${FFMPEG_SRC:-$ROOT/build/ffmpeg-src}
mkdir -p "$SRC"
[ -d "$SRC/ffmpeg-$VER" ] || { curl -sSL -o "$SRC/ffmpeg-$VER.tar.xz" "https://ffmpeg.org/releases/ffmpeg-$VER.tar.xz"; tar -C "$SRC" -xf "$SRC/ffmpeg-$VER.tar.xz"; }
docker run --rm -u "$(id -u):$(id -g)" -v "$SRC:/w" -w "/w/ffmpeg-$VER" familia-cross:bookworm sh -c '
  ./configure --prefix=/w/out --enable-cross-compile --arch=aarch64 --target-os=linux --cross-prefix=aarch64-linux-gnu- \
    --enable-shared --disable-static --enable-pic --disable-programs --disable-doc --disable-network --disable-autodetect \
    --disable-everything --disable-avdevice --disable-avfilter --disable-postproc \
    --enable-avformat --enable-avcodec --enable-swscale --enable-swresample --enable-protocol=file \
    --enable-demuxer=avi,bink --enable-decoder=indeo5,adpcm_ms,bink,binkaudio_rdft,binkaudio_dct \
    --extra-cflags="-O2 -mcpu=cortex-a53" && make -j"$(nproc)" && make install'
rm -rf "$ROOT/build/ffmpeg"; mkdir -p "$ROOT/build/ffmpeg"
cp -a "$SRC/out/include" "$SRC/out/lib" "$ROOT/build/ffmpeg/"
echo "build/ffmpeg ready"
