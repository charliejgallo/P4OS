#!/usr/bin/env python3
"""The 'juguete' pack: Blender's renders -> the icon sizes, the variant
sheets, and the pack from what was chosen.

    python3 tools/icons/juguete/post.py                 shrink every new render
    python3 tools/icons/juguete/post.py sheets [group]  the variant sheets
    python3 tools/icons/juguete/post.py pack            elegidos.json -> out/juguete/juguete.pak
    python3 tools/icons/juguete/post.py chosen          laminas/juguete_elegidos.jpg, every chosen icon

Shrink: out/juguete/raw/<id>_<v>_big.png (480) -> out/juguete/<id>_<v>_120.png
and _108.png; _mini.png (104) -> _26.png. Premultiplied alpha, so the
transparent corners do not darken the tile's edge; a light unsharp mask on
the 26.

Sheets: tools/icons/laminas/juguete_<group>.jpg - each app of the group with
its variants side by side, a letter on each, at 120 over the home screen's
gradient with the 26 next to it. The user answers with letters, which go to
tools/icons/elegidos.json:  {"juguete": {"aos.clock": "a", ...}}.

Pack: for every app in elegidos.json, <id>.png (120), <id>@108.png and
<id>@26.png into out/juguete/pack/, then tools/icons/mkpack.py makes
out/juguete/juguete.pak (the card's icons/juguete.pak).
"""

import json
import os
import shutil
import subprocess
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
ICONS = os.path.dirname(HERE)
OUT = os.path.join(ICONS, "out", "juguete")
RAW = os.path.join(OUT, "raw")
SHEETS = os.path.join(ICONS, "laminas")
FONT_DIR = os.path.expanduser("~/.cache/p4os-fonts/interzip/extras/ttf")
BG_TOP, BG_BOT = (24, 40, 74), (57, 28, 74)       # the home screen's gradient


def apps():
    return json.load(open(os.path.join(ICONS, "apps.json")))


def shrink(src, size, sharpen=False):
    im = Image.open(src).convert("RGBA").convert("RGBa")
    im = im.resize((size, size), Image.LANCZOS).convert("RGBA")
    if sharpen:
        im = im.filter(ImageFilter.UnsharpMask(radius=0.8, percent=60, threshold=1))
    return im


def sized(app_id, v, size):
    return os.path.join(OUT, f"{app_id}_{v}_{size}.png")


def do_shrink():
    import sprites
    sprites.compose_all()
    n = 0
    for f in sorted(os.listdir(RAW)):
        if not f.endswith(".png"):
            continue
        stem, kind = f[:-4].rsplit("_", 1)
        app_id, v = stem.rsplit("_", 1)
        src = os.path.join(RAW, f)
        sizes = ((120, False), (108, False)) if kind == "big" else ((26, True),)
        for size, sharp in sizes:
            dst = sized(app_id, v, size)
            if os.path.exists(dst) and os.path.getmtime(dst) > os.path.getmtime(src):
                continue
            shrink(src, size, sharp).save(dst)
            n += 1
    print("shrunk:", n)


def font(size, bold=False):
    f = os.path.join(FONT_DIR, "Inter-SemiBold.ttf" if bold else "Inter-Medium.ttf")
    return ImageFont.truetype(f if os.path.exists(f) else "/System/Library/Fonts/Helvetica.ttc", size)


