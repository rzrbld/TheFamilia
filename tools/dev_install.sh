#!/bin/sh
# dev_install.sh [BINARY] - install a build on the development handheld
# (FAMILIA_DEV): stripped, sent compressed, checked by SHA-256 on the device
# before it replaces familia.aarch64 (an interrupted upload never lands).
set -e
cd "$(dirname "$0")/.."
BIN=${1:-build/aarch64/familia}
D=/mnt/sdcard/ports/the_familia
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/src" -w /src familia-cross:bookworm aarch64-linux-gnu-strip -o build/dev_install.bin "$BIN"
H=$(sha256sum build/dev_install.bin | cut -c1-64)
gzip -1 -c build/dev_install.bin > build/dev_install.bin.gz
SSH_ASKPASS=${FAMILIA_DEV_ASKPASS:-$HOME/.cache/familia/askpass.sh} SSH_ASKPASS_REQUIRE=force \
  ssh -o ControlMaster=no -o ControlPath=none -o ServerAliveInterval=10 -o ServerAliveCountMax=3 -o ConnectTimeout=30 "${FAMILIA_DEV:?set FAMILIA_DEV}" "if pidof familia.aarch64 >/dev/null; then echo 'the game is running on the handheld' >&2; exit 1; fi
  rm -f $D/familia.aarch64.tmp; gzip -dc > $D/familia.aarch64.tmp && echo '$H  $D/familia.aarch64.tmp' | sha256sum -c - >/dev/null &&
  chmod +x $D/familia.aarch64.tmp && mv $D/familia.aarch64.tmp $D/familia.aarch64 && echo 'installed ${H%????????????????????????????????????????????????}'" < build/dev_install.bin.gz
