#!/usr/bin/env python3
"""
SUDOKU - the pack, sudoku_p4.pak: every theme's art and the sounds, all of
it painted and synthesised here, by code (no pictures, no recordings):

    python3 pack_p4.py                      # build/sudoku_p4.pak
    python3 pack_p4.py --preview DIR        # and PNGs of everything, to look at
    python3 pack_p4.py --only neon,mar      # just those themes (for trying)

For each theme (the keys of sd_theme.c, in its order):

    bg_<key>   1280 x 1280 JPEG, the picture behind everything; upright the
               screen shows its middle 720 columns, lying down its middle
               720 rows, so what matters goes in the middle
    tx_<key>   720 x 720 JPEG, the board's material under the cells
    th_<key>   200 x 200 JPEG, the shop's thumbnail (the picture and a board)
    g9_<key>   the given digits 1-9 drawn in the theme's style, 60 x 60
               RGB565A8 each, one after the other (9 x 9 boards)
    g6_<key>   the same at 88 x 88 for 6 x 6 boards
    d<i>_<key> the sprites that move over the picture

and seven sound sets, s<set>_<name>, mu-law at 16 kHz.

The layout is in ../main/sd_pak.h. The digits are Rubik (OFL, fonts/), or
segments and dots drawn here. Neither the pack nor the previews go in git.
"""
import argparse
import io
import math
import os
import struct
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw, ImageFilter, ImageFont

TOOLS = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(TOOLS)
BUILD = os.path.join(APP, 'build')
LZ4 = os.path.join(BUILD, 'lz4blk')
FONT = os.path.join(TOOLS, 'fonts', 'Rubik-Medium.ttf')

BG = 1280
TEX = 720
THUMB = 200
G9 = 60
G6 = 88
RATE = 16000

T_TEXT, T_SPRITE, T_PCM, T_JPEG = 0, 2, 6, 8

# the order of SD_THEME in sd_theme.c, with what the art needs of the palette
THEMES = ['clasico', 'campo', 'madera', 'papiro', 'playa', 'nieve', 'piedra', 'mar', 'ciudad', 'metal', 'jardin',
          'desierto', 'neon', 'espacio', 'embrujada', 'aliens']
PAL = {
    'clasico': dict(board=0xF4F1EA, box=0x2B2B2B, given=0x1A1A1A),
    'campo': dict(board=0xF3F0DC, box=0x4E6B2E, given=0x2F4A1A),
    'madera': dict(board=0xC99A63, box=0x4A2C14, given=0x3A200C),
    'papiro': dict(board=0xE8D3A6, box=0x5A3F1E, given=0x3A2510),
    'playa': dict(board=0xF2E2C0, box=0x8A6A3A, given=0x135D7A),
    'nieve': dict(board=0xE8F4FB, box=0x3D6688, given=0x1E4E78),
    'piedra': dict(board=0x8A8780, box=0x2E2D2A, given=0x252220),
    'mar': dict(board=0x0C4A66, box=0xA6F0FF, given=0xFFFFFF),
    'ciudad': dict(board=0x161D2E, box=0x6B7A99, given=0xFFC23D),
    'metal': dict(board=0x8E949B, box=0x24272B, given=0x1C1F22),
    'jardin': dict(board=0xF4EEE2, box=0x3A2A2A, given=0x1E1A1A),
    'desierto': dict(board=0xE9C48E, box=0x6A3E1C, given=0x7A2E12),
    'neon': dict(board=0x0E0820, box=0x00E5FF, given=0xFF4FE1),
    'espacio': dict(board=0x0C0F22, box=0x8C7CFF, given=0xFFFFFF),
    'embrujada': dict(board=0x241A2E, box=0x9C86BA, given=0xB8FF9E),
    'aliens': dict(board=0x10261D, box=0x6BFF5C, given=0x6BFF5C),
}

SOUNDS = ['place', 'note', 'erase', 'error', 'unit', 'win', 'tap', 'hint']

BAYER = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], np.int32)


# ---------------------------------------------------------------------------
# small tools

def rgb(h):
    return np.array([(h >> 16) & 255, (h >> 8) & 255, h & 255], np.float32)


def mixc(a, b, t):
    return a * (1 - t) + b * t


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


def rgb565a8(rgba):
    """an RGBA float image -> LVGL's RGB565A8: the colours, then the alpha"""
    a = np.clip(rgba[..., 3], 0, 255).astype(np.uint8)
    c = rgb565(np.clip(rgba[..., :3], 0, 255), dither=False)
    return c.tobytes() + a.tobytes()


def jpeg(img, q):
    b = io.BytesIO()
    Image.fromarray(np.clip(img, 0, 255).astype(np.uint8)).save(b, 'JPEG', quality=q, optimize=True,
                                                                  progressive=False, subsampling=2)
    return b.getvalue()


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

    def add(self, name, typ, w, h, raw, extra=0, compress=True):
        assert len(name.encode()) < 24, name
        assert all(e['name'] != name for e in self.entries), name
        c = lz4(raw) if compress and len(raw) > 64 else raw
        if len(c) >= len(raw):
            c = raw
        self.entries.append(dict(name=name, type=typ, w=int(w), h=int(h), raw=len(raw), data=c, extra=int(extra)))

    def write(self, path):
        os.makedirs(os.path.dirname(path), exist_ok=True)
        n = len(self.entries)
        off = 8 + n * 52
        tab = b''
        for e in self.entries:
            nm = e['name'].encode().ljust(24, b'\0')
            tab += nm + struct.pack('<BBHHhhIIIIH', e['type'], 0, e['w'], e['h'], 0, 0, off, len(e['data']), e['raw'],
                                    e['extra'], 0)
            off += len(e['data'])
        blob = b'SDPK' + struct.pack('<HH', 1, n) + tab + b''.join(e['data'] for e in self.entries)
        with open(path, 'wb') as f:
            f.write(blob)
        return len(blob)


# ---------------------------------------------------------------------------
# painting

class Rng:
    def __init__(self, seed):
        self.r = np.random.default_rng(seed)

    def u(self, a=0.0, b=1.0):
        return float(self.r.uniform(a, b))

    def i(self, a, b):
        return int(self.r.integers(a, b))


def noise(h, w, scale, octaves=4, seed=0, persist=0.5):
    """fBm value noise in [0, 1]: random grids, upsampled smoothly"""
    r = np.random.default_rng(seed)
    out = np.zeros((h, w), np.float32)
    amp, tot = 1.0, 0.0
    s = scale
    for _ in range(octaves):
        gh, gw = max(2, int(h / s) + 2), max(2, int(w / s) + 2)
        g = r.random((gh, gw)).astype(np.float32)
        im = Image.fromarray((g * 255).astype(np.uint8)).resize((int(gw * s), int(gh * s)), Image.BICUBIC)
        a = np.asarray(im, np.float32)[:h, :w] / 255.0
        out += a * amp
        tot += amp
        amp *= persist
        s /= 2
        if s < 1:
            break
    return out / tot


def vgrad(h, w, stops):
    """vertical gradient: stops = [(t, 0xRRGGBB), ...]"""
    t = np.linspace(0, 1, h, dtype=np.float32)[:, None]
    out = np.zeros((h, w, 3), np.float32)
    ts = [s[0] for s in stops]
    cs = [rgb(s[1]) for s in stops]
    for ch in range(3):
        out[..., ch] = np.interp(t, ts, [c[ch] for c in cs])
    return out


def put(img, color, alpha):
    """paint 'color' (an RGB or an image) with 'alpha' (HxW, 0..1)"""
    a = np.clip(alpha, 0, 1)[..., None]
    c = color if np.ndim(color) == 3 else np.asarray(color, np.float32)[None, None, :]
    img[...] = img * (1 - a) + c * a


def add(img, color, alpha):
    a = np.clip(alpha, 0, 1)[..., None]
    img[...] = np.minimum(255, img + np.asarray(color, np.float32)[None, None, :] * a)


def mask(w, h, draw, ss=2):
    im = Image.new('L', (w * ss, h * ss), 0)
    draw(ImageDraw.Draw(im), ss)
    if ss > 1:
        im = im.resize((w, h), Image.LANCZOS)
    return np.asarray(im, np.float32) / 255.0


def blur(m, r):
    im = Image.fromarray(np.clip(m * 255, 0, 255).astype(np.uint8))
    return np.asarray(im.filter(ImageFilter.GaussianBlur(r)), np.float32) / 255.0


def blur_rgb(img, r):
    im = Image.fromarray(np.clip(img, 0, 255).astype(np.uint8))
    return np.asarray(im.filter(ImageFilter.GaussianBlur(r)), np.float32)


def disc(h, w, cx, cy, r, soft=1.5):
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    d = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2)
    return np.clip((r - d) / soft + 0.5, 0, 1)


def glow(h, w, cx, cy, r):
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    d = np.sqrt((xx - cx) ** 2 + (yy - cy) ** 2) / r
    return np.exp(-d * d)


def vignette(img, k=0.45):
    h, w = img.shape[:2]
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    d = np.sqrt(((xx - w / 2) / (w / 2)) ** 2 + ((yy - h / 2) / (h / 2)) ** 2)
    img *= (1 - k * np.clip(d - 0.4, 0, 1.2) / 1.2)[..., None]


def ridge(w, base, amp, freqs, seed, h):
    """a skyline of hills: y for every x"""
    r = Rng(seed)
    x = np.arange(w, dtype=np.float32)
    y = np.full(w, base, np.float32)
    for f in freqs:
        y += amp * r.u(0.4, 1.0) * np.sin(x / w * math.pi * 2 * f + r.u(0, 6.28)) / len(freqs)
    return np.clip(y, 0, h)


def under(h, w, ys):
    yy = np.arange(h, dtype=np.float32)[:, None]
    return np.clip(yy - ys[None, :] + 0.5, 0, 1)


def stars(img, n, seed, ymax=None, color=0xFFFFFF, big=0.05):
    h, w = img.shape[:2]
    r = Rng(seed)
    ymax = ymax or h
    for _ in range(n):
        x, y = r.u(0, w), r.u(0, ymax)
        s = r.u(0.6, 1.6) if r.u() > big else r.u(2.0, 3.2)
        a = r.u(0.4, 1.0)
        x0, y0 = int(x) - 6, int(y) - 6
        if x0 < 0 or y0 < 0 or x0 + 13 > w or y0 + 13 > h:
            continue
        m = disc(13, 13, x - x0, y - y0, s, 1.0) * a + glow(13, 13, x - x0, y - y0, s * 2.5) * a * 0.35
        sub = img[y0:y0 + 13, x0:x0 + 13]
        add(sub, rgb(color), m)


def clouds(img, n, seed, y0, y1, color, alpha=0.8, size=1.0):
    h, w = img.shape[:2]
    r = Rng(seed)
    m = np.zeros((h, w), np.float32)
    for _ in range(n):
        cx, cy = r.u(-50, w + 50), r.u(y0, y1)
        sw = r.u(140, 300) * size

        def d(dr, ss, cx=cx, cy=cy, sw=sw):
            for k in range(7):
                ox = (k - 3) * sw / 6 + r.u(-20, 20)
                rr = sw / 5 * (1.2 - abs(k - 3) / 4) + r.u(0, 15)
                dr.ellipse([(cx + ox - rr) * ss, (cy - rr * 0.8) * ss, (cx + ox + rr) * ss, (cy + rr * 0.6) * ss],
                           fill=255)
        m = np.maximum(m, mask(w, h, d, 1))
    m = blur(m, 6 * size)
    put(img, rgb(color), m * alpha)


