#!/usr/bin/env python3
"""The 'linea' pack: render, sheets, pack.

    python3 tools/icons/linea/build.py                 every app in elegidos.json with a model
    python3 tools/icons/linea/build.py --only aos.clock,aos.calc
    python3 tools/icons/linea/build.py sheet [name] [ids]   laminas/linea_<name>.jpg
    python3 tools/icons/linea/build.py pack            -> out/linea/linea.pak

The apps are the juguete pack's (elegidos.json): the two packs hold the same
ones. Every models_*.py here has ART = {app id: art(m)} and, optionally,
PAIRS = {app id: (light, deep)}; an app with no pair takes pair_from() of its
colour in apps.json. Out: out/linea/<id>_120.png, _108.png, _26.png.
"""

import glob
import importlib
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ICONS = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import lib  # noqa: E402

OUT = os.path.join(ICONS, "out", "linea")


def models():
    art, pairs = {}, {}
    for f in sorted(glob.glob(os.path.join(HERE, "models_*.py"))):
        mod = importlib.import_module(os.path.basename(f)[:-3])
        art.update(mod.ART)
        pairs.update(getattr(mod, "PAIRS", {}))
    return art, pairs


def apps():
    return {a["id"]: a for a in json.load(open(os.path.join(ICONS, "apps.json")))["apps"]}


def chosen_ids():
    return list(json.load(open(os.path.join(ICONS, "elegidos.json")))["juguete"])


def png(app_id, size):
    return os.path.join(OUT, f"{app_id}_{size}.png")


def build(only=None):
    os.makedirs(OUT, exist_ok=True)
    art, pairs = models()
    info = apps()
    done, missing = 0, []
    for app_id in chosen_ids():
        if only and app_id not in only:
            continue
        if app_id not in art:
            missing.append(app_id)
            continue
        c0, c1 = pairs.get(app_id) or lib.pair_from(info[app_id]["colors"][0] or "#8E8E93")
        for mini, sizes in ((False, (120, 108)), (True, (26,))):
            lines, fills = art[app_id](lib.M(mini))
            text = lib.svg(lines, fills, c0, c1, mini)
            for s in sizes:
                lib.render(text, png(app_id, s), s)
        done += 1
    print("rendered:", done, "missing:", len(missing), " ".join(missing[:20]))


def sheet(name, ids):
    from PIL import Image, ImageDraw, ImageFont
    info = apps()
    font_dir = os.path.expanduser("~/.cache/p4os-fonts/interzip/extras/ttf")

    def font(size, bold=False):
        f = os.path.join(font_dir, "Inter-SemiBold.ttf" if bold else "Inter-Medium.ttf")
        return ImageFont.truetype(f if os.path.exists(f) else "/System/Library/Fonts/Helvetica.ttc", size)

    cols, cw, ch = 8, 150, 176
    rows = (len(ids) + cols - 1) // cols
    W, H = 24 + cols * cw, 80 + rows * ch
    im = Image.new("RGB", (W, H))
    d = ImageDraw.Draw(im)
    top, bot = (24, 40, 74), (57, 28, 74)
    for y in range(H):
        t = y / max(1, H - 1)
        d.line([(0, y), (W, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(top, bot)))
    d.text((24, 20), f"Línea · {name} ({len(ids)})", font=font(30, True), fill=(255, 255, 255))
    fs = font(14)
    for i, app_id in enumerate(ids):
        x, y = 24 + (i % cols) * cw, 80 + (i // cols) * ch
        if os.path.exists(png(app_id, 120)):
            ic = Image.open(png(app_id, 120)).convert("RGBA")
            im.paste(ic, (x, y), ic)
            m = Image.open(png(app_id, 26)).convert("RGBA")
            im.paste(m, (x + 112, y + 100), m)
        nm = info[app_id]["name"]
        nm = nm if len(nm) <= 16 else nm[:15] + "…"
        d.text((x + 60 - d.textlength(nm, font=fs) / 2, y + 132), nm, font=fs, fill=(230, 230, 240))
    dst = os.path.join(ICONS, "laminas", f"linea_{name}.jpg")
    im.save(dst, quality=90)
    print(dst)


def pack():
    d = os.path.join(OUT, "pack")
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    missing = []
    for app_id in chosen_ids():
        if not os.path.exists(png(app_id, 120)):
            missing.append(app_id)
            continue
        shutil.copy(png(app_id, 120), os.path.join(d, f"{app_id}.png"))
        shutil.copy(png(app_id, 108), os.path.join(d, f"{app_id}@108.png"))
        shutil.copy(png(app_id, 26), os.path.join(d, f"{app_id}@26.png"))
    if missing:
        print("not rendered:", ", ".join(missing))
    subprocess.run([sys.executable, os.path.join(ICONS, "mkpack.py"), d, os.path.join(OUT, "linea.pak")], check=True)


if __name__ == "__main__":
    a = sys.argv[1:]
    if a and a[0] == "sheet":
        ids = a[2].split(",") if len(a) > 2 else chosen_ids()
        sheet(a[1] if len(a) > 1 else "todos", ids)
    elif a and a[0] == "pack":
        pack()
    else:
        only = set(a[1].split(",")) if len(a) > 1 and a[0] == "--only" else None
        build(only)
