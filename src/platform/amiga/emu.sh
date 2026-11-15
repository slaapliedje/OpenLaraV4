#!/bin/bash
# Headless test of the main-engine Amiga build in Amiberry (68020+68882, no RTG, off screen on Xvfb).
#   ./emu.sh [LEVEL] [FRAMES] [NAME]     e.g. ./emu.sh DATA/GYM.PHD 20 gym
# Needs ../../../emu-run (olmain.uae, C/UAEquit, DATA/*.PHD). Results: emu-run/out/NAME.txt (level
# stats + frame times; compare with ./phdstats.py LEVEL) and NAME.png (last frame).
set -eu
H=$(cd "$(dirname "$0")" && pwd)
R=$(cd "$H/../../.." && pwd)/emu-run
L=${1:-DATA/GYM.PHD}; N=${2:-20}; NAME=${3:-gym}
cp "$H/OpenLara" "$R/C/OpenLara"
rm -f "$R/out/$NAME.txt" "$R/out/$NAME.raw" "$R/out/$NAME.log"
echo "C:OpenLara $L $N out/$NAME >out/$NAME.log" > "$R/S/Job"
cd "$R"
DISP=:${EMU_DISPLAY:-94}
XDG=$R/out/xdg; mkdir -p "$XDG"; chmod 700 "$XDG"
pkill -f "Xvfb $DISP" 2>/dev/null || true
Xvfb $DISP -screen 0 1024x768x24 -nolisten tcp > "$R/out/xvfb.log" 2>&1 &
XVFB=$!
trap 'kill $XVFB 2>/dev/null || true' EXIT
for i in $(seq 1 50); do [ -e /tmp/.X11-unix/X${DISP#:} ] && break; sleep 0.1; done
t0=$(date +%s)
env -u WAYLAND_DISPLAY DISPLAY=$DISP SDL_VIDEODRIVER=x11 SDL_RENDER_DRIVER=software XDG_RUNTIME_DIR="$XDG" \
  timeout "${TIMEOUT:-900}" amiberry -G -f "$R/olmain.uae" > "$R/out/amiberry.log" 2>&1 || true
echo "emulator: $(( $(date +%s) - t0 )) s"
cat "$R/out/$NAME.log" 2>/dev/null || true
[ -f "$R/out/$NAME.txt" ] && head -12 "$R/out/$NAME.txt"
if [ -f "$R/out/$NAME.raw" ]; then
  python3 -I - "$R/out/$NAME.raw" "$R/out/$NAME.png" <<'PY'
import sys
from PIL import Image
d = open(sys.argv[1], 'rb').read()
im = Image.new('RGB', (320, 240))
im.putdata([(((v >> 11) & 31) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3)
            for v in ((d[i] << 8) | d[i + 1] for i in range(0, len(d), 2))])
im.resize((640, 480), Image.NEAREST).save(sys.argv[2])
print("frame:", sys.argv[2])
PY
fi
