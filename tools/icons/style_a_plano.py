#!/usr/bin/env python3
"""Style A - flat and modern, with gradients.

    python3 tools/icons/style_a_plano.py        -> tools/icons/muestra/a_plano/

Like a phone's icons today: a tile with a vertical gradient (lighter at the
top), a soft sheen over the upper half, and a drawing that is simple but
cared for: its own light gradient and a soft shadow under it.

Every icon is an SVG written here (no downloaded art), in a 120 x 120
viewBox. The 120 and 108 PNGs are the same SVG; the 26 is another SVG of the
same idea with fewer, fatter shapes and no shadow (at 26 px a blur is dirt).

Recipe, for production:
  - tile: TILE(top, bottom) - two colours of the same hue, the top ~15 %
    lighter; the sheen and the hairline at the top come for free.
  - drawing: white-ish (#FFFFFF -> #E6E9F0) unless the idea needs colour;
    at most three colours besides the tile's.
  - shadow: SHADOW, a blur of 3 under the drawing, 2 down, 30 % black.
  - mini: the drawing's silhouette at ~70 % of the tile, strokes >= 9 units
    (2 px at 26), details that are smaller than 4 units (1 px) go.
"""

import math
import sys

import common as C

STYLE = "a_plano"

SHADOW = ('<filter id="sh" x="-20%" y="-20%" width="140%" height="150%">'
          '<feDropShadow dx="0" dy="2.2" stdDeviation="2.6" flood-color="#000" '
          'flood-opacity="0.30"/></filter>')
WHITE = ('<linearGradient id="wh" x1="0" y1="0" x2="0" y2="1">'
         '<stop offset="0" stop-color="#FFFFFF"/>'
         '<stop offset="1" stop-color="#E4E7EE"/></linearGradient>')


def TILE(top, bottom, mini=False):
    """The tile: gradient, sheen on the top half, a hairline of light."""
    defs = (f'<linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">'
            f'<stop offset="0" stop-color="{top}"/>'
            f'<stop offset="1" stop-color="{bottom}"/></linearGradient>'
            '<linearGradient id="sheen" x1="0" y1="0" x2="0" y2="1">'
            '<stop offset="0" stop-color="#fff" stop-opacity="0.22"/>'
            '<stop offset="0.5" stop-color="#fff" stop-opacity="0"/></linearGradient>')
    body = C.tile_path(120) + ' fill="url(#bg)"/>'
    body += C.tile_path(120) + ' fill="url(#sheen)"/>'
    if not mini:
        body += C.tile_path(120, 0.6) + (' fill="none" stroke="#fff" '
                                         'stroke-opacity="0.18" stroke-width="1.2"/>')
    return defs, body


def lin(id_, c0, c1, x2=0, y2=1):
    return (f'<linearGradient id="{id_}" x1="0" y1="0" x2="{x2}" y2="{y2}">'
            f'<stop offset="0" stop-color="{c0}"/>'
            f'<stop offset="1" stop-color="{c1}"/></linearGradient>')


def rad(id_, c0, c1, cx=0.35, cy=0.3):
    return (f'<radialGradient id="{id_}" cx="{cx}" cy="{cy}" r="0.75">'
            f'<stop offset="0" stop-color="{c0}"/>'
            f'<stop offset="1" stop-color="{c1}"/></radialGradient>')


# --------------------------------------------------------------------------
# The six. Each returns (defs, body) for the big one and for the mini.

def settings(mini):
    d, b = TILE("#A3A6AE", "#55585F", mini)
    if mini:
        g = C.gear_path(60, 60, 44, 33, 8, 0.52, hole=14)
        b += (f'<path d="{g}" fill="#fff" fill-rule="evenodd" stroke="#fff" '
              'stroke-width="6" stroke-linejoin="round"/>')
        return d, b
    d += SHADOW + WHITE + lin("hub", "#8C9098", "#5E6168")
    g = C.gear_path(60, 60, 40, 30, 9, 0.5, hole=17)
    b += ('<g filter="url(#sh)">'
          f'<path d="{g}" fill="url(#wh)" fill-rule="evenodd" stroke="url(#wh)" '
          'stroke-width="5" stroke-linejoin="round"/></g>')
    # the inner ring and the hub, so the gear has depth
    b += '<circle cx="60" cy="60" r="24" fill="none" stroke="#C9CCD3" stroke-width="2"/>'
    b += '<circle cx="60" cy="60" r="9" fill="url(#hub)"/>'
    return d, b


