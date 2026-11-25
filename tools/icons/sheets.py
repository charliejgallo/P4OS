#!/usr/bin/env python3
"""The sheets to choose from, and what each icon weighs.

    python3 tools/icons/sheets.py          -> tools/icons/muestra/laminas/*.jpg
                                              tools/icons/muestra/pesos.md

  estilo_<s>.jpg   one style: the six at 120, at 108 and at 26 (and the 26
                   blown up 3x, nearest neighbour, to see its pixels)
  comparar.jpg     the three styles one under the other, 120 and 26
  contexto_<s>.jpg the simulator's screenshots (sim_shots.py): the grid,
                   the folder with nine miniatures, the folder open, over a
                   photo, and lying down
  contexto_todos.jpg  the home screen of the three side by side

Weights: the PNG as Pillow writes it with optimize=True, and the QOI the
pack stores (tools/gen_emoji.py's encoder, what mkpack.py uses).
"""

import io
import os
import sys

from PIL import Image, ImageDraw, ImageFont

import common as C

sys.path.insert(0, os.path.join(C.HERE, ".."))
from gen_emoji import qoi_encode  # noqa: E402

STYLES = [("a_plano", "A · Plano con degradé"),
          ("b_chubby", "B · 3D de juguete (Blender)"),
          ("c_linea", "C · Línea duotono, fondo oscuro")]
SHEETS = os.path.join(C.OUT, "laminas")
FONT_DIR = os.path.expanduser("~/.cache/p4os-fonts/interzip/extras/ttf")
BG_TOP, BG_BOT = (24, 40, 74), (57, 28, 74)     # the home screen's gradient


def font(size, bold=False):
    for f in (os.path.join(FONT_DIR, "Inter-SemiBold.ttf" if bold else "Inter-Medium.ttf"),
              "/System/Library/Fonts/Helvetica.ttc"):
        if os.path.exists(f):
            return ImageFont.truetype(f, size)
    return ImageFont.load_default()


def gradient(w, h):
    im = Image.new("RGB", (w, h))
    d = ImageDraw.Draw(im)
    for y in range(h):
        t = y / max(1, h - 1)
        d.line([(0, y), (w, y)], fill=tuple(int(a + (b - a) * t) for a, b in zip(BG_TOP, BG_BOT)))
    return im


def icon(style, app_id, size):
    return Image.open(C.out_path(style, app_id, size)).convert("RGBA")


def centred(d, x, y, text, f, fill=(235, 235, 245)):
    w = d.textlength(text, font=f)
    d.text((x - w / 2, y), text, font=f, fill=fill)


def style_sheet(style, title):
    col = 170
    W, H = 40 + col * len(C.APPS), 560
    im = gradient(W, H)
    d = ImageDraw.Draw(im)
    d.text((24, 18), title, font=font(30, True), fill=(255, 255, 255))
    small = font(17)
    for i, (app_id, name, _idea) in enumerate(C.APPS):
        cx = 20 + col * i + col // 2
        p = icon(style, app_id, 120)
        im.paste(p, (cx - 60, 78), p)
        centred(d, cx, 204, name, small)
        p = icon(style, app_id, 108)
        im.paste(p, (cx - 54, 248), p)
        m = icon(style, app_id, 26)
        im.paste(m, (cx - 58, 440), m)
        big = m.resize((78, 78), Image.NEAREST)
        im.paste(big, (cx - 20, 414), big)
    d.text((24, 216 + 18), "120 px (arriba) · 108 px (acostada)", font=font(15), fill=(170, 170, 190))
    d.text((24, 520), "26 px, la miniatura de las carpetas: de verdad, y ampliada 3× para ver los píxeles",
           font=font(15), fill=(170, 170, 190))
    return im


def compare_sheet():
    col, row = 150, 210
    W, H = 300 + col * len(C.APPS), 70 + row * len(STYLES)
    im = gradient(W, H)
    d = ImageDraw.Draw(im)
    d.text((24, 18), "Los tres estilos: 120 px y la miniatura de 26 px", font=font(26, True), fill=(255, 255, 255))
    for r, (style, title) in enumerate(STYLES):
        y = 70 + r * row
        d.text((24, y + 46), title.split(" · ")[0], font=font(48, True), fill=(255, 255, 255))
        d.text((24, y + 110), title.split(" · ")[1], font=font(16), fill=(200, 200, 220))
        for i, (app_id, _n, _i) in enumerate(C.APPS):
            x = 280 + i * col
            p = icon(style, app_id, 120)
            im.paste(p, (x, y + 10), p)
            m = icon(style, app_id, 26)
            im.paste(m, (x + 47, y + 146), m)
    return im


def shot(style, kind, name, h):
    p = os.path.join(C.OUT, "sim", style, kind, name)
    im = Image.open(p).convert("RGB")
    return im.resize((round(im.width * h / im.height), h), Image.LANCZOS)


