#!/usr/bin/env python3
"""
BASTION - the packs for the card, from what Blender rendered (assets/) and
what is made here (fonts, icons, sounds):

    python3 pack_p4.py                 # build/bastion_p4.pak and one bastion_<world>_p4.pak a world
    python3 pack_p4.py --common        # only the common pack
    python3 pack_p4.py --worlds luna,selva
    python3 pack_p4.py --flat          # no renders at all: flat stand-ins, to try the game

The layouts are in ../main/bs_pak.h (the pack) and ../main/bs_gfx.h (a
sheet's runs, a font). A sheet that was not rendered gets a stand-in and a
warning, so the game always starts. Neither the renders nor the packs are
in git.
"""
import json
import os
import struct
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

TOOLS = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(TOOLS)
ASSETS = os.path.join(APP, 'assets')
BUILD = os.path.join(APP, 'build')
LZ4 = os.path.join(BUILD, 'lz4blk')
FONT = os.path.join(TOOLS, 'fonts', 'Rubik-Medium.ttf')
PART = 7_500_000

sys.path.insert(0, TOOLS)
import bsworlds  # noqa: E402

PK_TEXT, PK_RGB565, PK_SPRITE, PK_DEPTH, PK_SHEET, _, PK_PCM, PK_FONT = range(8)

# the order of bs_data.h
TOWERS = ['gatling', 'cannon', 'laser', 'missile', 'tesla', 'cryo', 'flame', 'rail', 'mortar', 'drone', 'emp',
          'gravity', 'radar', 'bank']
TOWER_COLOR = ['#ffb020', '#ff4038', '#ff3bd4', '#9be830', '#40b8ff', '#a8f4ff', '#ff7a1a', '#a070ff', '#2ee6c0',
               '#ff6fa8', '#5a6cff', '#c040ff', '#38ff9c', '#ffd83a']
SPIN = {'tesla', 'cryo', 'drone', 'emp', 'gravity', 'radar', 'bank'}
ALIENS = ['grub', 'runner', 'swarm', 'shell', 'wasp', 'split', 'shield', 'healer', 'brute', 'phase', 'jammer',
          'carrier', 'queen', 'titan', 'overmind', 'mini']
ALIEN_COLOR = ['#6cff4a', '#ffe040', '#ff5040', '#70a0ff', '#ff9030', '#c060ff', '#40e8ff', '#80ffc0', '#d03030',
               '#c8b0ff', '#ff40c0', '#30d0b0', '#b0ff40', '#ff5040', '#b070ff', '#c060ff']
ALIEN_SIZE = {'brute': 1, 'carrier': 1, 'queen': 2, 'titan': 2, 'overmind': 2}
SOUNDS = ['shot_gatling', 'shot_cannon', 'laser', 'shot_missile', 'zap', 'cryo', 'flame', 'shot_rail', 'shot_mortar',
          'drone', 'emp', 'gravity', 'boom_s', 'boom_m', 'boom_l', 'hit', 'die1', 'die2', 'die3', 'boss_roar',
          'boss_die', 'fort_hit', 'alarm', 'build', 'upgrade', 'sell', 'coin', 'wave_start', 'wave_clear', 'victory',
          'defeat', 'tap', 'deny', 'strike', 'freeze', 'repair', 'star']
MUSIC = ['music0', 'music1', 'music2', 'music3', 'music_menu']
ICONS = ['coin', 'heart', 'wave', 'pause', 'speed1', 'speed2', 'speed3', 'play', 'upgrade', 'sell', 'target',
         'strike', 'freeze', 'repair', 'lock', 'star', 'star_off', 'core', 'close', 'jam']

BAYER = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], np.int32)
WARN = []


