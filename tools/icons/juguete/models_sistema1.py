"""'juguete' models: the group sistema-1 of apps.json.

Each build(mini) makes the tile and the toy; mini is the 26 px one: the toy
bigger and the small parts left out.
"""

import math

from lib import (ball, blob, capsule, circle, cyl, gear_loop, glass, glossy, glow, group, matte,
                 metal, plastic, rbox, rounded_poly, rrect, slab, star, text, tile, tube, turn)

R90 = math.radians(90)


# -- Ajustes ---------------------------------------------------------------

def settings_a(mini):
    tile("#A9ACB4", "#4E5157")
    g = slab([gear_loop(0.34, 0.255, 8 if mini else 9, 0.52), circle(0, 0, 0.11 if mini else 0.13)],
             0.13, 0.028, plastic("#F4F5F8", 0.32))
    parts = [g]
    if not mini:
        parts.append(cyl((0, 0, 0.04), 0.075, 0.09, metal("#7C8089", 0.3), bevel=0.02))
    group(parts, loc=(0, 0.01, 0.05), rot=(-22, 14, 8), scale=1.25 if mini else 1.0)


def settings_b(mini):
    tile("#A9ACB4", "#4E5157")
    big = slab([gear_loop(0.26, 0.195, 9, 0.5), circle(0, 0, 0.08)], 0.12, 0.026, plastic("#F4F5F8", 0.3))
    small = slab([gear_loop(0.19, 0.14, 7, 0.5, (0.025, 0.03)), circle(0, 0, 0.06)], 0.12, 0.024,
                 plastic("#FF9F0A", 0.3))
    big.location.x, big.location.y = -0.11, 0.1
    small.location.x, small.location.y = 0.2, -0.19
    small.rotation_euler.z = math.radians(12)
    parts = [big, small]
    if not mini:
        parts += [cyl((-0.11, 0.1, 0.1), 0.05, 0.08, metal("#7C8089"), bevel=0.015),
                  cyl((0.2, -0.19, 0.1), 0.04, 0.08, metal("#7C8089"), bevel=0.015)]
    group(parts, loc=(0, 0, 0.04), rot=(-18, 10, 0), scale=1.1 if mini else 1.0)


def settings_c(mini):
    tile("#A9ACB4", "#4E5157")
    steel = metal("#E3E7EC", 0.22)
    # an open-end wrench: a ring with a jaw cut out, and a handle
    jaw = [(0.13 * math.cos(math.radians(a)), 0.13 * math.sin(math.radians(a))) for a in range(40, 321, 10)]
    head = slab([rounded_poly(jaw + [(0.02, -0.045), (0.02, 0.045)], 0.015)], 0.07, 0.02, steel)
    handle = rbox((-0.32, 0, 0.035), (0.46, 0.09, 0.06), 0.03, steel)
    wrench = group([head, handle], loc=(0, 0, 0), rot=(0, 0, 0), name="wrench")
    wrench.location = (0.17, 0.17, 0.03)
    wrench.rotation_euler.z = math.radians(45)
    wrench.scale = (1.05, 1.05, 1.05)
    parts = [wrench]
    red = glossy("#FF453A")
    sd = [cyl((0, -0.17, 0), 0.07, 0.26, red, rot=(R90, 0, 0), bevel=0.03, verts=6),
          cyl((0, 0.06, 0), 0.022, 0.24, steel, rot=(R90, 0, 0)),
          cyl((0, 0.2, 0), 0.03, 0.05, steel, rot=(R90, 0, 0), r2=0.008)]
    if not mini:
        sd.append(cyl((0, -0.3, 0), 0.065, 0.02, matte("#2C2C2E"), rot=(R90, 0, 0)))
    drv = group(sd, name="driver")
    drv.location = (0.0, 0.0, 0.1)
    drv.rotation_euler.z = math.radians(-45)
    group(parts + [drv], loc=(0, 0.0, 0.03), rot=(-12, 8, 0), scale=1.15 if mini else 1.0)


