# Decodes a WRAM range as SNES 4bpp tiles into a labelled sheet (tooling).
# gfx_sheet.py <wram.bin> <start-hex> <bytes-hex> <palette.pal> <out.png>
import sys
from PIL import Image, ImageDraw
wram = open(sys.argv[1], 'rb').read()
start, size = int(sys.argv[2], 16), int(sys.argv[3], 16)
# palette argument: a .pal file, or "cgram:<n>" for CGRAM palette n of the dump
if sys.argv[4].startswith('cgram:'):
    n = int(sys.argv[4][6:])
    pal_raw = wram[0x20000 + n * 32 + 2:0x20000 + n * 32 + 32]
else:
    pal_raw = open(sys.argv[4], 'rb').read()
pal = [(255, 0, 255)]
for i in range(0, len(pal_raw), 2):
    c = pal_raw[i] | (pal_raw[i + 1] << 8)
    pal.append(((c & 31) * 255 // 31, ((c >> 5) & 31) * 255 // 31, ((c >> 10) & 31) * 255 // 31))
data = wram[start:start + size]
tiles = len(data) // 32
cols = 16
rows = (tiles + cols - 1) // cols
scale = 4
img = Image.new('RGB', (cols * 8 * scale + cols, rows * 8 * scale + rows), (40, 40, 40))
for t in range(tiles):
    tile = data[t * 32:(t + 1) * 32]
    for y in range(8):
        p0, p1, p2, p3 = tile[y * 2], tile[y * 2 + 1], tile[16 + y * 2], tile[16 + y * 2 + 1]
        for x in range(8):
            b = 7 - x
            c = ((p0 >> b) & 1) | (((p1 >> b) & 1) << 1) | (((p2 >> b) & 1) << 2) | (((p3 >> b) & 1) << 3)
            col = pal[c] if c < len(pal) else (255, 255, 255)
            X = (t % cols) * (8 * scale + 1) + x * scale
            Y = (t // cols) * (8 * scale + 1) + y * scale
            for dy in range(scale):
                for dx in range(scale):
                    img.putpixel((X + dx, Y + dy), col)
img.save(sys.argv[5])
print(tiles, 'tiles')
