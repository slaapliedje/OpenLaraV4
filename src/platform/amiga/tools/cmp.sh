#!/bin/bash
# cmp.sh LEVEL SCRIPT NAME [LOOPS]: same scripted run on the V4 (off-screen, real Maggie, 640x480) and the GL reference; sheets in cmp/NAME
# never run beside another Maggie program: refuse while RAM:v4-busy exists (written by every launcher, incl. live runs)
if timeout 15 rsh -n ${V4_HOST:?set V4_HOST to the address of the V4} 'C:List RAM:v4-busy NOHEAD' 2>/dev/null | grep -q v4-busy; then echo "V4 busy (RAM:v4-busy exists): not running" >&2; exit 3; fi
V=${V4_HOST:?set V4_HOST to the address of the V4}; S=${CMP_OUT:-/tmp/olcmp}; L=$1; SC=$2; N=$3; FR=${4:-680}
O=$S/cmp/$N; mkdir -p $O; rm -f $O/*
cd $(git rev-parse --show-toplevel)/src/platform/amiga
want=$(stat -c %s OpenLaraHW); got=$(timeout 20 rsh -n $V 'C:List RAM:OLHW NOHEAD' 2>/dev/null | awk '{print $2}')
[ "$want" != "$got" ] && scp -q OpenLaraHW "${V}:RAM:OLHW" 2>/dev/null
scp -q $SC "${V}:RAM:hw.txt" 2>/dev/null
timeout 20 rsh -n $V 'Echo "Amiga A4000D setup CD session: OpenLara" >RAM:v4-busy'
timeout 20 rsh -n $V 'Delete RAM:h.#? RAM:h-#? QUIET' >/dev/null 2>&1
timeout 300 rsh -n $V "CD Programs:Games/OpenLaraMain
Set OLRES 640x480
RAM:OLHW DATA/$L.PHD $FR RAM:h RAM:hw.txt" > $O/v4.log 2>&1
timeout 20 rsh -n $V 'Delete RAM:v4-busy QUIET'
for f in $(timeout 20 rsh -n $V 'C:List RAM:h-#?.raw NOHEAD LFORMAT %N'); do scp -q "${V}:RAM:$f" $O/v4${f#h} 2>/dev/null; done
timeout 20 rsh -n $V 'Delete RAM:h.#? RAM:h-#? QUIET' >/dev/null 2>&1
cd $(git rev-parse --show-toplevel)/emu-run
OLLOW=1 OLSCRIPT=$SC OLOUT=$O/ref OLTIME0=7700 timeout 300 xvfb-run -n 97 -s "-screen 0 1024x768x24 +extension GLX" ${OL_REF_BIN:?set OL_REF_BIN to the GL reference build} --window-size=640x480 DATA/$L.PHD > $O/ref.log 2>&1
python3 - $O <<'PY'
import sys, glob, os, numpy as np
from PIL import Image
o = sys.argv[1]
for r in sorted(glob.glob(o + '/v4-*.raw')):
    k = os.path.basename(r)[3:7]
    a = np.frombuffer(open(r, 'rb').read(), '>u2').reshape(480, 640)
    v = np.stack([(a >> 11) << 3, ((a >> 5) & 63) << 2, (a & 31) << 3], -1).astype(np.int16)
    p = o + '/ref-' + k + '.ppm'
    if not os.path.exists(p): print(k, 'no reference frame'); continue
    g = np.asarray(Image.open(p).convert('RGB').resize((640, 480)), dtype=np.int16)
    d = np.abs(v - g)
    Image.fromarray(np.concatenate([v, g, np.clip(d * 3, 0, 255)], 1).astype(np.uint8)).save(o + '/pair-' + k + '.png')
    print(k, 'mean diff %.1f' % d.mean(), 'px>48: %.1f%%' % (100.0 * (d.max(-1) > 48).mean()))
PY
