#!/usr/bin/env python3
"""
Tortuga - the pack, from what Blender rendered (build/art):

    /Applications/Blender.app/Contents/MacOS/Blender -b -P tools/blender/board.py
    /Applications/Blender.app/Contents/MacOS/Blender -b -P tools/blender/turtle.py
    python3 tools/pack_p4.py            # build/tortuga_p4.pak

Every sheet (tools/blender/*.py writes <name>.png and <name>.json, a grid
of cells with the model's anchor) goes in as a sheet of runs, Bastion's
format (main/tg_gfx.h): frame = row * cols + col, each frame cropped to
what is not transparent. The sounds are made here (numpy). Neither the
renders nor the pack are in git.
"""
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
ART = os.path.join(BUILD, 'art')
LZ4 = os.path.join(BUILD, 'lz4blk')
PART = 7_500_000

PK_TEXT, PK_RGB565, PK_SPRITE, PK_DEPTH, PK_SHEET, _, PK_PCM = range(7)
BAYER = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], np.int32)

SHEETS = ['tg_ground', 'tg_rock', 'tg_lettuce', 'tg_key', 'tg_door', 'tg_goal', 'tg_bridge',
          'tg_walk', 'tg_bump', 'tg_eat', 'tg_party']


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


def add_sheet(p, art, name):
    meta = json.load(open(os.path.join(art, name + '.json')))
    im = np.asarray(Image.open(os.path.join(art, name + '.png')).convert('RGBA'))
    cw, ch = meta['cell']
    ax, ay = meta['anchor']
    cols, rows = meta['cols'], meta['rows']
    frames = []
    for r in range(rows):
        for c in range(cols):
            frames.append((im[r * ch:(r + 1) * ch, c * cw:(c + 1) * cw], ax, ay))
    p.add(name, PK_SHEET, cw, ch, encode_frames(frames, cols), extra=len(frames))


# ---------------------------------------------------------------------------
# Sounds, mu-law 8-bit mono at 16000 Hz
# ---------------------------------------------------------------------------

RATE = 16000


def env(n, a, d):
    t = np.arange(n) / RATE
    return np.minimum(1, t / a) * np.exp(-t / d)


def tone(freqs, dur, decay=0.25, gain=0.5, wave='sine'):
    n = int(dur * RATE)
    t = np.arange(n) / RATE
    out = np.zeros(n)
    for f in freqs:
        ph = 2 * np.pi * f * t
        out += np.sin(ph) if wave == 'sine' else np.sign(np.sin(ph)) * 0.5
    return gain * out / len(freqs) * env(n, 0.004, decay)


def seq(parts, gap=0.0):
    g = np.zeros(int(gap * RATE))
    out = []
    for p in parts:
        out += [p, g]
    return np.concatenate(out)


def sounds():
    rng = np.random.default_rng(3)
    step = tone([180, 260], 0.07, 0.025, 0.55)
    turn = tone([520], 0.06, 0.03, 0.35)
    n = int(0.16 * RATE)
    thud = (rng.standard_normal(n) * 0.4 + np.sin(2 * np.pi * 90 * np.arange(n) / RATE)) * env(n, 0.002, 0.05)
    n = int(0.45 * RATE)
    noise = rng.standard_normal(n)
    splash = np.convolve(noise, np.ones(6) / 6, 'same') * env(n, 0.01, 0.15) * 0.7
    munch = seq([tone([300, 450], 0.05, 0.02, 0.5), tone([280, 420], 0.05, 0.02, 0.5),
                 tone([320, 470], 0.05, 0.02, 0.5)], 0.03)
    key = seq([tone([1320, 1980], 0.08, 0.06, 0.4), tone([1760, 2640], 0.14, 0.1, 0.4)], 0.0)
    door = seq([tone([140, 210], 0.12, 0.08, 0.5, 'square'), tone([180, 270], 0.12, 0.08, 0.45, 'square')], 0.02)
    win = seq([tone([523, 659], 0.12, 0.1), tone([659, 784], 0.12, 0.1), tone([784, 1047], 0.12, 0.1),
               tone([1047, 1319, 1568], 0.5, 0.35)], 0.01)
    fail = seq([tone([392], 0.16, 0.12, 0.45), tone([330], 0.16, 0.12, 0.45), tone([262], 0.35, 0.25, 0.45)], 0.02)
    tap = tone([880], 0.03, 0.012, 0.3)
    return dict(snd_step=step, snd_turn=turn, snd_bump=thud, snd_splash=splash, snd_eat=munch, snd_key=key,
                snd_door=door, snd_win=win, snd_fail=fail, snd_tap=tap)


def main():
    art = sys.argv[sys.argv.index('--art') + 1] if '--art' in sys.argv else ART
    p = Pack()
    for name in SHEETS:
        add_sheet(p, art, name)
    for name, x in sounds().items():
        p.add(name, PK_PCM, len(x), 1, mulaw(x), extra=RATE)
    p.write(os.path.join(BUILD, 'tortuga_p4.pak'))


if __name__ == '__main__':
    main()
