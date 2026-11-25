"""The 'linea' pack (style C): a fine line in two colours on a dark tile.

Every icon is an SVG in a 120 x 120 viewBox, written by code (no downloaded
art), rendered by rsvg-convert. The recipe is style_c_linea.py's, the sample
the user liked:

  - the tile: near black (#262932 -> #0D0E12), a hairline edge, a faint
    halo of the app's colour behind the drawing;
  - the drawing: round lines of 6.5 units with a gradient between the app's
    two colours (light at the top left, deeper at the bottom right), over a
    blurred copy of themselves so they glow a little; dots and small solids
    with the same gradient; whatever lies behind is covered with the tile's
    colour (KO);
  - the miniature (26 px): the same drawing with an 11-unit line (2.4 px)
    and no glow; a model leaves out what would only be mud at that size.

A model is a function art(m) -> (lines, fills): two strings of SVG elements,
the first stroked with the gradient, the second filled with it. m.mini says
which size is being drawn; m.w is the line width.
"""

import colorsys
import math
import os
import subprocess

TILE_C = "#16181E"          # the tile's colour, to cover what lies behind
KO = f'fill="{TILE_C}"'
RADIUS = 0.225


class M:
    def __init__(self, mini):
        self.mini = mini
        self.w = 11.0 if mini else 6.5


def _hex(r, g, b):
    return "#%02X%02X%02X" % tuple(max(0, min(255, round(v * 255))) for v in (r, g, b))


def pair_from(color):
    """The two colours from an app's colour: a light one and a deeper,
    neighbouring one. Greys get white to the system's violet."""
    c = color.lstrip("#")
    r, g, b = (int(c[i:i + 2], 16) / 255 for i in (0, 2, 4))
    h, s, v = colorsys.rgb_to_hsv(r, g, b)
    if s < 0.18:
        return "#E6EAF2", "#8C7CFF"
    light = _hex(*colorsys.hsv_to_rgb(h - 0.03, min(1, s * 0.55 + 0.1), 1.0))
    deep = _hex(*colorsys.hsv_to_rgb(h + 0.04, min(1, max(s, 0.7)), 0.98))
    return light, deep


def tile_path(inset=0.0):
    s = 120 - 2 * inset
    r = s * RADIUS
    return f'<rect x="{inset}" y="{inset}" width="{s}" height="{s}" rx="{r:.2f}" ry="{r:.2f}"'


def svg(lines, fills, c0, c1, mini):
    w = 11.0 if mini else 6.5
    defs = (f'<linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">'
            f'<stop offset="0" stop-color="#262932"/><stop offset="1" stop-color="#0D0E12"/></linearGradient>'
            f'<radialGradient id="halo" cx="0.5" cy="0.5" r="0.5">'
            f'<stop offset="0" stop-color="{c1}" stop-opacity="0.22"/>'
            f'<stop offset="1" stop-color="{c1}" stop-opacity="0"/></radialGradient>'
            f'<linearGradient id="ink" gradientUnits="userSpaceOnUse" x1="22" y1="18" x2="98" y2="102">'
            f'<stop offset="0" stop-color="{c0}"/><stop offset="1" stop-color="{c1}"/></linearGradient>'
            '<filter id="glow" x="-30%" y="-30%" width="160%" height="160%">'
            '<feGaussianBlur stdDeviation="3.2"/></filter>')
    body = tile_path() + ' fill="url(#bg)"/>'
    body += '<circle cx="60" cy="62" r="52" fill="url(#halo)"/>'
    if not mini:
        body += tile_path(0.6) + ' fill="none" stroke="#fff" stroke-opacity="0.10" stroke-width="1.2"/>'
    g = (f'<g fill="none" stroke="url(#ink)" stroke-width="{w}" stroke-linecap="round" '
         f'stroke-linejoin="round">{lines}</g>')
    f = f'<g fill="url(#ink)">{fills}</g>' if fills else ""
    if not mini:
        body += f'<g filter="url(#glow)" opacity="0.65">{g}{f}</g>'
    body += g + f
    return (f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 120 120" width="120" height="120">'
            f'<defs>{defs}</defs>{body}</svg>')


