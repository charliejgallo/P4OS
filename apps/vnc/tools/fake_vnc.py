#!/usr/bin/env python3
"""A fake VNC (RFB) server, to try the VNC viewer without a real computer.

    python3 apps/vnc/tools/fake_vnc.py [--port 5999] [--password prueba]
        [--size 1920x1080] [--version 3.8|3.7|3.3|3.889] [--apple]
        [--resize-every 20] [--fps 15] [--no-jpeg]

It draws a desktop that changes on its own: a wallpaper made of gradients
(the photo-like part, sent as JPEG by Tight), a menu bar with a clock, a
window whose text scrolls a line a second (sent with CopyRect), a ball
bouncing across, a ring where the viewer's pointer is, a dot where it
clicked, and the keys it typed in a window of their own. Every pointer
and key event is printed too.

What it speaks: RFB 3.3, 3.7 and 3.8 (or Apple's 3.889 with --apple, which
also offers Apple's own security types 30 and 35, and VNC only with a
password: like a Mac with "VNC viewers may control screen with password");
security None, or VNC with --password (a TEST password: never a real one);
any true-colour pixel format the client asks for; the encodings Raw,
CopyRect, Hextile, ZRLE and Tight (fill, palette, gradient every few
frames, zlib and JPEG), in the client's order of preference; the
pseudo-encodings DesktopSize (--resize-every switches between two sizes)
and LastRect. Ctrl+G rings the bell.

Needs Pillow and numpy. Listens on 127.0.0.1 unless --host says otherwise.
"""
import argparse
import io
import math
import os
import select
import socket
import struct
import sys
import threading
import time
import zlib

import numpy as np
from PIL import Image, ImageDraw, ImageFont

# ---- DES (FIPS 46-3), for the VNC password ---------------------------------

IP = [58, 50, 42, 34, 26, 18, 10, 2, 60, 52, 44, 36, 28, 20, 12, 4,
      62, 54, 46, 38, 30, 22, 14, 6, 64, 56, 48, 40, 32, 24, 16, 8,
      57, 49, 41, 33, 25, 17, 9, 1, 59, 51, 43, 35, 27, 19, 11, 3,
      61, 53, 45, 37, 29, 21, 13, 5, 63, 55, 47, 39, 31, 23, 15, 7]
FP = [40, 8, 48, 16, 56, 24, 64, 32, 39, 7, 47, 15, 55, 23, 63, 31,
      38, 6, 46, 14, 54, 22, 62, 30, 37, 5, 45, 13, 53, 21, 61, 29,
      36, 4, 44, 12, 52, 20, 60, 28, 35, 3, 43, 11, 51, 19, 59, 27,
      34, 2, 42, 10, 50, 18, 58, 26, 33, 1, 41, 9, 49, 17, 57, 25]
E = [32, 1, 2, 3, 4, 5, 4, 5, 6, 7, 8, 9, 8, 9, 10, 11, 12, 13, 12, 13, 14, 15, 16, 17,
     16, 17, 18, 19, 20, 21, 20, 21, 22, 23, 24, 25, 24, 25, 26, 27, 28, 29, 28, 29, 30, 31, 32, 1]
P = [16, 7, 20, 21, 29, 12, 28, 17, 1, 15, 23, 26, 5, 18, 31, 10,
     2, 8, 24, 14, 32, 27, 3, 9, 19, 13, 30, 6, 22, 11, 4, 25]
PC1 = [57, 49, 41, 33, 25, 17, 9, 1, 58, 50, 42, 34, 26, 18, 10, 2, 59, 51, 43, 35, 27,
       19, 11, 3, 60, 52, 44, 36, 63, 55, 47, 39, 31, 23, 15, 7, 62, 54, 46, 38, 30, 22,
       14, 6, 61, 53, 45, 37, 29, 21, 13, 5, 28, 20, 12, 4]
PC2 = [14, 17, 11, 24, 1, 5, 3, 28, 15, 6, 21, 10, 23, 19, 12, 4, 26, 8, 16, 7, 27, 20, 13, 2,
       41, 52, 31, 37, 47, 55, 30, 40, 51, 45, 33, 48, 44, 49, 39, 56, 34, 53, 46, 42, 50, 36, 29, 32]
