"""'linea' models: network apps (juguete's red)."""

import math

from lib import KO, C, L, P, R, arc, poly, waves

PAIRS = {
    "aos.cameras": ("#C8D6E8", "#5EA8FF"),
}


def cameras(m):
    body = '<g transform="rotate(-20 56 50)">' + R(22, 36, 64, 30, 10) + "</g>"
    lens = C(30, 58, 8, KO)
    mount = P("M74,66 L80,86 M64,92 L96,92")
    lines = body + lens + mount
    fills = "" if m.mini else C(68, 40, 3.5)
    return lines, fills


ART = {"aos.cameras": cameras}