def context_sheet(style, title):
    h = 900
    parts = [(shot(style, "grid", "home.png", h), "El inicio: grilla y dock"),
             (shot(style, "folder", "home.png", h), "Una carpeta con 9 miniaturas"),
             (shot(style, "folder", "open.png", h), "La carpeta abierta"),
             (shot(style, "photo", "home.png", h), "Sobre una foto")]
    land = shot(style, "grid", "land.png", 506 - 60)
    W = sum(p.width for p, _ in parts) + 20 * (len(parts) + 1)
    H = 80 + h + 50 + land.height + 60
    im = Image.new("RGB", (W, H), (16, 16, 20))
    d = ImageDraw.Draw(im)
    d.text((20, 20), f"{title} — en el simulador, con el pack de íconos", font=font(30, True), fill=(255, 255, 255))
    x = 20
    for p, label in parts:
        im.paste(p, (x, 80))
        centred(d, x + p.width // 2, 80 + h + 12, label, font(20))
        x += p.width + 20
    y = 80 + h + 60
    im.paste(land, (20, y))
    d.text((40 + land.width, y + land.height // 2 - 12), "Acostada: 108 px", font=font(20), fill=(235, 235, 245))
    return im


def context_all():
    h = 1000
    shots = [shot(s, "grid", "home.png", h) for s, _ in STYLES]
    folders = [shot(s, "folder", "home.png", h).crop((0, 300, 562, 590)) for s, _ in STYLES]
    W = sum(s.width for s in shots) + 20 * 4
    H = 80 + h + 20 + folders[0].height + 60
    im = Image.new("RGB", (W, H), (16, 16, 20))
    d = ImageDraw.Draw(im)
    d.text((20, 20), "Los tres en el inicio (simulador)", font=font(30, True), fill=(255, 255, 255))
    x = 20
    for (style, title), s, f in zip(STYLES, shots, folders):
        im.paste(s, (x, 80))
        im.paste(f, (x, 80 + h + 20))
        centred(d, x + s.width // 2, 80 + h + 20 + f.height + 14, title, font(22, True))
        x += s.width + 20
    return im


def weights():
    rows = []
    tot = {}
    for style, title in STYLES:
        for app_id, _n, _i in C.ALL:
            r = [style, app_id]
            for size in C.SIZES:
                im = icon(style, app_id, size)
                b = io.BytesIO()
                im.save(b, "PNG", optimize=True)
                q = len(qoi_encode(im))
                r += [len(b.getvalue()), q]
                t = tot.setdefault((style, size), [0, 0, 0])
                t[0] += len(b.getvalue())
                t[1] += q
                t[2] += 1
            rows.append(r)
    out = ["# Lo que pesa cada ícono", "",
           "PNG con `optimize=True` y QOI (lo que guarda el pack, `mkpack.py`), en bytes.", "",
           "## Promedio por estilo", "",
           "| Estilo | PNG 120 | QOI 120 | PNG 108 | QOI 108 | PNG 26 | QOI 26 | pack de 9 × 3 tamaños (QOI) |",
           "|---|---|---|---|---|---|---|---|"]
    for style, title in STYLES:
        cells = []
        pack = 0
        for size in C.SIZES:
            p, q, n = tot[(style, size)]
            cells += [f"{p / n / 1024:.1f} KB", f"{q / n / 1024:.1f} KB"]
            pack += q
        out.append(f"| {title} | " + " | ".join(cells) + f" | {pack / 1024:.0f} KB |")
    out += ["", "## Cada uno", "",
            "| Estilo | App | PNG 120 | QOI 120 | PNG 108 | QOI 108 | PNG 26 | QOI 26 |",
            "|---|---|---|---|---|---|---|---|"]
    for r in rows:
        out.append(f"| {r[0]} | {r[1]} | " + " | ".join(f"{v:,}".replace(",", ".") for v in r[2:]) + " |")
    with open(os.path.join(C.OUT, "pesos.md"), "w") as f:
        f.write("\n".join(out) + "\n")
    return "\n".join(out[:12])


def main():
    os.makedirs(SHEETS, exist_ok=True)
    for style, title in STYLES:
        style_sheet(style, title).save(os.path.join(SHEETS, f"estilo_{style}.jpg"), quality=92)
        if os.path.isdir(os.path.join(C.OUT, "sim", style)):
            context_sheet(style, title).save(os.path.join(SHEETS, f"contexto_{style}.jpg"), quality=90)
    compare_sheet().save(os.path.join(SHEETS, "comparar.jpg"), quality=92)
    context_all().save(os.path.join(SHEETS, "contexto_todos.jpg"), quality=90)
    print(weights())


if __name__ == "__main__":
    main()
