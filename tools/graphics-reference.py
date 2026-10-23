#!/usr/bin/env python3
"""Generate committed oracle data; reads the StarNet reference without writing it.

Usage: python3 -B tools/graphics-reference.py /path/to/wasteland-graphics
The binary stores little-endian uint32 ARGB values. Source hashes accompany it.
"""
import hashlib
import importlib.util
import itertools
import json
from pathlib import Path
import random
import struct
import sys
from PIL import Image

sys.dont_write_bytecode = True
reference = Path(sys.argv[1])
repo = Path(__file__).resolve().parents[1]
def module(name):
    spec = importlib.util.spec_from_file_location(name, reference / (name + '.py'))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod
grade, upscale = module('grading'), module('upscale')
rng = random.Random(20261023)
colors = [(v, v, v) for v in range(256)]
colors += list(itertools.product((0, 1, 4, 8, 16, 63, 127, 128, 192, 248, 254, 255), repeat=3))
colors += [tuple(rng.randrange(256) for _ in range(3)) for _ in range(4096)]
# Every 6-bit game-palette gray, boundary cube, and seeded full RGB samples.
def packed(p):
    return (p[3] if len(p) == 4 else 255) << 24 | p[0] << 16 | p[1] << 8 | p[2]
images = []
for w, h in ((1, 1), (1, 9), (9, 1), (2, 2), (3, 7), (7, 3)):
    for mode in ('RGB', 'RGBA'):
        im = Image.new(mode, (w, h))
        pal = [(0,0,0,0), (255,255,255,255), (100,25,30,128), (10,50,200,64)]
        im.putdata([rng.choice(pal)[:len(mode)] for _ in range(w*h)])
        images.append(im)
for pattern in range(512):
    im = Image.new('RGB', (3, 3))
    im.putdata([(255,)*3 if pattern & (1 << bit) else (0,)*3 for bit in range(9)])
    images.append(im)
for _ in range(128):
    im = Image.new('RGBA', (11, 9))
    pal = [(0,0,0,0), (0,0,0,255), (255,255,255,255), (200,60,30,128)]
    im.putdata([rng.choice(pal) for _ in range(99)])
    images.append(im)
for sample in sorted((reference / 'samples').glob('*.png')):
    images.append(Image.open(sample).convert('RGB').crop((200, 100, 296, 164)))
with (repo / 'tests/graphics-reference.bin').open('wb') as f:
    def words(values):
        f.write(struct.pack('<' + 'I' * len(values), *values))
    words([0x47584631, len(colors), len(images)])
    for rgb in colors:
        words([packed(rgb)] + [packed(grade.apply_rgb(rgb, name)) for name in grade.PRESETS])
    for im in images:
        words([im.width, im.height])
        words([packed(p) for p in im.getdata()])
        for fn in (upscale.mmpx, upscale.scale3x):
            words([packed(p) for p in fn(im).getdata()])
metadata = {'grading_colors': len(colors), 'presets': list(grade.PRESETS), 'upscale_images': len(images),
            'sources': {name: hashlib.sha256((reference/name).read_bytes()).hexdigest() for name in ('grading.py', 'upscale.py')},
            'black_outputs': {name: grade.apply_rgb((0,0,0), name) for name in grade.PRESETS}}
(repo / 'tests/graphics-reference.json').write_text(json.dumps(metadata, indent=2) + '\n')
print(json.dumps(metadata, indent=2))