SHIFTS = [1, 1, 2, 2, 2, 2, 2, 2, 1, 2, 2, 2, 2, 2, 2, 1]
SBOX = [
    [14, 4, 13, 1, 2, 15, 11, 8, 3, 10, 6, 12, 5, 9, 0, 7, 0, 15, 7, 4, 14, 2, 13, 1, 10, 6, 12, 11, 9, 5, 3, 8,
     4, 1, 14, 8, 13, 6, 2, 11, 15, 12, 9, 7, 3, 10, 5, 0, 15, 12, 8, 2, 4, 9, 1, 7, 5, 11, 3, 14, 10, 0, 6, 13],
    [15, 1, 8, 14, 6, 11, 3, 4, 9, 7, 2, 13, 12, 0, 5, 10, 3, 13, 4, 7, 15, 2, 8, 14, 12, 0, 1, 10, 6, 9, 11, 5,
     0, 14, 7, 11, 10, 4, 13, 1, 5, 8, 12, 6, 9, 3, 2, 15, 13, 8, 10, 1, 3, 15, 4, 2, 11, 6, 7, 12, 0, 5, 14, 9],
    [10, 0, 9, 14, 6, 3, 15, 5, 1, 13, 12, 7, 11, 4, 2, 8, 13, 7, 0, 9, 3, 4, 6, 10, 2, 8, 5, 14, 12, 11, 15, 1,
     13, 6, 4, 9, 8, 15, 3, 0, 11, 1, 2, 12, 5, 10, 14, 7, 1, 10, 13, 0, 6, 9, 8, 7, 4, 15, 14, 3, 11, 5, 2, 12],
    [7, 13, 14, 3, 0, 6, 9, 10, 1, 2, 8, 5, 11, 12, 4, 15, 13, 8, 11, 5, 6, 15, 0, 3, 4, 7, 2, 12, 1, 10, 14, 9,
     10, 6, 9, 0, 12, 11, 7, 13, 15, 1, 3, 14, 5, 2, 8, 4, 3, 15, 0, 6, 10, 1, 13, 8, 9, 4, 5, 11, 12, 7, 2, 14],
    [2, 12, 4, 1, 7, 10, 11, 6, 8, 5, 3, 15, 13, 0, 14, 9, 14, 11, 2, 12, 4, 7, 13, 1, 5, 0, 15, 10, 3, 9, 8, 6,
     4, 2, 1, 11, 10, 13, 7, 8, 15, 9, 12, 5, 6, 3, 0, 14, 11, 8, 12, 7, 1, 14, 2, 13, 6, 15, 0, 9, 10, 4, 5, 3],
    [12, 1, 10, 15, 9, 2, 6, 8, 0, 13, 3, 4, 14, 7, 5, 11, 10, 15, 4, 2, 7, 12, 9, 5, 6, 1, 13, 14, 0, 11, 3, 8,
     9, 14, 15, 5, 2, 8, 12, 3, 7, 0, 4, 10, 1, 13, 11, 6, 4, 3, 2, 12, 9, 5, 15, 10, 11, 14, 1, 7, 6, 0, 8, 13],
    [4, 11, 2, 14, 15, 0, 8, 13, 3, 12, 9, 7, 5, 10, 6, 1, 13, 0, 11, 7, 4, 9, 1, 10, 14, 3, 5, 12, 2, 15, 8, 6,
     1, 4, 11, 13, 12, 3, 7, 14, 10, 15, 6, 8, 0, 5, 9, 2, 6, 11, 13, 8, 1, 4, 10, 7, 9, 5, 0, 15, 14, 2, 3, 12],
    [13, 2, 8, 4, 6, 15, 11, 1, 10, 9, 3, 14, 5, 0, 12, 7, 1, 15, 13, 8, 10, 3, 7, 4, 12, 5, 6, 11, 0, 14, 9, 2,
     7, 11, 4, 1, 9, 12, 14, 2, 0, 6, 10, 13, 15, 3, 5, 8, 2, 1, 14, 7, 4, 10, 8, 13, 15, 12, 9, 0, 3, 5, 6, 11],
]


