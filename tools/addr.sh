#!/bin/sh
# addr.sh OFFSET... : resolve executable offsets from a crash report (exe+0x...)
# against the unstripped build/aarch64/familia.
cd "$(dirname "$0")/.." || exit 1
docker run --rm -v "$PWD/build/aarch64:/b" familia-cross:bookworm aarch64-linux-gnu-addr2line -f -C -e /b/familia "$@"
