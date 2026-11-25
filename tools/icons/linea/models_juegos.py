"""'linea' models: the games (juguete's juegos-1, juegos-2 and juegos-3)."""

import math

from lib import KO, TILE_C, C, L, P, R, arc, heart, poly, star, waves

PAIRS = {
    "aos.dice": ("#E0B0FF", "#9B4DFF"),
    "demo.mila": ("#FFB0D0", "#FF5AA0"),
    "demo.tanques": ("#B6FF6A", "#3FB0FF"),
    "demo.sudoku": ("#9ED0FF", "#4F6BFF"),
}


def dice(m):
    a = '<g transform="rotate(-12 44 46)">' + R(22, 24, 44, 44, 10) + "</g>"
    b = '<g transform="rotate(14 78 78)">' + R(58, 58, 40, 40, 9, KO) + "</g>"
    if m.mini:
        return a + b, C(44, 46, 5) + C(78, 78, 5)
    pa = "".join(C(44 + dx, 46 + dy, 4) for dx, dy in ((-11, -11), (11, -11), (0, 0), (-11, 11), (11, 11)))
    pb = '<g transform="rotate(14 78 78)">' + "".join(C(78 + dx, 78 + dy, 3.6) for dx, dy in ((-9, -9), (0, 0), (9, 9))) + "</g>"
    return a + b, '<g transform="rotate(-12 44 46)">' + pa + "</g>" + pb


def mila(m):
    """Mila: a round head, two ears, the eyes; whiskers on the big one."""
    head = P("M30,62 C30,40 42,30 60,30 C78,30 90,40 90,62 C90,84 78,96 60,96 C42,96 30,84 30,62 Z")
    ears = P("M34,46 L30,18 L52,32") + P("M86,46 L90,18 L68,32")
    lines = head + ears
    fills = (f'<ellipse cx="48" cy="62" rx="6" ry="8"/><ellipse cx="72" cy="62" rx="6" ry="8"/>')
    if not m.mini:
        lines += P("M22,72 L40,74 M22,82 L40,80 M98,72 L80,74 M98,82 L80,80")
        fills += poly([(56, 74), (64, 74), (60, 79)])
    return lines, fills


def tanques(m):
    body = R(26, 56, 68, 26, 10)
    tracks = R(18, 76, 84, 16, 8)
    turret = P("M42,56 C42,40 78,40 78,56")
    barrel = L(66, 46, 100, 30)
    lines = body + turret + barrel + tracks
    fills = ""
    if not m.mini:
        fills = "".join(C(x, 84, 3.5) for x in (32, 46, 60, 74, 88))
    return lines, fills


def sudoku(m):
    lines = R(20, 20, 80, 80, 12)
    if not m.mini:
        lines += L(46.7, 24, 46.7, 96) + L(73.3, 24, 73.3, 96) + L(24, 46.7, 96, 46.7) + L(24, 73.3, 96, 73.3)
        fills = ('<text x="60" y="70" font-family="Helvetica" font-weight="bold" font-size="24" '
                 'text-anchor="middle">9</text>'
                 '<text x="33.5" y="43" font-family="Helvetica" font-weight="bold" font-size="18" '
                 'text-anchor="middle">5</text>'
                 '<text x="86.5" y="96" font-family="Helvetica" font-weight="bold" font-size="18" '
                 'text-anchor="middle">1</text>')
        return lines, fills
    return lines + L(60, 24, 60, 96) + L(24, 60, 96, 60), ""


ART = {"aos.dice": dice, "demo.mila": mila, "demo.tanques": tanques, "demo.sudoku": sudoku}
