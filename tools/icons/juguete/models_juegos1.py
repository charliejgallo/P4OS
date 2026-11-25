"""'juguete' models: the group juegos-1 of apps.json."""

import math

from lib import (arc_band, ball, blob, capsule, circle, cyl, glossy, glow, group, matte, metal, plastic, rbox,
                 rounded_poly, rrect, slab, star, text, tile, tube)

R90 = math.radians(90)

PIPS = {1: [(0, 0)], 2: [(-1, 1), (1, -1)], 3: [(-1, 1), (0, 0), (1, -1)],
        4: [(-1, 1), (1, 1), (-1, -1), (1, -1)], 5: [(-1, 1), (1, 1), (0, 0), (-1, -1), (1, -1)],
        6: [(-1, 1), (1, 1), (-1, 0), (1, 0), (-1, -1), (1, -1)]}


def _die(c, e, n, body, pip, rz=0, mini=False):
    parts = [rbox(c, (e, e, e), e * 0.18, body)]
    k = e * 0.27
    for px, py in PIPS[n if not mini else min(n, 3)]:
        parts.append(ball((c[0] + px * k, c[1] + py * k, c[2] + e / 2), e * 0.09, pip, scale=(1, 1, 0.4)))
    g = group(parts, name="die")
    g.rotation_euler.z = math.radians(rz)
    return g


# -- Dados -----------------------------------------------------------------

def dice_a(mini):
    tile("#C77DFF", "#5B2078")
    d1 = _die((0, 0, 0), 0.34, 5, glossy("#FFFFFF"), plastic("#1C1C1E"), 0, mini)
    d1.location = (-0.15, 0.12, 0.17)
    d2 = _die((0, 0, 0), 0.3, 3, glossy("#FF453A"), plastic("#FFFFFF"), 20, mini)
    d2.location = (0.18, -0.16, 0.15)
    group([d1, d2], loc=(0, 0, 0), rot=(-18, 12, 0), scale=1.0)


def dice_b(mini):
    tile("#C77DFF", "#5B2078")
    d = _die((0, 0, 0), 0.52, 6 if not mini else 3, glossy("#FFFFFF"), plastic("#5B2078"), 0, mini)
    d.location = (0, 0, 0.26)
    root = group([d], loc=(0, 0, 0), scale=1.0)
    from lib import turn
    turn(root, ("Z", 20), ("X", -30), ("Y", 12))


# -- Buscaminas ------------------------------------------------------------

def mines_a(mini):
    tile("#7A7A80", "#1C1C1E")
    parts = [ball((-0.04, -0.06, 0.2), 0.27, glossy("#1C1C1E")),
             cyl((0.12, 0.12, 0.34), 0.07, 0.1, metal("#8E93A0"), rot=(-0.6, 0.6, 0), bevel=0.02),
             tube([(0.16, 0.16, 0.38), (0.24, 0.26, 0.38), (0.3, 0.26, 0.36)], 0.018, matte("#C8A070"))]
    parts.append(slab([rounded_poly(star(0.32, 0.3, 0.1, 0.04, 8), 0.01)], 0.03, 0.01, glow("#FFC21A", 2.5), z=0.34))
    if not mini:
        parts.append(ball((-0.13, 0.04, 0.4), 0.06, glow("#FFFFFF", 0.4), scale=(1.3, 1, 0.5)))
    group(parts, loc=(0, 0, 0), rot=(-10, 8, 0), scale=1.1 if mini else 1.0)


def mines_b(mini):
    tile("#7A7A80", "#1C1C1E")
    parts = [rbox((0, -0.08, 0.05), (0.56, 0.56, 0.1), 0.05, plastic("#D9DCE3", 0.35)),
             capsule((-0.1, -0.25, 0.1), (-0.1, 0.32, 0.12), 0.025, plastic("#2C2C2E")),
             slab([rounded_poly([(-0.1, 0.32), (0.26, 0.2), (-0.1, 0.06)], 0.025)], 0.05, 0.015, glossy("#FF3B30"),
                  z=0.1),
             cyl((-0.1, -0.24, 0.1), 0.09, 0.03, plastic("#2C2C2E"), bevel=0.01)]
    group(parts, loc=(0, 0.0, 0.04), rot=(-16, 10, 0), scale=1.1 if mini else 1.0)


