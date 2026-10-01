#!/usr/bin/env python3
"""
MILA on P4OS - packs the Blender renders into mila_p4.pak, the one file the
game reads from /apps on the card (main/ml_art.c has the layout). The
watch's packer (AmoledOS apps/mila/tools/pack_assets.py) with the P4's
paths:

    python3 tools/pack_p4.py [--only dir,dir] [-v]      # -> build/mila_p4.pak

The renders come from art/ (tools/blender at ML_RES 1.5, the map at
720/368; see README.md), the levels from tools/levels.py into build/levels.
Neither art/ nor build/ is in git.

The casita is rendered bigger (ML_RES 1.92: the room is the screen's 720 px
across): art/casita_hd and art/mila_hd (Mila's casita frames). When they are
there, the casita and her casita frames at 1.5 are left out (art/casita
still gives the shop's toy icons), and Mila's casita sheets are packed frame
by frame (ML_ZIP, see main/ml_art.c): the game keeps them packed in PSRAM.

    --split MB      writes the pack in parts of at most MB (the portal takes
                    8 MB per upload): mila_p4.pak, mila_p4.pak.1, ...

    python3 tools/pack_p4.py --from-watch <AmoledOS apps/mila/assets>

makes a stand-in pack from the watch's own renders, smoothed up by 1.5 (and
the map by 720/368): only to try the engine before the renders are done.

Every assets/<dir>/meta.json is read (tools/blender/SPEC.md). Sprites become
SHEETS: the frames of an animation (names ending _NN) or the variants of a
tile (_vK) go together under the base name; a sprite's shadow and glow
become <base>_sh and <base>_gl (a shadow-only sprite named <x>_sh too).
Palettes (palettes.json in a dir) become pal_<name> blobs, levels
(assets/levels/*.bin) lvl_<name> blobs, assets/levels/worlds.txt the
"worlds" blob, and each kit's furniture names a props_<kit> blob.
"""
import json
import os
import re
import struct
import subprocess
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, 'art')
LEVELS = os.path.join(ROOT, 'build', 'levels')
OUT = os.path.join(ROOT, 'build', 'mila_p4.pak')
LZ4 = os.path.join(ROOT, 'build', 'tools', 'ml_lz4blk')
NAME_LEN = 32
RES = 1.5                   # the game's art against the watch's
MAP_RES = 720.0 / 368.0     # the map's panels: the whole 720 px column
SCALE = {}                  # --from-watch: dir -> factor to resample by
PATHS = {}                  # --stand-in dir=<watch dir>: that dir from the watch, smoothed up

COL, LID, PLANE, GLOW, IMG, RGB, BLOB = 1, 2, 3, 4, 5, 6, 8
ZIP = 0x80                  # a sheet packed frame by frame (main/ml_art.c)
BPP = {COL: 4, LID: 3, PLANE: 1, GLOW: 2, IMG: 3, RGB: 2}

# The casita at its own scale: which sprites each folder gives when the
# big renders are there (name filter), and which folders are packed ZIP
CASITA_HD = ('casita_hd', 'mila_hd')
TAKE = {
    'casita': lambda n: n.startswith('icon_'),      # the shop's thumbnails
    'casita_hd': lambda n: not n.startswith('icon_'),
    'mila': lambda n: '_c_' not in n,               # her casita frames: mila_hd
    'mila_hd': lambda n: '_c_' in n,
}
ZIP_DIRS = ('mila_hd',)


def kits():
    """the kits the worlds table names (tools/levels.py wrote it)"""
    out = []
    with open(os.path.join(LEVELS, 'worlds.txt')) as fh:
        for line in fh:
            if line.startswith('kit '):
                k = line.split()[1]
                if k not in out:
                    out.append(k)
    return tuple(out)

BAYER = np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], np.int32)


def lz4(data):
    if not os.path.exists(LZ4) or os.path.getmtime(LZ4) < os.path.getmtime(os.path.join(ROOT, 'tools', 'lz4blk.c')):
        os.makedirs(os.path.dirname(LZ4), exist_ok=True)
        subprocess.check_call(['cc', '-O2', '-o', LZ4, os.path.join(ROOT, 'tools', 'lz4blk.c')])
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


