#!/usr/bin/env python3
"""
MONSTER HOP on P4OS - packs the art into monsterhop_p4.pak, the one file the
game reads from the card (main/mh_art.c has the layout, the watch's).

    python3 tools/pack_p4.py [--only dir,dir] [-v]      # -> build/monsterhop_p4.pak

Where the art comes from (nothing of it is in this repository):

  - the sprites: the desktop's HD renders, MonsterHop/art/hd/<dir>/ (the
    Blender scenes of AmoledOS rendered at twice the watch's size, MH_RES=2),
    brought down here to 3/4: the P4 draws the art at 1.5 screen pixels per
    pixel of the watch's projection;
  - the interface's pictures (map, logo, house, emblems) and the test tiles:
    AmoledOS's apps/monsterhop/assets/ui and _test, which exist at the
    watch's size only, brought up to the P4's 720-pixel column;
  - the levels: tools/levels.py (a copy of AmoledOS's, with its maps).

    MH_ART=<dir>   the MonsterHop repository's art/ (default ../MonsterHop/art
                   beside P4OS)
    MH_AOS=<dir>   AmoledOS's apps/monsterhop (default
                   ../ESP32S3_AmoledOS/apps/monsterhop beside P4OS)

Why 1.5 and not the HD's 2: at 2 a boss level holds 19.5 MB of art in PSRAM
(the Brute alone is 7.3 MB), and next to the background cache (5.8 MB) and
two frames (3.7 MB) that is more than the board has for an app. At 1.5 the
projection's steps stay whole pixels along the ground ((90, 21) per metre
of X, (30, -63) of Y), which is what keeps neighbouring tiles seamless;
only a floor's height (34.5) is not, and every block of a floor rounds the
same way. The screen shows 480 x 853 pixels of the watch's projection
standing up and 853 x 480 lying down: the desktop's 800 x 450 view, nearly.

Everything else is AmoledOS's tools/pack_assets.py: sheets, formats, LZ4.
"""
import json
import os
import re
import struct
import subprocess
import sys

import numpy as np
from PIL import Image, ImageFilter

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)                       # apps/monsterhop
P4OS = os.path.dirname(os.path.dirname(ROOT))
CODE = os.path.dirname(P4OS)                        # where the repositories live side by side
ART = os.environ.get('MH_ART') or os.path.join(CODE, 'MonsterHop', 'art')
AOS = os.environ.get('MH_AOS') or os.path.join(CODE, 'ESP32S3_AmoledOS', 'apps', 'monsterhop')
BUILD = os.path.join(ROOT, 'build')
OUT = os.path.join(BUILD, 'monsterhop_p4.pak')
LZ4 = os.path.join(BUILD, 'lz4blk')
NAME_LEN = 32

# 3/4 of the HD renders: the P4's scale (MH_PX = 1.5 in main/mh_gfx.c)
NUM, DEN = 3, 4
# the interface's pictures: the watch's 368-pixel column to the P4's 720
UI_SCALE = 720 / 368

COL, LID, PLANE, GLOW, IMG, BLOB = 1, 2, 3, 4, 5, 8

BAYER = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], np.int32)

# Rendered but left out, as on the watch (the game falls back: a missing
# animation plays the walk, a missing facing plays the south one)
SKIP = re.compile(r'^trike_(run_|howl_[new]_|stun_[new]_)')


def lz4(data):
    src = os.path.join(TOOLS, 'lz4blk.c')
    if not os.path.exists(LZ4) or os.path.getmtime(LZ4) < os.path.getmtime(src):
        os.makedirs(BUILD, exist_ok=True)
        subprocess.check_call(['cc', '-O2', '-o', LZ4, src])
    return subprocess.run([LZ4], input=data, stdout=subprocess.PIPE, check=True).stdout


