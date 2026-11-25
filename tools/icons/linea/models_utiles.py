"""'linea' models: utilities, programming and the rest (juguete's utiles-programar, pruebas)."""

import math

from lib import KO, TILE_C, C, L, P, R, arc, cloud, poly, star, waves

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


def T(x, y, size, txt, extra=""):
    return (f'<text x="{x}" y="{y}" font-family="Helvetica" font-weight="bold" font-size="{size}" '
            f'text-anchor="middle" {extra}>{txt}</text>')


def bolsa(m):
    """Bars and a coin (juguete's b)."""
    lines = R(18, 64, 16, 34, 4) + R(40, 48, 16, 50, 4) + C(78, 72, 22, KO)
    fills = T(78, 81, 26, "$")
    if not m.mini:
        lines += R(62, 24, 16, 30, 4)
    return lines, fills


def cas(m):
    return "", T(60, 74, 40 if not m.mini else 44, "f(x)")


def cotiz(m):
    lines = C(60, 60, 38)
    if not m.mini:
        lines += C(60, 60, 29)
    return lines, T(60, 74, 40, "$")


def pato(m):
    body = P("M20,66 C22,96 96,98 98,70 C99,60 88,56 80,62 L64,64 C50,54 26,52 20,66 Z")
    head = C(74, 40, 17, KO)
    lines = body + head + ("" if m.mini else P("M40,74 C50,82 64,80 70,72"))
    return lines, poly([(90, 38), (106, 44), (90, 48)]) + C(78, 36, 3.5 if not m.mini else 4.5)


def traductor(m):
    a = P("M14,22 H64 Q70,22 70,28 V54 Q70,60 64,60 H34 L24,70 V60 H20 Q14,60 14,54 V28 Q14,22 20,22 Z")
    b = P("M50,54 H100 Q106,54 106,60 V86 Q106,92 100,92 H96 V102 L86,92 H56 Q50,92 50,86 V60 Q50,54 56,54 Z", KO)
    return a + b, T(42, 50, 24, "A") + T(78, 83, 24, "Z")


def mapas(m):
    lines = P("M16,30 L42,22 L78,30 L104,22 V90 L78,98 L42,90 L16,98 Z") + L(42, 22, 42, 90) + L(78, 30, 78, 98)
    pin = P("M60,72 C46,56 46,42 60,40 C74,42 74,56 60,72 Z", KO)
    return lines + pin, C(60, 52, 5)


def visor3d(m):
    lines = R(24, 40, 52, 52, 4) + R(44, 24, 52, 52, 4) + L(24, 40, 44, 24) + L(76, 40, 96, 24) + L(24, 92, 44, 76)
    lines += L(76, 92, 96, 76)
    return lines, ""


def lua(m):
    moon = P("M74,22 C50,22 32,40 32,62 C32,84 50,100 72,100 C86,100 98,94 104,82 C80,88 58,72 58,50 "
             "C58,38 64,28 74,22 Z")
    return moon, star(84, 42, 12, 5) + ("" if m.mini else star(98, 62, 6, 2.5))


def tortuga(m):
    """A turtle seen from the side: a domed shell, the head, the feet."""
    lines = P("M18,78 C18,42 84,42 84,78 Z") + P("M84,70 C90,60 102,58 106,66 C108,74 96,78 84,76")
    lines += L(30, 78, 28, 92) + L(70, 78, 72, 92)
    if not m.mini:
        lines += P("M34,62 L44,52 L58,52 L68,62") + L(51, 52, 51, 76)
    return lines, C(99, 64, 2.8)


def hello(m):
    bub = P("M18,26 H102 Q108,26 108,32 V72 Q108,78 102,78 H52 L36,96 V78 H18 Q12,78 12,72 V32 Q12,26 18,26 Z")
    return bub, T(60, 62, 28 if not m.mini else 30, "Hola" if not m.mini else "Hi")


def blittest(m):
    n = 7 if not m.mini else 4
    w = 80 / n
    fills = "".join(R(20 + i * w + 1, 26, w - 3, 68, 3, f'opacity="{1 - i * 0.6 / n:.2f}"') for i in range(n))
    return "", fills


ART.update({
    "aos.bolsa": bolsa, "aos.cas": cas, "aos.cotiz": cotiz, "aos.pato": pato, "aos.traductor": traductor,
    "demo.mapas": mapas, "demo.visor3d": visor3d, "aos.lua": lua, "demo.tortuga": tortuga,
    "demo.hello": hello, "dev.blittest": blittest,
})
PAIRS.update({
    "aos.pato": ("#FFE36A", "#FF9F0A"), "aos.lua": ("#FFF2A0", "#6A7CFF"), "aos.cotiz": ("#FFE36A", "#2EC85A"),
    "aos.bolsa": ("#9CFF5A", "#2EC8A0"), "aos.traductor": ("#8FD0FF", "#FFC62E"),
})
