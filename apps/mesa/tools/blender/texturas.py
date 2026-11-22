#!/usr/bin/env python3
"""
Juegos de mesa - the printed boards Blender lays on its models (ludo's
cross and yards, backgammon's points), drawn with PIL; run with the
system's python before ludo and backgammon are rendered:

    python3 texturas.py         # -> ../../assets/tex/ludo.png, backgammon.png

Coordinates match the games' (g_ludo.c, g_backgammon.c): 100 pixels a
cell, y up on the board (down in the picture).
"""
import math
import os

from PIL import Image, ImageDraw, ImageFilter

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.normpath(os.path.join(HERE, '..', '..', 'assets', 'tex'))
K = 100

SEAT = ['#d0281e', '#2a9a3c', '#e8b818', '#2a5cc8']
SEAT_LIGHT = ['#f4b8b0', '#b4e2ba', '#f6e2a0', '#b4c8f0']


def ludo():
    n = 15
    im = Image.new('RGB', (n * K, n * K), '#f6f0e2')
    d = ImageDraw.Draw(im)

    def cell(x, y, fill, outline='#3a3028'):
        # x, y in board cells, y up
        x0, y0 = x * K, (n - 1 - y) * K
        d.rectangle([x0, y0, x0 + K - 1, y0 + K - 1], fill=fill, outline=outline, width=3)

    # the track (as g_ludo.c lays it)
    tx, ty = [], []
    for x in range(0, 6): tx.append(x); ty.append(6)
    for y in range(5, -1, -1): tx.append(6); ty.append(y)
    tx.append(7); ty.append(0)
    for y in range(0, 6): tx.append(8); ty.append(y)
    for x in range(9, 15): tx.append(x); ty.append(6)
    tx.append(14); ty.append(7)
    for x in range(14, 8, -1): tx.append(x); ty.append(8)
    for y in range(9, 15): tx.append(8); ty.append(y)
    tx.append(7); ty.append(14)
    for y in range(14, 8, -1): tx.append(6); ty.append(y)
    for x in range(5, -1, -1): tx.append(x); ty.append(8)
    tx.append(0); ty.append(7)
    for t in range(52):
        fill = '#fbf7ee'
        if t % 13 == 1:
            fill = SEAT[t // 13]
        cell(tx[t], ty[t], fill)
        if t % 13 == 9 or t % 13 == 1:
            # a star on the safe squares
            cx, cy = tx[t] * K + K / 2, (n - 1 - ty[t]) * K + K / 2
            pts = []
            for i in range(10):
                r = 36 if i % 2 == 0 else 15
                a = -math.pi / 2 + i * math.pi / 5
                pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
            d.polygon(pts, fill='#fbf7ee' if t % 13 == 1 else '#c8b89a')
    # the ways home
    base = [(1, 7, 1, 0), (7, 1, 0, 1), (13, 7, -1, 0), (7, 13, 0, -1)]
    for s, (bx, by, sx, sy) in enumerate(base):
        for k in range(5):
            cell(bx + sx * k, by + sy * k, SEAT[s])
    # the yards
    yards = [(0, 0), (9, 0), (9, 9), (0, 9)]
    for s, (yx, yy) in enumerate(yards):
        x0, y0 = yx * K, (n - 6 - yy) * K
        d.rectangle([x0, y0, x0 + 6 * K - 1, y0 + 6 * K - 1], fill=SEAT[s], outline='#3a3028', width=4)
        d.rounded_rectangle([x0 + K * 0.8, y0 + K * 0.8, x0 + K * 5.2, y0 + K * 5.2], radius=40, fill='#fbf7ee',
                            outline='#3a3028', width=3)
        for k in range(4):
            cx = (yx + 1.5 + (k & 1) * 2 + 0.5) * K
            cy = (n - 1 - (yy + 1.5 + (k >> 1) * 2)) * K + K / 2
            d.ellipse([cx - 42, cy - 42, cx + 42, cy + 42], fill=SEAT_LIGHT[s], outline=SEAT[s], width=8)
    # the centre: four triangles
    c0, c1 = 6 * K, 9 * K
    mid = (7.5 * K, 7.5 * K)
    tris = [[(c0, c0), (c0, c1), mid], [(c0, c1), (c1, c1), mid], [(c1, c1), (c1, c0), mid], [(c1, c0), (c0, c0), mid]]
    # left: seat 0 (its way home comes from the left), bottom: seat 1, right: 2, top: 3
    for s, t in zip((0, 1, 2, 3), tris):
        d.polygon(t, fill=SEAT[s], outline='#3a3028')
    im = im.filter(ImageFilter.SMOOTH)
    im.save(os.path.join(OUT, 'ludo.png'))


def backgammon():
    w, h = 15, 11
    im = Image.new('RGB', (w * K, h * K), '#2e5a3a')
    d = ImageDraw.Draw(im)
    # the felt of the two halves, darker margins (the frame is modelled)
    d.rectangle([int(0.5 * K), int(0.5 * K), int(6.5 * K), int(10.5 * K)], fill='#2f6a44')
    d.rectangle([int(7.5 * K), int(0.5 * K), int(13.5 * K), int(10.5 * K)], fill='#2f6a44')
    cols = ['#e8dcc0', '#8a2a1c']

    def px(i):
        if i <= 5:
            return 13.0 - i
        if i <= 11:
            return 12.0 - i
        if i <= 17:
            return 1.0 + (i - 12)
        return 8.0 + (i - 18)

    for i in range(24):
        x = px(i) * K
        if i < 12:
            base, tip = (h - 0.5) * K, (h - 5.1) * K       # bottom of the board is the picture's bottom
        else:
            base, tip = 0.5 * K, 5.1 * K
        d.polygon([(x - 0.47 * K, base), (x + 0.47 * K, base), (x, tip)], fill=cols[i % 2], outline='#1a120a')
    im = im.filter(ImageFilter.SMOOTH)
    im.save(os.path.join(OUT, 'backgammon.png'))


os.makedirs(OUT, exist_ok=True)
ludo()
backgammon()
print('textures in', OUT)
