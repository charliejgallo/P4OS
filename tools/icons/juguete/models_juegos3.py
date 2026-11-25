"""'juguete' models: the group juegos-3 of apps.json."""

import math

from lib import (ball, blob, capsule, circle, cyl, glass, glossy, glow, group, matte, metal, plastic, rbox,
                 rounded_poly, slab, text, tile, tube)

R90 = math.radians(90)


# -- Mila (la gatita) ------------------------------------------------------

def _cat(x, y, s, mini):
    fur = glossy("#2A2A30")
    parts = [blob([(x, y - 0.1 * s, 0.14 * s, 0.26 * s), (x, y + 0.05 * s, 0.16 * s, 0.2 * s)], fur, scale=(1, 1, 0.8)),
             ball((x, y + 0.2 * s, 0.24 * s), 0.17 * s, fur)]
    for e in (-1, 1):
        parts.append(cyl((x + e * 0.1 * s, y + 0.33 * s, 0.26 * s), 0.06 * s, 0.12 * s, fur, r2=0.01,
                         rot=(-0.5, e * 0.35, 0)))
        parts.append(ball((x + e * 0.065 * s, y + 0.2 * s, 0.38 * s), 0.035 * s, glow("#9CFF5A", 1.2),
                          scale=(1, 1.3, 0.5)))
    if not mini:
        parts.append(tube([(x + 0.18 * s, y - 0.25 * s, 0.08 * s), (x + 0.36 * s, y - 0.2 * s, 0.1 * s),
                           (x + 0.34 * s, y + 0.02 * s, 0.14 * s)], 0.03 * s, fur))
        parts.append(ball((x, y + 0.14 * s, 0.39 * s), 0.02 * s, glossy("#FF8FB0"), scale=(1.3, 1, 0.5)))
    return parts


def mila_a(mini):
    tile("#F58AB0", "#3A2A5A")
    group(_cat(-0.02, -0.04, 1.1, mini), loc=(0, 0, 0), rot=(-14, 8, 0), scale=1.05 if mini else 1.0)


def mila_b(mini):
    tile("#F58AB0", "#3A2A5A")
    parts = _cat(-0.16, -0.02, 0.75, True)
    parts += [rbox((0.2, 0.0, 0.12), (0.3, 0.3, 0.24), 0.04, plastic("#E2A665", 0.5)),
              rbox((0.2, 0.0, 0.12), (0.06, 0.31, 0.25), 0.015, glossy("#F7E2B8"))]
    group(parts, loc=(0, 0, 0), rot=(-16, 10, 0), scale=1.08 if mini else 1.0)


# -- Monster Hop -----------------------------------------------------------

def monsterhop_a(mini):
    tile("#9A5AF5", "#1A6A3A")
    body = glossy("#B06BFF")
    parts = [blob([(0, 0.02, 0.2, 0.3), (0, -0.1, 0.16, 0.24)], body, scale=(1, 1, 0.85)),
             cyl((-0.14, 0.26, 0.24), 0.05, 0.12, glossy("#FFD60A"), r2=0.01, rot=(-0.4, -0.3, 0)),
             cyl((0.14, 0.26, 0.24), 0.05, 0.12, glossy("#FFD60A"), r2=0.01, rot=(-0.4, 0.3, 0)),
             ball((0, 0.06, 0.4), 0.09, plastic("#FFFFFF"), scale=(1, 1.1, 0.6)),
             ball((0.01, 0.07, 0.45), 0.045, plastic("#1C1C1E"))]
    if not mini:
        parts += [rbox((0, -0.12, 0.38), (0.14, 0.04, 0.02), 0.015, plastic("#5A1E8C")),
                  ball((-0.12, -0.34, 0.04), 0.07, body, scale=(1.3, 1, 0.6)),
                  ball((0.12, -0.34, 0.04), 0.07, body, scale=(1.3, 1, 0.6))]
    group(parts, loc=(0, 0, 0), rot=(-14, 8, 0), scale=1.05)