# -- Tienda ----------------------------------------------------------------

def _kraft_box(mini, e=0.46):
    box = plastic("#E2A665", 0.5)
    tape = glossy("#F7E2B8")
    parts = [rbox((0, 0, 0), (e, e, e), 0.04, box),
             rbox((0, 0, 0), (0.1, e + 0.012, e + 0.012), 0.02, tape)]
    if not mini:
        parts.append(rbox((0.1, -e / 2 - 0.004, -0.08), (0.14, 0.012, 0.1), 0.006, plastic("#FFFFFF", 0.4)))
    return parts


def store_a(mini):
    tile("#4DAFFF", "#0A5ADB")
    root = group(_kraft_box(mini), loc=(0, -0.03, 0.05), scale=1.18 if mini else 1.0)
    turn(root, ("Z", 45), ("X", -50))


def store_b(mini):
    tile("#4DAFFF", "#0A5ADB")
    bag = glossy("#FFB020")
    parts = [rbox((0, -0.06, 0.07), (0.56, 0.58, 0.14), 0.05, bag),
             rbox((0, 0.22, 0.13), (0.56, 0.04, 0.02), 0.01, plastic("#E08A00"))]
    white = plastic("#FFFFFF")
    for x in (-0.13, 0.13):
        parts.append(tube([(x - 0.06, 0.21, 0.12), (x, 0.44, 0.13), (x + 0.06, 0.21, 0.12)], 0.022, white))
    if not mini:
        s = slab([star(0, 0, 0.12, 0.055)], 0.04, 0.012, plastic("#FFFFFF"), z=0.14)
        s.location.y = -0.08
        parts.append(s)
    group(parts, loc=(0, -0.02, 0.0), rot=(-14, 10, 0), scale=1.12 if mini else 1.0)


def store_c(mini):
    tile("#4DAFFF", "#0A5ADB")
    root = group(_kraft_box(True, 0.42), loc=(-0.04, -0.12, 0.05), scale=1.0)
    turn(root, ("Z", 45), ("X", -50))
    s = slab([rounded_poly(star(0, 0, 0.2, 0.09), 0.025)], 0.08, 0.025, glossy("#FFD60A"), z=0.0)
    group([s], loc=(0.12, 0.18, 0.32), rot=(-20, 15, 12), scale=1.2 if mini else 1.0)


# -- Archivos --------------------------------------------------------------

def _folder(back_c, front_c, mini):
    back = slab([rounded_poly([(-0.36, -0.28), (0.36, -0.28), (0.36, 0.22), (-0.02, 0.22),
                               (-0.08, 0.3), (-0.36, 0.3)], [0.05, 0.05, 0.05, 0.03, 0.03, 0.05])],
                0.05, 0.018, plastic(back_c), z=0.0)
    parts = [back]
    if not mini:
        for i, c in enumerate(("#FFFFFF", "#EEF2F8")):
            parts.append(rbox((0.02 + i * 0.03, 0.06 - i * 0.03, 0.06 + i * 0.012), (0.58, 0.4, 0.012), 0.01,
                              plastic(c)))
    front = slab([rrect(0, -0.07, 0.74, 0.44, 0.06)], 0.06, 0.022, glossy(front_c), z=0.08)
    parts.append(front)
    return parts


def files_a(mini):
    tile("#5BB4FF", "#0A55D6")
    group(_folder("#1F6FE0", "#3D9BFF", mini), loc=(0, 0.02, 0.02), rot=(-18, 8, 0),
          scale=1.15 if mini else 1.0)


def files_b(mini):
    tile("#5BB4FF", "#0A55D6")
    body = plastic("#E9EEF5", 0.35)
    parts = [rbox((0, -0.04, 0.06), (0.6, 0.66, 0.12), 0.05, body)]
    for y in (0.1, -0.2):
        parts.append(rbox((0, y, 0.13), (0.5, 0.24, 0.04), 0.03, plastic("#C9D3E2", 0.35)))
        parts.append(capsule((-0.08, y, 0.17), (0.08, y, 0.17), 0.022, metal()))
    if not mini:
        for i, c in enumerate(("#FF6B5E", "#FFD60A", "#34C759")):
            parts.append(rbox((-0.15 + i * 0.15, 0.3, 0.1), (0.12, 0.1, 0.03), 0.02, glossy(c)))
    group(parts, loc=(0, 0, 0.02), rot=(-16, 8, 0), scale=1.1 if mini else 1.0)


