#!/usr/bin/env python3
"""Contact sheet of headless frame dumps: ./sheet.py OUT.png a-0000.raw a-0025.raw ..."""
import sys
from PIL import Image, ImageDraw
files = sys.argv[2:]
cols = 6
rows = (len(files) + cols - 1) // cols
sheet = Image.new('RGB', (cols * 320, rows * 240))
for i, f in enumerate(files):
    d = open(f, 'rb').read()
    im = Image.new('RGB', (320, 240))
    im.putdata([(((v >> 11) & 31) << 3, ((v >> 5) & 63) << 2, (v & 31) << 3)
                for v in ((d[j] << 8) | d[j + 1] for j in range(0, len(d), 2))])
    ImageDraw.Draw(im).text((4, 4), f.rsplit('-', 1)[-1].split('.')[0], fill=(255, 255, 0))
    sheet.paste(im, ((i % cols) * 320, (i // cols) * 240))
sheet.save(sys.argv[1])