# -- Simon -----------------------------------------------------------------

def simon_a(mini):
    tile("#3C3C41", "#0A0A0C")
    cols = ("#34C759", "#FF3B30", "#2D7BF0", "#FFD60A")
    parts = [cyl((0, 0, 0.04), 0.4, 0.08, plastic("#1C1C1E", 0.35), bevel=0.03, verts=96)]
    for i, c in enumerate(cols):
        a0 = 90 * i + 5
        m = glow(c, 1.6) if i == 1 else glossy(c)
        parts.append(slab([arc_band(0, 0, 0.15, 0.35, a0, a0 + 80, 20)], 0.06, 0.015, m, z=0.08))
    parts.append(cyl((0, 0, 0.1), 0.11, 0.06, plastic("#2C2C2E"), bevel=0.02))
    group(parts, loc=(0, 0, 0.06), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


def simon_b(mini):
    tile("#3C3C41", "#0A0A0C")
    cols = ("#34C759", "#FF3B30", "#FFD60A", "#2D7BF0")
    parts = []
    for i, c in enumerate(cols):
        x, y = (-0.17 if i % 2 == 0 else 0.17), (0.17 if i < 2 else -0.17)
        parts.append(rbox((x, y, 0.08), (0.3, 0.3, 0.12), 0.1, glow(c, 1.6) if i == 2 else glossy(c)))
    group(parts, loc=(0, 0, 0.0), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


# -- Truco -----------------------------------------------------------------

def _suit(kind, x, y, z, s):
    if kind == "oro":
        return [cyl((x, y, z), 0.08 * s, 0.02, metal("#F2C14E", 0.25), verts=48)]
    if kind == "copa":
        return [cyl((x, y + 0.03 * s, z), 0.07 * s, 0.02, glossy("#E33030"), verts=32),
                rbox((x, y - 0.06 * s, z), (0.03 * s, 0.1 * s, 0.02), 0.01, glossy("#E33030"))]
    if kind == "basto":
        return [capsule((x - 0.04 * s, y - 0.1 * s, z), (x + 0.04 * s, y + 0.1 * s, z), 0.035 * s, glossy("#2E8B3E"))]
    return [rbox((x, y + 0.02 * s, z), (0.03 * s, 0.24 * s, 0.02), 0.012, metal("#7FB2FF", 0.25)),
            rbox((x, y - 0.06 * s, z), (0.12 * s, 0.025 * s, 0.02), 0.01, metal("#F2C14E", 0.25))]


def _card(x, y, rz, kind, s=1.0, z=0.0):
    parts = [rbox((0, 0, 0), (0.3 * s, 0.46 * s, 0.025), 0.03 * s, plastic("#FFFDF5", 0.4)),
             slab([rrect(0, 0, 0.3 * s - 0.03, 0.46 * s - 0.03, 0.025 * s), rrect(0, 0, 0.3 * s - 0.05, 0.46 * s - 0.05, 0.02 * s)],
                  0.004, 0.0, plastic({"oro": "#E0A21A", "copa": "#E33030", "basto": "#2E8B3E", "espada": "#2D5BD8"}[kind]),
                  z=0.0125)]
    parts += _suit(kind, 0, 0, 0.02, s)
    return group(parts, loc=(x, y, z), rot=(0, 0, rz), name="card")


def truco_a(mini):
    tile("#2FA05E", "#0A3C21")
    cards = [_card(-0.17, -0.04, 18, "oro", z=0.04), _card(0.0, 0.02, 0, "copa", z=0.08),
             _card(0.17, -0.04, -18, "espada", z=0.12)]
    if mini:
        cards = [_card(-0.12, -0.02, 12, "oro", 1.2, z=0.04), _card(0.12, 0.0, -12, "espada", 1.2, z=0.08)]
    group(cards, loc=(0, 0.0, 0.02), rot=(-16, 10, 0), scale=1.05)


def truco_b(mini):
    tile("#2FA05E", "#0A3C21")
    group([_card(0, 0, -6, "espada", 1.7, z=0.04)], loc=(0, 0.0, 0.04), rot=(-16, 10, 0), scale=1.0)


def truco_c(mini):
    tile("#2FA05E", "#0A3C21")
    parts = [blob([(0, -0.06, 0.16, 0.34), (0, 0.06, 0.16, 0.28)], glossy("#7A4E2A"), scale=(1, 1, 0.9)),
             cyl((0, 0.1, 0.25), 0.15, 0.04, plastic("#5A3A1E"), verts=64),
             cyl((0, 0.1, 0.27), 0.12, 0.02, matte("#4E8A2A"), verts=64),
             capsule((0.04, 0.1, 0.28), (0.22, 0.38, 0.34), 0.02, metal("#D7DBE2", 0.2))]
    if not mini:
        parts.append(slab([circle(0, 0.1, 0.16), circle(0, 0.1, 0.13)], 0.03, 0.01, metal("#D7DBE2", 0.25), z=0.25))
    group(parts, loc=(0, -0.04, 0.0), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


# -- 2043 ------------------------------------------------------------------

def _ship(m, mini, s=1.0):
    hull = rounded_poly([(0, 0.36), (0.09, 0.06), (0.34, -0.14), (0.3, -0.24), (0.08, -0.18), (0.06, -0.3),
                         (-0.06, -0.3), (-0.08, -0.18), (-0.3, -0.24), (-0.34, -0.14), (-0.09, 0.06)],
                        [0.04, 0.03, 0.04, 0.04, 0.02, 0.02, 0.02, 0.02, 0.04, 0.04, 0.03])
    parts = [slab([[(x * s, y * s) for x, y in hull]], 0.12, 0.04, m),
             ball((0, 0.04 * s, 0.11), 0.07 * s, glossy("#5EC8FF"), scale=(0.8, 1.4, 0.6))]
    if not mini:
        for x in (-0.05, 0.05):
            parts.append(ball((x * s, -0.31 * s, 0.06), 0.035 * s, glow("#FFB020", 2.5)))
    return parts


def g2043_a(mini):
    tile("#FF6A50", "#6A2FB5")
    group(_ship(glossy("#F4F5F8"), mini), loc=(0, 0.0, 0.04), rot=(-16, 10, -12), scale=1.1 if mini else 1.0)


def g2043_b(mini):
    tile("#FF6A50", "#6A2FB5")
    parts = _ship(glossy("#F4F5F8"), mini, 0.75)
    for x, y in ((-0.12, 0.3), (0.12, 0.3), (-0.12, 0.44), (0.12, 0.44))[: 4 if not mini else 2]:
        parts.append(capsule((x, y - 0.05, 0.08), (x, y + 0.03, 0.08), 0.022, glow("#5CFF9A", 2.5)))
    group(parts, loc=(0, -0.08, 0.04), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


# -- Arkanos ---------------------------------------------------------------

def arkanos_a(mini):
    tile("#FFB347", "#0A6AD8")
    cols = ("#FF3B30", "#FFD60A", "#34C759", "#5EE7FF")
    parts = []
    n = 4 if not mini else 3
    for r in range(2):
        for c in range(n):
            w = 0.7 / n
            parts.append(rbox((-0.35 + w * (c + 0.5), 0.3 - r * 0.12, 0.06), (w * 0.9, 0.1, 0.1), 0.03,
                              glossy(cols[(r * 2 + c) % 4])))
    parts += [capsule((-0.14, -0.32, 0.07), (0.14, -0.32, 0.07), 0.05, glossy("#FFFFFF")),
              ball((0.08, -0.08, 0.08), 0.06, metal("#D7DBE2", 0.15))]
    group(parts, loc=(0, 0.0, 0.04), rot=(-16, 10, 0), scale=1.0)


def arkanos_b(mini):
    tile("#FFB347", "#0A6AD8")
    cols = ("#FF3B30", "#FF9F0A", "#FFD60A")
    parts = []
    for r in range(3):
        n = 3 if r % 2 == 0 else 2
        for c in range(n):
            x = (c - (n - 1) / 2) * 0.24
            parts.append(rbox((x, 0.2 - r * 0.17, 0.08), (0.22, 0.14, 0.14), 0.035, glossy(cols[r])))
    group(parts, loc=(0, 0.0, 0.06), rot=(-18, 12, 0), scale=1.05 if mini else 1.0)


# -- Deriva (asteroides) ---------------------------------------------------

def _rock(x, y, z, r, seed, m):
    import random
    rnd = random.Random(seed)
    sph = [(x + rnd.uniform(-0.4, 0.4) * r, y + rnd.uniform(-0.4, 0.4) * r, z, r * rnd.uniform(0.6, 0.9))
           for _ in range(5)]
    return blob(sph, m, threshold=0.5)


def asteroides_a(mini):
    tile("#2A5C8A", "#050A18")
    ship = slab([rounded_poly([(0, 0.18), (0.12, -0.14), (0, -0.07), (-0.12, -0.14)], 0.02)], 0.06, 0.02,
                glow("#5EE7FF", 1.2), z=0.06)
    ship.location.x, ship.location.y = -0.18, -0.16
    ship.rotation_euler.z = math.radians(-35)
    parts = [ship, _rock(0.14, 0.14, 0.12, 0.2, 3, plastic("#9A8A7A", 0.6))]
    if not mini:
        parts += [capsule((-0.06, 0.0, 0.08), (0.0, 0.06, 0.08), 0.015, glow("#FFFFFF", 2.0)),
                  _rock(-0.26, 0.26, 0.08, 0.07, 5, plastic("#7A6A5A", 0.6))]
    group(parts, loc=(0, 0, 0), rot=(-14, 8, 0), scale=1.1 if mini else 1.0)


def asteroides_b(mini):
    tile("#2A5C8A", "#050A18")
    parts = [_rock(0, 0, 0.18, 0.3, 7, plastic("#A89482", 0.6))]
    for x, y, r in ((-0.1, 0.08, 0.07), (0.12, -0.04, 0.05), (-0.02, -0.14, 0.04))[: 3 if not mini else 2]:
        parts.append(ball((x, y, 0.33), r, matte("#6E5E50"), scale=(1, 1, 0.3)))
    group(parts, loc=(0, 0, 0), rot=(-14, 8, 0), scale=1.05)


# -- Atasco ----------------------------------------------------------------

def _car(x, y, rz, body, s=1.0, mini=False):
    parts = [rbox((0, 0, 0.08 * s), (0.26 * s, 0.46 * s, 0.1 * s), 0.05 * s, body),
             rbox((0, -0.02 * s, 0.15 * s), (0.2 * s, 0.22 * s, 0.08 * s), 0.04 * s, glossy("#9FD3FF"))]
    for wx in (-0.14, 0.14):
        for wy in (-0.14, 0.14):
            parts.append(cyl((wx * s, wy * s, 0.05 * s), 0.055 * s, 0.05 * s, plastic("#1C1C1E", 0.5),
                             rot=(0, R90, 0), bevel=0.01))
    if not mini:
        for hx in (-0.07, 0.07):
            parts.append(ball((hx * s, 0.23 * s, 0.09 * s), 0.025 * s, glow("#FFF2B0", 1.5)))
    return group(parts, loc=(x, y, 0), rot=(0, 0, rz), name="car")


def atasco_a(mini):
    tile("#8A8A90", "#3A3A3C")
    group([_car(0, 0, -30, glossy("#FF3B30"), 1.4, mini)], loc=(0, 0, 0.02), rot=(-16, 10, 0), scale=1.05)


def atasco_b(mini):
    tile("#8A8A90", "#3A3A3C")
    parts = [_car(-0.02, 0.0, 90, glossy("#FF3B30"), 1.0, True),
             rbox((-0.3, 0.26, 0.08), (0.18, 0.3, 0.14), 0.05, glossy("#FFD60A")),
             rbox((0.28, -0.22, 0.08), (0.18, 0.4, 0.14), 0.05, glossy("#2D7BF0"))]
    if not mini:
        parts.append(rbox((0.2, 0.3, 0.08), (0.36, 0.16, 0.14), 0.05, glossy("#34C759")))
    group(parts, loc=(0, 0, 0.02), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


# -- Bastión ---------------------------------------------------------------

def bastion_a(mini):
    tile("#34609E", "#070A18")
    wall = plastic("#D9DEE5", 0.35)
    parts = [cyl((0, 0, 0.06), 0.36, 0.12, wall, bevel=0.03, verts=8),
             ball((0, 0, 0.12), 0.18, glass_dome())]
    for i in range(4 if not mini else 3):
        a = math.radians(45 + i * (90 if not mini else 120))
        x, y = 0.3 * math.cos(a), 0.3 * math.sin(a)
        parts += [cyl((x, y, 0.16), 0.08, 0.2, wall, bevel=0.02), ball((x, y, 0.27), 0.05, glow("#5EE7FF", 2.0))]
    group(parts, loc=(0, 0, 0.04), rot=(-20, 10, 0), scale=1.05 if mini else 0.95)


def glass_dome():
    return glow("#7FD4FF", 0.4)


def bastion_b(mini):
    tile("#34609E", "#070A18")
    parts = [cyl((0, -0.08, 0.06), 0.3, 0.12, plastic("#5A6170", 0.4), bevel=0.04, verts=64),
             ball((0, -0.08, 0.14), 0.2, glossy("#D9DEE5"), scale=(1, 1, 0.8)),
             cyl((0.12, 0.14, 0.2), 0.06, 0.4, metal("#8E93A0"), rot=(-R90 + 0.2, 0, -0.5), bevel=0.015),
             ball((0, -0.08, 0.28), 0.04, glow("#FF453A", 2.0))]
    if not mini:
        parts.append(cyl((0.22, 0.31, 0.22), 0.075, 0.06, metal("#5A6170"), rot=(-R90 + 0.2, 0, -0.5)))
    group(parts, loc=(0, 0, 0.0), rot=(-14, 8, 0), scale=1.05 if mini else 1.0)


# -- Blackjack -------------------------------------------------------------

def _chip_stack(x, y, n, c):
    out = []
    for i in range(n):
        out.append(cyl((x, y, 0.035 + i * 0.05), 0.14, 0.045, glossy(c), bevel=0.012, verts=64))
        out.append(slab([circle(x, y, 0.1), circle(x, y, 0.085)], 0.002, 0.0, plastic("#FFFFFF"), z=0.057 + i * 0.05))
    return out


def blackjack_a(mini):
    tile("#2E9358", "#0B3320")
    parts = []
    for x, rz, lab, c in ((-0.1, 10, "A", "#1C1C1E"), (0.12, -8, "K", "#E33030")):
        card = [rbox((0, 0, 0), (0.32, 0.46, 0.02), 0.03, plastic("#FFFFFF", 0.4)),
                text(lab, plastic(c, 0.4), size=0.22, depth=0.015, bevel=0.003, c=(0, 0.02, 0.01))]
        parts.append(group(card, loc=(x, 0.06, 0.05 + (0.04 if x > 0 else 0)), rot=(0, 0, rz), name="card"))
    if not mini:
        parts += _chip_stack(0.24, -0.26, 2, "#E33030")
    group(parts, loc=(0, 0, 0.02), rot=(-16, 10, 0), scale=1.15 if mini else 1.0)


def blackjack_b(mini):
    tile("#2E9358", "#0B3320")
    parts = _chip_stack(-0.14, 0.06, 4, "#E33030") + _chip_stack(0.16, -0.08, 3 if not mini else 2, "#2D5BD8")
    group(parts, loc=(0, 0, 0.0), rot=(-20, 10, 0), scale=1.1 if mini else 1.0)


# -- Burbujas --------------------------------------------------------------

def burbujas_a(mini):
    tile("#3A4CA8", "#101430")
    b = ((-0.18, 0.16, 0.15, "#FF3B30"), (0.12, 0.2, 0.13, "#FFD60A"), (0.2, -0.12, 0.16, "#34C759"),
         (-0.12, -0.18, 0.14, "#5EE7FF"), (0.0, 0.0, 0.1, "#BF5AF2"))
    parts = []
    for x, y, r, c in (b if not mini else b[:4]):
        parts += [ball((x, y, r), r, glossy(c)),
                  ball((x - r * 0.35, y + r * 0.35, r * 1.75), r * 0.22, glow("#FFFFFF", 0.6), scale=(1, 1, 0.5))]
    group(parts, loc=(0, 0, 0), rot=(-10, 8, 0), scale=1.1 if mini else 1.05)


def burbujas_b(mini):
    tile("#3A4CA8", "#101430")
    parts = [cyl((0, -0.3, 0.06), 0.2, 0.12, plastic("#D9DEE5", 0.35), bevel=0.04, verts=64),
             slab([rounded_poly([(-0.06, -0.3), (0.06, -0.3), (0.06, -0.02), (0.13, -0.02), (0, 0.12), (-0.13, -0.02),
                                 (-0.06, -0.02)], 0.02)], 0.06, 0.02, glossy("#FF9F0A"), z=0.12),
             ball((0, 0.26, 0.14), 0.14, glossy("#FF3B30")),
             ball((-0.05, 0.31, 0.25), 0.03, glow("#FFFFFF", 0.6), scale=(1, 1, 0.5))]
    group(parts, loc=(0, 0, 0), rot=(-16, 10, -20), scale=1.1 if mini else 1.0)


# -- Caños -----------------------------------------------------------------

def canos_a(mini):
    tile("#3F8FC4", "#0C2238")
    pipe = glossy("#34C759")
    parts = [tube([(-0.42, 0.16, 0.12), (0.1, 0.16, 0.12), (0.16, 0.16, 0.12), (0.16, 0.1, 0.12), (0.16, -0.42, 0.12)],
                  0.11, pipe, order=3)]
    for p in ((-0.3, 0.16), (0.16, -0.3)):
        horiz = p[1] == 0.16
        parts.append(cyl((p[0], p[1], 0.12), 0.135, 0.06, glossy("#2E9F4C"),
                         rot=((0, R90, 0) if horiz else (R90, 0, 0)), bevel=0.015))
    parts.append(ball((0.16, -0.36, 0.05), 0.08, glossy("#5EC8FF"), scale=(1.3, 1, 0.4)))
    group(parts, loc=(0, 0.04, 0.0), rot=(-14, 8, 0), scale=1.0)


def canos_b(mini):
    tile("#3F8FC4", "#0C2238")
    steel = metal("#C9CED6", 0.25)
    parts = [cyl((0, -0.1, 0.12), 0.1, 0.9, steel, rot=(0, R90, 0)),
             rbox((0, -0.1, 0.12), (0.2, 0.24, 0.24), 0.06, steel),
             capsule((0, -0.1, 0.24), (0, -0.1, 0.32), 0.03, steel),
             slab([circle(0, 0, 0.22), circle(0, 0, 0.17)], 0.05, 0.02, glossy("#FF3B30"), z=0.0)]
    wheel = parts[-1]
    wheel.location = (0, -0.1, 0.32)
    if not mini:
        for a in (0, 90):
            sp = rbox((0, -0.1, 0.345), (0.36, 0.035, 0.03), 0.012, glossy("#FF3B30"))
            sp.rotation_euler.z = math.radians(a)
            parts.append(sp)
    group(parts, loc=(0, 0.06, 0.0), rot=(-22, 10, 0), scale=1.05 if mini else 1.0)


MODELS = {
    "aos.dice": {"a": dice_a, "b": dice_b},
    "aos.mines": {"a": mines_a, "b": mines_b},
    "aos.simon": {"a": simon_a, "b": simon_b},
    "aos.truco": {"a": truco_a, "b": truco_b, "c": truco_c},
    "demo.2043": {"a": g2043_a, "b": g2043_b},
    "demo.arkanos": {"a": arkanos_a, "b": arkanos_b},
    "demo.asteroides": {"a": asteroides_a, "b": asteroides_b},
    "demo.atasco": {"a": atasco_a, "b": atasco_b},
    "demo.bastion": {"a": bastion_a, "b": bastion_b},
    "demo.blackjack": {"a": blackjack_a, "b": blackjack_b},
    "demo.burbujas": {"a": burbujas_a, "b": burbujas_b},
    "demo.canos": {"a": canos_a, "b": canos_b},
}
