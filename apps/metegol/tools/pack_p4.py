#!/usr/bin/env python3
"""
METEGOL - the packs for the card, from what Blender rendered (assets/) and
what is made here (fonts, sounds):

    python3 pack_p4.py                    # build/metegol_p4.pak (with two stadiums)
                                          # and one metegol_<stadium>_p4.pak each
    python3 pack_p4.py --stadiums rio     # only those stadiums' packs
    python3 pack_p4.py --common           # only the common pack

The layouts are in ../main/mg_pak.h (the pack), ../main/mg_gfx.h (a
sheet's runs, a font) and ../main/mg_kit.h (the figures' masks). A
stadium that was not rendered gets a plain backdrop and a warning, so the
game always starts. Neither the renders nor the packs are in git.
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
CROWD_STRIPS = 8         # the crowd's band in strips that move one at a time
SHADE_K = 0.66          # the render's grey to a figure's shade (160: the colour as it is)

PK_TEXT, PK_RGB565, PK_SPRITE, _, PK_SHEET, PK_JPEG, PK_PCM, PK_FONT, PK_FIGS = range(9)

# the stadiums: id, name (es, en, de), league (both, clubs, nations), accent;
# the first two travel in the common pack
STADIUMS = [
    ('laboca', ('Buenos Aires - La Boca', 'Buenos Aires - La Boca', 'Buenos Aires - La Boca'), 'ambas', 'FCD116'),
    ('nunez', ('Buenos Aires - Núñez', 'Buenos Aires - Núñez', 'Buenos Aires - Núñez'), 'ambas', 'E30613'),
    ('rio', ('Río de Janeiro', 'Rio de Janeiro', 'Rio de Janeiro'), 'selecciones', '2ECC71'),
    ('madrid', ('Madrid', 'Madrid', 'Madrid'), 'selecciones', 'F4F4F4'),
    ('munich', ('Múnich', 'Munich', 'München'), 'selecciones', 'E8443A'),
    ('londres', ('Londres', 'London', 'London'), 'selecciones', 'F4F4F4'),
    ('mexico', ('Ciudad de México', 'Mexico City', 'Mexiko-Stadt'), 'selecciones', '2ECC71'),
    ('tokio', ('Tokio', 'Tokyo', 'Tokio'), 'selecciones', 'F7A8C4'),
    ('avella_rojo', ('Avellaneda - el rojo', 'Avellaneda - the red one', 'Avellaneda - das rote'), 'clubes',
     'D7141A'),
    ('avella_celeste', ('Avellaneda - el celeste', 'Avellaneda - the sky-blue one',
                            'Avellaneda - das himmelblaue'), 'clubes', '7FBCE8'),
]
COMMON_STADIUMS = ['laboca', 'nunez']

SOUNDS = ['kick', 'kick_soft', 'wall', 'post', 'net', 'whistle', 'whistle_end', 'roar', 'serve', 'stop', 'tap', 'ooh']
LOOPS = ['crowd', 'music_menu']

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
# The figures' masks (mg_kit.h)
# ---------------------------------------------------------------------------

def figures(p):
    d = os.path.join(ASSETS, 'figures')
    info = json.load(open(os.path.join(d, 'figures.json')))
    a = rgba(os.path.join(d, 'figures.png'))
    part = np.round(np.asarray(Image.open(os.path.join(d, 'figures_part.png')).convert('RGB'))[..., 0].astype(np.float32)
                    * 16 / 255).astype(np.int32)
    uvw = np.asarray(Image.open(os.path.join(d, 'figures_uvw.png')).convert('RGB')).astype(np.int32)
    alpha = a[..., 3].astype(np.int32)
    # the parts and places come without antialiasing: an edge pixel the
    # light has but they miss takes its neighbour's
    for _ in range(3):
        miss = (alpha >= 8) & (part == 0)
        if not miss.any():
            break
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            sp = np.roll(np.roll(part, dy, 0), dx, 1)
            su = np.roll(np.roll(uvw, dy, 0), dx, 1)
            take = miss & (sp > 0)
            part[take] = sp[take]
            uvw[take] = su[take]
            miss &= ~take
    shade = np.clip(a[..., :3].astype(np.float32).mean(-1) * SHADE_K, 0, 255).astype(np.int32)
    cw, ch = info['cell']
    ax, ay = info['anchor']
    n, cols = info['frames'], info['cols']
    head = bytearray(b'MGFG' + struct.pack('<HH', n, cols))
    table = bytearray()
    body = bytearray()
    base = 8 + 12 * n
    for k in range(n):
        x0, y0 = (k % cols) * cw, (k // cols) * ch
        al = alpha[y0:y0 + ch, x0:x0 + cw]
        ys, xs = np.nonzero(al >= 8)
        bx0, bx1, by0, by1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
        sl = (slice(y0 + by0, y0 + by1), slice(x0 + bx0, x0 + bx1))
        px = np.stack([alpha[sl], shade[sl], part[sl], uvw[sl][..., 0], uvw[sl][..., 1], uvw[sl][..., 2]], -1)
        px = np.clip(px, 0, 255).astype(np.uint8)
        px[px[..., 0] < 8] = 0
        table += struct.pack('<hhHHI', int(bx0) - ax, int(by0) - ay, int(bx1 - bx0), int(by1 - by0), base + len(body))
        body += px.tobytes()
        while len(body) & 3:
            body += b'\0'
    blob = bytes(head + table + body)
    p.add('figs', PK_FIGS, cw, ch, blob, extra=n)


# ---------------------------------------------------------------------------
# Rods and ball
# ---------------------------------------------------------------------------

def rods(p):
    d = os.path.join(ASSETS, 'rods')
    info = json.load(open(os.path.join(d, 'rods.json')))
    sh = rgba(os.path.join(d, 'shaft.png'))
    ax, ay = info['shaft']['anchor']
    row = sh[ay]
    xs = np.nonzero(row[:, 3] >= 8)[0]
    x0, x1 = xs.min(), xs.max() + 1
    prof = row[x0:x1]
    # the profile: its pixels, then its alphas; x is where it starts from the axis
    p.add('rod_prof', PK_SPRITE, x1 - x0, 1, rgb565(prof[None, :, :3], dither=False).tobytes() +
          prof[:, 3].astype(np.uint8).tobytes(), x=int(x0) - ax)
    for name in ('bumper', 'handle_near', 'handle_far', 'end_near', 'end_far'):
        a = rgba(os.path.join(d, name + '.png'))
        ax, ay = info[name]['anchor']
        p.add('rod_' + name, PK_SHEET, a.shape[1], a.shape[0], encode_frames([(a, ax, ay)], 1), extra=1)


def ball(p):
    d = os.path.join(ASSETS, 'ball')
    info = json.load(open(os.path.join(d, 'ball.json')))
    a = rgba(os.path.join(d, 'ball.png'))
    cw, ch = info['cell']
    ax, ay = info['anchor']
    frames = [(a[:, k * cw:(k + 1) * cw], ax, ay) for k in range(info['frames'])]
    p.add('ball', PK_SHEET, cw, ch, encode_frames(frames, info['frames']), extra=info['frames'])


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
# A stadium: its picture with the table in it, the two bands, the crowd
# ---------------------------------------------------------------------------

def plain_backdrop(accent):
    """what a stadium not rendered yet looks like: dark stands, lights"""
    im = Image.new('RGB', (1280, 720), (18, 22, 30))
    d = ImageDraw.Draw(im)
    acc = tuple(int(accent[i:i + 2], 16) for i in (0, 2, 4))
    for y in range(0, 170, 6):
        k = 0.4 + 0.6 * y / 170
        d.rectangle([0, y, 1280, y + 5], fill=(int(30 * k + acc[0] * 0.15 * k), int(34 * k + acc[1] * 0.15 * k),
                                                  int(44 * k + acc[2] * 0.15 * k)))
    d.rectangle([0, 170, 1280, 720], fill=(38, 92, 46))
    for y in range(170, 720, 40):
        d.rectangle([0, y, 1280, y + 20], fill=(42, 100, 50))
    return im


def stadium(p, sid, names, league, accent):
    d = os.path.join(ASSETS, 'stadiums', sid)
    bgp = os.path.join(d, 'backdrop.png')
    tbl = os.path.join(d, 'table.png') if os.path.exists(os.path.join(d, 'table.png')) else \
        os.path.join(ASSETS, 'table', 'table.png')
    if os.path.exists(bgp):
        back = Image.open(bgp).convert('RGB')
    else:
        warn('stadium %s not rendered: a plain backdrop' % sid)
        back = plain_backdrop(accent)
    pic = back.convert('RGBA')
    pic.alpha_composite(Image.open(tbl).convert('RGBA'))
    pic = pic.convert('RGB')
    buf = io.BytesIO()
    pic.save(buf, 'JPEG', quality=88, optimize=True)
    p.add(sid + '.bg', PK_JPEG, 1280, 720, buf.getvalue(), compress=False)
    a = np.asarray(pic.convert('RGBA')).copy()
    # the bands over the rods: the picture's own pixels where the band is
    for which in ('near', 'far'):
        mp = os.path.join(d, which + '.png')
        if not os.path.exists(mp):
            mp = os.path.join(ASSETS, 'table', which + '.png')
        m = rgba(mp)[..., 3].astype(np.float32) / 255.0
        m = np.where(m > 0.5, 1.0, m)
        o = a.copy()
        o[..., 3] = (m * 255).astype(np.uint8)
        p.add('%s.%s' % (sid, which), PK_SHEET, 1280, 720, encode_frames([(o, 0, 0)], 1), extra=1)
    # the crowd: frames of the stands, already cut out by stadiums.py
    cj = os.path.join(d, 'crowd.json')
    crowd = ''
    if os.path.exists(cj):
        ci = json.load(open(cj))
        # in strips across the stands, each with its frames (strip * frames + k):
        # the app moves them one at a time, so a change redraws an eighth
        imgs = [rgba(os.path.join(d, 'crowd%d.png' % k)) for k in range(ci['frames'])]
        frames = []
        sw = ci['w'] // CROWD_STRIPS
        for st in range(CROWD_STRIPS):
            for k in range(ci['frames']):
                frames.append((imgs[k][:, st * sw:(st + 1) * sw], -(ci['x'] + st * sw), -ci['y']))
        p.add(sid + '.crowd', PK_SHEET, ci['w'], ci['h'], encode_frames(frames, ci['frames']), extra=len(frames),
              x=CROWD_STRIPS)
        crowd = 'crowd = %d,%d,%d,%d,%d\n' % (ci['x'], ci['y'], ci['w'], ci['h'], ci['frames'])
        for (fx, fy) in ci.get('flashes', []):
            crowd += 'flash = %d,%d\n' % (fx, fy)
    txt = ('id = %s\nnombre = %s\nname_en = %s\nname_de = %s\nliga = %s\nacento = %s\n%s' %
           (sid, names[0], names[1], names[2], league, accent, crowd))
    p.add(sid + '.def', PK_TEXT, 0, 0, txt.encode('utf-8'))


def common(path):
    p = Pack()
    figures(p)
    rods(p)
    ball(p)
    p.add('f_small', PK_FONT, 0, 0, font_entry(20))
    p.add('f_score', PK_FONT, 0, 0, font_entry(28))
    p.add('f_big', PK_FONT, 0, 0, font_entry(84))
    sounds(p)
    for s in STADIUMS:
        if s[0] in COMMON_STADIUMS:
            stadium(p, *s)
    return p.write(path)


def main():
    a = sys.argv[1:]
    only = None
    if '--stadiums' in a:
        only = a[a.index('--stadiums') + 1].split(',')
    sizes = {}
    if not only:
        sizes['metegol_p4.pak'] = common(os.path.join(BUILD, 'metegol_p4.pak'))
    if '--common' not in a:
        for s in STADIUMS:
            if s[0] in COMMON_STADIUMS or (only and s[0] not in only):
                continue
            p = Pack()
            stadium(p, *s)
            fn = 'metegol_%s_p4.pak' % s[0]
            sizes[fn] = p.write(os.path.join(BUILD, fn))
    if WARN:
        print('%d warnings' % len(WARN))


if __name__ == '__main__':
    main()
