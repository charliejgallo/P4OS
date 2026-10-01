#!/usr/bin/env python3
"""
GOLF on P4OS - packs the Blender renders into golf_p4.pak, the one file the
game reads (next to golf.so, in /apps on the card).

    # the renders, at twice the watch's pixels (the same cameras):
    B=/Applications/Blender.app/Contents/MacOS/Blender
    cd apps/golf/tools/blender
    $B -b -P props.py  -- --out ../../build/art/props  --scale 2 --ss 3
    $B -b -P golfer.py -- --out ../../build/art/render --scale 2
    # the pack:
    python3 apps/golf/tools/pack_p4.py      # -> apps/golf/build/golf_p4.pak

    --render DIR / --props DIR   where the renders are (default build/art/...)
    --fallback DIR               the watch's 1x renders (AmoledOS's
                                 apps/golf/assets/render): a frame not yet
                                 rendered at 2x is taken from there, scaled
                                 up, and said so. Only for trying the game
                                 while Blender runs.

Layout (little endian), the watch's with version 2 = GF_ART_SCALE 2:

    "GFPK" u16 version u16 count
    count x entry (50 bytes):
        char name[24]  u8 type  u8 flags  u16 w  u16 h  i16 ax  i16 ay
        u32 off  u32 clen  u32 rawlen  u16 ppm8  (px per metre x 8)  u16 extra
    data: one LZ4 block per entry

Types:
    1  sprite:  raw = w*h RGB565 (LE) then w*h alpha bytes. Alpha is straight.
    2  plane:   raw = w*h bytes (a shadow's strength); flags 1 = stored at
               half resolution, ax/ay in half pixels too
    3  golfer:  raw = w*h * 2 bytes: (id << 4 | alpha >> 4), shade.
               ax, ay = where the crop's top-left sits in the frame it was
               rendered in: 736x896 for the swing camera (principal point at
               its centre, 368, 448), 368x560 for the shop's turntable.
"""
import argparse
import json
import os
import struct
import subprocess
import sys

import numpy as np
from PIL import Image

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)                       # apps/golf
BUILD = os.path.join(ROOT, 'build')
LZ4 = os.path.join(BUILD, 'lz4blk')
VERSION = 2
SCALE = 2
SEQS = ('swing', 'idle', 'cheer', 'sad', 'turn')
HATS = 6

ap = argparse.ArgumentParser()
ap.add_argument('--render', default=os.path.join(BUILD, 'art', 'render'))
ap.add_argument('--props', default=os.path.join(BUILD, 'art', 'props'))
ap.add_argument('--fallback', default='')
ap.add_argument('--out', default=os.path.join(BUILD, 'golf_p4.pak'))
ARGS = ap.parse_args()


def lz4(data):
    src = os.path.join(TOOLS, 'lz4blk.c')
    if not os.path.exists(LZ4) or os.path.getmtime(LZ4) < os.path.getmtime(src):
        os.makedirs(BUILD, exist_ok=True)
        subprocess.check_call(['cc', '-O2', '-o', LZ4, src])
    return subprocess.run([LZ4], input=data, stdout=subprocess.PIPE, check=True).stdout


entries = []


def add(name, typ, w, h, ax, ay, raw, ppm=0.0, extra=0, flags=0):
    assert len(name) < 24, name
    entries.append(dict(name=name, type=typ, w=w, h=h, ax=int(round(ax)), ay=int(round(ay)),
                        raw=len(raw), data=lz4(raw), ppm8=int(round(ppm * 8)), extra=extra, flags=flags))


def rgb565(a):
    r = a[..., 0].astype(np.uint16)
    g = a[..., 1].astype(np.uint16)
    b = a[..., 2].astype(np.uint16)
    return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)


def sprite_raw(img):
    a = np.asarray(img.convert('RGBA'))
    return rgb565(a).astype('<u2').tobytes() + a[..., 3].astype(np.uint8).tobytes()