def monsterhop_b(mini):
    tile("#9A5AF5", "#1A6A3A")
    gold = metal("#F2C14E", 0.2)
    parts = [slab([circle(-0.2, 0.18, 0.16), circle(-0.2, 0.18, 0.07)], 0.08, 0.03, gold),
             rbox((0.06, -0.08, 0.04), (0.08, 0.5, 0.07), 0.03, gold)]
    parts[-1].rotation_euler.z = math.radians(45)
    parts[-1].location = (0.04, -0.05, 0.04)
    for d in (0.0, 0.1):
        t = rbox((0.2 - d * 0.7, -0.2 - d * 0.7, 0.04), (0.12, 0.06, 0.06), 0.02, gold)
        t.rotation_euler.z = math.radians(45)
        parts.append(t)
    if not mini:
        parts.append(ball((-0.26, 0.24, 0.1), 0.03, glow("#FFFFFF", 0.8), scale=(1.5, 1, 0.4)))
    group(parts, loc=(0, 0.0, 0.03), rot=(-18, 12, 0), scale=1.12 if mini else 1.05)


# -- Neon Snakes -----------------------------------------------------------

def neon_a(mini):
    tile("#2A1A60", "#05030F")
    pts = [(-0.34, -0.3, 0.05), (-0.34, 0.0, 0.05), (0.0, 0.0, 0.05), (0.0, -0.3, 0.05), (0.3, -0.3, 0.05),
           (0.3, 0.28, 0.05)]
    parts = [tube(pts, 0.04 if not mini else 0.055, glow("#5CFF9A", 2.0), order=2),
             ball((0.3, 0.3, 0.06), 0.07 if not mini else 0.085, glow("#5CFF9A", 2.5))]
    if not mini:
        parts.append(ball((-0.16, 0.26, 0.05), 0.06, glow("#FF4DA6", 2.5)))
    group(parts, loc=(0, 0.0, 0.0), rot=(-14, 8, 0), scale=1.0)


def neon_b(mini):
    tile("#2A1A60", "#05030F")
    pink = glow("#FF4DA6", 2.2)
    parts = [ball((-0.12, -0.12, 0.12), 0.15, pink), ball((0.14, -0.06, 0.12), 0.15, pink),
             tube([(-0.12, 0.0, 0.14), (-0.04, 0.22, 0.16), (0.02, 0.3, 0.16)], 0.02, glow("#5CFF9A", 2.0)),
             tube([(0.14, 0.06, 0.14), (0.08, 0.22, 0.16), (0.02, 0.3, 0.16)], 0.02, glow("#5CFF9A", 2.0))]
    if not mini:
        parts.append(ball((0.12, 0.32, 0.16), 0.08, glow("#5CFF9A", 2.0), scale=(1.4, 0.6, 0.4)))
    group(parts, loc=(0, 0.0, 0.0), rot=(-14, 8, 0), scale=1.1 if mini else 1.0)


# -- Pinball 3D ------------------------------------------------------------

def pinball_a(mini):
    tile("#7A3AB8", "#10082A")
    flip = rounded_poly([(-0.36, 0.02), (-0.3, 0.1), (0.24, -0.04), (0.26, -0.1), (0.2, -0.14), (-0.34, -0.08)],
                        [0.06, 0.06, 0.04, 0.04, 0.04, 0.06])
    parts = [slab([flip], 0.1, 0.035, glossy("#FF3B30"), z=0.02),
             cyl((-0.31, 0.0, 0.12), 0.04, 0.03, metal(), bevel=0.01),
             ball((0.1, 0.18, 0.14), 0.14, metal("#E3E7EC", 0.08))]
    group(parts, loc=(0, 0.0, 0.02), rot=(-16, 10, 0), scale=1.1 if mini else 1.0)


def pinball_b(mini):
    tile("#7A3AB8", "#10082A")
    parts = [cyl((-0.08, 0.06, 0.08), 0.12, 0.16, plastic("#F4F5F8", 0.35), bevel=0.03),
             ball((-0.08, 0.06, 0.16), 0.24, glow("#FFD60A", 1.2), scale=(1, 1, 0.5)),
             slab([circle(-0.08, 0.06, 0.26), circle(-0.08, 0.06, 0.22)], 0.04, 0.012, glow("#FF4DA6", 1.5), z=0.02),
             ball((0.24, -0.24, 0.12), 0.12, metal("#E3E7EC", 0.08))]
    group(parts, loc=(0, 0.0, 0.02), rot=(-16, 10, 0), scale=1.08 if mini else 1.0)