# -- Fotos -----------------------------------------------------------------

def photos_a(mini):
    tile("#FFC83D", "#EC4899")
    parts = [rbox((0, 0, 0.03), (0.74, 0.6, 0.08), 0.05, plastic("#FFFFFF")),
             rbox((0, 0, 0.07), (0.6, 0.46, 0.02), 0.02, matte("#CFE4FF"))]
    parts.append(slab([rounded_poly([(-0.3, -0.23), (-0.06, 0.1), (0.14, -0.23)], 0.03)], 0.06, 0.02,
                      plastic("#F0447F"), z=0.07))
    parts.append(slab([rounded_poly([(-0.02, -0.23), (0.14, 0.0), (0.3, -0.23)], 0.03)], 0.07, 0.02,
                      plastic("#FF8A2A"), z=0.075))
    parts.append(ball((0.17, 0.12, 0.1), 0.06, glow("#FFC21A", 0.35)))
    group(parts, loc=(0, 0, 0.02), rot=(-14, 10, 4), scale=1.18 if mini else 1.0)


def photos_b(mini):
    tile("#FFC83D", "#EC4899")
    body = plastic("#F4F5F8", 0.35)
    parts = [rbox((0, -0.03, 0.08), (0.74, 0.46, 0.16), 0.07, body),
             rbox((0, -0.03, 0.08), (0.75, 0.16, 0.17), 0.04, plastic("#3A3A3C", 0.5)),
             cyl((0, -0.04, 0.17), 0.17, 0.08, plastic("#2C2C2E", 0.4), bevel=0.02, verts=64),
             cyl((0, -0.04, 0.21), 0.11, 0.04, glass("#6FA8FF"), verts=64)]
    if not mini:
        parts += [rbox((-0.2, 0.22, 0.12), (0.16, 0.08, 0.08), 0.03, body),
                  cyl((0.24, 0.2, 0.16), 0.045, 0.06, glossy("#FF453A"), bevel=0.015),
                  rbox((0.22, 0.09, 0.17), (0.1, 0.05, 0.02), 0.01, glow("#FFF7D6", 0.4)),
                  ball((-0.03, 0.0, 0.24), 0.03, glow("#FFFFFF", 0.8))]
    group(parts, loc=(0, 0.02, 0.0), rot=(-16, 10, 0), scale=1.1 if mini else 1.0)


def _polaroid(rot_z, z, mini):
    parts = [rbox((0, 0, 0), (0.52, 0.62, 0.03), 0.015, plastic("#FFFFFF", 0.45)),
             rbox((0, 0.06, 0.016), (0.44, 0.42, 0.005), 0.003, matte("#9FD3FF"))]
    if not mini:
        parts.append(slab([[(-0.22, -0.15), (-0.05, 0.08), (0.12, -0.15)]], 0.012, 0.0, plastic("#34C759"), z=0.017))
        parts.append(slab([[(0.0, -0.15), (0.12, 0.02), (0.22, -0.15)]], 0.012, 0.0, plastic("#1F9D4A"), z=0.018))
        parts.append(ball((0.13, 0.17, 0.02), 0.045, glow("#FFC21A", 0.3), scale=(1, 1, 0.3)))
    else:
        parts.append(slab([[(-0.22, -0.15), (0.0, 0.1), (0.22, -0.15)]], 0.012, 0.0, plastic("#34C759"), z=0.017))
    return group(parts, loc=(0, 0, z), rot=(0, 0, rot_z), name="pol")


