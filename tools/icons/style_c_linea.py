#!/usr/bin/env python3
"""Style C - a fine line in two colours, on a dark tile.

    python3 tools/icons/style_c_linea.py        -> tools/icons/muestra/c_linea/

Every tile is the same near-black, with a hairline edge and a faint halo of
the app's colour behind the drawing; the drawing is a line (6.5 units of 120,
round ends and joins) painted with a gradient between the app's two colours,
over a blurred copy of itself so it glows a little. The colour lives only in
the line, so a page of these reads as one calm family and the apps are told
apart by their two colours and their shape.

The 26 px miniature cannot hold a 1.4 px line with a glow: it is the same
idea as a filled silhouette with the same gradient, on the same dark tile.

Recipe, for production:
  - pick the two colours (PAIRS below): a light one at the top left and a
    deeper one at the bottom right, from neighbouring hues;
  - draw with LINE (stroke 6.5, round), dots and small fills with the same
    gradient; things behind others are covered with the tile's colour (TILE_C);
  - mini: a silhouette filled with the gradient, nothing under 5 units.
"""

import math
import sys

import common as C
import style_a_plano as A        # the cloud's outline, the gear

STYLE = "c_linea"
TILE_C = "#16181E"

PAIRS = {
    "aos.settings": ("#E2E6F0", "#8C7CFF"),
    "aos.serial":   ("#7CFFB2", "#1FB8E8"),
    "aos.clima":    ("#FFD75A", "#3FA9FF"),
    "aos.notas":    ("#FFE36A", "#FF7A2A"),
    "demo.metegol": ("#B6FF6A", "#22C59A"),
    "aos.store":    ("#62C3FF", "#B266FF"),
    "aos.clock":    ("#FFFFFF", "#FF9F0A"),
    "aos.calc":     ("#FFD36A", "#E07A1A"),
    "aos.photos":   ("#FFC83D", "#FF3D8B"),
}


def frame(app_id, mini):
    c0, c1 = PAIRS[app_id]
    defs = (f'<linearGradient id="bg" x1="0" y1="0" x2="0" y2="1">'
            f'<stop offset="0" stop-color="#262932"/><stop offset="1" stop-color="#0D0E12"/></linearGradient>'
            f'<radialGradient id="halo" cx="0.5" cy="0.5" r="0.5">'
            f'<stop offset="0" stop-color="{c1}" stop-opacity="0.22"/>'
            f'<stop offset="1" stop-color="{c1}" stop-opacity="0"/></radialGradient>'
            f'<linearGradient id="ink" gradientUnits="userSpaceOnUse" x1="22" y1="18" x2="98" y2="102">'
            f'<stop offset="0" stop-color="{c0}"/><stop offset="1" stop-color="{c1}"/></linearGradient>'
            '<filter id="glow" x="-30%" y="-30%" width="160%" height="160%">'
            '<feGaussianBlur stdDeviation="3.2"/></filter>')
    body = C.tile_path(120) + ' fill="url(#bg)"/>'
    body += '<circle cx="60" cy="62" r="52" fill="url(#halo)"/>'
    if not mini:
        body += C.tile_path(120, 0.6) + (' fill="none" stroke="#fff" stroke-opacity="0.10" '
                                         'stroke-width="1.2"/>')
    return defs, body


def lined(defs, body, art):
    """The art twice: blurred and faint underneath, sharp on top."""
    g = ('<g fill="none" stroke="url(#ink)" stroke-width="6.5" stroke-linecap="round" '
         'stroke-linejoin="round">' + art + '</g>')
    return defs, body + f'<g filter="url(#glow)" opacity="0.65">{g}</g>' + g


def settings(mini):
    d, b = frame("aos.settings", mini)
    if mini:
        g = C.gear_path(60, 60, 44, 33, 8, 0.52, hole=14)
        return d, b + (f'<path d="{g}" fill="url(#ink)" fill-rule="evenodd" stroke="url(#ink)" '
                       'stroke-width="6" stroke-linejoin="round"/>')
    g = C.gear_path(60, 60, 40, 30, 9, 0.5)
    return lined(d, b, f'<path d="{g}"/><circle cx="60" cy="60" r="13"/>')


def serial(mini):
    d, b = frame("aos.serial", mini)
    if mini:
        b += ('<path d="M22,38 H98 L88,78 H32 Z" fill="url(#ink)" stroke="url(#ink)" '
              'stroke-width="8" stroke-linejoin="round"/>')
        b += f'<path d="M36,50 H84 L79,68 H41 Z" fill="{TILE_C}"/>'
        b += '<rect x="53" y="82" width="14" height="24" rx="4" fill="url(#ink)"/>'
        return d, b
    art = '<path d="M20,34 H100 L90,76 H30 Z"/><path d="M60,77 V106"/>'
    dots = "".join(f'<circle cx="{38 + i * 11}" cy="48" r="3.6"/>' for i in range(5))
    dots += "".join(f'<circle cx="{43.5 + i * 11}" cy="62" r="3.6"/>' for i in range(4))
    d, b = lined(d, b, art)
    return d, b + f'<g fill="url(#ink)">{dots}</g>'


