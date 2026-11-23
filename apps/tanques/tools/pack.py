#!/usr/bin/env python3
"""Tanques - the art pack, and contact sheets to look at the renders.

    python3 apps/tanques/tools/pack.py                 # build/art -> build/tanques_p4.pak and the DLCs'
                                                       # tanques_luna.pak, tanques_playa.pak
    python3 apps/tanques/tools/pack.py sheet top out.png [scheme]
    python3 apps/tanques/tools/pack.py tank out.png    # hull + turret together, every yaw

The pack (little-endian), read by tq_art.c:

    'TQPK' u32 version (3), u32 count
    count x { char name[24]; u32 offset; u32 size }      (offset from the file's start)
    the sprites, each 4-byte aligned:
        'TSPR' u16 w, h, i16 ax, ay, u32 n
        h x { u16 x0, u16 len, u32 off }    each row's covered run: pixels x0..x0+len-1
                                            are planes[off .. off+len-1]
        L[n] A[n] ID[n] Z[n] UV[n]           the planes (tanques_art.py), rows packed;
                                            no Z when n has bit 31 (the fp set), a UV
                                            (the badge's cell) when it has bit 30

The entries of a group (a tank's top, low and fp sprites; a tank's
turntable; a biome) are together, so the app reads each in one go.

Only the covered run of each row is kept: a barrel across a big box, or a
tank's corners, cost nothing.

Names are '<set>/<sprite>' (top/hull_07, low/block1_03, fp/barrel_04).
The colours here are the app's (tq_art.c, tq_ramp): keep them equal.
"""
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
ART = os.path.join(APP, "build", "art")
PAK = os.path.join(APP, "build", "tanques_p4.pak")
VERSION = 3

# The DLCs: what each one's pack carries (a sprite's name after its set
# starts with one of these and "_"); the rest is the game's pack. A DLC is
# installed when its pack is on the card (tq_art.c).
DLCS = {
    "tanques_luna.pak": ("tortuga", "luna", "marks_bc"),
    "tanques_playa.pak": ("anfibio", "playa", "marks_fl", "marks_ch"),
}


def pack_of(sprite):
    for pak, keys in DLCS.items():
        for k in keys:
            if sprite == k or sprite.startswith(k + "_"):
                return pak
    return os.path.basename(PAK)

FIXED = {3: (0.25, 0.24, 0.27), 4: (0.62, 0.64, 0.68), 5: (1.0, 0.92, 0.60), 6: (0.97, 0.97, 0.95),
         8: (0.40, 0.72, 0.28), 9: (0.52, 0.34, 0.20), 10: (0.70, 0.67, 0.62), 11: (0.92, 0.77, 0.52)}
SCHEMES = {
    "naranja": {1: (0.96, 0.55, 0.16), 2: (0.98, 0.88, 0.62)},
    "azul": {1: (0.28, 0.55, 0.92), 2: (0.95, 0.95, 0.98)},
    "verde": {1: (0.48, 0.70, 0.30), 2: (0.93, 0.80, 0.40)},
}


# The scenery's paints per biome (tq_art.c, BIOME_RGB): over FIXED
BIOMES = {
    "pradera": {},
    "desierto": {8: (0.37, 0.63, 0.35), 9: (0.60, 0.42, 0.26), 10: (0.88, 0.70, 0.48), 11: (0.95, 0.83, 0.61),
                 5: (1.0, 0.50, 0.66)},
    "nieve": {8: (0.25, 0.54, 0.35), 9: (0.43, 0.29, 0.20), 10: (0.66, 0.87, 0.94), 11: (0.85, 0.27, 0.23),
              7: (0.96, 0.98, 1.0), 12: (1.0, 0.54, 0.16), 3: (0.18, 0.17, 0.20)},
    "ruinas": {10: (0.72, 0.42, 0.31), 11: (0.85, 0.82, 0.77), 4: (0.42, 0.45, 0.50), 5: (1.0, 0.91, 0.63)},
    "volcan": {9: (0.24, 0.20, 0.19), 10: (0.31, 0.29, 0.32), 11: (0.54, 0.50, 0.47), 12: (1.0, 0.42, 0.12)},
    "luna": {8: (0.52, 0.88, 1.0), 9: (0.36, 0.37, 0.42), 10: (0.70, 0.70, 0.73), 11: (0.93, 0.94, 0.96)},
    "playa": {8: (0.36, 0.70, 0.30), 9: (0.66, 0.47, 0.29), 10: (0.68, 0.64, 0.58), 11: (0.92, 0.82, 0.62),
              2: (0.95, 0.32, 0.30)},
}
FIXED.update({7: (0.96, 0.97, 0.99), 12: (1.0, 0.54, 0.16)})


def ramp(c, L):
    """A region's colour at shading L (0..255): the base at L = 200, darker
    and a little cooler below, towards white above."""
    t = L / 200.0
    c = np.array(c)
    lo = np.minimum(t, 1.0)[..., None]
    col = c * lo
    col[..., 2] += 0.05 * (1 - lo[..., 0]) * lo[..., 0]          # cool shadows
    hi = np.clip(t - 1.0, 0, 1)[..., None]
    col = col + (1 - col) * hi * 1.4
    return np.clip(col, 0, 1)


