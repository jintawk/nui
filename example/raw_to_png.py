"""Turn what render.lua wrote (<name>.raw, BGRA rows, listed in shots.txt) into PNGs.

    python example/raw_to_png.py <out dir>

Needs Pillow. The .raw files are deleted once converted.
"""
import os
import sys

from PIL import Image

out = sys.argv[1]
for line in open(os.path.join(out, 'shots.txt')):
    parts = line.split()
    if not parts:
        continue
    name, x0, y0, x1, y1 = parts[0], *map(int, parts[1:5])
    raw = os.path.join(out, name + '.raw')
    data = open(raw, 'rb').read()
    im = Image.frombytes('RGBA', (x1 - x0, y1 - y0), data, 'raw', 'BGRA').convert('RGB')
    im.save(os.path.join(out, name + '.png'))
    os.remove(raw)
    print(name, im.size)