def clima(mini):
    d, b = frame("aos.clima", mini)
    cloud = A.cloud_path(14, 94, 1.55)
    if mini:
        b += '<circle cx="76" cy="44" r="24" fill="url(#ink)"/>'
        b += f'<path d="{A.cloud_path(12, 96, 1.75)}" fill="url(#ink)" stroke="{TILE_C}" stroke-width="7"/>'
        return d, b
    rays = ""
    for i in range(8):
        a = math.radians(i * 45)
        rays += (f'<path d="M{76 + 27 * math.cos(a):.1f},{42 + 27 * math.sin(a):.1f} '
                 f'L{76 + 34 * math.cos(a):.1f},{42 + 34 * math.sin(a):.1f}"/>')
    art = f'<circle cx="76" cy="42" r="18"/>{rays}'
    # the cloud hides what is behind it, then is drawn as a line too
    art += f'<path d="{cloud}" fill="{TILE_C}"/>'
    return lined(d, b, art)


def notas(mini):
    d, b = frame("aos.notas", mini)
    if mini:
        b += '<rect x="24" y="16" width="72" height="88" rx="12" fill="url(#ink)"/>'
        b += f'<rect x="36" y="46" width="48" height="9" rx="4.5" fill="{TILE_C}"/>'
        b += f'<rect x="36" y="66" width="48" height="9" rx="4.5" fill="{TILE_C}"/>'
        b += f'<rect x="36" y="86" width="30" height="9" rx="4.5" fill="{TILE_C}"/>'
        return d, b
    art = ('<rect x="28" y="18" width="64" height="84" rx="11"/>'
           '<path d="M28,36 H92"/>'
           '<path d="M40,54 H80"/><path d="M40,70 H80"/><path d="M40,86 H64"/>')
    return lined(d, b, art)


def metegol(mini):
    d, b = frame("demo.metegol", mini)
    if mini:
        b += '<rect x="16" y="20" width="88" height="80" rx="12" fill="none" stroke="url(#ink)" stroke-width="9"/>'
        b += '<rect x="30" y="40" width="16" height="40" rx="7" fill="url(#ink)"/>'
        b += '<rect x="74" y="40" width="16" height="40" rx="7" fill="url(#ink)"/>'
        b += '<circle cx="60" cy="60" r="9" fill="url(#ink)"/>'
        return d, b
    art = ('<rect x="14" y="22" width="92" height="76" rx="10"/>'
           '<path d="M60,22 V98"/><circle cx="60" cy="60" r="12"/>'
           '<path d="M36,10 V110"/><path d="M84,10 V110"/>')
    d, b = lined(d, b, art)
    players = "".join(f'<rect x="{x - 7}" y="{y - 9}" width="14" height="18" rx="5"/>'
                      for x in (36, 84) for y in (40, 80))
    return d, b + f'<g fill="url(#ink)">{players}<circle cx="70" cy="68" r="5"/></g>'


def store(mini):
    d, b = frame("aos.store", mini)
    if mini:
        hexa = "M60,16 L98,36 L98,82 L60,104 L22,82 L22,36 Z"
        b += f'<path d="{hexa}" fill="url(#ink)" stroke="url(#ink)" stroke-width="6" stroke-linejoin="round"/>'
        b += (f'<path d="M22,36 L60,56 L98,36 M60,56 V104" fill="none" stroke="{TILE_C}" '
              'stroke-width="7" stroke-linejoin="round"/>')
        return d, b
    art = ('<path d="M60,20 L96,39 L96,81 L60,100 L24,81 L24,39 Z"/>'
           '<path d="M24,39 L60,58 L96,39"/><path d="M60,58 V100"/>'
           '<path d="M42,29.5 L78,48.5 V62"/>')
    return lined(d, b, art)


def clock(mini):
    d, b = frame("aos.clock", mini)
    if mini:
        b += '<circle cx="60" cy="60" r="40" fill="url(#ink)"/>'
        return d, b + (f'<path d="M60,36 V60 L76,70" fill="none" stroke="{TILE_C}" stroke-width="9" '
                       'stroke-linecap="round" stroke-linejoin="round"/>')
    return lined(d, b, '<circle cx="60" cy="60" r="38"/><path d="M60,36 V60 L76,70"/>')


def calc(mini):
    d, b = frame("aos.calc", mini)
    if mini:
        b += '<rect x="28" y="16" width="64" height="88" rx="12" fill="url(#ink)"/>'
        b += f'<rect x="38" y="26" width="44" height="18" rx="4" fill="{TILE_C}"/>'
        for r in range(2):
            for c in range(2):
                b += f'<rect x="{38 + c * 26}" y="{54 + r * 24}" width="18" height="16" rx="4" fill="{TILE_C}"/>'
        return d, b
    d, b = lined(d, b, '<rect x="30" y="16" width="60" height="88" rx="11"/><path d="M42,34 H78"/>')
    keys = "".join(f'<circle cx="{44 + c * 16}" cy="{58 + r * 16}" r="4"/>' for r in range(3) for c in range(3))
    return d, b + f'<g fill="url(#ink)">{keys}</g>'


def photos(mini):
    d, b = frame("aos.photos", mini)
    if mini:
        b += '<rect x="18" y="26" width="84" height="68" rx="12" fill="url(#ink)"/>'
        b += f'<path d="M28,86 L50,58 L64,74 L76,64 L92,86 Z" fill="{TILE_C}"/>'
        return d, b + f'<circle cx="78" cy="44" r="8" fill="{TILE_C}"/>'
    return lined(d, b, '<rect x="18" y="26" width="84" height="68" rx="11"/>'
                       '<path d="M26,86 L50,58 L66,76 L76,66 L94,86"/><circle cx="78" cy="45" r="6"/>')


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
