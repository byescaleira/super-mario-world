#!/usr/bin/env python3
"""Builds App/AppIcon.icon (Icon Composer format) from art the game itself rendered.

Layers (front to back):
  Mario      - big Mario's peace-sign pose (PlayerPose $26), drawn by the game's
               own sprite code, integer-scaled pixel art cropped as a bust.
  Background - the classic Yoshi's Island sky, hills and clouds (BG2 of the
               intro level), softly blurred like a portrait photo.
usage: make_icon.py <mario.png> <background.png> <out.icon>
"""
import json, os, sys
from PIL import Image, ImageFilter

mario_src, bg_src, out = sys.argv[1:4]
S = 1024

# Mario: tight crop, integer scale, bust framing (legs fall off the bottom).
m = Image.open(mario_src).convert('RGBA')
m = m.crop(m.getbbox())
scale = 30
m = m.resize((m.width * scale, m.height * scale), Image.NEAREST)
mario = Image.new('RGBA', (S, S), (0, 0, 0, 0))
mario.alpha_composite(m, ((S - m.width) // 2 + 15, 214))

# Background: square crop around the tall hill, 5x pixels, portrait blur.
b = Image.open(bg_src).convert('RGB')
side = 205
cx = b.width // 2 + 20
box = (cx - side // 2, b.height - side, cx - side // 2 + side, b.height)
bg = b.crop(box).resize((side * 5, side * 5), Image.NEAREST).crop((0, 0, S, S))
bg = bg.filter(ImageFilter.GaussianBlur(5))

os.makedirs(os.path.join(out, 'Assets'), exist_ok=True)
mario.save(os.path.join(out, 'Assets', 'Mario.png'))
bg.save(os.path.join(out, 'Assets', 'Background.png'))

sky = b.getpixel((4, 4))
def srgb(c):
    return 'srgb:' + ','.join(f'{v / 255:.5f}' for v in c) + ',1.00000'

icon = {
    'fill': {'solid': srgb(sky)},
    'groups': [
        {
            'name': 'Mario',
            'layers': [{
                'name': 'Mario', 'image-name': 'Mario.png', 'glass': False, 'opacity': 1,
                'position': {'scale': 1, 'translation-in-points': [0, 0]},
            }],
            'shadow': {'kind': 'neutral', 'opacity': 0.45},
            'translucency': {'enabled': False, 'value': 0.5},
        },
        {
            'name': 'Background',
            'layers': [{
                'name': 'Background', 'image-name': 'Background.png', 'glass': False, 'opacity': 1,
                'position': {'scale': 1, 'translation-in-points': [0, 0]},
            }],
            'shadow': {'kind': 'none', 'opacity': 0},
            'translucency': {'enabled': False, 'value': 0.5},
        },
    ],
    'supported-platforms': {'circles': ['watchOS'], 'squares': 'shared'},
}
with open(os.path.join(out, 'icon.json'), 'w') as f:
    json.dump(icon, f, indent=2)

# Flattened preview for quick review.
prev = bg.convert('RGBA')
prev.alpha_composite(mario)
prev.save(os.path.join(os.path.dirname(out.rstrip('/')) or '.', 'AppIcon-preview.png'))
print('ok')
