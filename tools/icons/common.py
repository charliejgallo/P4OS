"""What the three icon styles share: the apps, the sizes, the tile and the
helpers that turn an SVG into PNGs.

The sizes are the board's (components/aos_ui/include/aos_theme.h):

  120  AOS_UI_ICON, the home screen upright
  108  AOS_UI_ICON_LAND, lying down
   26  the miniature inside a folder (folder_icon() in aos_home.c: a 120 px
       folder, pad 15, gap 5, three across -> 26 px each)

The 120 and the 108 are the same drawing; the 26 is drawn on its own, with
fewer and thicker things, because a 120 px drawing shrunk to 26 is mush.

The tile is a rounded square with the corner the firmware uses for icons and
folders (22.5 % of the side), so a new icon sits in the grid exactly where
the old one did and the folder behind the miniatures has the same shape.
"""

import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "muestra")

SIZES = (120, 108, 26)
RADIUS = 0.225              # of the side, like aos_home.c / aos_icon.c

# The sample: id, the name on the home screen, the idea of today's icon.
APPS = [
    ("aos.settings", "Ajustes",  "gear on grey"),
    ("aos.serial",   "Terminal", "serial connector on dark slate"),
    ("aos.clima",    "Clima",    "sun behind a cloud on blue"),
    ("aos.notas",    "Notas",    "a sheet with an orange band on yellow"),
    ("demo.metegol", "Metegol",  "the pitch, rods, two players and the ball"),
    ("aos.store",    "Tienda",   "a box (package) on blue"),
]

# Three more, only so the folder in the context sheet has nine real
# miniatures (folder_icon() shows up to 9). Not part of the choice.
EXTRAS = [
    ("aos.clock",    "Reloj",       "a clock face on black"),
    ("aos.calc",     "Calculadora", "a calculator on amber"),
    ("aos.photos",   "Fotos",       "a picture on yellow to pink"),
]
ALL = APPS + EXTRAS


def out_path(style, app_id, size):
    d = os.path.join(OUT, style)
    os.makedirs(d, exist_ok=True)
    return os.path.join(d, f"{app_id}_{size}.png")


def svg_to_png(svg, png, size):
    """rsvg-convert (librsvg, in Homebrew) renders the SVG at exactly
    size x size. The SVG's viewBox is whatever the drawing uses."""
    src = png[:-4] + ".svg"
    with open(src, "w") as f:
        f.write(svg)
    subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size),
                    "-o", png, src], check=True)
    os.remove(src)


def keep_svg(style, app_id, kind, svg):
    """The source stays next to the PNGs, so production can open and tweak it."""
    d = os.path.join(OUT, style, "svg")
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, f"{app_id}_{kind}.svg"), "w") as f:
        f.write(svg)


def svg_doc(vb, body, defs=""):
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 {vb} {vb}" '
            f'width="{vb}" height="{vb}"><defs>{defs}</defs>{body}</svg>')


def tile_path(vb, inset=0.0):
    s = vb - 2 * inset
    r = s * RADIUS
    return (f'<rect x="{inset}" y="{inset}" width="{s}" height="{s}" '
            f'rx="{r:.2f}" ry="{r:.2f}"')


def gear_path(cx, cy, r_out, r_in, teeth, tooth_frac=0.5, hole=0.0, round_=0.0):
    """A gear as one path: 'teeth' teeth between r_in and r_out, the top of each
    tooth tooth_frac of its pitch, and an optional hole (even-odd)."""
    import math
    pts = []
    pitch = 2 * math.pi / teeth
    top = pitch * tooth_frac * 0.5
    root = pitch * (1 - tooth_frac) * 0.5
    for i in range(teeth):
        a = i * pitch - math.pi / 2
        # flank up, top, flank down, root
        for ang, r in ((a - top - root * 0.35, r_in), (a - top, r_out),
                       (a + top, r_out), (a + top + root * 0.35, r_in)):
            pts.append((cx + r * math.cos(ang), cy + r * math.sin(ang)))
    d = "M" + " L".join(f"{x:.2f},{y:.2f}" for x, y in pts) + " Z"
    if hole:
        d += (f" M{cx + hole:.2f},{cy:.2f} A{hole:.2f},{hole:.2f} 0 1 0 "
              f"{cx - hole:.2f},{cy:.2f} A{hole:.2f},{hole:.2f} 0 1 0 "
              f"{cx + hole:.2f},{cy:.2f} Z")
    return d