def gradient(w, h):
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / max(1, h - 1)
        d.line([(0, y), (w, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(BG_TOP, BG_BOT)))
    return im


def sheet(group):
    """Two columns of apps; each app a row of its variants."""
    redo = []
    if group.startswith("rehacer"):     # the apps sent back: rehacer (d-f), rehacer2 (g-i)...
        first = chr(ord("d") + 3 * (int(group[7:] or 1) - 1))
        redo = json.load(open(os.path.join(ICONS, "elegidos.json"))).get("rehacer", [])
        lst = [dict(a, juguete={k: t for k, t in a["juguete"].items() if first <= k < chr(ord(first) + 3)})
               for a in apps()["apps"] if a["id"] in redo]
    else:
        lst = [a for a in apps()["apps"] if a["group"] == group]
    cell_w, cell_h, var_w = 3 * 176 + 24, 214, 176
    cols = 2
    rows = (len(lst) + cols - 1) // cols
    W, H = 24 + cols * cell_w, 86 + rows * cell_h + 20
    im = gradient(W, H)
    d = ImageDraw.Draw(im)
    d.text((24, 20), f"Juguete · {group} — elegí una letra por app", font=font(30, True), fill=(255, 255, 255))
    fn, fl, fs = font(19, True), font(26, True), font(14)
    for i, a in enumerate(lst):
        x0 = 24 + (i % cols) * cell_w
        y0 = 86 + (i // cols) * cell_h
        d.text((x0, y0), f"{a['name']}", font=fn, fill=(255, 255, 255))
        d.text((x0 + d.textlength(a["name"], font=fn) + 10, y0 + 4), a["id"], font=fs, fill=(160, 160, 190))
        for k, v in enumerate(sorted(a["juguete"])):
            x = x0 + k * var_w
            y = y0 + 30
            p = sized(a["id"], v, 120)
            if not os.path.exists(p):
                d.rounded_rectangle((x, y, x + 120, y + 120), 27, outline=(120, 120, 150), width=2)
                d.text((x + 30, y + 50), "falta", font=fs, fill=(160, 160, 190))
            else:
                ic = Image.open(p).convert("RGBA")
                im.paste(ic, (x, y), ic)
                m = Image.open(sized(a["id"], v, 26)).convert("RGBA")
                im.paste(m, (x + 128, y + 94), m)
            d.text((x + 130, y + 4), v, font=fl, fill=(255, 214, 10))
            idea = a["juguete"][v]
            d.text((x, y + 128), idea[:21] + ("…" if len(idea) > 21 else ""), font=fs, fill=(190, 190, 215))
    os.makedirs(SHEETS, exist_ok=True)
    dst = os.path.join(SHEETS, f"juguete_{group}.jpg")
    im.save(dst, quality=90)
    print(dst)


def chosen_sheet():
    """Every chosen icon at 120 on the home screen's gradient, 8 across, with
    its name, and the 26 px miniature in the corner."""
    chosen = json.load(open(os.path.join(ICONS, "elegidos.json")))["juguete"]
    lst = [a for a in apps()["apps"] if a["id"] in chosen]
    cols, cw, ch = 8, 150, 176
    rows = (len(lst) + cols - 1) // cols
    W, H = 24 + cols * cw, 80 + rows * ch
    im = gradient(W, H)
    d = ImageDraw.Draw(im)
    d.text((24, 20), f"Juguete · los {len(lst)} elegidos", font=font(30, True), fill=(255, 255, 255))
    fs = font(14)
    for i, a in enumerate(lst):
        v = chosen[a["id"]]
        x, y = 24 + (i % cols) * cw, 80 + (i // cols) * ch
        p = sized(a["id"], v, 120)
        if os.path.exists(p):
            ic = Image.open(p).convert("RGBA")
            im.paste(ic, (x, y), ic)
            m = Image.open(sized(a["id"], v, 26)).convert("RGBA")
            im.paste(m, (x + 112, y + 100), m)
        name = a["name"] if len(a["name"]) <= 16 else a["name"][:15] + "…"
        w = d.textlength(name, font=fs)
        d.text((x + 60 - w / 2, y + 132), name, font=fs, fill=(230, 230, 240))
    dst = os.path.join(SHEETS, "juguete_elegidos.jpg")
    im.save(dst, quality=90)
    print(dst)


def pack():
    chosen = json.load(open(os.path.join(ICONS, "elegidos.json")))["juguete"]
    d = os.path.join(OUT, "pack")
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    missing = []
    for app_id, v in sorted(chosen.items()):
        if not os.path.exists(sized(app_id, v, 120)):
            missing.append(f"{app_id}/{v}")
            continue
        shutil.copy(sized(app_id, v, 120), os.path.join(d, f"{app_id}.png"))
        shutil.copy(sized(app_id, v, 108), os.path.join(d, f"{app_id}@108.png"))
        shutil.copy(sized(app_id, v, 26), os.path.join(d, f"{app_id}@26.png"))
    if missing:
        print("not rendered:", ", ".join(missing))
    subprocess.run([sys.executable, os.path.join(ICONS, "mkpack.py"), d,
                    os.path.join(OUT, "juguete.pak")], check=True)


if __name__ == "__main__":
    a = sys.argv[1:]
    if not a:
        do_shrink()
    elif a[0] == "sheets":
        do_shrink()
        for g in (a[1:] or apps()["groups"]):
            sheet(g)
    elif a[0] == "pack":
        pack()
    elif a[0] == "chosen":
        do_shrink()
        chosen_sheet()
