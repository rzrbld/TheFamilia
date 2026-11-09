#!/bin/bash
# The Familia - Mafia: The City of Lost Heaven (2002) for PortMaster.
# Structure follows the PortMaster "Basic Launchscript" template.

XDG_DATA_HOME=${XDG_DATA_HOME:-$HOME/.local/share}

if [ -d "/opt/system/Tools/PortMaster/" ]; then
  controlfolder="/opt/system/Tools/PortMaster"
elif [ -d "/opt/tools/PortMaster/" ]; then
  controlfolder="/opt/tools/PortMaster"
elif [ -d "$XDG_DATA_HOME/PortMaster/" ]; then
  controlfolder="$XDG_DATA_HOME/PortMaster"
else
  controlfolder="/roms/ports/PortMaster"
fi

source $controlfolder/control.txt
[ -f "${controlfolder}/mod_${CFW_NAME}.txt" ] && source "${controlfolder}/mod_${CFW_NAME}.txt"
get_controls

GAMEDIR=/$directory/ports/the_familia
CONFDIR="$GAMEDIR/conf"
mkdir -p "$CONFDIR"
cd "$GAMEDIR"

> "$GAMEDIR/log.txt" && exec > >(tee "$GAMEDIR/log.txt") 2>&1

export XDG_DATA_HOME="$CONFDIR"
export LD_LIBRARY_PATH="$GAMEDIR/libs.${DEVICE_ARCH}:$LD_LIBRARY_PATH"
export SDL_GAMECONTROLLERCONFIG="$sdl_controllerconfig"

# 1. Launcher: first run imports the user's own game files from source/
#    (see README); later runs show settings. 0 = start, 10 = the user left.
export FAMILIA_7Z="$controlfolder/7zzs.${DEVICE_ARCH}"
"$GAMEDIR/familia-launcher.${DEVICE_ARCH}" --root "$GAMEDIR"
launcher_rc=$?
if [ "$launcher_rc" != 0 ]; then
  [ "$launcher_rc" != 10 ] && echo "[the_familia] launcher failed ($launcher_rc)"
  pm_finish
  exit 0
fi

# 2. Game. Saves, game config (registry), screenshots: conf/. data/ is never written.
export FAMILIA_USER_DIR="$CONFDIR"
[ -f "$CONFDIR/familia.cfg" ] && source "$CONFDIR/familia.cfg"
export FAMILIA_FPS=${FAMILIA_FPS:-1} FAMILIA_LIMIT=${FAMILIA_LIMIT:-0} FAMILIA_TEXTURES=${FAMILIA_TEXTURES:-32}
export FAMILIA_STATS=${FAMILIA_STATS:-0}     # 1: performance lines in log.txt every 5 s
[ -f "$CONFDIR/dev.cfg" ] && source "$CONFDIR/dev.cfg"   # developer switches (e.g. FAMILIA_PROF=1)

# CPU: the game runs on one thread and is CPU-bound. "perf" holds the cores
# at their rated speed (performance governor, capped at 1512 MHz: the H700's
# rating, no overclock) instead of letting the governor wander; everything is
# restored on exit. FAMILIA_CPU=system leaves the firmware's settings alone.
CPU_SAVED=""
cpu_restore() {
  local entry pol gov max
  for entry in $CPU_SAVED; do
    pol=${entry%%=*}; gov=${entry#*=}; max=${gov#*,}; gov=${gov%%,*}
    echo "$max" | $ESUDO tee "$pol/scaling_max_freq" >/dev/null 2>&1
    echo "$gov" | $ESUDO tee "$pol/scaling_governor" >/dev/null 2>&1
  done
  CPU_SAVED=""
}
cpu_perf() {
  local pol cap
  for pol in /sys/devices/system/cpu/cpufreq/policy*; do
    [ -w "$pol/scaling_governor" ] || [ -n "$ESUDO" ] || continue
    grep -qw performance "$pol/scaling_available_governors" 2>/dev/null || continue
    CPU_SAVED="$CPU_SAVED $pol=$(cat "$pol/scaling_governor"),$(cat "$pol/scaling_max_freq")"
    cap=$(cat "$pol/cpuinfo_max_freq"); [ "$cap" -gt 1512000 ] && cap=1512000
    echo "$cap" | $ESUDO tee "$pol/scaling_max_freq" >/dev/null 2>&1
    echo performance | $ESUDO tee "$pol/scaling_governor" >/dev/null 2>&1
  done
  echo "[the_familia] cpu: performance, max $cap kHz (restored on exit)"
}
trap cpu_restore EXIT
[ "${FAMILIA_CPU:-perf}" = perf ] && cpu_perf

# Controller: gptokeyb2 presents the handheld's controls as a virtual Xbox 360
# pad, which the game reads as its joystick (in-game Options > Controls can
# rebind it). Without gptokeyb2 the game reads the pad itself; gptokeyb then
# only provides the hotkey exit (familia.gptk maps nothing).
if [ -n "$GPTOKEYB2" ]; then
  $GPTOKEYB2 "familia.${DEVICE_ARCH}" -x &
else
  $GPTOKEYB "familia.${DEVICE_ARCH}" -c "$GAMEDIR/familia.gptk" &
fi
pm_platform_helper "$GAMEDIR/familia.${DEVICE_ARCH}"
if [ -n "$FAMILIA_TIMEOUT" ]; then        # automated tests only
  timeout -k 5 "$FAMILIA_TIMEOUT" ./familia.${DEVICE_ARCH} data
else
  ./familia.${DEVICE_ARCH} data
fi

cpu_restore
pm_finish
