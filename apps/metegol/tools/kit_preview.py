#!/usr/bin/env python3
"""
METEGOL - how the kits look on the rendered figure, without the board:
the same painting as main/mg_kit.c, in numpy.

    python3 tools/kit_preview.py out.png [team ...]

Reads assets/figures/ (tools/blender/figures.py) and the default teams
from main/mg_kit.c.
"""
import math
import os
import re
import sys

import numpy as np
from PIL import Image

APP = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIG = os.path.join(APP, 'assets', 'figures')
SHADE_K = 0.66          # the render's grey to the shade (160 is the colour as it is)
DESIGNS = ['lisa', 'rayas', 'bastones', 'franja', 'banda', 'v', 'mitades', 'cuadros', 'vivos', 'aros']


def teams():
    src = open(os.path.join(APP, 'main', 'mg_kit.c'), encoding='utf-8').read()
    body = src[src.index('DEFAULT_TEAMS[] ='):src.index(';\n\n/* ---- the file')]
    txt = ''.join(eval('"' + s + '"') for s in re.findall(r'"((?:[^"\\]|\\.)*)"', body))
    out, cur = {}, None
    for line in txt.split('\n'):
        line = line.strip()
        if not line or line.startswith('#'):
            continue
        if line.startswith('['):
            cur = line[1:-1]
            out[cur] = {}
            continue
        k, v = [x.strip() for x in line.split('=', 1)]
        out[cur][k] = v
    return out


def col(h):
    h = int(h, 16)
    return np.array([(h >> 16) & 255, (h >> 8) & 255, h & 255], np.float32)


def shirt(t, f, l, z):
    d = DESIGNS.index(t.get('diseño', 'lisa'))
    c1, c2 = col(t['color1']), col(t['color2'])
    m = np.zeros(f.shape, bool)
    if d in (1, 2):
        n = 10.0 if d == 2 else 16.0
        a = np.arctan2(l, f) / (2 * math.pi) + 0.5
        k = np.floor((a - 0.5) * n + 0.5 + 1000).astype(int)
        m = (k & 1) == 1
    elif d == 3:
        m = (z > 0.40) & (z < 0.68)
    elif d == 9:
        m = (np.floor(z * 7).astype(int) & 1) == 1
    elif d == 4:
        m = np.abs(z - (0.52 - 0.30 * l + 0.16 * f)) < 0.15
    elif d == 5:
        m = (f >= -0.15) & (np.abs(z - (0.55 + 0.42 * np.abs(l)) - 0.12 * (1 - f)) < 0.11)
    elif d == 6:
        m = l > 0
    elif d == 7:
        a = np.arctan2(l, f) / (2 * math.pi) + 0.5
        m = ((np.floor(a * 14 + 100).astype(int) + np.floor(z * 6).astype(int)) & 1) == 1
    elif d == 8:
        m = (np.abs(f) < 0.16) & (np.abs(l) > 0.7) & (z < 0.82)
    return np.where(m[..., None], c2, c1)


def paint(t, mirror=False):
    a = np.asarray(Image.open(os.path.join(FIG, 'figures.png')).convert('RGBA')).astype(np.float32)
    p = np.asarray(Image.open(os.path.join(FIG, 'figures_part.png')).convert('RGB')).astype(np.float32)
    u = np.asarray(Image.open(os.path.join(FIG, 'figures_uvw.png')).convert('RGB')).astype(np.float32)
    part = np.round(p[..., 0] * 16 / 255).astype(int)
    f = (u[..., 0] - 128) / 127
    l = (u[..., 1] - 128) / 127
    z = u[..., 2] / 255
    if mirror:
        l = -l
    shade = a[..., :3].mean(-1) * SHADE_K
    out = np.zeros(a.shape[:2] + (3,), np.float32)
    table = {1: col('E0A37A'), 2: col('2A1C14'), 5: col(t['cuello']), 6: col(t['pantalon']), 8: col('1A1A1A'),
             9: col('B8BCC4')}
    for k, c in table.items():
        out[part == k] = c
    sh = shirt(t, f, l, z)
    out[part == 3] = sh[part == 3]
    out[part == 4] = col(t['mangas'])
    socks = np.where((z > 0.7)[..., None] & (t.get('diseño') != 'lisa'), col(t['color2']), col(t['medias']))
    out[part == 7] = socks[part == 7]
    k = np.minimum(shade, 160)[..., None] / 160
    hi = np.maximum(shade - 160, 0)[..., None] * 2
    rgb = np.clip(out * k + hi, 0, 255)
    img = np.dstack([rgb, a[..., 3:4]]).astype(np.uint8)
    im = Image.fromarray(img)
    return im.transpose(Image.FLIP_LEFT_RIGHT) if mirror else im


def main():
    dst = sys.argv[1]
    ts = teams()
    names = sys.argv[2:] or list(ts)
    cells = [0, 1, 2, 30, 4]
    import json
    info = json.load(open(os.path.join(FIG, 'figures.json')))
    cw, ch = info['cell']
    cols = info['cols']
    rows = []
    for n in names:
        im = paint(ts[n])
        row = Image.new('RGBA', (cw * len(cells), ch), (40, 110, 60, 255))
        for i, c in enumerate(cells):
            x, y = (c % cols) * cw, (c // cols) * ch
            cell = im.crop((x, y, x + cw, y + ch))
            row.alpha_composite(cell, (i * cw, 0))
        rows.append(row)
    out = Image.new('RGBA', (cw * len(cells), ch * len(rows)))
    for i, r in enumerate(rows):
        out.paste(r, (0, i * ch))
    out = out.resize((out.width * 2, out.height * 2), Image.NEAREST)
    out.convert('RGB').save(dst)


main()