def load_spr(path):
    with open(path, "rb") as f:
        return load_spr_bytes(f.read())


def load_spr_bytes(b):
    """tanques_art.py's sprite: 'TSPR' (four planes) or 'TSP5' (and the
    badge's coordinates, UV = u * 16 + v, where the emblem goes)"""
    assert b[:4] in (b"TSPR", b"TSP5")
    w, h, ax, ay = struct.unpack_from("<HHhh", b, 4)
    n = w * h
    k = 5 if b[:4] == b"TSP5" else 4
    pl = [np.frombuffer(b, np.uint8, n, 12 + i * n).reshape(h, w) for i in range(k)]
    return dict(w=w, h=h, ax=ax, ay=ay, L=pl[0], A=pl[1], ID=pl[2], Z=pl[3], UV=pl[4] if k == 5 else None, raw=b)


def colorize(s, scheme="naranja"):
    pal = dict(FIXED)
    pal.update(SCHEMES[scheme] if scheme in SCHEMES else BIOMES[scheme])
    rgb = np.zeros((s["h"], s["w"], 3))
    for rid, c in pal.items():
        m = s["ID"] == rid
        if m.any():
            rgb[m] = ramp(c, s["L"][m].astype(np.float64))
    if s.get("UV") is not None:
        # a test emblem, a star, where the player's logo goes
        m = (s["ID"] == 6)
        u, v = (s["UV"] >> 4).astype(float), (s["UV"] & 15).astype(float)
        ang = np.arctan2(v - 7.5, u - 7.5)
        rad = np.hypot(u - 7.5, v - 7.5)
        star = rad < 4.2 + 2.6 * np.cos(5 * ang)
        sm = m & star
        rgb[sm] = ramp((0.85, 0.15, 0.2), s["L"][sm].astype(np.float64))
    a = s["A"].astype(np.float64)[..., None] / 255.0
    return rgb, a


def over(canvas, s, x, y, scheme, zbuf=None, zbase=0):
    """Draws s with its anchor at (x, y), depth-tested against zbuf."""
    rgb, a = colorize(s, scheme)
    x0, y0 = int(x - s["ax"]), int(y - s["ay"])
    H, W = canvas.shape[:2]
    for j in range(s["h"]):
        yy = y0 + j
        if not 0 <= yy < H:
            continue
        for i in range(s["w"]):
            xx = x0 + i
            if not 0 <= xx < W or s["A"][j, i] == 0:
                continue
            z = zbase + int(s["Z"][j, i])
            if zbuf is not None:
                if z >= zbuf[yy, xx]:
                    continue
                if s["A"][j, i] > 128:
                    zbuf[yy, xx] = z
            al = a[j, i, 0]
            canvas[yy, xx] = canvas[yy, xx] * (1 - al) + rgb[j, i] * al


def save_png(arr, path):
    from PIL import Image
    Image.fromarray((np.clip(arr, 0, 1) * 255 + 0.5).astype(np.uint8)).save(path)


