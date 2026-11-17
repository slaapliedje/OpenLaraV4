#!/usr/bin/env python3
"""raw2png.py OUT.png a.raw [b.raw ...]: RGB565 big-endian frame dumps (320x240 or 640x480 by size) side by side"""
import sys, numpy as np
from PIL import Image
SIZES = {153600: (320, 240), 614400: (640, 480), 460800: (640, 360), 512000: (640, 400)}
ims = []
for n in sys.argv[2:]:
    d = open(n, 'rb').read(); w, h = SIZES[len(d)]
    a = np.frombuffer(d, '>u2').reshape(h, w)
    ims.append(np.stack([(a >> 11) << 3, ((a >> 5) & 63) << 2, (a & 31) << 3], -1).astype(np.uint8))
h = max(i.shape[0] for i in ims)
ims = [np.pad(i, ((0, h - i.shape[0]), (0, 0), (0, 0))) for i in ims]
im = Image.fromarray(np.concatenate(ims, 1))
if im.width < 1300: im = im.resize((im.width * 2, im.height * 2), Image.NEAREST)
im.save(sys.argv[1]); print(sys.argv[1], im.size)
