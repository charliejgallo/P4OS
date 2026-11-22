#!/usr/bin/env python3
"""
HOCKEY - the packs for the card, from what Blender rendered (assets/) and
what is made here (fonts, sounds):

    python3 pack_p4.py                    # build/hockey_p4.pak (with the bar's table)
                                          # and one hockey_<table>_p4.pak each
    python3 pack_p4.py --tables neon      # only those tables' packs
    python3 pack_p4.py --common           # only the common pack

The layouts are in ../main/hk_pak.h (the pack) and ../main/hk_gfx.h (a
sheet's runs, a font). Neither the renders nor the packs are in git.
"""
import io
import json
import os
import struct
import subprocess
import sys
import wave

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

TOOLS = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(TOOLS)
ASSETS = os.path.join(APP, 'assets')
BUILD = os.path.join(APP, 'build')
LZ4 = os.path.join(BUILD, 'lz4blk')
FONT = os.path.join(TOOLS, 'fonts', 'Rubik-Medium.ttf')
PART = 7_500_000

PK_TEXT, PK_RGB565, PK_SPRITE, _, PK_SHEET, PK_JPEG, PK_PCM, PK_FONT = range(8)

# the tables: id, name (es, en, de), accent, each side's colour, the puck's, glow;
# the first travels in the common pack
TABLES = [
    ('bar', ('Clásica de bar', 'Classic bar', 'Kneipentisch'), 'E8443A', 'D3262B', '1E5BD8', 'F4B400', 0),
    ('neon', ('Neón', 'Neon', 'Neon'), 'FF2BD6', '22E6FF', 'FF2BD6', '7CFF4F', 1),
    ('hielo', ('Hielo', 'Ice rink', 'Eisbahn'), '7FB8E6', 'E0243F', '2A62C9', '202226', 0),
]
COMMON_TABLES = ['bar']

SOUNDS = ['hit', 'hit_soft', 'wall', 'post', 'goal', 'place', 'buzz', 'buzz_end', 'tap']
LOOPS = ['air']

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


# ---------------------------------------------------------------------------
# Sheets: frames as runs (mg_gfx.h)
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


def rgba(path):
    return np.asarray(Image.open(path).convert('RGBA'))


# ---------------------------------------------------------------------------
# Fonts
# ---------------------------------------------------------------------------

CHARS = ''.join(chr(c) for c in range(32, 127)) + '¡¿ÁÉÍÓÚÑÜÖÄáéíóúñüöäß°×·«»'


def font_entry(size):
    f = ImageFont.truetype(FONT, size)
    asc, desc = f.getmetrics()
    glyphs = []
    x = y = rowh = 0
    AW = 1024
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



# ---------------------------------------------------------------------------
# Sounds (tools/sounds.py writes them as WAV)
# ---------------------------------------------------------------------------

def sounds(p):
    d = os.path.join(ASSETS, 'sounds')
    for name in SOUNDS + LOOPS:
        path = os.path.join(d, name + '.wav')
        if not os.path.exists(path):
            warn('no sound ' + name)
            continue
        with wave.open(path) as w:
            rate = w.getframerate()
            x = np.frombuffer(w.readframes(w.getnframes()), '<i2').astype(np.float64) / 32768.0
        p.add('s_' + name, PK_PCM, 0, 0, mulaw(x), extra=rate)


# ---------------------------------------------------------------------------
# A table: its picture, its two ends over the pieces, the pieces
# ---------------------------------------------------------------------------

def table(p, tid, names, accent, c0, c1, puck, glow):
    d = os.path.join(ASSETS, 'tables', tid)
    pic = Image.open(os.path.join(d, 'bg.png')).convert('RGB')
    buf = io.BytesIO()
    pic.save(buf, 'JPEG', quality=90, optimize=True)
    p.add(tid + '.bg', PK_JPEG, 720, 1280, buf.getvalue(), compress=False)
    a = np.asarray(pic.convert('RGBA')).copy()
    # the ends over the pieces: the picture's own pixels where the end is
    for which in ('near', 'far'):
        m = rgba(os.path.join(d, which + '.png'))[..., 3].astype(np.float32) / 255.0
        m = np.where(m > 0.5, 1.0, m)
        o = a.copy()
        o[..., 3] = (m * 255).astype(np.uint8)
        p.add('%s.%s' % (tid, which), PK_SHEET, 720, 1280, encode_frames([(o, 0, 0)], 1), extra=1)
    info = json.load(open(os.path.join(d, 'pieces.json')))
    for name, entry in [('m%d_%s' % (s, k), 'm%d%s' % (s, k)) for s in (0, 1) for k in 'sml'] + [('puck', 'puck')]:
        im = rgba(os.path.join(d, name + '.png'))
        ax, ay = info[name]['anchor']
        p.add('%s.%s' % (tid, entry), PK_SHEET, im.shape[1], im.shape[0], encode_frames([(im, ax, ay)], 1), extra=1)
    txt = ('id = %s\nnombre = %s\nname_en = %s\nname_de = %s\nacento = %s\nlado0 = %s\nlado1 = %s\ndisco = %s\n'
           'brillo = %d\n' % (tid, names[0], names[1], names[2], accent, c0, c1, puck, glow))
    p.add(tid + '.def', PK_TEXT, 0, 0, txt.encode('utf-8'))


def common(path):
    p = Pack()
    p.add('f_small', PK_FONT, 0, 0, font_entry(22))
    p.add('f_score', PK_FONT, 0, 0, font_entry(34))
    p.add('f_big', PK_FONT, 0, 0, font_entry(84))
    p.add('f_clock', PK_FONT, 0, 0, font_entry(19))
    sounds(p)
    for t in TABLES:
        if t[0] in COMMON_TABLES:
            table(p, *t)
    return p.write(path)


def main():
    a = sys.argv[1:]
    only = None
    if '--tables' in a:
        only = a[a.index('--tables') + 1].split(',')
    sizes = {}
    if not only:
        sizes['hockey_p4.pak'] = common(os.path.join(BUILD, 'hockey_p4.pak'))
    if '--common' not in a:
        for t in TABLES:
            if t[0] in COMMON_TABLES or (only and t[0] not in only):
                continue
            if not os.path.exists(os.path.join(ASSETS, 'tables', t[0], 'bg.png')):
                warn('table %s not rendered' % t[0])
                continue
            p = Pack()
            table(p, *t)
            fn = 'hockey_%s_p4.pak' % t[0]
            sizes[fn] = p.write(os.path.join(BUILD, fn))
    if WARN:
        print('%d warnings' % len(WARN))


if __name__ == '__main__':
    main()
