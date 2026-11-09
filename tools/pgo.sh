#!/bin/sh
# pgo.sh - profile-guided build of the game binary (about an hour).
#  1. instrumented build in build/pgo (PGO=gen)
#  2. three training runs on the dev handheld (FAMILIA_DEV): Free Ride standing and
#     driving, mission 1, Free Ride driving; counters dumped after 100 s each into
#     <port>/pgo via GCOV_PREFIX and added together
#  3. counters copied to build/pgo-data, optimised build in build/pgo (PGO=use)
# Result: build/pgo/familia (package with tools/package.sh build/pgo/familia).
set -eu
cd "$(dirname "$0")/.."
D=/mnt/sdcard/ports/the_familia
DOCKER="docker run --rm -u $(id -u):$(id -g) -v $PWD:/src -w /src familia-cross:bookworm"
rm -rf build/pgo build/pgo-data
$DOCKER make ARCH=aarch64 OUT=build/pgo PGO=gen -j22
$DOCKER aarch64-linux-gnu-strip -o build/familia.pgogen build/pgo/familia
tools/dev.sh "rm -rf $D/pgo; mkdir -p $D/pgo; cat > $D/familia.aarch64.tmp && chmod +x $D/familia.aarch64.tmp && mv $D/familia.aarch64.tmp $D/familia.aarch64" < build/familia.pgogen
# three training sessions, counters added together (gcov merges into the same .gcda):
# Free Ride standing then driving, mission 1 (night chase), Free Ride driving
ENV="GCOV_PREFIX=$D/pgo; GCOV_PREFIX_STRIP=0; FAMILIA_GCOV_DUMP=100; FAMILIA_MOVIES=0; export GCOV_PREFIX GCOV_PREFIX_STRIP FAMILIA_GCOV_DUMP FAMILIA_MOVIES"
for S in "8:enter,13:down,14:down,15:down,16:down,18:enter,24:enter,80:up:15" \
         "8:enter,13:down,14:down,16:enter,22:enter,32:space,38:space,44:enter,50:space,56:enter,62:space" \
         "8:enter,13:down,14:down,15:down,16:down,18:enter,24:enter,40:up:30,72:left:1,74:up:20"; do
  tools/dev2_shoot.sh 115 30 "$S" "$ENV"
  grep -q "\[pgo\] profile written" build/logs/dev_last.txt || { echo "training run wrote no profile"; exit 1; }
done
mkdir -p build/pgo-data
tools/dev.sh "cd $D/pgo/src/build/pgo-data && tar cf - ." | tar xf - -C build/pgo-data
tools/dev.sh "rm -rf $D/pgo"
find build/pgo -name '*.o' -delete
$DOCKER make ARCH=aarch64 OUT=build/pgo PGO=use -j22
echo "build/pgo/familia: profile-guided build"