def pack_props():
    d = ARGS.props
    meta = json.load(open(os.path.join(d, 'meta.json')))
    for k in ['tree_pine', 'tree_oak', 'tree_poplar', 'tree_palm', 'bush']:
        m = meta[k]
        side = Image.open(os.path.join(d, m['side_files'][0]))
        add(k[:12] + '_s', 1, side.width, side.height, m['side_base_px'][0], m['side_base_px'][1],
            sprite_raw(side), m['side_px_per_m'], int(m['height_m'] * 100))
        top = Image.open(os.path.join(d, k + '_top.png'))
        add(k[:12] + '_t', 1, top.width, top.height, m['top_trunk_px'][0], m['top_trunk_px'][1],
            sprite_raw(top), m['top_px_per_m'], int(m['canopy_diameter_m'] * 100))
        sh = Image.open(os.path.join(d, k + '_topshadow.png')).convert('L')
        add(k[:12] + '_h', 2, sh.width, sh.height, m['top_trunk_px'][0], m['top_trunk_px'][1],
            np.asarray(sh).astype(np.uint8).tobytes(), m['top_px_per_m'])
    fm = meta.get('flag', {})
    for f in range(4):
        p = os.path.join(d, 'flag_side_%d.png' % f)
        if os.path.exists(p):
            im = Image.open(p)
            base = fm.get('side_base_px', [3.0 * SCALE, 62.85 * SCALE])
            add('flag_%d' % f, 1, im.width, im.height, base[0], base[1], sprite_raw(im),
                fm.get('side_px_per_m', 26.6 * SCALE))


fallbacks = []


def load_layer(prefix, hat):
    """(shade RGBA, ids L, shadow L or None) at 2x, from the 2x renders or,
    if missing and allowed, the watch's 1x scaled up"""
    shp = os.path.join(ARGS.render, prefix + '_shade.png')
    if os.path.exists(shp):
        sh = Image.open(shp).convert('RGBA')
        ids = Image.open(os.path.join(ARGS.render, prefix + '_id.png')).convert('L')
        sp = os.path.join(ARGS.render, prefix + '_shadow.png')
        shadow = Image.open(sp).convert('L') if (not hat and os.path.exists(sp)) else None
        return sh, ids, shadow
    if not ARGS.fallback:
        return None
    shp = os.path.join(ARGS.fallback, prefix + '_shade.png')
    if not os.path.exists(shp):
        return None
    fallbacks.append(prefix)
    sh = Image.open(shp).convert('RGBA')
    size = (sh.width * SCALE, sh.height * SCALE)
    sh = sh.resize(size, Image.BICUBIC)
    ids = Image.open(os.path.join(ARGS.fallback, prefix + '_id.png')).convert('L').resize(size, Image.NEAREST)
    sp = os.path.join(ARGS.fallback, prefix + '_shadow.png')
    shadow = Image.open(sp).convert('L').resize(size, Image.BILINEAR) if (not hat and os.path.exists(sp)) else None
    return sh, ids, shadow


