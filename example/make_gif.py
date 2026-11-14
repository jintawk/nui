"""Turn a rendered frame sequence (<name>_001.png ...) into an animated GIF.

    python example/make_gif.py <dir> <name> <ms per frame> [scale]
"""
import glob
import os
import sys

from PIL import Image

d, name, ms = sys.argv[1], sys.argv[2], int(sys.argv[3])
scale = float(sys.argv[4]) if len(sys.argv) > 4 else 1.0
files = sorted(glob.glob(os.path.join(d, name + '_*.png')))
if not files:
    raise SystemExit('no frames for ' + name)
frames = []
for f in files:
    im = Image.open(f).convert('RGB')
    if scale != 1.0:
        im = im.resize((int(im.width * scale), int(im.height * scale)), Image.LANCZOS)
    frames.append(im.quantize(colors=128, method=Image.MEDIANCUT, dither=Image.NONE))
out = os.path.join(d, name + '.gif')
frames[0].save(out, save_all=True, append_images=frames[1:], duration=ms, loop=0, optimize=True)
print(out, os.path.getsize(out) // 1024, 'KiB', frames[0].size, len(frames), 'frames')