def load(d, fn, mode):
    im = Image.open(os.path.join(d, fn)).convert(mode)
    k = SCALE.get(os.path.basename(d))
    if k:
        # the stand-in: ids and depths nearest (never averaged), the rest smooth
        w, h = max(1, round(im.width * k)), max(1, round(im.height * k))
        exact = fn.endswith('_id.png') or fn.endswith('_z.png')
        if mode == 'RGBA' and not exact:
            a = np.asarray(im).astype(np.float32)
            a[..., :3] *= a[..., 3:4] / 255.0
            big = [np.asarray(Image.fromarray(a[..., c]).resize((w, h), Image.BICUBIC)) for c in range(4)]
            b = np.stack(big, axis=2)
            al = np.clip(b[..., 3], 0, 255)
            rgb = b[..., :3] * 255.0 / np.maximum(al[..., None], 1)
            out = np.zeros((h, w, 4), np.uint8)
            out[..., :3] = np.clip(rgb, 0, 255)
            out[..., 3] = np.round(al)
            return out
        im = im.resize((w, h), Image.NEAREST if exact else Image.BILINEAR)
    return np.asarray(im)


def anchor(d, info):
    k = SCALE.get(os.path.basename(d), 1.0)
    return round(info.get('ax', 0) * k), round(info.get('ay', 0) * k)


def encode_frame(fmt, cover, planes, ax, ay):
    """cover: (h, w) bool of pixels that exist; planes: per-pixel byte arrays
    (h, w, bpp) uint8. Crops to cover, returns the frame's bytes."""
    ys, xs = np.nonzero(cover)
    if len(ys) == 0:
        # an empty frame: 1x1 with nothing in it
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


def frame_main(d, info):
    f = info['files']
    img = load(d, f['img'], 'RGBA')
    al = img[..., 3]
    z = load(d, f['z'], 'L') if 'z' in f else np.full(al.shape, 128, np.uint8)
    cover = al > 0
    if 'id' in f:
        ids = load(d, f['id'], 'L') // 16
        light = np.round(img[..., :3].astype(np.float32).mean(axis=2)).astype(np.uint8)
        a4 = np.clip((al.astype(np.int32) + 8) >> 4, 0, 15)
        cover &= a4 > 0
        ida = ((ids.astype(np.int32) << 4) | a4).astype(np.uint8)
        pl = np.stack([ida, light, z], axis=2)
        return LID, encode_frame(LID, cover, pl, *anchor(d, info))
    c = rgb565(img[..., :3])
    if info.get('kind') == 'map':
        # a map panel: opaque on black, every pixel, 2 bytes
        pl = np.stack([(c & 255).astype(np.uint8), (c >> 8).astype(np.uint8)], axis=2)
        return RGB, encode_frame(RGB, np.ones(al.shape, bool), pl, *anchor(d, info))
    if info.get('kind') in ('ui', 'map_stone', 'map_marker'):
        # interface art: colour and alpha for LVGL, no depth; opaque images
        # keep every pixel so a row is one copy
        pl = np.stack([(c & 255).astype(np.uint8), (c >> 8).astype(np.uint8), al], axis=2)
        return IMG, encode_frame(IMG, al > 0 if (al < 255).any() else np.ones(al.shape, bool), pl, 0, 0)
    pl = np.stack([(c & 255).astype(np.uint8), (c >> 8).astype(np.uint8), al, z], axis=2)
    return COL, encode_frame(COL, cover, pl, *anchor(d, info))


def frame_plane(d, fn, info):
    a = load(d, fn, 'L')
    return encode_frame(PLANE, a > 3, a[..., None], *anchor(d, info))


def frame_glow(d, fn, info):
    g = load(d, fn, 'RGB')
    c = rgb565(g, dither=False)
    cover = c > 0
    pl = np.stack([(c & 255).astype(np.uint8), (c >> 8).astype(np.uint8)], axis=2)
    return encode_frame(GLOW, cover, pl, *anchor(d, info))


def zip_sheet(frames, fmt):
    """ML_ZIP: every frame its own LZ4 block with the data's bytes in planes
    (main/ml_art.c, weave()); stored as it is, not packed again"""
    bpp = BPP[fmt]
    heads, blocks = bytearray(), bytearray()
    for fr in frames:
        w, h, ax, ay, dl = struct.unpack('<hhhhI', fr[:12])
        hdr = 12 + h * 8
        data = fr[hdr:hdr + dl]
        n = len(data) // bpp
        planar = b''.join(data[j:n * bpp:bpp] for j in range(bpp)) + data[n * bpp:]
        c = lz4(fr[:hdr] + planar)
        heads += struct.pack('<hhhhII', w, h, ax, ay, hdr + dl, len(c))
        blocks += c
        while len(blocks) & 3:
            blocks.append(0)
    return bytes(heads + blocks)


