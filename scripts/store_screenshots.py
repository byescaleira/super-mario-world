#!/usr/bin/env python3
"""Builds App Store Connect screenshots from runtime renders.

Each platform uses the widescreen margin that makes the 8:7-pixel image fill
its exact aspect ratio; scaling is sharp-bilinear (integer nearest-neighbour
prescale, then a bilinear pass), like the app's default filter.
usage: store_screenshots.py <renders-dir> <out-dir>
"""
import os, sys
from PIL import Image

renders, out = sys.argv[1], sys.argv[2]
PAR = 8 / 7
PLATFORMS = {
    # name: (width, height, widescreen margin used for the render)
    'iphone-6.9': (2868, 1320, 85),
    'ipad-13': (2752, 2064, 0),
    'mac': (2880, 1800, 29),
    'appletv': (3840, 2160, 46),
}
SCENES = ['00560', '01060', '01850', '02560', '02700', '02930']

for name, (W, H, margin) in PLATFORMS.items():
    os.makedirs(os.path.join(out, name), exist_ok=True)
    for i, scene in enumerate(SCENES, 1):
        src = Image.open(os.path.join(renders, f'w{margin}', f'frame_{scene}.png')).convert('RGB')
        # Display size at this aspect (8:7 pixels), cover the canvas.
        sx = W / (src.width * PAR)
        sy = H / src.height
        s = max(sx, sy)
        tw, th = round(src.width * PAR * s), round(src.height * s)
        k = max(1, int(min(tw / src.width, th / src.height)))
        img = src.resize((src.width * k, src.height * k), Image.NEAREST).resize((tw, th), Image.BILINEAR)
        left, top = (tw - W) // 2, (th - H) // 2
        img.crop((left, top, left + W, top + H)).save(os.path.join(out, name, f'{i:02d}.png'))
    print(name, W, 'x', H, len(SCENES), 'screenshots')
