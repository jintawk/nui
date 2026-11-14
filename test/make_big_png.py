"""make_big_png.py <out.png> - a 2048 x 2048 RGB PNG for test_render's pd test (nui 1.2.1).

Small on disk (rows are shifted copies of one seeded random pattern, so zlib finds them) but as
costly to decode as a big map: stb_image inflates 12 MB and the fit averages 4 M pixels, so the
decode is many times the read + hash + header that tex_load still does on the Lua thread. Stdlib
only (zlib, struct); the same bytes every run.
"""
import random
import struct
import sys
import zlib

W = H = 2048


def chunk(kind, data):
    c = struct.pack('>I', len(data)) + kind + data
    return c + struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF)


def main(out):
    rng = random.Random(1210)
    row = W * 3
    pattern = bytes(rng.getrandbits(8) for _ in range(row + 4096))
    raw = bytearray()
    for y in range(H):
        s = (y * 37) % 4096
        raw += b'\x00' + pattern[s:s + row]
    png = b'\x89PNG\r\n\x1a\n'
    png += chunk(b'IHDR', struct.pack('>IIBBBBB', W, H, 8, 2, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(bytes(raw), 6))
    png += chunk(b'IEND', b'')
    with open(out, 'wb') as f:
        f.write(png)
    print('%s: %d x %d, %d bytes' % (out, W, H, len(png)))


if __name__ == '__main__':
    main(sys.argv[1])
