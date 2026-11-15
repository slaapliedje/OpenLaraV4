#!/bin/bash
# Monkey-test every level in the second emulator instance (emu-run2, display :95).
H=$(cd "$(dirname "$0")" && pwd)
export EMU_RUN=$(cd "$H/../../.." && pwd)/emu-run2 EMU_DISPLAY=95 BIN=${BIN:-OpenLaraBatch}
for l in LEVEL4 LEVEL5 LEVEL6 LEVEL7A LEVEL7B LEVEL8A LEVEL8B LEVEL8C LEVEL10A LEVEL10B LEVEL10C CUT1 CUT2 CUT3 CUT4; do
  n=$(echo $l | tr A-Z a-z)
  "$H/monkey.sh" DATA/$l.PHD m-$n.txt b-$n ${FRAMES:-2000}
done