def serial(mini):
    d, b = TILE("#3C4A57", "#11171D", mini)
    # A D-sub connector seen from the front: a trapezoid shell, nine pins.
    if mini:
        b += ('<path d="M22,40 H98 L88,78 H32 Z" fill="#fff" stroke="#fff" '
              'stroke-width="8" stroke-linejoin="round"/>')
        b += '<path d="M37,50 H83 L78,68 H42 Z" fill="#1A232B"/>'
        b += '<g fill="#FFC83A"><circle cx="48" cy="59" r="4.6"/><circle cx="60" cy="59" r="4.6"/><circle cx="72" cy="59" r="4.6"/></g>'
        b += '<rect x="53" y="82" width="14" height="22" rx="4" fill="#fff"/>'
        return d, b
    d += SHADOW + lin("shell", "#F4F6F9", "#B9C0CA") + lin("face", "#0E1419", "#26313B")
    d += rad("pin", "#FFF2B0", "#E0A020", 0.4, 0.35) + lin("cable", "#D9DEE5", "#9AA3AE")
    b += '<g filter="url(#sh)">'
    b += '<rect x="54" y="80" width="12" height="40" rx="5" fill="url(#cable)"/>'
    b += '<rect x="49" y="76" width="22" height="12" rx="4" fill="url(#shell)"/>'
    b += ('<path d="M20,34 H100 L90,78 H30 Z" fill="url(#shell)" stroke="url(#shell)" '
          'stroke-width="7" stroke-linejoin="round"/>')
    b += '</g>'
    b += ('<path d="M31,43 H89 L82,69 H38 Z" fill="url(#face)" stroke="url(#face)" '
          'stroke-width="4" stroke-linejoin="round"/>')
    for i in range(5):
        b += f'<circle cx="{40 + i * 10}" cy="50" r="3.3" fill="url(#pin)"/>'
    for i in range(4):
        b += f'<circle cx="{45 + i * 10}" cy="62" r="3.3" fill="url(#pin)"/>'
    b += '<circle cx="16" cy="56" r="0" fill="none"/>'
    return d, b


def cloud_path(x, y, s):
    """A cloud as one path: flat bottom, three bumps. (x, y) bottom-left, s scale."""
    return (f"M{x + 8 * s:.1f},{y:.1f} "
            f"A{8 * s:.1f},{8 * s:.1f} 0 0 1 {x + 6 * s:.1f},{y - 15.5 * s:.1f} "
            f"A{11 * s:.1f},{11 * s:.1f} 0 0 1 {x + 22 * s:.1f},{y - 25 * s:.1f} "
            f"A{14 * s:.1f},{14 * s:.1f} 0 0 1 {x + 47 * s:.1f},{y - 19 * s:.1f} "
            f"A{10 * s:.1f},{10 * s:.1f} 0 0 1 {x + 52 * s:.1f},{y:.1f} Z")


def clima(mini):
    d, b = TILE("#3FA2FF", "#0A5BD6", mini)
    if mini:
        b += '<circle cx="76" cy="44" r="24" fill="#FFD43B"/>'
        b += f'<path d="{cloud_path(12, 96, 1.75)}" fill="#fff"/>'
        return d, b
    d += SHADOW + rad("sun", "#FFF3A0", "#FFA81E", 0.4, 0.35)
    d += lin("cl", "#FFFFFF", "#D5E6FA")
    # rays, then the sun, then the cloud in front
    rays = ""
    for i in range(8):
        a = math.radians(i * 45 + 22.5)
        x0, y0 = 76 + 26 * math.cos(a), 42 + 26 * math.sin(a)
        x1, y1 = 76 + 33 * math.cos(a), 42 + 33 * math.sin(a)
        rays += f'<line x1="{x0:.1f}" y1="{y0:.1f}" x2="{x1:.1f}" y2="{y1:.1f}"/>'
    b += f'<g stroke="#FFD43B" stroke-width="4.5" stroke-linecap="round">{rays}</g>'
    b += '<circle cx="76" cy="42" r="20" fill="url(#sun)" filter="url(#sh)"/>'
    b += f'<path d="{cloud_path(14, 94, 1.55)}" fill="url(#cl)" filter="url(#sh)"/>'
    return d, b


