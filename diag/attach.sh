#!/bin/bash
# Attach strace to every process paused by the SCDIAG layer, then release it.
# The game must run with SCDIAG_LOG=<dir>/game.log and SCDIAG_PAUSE=<dir> (same <dir>).
# usage: diag/attach.sh <dir>/game.log [SECONDS_TO_TRACE]
# Writes <dir>/trace.<pid> per traced process. strace comes from the system or devenv.
ROOT=$(cd "$(dirname "$0")/.." && pwd)
LOG=$(realpath "$1"); DUR=${2:-40}
DIR=$(dirname "$LOG")
STRACE=$(command -v strace || (cd "$ROOT" && devenv shell -- bash -c 'command -v strace' 2>/dev/null | tail -1))
[ -x "$STRACE" ] || { echo "strace not found" >&2; exit 1; }
cd "$DIR"
declare -A seen
while true; do
  for ns in $(grep -oP '^PAUSE \K[0-9]+' "$LOG" 2>/dev/null); do
    [ -n "${seen[$ns]}" ] && continue
    seen[$ns]=1
    # map the (possibly namespaced) pid to a host pid of a process that has SCDIAG_PAUSE set
    host=""
    for s in /proc/[0-9]*/status; do
      p=${s#/proc/}; p=${p%/status}
      [ "$(awk '/^NSpid:/{print $NF}' "$s" 2>/dev/null)" = "$ns" ] || continue
      tr '\0' '\n' < /proc/$p/environ 2>/dev/null | grep -q '^SCDIAG_PAUSE=' && { host=$p; break; }
    done
    [ -z "$host" ] && { echo "no host pid for $ns"; touch "go.$ns"; continue; }
    echo "attaching to $ns (host $host): $(tr '\0' ' ' < /proc/$host/cmdline | cut -c1-80)"
    timeout "$DUR" "$STRACE" -f -tt -y -s 200 -o "trace.$ns" -p "$host" &
    sleep 2
    touch "go.$ns"
  done
  sleep 0.5
done
