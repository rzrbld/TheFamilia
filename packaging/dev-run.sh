#!/bin/sh
# SSH test helper (development only): runs the real launcher for N seconds
# while muOS's frontend UI and its loop are paused (SIGSTOP), then resumes them.
# Usage: dev-run.sh [seconds]   (env FAMILIA_* is passed through)
PORTS_DIR=$(dirname "$(dirname "$(readlink -f "$0")")")      # <card>/ports
CARD=$(dirname "$PORTS_DIR")
LAUNCHER="$CARD/ROMS/Ports/The Familia.sh"
PAUSED=
# muOS 2506+: /opt/muos/frontend/muxfrontend; older (2502): /opt/muos/extra/mux*
for p in $(pgrep -f '^/opt/muos/frontend/muxfrontend') $(pgrep -f '^/opt/muos/extra/mux') $(pgrep -f '^/bin/sh /opt/muos/script/mux/frontend.sh'); do
  kill -STOP "$p" 2>/dev/null && PAUSED="$PAUSED $p"
done
trap 'for p in $PAUSED; do kill -CONT "$p" 2>/dev/null; done' EXIT
trap 'exit 130' INT TERM HUP
FAMILIA_AUTOSTART=1 FAMILIA_TIMEOUT=${1:-60} bash "$LAUNCHER"