def golfer_frame(prefix, name, hat=False):
    got = load_layer(prefix, hat)
    if not got:
        return False
    shi, idi, shadow = got
    sh = np.asarray(shi)
    ids = np.asarray(idi).astype(np.int32)
    alpha = sh[..., 3].astype(np.int32)
    shade = sh[..., :3].astype(np.int32).mean(axis=2)
    idv = (ids + 8) // 16
    cover = (alpha > 0) | (idv > 0)
    # pixels with coverage but no id borrow a neighbour's id
    if (cover & (idv == 0)).any():
        from itertools import product
        for _ in range(3):
            hole = cover & (idv == 0)
            if not hole.any():
                break
            for dx, dy in product((-1, 0, 1), repeat=2):
                sft = np.roll(np.roll(idv, dy, 0), dx, 1)
                fill = hole & (sft > 0)
                idv[fill] = sft[fill]
                hole = cover & (idv == 0)
    idv[alpha == 0] = 0
    ys, xs = np.nonzero(alpha > 0)
    if len(xs) == 0:
        add(name, 3, 1, 1, 0, 0, b'\0\0')
        return True
    x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
    a4 = np.clip((alpha[y0:y1, x0:x1] + 8) // 17, 0, 15)
    b0 = ((idv[y0:y1, x0:x1] & 15) << 4) | a4
    b1 = np.clip(np.round(shade[y0:y1, x0:x1]), 0, 255)
    b1[a4 == 0] = 0
    raw = np.stack([b0, b1], axis=-1).astype(np.uint8).tobytes()
    add(name, 3, int(x1 - x0), int(y1 - y0), x0, y0, raw)
    if shadow is not None:
        # Half resolution, smoothed and quantised: a soft shadow loses
        # nothing and the render's sampling noise stops eating the LZ4.
        # flags = 1: the game samples it back up.
        im = shadow.resize((shadow.width // 2, shadow.height // 2), Image.BOX)
        s = np.asarray(im).astype(np.float32)
        k = np.array([1, 2, 1], np.float32) / 4
        s = np.apply_along_axis(lambda r: np.convolve(r, k, 'same'), 1, s)
        s = np.apply_along_axis(lambda c: np.convolve(c, k, 'same'), 0, s)
        s = (np.round(s / 12) * 12).clip(0, 255).astype(np.uint8)
        s[s < 12] = 0
        ys, xs = np.nonzero(s > 0)
        if len(xs):
            sx0, sx1, sy0, sy1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
            add(name + 's', 2, int(sx1 - sx0), int(sy1 - sy0), sx0, sy0, s[sy0:sy1, sx0:sx1].tobytes(), flags=1)
    return True


def frame_count(seq):
    for d in (ARGS.render, ARGS.fallback):
        mp = os.path.join(d, 'meta.json') if d else ''
        if mp and os.path.exists(mp):
            m = json.load(open(mp)).get('sequences', {}).get(seq)
            if m:
                return m.get('frames', 0)
    return 0


def pack_golfer():
    for s in SEQS:
        n = frame_count(s)
        for f in range(n):
            base = '%s_%02d' % (s, f)
            if not golfer_frame(base, base):
                print('  %s: missing, the sequence stops at %d frames' % (base, f))
                break
            for k in range(HATS):
                golfer_frame('hat%d_%s' % (k, base), 'h%d%s%02d' % (k, s[:2], f), hat=True)


def main():
    pack_props()
    pack_golfer()
    hdr = b'GFPK' + struct.pack('<HH', VERSION, len(entries))
    off = len(hdr) + 50 * len(entries)
    table = bytearray()
    blob = bytearray()
    for e in entries:
        table += struct.pack('<24sBBHHhhIIIHH', e['name'].encode(), e['type'], e['flags'], e['w'], e['h'],
                             e['ax'], e['ay'], off + len(blob), len(e['data']), e['raw'],
                             e['ppm8'], e['extra'] & 0xFFFF)
        blob += e['data']
    os.makedirs(os.path.dirname(ARGS.out), exist_ok=True)
    with open(ARGS.out, 'wb') as f:
        f.write(hdr + table + blob)
    raw = sum(e['raw'] for e in entries)
    golfer = [e for e in entries if e['type'] == 3 and not e['name'].startswith('h')]
    print('%d entries, %d KB raw -> %d KB in %s' % (len(entries), raw // 1024,
                                                   (len(hdr) + len(table) + len(blob)) // 1024, ARGS.out))
    for s in SEQS:
        fr = [e for e in golfer if e['name'].startswith(s + '_')]
        if fr:
            print('  %-5s %2d frames, %4d KB coloured (RGB565 + alpha)' % (
                s, len(fr), sum(e['w'] * e['h'] * 3 for e in fr) // 1024))
    if fallbacks:
        print('WARNING: %d layers taken from the 1x renders, scaled up: %s ...' % (len(fallbacks), fallbacks[0]))


if __name__ == '__main__':
    main()
