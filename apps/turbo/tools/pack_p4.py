#!/usr/bin/env python3
"""
TURBO on P4OS - packs the Blender renders into turbo_p4.pak, the one file
the game reads from /apps on the card (next to turbo.so).

    # the renders (once, ~40 min on an M2; tools/blender, see README.md)
    Blender -b -P tools/blender/cars.py  -- --out apps/turbo/assets/cars
    Blender -b -P tools/blender/props.py -- --out apps/turbo/assets/props
    # the pack
    python3 apps/turbo/tools/pack_p4.py [--cars DIR] [--props DIR] [--out FILE]
                                       # -> apps/turbo/build/turbo_p4.pak

The layout is the watch's turbo.pak (tools/pack_assets.py in AmoledOS), so
tb_art.c reads it unchanged:

    "TBPK" u16 version u16 count
    count x entry (50 bytes):
        char name[24]  u8 type  u8 flags  u16 w  u16 h  i16 ax  i16 ay
        u32 off  u32 clen  u32 rawlen  u16 ppm8  (px per metre x 8)  u16 extra
    data: one LZ4 block per entry

Types:
    1  sprite:  raw = w*h RGB565 (LE, ordered dither) then w*h alpha bytes
    2  plane:   raw = w*h bytes, how much a shadow darkens
    4  vehicle: raw = w*h * 2 bytes: (id << 4 | alpha >> 4), light

What is new is the scale, chosen for the P4's camera (focal 600 px, twice
the watch's):
    - the player's car ("near") at 1:1: rendered at twice the watch's size
      (cars.py --near-scale 2), it is drawn exactly as rendered;
    - the traffic ("far") at the renders' own 100 px per metre (the watch
      packed them at 70 %): a car in the next lane, 5 m ahead, is drawn at
      120 px per metre, nearly 1:1;
    - the props as props.py rendered them, 1.5 times the watch's pixels;
    - the backdrops at 2048 x 320.
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
APP = os.path.dirname(TOOLS)
BUILD = os.path.join(APP, 'build')
LZ4 = os.path.join(BUILD, 'lz4blk')
FAR_SCALE = 1.0
# the arches span the road and are drawn big only for an instant
PROP_SCALE = {'checkpoint': 0.67, 'finish': 0.67}


def lz4(data):
    src = os.path.join(TOOLS, 'lz4blk.c')
    if not os.path.exists(LZ4) or os.path.getmtime(LZ4) < os.path.getmtime(src):
        os.makedirs(BUILD, exist_ok=True)
        subprocess.check_call(['cc', '-O2', '-o', LZ4, src])
    return subprocess.run([LZ4], input=data, stdout=subprocess.PIPE, check=True).stdout


def lz4_check(src: bytes, rawlen: int) -> bytes:
    """the decoder tb_art.c has, in numpy-free Python, on a sample"""
    out = bytearray()
    i = 0
    while i < len(src):
        tok = src[i]; i += 1
        lit = tok >> 4
        if lit == 15:
            while True:
                b = src[i]; i += 1
                lit += b
                if b != 255:
                    break
        out.extend(src[i:i + lit]); i += lit
        if i >= len(src):
            break
        off = src[i] | (src[i + 1] << 8); i += 2
        m = (tok & 15) + 4
        if (tok & 15) == 15:
            while True:
                b = src[i]; i += 1
                m += b
                if b != 255:
                    break
        start = len(out) - off
        for k in range(m):
            out.append(out[start + k])
    assert len(out) == rawlen, (len(out), rawlen)
    return bytes(out)


entries = []


def add(name, typ, w, h, ax, ay, raw, ppm=0.0, extra=0, flags=0):
    assert len(name) < 24, name
    c = lz4(raw)
    if len(entries) % 16 == 0:
        assert lz4_check(c, len(raw)) == raw, name
    entries.append(dict(name=name, type=typ, w=w, h=h, ax=int(round(ax)), ay=int(round(ay)),
                        raw=len(raw), data=c, ppm8=int(round(ppm * 8)), extra=extra, flags=flags))


BAYER = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], np.int32)


def rgb565_dither(a):
    h, w = a.shape[:2]
    d = np.tile(BAYER, (h // 4 + 1, w // 4 + 1))[:h, :w]
    r = np.clip(a[..., 0].astype(np.int32) + (d >> 1) - 4, 0, 255)
    g = np.clip(a[..., 1].astype(np.int32) + (d >> 2) - 2, 0, 255)
    b = np.clip(a[..., 2].astype(np.int32) + (d >> 1) - 4, 0, 255)
    return (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)).astype('<u2')


def crop_box(mask):
    ys, xs = np.nonzero(mask)
    if len(xs) == 0:
        return None
    return xs.min(), xs.max() + 1, ys.min(), ys.max() + 1


def add_sprite(name, path, anchor, ppm, extra=0, scale=1.0):
    im = Image.open(path).convert('RGBA')
    if scale != 1.0:
        # premultiplied for the resize, so transparent edges do not bleed dark
        a = np.asarray(im).astype(np.float32) / 255.0
        a[..., :3] *= a[..., 3:4]
        im = Image.fromarray((a * 255).astype(np.uint8))
        im = im.resize((max(1, int(im.width * scale)), max(1, int(im.height * scale))), Image.LANCZOS)
        a = np.asarray(im).astype(np.float32) / 255.0
        al = np.maximum(a[..., 3:4], 1e-4)
        a[..., :3] = np.clip(a[..., :3] / al, 0, 1)
        im = Image.fromarray((a * 255).astype(np.uint8))
        anchor = (anchor[0] * scale, anchor[1] * scale)
        ppm *= scale
    a = np.asarray(im)
    box = crop_box(a[..., 3] > 0)
    if box is None:
        return
    x0, x1, y0, y1 = box
    c = a[y0:y1, x0:x1]
    raw = rgb565_dither(c).tobytes() + c[..., 3].astype(np.uint8).tobytes()
    add(name, 1, int(x1 - x0), int(y1 - y0), anchor[0] - x0, anchor[1] - y0, raw, ppm, extra)


def add_plane(name, path, anchor, ppm=0.0, cut=10, scale=1.0):
    im = Image.open(path).convert('L')
    if scale != 1.0:
        im = im.resize((max(1, int(im.width * scale)), max(1, int(im.height * scale))), Image.BOX)
        anchor = (anchor[0] * scale, anchor[1] * scale)
        ppm *= scale
    s = np.asarray(im).astype(np.int32)
    # quantised: a soft shadow loses nothing and the render's noise stops
    # eating the LZ4
    s = (np.round(s / 8) * 8).clip(0, 255)
    s[s < cut] = 0
    box = crop_box(s > 0)
    if box is None:
        return
    x0, x1, y0, y1 = box
    add(name, 2, int(x1 - x0), int(y1 - y0), anchor[0] - x0, anchor[1] - y0,
        s[y0:y1, x0:x1].astype(np.uint8).tobytes(), ppm)


def add_vehicle(name, base, anchor, ppm, scale=1.0):
    shi = Image.open(base + '_shade.png').convert('RGBA')
    idi = Image.open(base + '_id.png').convert('L')
    if scale != 1.0:
        size = (max(1, int(shi.width * scale)), max(1, int(shi.height * scale)))
        shi = shi.resize(size, Image.BOX)
        idi = idi.resize(size, Image.NEAREST)
        anchor = (anchor[0] * scale, anchor[1] * scale)
        ppm *= scale
    sh = np.asarray(shi)
    ids = np.asarray(idi).astype(np.int32)
    alpha = sh[..., 3].astype(np.int32)
    light = sh[..., :3].astype(np.int32).mean(axis=2)
    idv = (ids + 8) // 16
    idv[alpha == 0] = 0
    box = crop_box(alpha > 0)
    x0, x1, y0, y1 = box
    a4 = np.clip((alpha[y0:y1, x0:x1] + 8) // 17, 0, 15)
    b0 = ((idv[y0:y1, x0:x1] & 15) << 4) | a4
    b1 = np.clip(np.round(light[y0:y1, x0:x1]), 0, 255)
    b1[a4 == 0] = 0
    raw = np.stack([b0, b1], axis=-1).astype(np.uint8).tobytes()
    add(name, 4, int(x1 - x0), int(y1 - y0), anchor[0] - x0, anchor[1] - y0, raw, ppm)


def pack_cars(cars):
    mp = os.path.join(cars, 'meta.json')
    if not os.path.exists(mp):
        print('no car renders in', cars)
        return
    meta = json.load(open(mp))
    for key, r in meta['renders'].items():
        base = os.path.join(cars, key)
        if not os.path.exists(base + '_shade.png'):
            continue
        anchor = r['anchor']
        ppm = r.get('ppm', 0.0)
        near = key.startswith('near_')
        add_vehicle(key, base, anchor, ppm, 1.0 if near else FAR_SCALE)
        sp = base + '_shadow.png'
        if not os.path.exists(sp):
            continue
        if near:
            # one shadow per car, the straight frame's
            if key.endswith('_y3'):
                add_plane('nsh_' + key[5:-3], sp, anchor, 0.0, cut=24)
        else:
            add_plane('fsh_' + key[4:], sp, anchor, ppm, cut=16, scale=0.5)


def pack_props(props):
    mp = os.path.join(props, 'meta.json')
    if not os.path.exists(mp):
        print('no prop renders in', props)
        return
    meta = json.load(open(mp))
    for name, m in meta.get('props', {}).items():
        path = os.path.join(props, m.get('file', name + '.png'))
        if not os.path.exists(path):
            continue
        add_sprite('p_' + name, path, m['anchor'], m['ppm'], int(m.get('height_m', 0) * 10),
                   PROP_SCALE.get(name, 1.0))
        sf = m.get('shadow_file')
        if sf and os.path.exists(os.path.join(props, sf)):
            add_plane('psh_' + name, os.path.join(props, sf), m.get('shadow_anchor', m['anchor']), m['ppm'])
    for stage, m in meta.get('backdrops', {}).items():
        f = m.get('file', 'bg_%s.png' % stage) if isinstance(m, dict) else 'bg_%s.png' % stage
        path = os.path.join(props, f)
        if not os.path.exists(path):
            continue
        a = np.asarray(Image.open(path).convert('RGBA'))
        h, w = a.shape[:2]
        raw = rgb565_dither(a).tobytes() + a[..., 3].astype(np.uint8).tobytes()
        add('bg_' + stage, 1, w, h, 0, h - 1, raw)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cars', default=os.path.join(APP, 'assets', 'cars'))
    ap.add_argument('--props', default=os.path.join(APP, 'assets', 'props'))
    ap.add_argument('--out', default=os.path.join(BUILD, 'turbo_p4.pak'))
    args = ap.parse_args()
    pack_cars(args.cars)
    pack_props(args.props)
    if not entries:
        sys.exit('nothing to pack')
    hdr = b'TBPK' + struct.pack('<HH', 2, len(entries))
    off = len(hdr) + 50 * len(entries)
    table = bytearray()
    blob = bytearray()
    for e in entries:
        table += struct.pack('<24sBBHHhhIIIHH', e['name'].encode(), e['type'], e['flags'], e['w'], e['h'],
                             e['ax'], e['ay'], off + len(blob), len(e['data']), e['raw'],
                             e['ppm8'], e['extra'] & 0xFFFF)
        blob += e['data']
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    with open(args.out, 'wb') as f:
        f.write(hdr + table + blob)
    raw = sum(e['raw'] for e in entries)
    kinds = {}
    for e in entries:
        kinds[e['type']] = kinds.get(e['type'], 0) + 1
    print('%d entries (%s), %d KB raw -> %d KB in %s' % (len(entries), kinds, raw // 1024,
                                                         (len(hdr) + len(table) + len(blob)) // 1024, args.out))
    # what each stage asks of PSRAM is measured by the game (the log's
    # "art ... KB" line); the biggest entries, to see where the pack goes
    big = sorted(entries, key=lambda e: -e['raw'])[:12]
    for e in big:
        print('  %-22s %4dx%-4d raw %6d KB' % (e['name'], e['w'], e['h'], e['raw'] // 1024))


if __name__ == '__main__':
    main()