def tree_branches(dr, ss, x, y, ang, length, width, depth, r, tips):
    if depth == 0 or length < 4:
        tips.append((x, y))
        return
    x2 = x + math.cos(ang) * length
    y2 = y - math.sin(ang) * length
    dr.line([x * ss, y * ss, x2 * ss, y2 * ss], fill=255, width=max(1, int(width * ss)))
    for k in range(2 if depth > 1 else 3):
        a2 = ang + r.u(-0.7, 0.7)
        tree_branches(dr, ss, x2, y2, a2, length * r.u(0.6, 0.8), width * 0.68, depth - 1, r, tips)


# ---------------------------------------------------------------------------
# the pictures, 1280 x 1280

def bg_clasico():
    img = vgrad(BG, BG, [(0, 0x1C2433), (0.55, 0x121822), (1, 0x0A0E15)])
    n = noise(BG, BG, 220, 4, 11)
    img += (n[..., None] - 0.5) * 22
    # a faint grid of 3x3 boxes, very large, like squared paper seen up close
    yy, xx = np.mgrid[0:BG, 0:BG]
    g = ((xx % 160) < 2) | ((yy % 160) < 2)
    put(img, rgb(0x3A4A66), g.astype(np.float32) * 0.25)
    g2 = ((xx % 480) < 4) | ((yy % 480) < 4)
    put(img, rgb(0x4A5E80), g2.astype(np.float32) * 0.25)
    add(img, rgb(0x2A4A80), glow(BG, BG, BG * 0.5, BG * 0.3, 520) * 0.35)
    vignette(img, 0.5)
    return img


def bg_campo():
    img = vgrad(BG, BG, [(0, 0x4FA3E0), (0.45, 0x9AD2F2), (0.62, 0xE8F4F2), (1, 0xE8F4F2)])
    add(img, rgb(0xFFF0B0), glow(BG, BG, 980, 230, 260) * 0.9)
    img[...] = np.maximum(img, disc(BG, BG, 980, 230, 70, 3)[..., None] * rgb(0xFFF6C8))
    clouds(img, 9, 3, 120, 520, 0xFFFFFF, 0.9)
    layers = [(720, 70, 0x9CC77A, 0x6E9F4E, 1), (820, 90, 0x7CB356, 0x4F8A35, 2), (960, 110, 0x5E9A3C, 0x3C6E25, 3),
              (1110, 90, 0x4A8530, 0x2E5A1C, 4)]
    for base, amp, c0, c1, s in layers:
        ys = ridge(BG, base, amp, [1, 2.3, 4.1], s, BG)
        m = under(BG, BG, ys)
        tone = vgrad(BG, BG, [(0, c0), (1, c1)])
        n = noise(BG, BG, 60, 3, 40 + s)
        tone *= (0.88 + n * 0.24)[..., None]
        put(img, tone, m)
    # rows of a ploughed field at the bottom
    yy, xx = np.mgrid[0:BG, 0:BG].astype(np.float32)
    rows = (np.sin((xx - BG / 2) / (1 + (yy - 1000) / 140).clip(0.3) / 9.0) > 0.6) & (yy > 1130)
    put(img, rgb(0x3A6A20), rows.astype(np.float32) * 0.35)
    # a fence
    def fence(dr, ss):
        for x in range(0, BG, 64):
            dr.rectangle([x * ss, 1050 * ss, (x + 10) * ss, 1130 * ss], fill=255)
        dr.rectangle([0, 1065 * ss, BG * ss, 1075 * ss], fill=255)
        dr.rectangle([0, 1095 * ss, BG * ss, 1105 * ss], fill=255)
    put(img, rgb(0x8A6A44), mask(BG, BG, fence) * 0.9)
    return img


def wood_grain(h, w, base, dark, seed, vertical=False, scale=1.0):
    n = noise(h, w, 90 * scale, 3, seed)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    a = xx if vertical else yy
    rings = np.sin((a / (5 * scale) + n * 6)) * 0.5 + 0.5
    fine = noise(h, w, 4 * scale, 2, seed + 1)
    t = np.clip(rings ** 3 * 0.6 + fine * 0.3, 0, 1)
    return mixc(rgb(base)[None, None, :], rgb(dark)[None, None, :], t[..., None])