def photos_c(mini):
    tile("#FFC83D", "#EC4899")
    a = _polaroid(12, 0.02, True)
    b = _polaroid(-10, 0.07, mini)
    a.location.x, a.location.y = 0.08, 0.04
    b.location.x, b.location.y = -0.04, -0.04
    group([a, b], loc=(0, 0, 0.02), rot=(-10, 8, 0), scale=1.12 if mini else 1.0)


# -- Música ----------------------------------------------------------------

def music_a(mini):
    tile("#FF6A88", "#B0123F")
    w = glossy("#FFFFFF")
    parts = []
    for x, y in ((-0.17, -0.2), (0.17, -0.12)):
        parts.append(ball((x, y, 0.08), 0.11, w, scale=(1.25, 0.95, 0.8)))
        parts.append(rbox((x + 0.115, y + 0.2, 0.08), (0.05, 0.42, 0.06), 0.022, w))
    beam = rbox((0.03, 0.31, 0.08), (0.42, 0.1, 0.07), 0.03, w)
    beam.rotation_euler.z = math.radians(11)
    parts.append(beam)
    group(parts, loc=(-0.04, 0.0, 0.03), rot=(-16, 10, -6), scale=1.15 if mini else 1.0)


def music_b(mini):
    tile("#FF6A88", "#B0123F")
    band = plastic("#2C2C2E", 0.35)
    pts = [(0.3 * math.cos(math.radians(a)), 0.05 + 0.3 * math.sin(math.radians(a)), 0.12) for a in range(0, 181, 30)]
    parts = [tube(pts, 0.045, band)]
    for s in (-1, 1):
        parts += [cyl((s * 0.3, -0.06, 0.12), 0.13, 0.12, glossy("#FFFFFF"), rot=(0, R90, 0), bevel=0.03, verts=64),
                  cyl((s * 0.23, -0.06, 0.12), 0.1, 0.05, plastic("#3A3A3C", 0.6), rot=(0, R90, 0), bevel=0.02)]
    group(parts, loc=(0, 0.02, 0.02), rot=(-28, 0, 0), scale=1.1 if mini else 1.0)


def music_c(mini):
    tile("#FF6A88", "#B0123F")
    parts = [cyl((0, 0, 0.04), 0.38, 0.04, mat_vinyl(), verts=96, bevel=0.01),
             cyl((0, 0, 0.065), 0.13, 0.012, glossy("#FF375F"), verts=64),
             cyl((0, 0, 0.07), 0.025, 0.02, matte("#1C1C1E"))]
    if not mini:
        for r in (0.2, 0.26, 0.32):
            parts.append(slab([circle(0, 0, r + 0.004), circle(0, 0, r)], 0.004, 0.0, glossy("#3A3A3C"), z=0.061))
        parts.append(ball((0.07, 0.07, 0.075), 0.03, glow("#FFFFFF", 0.3), scale=(1, 1, 0.3)))
    group(parts, loc=(0, 0, 0.03), rot=(-22, 12, 0), scale=1.1 if mini else 1.0)


def mat_vinyl():
    return glossy("#16161A")


# -- Calendario ------------------------------------------------------------

def calendar_a(mini):
    tile("#FF8A8A", "#D42A2A")
    parts = [rbox((0, -0.05, 0.05), (0.62, 0.62, 0.1), 0.05, plastic("#FFFFFF", 0.45)),
             rbox((0, 0.2, 0.08), (0.64, 0.16, 0.12), 0.05, glossy("#FF453A"))]
    parts.append(text("24", plastic("#2C2C2E"), size=0.36 if not mini else 0.42, depth=0.06, bevel=0.012,
                      c=(0, -0.1, 0.1)))
    if not mini:
        for x in (-0.16, 0.16):
            parts.append(tube([(x, 0.24, 0.13), (x, 0.32, 0.2), (x, 0.38, 0.13)], 0.022, metal()))
    group(parts, loc=(0, 0, 0.02), rot=(-16, 10, 0), scale=1.1 if mini else 1.0)


