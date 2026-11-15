#!/bin/bash
# Load every level in the emulator, headless, and report which ones crash or hang.
#   [BIN=OpenLara] [FRAMES=30] [WAIT=150] ./sweep.sh [LEVEL ...]   (default: all of emu-run/DATA/*.PHD)
# A run that does not finish in WAIT seconds gets a screenshot (emu-run/out/sweep-NAME.png) of the
# Amiga screen: the Software Failure requester shows the error number.
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../../.." && pwd)/emu-run
FRAMES=${FRAMES:-30}; WAIT=${WAIT:-150}
LEVELS=("$@"); [ ${#LEVELS[@]} -eq 0 ] && LEVELS=($(cd "$R" && ls DATA/*.PHD))
for L in "${LEVELS[@]}"; do
  n=$(basename "$L" .PHD | tr A-Z a-z)
  rm -f "$R/out/sweep-$n.png"
  TIMEOUT=$((WAIT + 30)) "$H/emu.sh" "$L" "$FRAMES" "sw-$n" > "$R/out/sw-$n.run" 2>&1 &
  pid=$!
  t=0
  while [ $t -lt $WAIT ] && kill -0 $pid 2>/dev/null; do
    [ -f "$R/out/sw-$n.raw" ] && break
    sleep 3; t=$((t + 3))
  done
  if [ -f "$R/out/sw-$n.raw" ]; then
    last=$(grep "^frame" "$R/out/sw-$n.txt" | tail -1)
    echo "OK    $L  ($last)"
  else
    import -display :${EMU_DISPLAY:-94} -window root "$R/out/sweep-$n.png" 2>/dev/null
    echo "FAIL  $L  last log: $(grep -v '^  ' "$R/out/sw-$n.log" 2>/dev/null | tail -1)  shot: out/sweep-$n.png"
  fi
  wait $pid 2>/dev/null
done