def warn(s):
    WARN.append(s)
    print('  ! ' + s)


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

    def add(self, name, typ, w, h, raw, extra=0, x=0, y=0):
        assert len(name.encode()) < 24, name
        assert all(e['name'] != name for e in self.entries), name
        c = lz4(raw) if len(raw) > 64 else raw
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
        # stale parts would be read as the end of the pack
        for i in range(1, 16):
            if os.path.exists('%s.%d' % (path, i)):
                os.remove('%s.%d' % (path, i))
        for i in range(0, max(1, (len(blob) + PART - 1) // PART)):
            with open(path if i == 0 else '%s.%d' % (path, i), 'wb') as f:
                f.write(blob[i * PART:(i + 1) * PART])
        raw = sum(e['raw'] for e in self.entries)
        print('%s: %d entries, %.1f MB (%.1f MB unpacked)' % (os.path.basename(path), n, len(blob) / 1e6, raw / 1e6))
        return len(blob)


# ---------------------------------------------------------------------------
# Sheets: frames as runs
# ---------------------------------------------------------------------------

def encode_frames(frames, cols):
    """frames: list of (rgba uint8 array, anchor x, anchor y) -> a sheet"""
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


def cells(img, info):
    """a rendered sheet cut into its cells: [(rgba, ax, ay)], cols"""
    a = np.asarray(img.convert('RGBA'))
    cw, ch = info['cell']
    ax, ay = info['anchor']
    out = []
    for r in range(info['rows']):
        for c in range(info['cols']):
            out.append((a[r * ch:(r + 1) * ch, c * cw:(c + 1) * cw], ax, ay))
    return out, info['cols']


def load_sheet(group, name):
    p = os.path.join(ASSETS, group, name + '.png')
    if FLAT or not os.path.exists(p) or not os.path.exists(p[:-4] + '.json'):
        return None, None
    return Image.open(p), json.load(open(p[:-4] + '.json'))


def hexrgb(c):
    c = c.lstrip('#')
    return tuple(int(c[i:i + 2], 16) for i in (0, 2, 4))


def stand_in(kind, color, cols, rows, cell, anchor):
    """a flat drawing in place of a render"""
    cw, ch = cell
    ax, ay = anchor
    im = Image.new('RGBA', (cw * cols, ch * rows), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    col = hexrgb(color)
    dark = tuple(v // 3 for v in col)
    for r in range(rows):
        for c in range(cols):
            x, y = c * cw + ax, r * ch + ay
            if kind == 'base':
                d.polygon([(x + 34 * np.cos(i * np.pi / 3), y + 27 * np.sin(i * np.pi / 3)) for i in range(6)],
                          fill=(58, 66, 86, 255), outline=col + (255,))
            elif kind == 'head':
                a = c * 2 * np.pi / cols
                d.ellipse([x - 18, y - 48, x + 18, y - 18], fill=col + (255,), outline=dark + (255,))
                d.line([x, y - 33, x + 40 * np.cos(a), y - 33 - 32 * np.sin(a)], fill=(220, 225, 235, 255), width=7)
            elif kind == 'fort':
                k = [1.0, 0.7, 0.4][c]
                d.polygon([(x + 100 * np.cos(i * np.pi / 3), y - 30 + 70 * np.sin(i * np.pi / 3)) for i in range(6)],
                          fill=(int(58 * k), int(66 * k), int(86 * k), 255), outline=col + (255,))
                d.rectangle([x - 40, y - 150, x + 40, y - 30], fill=(int(93 * k), int(104 * k), int(131 * k), 255))
                d.ellipse([x - 22, y - 190, x + 22, y - 146], fill=([(56, 216, 255), (255, 176, 32), (255, 64, 56)][c]) + (255,))
            else:
                a = c * 2 * np.pi / cols
                s = cw * 0.22 * (1.0 + 0.08 * (r & 1))
                d.ellipse([x - s, y - s * 1.5, x + s, y - s * 0.1], fill=col + (255,), outline=dark + (255,))
                ex, ey = x + s * 0.6 * np.cos(a), y - s * 0.8 - s * 0.5 * np.sin(a)
                d.ellipse([ex - 4, ey - 4, ex + 4, ey + 4], fill=(255, 255, 255, 255))
    return im, dict(cell=list(cell), anchor=list(anchor), cols=cols, rows=rows)


# ---------------------------------------------------------------------------
# Fonts and icons, made here
# ---------------------------------------------------------------------------

CHARS = ''.join(chr(c) for c in range(32, 127)) + '¡¿ÁÉÍÓÚÑÜÖÄáéíóúñüöäß°×·«»'


def font_entry(size):
    f = ImageFont.truetype(FONT, size)
    asc, desc = f.getmetrics()
    glyphs = []
    x = y = rowh = 0
    AW = 512
    place = []
    for ch in CHARS:
        bb = f.getbbox(ch, anchor='ls')
        w, h = max(0, bb[2] - bb[0]), max(0, bb[3] - bb[1])
        if x + w + 1 > AW:
            x = 0
            y += rowh + 1
            rowh = 0
        place.append((ch, x, y, w, h, bb))
        x += w + 1
        rowh = max(rowh, h)
    AH = y + rowh + 1
    atlas = Image.new('L', (AW, AH), 0)
    d = ImageDraw.Draw(atlas)
    for (ch, gx, gy, w, h, bb) in place:
        if w and h:
            d.text((gx - bb[0], gy - bb[1]), ch, font=f, fill=255, anchor='ls')
        adv = int(round(f.getlength(ch)))
        glyphs.append(struct.pack('<IHHBBbbBBH', ord(ch), gx, gy, min(w, 255), min(h, 255), max(-128, min(127, bb[0])),
                                  max(-128, min(127, asc + bb[1])), min(adv, 255), 0, 0))
    head = b'BSFN' + struct.pack('<HHHHH', len(glyphs), asc + desc, asc, AW, AH) + b'\0\0'
    return head + b''.join(glyphs) + np.asarray(atlas).tobytes()


def draw_icon(name, S=4):
    """the HUD's icons, drawn four times too big and brought down"""
    n = 56 * S
    im = Image.new('RGBA', (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    c = n / 2

    def P(pts):
        return [(c + x * S, c + y * S) for (x, y) in pts]
    W = (255, 255, 255, 255)
    if name == 'coin':
        d.ellipse(P([(-20, -20), (20, 20)]), fill=(255, 190, 40, 255), outline=(150, 90, 0, 255), width=3 * S)
        d.ellipse(P([(-11, -11), (11, 11)]), outline=(255, 235, 150, 255), width=3 * S)
    elif name == 'heart':
        d.ellipse(P([(-21, -18), (1, 4)]), fill=(255, 60, 80, 255))
        d.ellipse(P([(-1, -18), (21, 4)]), fill=(255, 60, 80, 255))
        d.polygon(P([(-20, -2), (20, -2), (0, 21)]), fill=(255, 60, 80, 255))
        d.ellipse(P([(-15, -13), (-7, -5)]), fill=(255, 170, 180, 255))
    elif name == 'wave':
        for k in (-12, 0, 12):
            d.polygon(P([(-16, k - 8), (0, k + 2), (16, k - 8), (16, k - 1), (0, k + 9), (-16, k - 1)]),
                      fill=(120, 230, 255, 255))
    elif name == 'pause':
        d.rounded_rectangle(P([(-15, -17), (-5, 17)]), radius=3 * S, fill=W)
        d.rounded_rectangle(P([(5, -17), (15, 17)]), radius=3 * S, fill=W)
    elif name in ('speed1', 'speed2', 'speed3'):
        k = int(name[-1])
        w = 13
        x0 = -w * k / 2
        for i in range(k):
            d.polygon(P([(x0 + i * w, -14), (x0 + i * w + 12, 0), (x0 + i * w, 14)]), fill=W)
    elif name == 'play':
        d.polygon(P([(-14, -20), (20, 0), (-14, 20)]), fill=W)
    elif name == 'upgrade':
        d.polygon(P([(0, -22), (19, -1), (8, -1), (8, 8), (-8, 8), (-8, -1), (-19, -1)]), fill=(90, 255, 120, 255))
        d.rounded_rectangle(P([(-8, 13), (8, 20)]), radius=2 * S, fill=(90, 255, 120, 255))
    elif name == 'sell':
        d.ellipse(P([(-18, -4), (18, 20)]), fill=(255, 190, 40, 255), outline=(150, 90, 0, 255), width=2 * S)
        d.polygon(P([(0, 2), (-12, -12), (-5, -12), (-5, -24), (5, -24), (5, -12), (12, -12)]), fill=W)
    elif name == 'target':
        d.ellipse(P([(-17, -17), (17, 17)]), outline=W, width=4 * S)
        for (a, b) in (((-24, 0), (-9, 0)), ((9, 0), (24, 0)), ((0, -24), (0, -9)), ((0, 9), (0, 24))):
            d.line(P([a, b]), fill=W, width=4 * S)
        d.ellipse(P([(-4, -4), (4, 4)]), fill=(255, 80, 80, 255))
    elif name == 'strike':
        d.polygon(P([(8, -26), (20, -26), (4, 6), (-6, 6)]), fill=(255, 150, 60, 255))
        d.polygon(P([(11, -26), (17, -26), (2, 4), (-2, 4)]), fill=(255, 240, 200, 255))
        for i in range(8):
            a = i * np.pi / 4
            d.line(P([(np.cos(a) * 8, 12 + np.sin(a) * 6), (np.cos(a) * 22, 12 + np.sin(a) * 14)]),
                   fill=(255, 190, 80, 255), width=3 * S)
        d.ellipse(P([(-9, 5), (9, 19)]), fill=(255, 250, 220, 255))
    elif name == 'freeze':
        for i in range(3):
            a = i * np.pi / 3
            dx, dy = np.cos(a) * 23, np.sin(a) * 23
            d.line(P([(-dx, -dy), (dx, dy)]), fill=(170, 240, 255, 255), width=4 * S)
            for s in (-1, 1):
                bx, by = dx * s * 0.62, dy * s * 0.62
                for t in (-1, 1):
                    b = a + (np.pi if s < 0 else 0) + t * 0.8
                    d.line(P([(bx, by), (bx + np.cos(b) * 8, by + np.sin(b) * 8)]), fill=(170, 240, 255, 255), width=3 * S)
        d.ellipse(P([(-5, -5), (5, 5)]), fill=W)
    elif name == 'repair':
        d.rounded_rectangle(P([(-7, -22), (7, 22)]), radius=3 * S, fill=(90, 255, 140, 255))
        d.rounded_rectangle(P([(-22, -7), (22, 7)]), radius=3 * S, fill=(90, 255, 140, 255))
    elif name == 'lock':
        d.arc(P([(-12, -24), (12, 4)]), 180, 360, fill=(190, 195, 210, 255), width=5 * S)
        d.rounded_rectangle(P([(-17, -8), (17, 20)]), radius=4 * S, fill=(190, 195, 210, 255))
        d.ellipse(P([(-4, 1), (4, 9)]), fill=(40, 44, 60, 255))
    elif name in ('star', 'star_off'):
        pts = []
        for i in range(10):
            a = -np.pi / 2 + i * np.pi / 5
            r = 14 if i % 2 == 0 else 6
            pts.append((np.cos(a) * r, np.sin(a) * r))
        d.polygon(P(pts), fill=(255, 205, 50, 255) if name == 'star' else (70, 76, 96, 255),
                  outline=(160, 100, 0, 255) if name == 'star' else (40, 44, 60, 255))
    elif name == 'core':
        hexa = [(np.cos(i * np.pi / 3 + np.pi / 6) * 20, np.sin(i * np.pi / 3 + np.pi / 6) * 20) for i in range(6)]
        d.polygon(P(hexa), fill=(40, 200, 255, 255), outline=(10, 80, 140, 255))
        d.polygon(P([hexa[4], hexa[5], hexa[0], (0, 0)]), fill=(150, 240, 255, 255))
        d.polygon(P([hexa[1], hexa[2], hexa[3], (0, 0)]), fill=(20, 130, 210, 255))
    elif name == 'close':
        d.line(P([(-14, -14), (14, 14)]), fill=W, width=6 * S)
        d.line(P([(-14, 14), (14, -14)]), fill=W, width=6 * S)
    elif name == 'jam':
        d.polygon(P([(4, -22), (-12, 4), (-2, 4), (-6, 22), (12, -6), (2, -6)]), fill=(255, 80, 220, 255))
    return im.resize((56, 56), Image.LANCZOS)


def fit(im, box):
    """an RGBA picture cropped to what shows and brought down to fit a box"""
    bb = im.getbbox()
    if not bb:
        return Image.new('RGBA', (box, box), (0, 0, 0, 0))
    im = im.crop(bb)
    k = min(box / im.width, box / im.height, 1.0)
    if k < 1.0:
        im = im.resize((max(1, int(im.width * k)), max(1, int(im.height * k))), Image.LANCZOS)
    out = Image.new('RGBA', (box, box), (0, 0, 0, 0))
    out.paste(im, ((box - im.width) // 2, (box - im.height) // 2))
    return out


def sprite_bytes(im):
    """LVGL's RGB565A8: the colour plane, then the alpha plane"""
    a = np.asarray(im.convert('RGBA'))
    return rgb565(a[..., :3]).tobytes() + a[..., 3].tobytes()


# ---------------------------------------------------------------------------
# The common pack
# ---------------------------------------------------------------------------

def gallery(p):
    """the cards' portraits (tools/blender/gallery.py): each alien's four
    frames cut to the same box, so they play as a loop, and each tower at
    its third tier. Without the renders, the game's own small ones."""
    for aid in ALIENS:
        if aid == 'mini':
            continue
        path = os.path.join(ASSETS, 'gallery', 'al_%s.png' % aid)
        if FLAT or not os.path.exists(path):
            if not FLAT:
                warn('the gallery lacks %s: run tools/blender/gallery.py' % aid)
            continue
        im = Image.open(path).convert('RGBA')
        w = im.width // 4
        frames = [im.crop((i * w, 0, (i + 1) * w, im.height)) for i in range(4)]
        # one box for the four: what any of them covers
        boxes = [f.getbbox() for f in frames if f.getbbox()]
        x0, y0 = min(b[0] for b in boxes), min(b[1] for b in boxes)
        x1, y1 = max(b[2] for b in boxes), max(b[3] for b in boxes)
        k = min(1.0, 260.0 / (x1 - x0), 240.0 / (y1 - y0))
        for i, f in enumerate(frames):
            c = f.crop((x0, y0, x1, y1))
            if k < 1.0:
                c = c.resize((max(1, int(c.width * k)), max(1, int(c.height * k))), Image.LANCZOS)
            p.add('ga_%s%d' % (aid, i), PK_SPRITE, c.width, c.height, sprite_bytes(c))
    for tid in TOWERS:
        path = os.path.join(ASSETS, 'gallery', 'tw_%s.png' % tid)
        if FLAT or not os.path.exists(path):
            if not FLAT:
                warn('the gallery lacks %s: run tools/blender/gallery.py' % tid)
            continue
        im = Image.open(path).convert('RGBA')
        c = im.crop(im.getbbox())
        k = min(1.0, 230.0 / c.width, 300.0 / c.height)
        if k < 1.0:
            c = c.resize((max(1, int(c.width * k)), max(1, int(c.height * k))), Image.LANCZOS)
        p.add('gt_%s' % tid, PK_SPRITE, c.width, c.height, sprite_bytes(c))


def title_picture():
    """the title screen: a world behind, the fort and some towers in front"""
    bgp = os.path.join(ASSETS, 'worlds', 'luna_bg.png')
    if not FLAT and os.path.exists(bgp):
        bg = Image.open(bgp).convert('RGB')
    else:
        bg = Image.new('RGB', (720, 1280), (10, 14, 30))
        d = ImageDraw.Draw(bg)
        rnd = np.random.RandomState(4)
        for _ in range(500):
            x, y, v = rnd.randint(0, 720), rnd.randint(0, 1280), rnd.randint(60, 255)
            d.point((x, y), fill=(v, v, v))
    bg = bg.filter(ImageFilter.GaussianBlur(3))
    a = np.asarray(bg).astype(np.float32)
    yy = np.linspace(0, 1, 1280)[:, None, None]
    a = a * (0.35 + 0.45 * np.abs(yy - 0.15))
    return Image.fromarray(np.clip(a, 0, 255).astype(np.uint8))


def common():
    p = Pack()
    print('fonts and icons')
    for nm, size in (('font_s', 22), ('font_m', 28), ('font_l', 40)):
        p.add(nm, PK_FONT, 0, 0, font_entry(size))
    p.add('ui', PK_SHEET, 0, 0, encode_frames([(np.asarray(draw_icon(n)), 28, 28) for n in ICONS], len(ICONS)), len(ICONS))

    print('towers')
    icons = []
    for ti, tid in enumerate(TOWERS):
        for tier in (1, 2, 3):
            base, bi = load_sheet('towers', '%s%d_base' % (tid, tier))
            head, hi = load_sheet('towers', '%s%d_head' % (tid, tier))
            if base is None or head is None:
                if not FLAT:
                    warn('tower %s%d is not rendered: a stand-in' % (tid, tier))
                base, bi = stand_in('base', TOWER_COLOR[ti], 1, 1, (128, 176), (64, 136))
                head, hi = stand_in('head', TOWER_COLOR[ti], 16, 1, (128, 176), (64, 136))
            fb, _ = cells(base, bi)
            fh, _ = cells(head, hi)
            p.add('tw_%s%d_b' % (tid, tier), PK_SHEET, 0, 0, encode_frames(fb, 1), 1)
            p.add('tw_%s%d_h' % (tid, tier), PK_SHEET, 0, 0, encode_frames(fh, 16), 16)
            if tier in (1, 3):
                # its portrait: looking down and to the left, or a frame of its loop
                k = 0 if tid in SPIN else 10
                both = Image.fromarray(fb[0][0]).copy()
                both.alpha_composite(Image.fromarray(fh[k][0]))
                if tier == 1:
                    icons.append((np.asarray(fit(both, 64)), 32, 32))
                    p.add('ti_%s' % tid, PK_SPRITE, 120, 120, sprite_bytes(fit(both, 120)))
                else:
                    p.add('tj_%s' % tid, PK_SPRITE, 120, 120, sprite_bytes(fit(both, 120)))
    p.add('tw_icons', PK_SHEET, 0, 0, encode_frames(icons, len(icons)), len(icons))

    print('aliens')
    icons = []
    for ai, aid in enumerate(ALIENS):
        size = ALIEN_SIZE.get(aid, 0)
        cell = [(96, 96), (144, 144), (224, 224)][size]
        anchor = [(48, 70), (72, 108), (112, 170)][size]
        src = 'split' if aid == 'mini' else aid
        img, info = load_sheet('aliens', src)
        if img is None:
            if not FLAT:
                warn('alien %s is not rendered: a stand-in' % aid)
            img, info = stand_in('alien', ALIEN_COLOR[ai], 8, 4, cell, anchor)
        if aid == 'mini':
            # the divider's children are the divider, smaller
            cw, ch = info['cell']
            small = Image.new('RGBA', img.size, (0, 0, 0, 0))
            k = 0.62
            for r in range(info['rows']):
                for c in range(info['cols']):
                    cel = img.crop((c * cw, r * ch, (c + 1) * cw, (r + 1) * ch))
                    cel = cel.resize((int(cw * k), int(ch * k)), Image.LANCZOS)
                    small.paste(cel, (c * cw + info['anchor'][0] - int(info['anchor'][0] * k),
                                      r * ch + info['anchor'][1] - int(info['anchor'][1] * k)))
            img = small
        fr, cols = cells(img, info)
        p.add('al_%s' % aid, PK_SHEET, 0, 0, encode_frames(fr, cols), len(fr))
        face = Image.fromarray(fr[5 % cols][0])
        icons.append((np.asarray(fit(face, 44)), 22, 22))
        if aid != 'mini':
            p.add('ai_%s' % aid, PK_SPRITE, 96, 96, sprite_bytes(fit(face, 96)))
    p.add('al_icons', PK_SHEET, 0, 0, encode_frames(icons, len(icons)), len(icons))

    print('the fort and the extras')
    for (group, name, kind, cols, rows, cell, anchor) in (
            ('fort', 'fort', 'fort', 3, 1, (320, 352), (160, 256)),
            ('extras', 'missile', 'alien', 16, 1, (48, 48), (24, 24)),
            ('extras', 'drone', 'alien', 8, 2, (64, 64), (32, 40))):
        img, info = load_sheet(group, name)
        if img is None:
            if not FLAT:
                warn('%s is not rendered: a stand-in' % name)
            img, info = stand_in(kind, '#38d8ff' if name == 'fort' else '#ffffff', cols, rows, cell, anchor)
        fr, c = cells(img, info)
        p.add(name, PK_SHEET, 0, 0, encode_frames(fr, c), len(fr))
        if name == 'fort':
            p.add('fort_big', PK_SPRITE, 320, 320, sprite_bytes(fit(Image.fromarray(fr[0][0]), 320)))
    print('the gallery')
    gallery(p)
    for n in ('core', 'star', 'star_off', 'lock', 'coin'):
        p.add('ic_' + n, PK_SPRITE, 56, 56, sprite_bytes(draw_icon(n)))
    t = title_picture()
    p.add('title', PK_RGB565, 720, 1280, rgb565(np.asarray(t)).tobytes())

    print('sounds')
    snd = {}
    try:
        import sounds
        snd = sounds.build()
        rate = getattr(sounds, 'RATE', 16000)
    except Exception as e:  # noqa
        warn('no sounds: %s' % e)
        rate = 16000
    for n in SOUNDS + MUSIC:
        if n in snd:
            p.add(('s_' + n) if n in SOUNDS else n, PK_PCM, 0, 0, mulaw(snd[n]), rate)
        elif snd:
            warn('sound %s is missing' % n)
    p.write(os.path.join(BUILD, 'bastion_p4.pak'))


# ---------------------------------------------------------------------------
# A world's pack
# ---------------------------------------------------------------------------

def flat_world(w):
    im = Image.new('RGB', (720, 1280), hexrgb(w.ground))
    d = ImageDraw.Draw(im)
    for path in w.paths:
        for (x, y) in path:
            cx, cy = 360 + x * 40, 1280 - y * 32
            d.ellipse([cx - 44, cy - 35, cx + 44, cy + 35], fill=hexrgb(w.road))
    for (x, y) in w.pads:
        cx, cy = 360 + x * 40, 1280 - y * 32
        d.polygon([(cx + 38 * np.cos(i * np.pi / 3), cy + 30 * np.sin(i * np.pi / 3)) for i in range(6)],
                  fill=(40, 46, 62), outline=hexrgb(w.accent))
    cx, cy = 360 + w.fort[0] * 40, 1280 - w.fort[1] * 32
    d.ellipse([cx - 108, cy - 86, cx + 108, cy + 86], fill=(50, 56, 74), outline=hexrgb(w.accent))
    return im


def world(w):
    p = Pack()
    p.add('def', PK_TEXT, 0, 0, w.def_text().encode('utf-8'))
    bgp = os.path.join(ASSETS, 'worlds', w.id + '_bg.png')
    dp = os.path.join(ASSETS, 'worlds', w.id + '_depth.npy')
    if not FLAT and os.path.exists(bgp):
        bg = Image.open(bgp).convert('RGB')
    else:
        if not FLAT:
            warn('world %s is not rendered: a flat one' % w.id)
        bg = flat_world(w)
    assert bg.size == (720, 1280), bg.size
    p.add('bg', PK_RGB565, 720, 1280, rgb565(np.asarray(bg)).tobytes())
    if not FLAT and os.path.exists(dp) and os.path.exists(bgp):
        z = np.load(dp)
        zu = np.clip(np.round((z + 16.0) * 1024.0), 0, 65535).astype('<u2')
        p.add('depth', PK_DEPTH, 720, 1280, zu.tobytes())
    th = bg.resize((300, 534), Image.LANCZOS)
    p.add('thumb', PK_RGB565, 300, 534, rgb565(np.asarray(th)).tobytes())
    p.write(os.path.join(BUILD, 'bastion_%s_p4.pak' % w.id))


FLAT = '--flat' in sys.argv


def main():
    a = sys.argv[1:]
    only = None
    if '--worlds' in a:
        only = a[a.index('--worlds') + 1].split(',')
    if '--common' in a or only is None:
        common()
    if '--common' not in a:
        for w in bsworlds.WORLDS:
            if only is None or w.id in only:
                world(w)
    if WARN:
        print('%d warnings' % len(WARN))


if __name__ == '__main__':
    main()
