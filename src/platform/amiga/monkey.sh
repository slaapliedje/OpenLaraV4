#!/bin/bash
# Run a scripted headless game in the emulator and screenshot it if it stalls (crash requester).
#   [BIN=OpenLara] ./monkey.sh LEVEL SCRIPT NAME [FRAMES]      e.g. ./monkey.sh DATA/LEVEL1.PHD monkey1.txt mk1 3000
H=$(cd "$(dirname "$0")" && pwd)
O=$(cd "$H/../../.." && pwd)/emu-run/out
L=$1; S=$2; N=${3:?name}; F=${4:-3000}
find "$O" -maxdepth 1 -name "$N-*" -delete
SCRIPT=$S TIMEOUT=${TIMEOUT:-1200} "$H/emu.sh" "$L" "$F" "$N" > "$O/$N.run" 2>&1 &
pid=$!
last=""
while kill -0 $pid 2>/dev/null; do
  sleep 20
  cur=$(wc -l < "$O/$N.txt" 2>/dev/null)
  if [ -n "$last" ] && [ "$cur" = "$last" ]; then
    import -display :${EMU_DISPLAY:-94} -window root "$O/$N-crash.png" 2>/dev/null
    echo "$N STALLED at $(grep '^loop' "$O/$N.txt" | tail -1)  screenshot $O/$N-crash.png"
    break
  fi
  last=$cur
done
wait $pid
echo "$N end: $(grep '^loop' "$O/$N.txt" | tail -1)"
