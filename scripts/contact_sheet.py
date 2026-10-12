# Builds a labelled grid of harness screenshots: contact_sheet.py <dir> <out.png> [cols] [glob]
import sys, glob, os
from PIL import Image, ImageDraw
d, out = sys.argv[1], sys.argv[2]
cols = int(sys.argv[3]) if len(sys.argv) > 3 else 4
pat = sys.argv[4] if len(sys.argv) > 4 else 'frame_*.png'
files = sorted(glob.glob(os.path.join(d, pat)))
rows = (len(files) + cols - 1) // cols
w0, h0 = Image.open(files[0]).size
cw, ch = w0 + 4, h0 + 16
sheet = Image.new('RGB', (cols * cw, rows * ch), 'white')
dr = ImageDraw.Draw(sheet)
for i, f in enumerate(files):
    im = Image.open(f).convert('RGB')
    x, y = (i % cols) * cw, (i // cols) * ch
    sheet.paste(im, (x + 2, y + 14))
    dr.text((x + 4, y + 1), os.path.basename(f)[6:-4], fill='black')
sheet.save(out)
print(len(files), 'frames')