# -- Solitario -------------------------------------------------------------

def _pcard(x, y, z, rz, lab=None, red=True, s=1.0, top=False):
    parts = [rbox((0, 0, 0), (0.3 * s, 0.44 * s, 0.02), 0.03 * s, plastic("#FFFFFF", 0.4))]
    if lab:
        c = "#E33030" if red else "#1C1C1E"
        ly, sz = (0.14 * s, 0.13 * s) if top else (0.05 * s, 0.2 * s)
        parts += [text(lab, plastic(c, 0.4), size=sz, depth=0.015, bevel=0.003, c=(-0.06 * s if top else 0, ly, 0.01)),
                  ball((0.07 * s if top else 0, ly if top else -0.1 * s, 0.015), 0.04 * s if top else 0.05 * s,
                       glossy(c), scale=(1, 1, 0.4))]
    return group(parts, loc=(x, y, z), rot=(0, 0, rz), name="card")


def solitario_a(mini):
    tile("#2F9A5E", "#0C3A22")
    parts = [_pcard(0.03 * i - 0.04, -0.03 * i + 0.04, 0.02 + i * 0.025, (i - 1) * 4) for i in range(3)]
    parts.append(_pcard(0.06, -0.04, 0.1, -3, "A", True, 1.15))
    group(parts, loc=(0, 0.0, 0.02), rot=(-16, 10, 0), scale=1.1 if mini else 1.0)


