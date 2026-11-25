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


def ble(m):
    """A chip and its radio waves (juguete's b)."""
    lines = R(22, 58, 40, 40, 8) + waves(42, 78, 3 if not m.mini else 2, 30, 14, 10, 80)
    return lines, R(34, 70, 16, 16, 3)


def cartero(m):
    lines = poly([(16, 58), (102, 20), (76, 98), (56, 70)]) + L(56, 70, 102, 20)
    if not m.mini:
        lines += P("M56,70 L52,94 L66,80")
    return lines, ""


def estado(m):
    """A house and a traffic light (juguete's a)."""
    lines = poly([(14, 98), (14, 60), (42, 36), (70, 60), (70, 98)]) + R(78, 24, 26, 74, 10)
    fills = C(91, 85, 7)
    if not m.mini:
        lines += C(91, 39, 6) + C(91, 62, 6) + R(34, 76, 16, 22, 4)
    return lines, fills


def impresoras(m):
    lines = P("M22,98 V22 H98 V98") + L(14, 98, 106, 98) + R(46, 30, 28, 16, 4) + L(60, 46, 60, 58)
    if not m.mini:
        lines += L(22, 38, 98, 38)
        return lines, R(46, 72, 28, 22, 4)
    return lines, R(46, 70, 28, 24, 4)


def ssh(m):
    """A terminal with a padlock (juguete's b)."""
    lines = R(12, 20, 76, 58, 10) + P("M24,38 L36,49 L24,60") + L(42, 62, 58, 62)
    lines += R(62, 62, 40, 34, 6, KO) + P("M71,62 V54 A11,11 0 0 1 93,54 V62")
    return lines, "" if m.mini else C(82, 79, 4)


def vnc(m):
    lines = R(14, 20, 92, 62, 8) + L(60, 82, 60, 96) + L(42, 98, 78, 98)
    pointer = poly([(54, 34), (54, 66), (62, 58), (68, 70), (74, 67), (68, 56), (78, 56)])
    return lines, pointer


def vuelos(m):
    """A radar with its sweep and two blips (juguete's b)."""
    lines = C(60, 60, 42) + L(60, 60, 88, 30)
    if not m.mini:
        lines += C(60, 60, 24)
    return lines, C(60, 60, 5) + C(40, 78, 6) + ("" if m.mini else C(84, 72, 4.5))


ART.update({
    "aos.ble": ble, "aos.cartero": cartero, "aos.estado": estado, "aos.impresoras": impresoras, "aos.ssh": ssh,
    "aos.vnc": vnc, "aos.vuelos": vuelos,
})
PAIRS.update({"aos.ssh": ("#7CFFB2", "#FFC62E"), "aos.vuelos": ("#9CFF5A", "#2DC8A0")})
