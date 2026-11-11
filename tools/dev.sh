#!/bin/sh
# dev.sh CMD... : run a command on the development handheld over SSH
# (FAMILIA_DEV=user@host, required). Password auth can go through SSH_ASKPASS
# (FAMILIA_DEV_ASKPASS); one control master is reused.
DEV=${FAMILIA_DEV:?set FAMILIA_DEV=user@host of the development handheld}
# hosts this machine must not use (one per line, e.g. a handheld lent to other work)
if [ -f "$HOME/.cache/familia/deny_hosts" ] && grep -qxF "${DEV#*@}" "$HOME/.cache/familia/deny_hosts"; then
  echo "dev.sh: ${DEV#*@} is in ~/.cache/familia/deny_hosts: not connecting" >&2; exit 97
fi
export SSH_ASKPASS=${FAMILIA_DEV_ASKPASS:-$HOME/.cache/familia/askpass.sh} SSH_ASKPASS_REQUIRE=force
exec ssh -o StrictHostKeyChecking=accept-new -o ControlMaster=auto -o ControlPath=${TMPDIR:-/tmp}/familia-dev-%C -o ControlPersist=600 -o ConnectTimeout=10 "$DEV" "$@"