def calendar_b(mini):
    tile("#FF8A8A", "#D42A2A")
    parts = [rbox((0, -0.04, 0.04), (0.66, 0.68, 0.08), 0.05, plastic("#FFFFFF", 0.45)),
             rbox((0, 0.23, 0.07), (0.68, 0.16, 0.1), 0.05, glossy("#FF453A"))]
    n, k = (3, 0.14) if mini else (4, 0.1)
    for r in range(3):
        for c in range(n):
            x = (c - (n - 1) / 2) * (k + 0.035)
            y = 0.04 - r * (k + 0.03)
            hot = (r, c) == (1, n - 2)
            parts.append(rbox((x, y, 0.09), (k, k * 0.85, 0.03), 0.02,
                              glossy("#FF453A") if hot else plastic("#D9DCE3")))
    group(parts, loc=(0, 0, 0.02), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


# -- Reloj -----------------------------------------------------------------

def _hands(parts, z, ink, accent, mini):
    for ang, ln, w in ((120, 0.15, 0.05), (0, 0.22, 0.04)):
        a = math.radians(ang)
        h = rbox((ln / 2 * math.sin(a), ln / 2 * math.cos(a), z), (w, ln, 0.03), 0.014, ink)
        h.rotation_euler = (0, 0, -a)
        parts.append(h)
    parts.append(cyl((0, 0, z + 0.01), 0.03, 0.04, accent, bevel=0.01))


def clock_a(mini):
    tile("#3C3C41", "#0A0A0C")
    rim, ink = glossy("#FF9F0A"), plastic("#1C1C1E")
    parts = [cyl((0, 0, 0.04), 0.36, 0.12, rim, bevel=0.04, verts=96),
             cyl((0, 0, 0.07), 0.3, 0.1, plastic("#FFFFFF", 0.45), bevel=0.02, verts=96)]
    if not mini:
        for i in range(12):
            a = math.radians(i * 30)
            parts.append(ball((0.24 * math.sin(a), 0.24 * math.cos(a), 0.12), 0.022 if i % 3 else 0.03, ink))
    _hands(parts, 0.14, ink, rim, mini)
    group(parts, loc=(0, 0.01, 0.02), rot=(-16, 10, 0), scale=1.15 if mini else 1.0)


def clock_b(mini):
    tile("#3C3C41", "#0A0A0C")
    red, ink = glossy("#FF453A"), plastic("#1C1C1E")
    parts = [cyl((0, -0.04, 0.08), 0.29, 0.16, red, bevel=0.05, verts=96),
             cyl((0, -0.04, 0.14), 0.23, 0.06, plastic("#FFFFFF", 0.45), bevel=0.015, verts=96)]
    for s in (-1, 1):
        parts.append(ball((s * 0.2, 0.25, 0.12), 0.1, metal("#E9C46A", 0.3), scale=(1, 1, 0.7)))
        if not mini:
            parts.append(capsule((s * 0.17, -0.3, 0.06), (s * 0.24, -0.38, 0.04), 0.03, ink))
    if not mini:
        parts.append(capsule((-0.14, 0.33, 0.14), (0.14, 0.33, 0.14), 0.02, metal()))
    sub = []
    _hands(sub, 0.0, ink, red, mini)
    hands = group(sub, loc=(0, -0.04, 0.19), scale=0.85, name="hands")
    parts.append(hands)
    group(parts, loc=(0, 0.02, 0.02), rot=(-16, 10, 0), scale=1.1 if mini else 1.0)


# -- Calculadora -----------------------------------------------------------

def calc_a(mini):
    tile("#F4BC3E", "#86562B")
    parts = [rbox((0, 0, 0.04), (0.56, 0.78, 0.1), 0.05, plastic("#FFFFFF", 0.4)),
             rbox((0, 0.24, 0.09), (0.42, 0.16, 0.03), 0.02, glossy("#3E4B36"))]
    n = 2 if mini else 3
    k = 0.15 if mini else 0.1
    for r in range(n):
        for c in range(n):
            x = (c - (n - 1) / 2) * (k + 0.035)
            y = 0.06 - r * (k + 0.035) - (0.02 if mini else 0)
            parts.append(rbox((x, y, 0.1), (k, k * 0.9, 0.05), 0.022,
                              glossy("#FF9F0A") if c == n - 1 else plastic("#D9DCE3", 0.4)))
    group(parts, loc=(0, 0, 0.02), rot=(-14, 8, -6), scale=1.1 if mini else 1.0)


def _sign(kind, x, y, z, m):
    bar = lambda rot: rbox((x, y, z), (0.17, 0.045, 0.035), 0.02, m)
    out = []
    if kind in "+x":
        h, v = bar(0), bar(0)
        v.rotation_euler.z = R90
        out = [h, v]
        if kind == "x":
            h.rotation_euler.z, v.rotation_euler.z = math.radians(45), math.radians(135)
    elif kind == "-":
        out = [bar(0)]
    else:                                   # the division sign
        out = [bar(0), ball((x, y + 0.065, z), 0.028, m), ball((x, y - 0.065, z), 0.028, m)]
    return out


def calc_b(mini):
    tile("#F4BC3E", "#86562B")
    parts = []
    for i, (k, c) in enumerate(zip("+-x/", ("#FF9F0A", "#FFFFFF", "#FFFFFF", "#FF9F0A"))):
        x, y = (-0.17 if i % 2 == 0 else 0.17), (0.17 if i < 2 else -0.17)
        parts.append(rbox((x, y, 0.07), (0.3, 0.3, 0.14), 0.07, glossy(c)))
        parts += _sign(k, x, y, 0.15, plastic("#5B3B1E" if c == "#FFFFFF" else "#FFFFFF"))
    group(parts, loc=(0, 0, 0.0), rot=(-16, 10, 0), scale=1.05 if mini else 0.95)


# -- Conversor -------------------------------------------------------------

def _arrow(length, w, head):
    h = length / 2
    return rounded_poly([(-h, -w / 2), (h - head, -w / 2), (h - head, -w / 2 - head * 0.55), (h, 0),
                         (h - head, w / 2 + head * 0.55), (h - head, w / 2), (-h, w / 2)], 0.025)


def convert_a(mini):
    tile("#4FD2EA", "#1C5C77")
    a1 = slab([_arrow(0.66, 0.14, 0.2)], 0.12, 0.04, glossy("#FFFFFF"))
    a2 = slab([_arrow(0.66, 0.14, 0.2)], 0.12, 0.04, glossy("#FFD60A"))
    a1.location.y = 0.15
    a2.location.y = -0.15
    a2.rotation_euler.z = math.pi
    group([a1, a2], loc=(0, 0, 0.03), rot=(-20, 10, 0), scale=1.1 if mini else 1.0)


def convert_b(mini):
    tile("#4FD2EA", "#1C5C77")
    gold, dark = metal("#F2C14E", 0.25), plastic("#2C2C2E")
    parts = [cyl((0, -0.32, 0.04), 0.14, 0.06, dark, bevel=0.02),
             rbox((0, -0.05, 0.1), (0.05, 0.52, 0.05), 0.02, gold),
             rbox((0, 0.2, 0.12), (0.7, 0.05, 0.05), 0.02, gold),
             ball((0, 0.22, 0.14), 0.05, gold)]
    for s in (-1, 1):
        parts.append(cyl((s * 0.3, -0.02, 0.1), 0.15, 0.03, glossy("#FFFFFF"), bevel=0.012, verts=64))
        if not mini:
            parts += [capsule((s * 0.33, 0.2, 0.12), (s * 0.2, 0.02, 0.11), 0.008, gold),
                      capsule((s * 0.33, 0.2, 0.12), (s * 0.42, 0.02, 0.11), 0.008, gold)]
    group(parts, loc=(0, 0.02, 0.02), rot=(-18, 8, 0), scale=1.1 if mini else 1.0)


# -- Claude (el robot) -----------------------------------------------------

def _robot_head(c, mini, s=1.0):
    x, y, z = c
    white = plastic("#FFF4EC", 0.35)
    parts = [rbox((x, y, z), (0.52 * s, 0.42 * s, 0.26 * s), 0.1 * s, white),
             rbox((x, y - 0.01 * s, z + 0.11 * s), (0.4 * s, 0.24 * s, 0.06 * s), 0.05 * s, glossy("#2A1E1A"))]
    for e in (-1, 1):
        parts.append(ball((x + e * 0.09 * s, y - 0.01 * s, z + 0.15 * s), 0.045 * s, glow("#5EE7FF", 2.0),
                          scale=(1, 1.3, 0.6)))
    parts += [capsule((x, y + 0.2 * s, z), (x, y + 0.3 * s, z + 0.02 * s), 0.018 * s, metal()),
              ball((x, y + 0.32 * s, z + 0.02 * s), 0.045 * s, glossy("#FF7A45"))]
    if not mini:
        for e in (-1, 1):
            parts.append(cyl((x + e * 0.27 * s, y, z), 0.06 * s, 0.06 * s, glossy("#FF7A45"),
                             rot=(0, R90, 0), bevel=0.015))
    return parts


def claude_a(mini):
    tile("#EA906C", "#A8492B")
    group(_robot_head((0, -0.03, 0.12), mini, 1.25), loc=(0, 0, 0), rot=(-14, 10, 0),
          scale=1.1 if mini else 1.0)


def claude_b(mini):
    tile("#EA906C", "#A8492B")
    white = plastic("#FFF4EC", 0.35)
    parts = _robot_head((0, 0.17, 0.16), True, 0.8)
    parts += [rbox((0, -0.13, 0.12), (0.32, 0.26, 0.2), 0.08, white),
              cyl((0, -0.13, 0.23), 0.045, 0.02, glow("#5EE7FF", 1.2)),
              capsule((-0.17, -0.06, 0.13), (-0.3, -0.18, 0.12), 0.04, glossy("#FF7A45")),
              capsule((0.17, -0.06, 0.13), (0.32, 0.08, 0.12), 0.04, glossy("#FF7A45"))]
    if not mini:
        parts += [capsule((-0.08, -0.28, 0.1), (-0.09, -0.38, 0.08), 0.045, white),
                  capsule((0.08, -0.28, 0.1), (0.09, -0.38, 0.08), 0.045, white)]
    group(parts, loc=(0, 0.02, 0.0), rot=(-12, 8, 0), scale=1.12 if mini else 1.0)


# -- Vida ------------------------------------------------------------------

def life_a(mini):
    tile("#3EDB6A", "#0E4622")
    n = 3 if mini else 5
    k = (0.74 if mini else 0.7) / n
    live = {(0, 1), (1, 2), (2, 0), (2, 1), (2, 2)}
    off = (n - 3) // 2
    parts = []
    for r in range(n):
        for c in range(n):
            on = (r - off, c - off) in live
            h = 0.14 if on else 0.04
            parts.append(rbox(((c - (n - 1) / 2) * k, ((n - 1) / 2 - r) * k, h / 2),
                              (k * 0.84, k * 0.84, h), k * 0.18,
                              glow("#B6FF6A", 0.5) if on else plastic("#1F7A3A", 0.45)))
    group(parts, loc=(0, 0, 0.0), rot=(-16, 10, 0), scale=1.0)


def life_b(mini):
    tile("#3EDB6A", "#0E4622")
    parts = [slab([circle(0, 0, 0.39), circle(0, 0, 0.34)], 0.08, 0.02, glossy("#F2F7FA")),
             cyl((0, 0, 0.015), 0.35, 0.03, matte("#D8F0C4"), verts=96)]
    cells = [(-0.12, 0.1, 0.09), (0.1, 0.14, 0.07), (0.15, -0.1, 0.1), (-0.1, -0.14, 0.07), (0.0, 0.0, 0.06)]
    if not mini:
        cells += [(-0.22, -0.02, 0.05), (0.24, 0.04, 0.045), (0.02, 0.25, 0.05)]
    for x, y, r in cells:
        parts.append(ball((x, y, 0.035), r, glossy("#34C759"), scale=(1, 1, 0.5)))
        if not mini:
            parts.append(ball((x + r * 0.2, y + r * 0.2, 0.05), r * 0.3, plastic("#0E6B2C"), scale=(1, 1, 0.6)))
    group(parts, loc=(0, 0, 0.0), rot=(-20, 10, 0), scale=1.05 if mini else 1.0)


# -- Monitor ---------------------------------------------------------------

def sysmon_a(mini):
    tile("#7C8CA3", "#1E293B")
    parts = [rbox((0, 0, 0.03), (0.8, 0.36, 0.05), 0.03, plastic("#1F9D55", 0.4))]
    for i in range(3 if not mini else 2):
        x = (i - (1 if not mini else 0.5)) * 0.24
        parts.append(rbox((x, 0.03, 0.08), (0.18, 0.2, 0.05), 0.02, plastic("#1C1C1E", 0.35)))
    if not mini:
        for i in range(10):
            parts.append(rbox((-0.34 + i * 0.075, -0.15, 0.06), (0.04, 0.05, 0.01), 0.008, metal("#F2C14E")))
    stick = group(parts, loc=(0, -0.12, 0.04), rot=(0, 0, 8), name="stick")
    bars = []
    for i, h in enumerate((0.12, 0.2, 0.28, 0.16)):
        bars.append(rbox((-0.2 + i * 0.13, 0.12 + h / 2, 0.1), (0.09, h, 0.06), 0.03,
                         glossy("#5EE7FF" if i != 2 else "#FF9F0A")))
    group([stick] + bars, loc=(0, 0, 0.0), rot=(-16, 10, 0), scale=1.1 if mini else 1.0)


def sysmon_b(mini):
    tile("#7C8CA3", "#1E293B")
    parts = [cyl((0, -0.03, 0.05), 0.37, 0.1, plastic("#2C2C2E"), bevel=0.04, verts=96),
             cyl((0, -0.03, 0.09), 0.31, 0.04, plastic("#FFFFFF", 0.45), verts=96)]
    cols = ("#34C759", "#34C759", "#FFD60A", "#FF9F0A", "#FF453A")
    for i in range(5 if not mini else 3):
        a = math.radians(200 - i * (220 / 4 if not mini else 220 / 2))
        tick = rbox((0.23 * math.cos(a), -0.03 + 0.23 * math.sin(a), 0.12), (0.09, 0.04, 0.03), 0.015,
                    glossy(cols[i] if not mini else cols[i * 2]))
        tick.rotation_euler.z = a
        parts.append(tick)
    a = math.radians(55)
    needle = rbox((0.1 * math.cos(a), -0.03 + 0.1 * math.sin(a), 0.14), (0.24, 0.04, 0.03), 0.015,
                  glossy("#FF453A"))
    needle.rotation_euler.z = a
    parts += [needle, cyl((0, -0.03, 0.15), 0.05, 0.05, metal(), bevel=0.015)]
    group(parts, loc=(0, 0.02, 0.0), rot=(-16, 10, 0), scale=1.1 if mini else 1.0)


MODELS = {
    "aos.settings": {"a": settings_a, "b": settings_b, "c": settings_c},
    "aos.store": {"a": store_a, "b": store_b, "c": store_c},
    "aos.files": {"a": files_a, "b": files_b},
    "aos.photos": {"a": photos_a, "b": photos_b, "c": photos_c},
    "aos.music": {"a": music_a, "b": music_b, "c": music_c},
    "aos.calendar": {"a": calendar_a, "b": calendar_b},
    "aos.clock": {"a": clock_a, "b": clock_b},
    "aos.calc": {"a": calc_a, "b": calc_b},
    "aos.convert": {"a": convert_a, "b": convert_b},
    "aos.claude": {"a": claude_a, "b": claude_b},
    "aos.life": {"a": life_a, "b": life_b},
    "aos.sysmon": {"a": sysmon_a, "b": sysmon_b},
}