def split_name(name):
    """(base, index) for sheet grouping; the shop's turntable frames stay one
    entry each (the shop loads only the one it shows)"""
    if '_turn_' in name:
        return name, 0
    m = re.match(r'^(.*)_(\d\d)$', name)
    if m:
        return m.group(1), int(m.group(2))
    m = re.match(r'^(.*)_v(\d)$', name)
    if m:
        return m.group(1), int(m.group(2))
    return name, 0


def main():
    global ASSETS
    only = []
    verbose = '-v' in sys.argv
    if '--from-watch' in sys.argv:
        ASSETS = os.path.abspath(sys.argv[sys.argv.index('--from-watch') + 1])
        for dn in os.listdir(ASSETS):
            SCALE[dn] = MAP_RES if dn == 'map' else RES
        print('a stand-in pack from the watch\'s renders in %s, smoothed up' % ASSETS)
    elif '--art' in sys.argv:
        ASSETS = os.path.abspath(sys.argv[sys.argv.index('--art') + 1])
    for i, v in enumerate(sys.argv):
        if v == '--stand-in' and i + 1 < len(sys.argv):
            dn, path = sys.argv[i + 1].split('=', 1)
            PATHS[dn] = os.path.abspath(path)
            SCALE[dn] = MAP_RES if dn == 'map' else RES
            print('%s: a stand-in from %s, smoothed up' % (dn, path))
    # the levels first: assets/levels/ is generated (and not in git), and a
    # level with problems stops the pack
    r = subprocess.run([sys.executable, os.path.join(ROOT, 'tools', 'levels.py')],
                       capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit('levels.py found problems:\n' + r.stdout)
    if '--only' in sys.argv:
        only = sys.argv[sys.argv.index('--only') + 1].split(',')
    dirs = sorted(set(d for d in os.listdir(ASSETS) if os.path.isfile(os.path.join(ASSETS, d, 'meta.json'))
                      and not d.startswith('_') and d != 'levels') | set(PATHS))
    if only:
        dirs = [d for d in dirs if d in only]
    hd = all(d in dirs for d in CASITA_HD)
    if not hd and any(d in dirs for d in CASITA_HD):
        sys.exit('art/casita_hd and art/mila_hd go together: render both (README.md)')
    if not hd:
        print('no art/casita_hd, art/mila_hd: the casita at 1.5 (the game wants the big one)')
    zipped = set()
    sheets = {}        # name -> [fmt, ms, {index: bytes}]
    props = {}         # kit -> furniture names
    KITS = kits()
    blobs = {}
    total_raw = 0

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

    for dn in dirs:
        d = PATHS.get(dn, os.path.join(ASSETS, dn))
        with open(os.path.join(d, 'meta.json')) as fh:
            meta = json.load(fh)
        n = 0
        take = TAKE.get(dn) if hd else None
        for name, info in sorted(meta.items()):
            if name.startswith('_') or not isinstance(info, dict) or 'files' not in info:
                continue
            if take and not take(name):
                continue
            f = info['files']
            if 'img' not in f:
                if 'sh' in f and (name.endswith('_sh') or '_sh_' in name):
                    # a shadow rendered on its own (a pushable thing's: x_v0_sh;
                    # a gate frame's: x_sh_02)
                    base, idx = split_name(name[:-3] if name.endswith('_sh') else name.replace('_sh_', '_', 1))
                    add(base + '_sh', PLANE, idx, frame_plane(d, f['sh'], info), int(info.get('ms', 0) or 0))
                continue
            base, idx = split_name(name)
            ms = int(info.get('ms', 0) or 0)
            if dn in ZIP_DIRS:
                zipped.update((base, base + '_sh'))
            fmt, data = frame_main(d, info)
            if info.get('kind') == 'ui' and fmt == COL:
                pass
            add(base, fmt, idx, data, ms)
            if 'nodes' in info or 'exit' in info:
                # a map panel's places: "nodes x,y x,y\nentry x,y\nexit x,y\ndoor x,y\nhouse x0,y0,x1,y1"
                k = SCALE.get(dn, 1.0)
                lines = ['nodes ' + ' '.join('%d,%d' % (round(x * k), round(y * k)) for x, y in info.get('nodes', []))]
                for key in ('entry', 'exit', 'door', 'house'):
                    if info.get(key):
                        lines.append(key + ' ' + ','.join(str(int(round(v * k))) for v in info[key]))
                blobs[base + '_nodes'] = ('\n'.join(lines)).encode() + b'\0'
            if base.startswith(tuple(k + '_prop' for k in KITS)):
                kit = base.split('_prop')[0]
                props.setdefault(kit, set()).add(base[len(kit) + 1:])
            if 'sh' in f:
                add(base + '_sh', PLANE, idx, frame_plane(d, f['sh'], info), ms)
            if 'gl' in f:
                add(base + '_gl', GLOW, idx, frame_glow(d, f['gl'], info), ms)
            n += 1
        pal = os.path.join(d, 'palettes.json')
        if os.path.exists(pal):
            with open(pal) as fh:
                pals = json.load(fh)

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
                            # {"zombie": {"office": {...}, ...}} -> pal_zombie_0, _1...
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
        print('%-14s %4d sprites' % (dn, n))

    lv = LEVELS
    if os.path.isdir(lv):
        for fn in sorted(os.listdir(lv)):
            if fn.endswith('.bin'):
                with open(os.path.join(lv, fn), 'rb') as fh:
                    blobs['lvl_' + fn[:-4]] = fh.read()
        with open(os.path.join(lv, 'worlds.txt'), 'rb') as fh:
            blobs['worlds'] = fh.read() + b'\0'
    for kit, names in props.items():
        one = sorted(n for n in names if n.startswith('prop_') and not n.endswith('_sh'))
        two = sorted(n for n in names if n.startswith('prop2_') and not n.endswith('_sh'))
        blobs['props_' + kit] = (' '.join(one) + '\n' + ' '.join(two)).encode() + b'\0'

    entries = []
    gaps = []
    for name, (fmt, ms, frames) in sheets.items():
        idx = sorted(frames)
        if idx != list(range(len(idx))):
            # a partial render (a style sample): the frames there are, in order
            gaps.append(name)
        if name in zipped:
            entries.append((name, fmt | ZIP, len(idx), ms, zip_sheet([frames[i] for i in idx], fmt)))
            continue
        raw = b''.join(frames[i] for i in idx)
        entries.append((name, fmt, len(idx), ms, raw))
    for name, raw in list(blobs.items()):
        if len(name) >= NAME_LEN:
            if name.startswith('pal_'):
                continue            # a sample look nobody asks for by name
            sys.exit('name too long: ' + name)
        entries.append((name, BLOB, 0, 0, raw))
    entries.sort(key=lambda e: e[0].encode())
    if gaps:
        print('%d sheets with missing frames (packed as they are): %s%s' %
              (len(gaps), ', '.join(sorted(gaps)[:8]), ' ...' if len(gaps) > 8 else ''))

    hdr_len = 8 + 48 * len(entries)
    table, data = bytearray(), bytearray()
    zbytes = 0
    for name, fmt, nf, ms, raw in entries:
        c = raw if fmt & ZIP else lz4(raw)
        if fmt & ZIP:
            zbytes += len(raw)
        total_raw += len(raw)
        table += name.encode().ljust(NAME_LEN, b'\0')
        table += struct.pack('<BBHIII', fmt, nf, ms, hdr_len + len(data), len(c), len(raw))
        data += c
        if verbose:
            print('  %-31s fmt %d x%-3d raw %7d lz4 %7d' % (name, fmt, nf, len(raw), len(c)))
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    blob = b'MHPK' + struct.pack('<HH', 1, len(entries)) + bytes(table) + bytes(data)
    part = int(float(sys.argv[sys.argv.index('--split') + 1]) * 1e6) if '--split' in sys.argv else len(blob)
    for k in range(8):
        # parts left from an earlier pack would be read as the end of this one
        pp = OUT if k == 0 else '%s.%d' % (OUT, k)
        if k > 0 and os.path.exists(pp):
            os.remove(pp)
    names = []
    for k, o in enumerate(range(0, len(blob), part)):
        if k >= 8:
            sys.exit('more than 8 parts: a bigger --split')
        pp = OUT if k == 0 else '%s.%d' % (OUT, k)
        with open(pp, 'wb') as fh:
            fh.write(blob[o:o + part])
        names.append(os.path.basename(pp))
    print('%s: %d entries, %.2f MB raw, %.2f MB on the card (%.2f MB of it frames packed apart)%s' %
          (os.path.relpath(OUT, ROOT), len(entries), total_raw / 1e6, len(blob) / 1e6, zbytes / 1e6,
           ', in %d parts: %s' % (len(names), ' '.join(names)) if len(names) > 1 else ''))


if __name__ == '__main__':
    main()