def solitario_b(mini):
    tile("#2F9A5E", "#0C3A22")
    labs = (("K", False), ("Q", True), ("J", False))
    parts = [_pcard(-0.1 + i * 0.1, 0.16 - i * 0.17, 0.02 + i * 0.025, 0, l, r, 0.85, top=True)
             for i, (l, r) in enumerate(labs)]
    group(parts, loc=(0, 0.0, 0.02), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


# -- Sudoku ----------------------------------------------------------------

def sudoku_a(mini):
    tile("#4F86FF", "#16205E")
    k = 0.22
    parts = [rbox((0, 0, 0.04), (0.74, 0.74, 0.08), 0.05, plastic("#F4F5F8", 0.4))]
    digits = {(0, 0): "5", (1, 1): "9", (2, 0): "3", (0, 2): "7", (2, 2): "1"} if not mini else {(1, 1): "9", (0, 0): "5"}
    for r in range(3):
        for c in range(3):
            x, y = (c - 1) * (k + 0.012), (1 - r) * (k + 0.012)
            parts.append(rbox((x, y, 0.09), (k, k, 0.025), 0.03, plastic("#E3E9F8" if (r + c) % 2 else "#FFFFFF", 0.4)))
            if (r, c) in digits:
                parts.append(text(digits[(r, c)], glossy("#2D5BD8" if (r, c) != (1, 1) else "#FF3B30"),
                                  size=0.17 if not mini else 0.2, depth=0.03, bevel=0.005, c=(x, y, 0.1)))
    group(parts, loc=(0, 0, 0.12), rot=(-16, 10, 0), scale=0.95)


def sudoku_b(mini):
    tile("#4F86FF", "#16205E")
    group([text("9", glossy("#FFFFFF"), size=0.78, depth=0.16, bevel=0.035, c=(0, 0.0, 0.02))],
          loc=(0, 0, 0.04), rot=(-18, 12, 0), scale=1.0)


# -- Fusión ----------------------------------------------------------------

def _num_tile(x, y, z, lab, c, s=1.0, rz=0):
    parts = [rbox((0, 0, 0), (0.32 * s, 0.32 * s, 0.14 * s), 0.07 * s, glossy(c)),
             text(lab, plastic("#FFFFFF", 0.35), size=0.22 * s, depth=0.03, bevel=0.007, c=(0, 0, 0.065 * s))]
    return group(parts, loc=(x, y, z), rot=(0, 0, rz), name="num")


def sumar_a(mini):
    tile("#5A4CC8", "#0D1233")
    parts = [_num_tile(-0.19, 0.08, 0.1, "2", "#FF9F0A", 1.0, 8), _num_tile(0.19, -0.08, 0.1, "2", "#FF9F0A", 1.0, -8)]
    if not mini:
        for a in (-40, 0, 40):
            r = math.radians(a + 90)
            parts.append(capsule((0.08 * math.cos(r), 0.12 + 0.08 * math.sin(r), 0.12),
                                 (0.14 * math.cos(r), 0.12 + 0.14 * math.sin(r), 0.12), 0.015, glow("#FFFFFF", 1.0)))
    group(parts, loc=(0, 0, 0), rot=(-16, 10, 0), scale=1.1 if mini else 1.0)


def sumar_b(mini):
    tile("#5A4CC8", "#0D1233")
    parts = [rbox((0, 0, 0.1), (0.56, 0.56, 0.2), 0.12, glossy("#FF9F0A"))]
    for rz in (0, 90):
        b = rbox((0, 0, 0.21), (0.32, 0.08, 0.04), 0.035, plastic("#FFFFFF", 0.35))
        b.rotation_euler.z = math.radians(rz)
        parts.append(b)
    group(parts, loc=(0, 0, 0), rot=(-16, 10, 0), scale=1.05)


# -- Tanques ---------------------------------------------------------------

def _tank(mini, s=1.0):
    green, dark = glossy("#6FB04A"), plastic("#3A3F2E", 0.5)
    parts = [rbox((0, 0, 0.1 * s), (0.44 * s, 0.6 * s, 0.12 * s), 0.05 * s, green)]
    for x in (-0.26, 0.26):
        parts.append(rbox((x * s, 0, 0.07 * s), (0.12 * s, 0.66 * s, 0.12 * s), 0.05 * s, dark))
    parts += [ball((0, -0.04 * s, 0.18 * s), 0.17 * s, glossy("#7FC85A"), scale=(1, 1, 0.6)),
              capsule((0, 0.04 * s, 0.22 * s), (0.0, 0.42 * s, 0.24 * s), 0.035 * s, glossy("#5A9A3A"))]
    if not mini:
        parts.append(cyl((0, -0.04 * s, 0.28 * s), 0.05 * s, 0.03, plastic("#4A7A2E"), bevel=0.01))
        for y in (-0.2, 0.0, 0.2):
            for x in (-0.26, 0.26):
                parts.append(cyl((x * s, y * s, 0.07 * s), 0.05 * s, 0.13 * s, metal("#8E939C"), rot=(0, R90, 0)))
    return parts


def tanques_a(mini):
    tile("#7FC85A", "#2E6FB0")
    group(_tank(mini, 1.0), loc=(0, -0.04, 0.0), rot=(-16, 10, -30), scale=1.08 if mini else 1.0)


def tanques_b(mini):
    tile("#7FC85A", "#2E6FB0")
    parts = [ball((0, -0.08, 0.1), 0.3, glossy("#7FC85A"), scale=(1, 1, 0.55)),
             cyl((0, 0.22, 0.18), 0.07, 0.42, glossy("#5A9A3A"), rot=(-R90 + 0.15, 0, 0), bevel=0.02),
             cyl((0, 0.43, 0.21), 0.085, 0.06, plastic("#3A3F2E"), rot=(-R90 + 0.15, 0, 0), bevel=0.015)]
    if not mini:
        parts += [cyl((0.1, -0.16, 0.26), 0.07, 0.05, plastic("#4A7A2E"), bevel=0.015),
                  ball((-0.12, -0.1, 0.26), 0.035, glossy("#FFD60A"))]
    group(parts, loc=(0, -0.04, 0.0), rot=(-16, 10, -20), scale=1.05 if mini else 1.0)


# -- Topos -----------------------------------------------------------------

def _mole(x, y, s, mini):
    fur = glossy("#8A5A3C")
    parts = [ball((x, y - 0.16 * s, 0.01), 0.3 * s, matte("#3A2414"), scale=(1, 0.5, 0.1)),
             blob([(x, y, 0.12 * s, 0.24 * s), (x, y + 0.1 * s, 0.14 * s, 0.2 * s)], fur, scale=(1, 1, 0.9)),
             ball((x, y + 0.04 * s, 0.3 * s), 0.05 * s, glossy("#FF8FB0"))]
    for e in (-1, 1):
        parts.append(ball((x + e * 0.08 * s, y + 0.12 * s, 0.3 * s), 0.03 * s, plastic("#1C1C1E")))
    if not mini:
        parts.append(rbox((x, y - 0.03 * s, 0.29 * s), (0.06 * s, 0.04 * s, 0.03 * s), 0.01, plastic("#FFFFFF")))
    return parts


def topos_a(mini):
    tile("#5AAE3E", "#6B4020")
    group(_mole(0, 0.0, 1.15, mini), loc=(0, 0, 0), rot=(-14, 8, 0), scale=1.05)


def topos_b(mini):
    tile("#5AAE3E", "#6B4020")
    parts = _mole(-0.12, -0.12, 0.8, True)
    wood = plastic("#C8874A", 0.45)
    ham = [cyl((0, 0, 0), 0.1, 0.3, glossy("#FF3B30"), rot=(0, R90, 0), bevel=0.04),
           capsule((0, -0.04, 0), (0, -0.4, 0), 0.03, wood)]
    parts.append(group(ham, loc=(0.18, 0.22, 0.3), rot=(0, 0, -30), name="hammer"))
    group(parts, loc=(0, 0, 0), rot=(-14, 8, 0), scale=1.05 if mini else 1.0)


# -- Turbo -----------------------------------------------------------------

def turbo_a(mini):
    tile("#F06A3A", "#3A1060")
    red = glossy("#FF2D2D")
    parts = [rbox((0, 0, 0.08), (0.34, 0.7, 0.08), 0.04, red),
             rbox((0, -0.04, 0.14), (0.26, 0.26, 0.07), 0.035, glossy("#1C2A44")),
             rbox((0, -0.32, 0.15), (0.38, 0.06, 0.04), 0.02, red)]
    for x in (-0.19, 0.19):
        for y in (-0.22, 0.22):
            parts.append(cyl((x, y, 0.06), 0.07, 0.07, plastic("#1C1C1E", 0.5), rot=(0, R90, 0), bevel=0.012))
    if not mini:
        for x in (-0.1, 0.1):
            parts.append(ball((x, 0.34, 0.09), 0.03, glow("#FFF2B0", 1.5)))
        parts.append(rbox((0, 0.12, 0.125), (0.06, 0.3, 0.01), 0.005, plastic("#FFFFFF")))
    group(parts, loc=(0, 0.0, 0.0), rot=(-16, 10, -35), scale=1.1 if mini else 1.05)


def turbo_b(mini):
    tile("#F06A3A", "#3A1060")
    dark = plastic("#2C2C2E", 0.35)
    parts = [slab([circle(0, 0, 0.36), circle(0, 0, 0.27)], 0.09, 0.035, dark)]
    for a in (210, 330, 90):
        r = math.radians(a)
        parts.append(capsule((0, 0, 0.05), (0.28 * math.cos(r), 0.28 * math.sin(r), 0.05), 0.04, dark))
    parts += [cyl((0, 0, 0.07), 0.1, 0.06, glossy("#FF2D2D"), bevel=0.02)]
    if not mini:
        parts.append(rbox((0, 0.31, 0.09), (0.06, 0.08, 0.02), 0.01, glossy("#FFD60A")))
    group(parts, loc=(0, 0, 0.1), rot=(-20, 10, 0), scale=1.05)


# -- Vitral ----------------------------------------------------------------

def vitral_a(mini):
    tile("#4A3A6A", "#14102A")
    lead = plastic("#2C2C2E", 0.4)
    cols = ("#FF3B30", "#2D7BF0", "#FFD60A", "#34C759", "#BF5AF2", "#FF9F0A")
    n = 3 if not mini else 2
    k = 0.6 / n
    parts = [rbox((0, 0, 0.03), (0.7, 0.7, 0.06), 0.05, lead)]
    for r in range(n):
        for c in range(n):
            parts.append(rbox(((c - (n - 1) / 2) * k, ((n - 1) / 2 - r) * k, 0.07), (k * 0.86, k * 0.86, 0.03),
                              0.02, glow(cols[(r * n + c) % len(cols)], 0.7)))
    group(parts, loc=(0, 0, 0.1), rot=(-16, 10, 0), scale=1.0)


def vitral_b(mini):
    tile("#4A3A6A", "#14102A")
    k = 0.2
    cells = ((-1, 0), (0, 0), (1, 0), (0, -1))
    parts = []
    for cx, cy in cells:
        parts.append(rbox((cx * (k + 0.02), cy * (k + 0.02) + 0.1, 0.1), (k, k, 0.14), 0.04, glow("#2D9BFF", 0.8)))
    if not mini:
        for x, y, c in ((-0.26, -0.34, "#FF3B30"), (0.0, -0.34, "#FFD60A"), (0.26, -0.34, "#34C759")):
            parts.append(rbox((x, y, 0.05), (0.22, 0.12, 0.08), 0.03, glow(c, 0.6)))
    group(parts, loc=(0, 0, 0), rot=(-16, 10, 0), scale=1.05)


# -- Planeo ----------------------------------------------------------------

def _avioneta(mini, s=1.0):
    red, white = glossy("#FF3B30"), glossy("#F4F5F8")
    parts = [capsule((0, -0.3 * s, 0.14), (0, 0.26 * s, 0.14), 0.07 * s, white),
             rbox((0, 0.06 * s, 0.2), (0.8 * s, 0.14 * s, 0.04), 0.03, red),
             rbox((0, -0.3 * s, 0.17), (0.3 * s, 0.08 * s, 0.03), 0.02, red),
             ball((0, 0.12 * s, 0.2), 0.06 * s, glossy("#5EC8FF"), scale=(1, 1.4, 0.6)),
             cyl((0, 0.32 * s, 0.14), 0.035 * s, 0.05 * s, red, rot=(R90, 0, 0))]
    blade = rbox((0, 0.35 * s, 0.14), (0.3 * s, 0.03 * s, 0.015), 0.01, plastic("#2C2C2E"))
    blade.rotation_euler.y = math.radians(20)
    parts.append(blade)
    return parts


def vuelo_a(mini):
    tile("#9AD8F4", "#3C8CE0")
    group(_avioneta(mini, 1.0), loc=(0, -0.02, 0.0), rot=(-18, 10, -30), scale=1.05)


def vuelo_b(mini):
    tile("#9AD8F4", "#3C8CE0")
    parts = [cyl((0, -0.1, 0.02), 0.34, 0.04, glossy("#2D9BFF"), verts=96),
             blob([(-0.04, -0.12, 0.04, 0.2), (0.1, -0.06, 0.04, 0.16), (-0.12, -0.2, 0.04, 0.12)],
                  plastic("#5AC85A", 0.45), scale=(1, 1, 0.6)),
             ball((0.0, -0.1, 0.1), 0.08, plastic("#FFFFFF", 0.5), scale=(1, 1, 0.9))]
    plane = group(_avioneta(True, 1.0), loc=(0.14, 0.24, 0.12), rot=(0, 0, -30), scale=0.42, name="plane")
    group(parts + [plane], loc=(0, 0, 0.06), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


MODELS = {
    "demo.mila": {"a": mila_a, "b": mila_b},
    "demo.monsterhop": {"a": monsterhop_a, "b": monsterhop_b},
    "demo.neon": {"a": neon_a, "b": neon_b},
    "demo.pinball": {"a": pinball_a, "b": pinball_b},
    "demo.solitario": {"a": solitario_a, "b": solitario_b},
    "demo.sudoku": {"a": sudoku_a, "b": sudoku_b},
    "demo.sumar": {"a": sumar_a, "b": sumar_b},
    "demo.tanques": {"a": tanques_a, "b": tanques_b},
    "demo.topos": {"a": topos_a, "b": topos_b},
    "demo.turbo": {"a": turbo_a, "b": turbo_b},
    "demo.vitral": {"a": vitral_a, "b": vitral_b},
    "demo.vuelo": {"a": vuelo_a, "b": vuelo_b},
}
