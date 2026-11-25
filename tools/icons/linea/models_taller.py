"""'linea' models: the workshop (juguete's taller)."""

import math

from lib import KO, C, L, P, R, arc, poly, waves

PAIRS = {
    "aos.luces": ("#FFE07A", "#FF5AC8"),
    "aos.logica": ("#B6FF6A", "#22C59A"),
    "aos.plotter": ("#7CFFB2", "#1FB8E8"),
}


def _sine(x0, x1, y, amp, cycles, phase=0.0, n=40):
    return poly([(x0 + (x1 - x0) * i / n, y - amp * math.sin(phase + cycles * 2 * math.pi * i / n)) for i in range(n + 1)],
                close=False)


def can(m):
    lines = _sine(26, 94, 60, 10, 2) + _sine(26, 94, 60, 10, 2, math.pi)
    lines += R(10, 48, 16, 24, 5) + R(94, 48, 16, 24, 5)
    return lines, ""


def datalog(m):
    lines = R(16, 22, 88, 64, 8) + L(40, 86, 32, 102) + L(80, 86, 88, 102)
    pts = [(28, 72), (44, 56), (58, 62), (74, 40), (92, 46)]
    lines += poly(pts, close=False)
    return lines, "" if m.mini else "".join(C(x, y, 3.6) for x, y in pts)


def dmx(m):
    """A stage light on its yoke."""
    lines = R(18, 34, 50, 36, 12) + C(70, 52, 14, KO) + P("M42,70 V84 M26,94 H58")
    if not m.mini:
        lines += L(88, 40, 104, 30) + L(90, 52, 106, 52) + L(88, 64, 104, 74)
    return lines, C(70, 52, 6)


def eeprom(m):
    lines = R(36, 22, 48, 76, 8) + arc(60, 22, 8, 180, 360)
    n = 4 if not m.mini else 2
    for i in range(n):
        y = 36 + i * (48 / max(1, n - 1))
        lines += L(36, y, 24, y) + L(84, y, 96, y)
    return lines, "" if m.mini else C(48, 34, 3.5)


def equipos(m):
    lines = R(14, 28, 92, 64, 10) + R(24, 38, 44, 20, 4) + C(86, 48, 9)
    fills = C(36, 76, 6) + C(56, 76, 6)
    if not m.mini:
        lines += L(86, 48, 86, 41)
        fills += C(86, 76, 4)
    return lines, fills


def infrarrojo(m):
    g = '<g transform="rotate(-24 56 66)">'
    lines = g + R(40, 30, 32, 76, 12) + "</g>"
    fills = g + C(56, 40, 5) + (("" if m.mini else C(49, 58, 3.5) + C(63, 58, 3.5) + C(49, 72, 3.5) + C(63, 72, 3.5))) + "</g>"
    lines += waves(46, 32, 2 if not m.mini else 1, 16, 12, 30, 100)
    return lines, fills


def logica(m):
    return P("M12,78 H30 V42 H52 V78 H74 V42 H94 V78 H108"), ""


def luces(m):
    lines = P("M46,80 C30,68 32,26 60,24 C88,26 90,68 74,80 Z") + L(48, 90, 72, 90) + L(52, 100, 68, 100)
    if not m.mini:
        lines += P("M52,62 L57,52 L63,62 L68,52")
    return lines, ""


def modbus(m):
    """A terminal block, three wires in (juguete's e)."""
    lines = R(16, 30, 88, 38, 8)
    xs = (36, 60, 84) if not m.mini else (40, 80)
    for x in xs:
        lines += C(x, 49, 8) + P(f"M{x},68 V{88 if x != 60 else 96} Q{x},104 {x - 8},106")
        if not m.mini:
            lines += L(x - 4, 53, x + 4, 45)
    return lines, ""


def plotter(m):
    lines = R(14, 24, 92, 70, 10) + R(22, 32, 56, 54, 6) + _sine(28, 72, 59, 13, 1.5)
    return lines, "" if m.mini else C(92, 44, 5) + C(92, 70, 5)


def pwm(m):
    lines = C(60, 62, 28) + L(60, 62, 60, 42)
    if m.mini:
        return lines, ""
    ticks = "".join(C(60 + 40 * math.cos(math.radians(a)), 62 - 40 * math.sin(math.radians(a)), 3)
                    for a in range(-45, 226, 45))
    return lines, ticks


def rf(m):
    lines = R(22, 76, 32, 26, 6) + L(38, 76, 76, 30) + waves(80, 26, 2 if not m.mini else 1, 14, 12, -60, 40)
    return lines, C(78, 28, 6)


ART = {
    "aos.can": can, "aos.datalog": datalog, "aos.dmx": dmx, "aos.eeprom": eeprom, "aos.equipos": equipos,
    "aos.infrarrojo": infrarrojo, "aos.logica": logica, "aos.luces": luces, "aos.modbus": modbus,
    "aos.plotter": plotter, "aos.pwm": pwm, "aos.rf": rf,
}
