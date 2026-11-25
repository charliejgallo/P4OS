"""The 'juguete' pack: render the models' variants. Inside Blender, always -b.

    B=/Applications/Blender.app/Contents/MacOS/Blender
    $B -b -P tools/icons/juguete/render.py -- --group sistema-1
    $B -b -P tools/icons/juguete/render.py -- --only aos.clock,aos.calc [--variants a] [--samples 64]
    $B -b -P tools/icons/juguete/render.py -- --chosen        # only what elegidos.json says

Every models_*.py next to this file holds MODELS = {app id: {variant: build}};
build(mini) makes one icon's scene. Out: tools/icons/out/juguete/raw/
<id>_<v>_big.png (480 px) and <id>_<v>_mini.png (104 px); post.py shrinks
them. Already rendered files are kept unless --force (or the model file is
newer than the render).
"""

import argparse
import glob
import importlib
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import lib  # noqa: E402
import bpy  # noqa: E402

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
ap = argparse.ArgumentParser()
ap.add_argument("--group", default="")
ap.add_argument("--only", default="")
ap.add_argument("--variants", default="")
ap.add_argument("--chosen", action="store_true")
ap.add_argument("--samples", type=int, default=128)
ap.add_argument("--force", action="store_true")
ap.add_argument("--cpu", action="store_true")
ARGS = ap.parse_args(argv)

ICONS = os.path.dirname(HERE)
RAW = os.path.join(ICONS, "out", "juguete", "raw")
BIG, MINI = 480, 104


def models():
    """Every model, and the file it comes from (to know when it changed)."""
    out = {}
    for f in sorted(glob.glob(os.path.join(HERE, "models_*.py"))):
        mod = importlib.import_module(os.path.basename(f)[:-3])
        for app_id, variants in mod.MODELS.items():
            for v, fn in variants.items():
                out[(app_id, v)] = (fn, f)
    return out


def main():
    os.makedirs(RAW, exist_ok=True)
    apps = json.load(open(os.path.join(ICONS, "apps.json")))["apps"]
    group_of = {a["id"]: a["group"] for a in apps}
    only = {s for s in ARGS.only.split(",") if s}
    vs = {s for s in ARGS.variants.split(",") if s}
    chosen = {}
    if ARGS.chosen:
        chosen = json.load(open(os.path.join(ICONS, "elegidos.json")))["juguete"]
    done = 0
    for (app_id, v), (fn, src) in models().items():
        if ARGS.group and group_of.get(app_id) != ARGS.group:
            continue
        if only and app_id not in only:
            continue
        if vs and v not in vs:
            continue
        if ARGS.chosen and chosen.get(app_id) != v:
            continue
        for mini, res in ((False, BIG), (True, MINI)):
            # a sprite model renders only the tile, into raw/tile/; sprites.py
            # lays the game's art on it and writes raw/<id>_<v>_*.png
            base = os.path.join(RAW, "tile") if getattr(fn, "SPRITE", False) else RAW
            os.makedirs(base, exist_ok=True)
            path = os.path.join(base, f"{app_id}_{v}_{'mini' if mini else 'big'}.png")
            if (not ARGS.force and os.path.exists(path)
                    and os.path.getmtime(path) > max(os.path.getmtime(src),
                                                     os.path.getmtime(os.path.join(HERE, "lib.py")))):
                continue
            sc = lib.reset(res, ARGS.samples, ARGS.cpu)
            fn(mini)
            moved = fit(MARGIN_MINI if mini else MARGIN)
            if moved:
                FITS.append((app_id, v, "mini" if mini else "big", moved))
            sc.render.filepath = path
            bpy.ops.render.render(write_still=True)
            done += 1
            print("rendered", os.path.basename(path), flush=True)
    print("renders:", done)
    if FITS:
        log = os.path.join(os.path.dirname(RAW), "fit.log")
        with open(log, "a") as f:
            for app_id, v, kind, (dx, dy, k, dz) in FITS:
                line = f"{app_id} {v} {kind}: moved {dx:+.3f} {dy:+.3f}, scaled {k:.2f}, lifted {dz:.3f}"
                print("fit", line)
                f.write(line + "\n")


# --------------------------------------------------------------------------
# Every toy inside the tile, whole, with a margin.
#
# The camera looks straight down, orthographic, so what the picture shows of
# a toy is its vertices' x and y. Those must lie inside the tile's rounded
# square shrunk by MARGIN (the same corner, smaller), or the tile's edge cuts
# the toy. When a toy does not fit, it is centred and then shrunk until it
# does. And no part of it may sink below the tile's face (z = 0) - a big flat
# toy, tilted, dips into the tile on one side and the tile swallows it - so
# it is lifted until it clears. A model is never trusted to have done the
# sums itself.

MARGIN = 0.06           # of the side: ~7 px at 120
MARGIN_MINI = 0.05      # ~1.3 px at 26
FITS = []
FLAT = 0.008            # paper-thin things (a pitch's lines) belong to the tile
LIFT = 0.004            # nothing of a toy below this: under z = 0 the tile hides it


def _inside(x, y, h, r):
    ax, ay = abs(x), abs(y)
    if ax > h or ay > h:
        return False
    if ax > h - r and ay > h - r:
        return (ax - (h - r)) ** 2 + (ay - (h - r)) ** 2 <= r * r
    return True


def _toy_points():
    """Every vertex of every visible thing but the tile, in world x, y."""
    dg = bpy.context.evaluated_depsgraph_get()
    pts = []
    for ob in bpy.context.scene.objects:
        if ob.name.startswith("tile") or ob.type not in ("MESH", "CURVE", "FONT", "META", "SURFACE"):
            continue
        ev = ob.evaluated_get(dg)
        try:
            me = ev.to_mesh()
        except RuntimeError:
            me = None
        if me is None:
            continue
        mw = ev.matrix_world
        vs = [mw @ v.co for v in me.vertices]
        ev.to_mesh_clear()
        if vs and max(v.z for v in vs) - min(v.z for v in vs) <= FLAT:
            continue
        pts += [(v.x, v.y, v.z) for v in vs]
    return pts


def fit(margin):
    bpy.context.view_layer.update()
    pts = _toy_points()
    h, r = 0.5 - margin, lib.RADIUS - margin * 0.5
    if not pts:
        return None
    zmin = min(p[2] for p in pts)
    outside = not all(_inside(x, y, h, r) for x, y, _z in pts)
    if not outside and zmin >= LIFT:
        return None
    cx = cy = 0.0
    k = 1.0
    if outside:
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        cx, cy = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
        while k > 0.3 and not all(_inside((x - cx) * k, (y - cy) * k, h, r) for x, y, _z in pts):
            k *= 0.98
    dz = max(0.0, LIFT - zmin * k)
    # move and shrink every top-level thing but the tile and the lights
    from mathutils import Matrix, Vector
    m = (Matrix.Translation(Vector((0, 0, dz))) @ Matrix.Scale(k, 4) @ Matrix.Translation(Vector((-cx, -cy, 0))))
    for ob in bpy.context.scene.objects:
        if ob.parent is None and not ob.name.startswith("tile") and ob.type not in ("CAMERA", "LIGHT"):
            if ob.type != "EMPTY" and ob.type in ("MESH", "CURVE", "FONT", "META"):
                zs = [(ob.matrix_world @ Vector(c)).z for c in ob.bound_box]
                if max(zs) - min(zs) <= FLAT:
                    continue
            ob.matrix_world = m @ ob.matrix_world
    bpy.context.view_layer.update()
    return (-cx, -cy, k, dz)


main()
