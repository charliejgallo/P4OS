"""'linea' models: making things, sound and picture (juguete's crear-medios)."""

import math

from lib import KO, C, L, P, R, arc, heart, poly, waves

PAIRS = {
    "aos.notas": ("#FFE36A", "#FF7A2A"),
    "aos.pixel": ("#FFB0C8", "#FF2D6B"),
    "aos.dj": ("#5EC8FF", "#FF7A3D"),
}


def dibujo(m):
    """A painter's palette (juguete's b)."""
    pal = P("M60,18 C88,18 104,38 102,58 C100,74 86,72 78,78 C70,84 82,100 60,102 C34,104 16,84 16,60 "
            "C16,36 34,18 60,18 Z")
    lines = pal + C(44, 80, 8, KO)
    dots = ((42, 38), (64, 32), (84, 46), (30, 58)) if not m.mini else ((44, 40), (76, 40))
    return lines, "".join(C(x, y, 7 if not m.mini else 9) for x, y in dots)


def notas(m):
    """A sticky note with a pin (juguete's b)."""
    lines = P("M22,26 H98 V78 L76,100 H22 Z") + P("M98,78 H76 V100")
    if not m.mini:
        lines += L(34, 50, 82, 50) + L(34, 66, 70, 66)
    return lines, C(60, 26, 7)


def oficina(m):
    lines = R(32, 14, 58, 76, 8) + R(22, 26, 58, 80, 8, KO)
    if not m.mini:
        for y in (48, 64, 80):
            lines += L(32, y, 70, y)
        lines += L(46, 40, 46, 94)
        return lines, R(30, 34, 42, 8, 2)
    return lines + L(32, 56, 70, 56) + L(32, 76, 70, 76), ""


def pixel(m):
    pat = (["0110110", "1111111", "1111111", "0111110", "0011100", "0001000"] if not m.mini
           else ["01010", "11111", "01110", "00100"])
    k = 80 / len(pat[0])
    fills = ""
    for r, row in enumerate(pat):
        for c, ch in enumerate(row):
            if ch == "1":
                fills += R(20 + c * k + 1, 28 + r * k + 1, k - 2.5, k - 2.5, 2)
    return "", fills


def dj(m):
    lines = R(14, 22, 92, 76, 10) + C(52, 60, 26) + P("M92,32 V62 L76,74")
    if not m.mini:
        lines += C(52, 60, 15)
    return lines, C(52, 60, 5)


def estudio(m):
    lines = R(12, 34, 96, 54, 8)
    n = 7 if not m.mini else 4
    w = 96 / n
    for i in range(1, n):
        lines += L(12 + i * w, 60, 12 + i * w, 88)
    fills = "".join(R(12 + i * w - w * 0.3, 34, w * 0.6, 26, 2) for i in range(1, n) if i % 7 not in (3,) or m.mini)
    return lines, fills


def radio(m):
    lines = R(14, 38, 92, 60, 12) + P("M36,38 C36,22 84,22 84,38") + C(44, 68, 16) + C(84, 60, 9)
    if not m.mini:
        lines += C(44, 68, 7)
    return lines, C(84, 84, 4.5)


def tuner(m):
    return P("M46,16 V56 C46,72 74,72 74,56 V16") + L(60, 70, 60, 106), ""


def video(m):
    lines = R(18, 50, 84, 50, 6)
    top = '<g transform="rotate(-12 18 44)">' + R(18, 32, 84, 16, 4) + "</g>"
    lines += top
    if not m.mini:
        lines += L(30, 66, 60, 66) + L(30, 80, 52, 80)
        top += ""
    return lines, ""


def recorder(m):
    lines = R(44, 16, 32, 50, 16) + P("M34,54 C34,82 86,82 86,54") + L(60, 78, 60, 96) + L(44, 98, 76, 98)
    if not m.mini:
        lines += L(50, 32, 70, 32) + L(50, 44, 70, 44)
    return lines, ""


ART = {
    "aos.dibujo": dibujo, "aos.notas": notas, "aos.oficina": oficina, "aos.pixel": pixel, "aos.dj": dj,
    "aos.estudio": estudio, "aos.radio": radio, "aos.tuner": tuner, "aos.video": video, "app.recorder": recorder,
}



def _corners(m, e=18, arm=16):
    out = ""
    for x0, y0, dx, dy in ((e, e, 1, 1), (120 - e, e, -1, 1), (e, 120 - e, 1, -1), (120 - e, 120 - e, -1, -1)):
        out += P(f"M{x0},{y0 + dy * arm} V{y0} H{x0 + dx * arm}")
    return out


def vision(m):
    """An eye between a frame's four corners (the app's own icon, juguete's a)."""
    eye = P("M28,60 C40,40 80,40 92,60 C80,80 40,80 28,60 Z")
    lines = _corners(m) + eye
    if not m.mini:
        lines += C(60, 60, 11)
    return lines, C(60, 60, 5 if not m.mini else 8)


def vision_b(m):
    """A round webcam inside a detection box (juguete's b)."""
    lines = _corners(m, 16, 14) + C(60, 52, 20) + P("M50,72 V82 H70 V72") + L(40, 88, 80, 88)
    return lines, C(60, 52, 7)


ART["aos.vision"] = vision
PAIRS["aos.vision"] = ("#B6FF6A", "#22C55E")