def render(svg_text, png, size):
    src = png[:-4] + ".svg"
    with open(src, "w") as fh:
        fh.write(svg_text)
    subprocess.run(["rsvg-convert", "-w", str(size), "-h", str(size), "-o", png, src], check=True)
    os.remove(src)


# --------------------------------------------------------------------------
# Drawing helpers: each returns an SVG element string.

def P(d, extra=""):
    return f'<path d="{d}" {extra}/>'


def C(cx, cy, r, extra=""):
    return f'<circle cx="{cx:.1f}" cy="{cy:.1f}" r="{r:.1f}" {extra}/>'


def R(x, y, w, h, rx=0, extra=""):
    return f'<rect x="{x:.1f}" y="{y:.1f}" width="{w:.1f}" height="{h:.1f}" rx="{rx:.1f}" {extra}/>'


def L(x1, y1, x2, y2):
    return f'<path d="M{x1:.1f},{y1:.1f} L{x2:.1f},{y2:.1f}"/>'


def poly(pts, close=True, extra=""):
    d = "M" + " L".join(f"{x:.1f},{y:.1f}" for x, y in pts) + (" Z" if close else "")
    return P(d, extra)


def arc(cx, cy, r, a0, a1):
    """An arc from a0 to a1 degrees (0 = right, counter-clockwise on screen)."""
    x0, y0 = cx + r * math.cos(math.radians(a0)), cy - r * math.sin(math.radians(a0))
    x1, y1 = cx + r * math.cos(math.radians(a1)), cy - r * math.sin(math.radians(a1))
    large = 1 if (a1 - a0) % 360 > 180 else 0
    return P(f"M{x0:.1f},{y0:.1f} A{r:.1f},{r:.1f} 0 {large} 0 {x1:.1f},{y1:.1f}")


def waves(cx, cy, n, r0, step, a0, a1):
    return "".join(arc(cx, cy, r0 + i * step, a0, a1) for i in range(n))


def gear(cx, cy, r_out, r_in, teeth, frac=0.5):
    pts = []
    pitch = 2 * math.pi / teeth
    top, root = pitch * frac * 0.5, pitch * (1 - frac) * 0.5
    for i in range(teeth):
        a = i * pitch - math.pi / 2
        for ang, r in ((a - top - root * 0.35, r_in), (a - top, r_out), (a + top, r_out),
                       (a + top + root * 0.35, r_in)):
            pts.append((cx + r * math.cos(ang), cy + r * math.sin(ang)))
    return poly(pts)


def star(cx, cy, r_out, r_in, n=5, extra=""):
    pts = []
    for i in range(2 * n):
        r = r_out if i % 2 == 0 else r_in
        a = math.radians(-90 + i * 180 / n)
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return poly(pts, extra=extra)


def cloud(x, y, s, extra=""):
    """A cloud: flat bottom, three bumps; (x, y) its bottom-left."""
    return P(f"M{x + 8 * s:.1f},{y:.1f} A{8 * s:.1f},{8 * s:.1f} 0 0 1 {x + 6 * s:.1f},{y - 15.5 * s:.1f} "
             f"A{11 * s:.1f},{11 * s:.1f} 0 0 1 {x + 22 * s:.1f},{y - 25 * s:.1f} "
             f"A{14 * s:.1f},{14 * s:.1f} 0 0 1 {x + 47 * s:.1f},{y - 19 * s:.1f} "
             f"A{10 * s:.1f},{10 * s:.1f} 0 0 1 {x + 52 * s:.1f},{y:.1f} Z", extra)


def heart(cx, cy, s, extra=""):
    pts = []
    for i in range(36):
        t = 2 * math.pi * i / 36
        x = 16 * math.sin(t) ** 3
        y = 13 * math.cos(t) - 5 * math.cos(2 * t) - 2 * math.cos(3 * t) - math.cos(4 * t)
        pts.append((cx + x * s / 17, cy - y * s / 17))
    return poly(pts, extra=extra)
