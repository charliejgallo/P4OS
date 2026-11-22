#!/usr/bin/env python3
"""
Juegos de mesa - the packs, from what Blender rendered (../assets/<set>/):

    python3 pack_p4.py                  # build/mesa_p4.pak (the wooden set and the sounds)
                                        # and build/mesa_<set>_p4.pak for every other set rendered
    python3 pack_p4.py --sets madera    # only those

A board (B_bg, B_bgp, B_fl) goes as a JPEG, which the P4's engine decodes,
with its geometry as a line of text (B_geo, B_geop, B_flgeo); a piece's
frames as a sheet of runs (Bastion's format, ../main/jm_gfx.h) anchored at
its foot (the flat view's at its middle); the menu's pictures as RGB565.
The sounds are made here (numpy), into the wooden pack only. Neither the
renders nor the packs are in git.
"""
import glob
import io
import json
import os
import struct
import subprocess
import sys

import numpy as np
from PIL import Image

TOOLS = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(TOOLS)
ASSETS = os.path.join(APP, 'assets')
BUILD = os.path.join(APP, 'build')
LZ4 = os.path.join(BUILD, 'lz4blk')
PART = 7_500_000
SETS = ['madera', 'marmol', 'vidrio', 'metal']

PK_TEXT, PK_RGB565, PK_SPRITE, PK_DEPTH, PK_SHEET, _, PK_PCM, PK_FONT, PK_JPEG = range(9)
BAYER = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], np.int32)


def lz4(data):
    src = os.path.join(TOOLS, 'lz4blk.c')
    if not os.path.exists(LZ4) or os.path.getmtime(LZ4) < os.path.getmtime(src):
        os.makedirs(BUILD, exist_ok=True)
        subprocess.check_call(['cc', '-O2', '-o', LZ4, src])
    return subprocess.run([LZ4], input=data, stdout=subprocess.PIPE, check=True).stdout


