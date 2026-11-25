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


def files(m):
    folder = P("M18,38 Q18,30 26,30 H46 L54,38 H94 Q102,38 102,46 V86 Q102,94 94,94 H26 Q18,94 18,86 Z")
    return folder + L(18, 52, 102, 52), ""


def photos(m):
    """An instant photo, a little tilted (juguete's c)."""
    g = '<g transform="rotate(-8 60 60)">'
    lines = R(26, 16, 68, 86, 6) + R(34, 24, 52, 52, 3)
    if not m.mini:
        lines += P("M36,70 L50,52 L60,62 L68,54 L84,70")
        return g + lines + "</g>", g + C(74, 36, 5) + "</g>"
    return g + lines + P("M38,70 L54,50 L82,70") + "</g>", ""


def calendar(m):
    lines = R(18, 26, 84, 72, 10) + L(18, 46, 102, 46) + L(40, 16, 40, 34) + L(80, 16, 80, 34)
    if m.mini:
        return lines, C(46, 70, 7) + C(74, 70, 7)
    dots = "".join(C(36 + c * 16, 60 + r * 15, 3.6) for r in range(3) for c in range(4) if (r, c) != (1, 2))
    return lines, dots + R(64, 70, 12, 10, 3)


def clock(m):
    lines = C(60, 60, 38) + P("M60,36 V60 L76,70")
    if m.mini:
        return lines, ""
    return lines, "".join(C(60 + 29 * math.sin(math.radians(a)), 60 - 29 * math.cos(math.radians(a)), 3) for a in range(0, 360, 90))


def calc(m):
    lines = R(30, 16, 60, 88, 10) + R(40, 26, 40, 18, 4)
    if m.mini:
        return lines, R(40, 56, 14, 12, 3) + R(66, 56, 14, 12, 3) + R(40, 78, 14, 12, 3) + R(66, 78, 14, 12, 3)
    return lines, "".join(C(45 + c * 15, 60 + r * 14, 4.2) for r in range(3) for c in range(3))


def convert(m):
    """A balance (juguete's b)."""
    lines = L(60, 26, 60, 96) + L(40, 98, 80, 98) + L(22, 34, 98, 34)
    pans = P("M14,64 L22,34 L30,64") + P("M90,64 L98,34 L106,64")
    lines += P("M10,64 Q22,78 34,64 Z") + P("M86,64 Q98,78 110,64 Z")
    return lines + ("" if m.mini else pans), C(60, 26, 5)


def claude(m):
    """A robot's head (juguete's a)."""
    lines = R(24, 34, 72, 56, 16) + L(60, 34, 60, 22) + R(16, 52, 8, 20, 4) + R(96, 52, 8, 20, 4)
    fills = C(60, 18, 5) + C(45, 60, 7) + C(75, 60, 7)
    if not m.mini:
        lines += L(50, 78, 70, 78)
    return lines, fills


def life(m):
    live = {(0, 1), (1, 2), (2, 0), (2, 1), (2, 2)}
    lines, fills = "", ""
    for r in range(3):
        for c in range(3):
            x, y = 24 + c * 26, 24 + r * 26
            if (r, c) in live:
                fills += R(x, y, 20, 20, 5)
            elif not m.mini:
                fills += C(x + 10, y + 10, 3)
    return lines, fills


def sysmon(m):
    lines = R(32, 32, 56, 56, 8)
    n = 3 if not m.mini else 2
    for i in range(n):
        k = 44 + i * (32 / max(1, n - 1)) if n > 1 else 60
        lines += L(k, 32, k, 20) + L(k, 88, k, 100) + L(32, k, 20, k) + L(88, k, 100, k)
    return lines, R(48, 48, 24, 24, 4)


def serial(m):
    lines = poly([(20, 34), (100, 34), (90, 76), (30, 76)]) + L(60, 77, 60, 104)
    if m.mini:
        return lines, ""
    dots = "".join(C(38 + i * 11, 48, 3.6) for i in range(5)) + "".join(C(43.5 + i * 11, 62, 3.6) for i in range(4))
    return lines, dots


def flasher(m):
    lines = R(34, 46, 52, 52, 8) + P("M60,12 V36 M50,27 L60,37 L70,27")
    for k in ((58, 72, 86) if not m.mini else (64, 80)):
        lines += L(34, k, 24, k) + L(86, k, 96, k)
    return lines, R(48, 60, 24, 24, 4)