def notas(mini):
    d, b = TILE("#FFE45C", "#FFA90A", mini)
    if mini:
        b += '<rect x="24" y="18" width="72" height="86" rx="11" fill="#fff"/>'
        b += '<rect x="24" y="18" width="72" height="22" rx="11" fill="#FF8A00"/>'
        b += '<rect x="24" y="30" width="72" height="10" fill="#FF8A00"/>'
        b += '<rect x="34" y="56" width="52" height="9" rx="4.5" fill="#C9CCD3"/>'
        b += '<rect x="34" y="76" width="36" height="9" rx="4.5" fill="#C9CCD3"/>'
        return d, b
    d += SHADOW + WHITE + lin("band", "#FF9F2E", "#F07800")
    d += lin("pen", "#FFB84D", "#F07800", 1, 0) + lin("wood", "#FFE2B8", "#E8B77A")
    b += '<g filter="url(#sh)">'
    b += '<rect x="26" y="18" width="68" height="84" rx="10" fill="url(#wh)"/>'
    b += '</g>'
    b += '<path d="M26,28 a10,10 0 0 1 10,-10 h48 a10,10 0 0 1 10,10 v8 h-68 z" fill="url(#band)"/>'
    for i, w in enumerate((48, 48, 34)):
        b += f'<rect x="36" y="{50 + i * 13}" width="{w}" height="4.5" rx="2.25" fill="#C6CAD2"/>'
    # a pencil across the corner
    b += '<g transform="rotate(-45 86 84)" filter="url(#sh)">'
    b += '<rect x="62" y="79" width="34" height="10" rx="2" fill="url(#pen)"/>'
    b += '<rect x="96" y="79" width="7" height="10" rx="2" fill="#FF6F91"/>'
    b += '<rect x="93" y="79" width="4" height="10" fill="#C9CCD3"/>'
    b += '<path d="M62,79 L52,84 L62,89 Z" fill="url(#wood)"/>'
    b += '<path d="M55.5,82.2 L52,84 L55.5,85.8 Z" fill="#3A3A3C"/>'
    b += '</g>'
    return d, b


def metegol(mini):
    d, b = TILE("#3BB25A", "#167A35", mini)
    if mini:
        b += '<rect x="58" y="10" width="4" height="100" fill="#fff" opacity="0.8"/>'
        b += '<circle cx="60" cy="60" r="16" fill="none" stroke="#fff" stroke-width="5" opacity="0.8"/>'
        b += '<rect x="22" y="40" width="20" height="30" rx="7" fill="#2F6BFF"/>'
        b += '<rect x="78" y="50" width="20" height="30" rx="7" fill="#FF3B30"/>'
        b += '<circle cx="60" cy="86" r="11" fill="#fff"/>'
        return d, b
    d += SHADOW + lin("rod", "#F2F4F7", "#9BA3AE", 1, 0)
    d += lin("red", "#FF6B5E", "#D9261C") + lin("blue", "#5A8BFF", "#1F4FD6")
    d += rad("ball", "#FFFFFF", "#C9CED6", 0.38, 0.32)
    # mowed stripes, clipped to the tile
    d += '<clipPath id="tc">' + C.tile_path(120) + '/></clipPath>'
    stripes = "".join(f'<rect x="{x}" y="0" width="15" height="120" fill="#fff" opacity="0.07"/>'
                      for x in range(0, 120, 30))
    b += f'<g clip-path="url(#tc)">{stripes}</g>'
    b += '<g stroke="#fff" stroke-opacity="0.75" stroke-width="2.4" fill="none">'
    b += '<line x1="60" y1="8" x2="60" y2="112"/><circle cx="60" cy="60" r="15"/>'
    b += '<rect x="-2" y="38" width="16" height="44" rx="3"/><rect x="106" y="38" width="16" height="44" rx="3"/>'
    b += '</g>'

    def rod(x, grad, ys):
        s = f'<rect x="{x - 2.5}" y="-2" width="5" height="124" fill="url(#rod)" filter="url(#sh)"/>'
        for y in ys:
            s += f'<g filter="url(#sh)"><rect x="{x - 9}" y="{y - 13}" width="18" height="26" rx="7" fill="url(#{grad})"/>'
            s += f'<circle cx="{x}" cy="{y - 4}" r="5" fill="#FFD9B8"/></g>'
        return s
    b += rod(34, "blue", (34, 86))
    b += rod(86, "red", (34, 86))
    b += '<circle cx="62" cy="76" r="7.5" fill="url(#ball)" filter="url(#sh)"/>'
    return d, b


