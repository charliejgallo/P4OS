"""'linea' models: utilities, programming and the rest (juguete's utiles-programar, pruebas)."""

import math

from lib import KO, C, L, P, R, arc, cloud, poly, star, waves

PAIRS = {
    "aos.clima": ("#FFD75A", "#3FA9FF"),
    "aos.python": ("#FFE36A", "#3FA0FF"),
}


def clima(m):
    if m.mini:
        return C(80, 36, 18) + cloud(10, 100, 1.6, KO), ""
    rays = "".join(L(76 + 27 * math.cos(math.radians(a)), 42 - 27 * math.sin(math.radians(a)),
                     76 + 34 * math.cos(math.radians(a)), 42 - 34 * math.sin(math.radians(a))) for a in range(0, 360, 45))
    return C(76, 42, 18) + rays + cloud(14, 94, 1.55, KO), ""


def python(m):
    """A snake: an S, its head, an eye."""
    body = P("M26,96 C60,104 92,88 78,70 C66,54 38,62 40,44 C42,28 68,26 80,32")
    if m.mini:
        return body, C(84, 32, 12)
    return body + P("M96,32 L104,28 M96,32 L104,36"), C(84, 32, 11) + C(87, 29, 3, 'fill="#16181E"')


ART = {"aos.clima": clima, "aos.python": python}
