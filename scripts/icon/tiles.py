# Assembles an image from 8x8 tiles of the WRAM dump (tooling).
# tiles.py <wram.bin> <gfx-start-hex> <cgram-palette> <row0> <col0> <rows> <cols> <scale> <out.png>
import sys
from PIL import Image
wram = open(sys.argv[1], 'rb').read()
start = int(sys.argv[2], 16)
n = int(sys.argv[3])
raw = wram[0x20000 + n * 32:0x20000 + n * 32 + 32]
pal = [None] + [((c & 31) * 255 // 31, ((c >> 5) & 31) * 255 // 31, ((c >> 10) & 31) * 255 // 31)
                for c in (raw[i] | (raw[i + 1] << 8) for i in range(2, 32, 2))]
r0, c0, rows, cols, scale = map(int, sys.argv[4:9])
img = Image.new('RGBA', (cols * 8, rows * 8), (0, 0, 0, 0))
for tr in range(rows):
    for tc in range(cols):
        t = (r0 + tr) * 16 + (c0 + tc)
        tile = wram[start + t * 32:start + t * 32 + 32]
        for y in range(8):
            p0, p1, p2, p3 = tile[y * 2], tile[y * 2 + 1], tile[16 + y * 2], tile[16 + y * 2 + 1]
            for x in range(8):
                b = 7 - x
                c = ((p0 >> b) & 1) | (((p1 >> b) & 1) << 1) | (((p2 >> b) & 1) << 2) | (((p3 >> b) & 1) << 3)
                if c:
                    img.putpixel((tc * 8 + x, tr * 8 + y), pal[c] + (255,))
img = img.resize((img.width * scale, img.height * scale), Image.NEAREST)
img.save(sys.argv[9])