def rgb565(a, dither=True):
    a = np.asarray(a)[..., :3].astype(np.int32)
    h, w = a.shape[:2]
    if dither:
        d = np.tile(BAYER, (h // 4 + 1, w // 4 + 1))[:h, :w]
        r = np.clip(a[..., 0] + (d >> 1) - 4, 0, 255)
        g = np.clip(a[..., 1] + (d >> 2) - 2, 0, 255)
        b = np.clip(a[..., 2] + (d >> 1) - 4, 0, 255)
    else:
        r, g, b = a[..., 0], a[..., 1], a[..., 2]
    return (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)).astype('<u2')


def mulaw(x):
    x = np.clip(np.asarray(x, np.float64), -1, 1)
    s = (x * 32635).astype(np.int32)
    sign = (s < 0)
    s = np.abs(s) + 0x84
    s = np.minimum(s, 32635 + 0x84)
    exp = np.floor(np.log2(np.maximum(s, 1))).astype(np.int32) - 7
    exp = np.clip(exp, 0, 7)
    man = (s >> (exp + 3)) & 0x0F
    u = ~((sign.astype(np.int32) << 7) | (exp << 4) | man) & 0xFF
    return u.astype(np.uint8).tobytes()


class Pack:
    def __init__(self):
        self.entries = []

    def add(self, name, typ, w, h, raw, extra=0, x=0, y=0, compress=True):
        assert len(name.encode()) < 24, name
        assert all(e['name'] != name for e in self.entries), name
        c = lz4(raw) if compress and len(raw) > 64 else raw
        if len(c) >= len(raw):
            c = raw
        self.entries.append(dict(name=name, type=typ, w=int(w), h=int(h), x=int(x), y=int(y), raw=len(raw), data=c,
                                 extra=int(extra)))

    def write(self, path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        n = len(self.entries)
        off = 8 + n * 52
        tab = b''
        for e in self.entries:
            nm = e['name'].encode().ljust(24, b'\0')
            tab += nm + struct.pack('<BBHHhhIIIIH', e['type'], 0, e['w'], e['h'], e['x'], e['y'], off, len(e['data']),
                                    e['raw'], e['extra'], 0)
            off += len(e['data'])
        blob = b'BSPK' + struct.pack('<HH', 1, n) + tab + b''.join(e['data'] for e in self.entries)
        for i in range(1, 16):
            if os.path.exists('%s.%d' % (path, i)):
                os.remove('%s.%d' % (path, i))
        for i in range(0, max(1, (len(blob) + PART - 1) // PART)):
            with open(path if i == 0 else '%s.%d' % (path, i), 'wb') as f:
                f.write(blob[i * PART:(i + 1) * PART])
        raw = sum(e['raw'] for e in self.entries)
        print('%s: %d entries, %.2f MB (%.1f MB unpacked)' % (os.path.basename(path), n, len(blob) / 1e6, raw / 1e6))
        return len(blob)


def encode_frames(frames, cols):
    """frames: [(rgba uint8 array, anchor x, anchor y)] -> a sheet of runs"""
    head = bytearray(b'BSSH' + struct.pack('<HH', len(frames), cols))
    table = bytearray()
    body = bytearray()
    base = 8 + 12 * len(frames)
    for (a, ax, ay) in frames:
        al = a[..., 3]
        ys, xs = np.nonzero(al >= 8)
        if len(xs) == 0:
            table += struct.pack('<hhHHI', 0, 0, 0, 0, 0)
            continue
        x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
        c = a[y0:y1, x0:x1]
        px = rgb565(c[..., :3])
        al = c[..., 3].astype(np.int32)
        cls = np.where(al < 8, 0, np.where(al >= 248, 1, 2))
        h, w = cls.shape
        off = base + len(body)
        rows = bytearray(4 * h)
        ops = bytearray()
        for y in range(h):
            struct.pack_into('<I', rows, 4 * y, off + 4 * h + len(ops))
            r = cls[y]
            cut = np.flatnonzero(np.diff(r)) + 1
            starts = np.concatenate(([0], cut))
            ends = np.concatenate((cut, [w]))
            for s, e in zip(starts, ends):
                k = int(r[s])
                n = int(e - s)
                if k == 0:
                    if e == w:
                        break
                    ops += struct.pack('<H', (1 << 14) | n)
                elif k == 1:
                    ops += struct.pack('<H', (2 << 14) | n) + px[y, s:e].tobytes()
                else:
                    pair = np.empty(n * 2, '<u2')
                    pair[0::2] = px[y, s:e]
                    pair[1::2] = al[y, s:e]
                    ops += struct.pack('<H', (3 << 14) | n) + pair.tobytes()
            ops += struct.pack('<H', 0)
        if len(ops) & 2:
            ops += b'\0\0'
        body += rows + ops
        table += struct.pack('<hhHHI', int(x0) - ax, int(y0) - ay, w, h, off)
    return bytes(head + table + body)


# ---------------------------------------------------------------------------
# A set
# ---------------------------------------------------------------------------

BOARD_SUFFIX = {'_bg': '_geo', '_bgp': '_geop', '_fl': '_flgeo'}


def add_board(p, d, base):
    meta = json.load(open(os.path.join(d, base + '.json')))
    im = Image.open(os.path.join(d, base + '.png')).convert('RGB')
    buf = io.BytesIO()
    im.save(buf, 'JPEG', quality=88, subsampling=2, optimize=True)
    p.add(base, PK_JPEG, im.width, im.height, buf.getvalue(), compress=False)
    for suf, geo in BOARD_SUFFIX.items():
        if base.endswith(suf):
            name = base[:-len(suf)] + geo
            txt = 'S=%g E=%g rot=%d cx=%g cy=%g' % (meta['S'], meta['E'], meta.get('rot', 0), meta['cx'], meta['cy'])
            p.add(name, PK_TEXT, len(txt), 1, txt.encode())


def add_sheet(p, d, base):
    meta = json.load(open(os.path.join(d, base + '.json')))
    frames = []
    for f in meta['frames']:
        a = np.asarray(Image.open(os.path.join(d, f)).convert('RGBA'))
        frames.append((a, int(meta['ax']), int(meta['ay'])))
    data = encode_frames(frames, len(frames))
    p.add(base, PK_SHEET, meta['w'], meta['h'], data, extra=len(frames))


def add_thumb(p, d, base):
    im = Image.open(os.path.join(d, base + '.png')).convert('RGB')
    p.add(base, PK_RGB565, im.width, im.height, rgb565(np.asarray(im)).tobytes())


def pack_set(s, sounds):
    d = os.path.join(ASSETS, s)
    if not os.path.isdir(d):
        return None
    p = Pack()
    for js in sorted(glob.glob(os.path.join(d, '*.json'))):
        base = os.path.basename(js)[:-5]
        if any(base.endswith(k) for k in BOARD_SUFFIX):
            add_board(p, d, base)
        else:
            add_sheet(p, d, base)
    for th in sorted(glob.glob(os.path.join(d, '*_th.png'))):
        add_thumb(p, d, os.path.basename(th)[:-4])
    if sounds:
        add_sounds(p)
    name = 'mesa_p4.pak' if s == 'madera' else 'mesa_%s_p4.pak' % s
    p.write(os.path.join(BUILD, name))
    return name


# ---------------------------------------------------------------------------
# Sounds: wood and dice, synthesised
# ---------------------------------------------------------------------------

RATE = 16000


def env(n, a, d):
    t = np.arange(n) / RATE
    return np.minimum(t / a, 1.0) * np.exp(-t / d)


def knock(f0=520, dur=0.18, bright=1.0, seed=1):
    """a piece set down on wood: a damped resonance and a click"""
    rng = np.random.default_rng(seed)
    n = int(RATE * dur)
    t = np.arange(n) / RATE
    s = np.zeros(n)
    for k, (m, g) in enumerate(((1.0, 1.0), (2.32, 0.5), (3.9, 0.25 * bright), (5.1, 0.12 * bright))):
        s += g * np.sin(2 * np.pi * f0 * m * t + k) * np.exp(-t / (0.035 / (1 + k * 0.6)))
    click = rng.standard_normal(n) * np.exp(-t / 0.004) * 0.6 * bright
    s += click
    return s / np.max(np.abs(s)) * 0.9


def tone(freqs, dur, decay=0.25, gain=0.6):
    n = int(RATE * dur)
    t = np.arange(n) / RATE
    s = np.zeros(n)
    for f in freqs:
        s += np.sin(2 * np.pi * f * t) * np.exp(-t / decay)
    return s / max(1e-9, np.max(np.abs(s))) * gain * env(n, 0.004, 10)


def seq(parts, gap=0.0):
    out = []
    for p in parts:
        out.append(p)
        if gap:
            out.append(np.zeros(int(RATE * gap)))
    return np.concatenate(out)


def dice():
    rng = np.random.default_rng(7)
    parts = []
    for i in range(7):
        parts.append(knock(900 + rng.integers(0, 600), 0.05, 1.4, seed=i) * (0.9 - i * 0.1))
        parts.append(np.zeros(int(RATE * (0.03 + rng.random() * 0.05))))
    return np.concatenate(parts)


def add_sounds(p):
    S = {
        'move': knock(480, 0.16),
        'capture': seq([knock(430, 0.10, seed=2), knock(560, 0.16, seed=3)], 0.04),
        'check': tone([880, 1320], 0.35, 0.12, 0.45),
        'dice': dice(),
        'flip': knock(1200, 0.08, 0.6, seed=4) * 0.8,
        'drop': seq([knock(700, 0.06, 1.2, seed=5), knock(760, 0.08, 1.0, seed=6) * 0.5], 0.03),
        'win': seq([tone([523], 0.14, 0.2), tone([659], 0.14, 0.2), tone([784], 0.14, 0.2), tone([1047, 784], 0.5, 0.3)]),
        'lose': seq([tone([392], 0.18, 0.2), tone([349], 0.18, 0.2), tone([294, 220], 0.5, 0.3)]),
        'tap': knock(1500, 0.04, 0.5, seed=8) * 0.6,
        'deny': tone([180, 190], 0.16, 0.1, 0.5),
    }
    for k, v in S.items():
        raw = mulaw(v)
        p.add('s_' + k, PK_PCM, len(raw), 1, raw, extra=RATE)


def main():
    want = SETS
    if '--sets' in sys.argv:
        want = sys.argv[sys.argv.index('--sets') + 1].split(',')
    for s in want:
        n = pack_set(s, s == 'madera')
        if not n and s == 'madera':
            print('no renders in assets/madera: run the Blender scripts first')


main()