def store(mini):
    d, b = TILE("#4AAEFF", "#0B5FE0", mini)
    # an isometric box: top, left and right faces, a band of tape
    top = "M60,22 L96,40 L60,58 L24,40 Z"
    left = "M24,40 L60,58 L60,100 L24,82 Z"
    right = "M96,40 L60,58 L60,100 L96,82 Z"
    if mini:
        b += f'<path d="{top}" fill="#fff" stroke="#fff" stroke-width="6" stroke-linejoin="round"/>'
        b += f'<path d="{left}" fill="#DCE7F5" stroke="#DCE7F5" stroke-width="6" stroke-linejoin="round"/>'
        b += f'<path d="{right}" fill="#B4C8E2" stroke="#B4C8E2" stroke-width="6" stroke-linejoin="round"/>'
        return d, b
    d += SHADOW + lin("tp", "#FFFFFF", "#EEF3FA") + lin("lf", "#E3EBF6", "#C9D6E8")
    d += lin("rt", "#BFD0E6", "#9EB3D0") + lin("tape", "#FFC94A", "#F2A100")
    b += '<g filter="url(#sh)" stroke-linejoin="round" stroke-width="4">'
    b += f'<path d="{left}" fill="url(#lf)" stroke="url(#lf)"/>'
    b += f'<path d="{right}" fill="url(#rt)" stroke="url(#rt)"/>'
    b += f'<path d="{top}" fill="url(#tp)" stroke="url(#tp)"/>'
    b += '</g>'
    # the tape: along the middle of the top, then down the right face
    b += '<path d="M46,29 L82,47 L74,51 L38,33 Z" fill="url(#tape)"/>'
    b += '<path d="M74,51 L82,47 L82,89 L74,93 Z" fill="#E59A00"/>'
    # a label on the left face
    b += '<path d="M32,62 L50,71 L50,81 L32,72 Z" fill="#fff" opacity="0.9"/>'
    b += '<path d="M35,67 L46,72.5 M35,71 L42,74.5" stroke="#9EB3D0" stroke-width="1.6" stroke-linecap="round"/>'
    return d, b


def clock(mini):
    d, b = TILE("#3A3A3E", "#0B0B0D", mini)
    if mini:
        b += '<circle cx="60" cy="60" r="40" fill="#fff"/>'
        b += ('<path d="M60,34 V60 L76,70" fill="none" stroke="#1C1C1E" stroke-width="9" '
              'stroke-linecap="round" stroke-linejoin="round"/>')
        return d, b
    d += SHADOW + WHITE
    b += '<circle cx="60" cy="60" r="40" fill="url(#wh)" filter="url(#sh)"/>'
    for i in range(12):
        a = math.radians(i * 30)
        r0 = 30 if i % 3 else 27
        b += (f'<line x1="{60 + r0 * math.sin(a):.1f}" y1="{60 - r0 * math.cos(a):.1f}" '
              f'x2="{60 + 34 * math.sin(a):.1f}" y2="{60 - 34 * math.cos(a):.1f}" '
              f'stroke="#3A3A3C" stroke-width="{3 if i % 3 == 0 else 1.6}" stroke-linecap="round"/>')
    b += '<path d="M60,60 L60,36" stroke="#1C1C1E" stroke-width="4.5" stroke-linecap="round"/>'
    b += '<path d="M60,60 L77,70" stroke="#1C1C1E" stroke-width="4.5" stroke-linecap="round"/>'
    b += '<path d="M60,66 L60,30" stroke="#FF9F0A" stroke-width="2" stroke-linecap="round"/>'
    b += '<circle cx="60" cy="60" r="4" fill="#FF9F0A"/>'
    return d, b