def bus(m):
    """Coloured wires with their tips (juguete's b)."""
    lines = ""
    xs = (36, 60, 84) if not m.mini else (42, 78)
    for i, x in enumerate(xs):
        lines += P(f"M{x - 10},104 C{x + 14},84 {x - 14},60 {x},44")
    fills = "".join(R(x - 7, 18, 14, 24, 3) for x in xs)
    return lines, fills


def bench(m):
    """A multimeter (juguete's b)."""
    lines = R(30, 14, 60, 92, 12) + R(40, 24, 40, 20, 4) + C(60, 68, 14)
    if not m.mini:
        lines += L(60, 68, 66, 58)
        return lines, C(44, 96, 4) + C(76, 96, 4)
    return lines, ""


def elec(m):
    """A resistor and its bands (juguete's b)."""
    g = '<g transform="rotate(-35 60 60)">'
    lines = L(16, 60, 32, 60) + L(88, 60, 104, 60) + R(32, 46, 56, 28, 12)
    fills = "" if m.mini else R(42, 46, 6, 28, 1) + R(54, 46, 6, 28, 1) + R(66, 46, 6, 28, 1)
    if m.mini:
        lines += L(50, 46, 50, 74) + L(70, 46, 70, 74)
    return g + lines + "</g>", g + fills + "</g>"


def leds(m):
    """One big LED (juguete's b)."""
    lines = P("M42,72 V46 A18,18 0 0 1 78,46 V72") + L(34, 72, 86, 72) + L(52, 72, 52, 104) + L(68, 72, 68, 96)
    if not m.mini:
        lines += L(60, 14, 60, 8) + L(32, 24, 27, 19) + L(88, 24, 93, 19)
    return lines, ""


def macropad(m):
    """Four keys and a knob (juguete's b)."""
    lines = R(16, 24, 88, 72, 12) + C(80, 60, 14)
    if m.mini:
        return lines, R(28, 40, 18, 18, 4) + R(28, 62, 18, 18, 4)
    lines += L(80, 60, 80, 48)
    return lines, R(26, 36, 16, 16, 4) + R(46, 36, 16, 16, 4) + R(26, 56, 16, 16, 4) + R(46, 56, 16, 16, 4)


def modules(m):
    lines = R(16, 30, 88, 54, 8)
    n = 5 if not m.mini else 3
    for i in range(n):
        x = 30 + i * (60 / (n - 1))
        lines += L(x, 84, x, 96)
    return lines, R(30, 42, 26, 26, 4) + ("" if m.mini else C(84, 48, 4))


def mqtt(m):
    lines = L(60, 54, 60, 100) + L(46, 100, 74, 100)
    lines += waves(60, 46, 2 if not m.mini else 1, 18, 14, -45, 45) + waves(60, 46, 2 if not m.mini else 1, 18, 14, 135, 225)
    return lines, C(60, 46, 7)


def net(m):
    lines = R(28, 16, 64, 88, 10) + L(28, 45, 92, 45) + L(28, 75, 92, 75)
    fills = C(40, 30, 4.5) + C(40, 60, 4.5) + C(40, 90, 4.5)
    if not m.mini:
        lines += L(64, 30, 80, 30) + L(64, 60, 80, 60) + L(64, 90, 80, 90)
    return lines, fills


ART.update({
    "aos.files": files, "aos.photos": photos, "aos.calendar": calendar, "aos.clock": clock, "aos.calc": calc,
    "aos.convert": convert, "aos.claude": claude, "aos.life": life, "aos.sysmon": sysmon, "aos.serial": serial,
    "aos.flasher": flasher, "aos.bus": bus, "aos.bench": bench, "aos.elec": elec, "aos.leds": leds,
    "aos.macropad": macropad, "aos.modules": modules, "aos.mqtt": mqtt, "aos.net": net,
})
PAIRS.update({
    "aos.files": ("#8FD0FF", "#2E7BFF"), "aos.photos": ("#FFD06A", "#FF4D9A"), "aos.calendar": ("#FFB0A0", "#FF3B30"),
    "aos.clock": ("#FFFFFF", "#FF9F0A"), "aos.calc": ("#FFD36A", "#E07A1A"), "aos.claude": ("#FFC2A8", "#E0703E"),
    "aos.serial": ("#7CFFB2", "#1FB8E8"), "aos.sysmon": ("#C8D6E8", "#5E8CFF"),
})