def _bits(data):
    return [(data[i // 8] >> (7 - i % 8)) & 1 for i in range(len(data) * 8)]


def _bytes(bits):
    return bytes(int(''.join(map(str, bits[i:i + 8])), 2) for i in range(0, len(bits), 8))


def des_block(key, block):
    k = _bits(key)
    cd = [k[i - 1] for i in PC1]
    subs = []
    for s in SHIFTS:
        cd = cd[s:28] + cd[:s] + cd[28 + s:] + cd[28:28 + s]
        subs.append([cd[i - 1] for i in PC2])
    b = _bits(block)
    lr = [b[i - 1] for i in IP]
    l, r = lr[:32], lr[32:]
    for sub in subs:
        e = [r[i - 1] ^ sub[n] for n, i in enumerate(E)]
        out = []
        for j in range(8):
            x = e[j * 6:j * 6 + 6]
            v = SBOX[j][(x[0] << 1 | x[5]) * 16 + (x[1] << 3 | x[2] << 2 | x[3] << 1 | x[4])]
            out += [(v >> (3 - n)) & 1 for n in range(4)]
        f = [out[i - 1] for i in P]
        l, r = r, [a ^ c for a, c in zip(l, f)]
    pre = r + l
    return _bytes([pre[i - 1] for i in FP])


def vnc_response(password, challenge):
    key = bytes(int('{:08b}'.format(c)[::-1], 2) for c in password.encode('latin-1')[:8].ljust(8, b'\0'))
    return des_block(key, challenge[:8]) + des_block(key, challenge[8:])


# ---- the desktop -----------------------------------------------------------

KEYSYM_NAMES = {
    0xff08: 'BackSpace', 0xff09: 'Tab', 0xff0d: 'Return', 0xff1b: 'Escape', 0xff50: 'Home',
    0xff51: 'Left', 0xff52: 'Up', 0xff53: 'Right', 0xff54: 'Down', 0xff55: 'Page_Up',
    0xff56: 'Page_Down', 0xff57: 'End', 0xff63: 'Insert', 0xffe1: 'Shift_L', 0xffe3: 'Control_L',
    0xffe7: 'Meta_L', 0xffe9: 'Alt_L', 0xffeb: 'Super_L', 0xffff: 'Delete',
}
for _i in range(12):
    KEYSYM_NAMES[0xffbe + _i] = 'F%d' % (_i + 1)


def keysym_name(k):
    if k in KEYSYM_NAMES:
        return KEYSYM_NAMES[k]
    if 0x20 <= k < 0x7f or 0xa0 <= k <= 0xff:
        return repr(chr(k))
    if k & 0xff000000 == 0x01000000:
        return repr(chr(k & 0xffffff))
    return hex(k)


def font(size):
    for path in ('/System/Library/Fonts/Supplemental/Arial.ttf', '/Library/Fonts/Arial.ttf',
                 '/System/Library/Fonts/Helvetica.ttc', '/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf'):
        if os.path.exists(path):
            try:
                return ImageFont.truetype(path, size)
            except OSError:
                pass
    return ImageFont.load_default()


class Desktop:
    """The shared picture; each client keeps its own copy of what it has."""

    def __init__(self, w, h, title):
        self.lock = threading.Lock()
        self.title = title
        self.t0 = time.time()
        self.pointer = None             # last pointer position, any client
        self.clicks = []                # (x, y, button) recent
        self.typed = ''
        self.scroll_n = 0
        self.log = ['P4OS fake VNC server', 'listening', '']
        self.resize(w, h)

    def resize(self, w, h):
        self.w, self.h = w, h
        yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
        r = 90 + 70 * np.sin(xx / 97.0) * np.cos(yy / 131.0)
        g = 110 + 60 * np.sin((xx + yy) / 173.0)
        b = 170 + 60 * np.cos(xx / 211.0 - yy / 89.0)
        self.wall = np.clip(np.stack([r, g, b], axis=2), 0, 255).astype(np.uint8)
        self.font_s, self.font_m, self.font_l = font(max(14, h // 60)), font(max(18, h // 45)), font(max(28, h // 18))
        self.term = (int(w * 0.05), int(h * 0.12), int(w * 0.42), int(h * 0.40))   # x, y, w, h
        self.type_win = (int(w * 0.52), int(h * 0.12), int(w * 0.43), int(h * 0.22))
        self.line_h = max(18, h // 45) + 6
        self.scroll_due = False

    def tick(self):
        """Advances the clock; returns the terminal's scroll in pixels (0: none)."""
        n = int(time.time() - self.t0)
        if n != self.scroll_n:
            self.scroll_n = n
            self.log.append('%05d  tick %s' % (n, time.strftime('%H:%M:%S')))
            self.log = self.log[-200:]
            return self.line_h
        return 0

    def render(self):
        w, h = self.w, self.h
        img = Image.fromarray(self.wall.copy(), 'RGB')
        d = ImageDraw.Draw(img)
        t = time.time() - self.t0
        # menu bar
        bar = max(28, h // 36)
        d.rectangle([0, 0, w, bar], fill=(236, 236, 240))
        d.text((16, bar // 6), 'P4OS  Archivo  Editar  Ver  Ventana', fill=(20, 20, 20), font=self.font_s)
        d.text((w - 16 - 260, bar // 6), time.strftime('%a %d %b  %H:%M:%S'), fill=(20, 20, 20), font=self.font_s)
        # the terminal (its text scrolls a line a second)
        x, y, tw, th = self.term
        d.rounded_rectangle([x, y, x + tw, y + th], radius=10, fill=(24, 24, 28), outline=(90, 90, 96))
        d.rectangle([x, y, x + tw, y + 30], fill=(58, 58, 64))
        d.text((x + 12, y + 6), 'Terminal', fill=(230, 230, 230), font=self.font_s)
        lines = (th - 40) // self.line_h
        for i, line in enumerate(self.log[-lines:]):
            d.text((x + 12, y + 36 + i * self.line_h), line, fill=(120, 230, 120), font=self.font_m)
        # what the viewer typed
        x, y, tw, th = self.type_win
        d.rounded_rectangle([x, y, x + tw, y + th], radius=10, fill=(250, 250, 250), outline=(160, 160, 170))
        d.rectangle([x, y, x + tw, y + 30], fill=(210, 214, 222))
        d.text((x + 12, y + 6), 'Teclado', fill=(30, 30, 30), font=self.font_s)
        d.text((x + 14, y + 44), self.typed[-60:] + '|', fill=(10, 10, 10), font=self.font_l)
        # the title, a few flat "icons"
        d.text((int(w * 0.52), int(h * 0.40)), self.title, fill=(255, 255, 255), font=self.font_l)
        for i, c in enumerate([(255, 69, 58), (255, 159, 10), (48, 209, 88), (10, 132, 255), (191, 90, 242)]):
            cx = int(w * 0.55) + i * int(w * 0.08)
            d.rounded_rectangle([cx, int(h * 0.55), cx + int(w * 0.06), int(h * 0.55) + int(w * 0.06)],
                                radius=14, fill=c)
        # a ball bouncing
        bx = (math.sin(t * 0.9) * 0.45 + 0.5) * (w - 80)
        by = (abs(math.sin(t * 1.7)) * -0.35 + 0.88) * (h - 80)
        d.ellipse([bx, by, bx + 80, by + 80], fill=(255, 214, 10), outline=(120, 90, 0), width=3)
        # the clicks and the pointer
        for (cx, cy, btn) in self.clicks[-20:]:
            col = (255, 55, 95) if btn & 4 else (0, 200, 255) if btn & 2 else (255, 255, 255)
            d.ellipse([cx - 9, cy - 9, cx + 9, cy + 9], fill=col, outline=(0, 0, 0))
        if self.pointer:
            px, py = self.pointer
            d.ellipse([px - 16, py - 16, px + 16, py + 16], outline=(255, 0, 0), width=3)
            d.line([px - 24, py, px + 24, py], fill=(255, 0, 0), width=1)
            d.line([px, py - 24, px, py + 24], fill=(255, 0, 0), width=1)
        return np.asarray(img, dtype=np.uint8).copy()


# ---- pixels in the client's format -------------------------------------------

class PixelFormat:
    def __init__(self, raw):
        (self.bpp, self.depth, self.be, self.tc, self.rmax, self.gmax, self.bmax,
         self.rs, self.gs, self.bs) = struct.unpack('>BBBBHHHBBB3x', raw)

    def pack(self, rgb):
        """rgb: (..., 3) uint8 -> (..., bpp/8) uint8 in the client's order"""
        r = (rgb[..., 0].astype(np.uint32) * self.rmax + 127) // 255
        g = (rgb[..., 1].astype(np.uint32) * self.gmax + 127) // 255
        b = (rgb[..., 2].astype(np.uint32) * self.bmax + 127) // 255
        v = (r << self.rs) | (g << self.gs) | (b << self.bs)
        n = self.bpp // 8
        dt = {1: np.uint8, 2: np.dtype('>u2') if self.be else np.dtype('<u2'),
              4: np.dtype('>u4') if self.be else np.dtype('<u4')}[n]
        return np.frombuffer(np.ascontiguousarray(v.astype(dt)).tobytes(), np.uint8).reshape(rgb.shape[:-1] + (n,))

    def cpixel_bytes(self):
        """ZRLE's CPIXEL: 3 bytes when a 32-bit pixel's colour fits in 3 of them"""
        if self.bpp == 32 and self.depth <= 24 and self.tc:
            top = max(self.rs + self.rmax.bit_length(), self.gs + self.gmax.bit_length(),
                      self.bs + self.bmax.bit_length())
            low = min(self.rs, self.gs, self.bs)
            if top <= 24:
                return 3, 'low'
            if low >= 8:
                return 3, 'high'
        return self.bpp // 8, None

    def cpack(self, rgb):
        n, which = self.cpixel_bytes()
        p = self.pack(rgb)
        if which is None:
            return p
        lsb_first = not self.be
        if which == 'low':
            return p[..., 0:3] if lsb_first else p[..., 1:4]
        return p[..., 1:4] if lsb_first else p[..., 0:3]

    def tight24(self):
        return (self.bpp == 32 and self.depth == 24 and self.rmax == 255 and self.gmax == 255
                and self.bmax == 255)

    def tpack(self, rgb):
        if self.tight24():
            return rgb.astype(np.uint8)
        return self.pack(rgb)


# ---- the encodings -------------------------------------------------------------

ENC_RAW, ENC_COPYRECT, ENC_HEXTILE, ENC_TIGHT, ENC_ZRLE = 0, 1, 5, 7, 16
ENC_DESKTOPSIZE, ENC_LASTRECT = -223, -224


def rect_header(x, y, w, h, enc):
    return struct.pack('>HHHHi', x, y, w, h, enc)


def colours(tile):
    flat = tile.reshape(-1, 3)
    packed = (flat[:, 0].astype(np.uint32) << 16) | (flat[:, 1].astype(np.uint32) << 8) | flat[:, 2]
    uniq, inv = np.unique(packed, return_inverse=True)
    pal = np.stack([(uniq >> 16) & 255, (uniq >> 8) & 255, uniq & 255], axis=1).astype(np.uint8)
    return pal, inv.reshape(tile.shape[:2])


def enc_raw(pf, tile):
    return pf.pack(tile).tobytes()


def enc_hextile(pf, tile):
    h, w = tile.shape[:2]
    out = bytearray()
    for ty in range(0, h, 16):
        for tx in range(0, w, 16):
            t = tile[ty:ty + 16, tx:tx + 16]
            pal, idx = colours(t)
            th, tw = t.shape[:2]
            if len(pal) == 1:
                out += bytes([2]) + pf.pack(pal[0]).tobytes()
                continue
            if len(pal) > 6:
                out += bytes([1]) + pf.pack(t).tobytes()
                continue
            # background = the commonest colour; coloured subrects, one per run of a row
            counts = np.bincount(idx.ravel())
            bg = int(counts.argmax())
            subs = bytearray()
            n = 0
            for yy in range(th):
                xx = 0
                while xx < tw:
                    c = idx[yy, xx]
                    if c == bg:
                        xx += 1
                        continue
                    x0 = xx
                    while xx < tw and idx[yy, xx] == c:
                        xx += 1
                    subs += pf.pack(pal[c]).tobytes() + bytes([(x0 << 4) | yy, ((xx - x0 - 1) << 4)])
                    n += 1
            if n > 255 or len(subs) > tw * th * pf.bpp // 8:
                out += bytes([1]) + pf.pack(t).tobytes()
                continue
            out += bytes([2 | 8 | 16]) + pf.pack(pal[bg]).tobytes() + bytes([n]) + subs
    return bytes(out)


def runs_of(flat):
    """[(value, length)] of a 1-D array"""
    if len(flat) == 0:
        return []
    change = np.flatnonzero(np.diff(flat)) + 1
    starts = np.concatenate([[0], change])
    ends = np.concatenate([change, [len(flat)]])
    return [(flat[s], e - s) for s, e in zip(starts, ends)]


def run_len(n):
    n -= 1
    return bytes([255] * (n // 255) + [n % 255])


def enc_zrle_tile(pf, t, frame_no):
    pal, idx = colours(t)
    th, tw = t.shape[:2]
    if len(pal) == 1:
        return bytes([1]) + pf.cpack(pal[0]).tobytes()
    cp = pf.cpack(pal)
    flat = idx.ravel()
    runs = runs_of(flat)
    if len(pal) <= 16 and len(runs) > tw * th // 4:
        # packed palette
        bits = 1 if len(pal) == 2 else 2 if len(pal) <= 4 else 4
        rows = bytearray()
        for yy in range(th):
            acc, nb = 0, 0
            row = bytearray()
            for xx in range(tw):
                acc = (acc << bits) | int(idx[yy, xx])
                nb += bits
                if nb == 8:
                    row.append(acc)
                    acc, nb = 0, 0
            if nb:
                row.append(acc << (8 - nb))
            rows += row
        return bytes([len(pal)]) + cp.tobytes() + bytes(rows)
    if len(pal) <= 127:
        out = bytearray([128 + len(pal)]) + cp.tobytes()
        for v, n in runs:
            if n == 1:
                out.append(int(v))
            else:
                out.append(128 | int(v))
                out += run_len(n)
        return bytes(out)
    if len(runs) < tw * th // 3:
        out = bytearray([128])
        cps = pf.cpack(t).reshape(-1, pf.cpixel_bytes()[0])
        pos = 0
        for v, n in runs:
            out += cps[pos].tobytes() + run_len(n)
            pos += n
        return bytes(out)
    return bytes([0]) + pf.cpack(t).tobytes()


def tight_len(n):
    out = bytearray([n & 0x7f])
    if n > 0x7f:
        out[0] |= 0x80
        out.append((n >> 7) & 0x7f)
        if n > 0x3fff:
            out[1] |= 0x80
            out.append(n >> 14)
    return bytes(out)


class Client:
    def __init__(self, conn, addr, desk, args):
        self.c, self.addr, self.desk, self.args = conn, addr, desk, args
        self.buf = b''
        self.pf = None
        self.encs = []
        self.want = None            # (incremental, x, y, w, h)
        self.have = None            # the client's picture, as we sent it
        self.zrle = zlib.compressobj(6)
        self.tz = [zlib.compressobj(6) for _ in range(4)]
        self.buttons = 0
        self.mods = set()
        self.frame_no = 0
        self.size = (desk.w, desk.h)
        self.log_t = 0
        self.bytes_out = 0

    # -- socket helpers
    def recv_exact(self, n):
        while len(self.buf) < n:
            d = self.c.recv(65536)
            if not d:
                raise ConnectionError('closed')
            self.buf += d
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def send(self, data):
        self.c.sendall(data)
        self.bytes_out += len(data)

    def say(self, *a):
        print('[%s:%d]' % self.addr, *a, flush=True)

    # -- handshake
    def handshake(self):
        ver = self.args.version
        self.send(b'RFB %03d.%03d\n' % (3, {'3.3': 3, '3.7': 7, '3.8': 8, '3.889': 889}[ver]))
        cv = self.recv_exact(12)
        self.say('client says', cv.strip())
        minor = int(cv[8:11])
        pw = self.args.password
        if minor < 7:
            self.send(struct.pack('>I', 2 if pw else 1))
            chosen = 2 if pw else 1
        else:
            types = []
            if self.args.apple:
                types += [30, 35]
            if pw:
                types.append(2)
            elif not self.args.apple:
                types.append(1)
            if not types:
                self.send(bytes([0]))
                reason = b'no security type the viewer could use'
                self.send(struct.pack('>I', len(reason)) + reason)
                raise ConnectionError('no types')
            self.send(bytes([len(types)] + types))
            chosen = self.recv_exact(1)[0]
            self.say('security type', chosen)
            if chosen not in types:
                raise ConnectionError('bad type')
        if chosen == 2:
            ch = os.urandom(16)
            self.send(ch)
            resp = self.recv_exact(16)
            ok = resp == vnc_response(pw, ch)
            self.say('VNC password', 'right' if ok else 'WRONG')
            if not ok:
                self.send(struct.pack('>I', 1))
                if minor >= 8:
                    reason = b'Authentication failed'
                    self.send(struct.pack('>I', len(reason)) + reason)
                raise ConnectionError('auth')
            self.send(struct.pack('>I', 0))
        elif minor >= 8:
            self.send(struct.pack('>I', 0))
        shared = self.recv_exact(1)[0]
        name = self.desk.title.encode('utf-8')
        # the server's own format: 32-bit BGRX, as most servers start
        spf = struct.pack('>BBBBHHHBBB3x', 32, 24, 0, 1, 255, 255, 255, 16, 8, 0)
        self.pf = PixelFormat(spf)
        self.send(struct.pack('>HH', self.desk.w, self.desk.h) + spf + struct.pack('>I', len(name)) + name)
        self.say('shared', shared, 'size %dx%d' % (self.desk.w, self.desk.h))

    # -- client messages
    def read_messages(self):
        while True:
            r, _, _ = select.select([self.c], [], [], 0)
            if not r and not self.buf:
                return
            if r:
                d = self.c.recv(65536)
                if not d:
                    raise ConnectionError('closed')
                self.buf += d
            if not self.handle_one():
                return

    def handle_one(self):
        if not self.buf:
            return False
        t = self.buf[0]
        need = {0: 20, 2: 4, 3: 10, 4: 8, 5: 6, 6: 8}.get(t)
        if need is None:
            raise ConnectionError('unknown message %d' % t)
        if len(self.buf) < need:
            return False
        if t == 2:
            n = struct.unpack('>H', self.buf[2:4])[0]
            need = 4 + 4 * n
        elif t == 6:
            n = struct.unpack('>I', self.buf[4:8])[0]
            need = 8 + n
        if len(self.buf) < need:
            return False
        m, self.buf = self.buf[:need], self.buf[need:]
        if t == 0:
            self.pf = PixelFormat(m[4:20])
            self.say('pixel format: %d bpp, depth %d, %s-endian, max %d/%d/%d, shift %d/%d/%d' % (
                self.pf.bpp, self.pf.depth, 'big' if self.pf.be else 'little', self.pf.rmax, self.pf.gmax,
                self.pf.bmax, self.pf.rs, self.pf.gs, self.pf.bs))
            self.have = None
        elif t == 2:
            self.encs = list(struct.unpack('>%di' % n, m[4:]))
            self.say('encodings', self.encs)
        elif t == 3:
            inc, x, y, w, h = struct.unpack('>BHHHH', m[1:10])
            if not inc or self.want is None or self.want[0]:
                self.want = (inc, x, y, w, h)
        elif t == 4:
            down, key = m[1], struct.unpack('>I', m[4:8])[0]
            self.say('key %s %s' % ('down' if down else 'up  ', keysym_name(key)))
            if key in (0xffe3, 0xffe1, 0xffe9, 0xffeb, 0xffe7):
                (self.mods.add if down else self.mods.discard)(key)
            elif down:
                with self.desk.lock:
                    if key == 0xff08:
                        self.desk.typed = self.desk.typed[:-1]
                    elif key == 0xff0d:
                        self.desk.typed += ' / '
                    elif 0xffe3 in self.mods and key in (ord('g'), ord('G')):
                        self.send(bytes([2]))
                        self.say('bell')
                    elif 0x20 <= key <= 0xff or key & 0xff000000 == 0x01000000:
                        ch = chr(key & 0xffffff)
                        self.desk.typed += ('^' + ch) if 0xffe3 in self.mods else ch
                    else:
                        self.desk.typed += '<%s>' % keysym_name(key)
        elif t == 5:
            buttons, x, y = struct.unpack('>BHH', m[1:6])
            if buttons != self.buttons:
                self.say('pointer %d,%d buttons %s' % (x, y, bin(buttons)))
                pressed = buttons & ~self.buttons
                if pressed:
                    with self.desk.lock:
                        self.desk.clicks.append((x, y, pressed))
                        self.desk.clicks = self.desk.clicks[-40:]
            elif time.time() - self.log_t > 0.5:
                self.say('pointer %d,%d' % (x, y))
                self.log_t = time.time()
            self.buttons = buttons
            with self.desk.lock:
                self.desk.pointer = (x, y)
        elif t == 6:
            self.say('client cut text', m[8:][:60])
        return True

    # -- updates
    def pick_encoding(self):
        for e in self.encs:
            if e in (ENC_TIGHT, ENC_ZRLE, ENC_HEXTILE, ENC_RAW):
                return e
        return ENC_RAW

    def tight_rect(self, x, y, tile, jpeg_ok):
        pf = self.pf
        h, w = tile.shape[:2]
        pal, idx = colours(tile)
        if len(pal) == 1:
            return rect_header(x, y, w, h, ENC_TIGHT) + bytes([0x80]) + pf.tpack(pal[0]).tobytes()
        quality = next((e + 32 for e in self.encs if -32 <= e <= -23), None)
        if jpeg_ok and quality is not None and len(pal) > 64 and w >= 16 and h >= 16 and not self.args.no_jpeg:
            b = io.BytesIO()
            Image.fromarray(tile, 'RGB').save(b, 'JPEG', quality=20 + quality * 8)
            j = b.getvalue()
            return rect_header(x, y, w, h, ENC_TIGHT) + bytes([0x90]) + tight_len(len(j)) + j
        if len(pal) <= 2:
            stream, filt = 1, 1
            rowb = (w + 7) // 8
            bits = np.packbits(idx.astype(np.uint8), axis=1)[:, :rowb]
            data = bits.tobytes()
            head = bytes([0x40 | (stream << 4), filt, len(pal) - 1]) + pf.tpack(pal).tobytes()
        elif len(pal) <= 256 and len(pal) < w * h // 4:
            stream, filt = 2, 1
            data = idx.astype(np.uint8).tobytes()
            head = bytes([0x40 | (stream << 4), filt, len(pal) - 1]) + pf.tpack(pal).tobytes()
        elif self.frame_no % 4 == 3:
            # the gradient filter, now and then, so the viewer's gets used
            stream = 3
            comp = pf.tpack(tile)
            if pf.tight24():
                c = comp.astype(np.int32)
                maxes = [255, 255, 255]
            else:
                v = comp.view(np.dtype('>u2' if pf.be else '<u2')).reshape(h, w).astype(np.int32) if pf.bpp == 16 \
                    else comp.view(np.dtype('>u4' if pf.be else '<u4')).reshape(h, w).astype(np.int32)
                c = np.stack([(v >> pf.rs) & pf.rmax, (v >> pf.gs) & pf.gmax, (v >> pf.bs) & pf.bmax], axis=2)
                maxes = [pf.rmax, pf.gmax, pf.bmax]
            up = np.zeros_like(c)
            up[1:] = c[:-1]
            left = np.zeros_like(c)
            left[:, 1:] = c[:, :-1]
            ul = np.zeros_like(c)
            ul[1:, 1:] = c[:-1, :-1]
            pred = left + up - ul
            diff = np.empty_like(c)
            for k in range(3):
                p = np.clip(pred[..., k], 0, maxes[k])
                diff[..., k] = (c[..., k] - p) & maxes[k]
            if pf.tight24():
                data = diff.astype(np.uint8).tobytes()
            else:
                v = (diff[..., 0] << pf.rs) | (diff[..., 1] << pf.gs) | (diff[..., 2] << pf.bs)
                dt = np.dtype(('>' if pf.be else '<') + ('u2' if pf.bpp == 16 else 'u4'))
                data = v.astype(dt).tobytes()
            head = bytes([0x40 | (stream << 4), 2])
        else:
            stream = 0
            data = pf.tpack(tile).tobytes()
            head = bytes([stream << 4])
        if len(data) < 12:
            return rect_header(x, y, w, h, ENC_TIGHT) + head + data
        z = self.tz[stream]
        comp = z.compress(data) + z.flush(zlib.Z_SYNC_FLUSH)
        return rect_header(x, y, w, h, ENC_TIGHT) + head + tight_len(len(comp)) + comp

    def encode(self, x, y, tile, enc):
        h, w = tile.shape[:2]
        if enc == ENC_RAW:
            return [rect_header(x, y, w, h, ENC_RAW) + enc_raw(self.pf, tile)]
        if enc == ENC_HEXTILE:
            return [rect_header(x, y, w, h, ENC_HEXTILE) + enc_hextile(self.pf, tile)]
        if enc == ENC_ZRLE:
            data = bytearray()
            for ty in range(0, h, 64):
                for tx in range(0, w, 64):
                    data += enc_zrle_tile(self.pf, tile[ty:ty + 64, tx:tx + 64], self.frame_no)
            comp = self.zrle.compress(bytes(data)) + self.zrle.flush(zlib.Z_SYNC_FLUSH)
            return [rect_header(x, y, w, h, ENC_ZRLE) + struct.pack('>I', len(comp)) + comp]
        # Tight: rectangles of at most 256 x 128 (2048 wide and 64K pixels are the limits)
        out = []
        for ty in range(0, h, 128):
            for tx in range(0, w, 256):
                t = tile[ty:ty + 128, tx:tx + 256]
                out.append(self.tight_rect(x + tx, y + ty, t, True))
        return out

    def update(self, picture, scroll):
        if self.want is None:
            return
        inc, wx, wy, ww, wh = self.want
        H, W = picture.shape[:2]
        rects = []
        if (W, H) != self.size:
            if ENC_DESKTOPSIZE in self.encs:
                rects.append(rect_header(0, 0, W, H, ENC_DESKTOPSIZE))
                self.say('desktop size %dx%d' % (W, H))
            self.size = (W, H)
            self.have = None
            inc, wx, wy, ww, wh = 0, 0, 0, W, H
        enc = self.pick_encoding()
        if self.have is None or not inc or self.have.shape != picture.shape:
            self.have = np.zeros_like(picture)
            dirty = [(wx, wy, min(ww, W - wx), min(wh, H - wy))]
            # a full update in tiles: Tight's fill and JPEG get their chance
            if enc != ENC_RAW:
                dirty = [(tx, ty, min(256, W - tx), min(256, H - ty))
                         for ty in range(wy, wy + wh, 256) for tx in range(wx, wx + ww, 256)]
        else:
            if scroll and ENC_COPYRECT in self.encs:
                x, y, tw, th = self.desk.term
                top = y + 36
                hgt = th - 40 - scroll
                if hgt > 0:
                    rects.append(rect_header(x + 2, top, tw - 4, hgt, ENC_COPYRECT) +
                                 struct.pack('>HH', x + 2, top + scroll))
                    self.have[top:top + hgt, x + 2:x + tw - 2] = self.have[top + scroll:top + scroll + hgt,
                                                                           x + 2:x + tw - 2]
            # changed 64 x 64 tiles, merged along each row of tiles
            dirty = []
            T = 64
            diff = np.any(picture != self.have, axis=2)
            for ty in range(0, H, T):
                row = diff[ty:ty + T]
                cols = [tx for tx in range(0, W, T) if row[:, tx:tx + T].any()]
                i = 0
                while i < len(cols):
                    j = i
                    while j + 1 < len(cols) and cols[j + 1] == cols[j] + T:
                        j += 1
                    x0, x1 = cols[i], min(W, cols[j] + T)
                    dirty.append((x0, ty, x1 - x0, min(T, H - ty)))
                    i = j + 1
        if not dirty and not rects:
            return              # nothing changed: the request stays pending
        for (x, y, w, h) in dirty:
            tile = picture[y:y + h, x:x + w]
            rects += self.encode(x, y, tile, enc)
            self.have[y:y + h, x:x + w] = tile
        lastrect = ENC_LASTRECT in self.encs and len(rects) > 200
        head = struct.pack('>BxH', 0, 0xFFFF if lastrect else len(rects))
        body = b''.join(rects)
        if lastrect:
            body += rect_header(0, 0, 0, 0, ENC_LASTRECT)
        self.send(head + body)
        self.want = None
        self.frame_no += 1

    def run(self):
        try:
            self.handshake()
            next_t = time.time()
            while True:
                self.read_messages()
                now = time.time()
                if now >= next_t:
                    next_t = now + 1.0 / self.args.fps
                    with self.desk.lock:
                        scroll = self.desk.tick()
                        pic = self.desk.render()
                    self.update(pic, scroll)
                select.select([self.c], [], [], max(0.0, next_t - time.time()))
        except (ConnectionError, OSError) as e:
            self.say('gone:', e, '(%d KB sent)' % (self.bytes_out // 1024))
        finally:
            self.c.close()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--host', default='127.0.0.1')
    ap.add_argument('--port', type=int, default=5999)
    ap.add_argument('--password', default='', help='a TEST password (VNC auth); none if empty')
    ap.add_argument('--size', default='1920x1080')
    ap.add_argument('--size2', default='1280x800', help='the other size, with --resize-every')
    ap.add_argument('--version', default='3.8', choices=['3.3', '3.7', '3.8', '3.889'])
    ap.add_argument('--apple', action='store_true', help="Apple's 3.889 and security types 30, 35")
    ap.add_argument('--resize-every', type=float, default=0)
    ap.add_argument('--fps', type=float, default=15)
    ap.add_argument('--no-jpeg', action='store_true')
    ap.add_argument('--title', default='Escritorio de prueba')
    args = ap.parse_args()
    if args.apple:
        args.version = '3.889'
    w, h = (int(v) for v in args.size.split('x'))
    desk = Desktop(w, h, args.title)
    if args.resize_every > 0:
        sizes = [(w, h), tuple(int(v) for v in args.size2.split('x'))]

        def resizer():
            i = 0
            while True:
                time.sleep(args.resize_every)
                i ^= 1
                with desk.lock:
                    desk.resize(*sizes[i])
                print('resized to %dx%d' % sizes[i], flush=True)
        threading.Thread(target=resizer, daemon=True).start()
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind((args.host, args.port))
    s.listen(4)
    print('fake VNC on %s:%d, %dx%d, RFB %s, %s' % (args.host, args.port, w, h, args.version,
          'VNC password' if args.password else 'no password'), flush=True)
    while True:
        c, a = s.accept()
        c.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        threading.Thread(target=Client(c, a, desk, args).run, daemon=True).start()


if __name__ == '__main__':
    main()