def calc(mini):
    d, b = TILE("#F2B93B", "#8A5A2E", mini)
    if mini:
        b += '<rect x="28" y="16" width="64" height="88" rx="12" fill="#fff"/>'
        b += '<rect x="38" y="26" width="44" height="18" rx="4" fill="#5B3B1E"/>'
        for r in range(2):
            for c in range(2):
                b += f'<rect x="{38 + c * 26}" y="{54 + r * 24}" width="18" height="16" rx="4" fill="#E0A21A"/>'
        return d, b
    d += SHADOW + WHITE + lin("key", "#FFC24D", "#E08A00") + lin("lcd", "#5A6B4E", "#3A4733")
    b += '<rect x="28" y="14" width="64" height="92" rx="12" fill="url(#wh)" filter="url(#sh)"/>'
    b += '<rect x="36" y="22" width="48" height="22" rx="5" fill="url(#lcd)"/>'
    b += '<text x="80" y="39" font-family="Helvetica" font-weight="bold" font-size="15" fill="#C8F0A0" text-anchor="end">42</text>'
    for r in range(3):
        for c in range(3):
            fill = "url(#key)" if c == 2 else "#D9DCE3"
            b += f'<rect x="{37 + c * 16}" y="{52 + r * 17}" width="12" height="12" rx="3.5" fill="{fill}"/>'
    return d, b


def photos(mini):
    d, b = TILE("#FFC83D", "#EC4899", mini)
    if mini:
        b += '<rect x="18" y="26" width="84" height="68" rx="12" fill="#fff"/>'
        b += '<path d="M24,90 L50,56 L66,74 L78,62 L98,90 Z" fill="#EC4899"/>'
        b += '<circle cx="78" cy="44" r="9" fill="#FFB020"/>'
        return d, b
    d += SHADOW + WHITE + lin("mtn", "#FF6FA8", "#D61F69") + lin("mtn2", "#FFA05A", "#F06A1E")
    b += '<rect x="18" y="26" width="84" height="68" rx="11" fill="url(#wh)" filter="url(#sh)"/>'
    b += '<clipPath id="ph"><rect x="25" y="33" width="70" height="54" rx="6"/></clipPath>'
    b += '<g clip-path="url(#ph)"><rect x="25" y="33" width="70" height="54" fill="#DDEBFF"/>'
    b += '<circle cx="78" cy="48" r="7" fill="#FFB020"/>'
    b += '<path d="M20,92 L50,56 L70,80 Z" fill="url(#mtn)"/>'
    b += '<path d="M52,92 L74,66 L100,92 Z" fill="url(#mtn2)"/></g>'
    return d, b


DRAW = {
    "aos.settings": settings, "aos.serial": serial, "aos.clima": clima,
    "aos.notas": notas, "demo.metegol": metegol, "aos.store": store,
    "aos.clock": clock, "aos.calc": calc, "aos.photos": photos,
}


def main():
    for app_id, _name, _idea in C.ALL:
        for mini in (False, True):
            defs, body = DRAW[app_id](mini)
            svg = C.svg_doc(120, body, defs)
            C.keep_svg(STYLE, app_id, "mini" if mini else "big", svg)
            for size in ((26,) if mini else (120, 108)):
                C.svg_to_png(svg, C.out_path(STYLE, app_id, size), size)
    print("ok:", C.OUT + "/" + STYLE)


if __name__ == "__main__":
    sys.exit(main())
