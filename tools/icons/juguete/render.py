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
            path = os.path.join(RAW, f"{app_id}_{v}_{'mini' if mini else 'big'}.png")
            if (not ARGS.force and os.path.exists(path)
                    and os.path.getmtime(path) > max(os.path.getmtime(src),
                                                     os.path.getmtime(os.path.join(HERE, "lib.py")))):
                continue
            sc = lib.reset(res, ARGS.samples, ARGS.cpu)
            fn(mini)
            sc.render.filepath = path
            bpy.ops.render.render(write_still=True)
            done += 1
            print("rendered", os.path.basename(path), flush=True)
    print("renders:", done)


main()