def rgb565(rgb, dither=True):
    """rgb: (h, w, 3) uint8 -> (h, w) uint16, ordered dither"""
    h, w = rgb.shape[:2]
    r = rgb[..., 0].astype(np.int32)
    g = rgb[..., 1].astype(np.int32)
    b = rgb[..., 2].astype(np.int32)
    if dither:
        d = BAYER[np.arange(h)[:, None] & 3, np.arange(w)[None, :] & 3]
        r = np.clip(r + (d >> 1) - 4, 0, 255)
        g = np.clip(g + (d >> 2) - 2, 0, 255)
        b = np.clip(b + (d >> 1) - 4, 0, 255)
    return (((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)).astype(np.uint16)


def encode_frame(fmt, cover, planes, ax, ay):
    """cover: (h, w) bool of pixels that exist; planes: per-pixel byte arrays
    (h, w, bpp) uint8. Crops to cover, returns the frame's bytes."""
    ys, xs = np.nonzero(cover)
    if len(ys) == 0:
        return struct.pack('<hhhhI', 1, 1, 0, 0, 0) + struct.pack('<HH', 0, 0) + struct.pack('<I', 0)
    y0, y1, x0, x1 = ys.min(), ys.max() + 1, xs.min(), xs.max() + 1
    cov = cover[y0:y1, x0:x1]
    pl = planes[y0:y1, x0:x1]
    h, w = cov.shape
    spans, offs, data = [], [], bytearray()
    for y in range(h):
        row = np.nonzero(cov[y])[0]
        offs.append(len(data))
        if len(row) == 0:
            spans.append((0, 0))
            continue
        a, b = int(row.min()), int(row.max()) + 1
        spans.append((a, b))
        data.extend(pl[y, a:b].tobytes())
    while len(data) & 3:
        data.append(0)
    out = struct.pack('<hhhhI', w, h, int(ax - x0), int(ay - y0), len(data))
    out += b''.join(struct.pack('<HH', a, b) for a, b in spans)
    out += b''.join(struct.pack('<I', o) for o in offs)
    return out + bytes(data)


# ---- bringing an HD frame down to 3/4 ----

class Frame:
    """One frame's planes, padded so the anchor and the size are multiples of
    DEN: then the 3/4 resize puts the anchor on a whole pixel, and every
    sprite keeps its place to the pixel."""

    def __init__(self, d, info):
        self.d = d
        self.info = info
        ax, ay = info.get('ax', 0), info.get('ay', 0)
        w0, h0 = Image.open(os.path.join(d, info['files']['img'])).size
        self.px = (-ax) % DEN
        self.py = (-ay) % DEN
        self.W = -(-(w0 + self.px) // DEN) * DEN
        self.H = -(-(h0 + self.py) // DEN) * DEN
        self.ax = (ax + self.px) * NUM // DEN
        self.ay = (ay + self.py) * NUM // DEN
        self.w = self.W * NUM // DEN
        self.h = self.H * NUM // DEN
        self._pick = None

    def raw(self, fn, mode):
        im = Image.open(os.path.join(self.d, fn)).convert(mode)
        pad = Image.new(mode, (self.W, self.H), 0)
        pad.paste(im, (self.px, self.py))
        return pad

    def smooth(self, fn, mode):
        """area-weighted: colour (premultiplied, no dark fringe), shadows, glows"""
        im = self.raw(fn, mode)
        if mode == 'RGBA':
            im = im.convert('RGBa').resize((self.w, self.h), Image.LANCZOS).convert('RGBA')
        else:
            im = im.resize((self.w, self.h), Image.LANCZOS)
        return np.asarray(im)

    def pick(self):
        """for each output pixel, the source pixel of its footprint with the
        most alpha: the region ids and the depth are taken from there, never
        averaged (an id is a name; a mean depth is a place that is not there)"""
        if self._pick is None:
            al = np.asarray(self.raw(self.info['files']['img'], 'RGBA'))[..., 3].astype(np.int32)
            u = np.arange(self.w)
            v = np.arange(self.h)
            xs = [np.minimum(u * DEN // NUM, self.W - 1), np.minimum((u * DEN + DEN - 1) // NUM, self.W - 1)]
            ys = [np.minimum(v * DEN // NUM, self.H - 1), np.minimum((v * DEN + DEN - 1) // NUM, self.H - 1)]
            best = np.full((self.h, self.w), -1, np.int32)
            by = np.zeros((self.h, self.w), np.int64)
            bx = np.zeros((self.h, self.w), np.int64)
            for yy in ys:
                for xx in xs:
                    a = al[yy[:, None], xx[None, :]]
                    m = a > best
                    best = np.where(m, a, best)
                    by = np.where(m, yy[:, None], by)
                    bx = np.where(m, xx[None, :], bx)
            self._pick = (by, bx)
        return self._pick

    def picked(self, fn):
        src = np.asarray(self.raw(fn, 'L'))
        by, bx = self.pick()
        return src[by, bx]


def frame_main(d, info):
    f = info['files']
    fr = Frame(d, info)
    img = fr.smooth(f['img'], 'RGBA')
    al = img[..., 3]
    z = fr.picked(f['z']) if 'z' in f else np.full(al.shape, 128, np.uint8)
    cover = al > 0
    if 'id' in f:
        ids = fr.picked(f['id']) // 16
        light = np.round(img[..., :3].astype(np.float32).mean(axis=2)).astype(np.uint8)
        a4 = np.clip((al.astype(np.int32) + 8) >> 4, 0, 15)
        cover &= a4 > 0
        ida = ((ids.astype(np.int32) << 4) | a4).astype(np.uint8)
        pl = np.stack([ida, light, z], axis=2)
        return LID, encode_frame(LID, cover, pl, fr.ax, fr.ay), fr
    # a pixel that exists must have a depth: the id/z of its footprint's
    # strongest source pixel is there, but a smoothed edge can reach one
    # step further, where the source was empty
    z = np.where(cover & (z == 255), np.asarray(Image.fromarray(z).filter(ImageFilter.MinFilter(3))), z)
    c = rgb565(img[..., :3])
    pl = np.stack([(c & 255).astype(np.uint8), (c >> 8).astype(np.uint8), al, z], axis=2)
    return COL, encode_frame(COL, cover, pl, fr.ax, fr.ay), fr


def frame_plane(fr, fn):
    a = fr.smooth(fn, 'L')
    return encode_frame(PLANE, a > 3, a[..., None], fr.ax, fr.ay)


def frame_glow(fr, fn):
    g = fr.smooth(fn, 'RGB')
    c = rgb565(g, dither=False)
    cover = c > 0
    pl = np.stack([(c & 255).astype(np.uint8), (c >> 8).astype(np.uint8)], axis=2)
    return encode_frame(GLOW, cover, pl, fr.ax, fr.ay)


def frame_ui(d, info, scale):
    """the interface's art for LVGL: colour and alpha, no depth, smoothed up"""
    im = Image.open(os.path.join(d, info['files']['img'])).convert('RGBA')
    if scale != 1:
        im = im.convert('RGBa').resize((round(im.width * scale), round(im.height * scale)),
                                       Image.LANCZOS).convert('RGBA')
    img = np.asarray(im)
    al = img[..., 3]
    c = rgb565(img[..., :3])
    pl = np.stack([(c & 255).astype(np.uint8), (c >> 8).astype(np.uint8), al], axis=2)
    # opaque pictures keep every pixel so a row is one copy
    return IMG, encode_frame(IMG, al > 0 if (al < 255).any() else np.ones(al.shape, bool), pl, 0, 0)


def frame_test(d, info):
    """the test field's tiles exist at the watch's size only: doubled, then
    brought down like the rest (a stand-in; nobody plays it)"""
    f = info['files']
    k = 2 * NUM / DEN
    ax, ay = round(info.get('ax', 0) * k), round(info.get('ay', 0) * k)

    def up(fn, mode, rs):
        im = Image.open(os.path.join(d, fn)).convert(mode)
        return np.asarray(im.resize((round(im.width * k), round(im.height * k)), rs))
    img = up(f['img'], 'RGBA', Image.LANCZOS)
    al = img[..., 3]
    z = up(f['z'], 'L', Image.NEAREST) if 'z' in f else np.full(al.shape, 128, np.uint8)
    cover = al > 0
    if 'id' in f:
        ids = up(f['id'], 'L', Image.NEAREST) // 16
        light = np.round(img[..., :3].astype(np.float32).mean(axis=2)).astype(np.uint8)
        a4 = np.clip((al.astype(np.int32) + 8) >> 4, 0, 15)
        cover &= a4 > 0
        pl = np.stack([((ids.astype(np.int32) << 4) | a4).astype(np.uint8), light, z], axis=2)
        return LID, encode_frame(LID, cover, pl, ax, ay)
    c = rgb565(img[..., :3])
    pl = np.stack([(c & 255).astype(np.uint8), (c >> 8).astype(np.uint8), al, z], axis=2)
    return COL, encode_frame(COL, cover, pl, ax, ay)


def split_name(name):
    """(base, index) for sheet grouping"""
    m = re.match(r'^(.*)_(\d\d)$', name)
    if m:
        return m.group(1), int(m.group(2))
    m = re.match(r'^(.*)_v(\d)$', name)
    if m:
        return m.group(1), int(m.group(2))
    return name, 0


def palettes(pals, blobs):
    def put_pal(nm, p):
        b = bytearray(48)
        for k, v in p.items():
            if str(k).isdigit() and 0 <= int(k) < 16 and isinstance(v, (list, tuple)) and len(v) >= 3:
                b[int(k) * 3:int(k) * 3 + 3] = bytes(int(c) & 255 for c in v[:3])
        blobs['pal_' + nm] = bytes(b)

    def walk(prefix, obj):
        if isinstance(obj, dict) and any(str(k).isdigit() for k in obj):
            put_pal(prefix, obj)
        elif isinstance(obj, dict):
            for k, v in obj.items():
                if k == '_variants' and isinstance(v, dict):
                    for mon, var in v.items():
                        items = list(var.values()) if isinstance(var, dict) else list(var)
                        for i, pv in enumerate(items):
                            walk('%s_%d' % (mon, i), pv)
                    continue
                if k.startswith('_'):
                    continue
                walk(prefix + '_' + k if prefix else k, v)
        elif isinstance(obj, list):
            for i, v in enumerate(obj):
                walk('%s_%d' % (prefix, i), v)
    walk('', pals)


def main():
    verbose = '-v' in sys.argv
    only = sys.argv[sys.argv.index('--only') + 1].split(',') if '--only' in sys.argv else []
    for need, what in ((os.path.join(ART, 'hd'), 'MH_ART (the MonsterHop repository\'s art/)'),
                       (os.path.join(AOS, 'assets', 'ui'), 'MH_AOS (AmoledOS\'s apps/monsterhop)')):
        if not os.path.isdir(need):
            sys.exit('%s not found: set %s' % (need, what))
    os.makedirs(BUILD, exist_ok=True)
    # the levels first; one with problems stops the pack
    lvdir = os.path.join(BUILD, 'levels')
    r = subprocess.run([sys.executable, os.path.join(TOOLS, 'levels.py'), '--check'],
                       capture_output=True, text=True, env=dict(os.environ, MH_LEVELS_OUT=lvdir))
    if r.returncode != 0:
        sys.exit('levels.py found problems:\n' + r.stdout + r.stderr)

    hd = os.path.join(ART, 'hd')
    dirs = sorted(dn for dn in os.listdir(hd) if os.path.isfile(os.path.join(hd, dn, 'meta.json')))
    dirs = [(dn, os.path.join(hd, dn), 'hd') for dn in dirs]
    for dn in ('ui', '_test'):
        dirs.append((dn, os.path.join(AOS, 'assets', dn), dn))
    if only:
        dirs = [x for x in dirs if x[0] in only]

    sheets = {}        # name -> [fmt, ms, {index: bytes}]
    blobs = {}

    def add(name, fmt, idx, data, ms=0):
        if len(name) >= NAME_LEN:
            sys.exit('name too long (%d): %s' % (len(name), name))
        s = sheets.setdefault(name, [fmt, ms, {}])
        if s[0] != fmt:
            sys.exit('%s: frames in two formats' % name)
        if idx in s[2]:
            sys.exit('%s: frame %d twice' % (name, idx))
        s[2][idx] = data
        if ms and not s[1]:
            s[1] = ms

    for dn, d, kind in dirs:
        with open(os.path.join(d, 'meta.json')) as fh:
            meta = json.load(fh)
        n = 0
        for name, info in sorted(meta.items()):
            if SKIP.match(name):
                continue
            if name.startswith('_') or not isinstance(info, dict) or 'files' not in info:
                continue
            f = info['files']
            if 'img' not in f:
                continue
            base, idx = split_name(name)
            ms = int(info.get('ms', 0) or 0)
            if kind == 'ui':
                fmt, data = frame_ui(d, info, UI_SCALE)
                fr = None
            elif kind == '_test':
                fmt, data = frame_test(d, info)
                fr = None
            else:
                fmt, data, fr = frame_main(d, info)
            add(base, fmt, idx, data, ms)
            if 'levels' in info:
                # the map's spots, scaled with the map: the levels in the
                # table's order (main/monsterhop.c: level_table), the house,
                # the two future zones
                pts = []
                for z in ('city', 'castle', 'desert', 'forest', 'dino', 'bay'):
                    lv = info['levels'].get(z, [])
                    for k in range(4):
                        pts.append(lv[k] if k < len(lv) else [0, 0])
                lk = info.get('locked', {})
                hs = info.get('house') or info['levels'].get('house') or [0, 0]
                pts += [hs, lk.get('swamp', [0, 0]), lk.get('graveyard', [0, 0])]
                blobs[base + '_spots'] = struct.pack('<H', len(pts)) + b''.join(
                    struct.pack('<hh', int(round(x * UI_SCALE)), int(round(y * UI_SCALE))) for x, y in pts)
            if fr is not None and 'sh' in f:
                add(base + '_sh', PLANE, idx, frame_plane(fr, f['sh']), ms)
            if fr is not None and 'gl' in f:
                add(base + '_gl', GLOW, idx, frame_glow(fr, f['gl']), ms)
            n += 1
        # the palettes: the HD folder's, else the watch's (the same colours)
        for pd in (d, os.path.join(AOS, 'assets', dn)):
            pal = os.path.join(pd, 'palettes.json')
            if os.path.exists(pal):
                with open(pal) as fh:
                    palettes(json.load(fh), blobs)
                break
        print('%-14s %4d sprites' % (dn, n))

    for fn in sorted(os.listdir(lvdir)):
        if fn.endswith('.bin'):
            with open(os.path.join(lvdir, fn), 'rb') as fh:
                blobs['lvl_' + fn[:-4]] = fh.read()

    entries = []
    for name, (fmt, ms, frames) in sheets.items():
        idx = sorted(frames)
        raw = b''.join(frames[i] for i in idx)
        entries.append((name, fmt, len(idx), ms, raw))
    for name, raw in list(blobs.items()):
        if len(name) >= NAME_LEN:
            if name.startswith('pal_'):
                continue            # a sample look nobody asks for by name
            sys.exit('name too long: ' + name)
        entries.append((name, BLOB, 0, 0, raw))
    entries.sort(key=lambda e: e[0].encode())

    hdr_len = 8 + 48 * len(entries)
    table, data = bytearray(), bytearray()
    total_raw = 0
    for name, fmt, nf, ms, raw in entries:
        c = lz4(raw)
        total_raw += len(raw)
        table += name.encode().ljust(NAME_LEN, b'\0')
        table += struct.pack('<BBHIII', fmt, nf, ms, hdr_len + len(data), len(c), len(raw))
        data += c
        if verbose:
            print('  %-31s fmt %d x%-3d raw %7d lz4 %7d' % (name, fmt, nf, len(raw), len(c)))
    with open(OUT, 'wb') as fh:
        fh.write(b'MHPK' + struct.pack('<HH', 1, len(entries)))
        fh.write(table)
        fh.write(data)
    print('%s: %d entries, %.2f MB unpacked, %.2f MB on the card' %
          (os.path.relpath(OUT, P4OS), len(entries), total_raw / 1e6, (hdr_len + len(data)) / 1e6))


if __name__ == '__main__':
    main()