def sheet(set_name, out, scheme="naranja"):
    d = os.path.join(ART, set_name)
    names = sorted(n[:-4] for n in os.listdir(d) if n.endswith(".spr"))
    sp = [load_spr(os.path.join(d, n + ".spr")) for n in names]
    cw = max(s["w"] for s in sp) + 8
    ch = max(s["h"] for s in sp) + 8
    cols = min(8, len(sp))
    rows = (len(sp) + cols - 1) // cols
    canvas = np.zeros((rows * ch, cols * cw, 3)) + np.array((0.42, 0.55, 0.36))
    for k, s in enumerate(sp):
        cx, cy = (k % cols) * cw + cw // 2, (k // cols) * ch + ch // 2
        over(canvas, s, cx - s["w"] // 2 + s["ax"], cy - s["h"] // 2 + s["ay"], scheme)
    save_png(canvas, out)


def tank(out, set_name="top"):
    d = os.path.join(ART, set_name)
    n = len([x for x in os.listdir(d) if x.startswith("hull_")])
    cw, ch = (150, 150) if set_name == "top" else (240, 150)
    cols = 8
    rows = (n + cols - 1) // cols * len(SCHEMES)
    canvas = np.zeros((rows * ch, cols * cw, 3)) + np.array((0.45, 0.60, 0.35))
    r0 = 0
    for scheme in SCHEMES:
        for k in range(n):
            hull = load_spr(os.path.join(d, "hull_%02d.spr" % k))
            tur = load_spr(os.path.join(d, "turret_%02d.spr" % ((k + 3) % n)))
            cx, cy = (k % cols) * cw + cw // 2, (r0 + k // cols) * ch + ch * 2 // 3
            zb = np.full(canvas.shape[:2], 1 << 20, np.int32)
            over(canvas, hull, cx, cy, scheme, zb)
            over(canvas, tur, cx, cy, scheme, zb)
        r0 += (n + cols - 1) // cols
    save_png(canvas, out)


def halve(s):
    """The low set travels at half its render's size (24 px a metre): the
    first person sees tanks mostly far, and the memory is the board's.
    Coverage and shade box-filtered, the rest from the most covered pixel."""
    w, h = (s["w"] + 1) // 2, (s["h"] + 1) // 2
    pad = lambda p: np.pad(p, ((0, h * 2 - s["h"]), (0, w * 2 - s["w"])))
    A = pad(s["A"]).astype(np.int32)
    L = pad(s["L"]).astype(np.int32)
    blocks = lambda p: p.reshape(h, 2, w, 2).transpose(0, 2, 1, 3).reshape(h, w, 4)
    Ab, Lb = blocks(A), blocks(L)
    asum = Ab.sum(-1)
    out = dict(w=w, h=h, ax=s["ax"] // 2, ay=s["ay"] // 2)
    out["A"] = (asum // 4).astype(np.uint8)
    out["L"] = np.where(asum > 0, (Ab * Lb).sum(-1) // np.maximum(asum, 1), 0).astype(np.uint8)
    best = Ab.argmax(-1)
    for key in ("ID", "Z", "UV"):
        if s.get(key) is None:
            out[key] = None
            continue
        out[key] = np.take_along_axis(blocks(pad(s[key]).astype(np.int32)), best[..., None], -1)[..., 0].astype(np.uint8)
    return out


def compact(raw, with_z=True, half=False):
    """tanques_art.py's sprite (full planes) in the pack's form. Without
    its depth (the gunner's own turret, drawn last and in order), n carries
    bit 31 and there is no Z plane."""
    s = load_spr_bytes(raw)
    if half:
        s = halve(s)
    w, h = s["w"], s["h"]
    rows, planes, n, runs = b"", [b"", b"", b"", b""], 0, []
    for y in range(h):
        xs = np.nonzero(s["A"][y])[0]
        x0, x1 = (int(xs[0]), int(xs[-1]) + 1) if len(xs) else (0, 0)
        runs.append((x0, x1 - x0))
        rows += struct.pack("<HHI", x0, x1 - x0, n)
        for k, key in enumerate(("L", "A", "ID", "Z")):
            planes[k] += s[key][y, x0:x1].tobytes()
        n += x1 - x0
    uv = s.get("UV") is not None
    if uv:
        planes.append(b"".join(s["UV"][y, r[0]:r[0] + r[1]].tobytes() for y, r in enumerate(runs)))
    if not with_z:
        del planes[3]
    flags = (0 if with_z else 1 << 31) | (1 << 30 if uv else 0)
    return b"TSPR" + struct.pack("<HHhhI", w, h, s["ax"], s["ay"], n | flags) + rows + b"".join(planes)


def group_of(set_name, sprite):
    """What the app loads together: a tank (its top, low and fp sprites),
    a tank's turntable, a biome"""
    key = sprite.split("_")[0]
    return ("show:" if set_name == "show" else "") + key


def pack():
    for name in [os.path.basename(PAK)] + list(DLCS):
        pack_one(name)


def pack_one(pak_name):
    entries = []
    for set_name in sorted(os.listdir(ART)):
        d = os.path.join(ART, set_name)
        if not os.path.isdir(d) or set_name == "mesh":
            continue
        for n in sorted(os.listdir(d)):
            if n.endswith(".spr"):
                if pack_of(n[:-4]) != pak_name:
                    continue
                name = "%s/%s" % (set_name, n[:-4])
                assert len(name) < 24, name
                with open(os.path.join(d, n), "rb") as f:
                    # no depth where the sprite is drawn alone or over the
                    # ground only (the tanks from above: their anchor's
                    # depth is enough); the low set at half size
                    tank_top = set_name == "top" and ("_h_" in n or "_t_" in n)
                    with_z = set_name not in ("fp", "show") and not tank_top
                    entries.append((group_of(set_name, n[:-4]), name,
                                    compact(f.read(), with_z, half=set_name == "low")))
    entries.sort(key=lambda e: (e[0], e[1]))
    off = 12 + 32 * len(entries)
    table, blobs = b"", b""
    for _, name, b in entries:
        table += struct.pack("<24sII", name.encode(), off + len(blobs), len(b))
        blobs += b
        while len(blobs) % 4:
            blobs += b"\0"
    out = os.path.join(os.path.dirname(PAK), pak_name)
    with open(out, "wb") as f:
        f.write(b"TQPK" + struct.pack("<II", VERSION, len(entries)) + table + blobs)
    groups = {}
    for g, _, b in entries:
        groups[g] = groups.get(g, 0) + len(b)
    print("%s: %d sprites, %.1f MB" % (out, len(entries), (off + len(blobs)) / 1e6))
    for g in sorted(groups):
        print("  %-14s %6.0f KB" % (g, groups[g] / 1024))


if __name__ == "__main__":
    a = sys.argv[1:]
    if a and a[0] == "art":
        ART = a[1]
        a = a[2:]
    if not a:
        pack()
    elif a[0] == "sheet":
        sheet(a[1], a[2], a[3] if len(a) > 3 else "naranja")
    elif a[0] == "tank":
        tank(a[1], a[2] if len(a) > 2 else "top")
