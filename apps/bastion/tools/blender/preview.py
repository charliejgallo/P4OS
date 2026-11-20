"""
BASTION - looking at what Blender rendered, at its real size.

    python3 preview.py <sheet.png> [...] --out out.png [--scale 2] [--bg 5a6a64]
    python3 preview.py --world ../../assets/worlds/luna --out out.png

Each sheet is drawn cell by cell over a flat ground colour, with the cell's
border and its anchor marked, so a model that touches its cell or stands
off its origin shows. A tower's `_head` sheet is drawn over its `_base`.
`--world` draws a world's picture with the pads' and the road's outlines
from bsworlds.py over it, and its depth beside it. Plain python3 with
Pillow and numpy (not Blender's Python).
"""
import json
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..'))


def sheet(path, bg, scale):
    im = Image.open(path).convert('RGBA')
    info = json.load(open(path[:-4] + '.json'))
    cw, ch = info['cell']
    ax, ay = info['anchor']
    cols, rows = info['cols'], info['rows']
    out = Image.new('RGB', (cw * cols, ch * rows), bg)
    base = None
    if path.endswith('_head.png') and os.path.exists(path.replace('_head.png', '_base.png')):
        base = Image.open(path.replace('_head.png', '_base.png')).convert('RGBA')
    d = ImageDraw.Draw(out)
    a = np.asarray(im)[..., 3]
    for r in range(rows):
        for c in range(cols):
            x, y = c * cw, r * ch
            d.ellipse([x + ax - 30, y + ay - 12, x + ax + 30, y + ay + 12], fill=tuple(max(0, v - 28) for v in bg))
            if base is not None:
                out.paste(base, (x, y), base)
            cell = im.crop((x, y, x + cw, y + ch))
            out.paste(cell, (x, y), cell)
            edge = a[y:y + ch, x:x + cw]
            touch = edge[0].any() or edge[-1].any() or edge[:, 0].any() or edge[:, -1].any()
            d.rectangle([x, y, x + cw - 1, y + ch - 1], outline=(255, 0, 0) if touch else tuple(min(255, v + 30) for v in bg))
            d.line([x + ax - 4, y + ay, x + ax + 4, y + ay], fill=(255, 255, 255))
            d.line([x + ax, y + ay - 4, x + ax, y + ay + 4], fill=(255, 255, 255))
    if scale != 1:
        out = out.resize((int(out.width * scale), int(out.height * scale)), Image.LANCZOS)
    return out


def world(prefix):
    import bsworlds
    wid = os.path.basename(prefix)
    w = bsworlds.BY_ID[wid]
    bg = Image.open(prefix + '_bg.png').convert('RGB')
    over = bg.copy()
    d = ImageDraw.Draw(over)

    def P(x, y):
        return (360 + x * 40, 1280 - y * 32)
    for p in w.paths:
        for off in (-bsworlds.PATH_W / 2, bsworlds.PATH_W / 2):
            pts = []
            for i, (x, y) in enumerate(p):
                a, b = p[max(0, i - 1)], p[min(len(p) - 1, i + 1)]
                dx, dy = b[0] - a[0], b[1] - a[1]
                l = (dx * dx + dy * dy) ** 0.5 or 1
                pts.append(P(x - dy / l * off, y + dx / l * off))
            d.line(pts, fill=(255, 255, 0), width=1)
    for (x, y) in w.pads:
        cx, cy = P(x, y)
        d.ellipse([cx - 38, cy - 30, cx + 38, cy + 30], outline=(0, 255, 255))
    cx, cy = P(*w.fort)
    d.ellipse([cx - 108, cy - 86, cx + 108, cy + 86], outline=(255, 255, 255))
    d.rectangle([0, 0, 719, 96], outline=(255, 0, 255))
    d.rectangle([0, 1090, 719, 1279], outline=(255, 0, 255))
    z = np.load(prefix + '_depth.npy')
    zz = np.clip((z - z.min()) / max(1e-3, (np.percentile(z, 99) - z.min())), 0, 1)
    dep = Image.fromarray((zz * 255).astype(np.uint8)).convert('RGB')
    out = Image.new('RGB', (720 * 3, 1280))
    out.paste(bg, (0, 0))
    out.paste(over, (720, 0))
    out.paste(dep, (1440, 0))
    return out


def main():
    a = sys.argv[1:]
    out = 'preview.png'
    scale = 1.0
    bg = (0x5a, 0x6a, 0x64)
    files = []
    wprefix = None
    i = 0
    while i < len(a):
        if a[i] == '--out':
            out = a[i + 1]
            i += 2
        elif a[i] == '--scale':
            scale = float(a[i + 1])
            i += 2
        elif a[i] == '--bg':
            v = a[i + 1]
            bg = tuple(int(v[k:k + 2], 16) for k in (0, 2, 4))
            i += 2
        elif a[i] == '--world':
            wprefix = a[i + 1]
            i += 2
        else:
            files.append(a[i])
            i += 1
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    if wprefix:
        world(wprefix).save(out)
        return
    ims = [sheet(f, bg, scale) for f in files]
    W = max(im.width for im in ims)
    H = sum(im.height for im in ims)
    res = Image.new('RGB', (W, H), bg)
    y = 0
    for im in ims:
        res.paste(im, (0, y))
        y += im.height
    res.save(out)
    print(out, res.size)


if __name__ == '__main__':
    main()
