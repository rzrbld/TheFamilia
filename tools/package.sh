#!/bin/sh
# package.sh - assemble the PortMaster package in dist/ (no game files).
#   dist/the_familia.zip:  The Familia.sh, the_familia/{familia.aarch64,
#   familia-launcher.aarch64, familia-import.aarch64, familia.gptk,
#   libs.aarch64/, licenses/, README.md, port.json, gameinfo.xml, screenshot.png}
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ELF=${1:-build/aarch64/familia}     # the unstripped game binary (tools/pgo.sh: build/pgo/familia)
OUT=$ROOT/dist/stage
rm -rf "$OUT"; mkdir -p "$OUT/the_familia/libs.aarch64" "$OUT/the_familia/licenses"
cp "$ROOT/packaging/The Familia.sh" "$OUT/"
P=$OUT/the_familia
docker run --rm -u "$(id -u):$(id -g)" -v "$ROOT:/src" -e ELF="$ELF" familia-cross:bookworm sh -c '
  aarch64-linux-gnu-strip -o dist/stage/the_familia/familia.aarch64 "$ELF"
  cp build/familia-launcher.aarch64 build/familia-import.aarch64 dist/stage/the_familia/
  aarch64-linux-gnu-strip dist/stage/the_familia/familia-launcher.aarch64 dist/stage/the_familia/familia-import.aarch64
  for l in libvorbisfile.so.3 libvorbis.so.0 libogg.so.0; do cp -L /usr/lib/aarch64-linux-gnu/$l dist/stage/the_familia/libs.aarch64/$l; done
  cp /usr/share/doc/libvorbis0a/copyright dist/stage/the_familia/licenses/LICENSE-libvorbis.txt
  cp /usr/share/doc/libogg0/copyright dist/stage/the_familia/licenses/LICENSE-libogg.txt'
cp "$ROOT/packaging/familia.gptk" "$ROOT/packaging/shaders.seed" "$P/"
mkdir -p "$P/fonts" && cp "$ROOT/packaging/fonts/SpecialElite-Regular.ttf" "$P/fonts/"
cp "$ROOT/packaging/licenses/LICENSE-SpecialElite.txt" "$P/licenses/"
cp "$ROOT/packaging/licenses/LICENSE-gptokeyb.txt" "$P/licenses/"
# the movie player's minimal FFmpeg (tools/build_ffmpeg.sh), LGPL 2.1, shared
for l in libavformat.so.60 libavcodec.so.60 libswscale.so.7 libswresample.so.4 libavutil.so.58; do
  cp -L "$ROOT/build/ffmpeg/lib/$l" "$P/libs.aarch64/$l"
done
cp "${FFMPEG_SRC:-$ROOT/build/ffmpeg-src}/ffmpeg-6.1.2/COPYING.LGPLv2.1" "$P/licenses/LICENSE-ffmpeg-LGPL-2.1.txt"
cp "$ROOT/third_party/pcrecomp/LICENSE" "$P/licenses/LICENSE-pcrecomp.txt"
cp "$ROOT/LICENSE" "$P/licenses/LICENSE-the_familia.txt"
cp "$ROOT/THIRD_PARTY.md" "$P/licenses/THIRD_PARTY.md"
cat > "$P/licenses/FFMPEG-SOURCE.txt" <<'TXT'
The libav*.so and libsw*.so libraries in libs.aarch64 are FFmpeg 6.1.2, built
unmodified from https://ffmpeg.org/releases/ffmpeg-6.1.2.tar.xz as shared
libraries (LGPL 2.1 or later, see LICENSE-ffmpeg-LGPL-2.1.txt). The exact
configuration is in tools/build_ffmpeg.sh of https://github.com/rzrbld/TheFamilia.
They are loaded dynamically and can be replaced with any compatible build.
TXT
cp "$ROOT/packaging/portmaster/README.md" "$ROOT/packaging/portmaster/port.json" "$ROOT/packaging/portmaster/gameinfo.xml" "$P/"
[ -f "$ROOT/packaging/portmaster/screenshot.png" ] && cp "$ROOT/packaging/portmaster/screenshot.png" "$P/"
mkdir -p "$P/source" && echo "Put your own Mafia files here (see ../README.md)." > "$P/source/PUT_YOUR_GAME_FILES_HERE.txt"
(cd "$OUT" && rm -f ../the_familia.zip && zip -qr ../the_familia.zip "The Familia.sh" the_familia)
echo "dist/the_familia.zip:"; unzip -l "$ROOT/dist/the_familia.zip" | tail -n +4 | head -40
