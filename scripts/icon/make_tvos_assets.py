#!/usr/bin/env python3
"""Builds the Apple TV brand assets (layered parallax app icon + Top Shelf) from
art rendered by the game itself.
usage: make_tvos_assets.py <hills.png> <ground.png> <mario.png> <out.xcassets>
"""
import json, os, sys
from PIL import Image

hills_src, ground_src, mario_src, out = sys.argv[1:5]
hills = Image.open(hills_src).convert('RGBA')            # 480x224, opaque sky + hills
ground = Image.open(ground_src).convert('RGBA')          # 480x224, BG1 only (transparent)
m = Image.open(mario_src).convert('RGBA')
mario = m.crop(m.getbbox())                              # 16x28 peace-sign pose
W, H = hills.size
GROUND_Y = 191                                           # first row of the grass strip

def ground_layer(width):
    """Bushes where the level has them, grass/dirt strip tiled across `width`."""
    layer = Image.new('RGBA', (width, H), (0, 0, 0, 0))
    off = (width - W) // 2
    layer.alpha_composite(ground, (off, 0))
    strip = ground.crop((128, GROUND_Y, 144, H))         # one 16 px period of the ground
    for x in range(0, width, 16):
        layer.alpha_composite(strip, (x, GROUND_Y))
    layer.alpha_composite(ground.crop((112, 0, 400, GROUND_Y)), (off + 112, 0))
    return layer

def hills_layer(width):
    layer = Image.new('RGBA', (width, H))
    for x in range(-W, width + W, W):
        layer.alpha_composite(hills, (x + (width - W) // 2, 0))
    return layer

def scaled(img, s):
    return img.resize((img.width * s, img.height * s), Image.NEAREST)

def crop_center_bottom(img, w, h, xc=None):
    xc = img.width // 2 if xc is None else xc
    return img.crop((xc - w // 2, img.height - h, xc - w // 2 + w, img.height))

def write_json(path, data):
    os.makedirs(path, exist_ok=True)
    with open(os.path.join(path, 'Contents.json'), 'w') as f:
        json.dump(data, f, indent=2)

INFO = {'author': 'xcode', 'version': 1}

def imageset(path, images):
    """images: list of (filename, PIL image, scale string)."""
    write_json(path, {'images': [{'filename': f, 'idiom': 'tv', 'scale': sc} for f, _, sc in images], 'info': INFO})
    for f, img, _ in images:
        img.save(os.path.join(path, f))

def icon_layers(w, h):
    """Back (sky + hills), Middle (ground + bushes), Front (Mario) at w x h."""
    src_w, src_h = 200, 120                       # 5:3 window of the scene
    s = w / src_w
    xc = 256
    back = crop_center_bottom(hills, src_w, src_h, xc).resize((w, h), Image.NEAREST)
    mid = crop_center_bottom(ground, src_w, src_h, xc).resize((w, h), Image.NEAREST)
    front = Image.new('RGBA', (w, h), (0, 0, 0, 0))
    ms = max(1, round(h * 0.72 / mario.height))
    big = scaled(mario, ms)
    front.alpha_composite(big, ((w - big.width) // 2, h - big.height - round(h * 0.06)))
    return back, mid, front

def imagestack(path, sizes):
    """sizes: list of (w, h, scale-string)."""
    names = ['Front', 'Middle', 'Back']
    write_json(path, {'info': INFO, 'layers': [{'filename': f'{n}.imagestacklayer'} for n in names]})
    rendered = {sc: icon_layers(w, h) for w, h, sc in sizes}
    for i, n in enumerate(names):
        layer_dir = os.path.join(path, f'{n}.imagestacklayer')
        write_json(layer_dir, {'info': INFO})
        idx = {'Back': 0, 'Middle': 1, 'Front': 2}[n]
        imageset(os.path.join(layer_dir, 'Content.imageset'),
                 [(f'{n.lower()}{"" if sc == "1x" else "@2x"}.png', rendered[sc][idx], sc) for _, _, sc in sizes])

def top_shelf(w, h, s):
    src_w = -(-w // s)
    scene = hills_layer(src_w + 32)
    scene.alpha_composite(ground_layer(src_w + 32))
    scene.alpha_composite(mario, ((scene.width - mario.width) // 2 + 40, GROUND_Y - mario.height + 1))
    return crop_center_bottom(scaled(scene, s), w, h).convert('RGB')

root = out
write_json(root, {'info': INFO})
brand = os.path.join(root, 'App Icon & Top Shelf Image.brandassets')
write_json(brand, {'info': INFO, 'assets': [
    {'filename': 'App Icon - App Store.imagestack', 'idiom': 'tv', 'role': 'primary-app-icon', 'size': '1280x768'},
    {'filename': 'App Icon.imagestack', 'idiom': 'tv', 'role': 'primary-app-icon', 'size': '400x240'},
    {'filename': 'Top Shelf Image Wide.imageset', 'idiom': 'tv', 'role': 'top-shelf-image-wide', 'size': '2320x720'},
    {'filename': 'Top Shelf Image.imageset', 'idiom': 'tv', 'role': 'top-shelf-image', 'size': '1920x720'},
]})
imagestack(os.path.join(brand, 'App Icon.imagestack'), [(400, 240, '1x'), (800, 480, '2x')])
imagestack(os.path.join(brand, 'App Icon - App Store.imagestack'), [(1280, 768, '1x')])
imageset(os.path.join(brand, 'Top Shelf Image.imageset'),
         [('topshelf.png', top_shelf(1920, 720, 4), '1x'), ('topshelf@2x.png', top_shelf(3840, 1440, 8), '2x')])
imageset(os.path.join(brand, 'Top Shelf Image Wide.imageset'),
         [('topshelf-wide.png', top_shelf(2320, 720, 5), '1x'), ('topshelf-wide@2x.png', top_shelf(4640, 1440, 10), '2x')])
# Review image of the 2x icon and the wide top shelf.
b, mdl, f = icon_layers(800, 480)
prev = b.copy(); prev.alpha_composite(mdl); prev.alpha_composite(f)
prev.save('build/tvos_icon_preview.png')
top_shelf(2320, 720, 5).save('build/tvos_topshelf_preview.png')
print('ok')