def bg_madera():
    img = np.zeros((BG, BG, 3), np.float32)
    pw = 160
    for k in range(BG // pw + 1):
        x0 = k * pw
        cols = [0x8A5A30, 0x7A4E28, 0x946238, 0x6E4422]
        plank = wood_grain(BG, pw, cols[k % 4], 0x3E2410, 100 + k, vertical=True, scale=1.4)
        img[:, x0:x0 + pw] = plank[:, :min(pw, BG - x0)]
        img[:, x0:x0 + 3] *= 0.35
        # nails
        for y in (140, 640, 1140):
            m = disc(BG, BG, x0 + pw / 2, y, 7, 1.5)
            put(img, rgb(0x2A1A10), m)
            put(img, rgb(0x6A5A50), disc(BG, BG, x0 + pw / 2 - 2, y - 2, 3, 1.2))
    vignette(img, 0.55)
    return img


def bg_papiro():
    img = vgrad(BG, BG, [(0, 0x4A3420), (1, 0x2A1C10)])
    n = noise(BG, BG, 160, 5, 7)
    img *= (0.8 + n * 0.4)[..., None]
    # a sheet of papyrus under the board, its edges torn
    m = np.zeros((BG, BG), np.float32)
    e = noise(BG, BG, 18, 3, 8)
    yy, xx = np.mgrid[0:BG, 0:BG].astype(np.float32)
    inside = (np.abs(xx - BG / 2) < 560 + e * 60) & (np.abs(yy - BG / 2) < 600 + e * 60)
    m = blur(inside.astype(np.float32), 2)
    sheet = tex_papiro(BG)
    put(img, blur_rgb(img, 0) * 0.5, blur(inside.astype(np.float32), 18) * 0.6)
    put(img, sheet, m)
    # ink: a few lines of made-up script, and stains
    r = Rng(9)

    def script(dr, ss):
        for row in range(14):
            y = 110 + row * 80
            x = 140 + r.u(0, 40)
            while x < BG - 160:
                wdt = r.u(14, 40)
                if r.u() < 0.8:
                    dr.arc([x * ss, (y - 10) * ss, (x + wdt) * ss, (y + 14) * ss], r.u(0, 180), r.u(180, 360),
                           fill=255, width=3 * ss)
                    dr.line([x * ss, (y + 10) * ss, (x + wdt) * ss, (y + r.u(-8, 8)) * ss], fill=255, width=2 * ss)
                x += wdt + r.u(8, 30)
    put(img, rgb(0x4A2E14), mask(BG, BG, script) * 0.28 * m)
    for _ in range(6):
        put(img, img * 0.82, glow(BG, BG, r.u(100, 1180), r.u(100, 1180), r.u(40, 120)) * 0.7 * m)
    vignette(img, 0.4)
    return img


def bg_playa():
    img = vgrad(BG, BG, [(0, 0x2E8FD8), (0.42, 0x8ED4F5), (0.52, 0xD8F2FA), (1, 0xD8F2FA)])
    add(img, rgb(0xFFF3C0), glow(BG, BG, 330, 260, 240) * 0.9)
    img[...] = np.maximum(img, disc(BG, BG, 330, 260, 80, 3)[..., None] * rgb(0xFFFBE0))
    clouds(img, 5, 21, 120, 420, 0xFFFFFF, 0.85, 0.9)
    horizon = 690
    yy, xx = np.mgrid[0:BG, 0:BG].astype(np.float32)
    sea = vgrad(BG, BG, [(0, 0x1C7FB8), (0.54, 0x1C7FB8), (0.62, 0x2BB3C8), (0.8, 0x6FD8D2), (1, 0x6FD8D2)])
    m = (yy >= horizon).astype(np.float32)
    put(img, sea, m)
    # wave lines on the sea
    n = noise(BG, BG, 40, 3, 22)
    lines = (np.sin(yy / 6 + n * 12) > 0.93) & (yy > horizon) & (yy < 1000)
    put(img, rgb(0xEAFBFF), lines.astype(np.float32) * 0.5)
    # the beach, its wet edge, foam
    ys = ridge(BG, 1010, 50, [0.8, 1.7, 3.2], 23, BG)
    sand = vgrad(BG, BG, [(0, 0xF2DCA8), (1, 0xE0C080)])
    sand *= (0.9 + noise(BG, BG, 3, 2, 24) * 0.2)[..., None]
    wet = under(BG, BG, ys - 18) - under(BG, BG, ys)
    put(img, rgb(0xFFFFFF), np.clip(wet, 0, 1) * 0.85)
    put(img, sand, under(BG, BG, ys))
    put(img, rgb(0xC8A46A), np.clip(under(BG, BG, ys) - under(BG, BG, ys + 26), 0, 1) * 0.35)
    # a palm, left
    def palm(dr, ss):
        pts = [(60 + 60 * math.sin(t / 30), 1200 - t * 7.2) for t in range(0, 90)]
        for k in range(len(pts) - 1):
            dr.line([pts[k][0] * ss, pts[k][1] * ss, pts[k + 1][0] * ss, pts[k + 1][1] * ss], fill=255,
                    width=int((30 - k * 0.2) * ss))
        top = pts[-1]
        for a in range(0, 360, 40):
            ar = math.radians(a + 10)
            for s in range(1, 12):
                x = top[0] + math.cos(ar) * s * 22
                y = top[1] + math.sin(ar) * s * 14 + s * s * 1.6
                rr = 18 - s
                dr.ellipse([(x - rr) * ss, (y - rr / 2) * ss, (x + rr) * ss, (y + rr / 2) * ss], fill=255)
    put(img, rgb(0x2E4A28), mask(BG, BG, palm) * 0.92)
    return img


def bg_nieve():
    img = vgrad(BG, BG, [(0, 0x0A1A36), (0.5, 0x23507E), (0.7, 0x5C8DB8), (1, 0x8FB8DA)])
    stars(img, 220, 30, 700)
    add(img, rgb(0xDDEEFF), glow(BG, BG, 900, 220, 160) * 0.5)
    img[...] = np.maximum(img, disc(BG, BG, 900, 220, 52, 2)[..., None] * rgb(0xF2F8FF))
    for base, amp, c, s in [(760, 80, 0xA9C6E0, 31), (880, 90, 0xC9DDF0, 32), (1040, 80, 0xE8F2FB, 33)]:
        ys = ridge(BG, base, amp, [1, 2.1, 3.7], s, BG)
        sh = vgrad(BG, BG, [(0, c), (1, 0xFFFFFF)])
        put(img, sh, under(BG, BG, ys))
        # pines along the ridge
        r = Rng(s)

        def pines(dr, ss, ys=ys, r=r, base=base):
            for _ in range(int(16 - (base - 760) / 40)):
                x = r.u(0, BG)
                y = ys[int(np.clip(x, 0, BG - 1))] + 6
                hgt = r.u(60, 140) * (0.6 + (base - 760) / 500)
                for k in range(4):
                    t = k / 4
                    wdt = hgt * 0.42 * (1 - t * 0.7)
                    dr.polygon([((x - wdt) * ss, (y - hgt * t * 0.9) * ss), ((x + wdt) * ss, (y - hgt * t * 0.9) * ss),
                                (x * ss, (y - hgt * (t + 0.42)) * ss)], fill=255)
        pm = mask(BG, BG, pines)
        put(img, rgb(0x1E3A4A), pm)
        put(img, rgb(0xF2F8FF), np.clip(pm - np.roll(pm, 4, axis=0), 0, 1) * 0.9)
    return img


def bg_piedra():
    img = np.zeros((BG, BG, 3), np.float32)
    r = Rng(40)
    n = noise(BG, BG, 30, 4, 41)
    rowh = 120
    for row in range(BG // rowh + 1):
        y0 = row * rowh
        x = -r.u(0, 200)
        while x < BG:
            wdt = r.u(160, 300)
            c = rgb(0x6A6862) * r.u(0.75, 1.15)
            x0i, x1i = int(max(0, x + 4)), int(min(BG, x + wdt - 4))
            y0i, y1i = int(y0 + 4), int(min(BG, y0 + rowh - 4))
            if x1i > x0i and y1i > y0i:
                blk = c[None, None, :] * (0.75 + n[y0i:y1i, x0i:x1i, None] * 0.5)
                # bevel: light top-left, dark bottom-right
                hh, ww = y1i - y0i, x1i - x0i
                yy, xx = np.mgrid[0:hh, 0:ww].astype(np.float32)
                edge = np.minimum(np.minimum(xx, yy), np.minimum(ww - 1 - xx, hh - 1 - yy))
                lit = np.clip(1 - edge / 10, 0, 1)
                side = np.where((xx < yy * ww / hh), 1.0, -1.0) * np.where(xx + yy < (ww + hh) / 2, 1, -1)
                blk *= (1 + lit * 0.22 * np.where((xx < 10) | (yy < 10), 1, -1))[..., None]
                img[y0i:y1i, x0i:x1i] = blk
                del side
            x += wdt
    mortar = (img.sum(axis=2) == 0)
    img[mortar] = rgb(0x2A2826) * 1.0
    img *= (0.9 + noise(BG, BG, 4, 2, 42) * 0.2)[..., None]
    # moss in the cracks, and a lantern's light
    moss = noise(BG, BG, 80, 3, 43) > 0.62
    put(img, rgb(0x56703A), (moss & mortar).astype(np.float32) * 0.8)
    add(img, rgb(0xFFB060), glow(BG, BG, 200, 180, 300) * 0.35)
    add(img, rgb(0xFFB060), glow(BG, BG, 1080, 1100, 300) * 0.3)
    vignette(img, 0.6)
    return img


def bg_mar():
    img = vgrad(BG, BG, [(0, 0x2BA8C8), (0.25, 0x13719C), (0.7, 0x073656), (1, 0x041C30)])
    yy, xx = np.mgrid[0:BG, 0:BG].astype(np.float32)
    # light from above, in rays
    for k in range(7):
        x0 = 120 + k * 170
        a = (np.abs((xx - x0) - (yy * 0.35)) < 30 + k * 5).astype(np.float32)
        a *= np.clip(1 - yy / 900, 0, 1)
        add(img, rgb(0x9AE8FF), blur(a, 18) * 0.22)
    c = noise(BG, BG, 50, 3, 51)
    caus = np.clip(1 - np.abs(c - 0.5) * 22, 0, 1) * np.clip(1 - yy / 420, 0, 1)
    add(img, rgb(0xBFF6FF), caus * 0.2)
    # the floor: sand, rocks, weed
    ys = ridge(BG, 1130, 50, [0.7, 1.9, 3.3], 52, BG)
    floor = vgrad(BG, BG, [(0, 0xC8B080), (1, 0x8A7450)]) * (0.85 + noise(BG, BG, 5, 2, 53) * 0.3)[..., None]
    put(img, floor * 0.6, under(BG, BG, ys))
    r = Rng(54)

    def weed(dr, ss):
        for _ in range(26):
            x = r.u(0, BG)
            y = ys[int(np.clip(x, 0, BG - 1))] + 10
            hgt = r.u(120, 380)
            ph = r.u(0, 6)
            pts = [(x + 18 * math.sin(t / 25 + ph), y - t) for t in range(0, int(hgt), 4)]
            for k in range(len(pts) - 1):
                wd = max(2, 12 * (1 - k / len(pts)))
                dr.line([pts[k][0] * ss, pts[k][1] * ss, pts[k + 1][0] * ss, pts[k + 1][1] * ss], fill=255,
                        width=int(wd * ss))
    put(img, rgb(0x1E6E4A), mask(BG, BG, weed) * 0.85)

    def rocks(dr, ss):
        for _ in range(7):
            x = r.u(0, BG)
            y = ys[int(np.clip(x, 0, BG - 1))] + 20
            wdt = r.u(60, 160)
            dr.ellipse([(x - wdt) * ss, (y - wdt * 0.6) * ss, (x + wdt) * ss, (y + wdt * 0.5) * ss], fill=255)
    put(img, rgb(0x2A3E48), mask(BG, BG, rocks) * 0.95)
    return img


def bg_ciudad():
    img = vgrad(BG, BG, [(0, 0x070A1A), (0.5, 0x1A1E44), (0.72, 0x4A2E5E), (1, 0x2A1A30)])
    stars(img, 160, 60, 600)
    add(img, rgb(0xFFE8C0), glow(BG, BG, 960, 240, 120) * 0.5)
    img[...] = np.maximum(img, disc(BG, BG, 960, 240, 46, 2)[..., None] * rgb(0xFFF4DC))
    add(img, rgb(0xFF7A50), glow(BG, BG, BG / 2, 980, 600) * 0.25)
    r = Rng(61)
    for layer, (base, hmin, hmax, col, lit) in enumerate([(980, 200, 420, 0x22264A, 0.12), (1060, 160, 520, 0x151832, 0.22),
                                                          (1180, 120, 380, 0x0A0C1C, 0.32)]):
        x = -r.u(0, 60)
        while x < BG:
            wdt = r.u(70, 170)
            hgt = r.u(hmin, hmax)
            top = base - hgt
            x0, x1 = int(max(0, x)), int(min(BG, x + wdt))
            img[int(top):BG, x0:x1] = rgb(col)
            if r.u() < 0.3:     # an antenna
                ax = int((x0 + x1) / 2)
                img[int(top - 50):int(top), ax:ax + 3] = rgb(col)
                img[int(top - 54):int(top - 48), ax - 2:ax + 5] = rgb(0xFF4040)
            # windows
            for wy in range(int(top) + 14, BG - 10, 26):
                for wx in range(x0 + 10, x1 - 14, 22):
                    if r.u() < lit:
                        c = [0xFFD27A, 0xFFE9A8, 0x9AD8FF][r.i(0, 3)]
                        img[wy:wy + 12, wx:wx + 9] = rgb(c) * r.u(0.6, 1.0)
            x += wdt + r.u(4, 18)
    return img


def bg_metal():
    yy, xx = np.mgrid[0:BG, 0:BG].astype(np.float32)
    n = noise(BG, 4, 1, 1, 70)
    streak = np.asarray(Image.fromarray((np.random.default_rng(71).random((BG, 64)) * 255).astype(np.uint8))
                        .resize((BG, BG), Image.BILINEAR), np.float32) / 255.0
    img = vgrad(BG, BG, [(0, 0x5A6068), (0.5, 0x40454C), (1, 0x2A2E33)])
    img *= (0.85 + streak * 0.25)[..., None]
    del n
    # plates with seams and rivets
    pw = 320
    for gx in range(0, BG + 1, pw):
        img[:, max(0, gx - 2):gx + 2] *= 0.4
        img[:, gx + 2:gx + 4] *= 1.3
    for gy in range(0, BG + 1, pw):
        img[max(0, gy - 2):gy + 2, :] *= 0.4
        img[gy + 2:gy + 4, :] *= 1.3
    for gx in range(0, BG, pw):
        for gy in range(0, BG, pw):
            for dx, dy in ((22, 22), (pw - 22, 22), (22, pw - 22), (pw - 22, pw - 22)):
                cx, cy = gx + dx, gy + dy
                y0, x0 = max(0, cy - 14), max(0, cx - 14)
                sub = img[y0:cy + 14, x0:cx + 14]
                hh, ww = sub.shape[:2]
                put(sub, rgb(0x2A2D31), disc(hh, ww, cx - x0, cy - y0, 9, 1.5))
                put(sub, rgb(0xC8CED6), disc(hh, ww, cx - x0 - 2, cy - y0 - 2, 6, 1.5))
                put(sub, rgb(0x8A9098), disc(hh, ww, cx - x0, cy - y0, 4, 1.5))
    # a diagonal sheen
    sheen = np.exp(-((xx - yy - 200) / 260) ** 2)
    add(img, rgb(0xFFFFFF), sheen * 0.12)
    add(img, rgb(0xFF8A00), glow(BG, BG, BG, BG, 500) * 0.18)
    return img


def bg_jardin():
    img = vgrad(BG, BG, [(0, 0x3A2454), (0.35, 0xB0587A), (0.55, 0xF2A27A), (0.62, 0xF6C79A), (1, 0xF6C79A)])
    add(img, rgb(0xFFE0C0), glow(BG, BG, 640, 560, 260) * 0.6)
    img[...] = np.maximum(img, disc(BG, BG, 640, 560, 140, 3)[..., None] * rgb(0xFFE6CC))
    # the mountain, far away, with its snow
    def mount(dr, ss):
        dr.polygon([(-200 * ss, 860 * ss), (640 * ss, 430 * ss), (1480 * ss, 860 * ss)], fill=255)
    mm = mask(BG, BG, mount)
    put(img, rgb(0x6A4A78), mm * 0.85)

    def snow(dr, ss):
        dr.polygon([(520 * ss, 492 * ss), (640 * ss, 430 * ss), (760 * ss, 492 * ss), (720 * ss, 520 * ss),
                    (680 * ss, 500 * ss), (640 * ss, 530 * ss), (600 * ss, 502 * ss), (560 * ss, 524 * ss)], fill=255)
    put(img, rgb(0xF4ECF2), mask(BG, BG, snow) * 0.9)
    # the pond, with the sky in it
    yy = np.arange(BG, dtype=np.float32)[:, None]
    pond = (yy > 860).astype(np.float32) * np.ones((1, BG), np.float32)
    refl = img[::-1].copy() * 0.7 + rgb(0x203050) * 0.3
    n = noise(BG, BG, 12, 2, 81)
    refl *= (0.9 + n * 0.2)[..., None]
    put(img, refl, pond)
    put(img, rgb(0x2A1E30), np.clip(1 - np.abs(yy - 862) / 6, 0, 1) * np.ones((1, BG)) * 0.8)
    # cherry branches from the corners, in bloom
    r = Rng(82)
    tips = []

    def branches(dr, ss):
        tree_branches(dr, ss, -20, 260, -0.15, 220, 26, 6, r, tips)
        tree_branches(dr, ss, BG + 20, 160, math.pi + 0.25, 240, 28, 6, r, tips)
        tree_branches(dr, ss, BG + 20, 1220, math.pi - 0.4, 200, 22, 5, r, tips)
    bm = mask(BG, BG, branches)
    put(img, rgb(0x2A1A22), bm)

    def blossoms(dr, ss):
        for (x, y) in tips:
            for _ in range(5):
                bx, by = x + r.u(-26, 26), y + r.u(-26, 26)
                rr = r.u(6, 13)
                dr.ellipse([(bx - rr) * ss, (by - rr) * ss, (bx + rr) * ss, (by + rr) * ss], fill=255)
    put(img, rgb(0xF6B8CA), mask(BG, BG, blossoms) * 0.95)
    return img


def bg_desierto():
    img = vgrad(BG, BG, [(0, 0xE8742E), (0.35, 0xF2A65A), (0.6, 0xFAD58A), (1, 0xFAD58A)])
    add(img, rgb(0xFFF2C0), glow(BG, BG, 860, 400, 300) * 0.8)
    img[...] = np.maximum(img, disc(BG, BG, 860, 400, 110, 3)[..., None] * rgb(0xFFF4D0))
    for base, amp, c0, c1, s in [(760, 90, 0xE8A866, 0xC88848, 91), (880, 120, 0xE0A060, 0xB87840, 92),
                                 (1040, 140, 0xD89458, 0xA86A38, 93), (1180, 100, 0xC98A4E, 0x9A5E30, 94)]:
        ys = ridge(BG, base, amp, [0.7, 1.6], s, BG)
        m = under(BG, BG, ys)
        tone = vgrad(BG, BG, [(0, c0), (1, c1)])
        # the lit side of each dune
        slope = np.gradient(ys)
        tone *= (1 + np.clip(-slope, -1, 1)[None, :, None] * 0.12)
        tone *= (0.93 + noise(BG, BG, 3, 2, s + 10) * 0.14)[..., None]
        put(img, tone, m)
    r = Rng(95)

    def cacti(dr, ss):
        for (x, y, h) in ((150, 1150, 260), (1120, 1060, 200), (980, 1220, 150)):
            w = h * 0.16
            dr.rounded_rectangle([(x - w) * ss, (y - h) * ss, (x + w) * ss, y * ss], radius=w * ss, fill=255)
            for side in (-1, 1):
                ay = y - h * r.u(0.35, 0.6)
                ax = x + side * w * 2.6
                dr.rounded_rectangle([min(x, ax) * ss, (ay - w * 0.7) * ss, max(x, ax) * ss, (ay + w * 0.7) * ss],
                                     radius=w * 0.7 * ss, fill=255)
                dr.rounded_rectangle([(ax - w * 0.7) * ss, (ay - h * 0.35) * ss, (ax + w * 0.7) * ss, ay * ss],
                                     radius=w * 0.7 * ss, fill=255)
    put(img, rgb(0x4A5A2A), mask(BG, BG, cacti) * 0.95)
    return img


def bg_neon():
    img = vgrad(BG, BG, [(0, 0x05010E), (0.4, 0x1A0634), (0.62, 0x4A0E5A), (0.66, 0x0A0418), (1, 0x05010E)])
    stars(img, 200, 120, 760, 0xFFD8FF)
    yy, xx = np.mgrid[0:BG, 0:BG].astype(np.float32)
    # the sun, striped
    sun = disc(BG, BG, 640, 640, 230, 2) * (yy < 820)
    stripes = ((yy > 600) & (np.sin((yy - 600) / 9.0) > (1 - (yy - 600) / 260))).astype(np.float32)
    sg = vgrad(BG, BG, [(0, 0xFFE14D), (0.45, 0xFFE14D), (0.62, 0xFF2BD6), (1, 0xFF2BD6)])
    add(img, rgb(0xFF2BD6), glow(BG, BG, 640, 640, 380) * 0.45)
    put(img, sg, sun * (1 - stripes))
    # mountains in outline
    ys = ridge(BG, 800, 140, [1.3, 2.7, 5.1], 101, BG)
    m = under(BG, BG, ys)
    put(img, rgb(0x0E0424), m)
    edge = np.clip(m - under(BG, BG, ys + 3), 0, 1)
    add(img, rgb(0x00E5FF), blur(edge, 3) * 1.5)
    put(img, rgb(0x7AF6FF), edge)
    # the floor: a grid in perspective
    horizon = 840
    floor = yy > horizon
    put(img, rgb(0x08021A), floor.astype(np.float32))
    d = (yy - horizon).clip(1)
    zx = (xx - 640) / d * 90
    zz = 9000 / d
    gl = ((np.abs(zx - np.round(zx / 60) * 60) < 60 * 0.012 * d / 30 + 0.8) | (np.abs(zz - np.round(zz / 40) * 40) < 1.6)) & floor
    g = gl.astype(np.float32) * np.clip((yy - horizon) / 300, 0.15, 1)
    add(img, rgb(0xFF2BD6), blur(g, 3) * 0.9)
    put(img, rgb(0xFF7AE8), g * 0.8)
    return img


def bg_espacio():
    img = np.zeros((BG, BG, 3), np.float32) + rgb(0x04050C)
    n1 = noise(BG, BG, 260, 6, 111)
    n2 = noise(BG, BG, 200, 6, 112)
    neb = np.clip((n1 - 0.45) * 3, 0, 1) ** 1.5
    put(img, rgb(0x6A2A9A), neb * 0.7)
    neb2 = np.clip((n2 - 0.5) * 3, 0, 1) ** 1.5
    put(img, rgb(0x1A5AA8), neb2 * 0.6)
    add(img, rgb(0xFF6AA8), np.clip((n1 * n2 - 0.3) * 4, 0, 1) * 0.35)
    stars(img, 900, 113, None, 0xFFFFFF, 0.04)
    # a ringed planet, bottom left, and a little moon
    yy, xx = np.mgrid[0:BG, 0:BG].astype(np.float32)
    cx, cy, r = 260, 1030, 200
    pl = disc(BG, BG, cx, cy, r, 2)
    band = np.sin((yy - cy) / 14 + noise(BG, BG, 40, 2, 114) * 4) * 0.5 + 0.5
    pc = mixc(rgb(0xD89A5A)[None, None, :], rgb(0x9A5A3A)[None, None, :], band[..., None])
    shade = np.clip(((xx - cx) * 0.5 + (yy - cy) * -0.7) / r + 0.6, 0, 1)
    pc *= (0.25 + 0.75 * shade)[..., None]
    ring = np.abs(np.sqrt(((xx - cx) / 2.3) ** 2 + ((yy - cy - (xx - cx) * 0.25) / 0.55) ** 2) - 150) < 26
    ring_back = ring & (yy < cy + (xx - cx) * 0.25)
    put(img, rgb(0xE8C890), ring_back.astype(np.float32) * 0.55)
    put(img, pc, pl)
    put(img, rgb(0xF2D8A8), (ring & ~ring_back).astype(np.float32) * 0.8)
    m = disc(BG, BG, 1060, 220, 60, 2)
    mc = np.zeros_like(img) + rgb(0xB8BCC8)
    mc *= (0.4 + 0.6 * np.clip(((xx - 1060) * -0.6 + (yy - 220) * -0.4) / 60 + 0.5, 0, 1))[..., None]
    put(img, mc, m)
    return img


def bg_embrujada():
    img = vgrad(BG, BG, [(0, 0x0A0614), (0.5, 0x2A1846), (0.75, 0x3E2A5A), (1, 0x120A1C)])
    stars(img, 120, 130, 700, 0xE8DDFF)
    add(img, rgb(0xE8F0C0), glow(BG, BG, 820, 300, 260) * 0.45)
    img[...] = np.maximum(img, disc(BG, BG, 820, 300, 140, 2)[..., None] * rgb(0xF2F0D2))
    n = noise(BG, BG, 70, 3, 131)
    put(img, rgb(0xC8C0A8), (disc(BG, BG, 820, 300, 140, 2) * np.clip(n - 0.55, 0, 1) * 1.8))
    clouds(img, 5, 132, 260, 420, 0x2A1E40, 0.85, 0.8)
    # the hill, the house on it, the trees
    ys = ridge(BG, 960, 80, [0.6, 1.4], 133, BG)
    hill = under(BG, BG, ys)
    r = Rng(134)

    def house(dr, ss):
        x, y = 640, int(ys[640]) + 10
        dr.rectangle([(x - 200) * ss, (y - 250) * ss, (x + 200) * ss, y * ss], fill=255)
        dr.polygon([((x - 230) * ss, (y - 250) * ss), (x * ss, (y - 400) * ss), ((x + 230) * ss, (y - 250) * ss)],
                   fill=255)
        dr.rectangle([(x + 110) * ss, (y - 470) * ss, (x + 190) * ss, (y - 250) * ss], fill=255)
        dr.polygon([((x + 95) * ss, (y - 470) * ss), ((x + 150) * ss, (y - 580) * ss), ((x + 205) * ss, (y - 470) * ss)],
                   fill=255)
        dr.rectangle([(x - 190) * ss, (y - 330) * ss, (x - 130) * ss, (y - 250) * ss], fill=255)
        dr.rectangle([(x - 175) * ss, (y - 370) * ss, (x - 150) * ss, (y - 330) * ss], fill=255)
        tips = []
        tree_branches(dr, ss, 230, ys[230] + 5, math.pi / 2 + 0.1, 160, 22, 7, r, tips)
        tree_branches(dr, ss, 1080, ys[1080] + 5, math.pi / 2 - 0.15, 140, 20, 7, r, tips)
        for gx in (380, 460, 860, 940):
            gy = ys[gx] + 4
            dr.rounded_rectangle([(gx - 18) * ss, (gy - 50) * ss, (gx + 18) * ss, (gy + 6) * ss], radius=16 * ss,
                                 fill=255)
    put(img, rgb(0x08040E), np.maximum(hill, mask(BG, BG, house)))

    def windows(dr, ss):
        x, y = 640, int(ys[640]) + 10
        for (wx, wy) in ((-140, -200), (-50, -200), (40, -200), (-140, -110), (40, -110), (130, -400)):
            dr.rectangle([(x + wx) * ss, (y + wy) * ss, (x + wx + 36) * ss, (y + wy + 50) * ss], fill=255)
        dr.ellipse([(x - 26) * ss, (y - 340) * ss, (x + 26) * ss, (y - 288) * ss], fill=255)
    wm = mask(BG, BG, windows)
    add(img, rgb(0xFFB040), blur(wm, 14) * 0.7)
    put(img, rgb(0xFFD070), wm)
    # fog
    fog = noise(BG, BG, 120, 3, 135) * np.clip((yy_grid(BG) - 880) / 250, 0, 1)
    put(img, rgb(0x6A5A8A), fog * 0.45)
    return img


def yy_grid(n):
    return np.arange(n, dtype=np.float32)[:, None] * np.ones((1, n), np.float32)


def bg_aliens():
    img = vgrad(BG, BG, [(0, 0x061A1A), (0.45, 0x0E4A3A), (0.66, 0x4AA878), (1, 0x0A2A1E)])
    stars(img, 260, 140, 700, 0xD8FFE8)
    for (cx, cy, r, c) in ((300, 260, 90, 0xD8F0A0), (1000, 180, 50, 0xF0B8F0)):
        add(img, rgb(c), glow(BG, BG, cx, cy, r * 2.4) * 0.35)
        m = disc(BG, BG, cx, cy, r, 2)
        yy, xx = np.mgrid[0:BG, 0:BG].astype(np.float32)
        cc = np.zeros_like(img) + rgb(c)
        cc *= (0.35 + 0.65 * np.clip(((xx - cx) * 0.7 + (yy - cy) * -0.6) / r + 0.5, 0, 1))[..., None]
        cr = noise(BG, BG, 18, 2, 141 + cx) > 0.66
        cc[cr] *= 0.85
        put(img, cc, m)
    for base, amp, c0, c1, s in [(820, 120, 0x1E5A48, 0x0E3A2E, 142), (960, 130, 0x164838, 0x0A2A20, 143),
                                 (1120, 90, 0x0E3428, 0x061C16, 144)]:
        ys = ridge(BG, base, amp, [1.1, 2.9, 6.3], s, BG)
        tone = vgrad(BG, BG, [(0, c0), (1, c1)]) * (0.85 + noise(BG, BG, 20, 3, s + 5) * 0.3)[..., None]
        put(img, tone, under(BG, BG, ys))
    # glowing plants
    r = Rng(145)

    def plants(dr, ss):
        for _ in range(14):
            x = r.u(0, BG)
            y = r.u(1080, 1260)
            h = r.u(60, 200)
            dr.line([x * ss, y * ss, (x + r.u(-20, 20)) * ss, (y - h) * ss], fill=255, width=int(8 * ss))
            dr.ellipse([(x - h * 0.3) * ss, (y - h - h * 0.15) * ss, (x + h * 0.3) * ss, (y - h + h * 0.12) * ss],
                       fill=255)
    pm = mask(BG, BG, plants)
    add(img, rgb(0x6BFF5C), blur(pm, 16) * 0.5)
    put(img, rgb(0x2A6A3A), pm)
    put(img, rgb(0xA8FF90), np.clip(pm - np.roll(pm, 5, axis=0), 0, 1) * 0.7)
    return img


# ---------------------------------------------------------------------------
# the board's materials, 720 x 720

def tex_paper(base, seed, fiber=0.08):
    img = np.zeros((TEX, TEX, 3), np.float32) + rgb(base)
    n = noise(TEX, TEX, 90, 4, seed)
    f = noise(TEX, TEX, 2, 2, seed + 1)
    img *= (1 - fiber + n[..., None] * fiber * 1.2 + f[..., None] * fiber * 0.6)
    return img


def tex_papiro(size=TEX):
    img = np.zeros((size, size, 3), np.float32) + rgb(0xE8D3A6)
    r = np.random.default_rng(150)
    h = np.asarray(Image.fromarray((r.random((size, 40)) * 255).astype(np.uint8)).resize((size, size), Image.BILINEAR),
                   np.float32) / 255
    v = np.asarray(Image.fromarray((r.random((40, size)) * 255).astype(np.uint8)).resize((size, size), Image.BILINEAR),
                   np.float32) / 255
    img *= (0.84 + h[..., None] * 0.12 + v[..., None] * 0.12)
    img *= (0.92 + noise(size, size, 120, 3, 151)[..., None] * 0.14)
    return img


def tex_wood():
    img = wood_grain(TEX, TEX, 0xD2A26A, 0x9A6A3A, 160, vertical=False, scale=1.2)
    return img


def tex_stone(base, seed):
    n = noise(TEX, TEX, 60, 5, seed)
    f = noise(TEX, TEX, 3, 2, seed + 1)
    img = np.zeros((TEX, TEX, 3), np.float32) + rgb(base)
    img *= (0.82 + n[..., None] * 0.3 + f[..., None] * 0.1)
    cr = np.abs(noise(TEX, TEX, 140, 3, seed + 2) - 0.5) < 0.006
    img[cr] *= 0.6
    return img


def tex_brushed(base, seed):
    r = np.random.default_rng(seed)
    s = np.asarray(Image.fromarray((r.random((TEX, 48)) * 255).astype(np.uint8)).resize((TEX, TEX), Image.BILINEAR),
                   np.float32) / 255
    img = np.zeros((TEX, TEX, 3), np.float32) + rgb(base)
    img *= (0.88 + s[..., None] * 0.2)
    yy, xx = np.mgrid[0:TEX, 0:TEX].astype(np.float32)
    add(img, rgb(0xFFFFFF), np.exp(-((xx + yy - 700) / 300) ** 2) * 0.12)
    return img


def tex_sand(base, seed):
    img = np.zeros((TEX, TEX, 3), np.float32) + rgb(base)
    img *= (0.9 + noise(TEX, TEX, 2, 1, seed)[..., None] * 0.14 + noise(TEX, TEX, 80, 3, seed + 1)[..., None] * 0.08)
    return img


def tex_glass(base, seed, accent):
    img = np.zeros((TEX, TEX, 3), np.float32) + rgb(base)
    yy, xx = np.mgrid[0:TEX, 0:TEX].astype(np.float32)
    add(img, rgb(accent), np.exp(-((xx - yy * 0.6 - 200) / 120) ** 2) * 0.08)
    add(img, rgb(accent), np.exp(-((xx - yy * 0.6 - 520) / 60) ** 2) * 0.05)
    img *= (0.92 + noise(TEX, TEX, 90, 3, seed)[..., None] * 0.16)
    return img


def tex_for(key):
    if key == 'clasico':
        return tex_paper(0xF4F1EA, 170, 0.05)
    if key == 'campo':
        img = tex_paper(0xF3F0DC, 171, 0.05)
        yy, xx = np.mgrid[0:TEX, 0:TEX]
        weave = ((xx % 6 < 1) | (yy % 6 < 1)).astype(np.float32)
        put(img, rgb(0xC8C4A0), weave * 0.18)
        return img
    if key == 'madera':
        return tex_wood()
    if key == 'papiro':
        return tex_papiro()
    if key in ('playa', 'desierto'):
        img = tex_sand(PAL[key]['board'], 172 if key == 'playa' else 173)
        if key == 'desierto':
            yy = np.arange(TEX, dtype=np.float32)[:, None]
            strata = np.sin(yy / 22 + noise(TEX, TEX, 50, 2, 174) * 5) * 0.5 + 0.5
            img *= (0.93 + strata[..., None] * 0.1)
        return img
    if key == 'nieve':
        img = tex_paper(0xE8F4FB, 175, 0.04)
        cr = noise(TEX, TEX, 10, 2, 176)
        add(img, rgb(0xFFFFFF), np.clip((cr - 0.62) * 4, 0, 1) * 0.5)
        return img
    if key == 'piedra':
        return tex_stone(0x8A8780, 177)
    if key == 'mar':
        img = np.zeros((TEX, TEX, 3), np.float32) + rgb(0x0C4A66)
        c = noise(TEX, TEX, 40, 3, 178)
        add(img, rgb(0x7FE6FF), np.clip(1 - np.abs(c - 0.5) * 18, 0, 1) * 0.11)
        img *= (0.9 + noise(TEX, TEX, 120, 2, 179)[..., None] * 0.2)
        return img
    if key == 'ciudad':
        return tex_glass(0x161D2E, 180, 0x7FD6FF)
    if key == 'metal':
        return tex_brushed(0x9AA0A8, 181)
    if key == 'jardin':
        img = tex_paper(0xF4EEE2, 182, 0.06)
        r = np.random.default_rng(183)
        for _ in range(260):
            x, y = r.integers(0, TEX - 40, 2)
            a = r.uniform(0, math.pi)
            ln = r.integers(10, 40)
            xs = (x + np.cos(a) * np.arange(ln)).astype(int)
            ys = (y + np.sin(a) * np.arange(ln)).astype(int)
            ok = (xs >= 0) & (xs < TEX) & (ys >= 0) & (ys < TEX)
            img[ys[ok], xs[ok]] *= 0.93
        return img
    if key == 'neon':
        img = np.zeros((TEX, TEX, 3), np.float32) + rgb(0x0E0820)
        yy, xx = np.mgrid[0:TEX, 0:TEX]
        g = ((xx % 40 == 0) | (yy % 40 == 0)).astype(np.float32)
        add(img, rgb(0x00E5FF), blur(g, 1.5) * 0.25)
        return img
    if key == 'espacio':
        img = tex_glass(0x0C0F22, 184, 0x8C7CFF)
        stars(img, 160, 185, None, 0xCFD4FF, 0)
        img = img * 0.9 + rgb(0x0C0F22) * 0.1
        return img
    if key == 'embrujada':
        img = np.zeros((TEX, TEX, 3), np.float32) + rgb(0x241A2E)
        # a damask-like wallpaper, faint
        tile = 90

        def motif(dr, ss):
            for ty in range(-1, TEX // tile + 2):
                for tx in range(-1, TEX // tile + 2):
                    cx = tx * tile + (tile / 2 if ty % 2 else 0)
                    cy = ty * tile
                    dr.ellipse([(cx - 14) * ss, (cy - 26) * ss, (cx + 14) * ss, (cy + 26) * ss], outline=255,
                               width=2 * ss)
                    dr.ellipse([(cx - 4) * ss, (cy - 4) * ss, (cx + 4) * ss, (cy + 4) * ss], fill=255)
        put(img, rgb(0x46355A), mask(TEX, TEX, motif) * 0.55)
        img *= (0.9 + noise(TEX, TEX, 80, 3, 186)[..., None] * 0.2)
        return img
    if key == 'aliens':
        img = np.zeros((TEX, TEX, 3), np.float32) + rgb(0x10261D)
        s = 26

        def hexes(dr, ss):
            for row in range(-1, int(TEX / (s * 1.5)) + 2):
                for col in range(-1, int(TEX / (s * 1.732)) + 2):
                    cx = col * s * 1.732 + (s * 0.866 if row % 2 else 0)
                    cy = row * s * 1.5
                    pts = [((cx + s * math.cos(math.radians(60 * k + 30))) * ss,
                            (cy + s * math.sin(math.radians(60 * k + 30))) * ss) for k in range(6)]
                    dr.polygon(pts, outline=255, width=2 * ss)
        put(img, rgb(0x2A6A4A), mask(TEX, TEX, hexes) * 0.6)
        return img
    raise KeyError(key)


# ---------------------------------------------------------------------------
# the given digits

_font_cache = {}


def digit_mask(d, size, scale=0.78, weight=0):
    """the digit's shape at size x size, 0..1, from Rubik"""
    ss = 4
    key = (size, scale)
    if key not in _font_cache:
        _font_cache[key] = ImageFont.truetype(FONT, int(size * ss * scale))
    f = _font_cache[key]
    im = Image.new('L', (size * ss, size * ss), 0)
    dr = ImageDraw.Draw(im)
    b = dr.textbbox((0, 0), str(d), font=f)
    x = (size * ss - (b[2] - b[0])) / 2 - b[0]
    y = (size * ss - (b[3] - b[1])) / 2 - b[1]
    dr.text((x, y), str(d), font=f, fill=255)
    if weight:
        im = im.filter(ImageFilter.MaxFilter(weight * 2 + 1) if weight > 0 else ImageFilter.MinFilter(-weight * 2 + 1))
    im = im.resize((size, size), Image.LANCZOS)
    return np.asarray(im, np.float32) / 255.0


SEG = {0: 'abcdef', 1: 'bc', 2: 'abged', 3: 'abgcd', 4: 'fgbc', 5: 'afgcd', 6: 'afgedc', 7: 'abc', 8: 'abcdefg',
       9: 'abcfgd'}


def seg_mask(d, size):
    ss = 4
    w, h = size * 0.46, size * 0.74
    x0, y0 = (size - w) / 2, (size - h) / 2
    t = size * 0.1

    def draw(dr, s):
        def hseg(y):
            dr.polygon([((x0 + t * 0.6) * s, y * s), ((x0 + t * 1.1) * s, (y - t / 2) * s), ((x0 + w - t * 1.1) * s, (y - t / 2) * s),
                        ((x0 + w - t * 0.6) * s, y * s), ((x0 + w - t * 1.1) * s, (y + t / 2) * s), ((x0 + t * 1.1) * s, (y + t / 2) * s)],
                       fill=255)

        def vseg(x, ya, yb):
            dr.polygon([(x * s, (ya + t * 0.6) * s), ((x + t / 2) * s, (ya + t * 1.1) * s), ((x + t / 2) * s, (yb - t * 1.1) * s),
                        (x * s, (yb - t * 0.6) * s), ((x - t / 2) * s, (yb - t * 1.1) * s), ((x - t / 2) * s, (ya + t * 1.1) * s)],
                       fill=255)
        segs = SEG[d]
        ym = y0 + h / 2
        if 'a' in segs: hseg(y0)
        if 'g' in segs: hseg(ym)
        if 'd' in segs: hseg(y0 + h)
        if 'f' in segs: vseg(x0, y0, ym)
        if 'b' in segs: vseg(x0 + w, y0, ym)
        if 'e' in segs: vseg(x0, ym, y0 + h)
        if 'c' in segs: vseg(x0 + w, ym, y0 + h)
    return mask(size, size, draw, ss)


# the digits of an LED board, 5 x 7
DOTS = {
    1: ['..#..', '.##..', '..#..', '..#..', '..#..', '..#..', '.###.'],
    2: ['.###.', '#...#', '....#', '...#.', '..#..', '.#...', '#####'],
    3: ['####.', '....#', '....#', '.###.', '....#', '....#', '####.'],
    4: ['...#.', '..##.', '.#.#.', '#..#.', '#####', '...#.', '...#.'],
    5: ['#####', '#....', '####.', '....#', '....#', '#...#', '.###.'],
    6: ['..##.', '.#...', '#....', '####.', '#...#', '#...#', '.###.'],
    7: ['#####', '....#', '...#.', '..#..', '.#...', '.#...', '.#...'],
    8: ['.###.', '#...#', '#...#', '.###.', '#...#', '#...#', '.###.'],
    9: ['.###.', '#...#', '#...#', '.####', '....#', '...#.', '.##..'],
}


def dot_mask(d, size):
    out = np.zeros((size, size), np.float32)
    cols, rows = 5, 7
    cw, ch = size * 0.62 / cols, size * 0.84 / rows
    x0, y0 = (size - cw * cols) / 2, (size - ch * rows) / 2
    for r in range(rows):
        for c in range(cols):
            if DOTS[d][r][c] == '#':
                cx, cy = x0 + (c + 0.5) * cw, y0 + (r + 0.5) * ch
                out = np.maximum(out, disc(size, size, cx, cy, min(cw, ch) * 0.42, 1.0))
    return out


def style_digit(key, d, size):
    """RGBA float of the digit d, size x size"""
    p = PAL[key]
    col = rgb(p['given'])
    out = np.zeros((size, size, 4), np.float32)
    k = size / 60.0
    if key == 'aliens':
        m = seg_mask(d, size)
        g = blur(m, 3 * k)
        a = np.maximum(m, g * 0.9)
        out[..., :3] = mixc(col * 0.8, rgb(0xD8FFD0), m[..., None] * 0.75)
        out[..., 3] = a * 255
        return out
    if key == 'ciudad':
        m = dot_mask(d, size)
        g = blur(m, 2.5 * k)
        a = np.maximum(m, g * 0.8)
        out[..., :3] = mixc(col * 0.7, rgb(0xFFF0C0), m[..., None] * 0.6)
        out[..., 3] = a * 255
        return out
    if key == 'neon':
        m = digit_mask(d, size, 0.86)
        inner = digit_mask(d, size, 0.86, weight=-int(6 * k))
        tube = np.clip(m - inner * 0.85, 0, 1)
        g = blur(tube, 3.5 * k)
        a = np.clip(np.maximum(tube, g * 1.8), 0, 1)
        out[..., :3] = mixc(col, rgb(0xFFF0FC), np.clip(tube * 1.3, 0, 1)[..., None] * 0.7)
        out[..., 3] = a * 255
        return out
    if key in ('mar', 'espacio', 'embrujada'):
        m = digit_mask(d, size, 0.78)
        if key == 'embrujada':
            # a slight wobble, like seen through old glass
            yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
            sx = np.clip(xx + np.sin(yy / (6 * k)) * 1.6 * k, 0, size - 1).astype(int)
            m = m[yy.astype(int), sx]
        glow_c = {'mar': 0x7FE6FF, 'espacio': 0x8C7CFF, 'embrujada': 0x8CFF6B}[key]
        g = blur(m, 3.5 * k)
        a = np.clip(np.maximum(m, g * 1.1), 0, 1)
        out[..., :3] = mixc(rgb(glow_c), col, np.clip(m * 1.4, 0, 1)[..., None])
        out[..., 3] = a * 255
        return out
    if key in ('piedra', 'madera'):
        # carved: dark inside, a lit lower-right lip and a shadow upper-left
        m = digit_mask(d, size, 0.8, weight=int(1 * k))
        up = np.roll(np.roll(m, -2, 0), -2, 1)
        dn = np.roll(np.roll(m, 2, 0), 2, 1)
        light = np.clip(dn - m, 0, 1)
        shade = np.clip(up - m, 0, 1)
        base = col if key == 'piedra' else rgb(0x3A200C)
        c = np.zeros((size, size, 3), np.float32) + base
        inner = blur(m, 1.5 * k)
        c = mixc(c * 0.7, c * 1.3, (1 - inner)[..., None] * 0.6)
        lip = rgb(0xD8D4CC) if key == 'piedra' else rgb(0xE8C090)
        out[..., :3] = c * m[..., None] + lip * light[..., None] + (base * 0.5) * shade[..., None]
        a = np.clip(m + light * 0.8 + shade * 0.6, 0, 1)
        out[..., :3] /= np.maximum(a, 1e-3)[..., None]
        out[..., 3] = a * 255
        return out
    if key == 'metal':
        m = digit_mask(d, size, 0.8, weight=int(1 * k))
        yy = np.linspace(0, 1, size, dtype=np.float32)[:, None] * np.ones((1, size), np.float32)
        chrome = np.interp(yy, [0, 0.45, 0.5, 1], [215, 120, 55, 150]).astype(np.float32)
        c = np.stack([chrome * 0.95, chrome * 0.98, chrome], -1)
        edge = np.clip(blur(m, 1.2 * k) * 2 - m, 0, 1)
        ring = np.clip(digit_mask(d, size, 0.8, weight=int(3 * k)) - m, 0, 1)
        out[..., :3] = c * m[..., None] + rgb(0x0E1012) * ring[..., None]
        a = np.clip(m + ring, 0, 1)
        out[..., :3] /= np.maximum(a, 1e-3)[..., None]
        out[..., 3] = a * 255
        del edge
        return out
    if key in ('papiro', 'jardin'):
        # ink: edges eaten a little, the stroke a little uneven
        m = digit_mask(d, size, 0.8)
        n = noise(size, size, 3 * k, 2, d * 7 + (1 if key == 'jardin' else 0))
        m = np.clip(m * (0.75 + n * 0.5) * 1.15 - 0.1, 0, 1)
        m = blur(m, 0.5)
        out[..., :3] = col
        out[..., 3] = m * 255
        return out
    if key in ('playa', 'campo', 'nieve', 'desierto'):
        # painted, with a light outline that lifts it off the cell
        m = digit_mask(d, size, 0.78)
        ring = np.clip(digit_mask(d, size, 0.78, weight=int(2 * k)) - m, 0, 1)
        lc = {'playa': 0xFFFFFF, 'campo': 0xFFFFF0, 'nieve': 0xFFFFFF, 'desierto': 0xFFE8C8}[key]
        c = np.zeros((size, size, 3), np.float32) + col
        if key == 'nieve':
            yy = np.linspace(0, 1, size, dtype=np.float32)[:, None] * np.ones((1, size), np.float32)
            c = mixc(rgb(0x9FD4F2)[None, None, :], col[None, None, :], np.clip(yy * 1.6, 0, 1)[..., None])
        out[..., :3] = c * m[..., None] + rgb(lc) * ring[..., None]
        a = np.clip(m + ring * 0.9, 0, 1)
        out[..., :3] /= np.maximum(a, 1e-3)[..., None]
        out[..., 3] = a * 255
        return out
    # clasico: plain, a soft shadow
    m = digit_mask(d, size, 0.78)
    sh = np.roll(np.roll(blur(m, 1.2 * k), int(1 * k + 0.5), 0), int(1 * k + 0.5), 1)
    a = np.clip(m + sh * 0.25, 0, 1)
    out[..., :3] = col
    out[..., 3] = a * 255
    return out


# ---------------------------------------------------------------------------
# the sprites that move

def sprite(w, h, draw, color, blur_r=0, glow_c=None, alpha=1.0):
    m = mask(w, h, draw, 3)
    if blur_r:
        m = blur(m, blur_r)
    out = np.zeros((h, w, 4), np.float32)
    out[..., :3] = rgb(color)
    a = m
    if glow_c is not None:
        g = blur(m, max(w, h) / 8)
        a = np.clip(np.maximum(m, g * 1.2), 0, 1)
        out[..., :3] = mixc(rgb(glow_c)[None, None, :], rgb(color)[None, None, :], m[..., None])
    out[..., 3] = a * 255 * alpha
    return out


def spr_bubble():
    s = 40
    out = np.zeros((s, s, 4), np.float32)
    ring = np.clip(disc(s, s, 20, 20, 17, 1.2) - disc(s, s, 20, 20, 14.5, 1.2), 0, 1)
    hi = disc(s, s, 14, 13, 4, 1.5)
    fill = disc(s, s, 20, 20, 15, 1.2) * 0.12
    out[..., :3] = rgb(0xDFFBFF)
    out[..., 3] = np.clip(ring * 0.75 + hi * 0.9 + fill, 0, 1) * 255
    return out


def spr_fish(w, h, c1, c2):
    def d(dr, s):
        dr.ellipse([w * 0.12 * s, h * 0.15 * s, w * 0.78 * s, h * 0.85 * s], fill=255)
        dr.polygon([(w * 0.7 * s, h * 0.5 * s), (w * 0.98 * s, h * 0.12 * s), (w * 0.98 * s, h * 0.88 * s)], fill=255)
    m = mask(w, h, d, 3)
    out = np.zeros((h, w, 4), np.float32)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    stripe = (np.sin(xx / (w * 0.08)) > 0.6)
    out[..., :3] = np.where(stripe[..., None], rgb(c2), rgb(c1))
    eye = disc(h, w, w * 0.26, h * 0.42, h * 0.08, 1)
    out[..., :3] = mixc(out[..., :3], rgb(0x101010)[None, None, :], eye[..., None])
    out[..., 3] = m * 255
    return out


def spr_ghost():
    w, h = 88, 104

    def d(dr, s):
        dr.ellipse([8 * s, 4 * s, 80 * s, 76 * s], fill=255)
        dr.rectangle([8 * s, 40 * s, 80 * s, 86 * s], fill=255)
        for k in range(4):
            x = 8 + k * 18
            dr.ellipse([x * s, 78 * s, (x + 18) * s, 100 * s], fill=255)
    m = mask(w, h, d, 3)
    out = np.zeros((h, w, 4), np.float32)
    out[..., :3] = rgb(0xE8F2FF)
    eyes = disc(h, w, 32, 38, 6, 1) + disc(h, w, 56, 38, 6, 1) + disc(h, w, 44, 58, 7, 1) * 0.8
    out[..., :3] = mixc(out[..., :3], rgb(0x1A1028)[None, None, :], np.clip(eyes, 0, 1)[..., None])
    g = blur(m, 6)
    out[..., 3] = np.clip(np.maximum(m * 0.72, g * 0.5), 0, 1) * 255
    return out


def spr_bat():
    w, h = 64, 30

    def d(dr, s):
        dr.ellipse([27 * s, 9 * s, 37 * s, 23 * s], fill=255)
        for side in (-1, 1):
            pts = [(32, 12), (32 + side * 12, 4), (32 + side * 30, 2), (32 + side * 24, 12), (32 + side * 20, 10),
                   (32 + side * 16, 18), (32 + side * 10, 14), (32, 20)]
            dr.polygon([(x * s, y * s) for x, y in pts], fill=255)
    return sprite(w, h, d, 0x0A0410)


def spr_gull():
    w, h = 60, 24

    def d(dr, s):
        dr.arc([2 * s, 4 * s, 32 * s, 34 * s], 200, 330, fill=255, width=4 * s)
        dr.arc([28 * s, 4 * s, 58 * s, 34 * s], 210, 340, fill=255, width=4 * s)
    return sprite(w, h, d, 0x2A3A48)


def spr_foam():
    w, h = 190, 34

    def d(dr, s):
        for k in range(6):
            x = 10 + k * 30
            dr.ellipse([x * s, (12 + (k % 2) * 4) * s, (x + 40) * s, (30) * s], fill=255)
    return sprite(w, h, d, 0xFFFFFF, blur_r=2, alpha=0.75)


def spr_cloud(w=170, h=70, color=0xFFFFFF, alpha=0.92):
    def d(dr, s):
        dr.ellipse([10 * s, 30 * s, 70 * s, 66 * s], fill=255)
        dr.ellipse([40 * s, 8 * s, 110 * s, 64 * s], fill=255)
        dr.ellipse([90 * s, 22 * s, 160 * s, 66 * s], fill=255)
    return sprite(w, h, d, color, blur_r=3, alpha=alpha)


def spr_butterfly():
    w, h = 40, 32

    def d(dr, s):
        dr.ellipse([4 * s, 2 * s, 20 * s, 18 * s], fill=255)
        dr.ellipse([20 * s, 2 * s, 36 * s, 18 * s], fill=255)
        dr.ellipse([8 * s, 14 * s, 20 * s, 28 * s], fill=255)
        dr.ellipse([20 * s, 14 * s, 32 * s, 28 * s], fill=255)
    out = sprite(w, h, d, 0xFF9A2A)
    out[12:30, 19:21, :3] = rgb(0x2A1A10)
    out[12:30, 19:21, 3] = 255
    return out


def spr_dot(s, color, glow_c=None, core=0.35):
    out = np.zeros((s, s, 4), np.float32)
    c = s / 2
    g = glow(s, s, c - 0.5, c - 0.5, s * 0.28)
    m = disc(s, s, c - 0.5, c - 0.5, s * core / 2, 1)
    out[..., :3] = mixc(rgb(glow_c if glow_c else color)[None, None, :], rgb(color)[None, None, :], m[..., None])
    out[..., 3] = np.clip(np.maximum(m, g * 0.85), 0, 1) * 255
    return out


def spr_sparkle(s, color):
    out = np.zeros((s, s, 4), np.float32)
    yy, xx = np.mgrid[0:s, 0:s].astype(np.float32)
    c = s / 2 - 0.5
    dx, dy = np.abs(xx - c), np.abs(yy - c)
    star = np.clip(1 - (np.minimum(dx, dy) * 4 + np.maximum(dx, dy) * 0.35) / (s * 0.25), 0, 1)
    g = glow(s, s, c, c, s * 0.18)
    out[..., :3] = rgb(color)
    out[..., 3] = np.clip(np.maximum(star, g), 0, 1) * 255
    return out


def spr_flake(s):
    def d(dr, ss):
        c = s / 2
        for k in range(6):
            a = math.radians(k * 60)
            dr.line([c * ss, c * ss, (c + math.cos(a) * c * 0.9) * ss, (c + math.sin(a) * c * 0.9) * ss], fill=255,
                    width=max(1, int(s / 12 * ss)))
            for t in (0.5, 0.7):
                bx, by = c + math.cos(a) * c * t, c + math.sin(a) * c * t
                for sgn in (-1, 1):
                    b = a + sgn * 0.8
                    dr.line([bx * ss, by * ss, (bx + math.cos(b) * c * 0.25) * ss, (by + math.sin(b) * c * 0.25) * ss],
                            fill=255, width=max(1, int(s / 16 * ss)))
    return sprite(s, s, d, 0xFFFFFF, alpha=0.9)


def spr_drop():
    w, h = 14, 24

    def d(dr, s):
        dr.polygon([(7 * s, 0), (12 * s, 13 * s), (2 * s, 13 * s)], fill=255)
        dr.ellipse([2 * s, 8 * s, 12 * s, 22 * s], fill=255)
    return sprite(w, h, d, 0xA8D8F0, alpha=0.7)


def spr_petal():
    w, h = 26, 20

    def d(dr, s):
        dr.ellipse([2 * s, 4 * s, 24 * s, 16 * s], fill=255)
    out = sprite(w, h, d, 0xF7B6CA, alpha=0.95)
    out[8:12, 2:8, :3] = rgb(0xE88AA6)
    return out


def spr_tumbleweed():
    s = 72
    r = Rng(300)

    def d(dr, ss):
        for _ in range(40):
            a, b = r.u(0, 360), r.u(0, 360)
            rr = r.u(14, 34)
            dr.arc([(36 - rr) * ss, (36 - rr) * ss, (36 + rr) * ss, (36 + rr) * ss], a, a + r.u(40, 160), fill=255,
                   width=int(2 * ss))
            del b
    return sprite(s, s, d, 0x7A5A30)


def spr_puff():
    return spr_cloud(90, 34, 0xF2D2A0, 0.45)


def spr_plane():
    w, h = 44, 14
    out = np.zeros((h, w, 4), np.float32)
    for (x, c) in ((6, 0xFF4040), (38, 0x40FF60)):
        m = glow(h, w, x, 7, 3)
        out[..., :3] = np.where(m[..., None] > out[..., 3:4] / 255, rgb(c), out[..., :3])
        out[..., 3] = np.maximum(out[..., 3], m * 255)
    out[6:8, 8:36, :3] = rgb(0xC8D0E0)
    out[6:8, 8:36, 3] = 160
    return out


def spr_window():
    out = np.zeros((20, 16, 4), np.float32)
    out[..., :3] = rgb(0xFFD27A)
    g = glow(20, 16, 7.5, 9.5, 7)
    out[..., 3] = np.clip(g * 0.6, 0, 1) * 255
    out[4:16, 4:12, 3] = 255
    return out


def spr_comet():
    w, h = 130, 28
    out = np.zeros((h, w, 4), np.float32)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    tail = np.clip(1 - np.abs(yy - h / 2) / (2 + xx / w * 10), 0, 1) * (xx / w) ** 2
    head = glow(h, w, w - 12, h / 2, 6)
    out[..., :3] = mixc(rgb(0x8CA8FF)[None, None, :], rgb(0xFFFFFF)[None, None, :], head[..., None])
    out[..., 3] = np.clip(np.maximum(tail * 0.8, head), 0, 1) * 255
    return out


def spr_ufo():
    w, h = 120, 64

    def body(dr, s):
        dr.ellipse([4 * s, 28 * s, 116 * s, 52 * s], fill=255)
        dr.ellipse([36 * s, 8 * s, 84 * s, 40 * s], fill=255)
    m = mask(w, h, body, 3)
    out = np.zeros((h, w, 4), np.float32)
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    out[..., :3] = np.where((yy < 30)[..., None], rgb(0xA8FFD0), rgb(0x8A96A8))
    lights = sum(disc(h, w, x, 41, 3.5, 1) for x in (20, 40, 60, 80, 100))
    out[..., :3] = mixc(out[..., :3], rgb(0xF5FF6B)[None, None, :], np.clip(lights, 0, 1)[..., None])
    beam = np.clip(1 - np.abs(xx - 60) / (10 + (yy - 50) * 1.2), 0, 1) * (yy > 50)
    out[..., 3] = np.clip(np.maximum(m, beam * 0.35), 0, 1) * 255
    out[..., :3] = np.where((beam > 0.01)[..., None] & (m < 0.5)[..., None], rgb(0xA8FFB0), out[..., :3])
    return out


def sprites_for(key):
    if key == 'clasico': return [spr_dot(22, 0xBFD6FF, 0x6A9AE8)]
    if key == 'campo': return [spr_cloud(), spr_butterfly()]
    if key == 'madera': return [spr_dot(18, 0xFFE0A8, 0xC88A3A)]
    if key == 'papiro': return [spr_dot(14, 0xF2DDB0, 0xA88850)]
    if key == 'playa': return [spr_gull(), spr_foam()]
    if key == 'nieve': return [spr_flake(30), spr_flake(16)]
    if key == 'piedra': return [spr_drop(), spr_dot(20, 0xC8FF9A, 0x6A9A3A)]
    if key == 'mar': return [spr_bubble(), spr_fish(96, 52, 0xFF8A3A, 0xFFFFFF), spr_fish(60, 32, 0x3AC8FF, 0x1A6AB8)]
    if key == 'ciudad': return [spr_window(), spr_plane()]
    if key == 'metal': return [spr_sparkle(44, 0xFFF4E0)]
    if key == 'jardin': return [spr_petal()]
    if key == 'desierto': return [spr_tumbleweed(), spr_puff()]
    if key == 'neon': return [spr_dot(36, 0xFFD8FA, 0xFF2BD6)]
    if key == 'espacio': return [spr_sparkle(28, 0xFFFFFF), spr_comet()]
    if key == 'embrujada': return [spr_ghost(), spr_bat()]
    if key == 'aliens': return [spr_ufo(), spr_sparkle(24, 0xD8FFE8)]
    return []


# ---------------------------------------------------------------------------
# the thumbnail: the picture's middle and a little board on it

def thumbnail(key, bg, tex):
    p = PAL[key]
    c = Image.fromarray(np.clip(bg[280:1000, 280:1000], 0, 255).astype(np.uint8)).resize((THUMB, THUMB), Image.LANCZOS)
    img = np.asarray(c, np.float32).copy()
    bs = 132
    t = Image.fromarray(np.clip(tex[:bs * 3, :bs * 3], 0, 255).astype(np.uint8)).resize((bs, bs), Image.LANCZOS)
    x0 = y0 = (THUMB - bs) // 2
    sh = blur(np.pad(np.ones((bs, bs), np.float32), 34)[:THUMB, :THUMB], 8)
    put(img, rgb(0x000000), np.roll(np.roll(sh, 4, 0), 4, 1) * 0.45)
    img[y0:y0 + bs, x0:x0 + bs] = np.asarray(t, np.float32)
    box = rgb(p['box'])
    for k in range(4):
        q = x0 + k * bs // 3
        img[y0:y0 + bs, max(x0, q - 1):min(x0 + bs, q + 2)] = box
        q = y0 + k * bs // 3
        img[max(y0, q - 1):min(y0 + bs, q + 2), x0:x0 + bs] = box
    for k in range(1, 9):
        if k % 3 == 0:
            continue
        q = x0 + k * bs // 9
        img[y0:y0 + bs, q] = mixc(img[y0:y0 + bs, q], box, 0.4)
        q = y0 + k * bs // 9
        img[q, x0:x0 + bs] = mixc(img[q, x0:x0 + bs], box, 0.4)
    cell = bs / 9
    for (r, cc, d) in ((0, 0, 5), (1, 4, 7), (2, 7, 3), (4, 2, 9), (4, 6, 1), (6, 1, 8), (7, 5, 2), (8, 8, 6), (5, 4, 4)):
        g = style_digit(key, d, 18)
        gx, gy = int(x0 + cc * cell + (cell - 18) / 2), int(y0 + r * cell + (cell - 18) / 2)
        sub = img[gy:gy + 18, gx:gx + 18]
        put(sub, g[..., :3], g[..., 3] / 255)
    return img


# ---------------------------------------------------------------------------
# the sounds

def env(n, a=0.002, d=0.08):
    t = np.arange(n) / RATE
    return np.minimum(1, t / max(a, 1e-4)) * np.exp(-t / d)


def tone(f, dur, kind='sine', a=0.002, d=0.08, f2=None, vib=0):
    n = int(dur * RATE)
    t = np.arange(n) / RATE
    fr = np.full(n, float(f)) if f2 is None else np.geomspace(f, f2, n)
    if vib:
        fr = fr * (1 + 0.02 * np.sin(2 * math.pi * vib * t))
    ph = np.cumsum(fr) / RATE * 2 * math.pi
    if kind == 'sine':
        s = np.sin(ph)
    elif kind == 'square':
        s = np.sign(np.sin(ph)) * 0.6
    elif kind == 'saw':
        s = (ph / math.pi % 2 - 1) * 0.6
    elif kind == 'tri':
        s = (2 * np.abs(ph / math.pi % 2 - 1) - 1)
    else:
        s = np.sin(ph)
    return s * env(n, a, d)


def lowpass(x, cut):
    a = math.exp(-2 * math.pi * cut / RATE)
    y = np.zeros_like(x)
    acc = 0.0
    for i in range(len(x)):
        acc = (1 - a) * x[i] + a * acc
        y[i] = acc
    return y


def bell(f, dur, ratio=3.5, idx=2.0, d=0.25):
    n = int(dur * RATE)
    t = np.arange(n) / RATE
    m = np.sin(2 * math.pi * f * ratio * t) * idx * np.exp(-t / (d * 0.6))
    return np.sin(2 * math.pi * f * t + m) * env(n, 0.001, d)


def noise_burst(dur, d, cut):
    n = int(dur * RATE)
    x = np.random.default_rng(int(dur * 1000 + cut)).uniform(-1, 1, n)
    return lowpass(x, cut) * env(n, 0.001, d) * 2.0


def seq(parts, gap=0.0):
    out = []
    for p in parts:
        out.append(p)
        if gap:
            out.append(np.zeros(int(gap * RATE)))
    return np.concatenate(out)


def mixs(*xs):
    n = max(len(x) for x in xs)
    out = np.zeros(n)
    for x in xs:
        out[:len(x)] += x
    return out


def norm(x, peak=0.7):
    m = np.max(np.abs(x)) or 1
    return x / m * peak


NOTE = {k: 440 * 2 ** ((v - 9) / 12) for k, v in
        dict(C=0, D=2, E=4, F=5, G=7, A=9, B=11).items()}


def nt(name, octave):
    return NOTE[name] * 2 ** (octave - 4)


def sound_set(s):
    if s == 0:      # soft clicks
        k = dict(place=mixs(tone(880, 0.12, d=0.05), tone(1760, 0.05, d=0.01) * 0.3),
                 note=tone(1320, 0.06, d=0.02) * 0.6,
                 erase=tone(700, 0.1, f2=350, d=0.05),
                 error=seq([tone(330, 0.1, 'tri', d=0.06), tone(260, 0.16, 'tri', d=0.08)]),
                 unit=seq([tone(nt('E', 5), 0.09, d=0.06), tone(nt('G', 5), 0.09, d=0.06), tone(nt('C', 6), 0.22, d=0.12)]),
                 win=seq([tone(nt(n, o), 0.13, d=0.1) for n, o in (('C', 5), ('E', 5), ('G', 5))] +
                         [tone(nt('C', 6), 0.5, d=0.3)]),
                 tap=tone(2200, 0.02, d=0.006) * 0.4,
                 hint=seq([tone(990, 0.08, d=0.06), tone(1320, 0.2, d=0.12)]))
    elif s == 1:    # synth
        k = dict(place=lowpass(tone(440, 0.12, 'square', d=0.05), 2600),
                 note=lowpass(tone(880, 0.06, 'square', d=0.025), 3000) * 0.6,
                 erase=lowpass(tone(600, 0.12, 'saw', f2=200, d=0.06), 2000),
                 error=lowpass(seq([tone(160, 0.12, 'saw', d=0.1), tone(120, 0.18, 'saw', d=0.12)]), 1200),
                 unit=lowpass(seq([tone(nt(n, 5), 0.07, 'square', d=0.05) for n in 'CEGC'][:3] +
                                  [tone(nt('C', 6), 0.2, 'square', d=0.12)]), 3000),
                 win=lowpass(seq([tone(nt(n, o), 0.11, 'square', d=0.08) for n, o in
                                  (('C', 5), ('G', 5), ('E', 5), ('C', 6), ('G', 6))] +
                                 [tone(nt('C', 7), 0.4, 'square', d=0.25)]), 3500),
                 tap=lowpass(tone(1800, 0.02, 'square', d=0.006), 3000) * 0.35,
                 hint=lowpass(tone(500, 0.3, 'saw', f2=1500, d=0.2), 2500))
    elif s == 2:    # bells
        k = dict(place=bell(1046, 0.3, d=0.12),
                 note=bell(1568, 0.15, idx=1.2, d=0.06) * 0.6,
                 erase=bell(523, 0.2, ratio=1.4, d=0.08),
                 error=mixs(bell(311, 0.35, ratio=2.7, d=0.15), bell(329, 0.35, ratio=2.7, d=0.15)),
                 unit=seq([bell(nt(n, 6), 0.12, d=0.1) for n in 'CEG'] + [bell(nt('C', 7), 0.4, d=0.25)]),
                 win=seq([bell(nt(n, o), 0.15, d=0.12) for n, o in (('G', 5), ('C', 6), ('E', 6), ('G', 6))] +
                         [bell(nt('C', 7), 0.8, d=0.45)]),
                 tap=bell(2093, 0.05, idx=0.6, d=0.015) * 0.4,
                 hint=seq([bell(1318, 0.12, d=0.1), bell(1760, 0.3, d=0.2)]))
    elif s == 3:    # wood
        def knock(f, d=0.04):
            return mixs(tone(f, 0.12, d=d) * 0.8, noise_burst(0.05, 0.01, 2500) * 0.5)
        k = dict(place=knock(420),
                 note=knock(760, 0.02) * 0.6,
                 erase=seq([knock(520, 0.02), knock(360, 0.03)]),
                 error=seq([knock(200, 0.06), knock(180, 0.08)], 0.03),
                 unit=seq([knock(nt(n, 5) / 2, 0.05) for n in 'CEG'] + [knock(nt('C', 5), 0.12)], 0.04),
                 win=seq([knock(nt(n, o) / 2, 0.06) for n, o in (('C', 5), ('E', 5), ('G', 5), ('C', 6), ('E', 6))] +
                         [knock(nt('G', 5), 0.25)], 0.05),
                 tap=noise_burst(0.03, 0.006, 3000) * 0.5,
                 hint=seq([knock(660, 0.04), knock(880, 0.08)], 0.04))
    elif s == 4:    # stone
        def thud(f, d=0.06):
            return mixs(tone(f, 0.18, d=d), noise_burst(0.12, 0.04, 900) * 0.7)
        k = dict(place=thud(150),
                 note=mixs(noise_burst(0.06, 0.015, 1800) * 0.6, tone(330, 0.05, d=0.02) * 0.3),
                 erase=noise_burst(0.15, 0.05, 1400),
                 error=thud(90, 0.12),
                 unit=seq([thud(nt(n, 3), 0.08) for n in 'CEG'] + [thud(nt('C', 4), 0.18)], 0.03),
                 win=seq([thud(nt(n, o), 0.1) for n, o in (('C', 3), ('G', 3), ('C', 4), ('E', 4))] +
                         [mixs(tone(nt('G', 4), 0.6, d=0.3), noise_burst(0.3, 0.1, 800) * 0.4)], 0.04),
                 tap=noise_burst(0.03, 0.006, 1500) * 0.5,
                 hint=seq([tone(392, 0.12, d=0.06), tone(523, 0.25, d=0.12)]))
    elif s == 5:    # water
        def bloop(f, d=0.06):
            return tone(f, 0.14, f2=f * 2.2, d=d)
        k = dict(place=bloop(380),
                 note=bloop(700, 0.03) * 0.6,
                 erase=tone(800, 0.12, f2=300, d=0.05),
                 error=seq([tone(260, 0.12, f2=200, d=0.08), tone(220, 0.16, f2=160, d=0.1)]),
                 unit=seq([bloop(nt(n, 4), 0.05) for n in 'CEG'] + [bloop(nt('C', 5), 0.12)], 0.02),
                 win=seq([bloop(nt(n, o), 0.06) for n, o in (('C', 4), ('E', 4), ('G', 4), ('C', 5))] +
                         [mixs(*[bloop(f, 0.2) for f in (nt('C', 5), nt('E', 5), nt('G', 5))])], 0.03),
                 tap=bloop(1200, 0.01) * 0.35,
                 hint=seq([bloop(500, 0.05), bloop(750, 0.1)], 0.03))
    else:           # eerie
        def wail(f, dur, f2=None, d=0.2):
            return tone(f, dur, 'sine', a=0.02, d=d, f2=f2, vib=6)
        k = dict(place=wail(520, 0.16, d=0.06),
                 note=wail(880, 0.08, d=0.03) * 0.6,
                 erase=wail(700, 0.2, 350, 0.08),
                 error=mixs(wail(220, 0.35, d=0.15), wail(233, 0.35, d=0.15)),
                 unit=seq([wail(f, 0.12, d=0.08) for f in (nt('A', 4), nt('C', 5), nt('E', 5))] +
                          [wail(nt('A', 5), 0.3, d=0.18)]),
                 win=seq([wail(f, 0.16, d=0.12) for f in (nt('A', 4), nt('E', 5), nt('A', 5), nt('C', 6))] +
                         [wail(nt('E', 6), 0.8, nt('A', 5), 0.45)]),
                 tap=wail(1400, 0.03, d=0.01) * 0.3,
                 hint=wail(600, 0.4, 1200, 0.2))
    return {n: norm(np.asarray(v, np.float64), 0.75 if n != 'tap' else 0.45) for n, v in k.items()}


# ---------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--preview', help='also write PNGs here')
    ap.add_argument('--only', help='themes, comma separated')
    ap.add_argument('--out', default=os.path.join(BUILD, 'sudoku_p4.pak'))
    args = ap.parse_args()
    only = set(args.only.split(',')) if args.only else None
    pv = args.preview
    if pv:
        os.makedirs(pv, exist_ok=True)

    pack = Pack()
    sizes = {}
    for key in THEMES:
        if only and key not in only:
            continue
        print('  ' + key, flush=True)
        bg = globals()['bg_' + key]()
        tex = tex_for(key)
        th = thumbnail(key, bg, tex)
        jb, jt, jh = jpeg(bg, 80), jpeg(tex, 86), jpeg(th, 88)
        pack.add('bg_' + key, T_JPEG, BG, BG, jb, compress=False)
        pack.add('tx_' + key, T_JPEG, TEX, TEX, jt, compress=False)
        pack.add('th_' + key, T_JPEG, THUMB, THUMB, jh, compress=False)
        for name, size in (('g9_', G9), ('g6_', G6)):
            raw = b''.join(rgb565a8(style_digit(key, d, size)) for d in range(1, 10))
            pack.add(name + key, T_SPRITE, size, size, raw, extra=9)
        spr = sprites_for(key)
        for i, s in enumerate(spr):
            pack.add('d%d_%s' % (i, key), T_SPRITE, s.shape[1], s.shape[0], rgb565a8(s), extra=1)
        sizes[key] = len(jb) + len(jt) + len(jh)
        if pv:
            Image.fromarray(np.clip(bg, 0, 255).astype(np.uint8)).save(os.path.join(pv, 'bg_%s.jpg' % key), quality=85)
            Image.fromarray(np.clip(tex, 0, 255).astype(np.uint8)).save(os.path.join(pv, 'tx_%s.jpg' % key), quality=85)
            Image.fromarray(np.clip(th, 0, 255).astype(np.uint8)).save(os.path.join(pv, 'th_%s.png' % key))
            sheet = np.zeros((G6 + 8, 9 * (G6 + 4), 4), np.float32)
            for d in range(1, 10):
                g = style_digit(key, d, G6)
                sheet[4:4 + G6, (d - 1) * (G6 + 4):(d - 1) * (G6 + 4) + G6] = g
            base = np.zeros((sheet.shape[0], sheet.shape[1], 3), np.float32) + rgb(PAL[key]['board'])
            put(base, sheet[..., :3], sheet[..., 3] / 255)
            Image.fromarray(np.clip(base, 0, 255).astype(np.uint8)).save(os.path.join(pv, 'g_%s.png' % key))
            for i, s in enumerate(spr):
                Image.fromarray(np.clip(s, 0, 255).astype(np.uint8)).save(os.path.join(pv, 'd%d_%s.png' % (i, key)))

    if not only:
        for s in range(7):
            snd = sound_set(s)
            for name in SOUNDS:
                pack.add('s%d_%s' % (s, name), T_PCM, 0, 0, mulaw(snd[name]), extra=RATE)
                if pv and s in (0, 3):
                    import wave
                    with wave.open(os.path.join(pv, 's%d_%s.wav' % (s, name)), 'wb') as w:
                        w.setnchannels(1)
                        w.setsampwidth(2)
                        w.setframerate(RATE)
                        w.writeframes((snd[name] * 32000).astype('<i2').tobytes())

    total = pack.write(args.out)
    print('%s: %d entries, %.2f MB' % (args.out, len(pack.entries), total / 1e6))
    for k, v in sizes.items():
        print('  %-10s %4d KB of JPEG' % (k, v // 1024))
    if total > 7_500_000:
        print('  ! over 7.5 MB: the HAL decodes the JPEGs from one file, keep it in one part')


if __name__ == '__main__':
    main()
