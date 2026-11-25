"""'linea' models: the system's apps (juguete's sistema-1 and sistema-2, and Zigbee)."""

import math

from lib import KO, C, L, P, R, arc, cloud, gear, poly, star, waves

PAIRS = {
    "aos.settings": ("#E2E6F0", "#8C7CFF"),
    "aos.store": ("#62C3FF", "#B266FF"),
    "aos.music": ("#FF9AB0", "#FF2D6B"),
    "aos.ha": ("#8FE3FF", "#18A8F0"),
    "aos.zigbee": ("#FF9AB4", "#EB0443"),
}


def settings(m):
    if m.mini:
        return gear(60, 60, 42, 31, 8) + C(60, 60, 11), ""
    return gear(60, 60, 40, 30, 9) + C(60, 60, 13), ""


def store(m):
    box = poly([(60, 20), (96, 39), (96, 81), (60, 100), (24, 81), (24, 39)])
    lines = box + P("M24,39 L60,58 L96,39") + L(60, 58, 60, 100)
    if not m.mini:
        lines += P("M42,29.5 L78,48.5 V62")
    return lines, ""


def music(m):
    lines = L(46, 84, 46, 30) + L(86, 76, 86, 22) + P("M46,30 L86,22")
    if not m.mini:
        lines += P("M46,42 L86,34")
    fills = (f'<ellipse cx="36" cy="86" rx="12" ry="9" transform="rotate(-20 36 86)"/>'
             f'<ellipse cx="76" cy="78" rx="12" ry="9" transform="rotate(-20 76 78)"/>')
    return lines, fills


def ha(m):
    """A house with a small network inside: evokes, does not copy, the HA mark."""
    house = poly([(22, 98), (22, 52), (60, 20), (98, 52), (98, 98)])
    if m.mini:
        return house + L(60, 84, 60, 58), C(60, 56, 7) + C(60, 84, 7)
    net = L(60, 86, 60, 62) + L(60, 62, 46, 50) + L(60, 62, 74, 54)
    return house + net, C(60, 86, 6) + C(60, 62, 6) + C(46, 50, 6) + C(74, 54, 6)


def zigbee(m):
    """Today's glyph: a dot, two pairs of arcs."""
    n = 2
    lines = waves(60, 60, n, 20, 15 if not m.mini else 17, -45, 45) + waves(60, 60, n, 20, 15 if not m.mini else 17, 135, 225)
    return lines, C(60, 60, 9 if not m.mini else 11)


ART = {
    "aos.settings": settings, "aos.store": store, "aos.music": music, "aos.ha": ha, "aos.zigbee": zigbee,
}
