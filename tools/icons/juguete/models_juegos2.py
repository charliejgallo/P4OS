"""'juguete' models: the group juegos-2 of apps.json."""

import math

from lib import (ball, blob, capsule, circle, cyl, gear_loop, glossy, glow, group, matte, metal, plastic, rbox,
                 rounded_poly, slab, text, tile, tube)

R90 = math.radians(90)


def _eyes(x, y, z, s, gap=0.08):
    out = []
    for e in (-1, 1):
        out.append(ball((x + e * gap * s, y, z), 0.05 * s, plastic("#FFFFFF"), scale=(1, 1.2, 0.6)))
        out.append(ball((x + e * gap * s + 0.01 * s, y + 0.01 * s, z + 0.025 * s), 0.025 * s, plastic("#1C1C1E")))
    return out


# -- Chatarra --------------------------------------------------------------

def chatarra_a(mini):
    tile("#B0654A", "#2B1810")
    rust, steel = plastic("#C8743E", 0.5), metal("#9AA0A8", 0.4)
    parts = [rbox((0, -0.14, 0.12), (0.4, 0.3, 0.2), 0.05, rust),
             rbox((0, 0.16, 0.14), (0.32, 0.26, 0.2), 0.06, steel),
             cyl((0, 0.15, 0.25), 0.08, 0.04, plastic("#1C1C1E"), bevel=0.01),
             ball((0, 0.15, 0.27), 0.05, glow("#5CFF9A", 2.0)),
             capsule((0.1, 0.28, 0.2), (0.16, 0.4, 0.22), 0.015, steel),
             ball((0.16, 0.4, 0.22), 0.03, glossy("#FF3B30")),
             capsule((-0.22, -0.08, 0.14), (-0.36, -0.24, 0.12), 0.04, steel),
             capsule((0.22, -0.08, 0.14), (0.34, 0.06, 0.16), 0.04, steel)]
    if not mini:
        for x, y in ((-0.14, -0.06), (0.14, -0.06), (-0.14, -0.22), (0.14, -0.22)):
            parts.append(cyl((x, y, 0.225), 0.02, 0.02, metal("#5A5E66"), verts=6))
        parts.append(rbox((0.06, -0.16, 0.225), (0.14, 0.1, 0.01), 0.01, plastic("#7A4A2A", 0.6)))
    group(parts, loc=(0, 0.02, 0.0), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


def chatarra_b(mini):
    tile("#B0654A", "#2B1810")
    g = slab([gear_loop(0.32, 0.24, 8, 0.5), circle(0, 0, 0.1)], 0.12, 0.03, plastic("#C8743E", 0.55))
    parts = [g]
    for x, y in ((0.26, -0.3), (-0.32, 0.26)) if not mini else ((0.26, -0.3),):
        parts += [cyl((x, y, 0.1), 0.07, 0.04, metal("#9AA0A8", 0.35), verts=6, bevel=0.01),
                  cyl((x, y, 0.05), 0.03, 0.1, metal("#9AA0A8", 0.35))]
    group(parts, loc=(0, 0.02, 0.04), rot=(-18, 12, 0), scale=1.1 if mini else 1.0)


# -- Claude Jump -----------------------------------------------------------

def cjump_a(mini):
    tile("#5AB0FF", "#2E9E52")
    parts = [rbox((-0.18, -0.3, 0.05), (0.34, 0.08, 0.08), 0.04, glossy("#34C759")),
             rbox((0.22, 0.02, 0.05), (0.3, 0.08, 0.08), 0.04, glossy("#34C759")),
             ball((0.0, 0.1, 0.2), 0.16, glossy("#FF8A4A"), scale=(1, 1.05, 0.95))]
    parts += _eyes(0.0, 0.14, 0.33, 1.0, 0.06)
    if not mini:
        parts.append(rbox((-0.08, 0.3, 0.05), (0.26, 0.07, 0.07), 0.035, glossy("#34C759")))
        for x in (-0.06, 0.06):
            parts.append(capsule((x, -0.04, 0.16), (x * 1.6, -0.12, 0.12), 0.03, glossy("#FF8A4A")))
    group(parts, loc=(0, 0, 0.0), rot=(-14, 8, 0), scale=1.1 if mini else 1.0)


def cjump_b(mini):
    tile("#5AB0FF", "#2E9E52")
    n = 6
    coil = [(0.14 * math.cos(t * 2 * math.pi), -0.28 + 0.5 * t / n, 0.12 + 0.06 * math.sin(t * 2 * math.pi))
            for t in [i / 4 for i in range(n * 4 + 1)]]
    coil = [(x, -0.3 + i * 0.022, z) for i, (x, _y, z) in enumerate(coil)]
    parts = [tube(coil, 0.025 if not mini else 0.035, metal("#D7DBE2", 0.2), order=3),
             rbox((0, 0.3, 0.12), (0.5, 0.12, 0.1), 0.05, glossy("#34C759")),
             rbox((0, -0.36, 0.05), (0.36, 0.08, 0.06), 0.03, glossy("#FF8A4A"))]
    group(parts, loc=(0, 0.0, 0.0), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


# -- Claudito --------------------------------------------------------------

def _pet(x, y, s, mini):
    body = glossy("#FFA06A")
    parts = [blob([(x, y, 0.16 * s, 0.32 * s), (x, y + 0.08 * s, 0.18 * s, 0.24 * s)], body, scale=(1, 1, 0.85)),
             ball((x - 0.15 * s, y + 0.24 * s, 0.14 * s), 0.07 * s, body),
             ball((x + 0.15 * s, y + 0.24 * s, 0.14 * s), 0.07 * s, body)]
    parts += _eyes(x, y + 0.06 * s, 0.32 * s, s, 0.08)
    if not mini:
        for e in (-1, 1):
            parts.append(ball((x + e * 0.15 * s, y - 0.04 * s, 0.3 * s), 0.035 * s, glossy("#FF6B8A"), scale=(1.3, 1, 0.3)))
        parts.append(ball((x, y - 0.05 * s, 0.33 * s), 0.02 * s, plastic("#5A2A1A")))
    return parts


def claudito_a(mini):
    tile("#EA906C", "#9C4A32")
    group(_pet(0, -0.04, 1.05, mini), loc=(0, 0, 0), rot=(-14, 8, 0), scale=1.1 if mini else 1.0)


def claudito_b(mini):
    tile("#EA906C", "#9C4A32")
    parts = _pet(-0.06, 0.08, 0.78, True)
    parts += [cyl((0.18, -0.26, 0.06), 0.17, 0.1, glossy("#2D7BF0"), r2=0.13, bevel=0.03),
              cyl((0.18, -0.26, 0.1), 0.13, 0.03, plastic("#8A5A3C", 0.6))]
    if not mini:
        for dx, dy in ((-0.04, 0.02), (0.05, -0.01), (0.0, -0.05)):
            parts.append(ball((0.18 + dx, -0.26 + dy, 0.13), 0.035, plastic("#B07A4A", 0.6)))
    group(parts, loc=(0, 0, 0), rot=(-14, 8, 0), scale=1.1 if mini else 1.0)


# -- Doom ------------------------------------------------------------------

def doom_a(mini):
    tile("#8A2A1A", "#140404")
    green = glossy("#4E7A3A")
    parts = [ball((0, 0.02, 0.18), 0.32, green, scale=(1, 1.05, 0.85)),
             rbox((0, -0.08, 0.4), (0.46, 0.2, 0.1), 0.05, metal("#E0B050", 0.2)),
             rbox((0, -0.3, 0.12), (0.36, 0.12, 0.12), 0.05, plastic("#3A4A2E", 0.5))]
    if not mini:
        parts.append(ball((-0.12, -0.05, 0.455), 0.035, glow("#FFFFFF", 0.6), scale=(1.6, 1, 0.4)))
        for x in (-0.24, 0.24):
            parts.append(rbox((x, 0.0, 0.26), (0.06, 0.2, 0.08), 0.02, plastic("#3A4A2E", 0.5)))
    group(parts, loc=(0, 0.02, 0.0), rot=(-14, 8, 0), scale=1.0)


def doom_b(mini):
    tile("#8A2A1A", "#140404")
    door = metal("#8E939C", 0.4)
    parts = [rbox((-0.08, 0.04, 0.05), (0.56, 0.7, 0.08), 0.03, door)]
    for i in range(4 if not mini else 2):
        y = 0.24 - i * (0.14 if not mini else 0.3)
        parts.append(rbox((-0.08, y, 0.095), (0.46, 0.05, 0.02), 0.01, plastic("#5A5E66", 0.5)))
    parts.append(rbox((-0.08, 0.04, 0.1), (0.05, 0.6, 0.02), 0.01, glossy("#FF3B30")))
    key = [rbox((0, 0, 0), (0.22, 0.32, 0.03), 0.03, glossy("#FF3B30")),
           rbox((0, 0.08, 0.02), (0.14, 0.06, 0.01), 0.01, plastic("#FFFFFF"))]
    parts.append(group(key, loc=(0.24, -0.2, 0.2), rot=(0, 0, -20), name="key"))
    group(parts, loc=(0, 0, 0.04), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


# -- Duke Nukem 3D ---------------------------------------------------------

def duke3d_a(mini):
    tile("#FFD84A", "#B06A00")
    black = glossy("#16161A")
    parts = []
    for s in (-1, 1):
        lens = rounded_poly([(s * 0.04, 0.1), (s * 0.38, 0.1), (s * 0.34, -0.1), (s * 0.08, -0.12)], 0.05)
        parts.append(slab([lens], 0.07, 0.025, black, z=0.1))
        if not mini:
            parts.append(ball((s * 0.24, 0.04, 0.17), 0.03, glow("#FFFFFF", 0.6), scale=(1.8, 0.8, 0.3)))
    parts += [rbox((0, 0.06, 0.13), (0.16, 0.05, 0.04), 0.02, black),
              capsule((-0.38, 0.08, 0.12), (-0.44, 0.2, 0.08), 0.02, black),
              capsule((0.38, 0.08, 0.12), (0.44, 0.2, 0.08), 0.02, black)]
    group(parts, loc=(0, 0.0, 0.02), rot=(-16, 10, -8), scale=1.05)


def duke3d_b(mini):
    tile("#FFD84A", "#B06A00")
    group([text("3D", glossy("#E33030"), size=0.5, depth=0.14, bevel=0.025, c=(0, 0.02, 0.02))],
          loc=(0, 0, 0.02), rot=(-18, 12, 0), scale=1.0)


# -- Emulador --------------------------------------------------------------

def emulador_a(mini):
    tile("#FF4DA6", "#5B2EFF")
    parts = [rbox((0, -0.12, 0.08), (0.74, 0.42, 0.14), 0.07, plastic("#2C2F36", 0.35)),
             capsule((-0.16, -0.1, 0.16), (-0.16, -0.04, 0.38), 0.03, metal()),
             ball((-0.16, -0.04, 0.4), 0.1, glossy("#FF3B30"))]
    for i, c in enumerate(("#FFD60A", "#2D7BF0") if mini else ("#FFD60A", "#2D7BF0", "#34C759")):
        parts.append(cyl((0.1 + i * 0.12, -0.12 + (0.04 if i == 1 else 0), 0.17), 0.05, 0.04, glossy(c), bevel=0.015))
    group(parts, loc=(0, 0.04, 0.0), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


def emulador_b(mini):
    tile("#FF4DA6", "#5B2EFF")
    grey = plastic("#8E939C", 0.4)
    parts = [rbox((0, 0.0, 0.07), (0.56, 0.68, 0.12), 0.04, grey),
             rbox((0, 0.06, 0.135), (0.42, 0.36, 0.02), 0.03, glossy("#FFD60A")),
             ball((0, 0.06, 0.15), 0.08, glossy("#FF3B30"), scale=(1, 1, 0.3))]
    if not mini:
        for i in range(5):
            parts.append(rbox((-0.16 + i * 0.08, -0.26, 0.13), (0.04, 0.1, 0.02), 0.01, plastic("#6E737C")))
    group(parts, loc=(0, 0.0, 0.04), rot=(-18, 10, 8), scale=1.1 if mini else 1.0)


# -- Flappy ----------------------------------------------------------------

def _bird(x, y, s, mini):
    yel = glossy("#FFD60A")
    parts = [ball((x, y, 0.16 * s), 0.24 * s, yel, scale=(1.1, 1, 0.9)),
             ball((x - 0.12 * s, y - 0.04 * s, 0.3 * s), 0.1 * s, glossy("#FFF2A0"), scale=(1.3, 0.8, 0.5)),
             ball((x + 0.22 * s, y - 0.04 * s, 0.18 * s), 0.08 * s, glossy("#FF7A1A"), scale=(1.4, 0.7, 0.6)),
             ball((x + 0.12 * s, y + 0.1 * s, 0.32 * s), 0.07 * s, plastic("#FFFFFF")),
             ball((x + 0.15 * s, y + 0.11 * s, 0.37 * s), 0.035 * s, plastic("#1C1C1E"))]
    return parts


def flappy_a(mini):
    tile("#5CC8FF", "#1F8FD6")
    group(_bird(0, 0, 1.2, mini), loc=(-0.02, 0.0, 0.0), rot=(-12, 8, 10), scale=1.05)


def flappy_b(mini):
    tile("#5CC8FF", "#1F8FD6")
    pipe = glossy("#34C759")
    parts = [cyl((0.2, -0.2, 0.12), 0.13, 0.5, pipe, rot=(R90, 0, 0)),
             cyl((0.2, 0.06, 0.12), 0.16, 0.1, glossy("#2EB04F"), rot=(R90, 0, 0), bevel=0.02)]
    parts += _bird(-0.16, 0.18, 0.7, True)
    group(parts, loc=(0, 0, 0.0), rot=(-14, 8, 0), scale=1.05 if mini else 1.0)


# -- Gemas -----------------------------------------------------------------

def _gem(x, y, z, r, c, rz=0):
    m = glossy(c)
    crown = cyl((0, 0, 0.06 * r / 0.2), r, 0.12 * r / 0.2, m, r2=r * 0.6, verts=8)
    pav = cyl((0, 0, -0.06 * r / 0.2 - 0.06 * r / 0.2), r, 0.12 * r / 0.2, m, r2=0.01, verts=8)
    pav.rotation_euler.x = math.pi
    for o in (crown, pav):
        o.data.auto_smooth_angle = math.radians(5)
    g = group([crown, pav], loc=(x, y, z), rot=(0, 0, rz), name="gem")
    return g


def gemas_a(mini):
    tile("#7A4CFF", "#2A1A6E")
    g = _gem(0, 0, 0.22, 0.34, "#FF2D45", 22)
    root = group([g], loc=(0, 0.02, 0.0))
    from lib import turn
    turn(root, ("X", -30), ("Y", 10))


def gemas_b(mini):
    tile("#7A4CFF", "#2A1A6E")
    gems = [_gem(-0.2, 0.14, 0.18, 0.18, "#FF2D45", 10), _gem(0.2, 0.12, 0.18, 0.18, "#2E7BFF", -5),
            _gem(0.0, -0.18, 0.2, 0.2, "#34C759", 20)]
    root = group(gems, loc=(0, 0.0, 0.0), scale=1.1 if mini else 1.0)
    from lib import turn
    turn(root, ("X", -28), ("Y", 8))


# -- Golf ------------------------------------------------------------------

def golf_a(mini):
    tile("#4FB86A", "#1E6B34")
    parts = [ball((0, 0.06, 0.24), 0.2, glossy("#FFFFFF")),
             cyl((0, -0.16, 0.06), 0.06, 0.24, glossy("#FF9F0A"), rot=(R90, 0, 0), r2=0.012),
             cyl((0, -0.06, 0.06), 0.09, 0.03, glossy("#FF9F0A"), rot=(R90, 0, 0))]
    if not mini:
        for i in range(10):
            a = i * 0.9
            parts.append(ball((0.11 * math.cos(a), 0.06 + 0.11 * math.sin(a), 0.41), 0.022, matte("#D9DCE3"),
                              scale=(1, 1, 0.3)))
    group(parts, loc=(0, 0.0, 0.0), rot=(-20, 10, 0), scale=1.05)


def golf_b(mini):
    tile("#4FB86A", "#1E6B34")
    parts = [ball((-0.04, -0.24, 0.01), 0.14, matte("#0E2A16"), scale=(1.3, 0.7, 0.1)),
             capsule((-0.04, -0.24, 0.04), (-0.04, 0.38, 0.06), 0.022, plastic("#FFFFFF")),
             slab([rounded_poly([(-0.04, 0.38), (0.32, 0.27), (-0.04, 0.16)], 0.02)], 0.04, 0.012, glossy("#FF3B30"),
                  z=0.04)]
    if not mini:
        parts.append(ball((0.22, -0.3, 0.06), 0.06, glossy("#FFFFFF")))
    group(parts, loc=(0, 0.0, 0.02), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


# -- Hockey de mesa --------------------------------------------------------

def _mallet(x, y, c):
    return [cyl((x, y, 0.06), 0.15, 0.08, glossy(c), bevel=0.03, verts=64),
            cyl((x, y, 0.14), 0.06, 0.1, glossy(c), bevel=0.02),
            ball((x, y, 0.2), 0.07, glossy(c))]


def hockey_a(mini):
    tile("#E84A4E", "#1A1C22")
    parts = _mallet(-0.12, 0.1, "#FF3B30") + [cyl((0.2, -0.2, 0.03), 0.12, 0.05, plastic("#1C1C1E", 0.3), bevel=0.015)]
    if not mini:
        parts.append(slab([circle(0.2, -0.2, 0.09), circle(0.2, -0.2, 0.07)], 0.004, 0.0, glossy("#FFD60A"), z=0.055))
    group(parts, loc=(0, 0, 0.02), rot=(-18, 10, 0), scale=1.15 if mini else 1.05)


def hockey_b(mini):
    tile("#E84A4E", "#1A1C22")
    parts = [rbox((0, 0, 0.04), (0.6, 0.8, 0.07), 0.06, plastic("#F4F5F8", 0.35)),
             rbox((0, 0, 0.078), (0.56, 0.012, 0.004), 0.002, glossy("#FF3B30")),
             slab([circle(0, 0, 0.1), circle(0, 0, 0.088)], 0.004, 0.0, glossy("#FF3B30"), z=0.077)]
    for y in (-0.39, 0.39):
        parts.append(rbox((0, y, 0.06), (0.2, 0.03, 0.04), 0.01, plastic("#1C1C1E")))
    parts += [cyl((0.1, 0.08, 0.09), 0.05, 0.03, plastic("#1C1C1E"), bevel=0.01)]
    for (x, y, c) in ((-0.1, 0.26, "#2D7BF0"), (0.08, -0.26, "#FF3B30")):
        parts += [cyl((x, y, 0.1), 0.08, 0.05, glossy(c), bevel=0.015), ball((x, y, 0.15), 0.04, glossy(c))]
    group(parts, loc=(0, 0, 0.06), rot=(-18, 10, 0), scale=1.0)


# -- Juegos de mesa --------------------------------------------------------

def mesa_a(mini):
    tile("#A0703E", "#2A170A")
    ivory = glossy("#FFF8EA")
    parts = [cyl((-0.1, 0.02, 0.05), 0.17, 0.08, ivory, bevel=0.03),
             cyl((-0.1, 0.02, 0.2), 0.13, 0.24, ivory, r2=0.06),
             ball((-0.1, 0.02, 0.36), 0.1, ivory)]
    from models_juegos1 import _die
    d = _die((0, 0, 0), 0.24, 3, glossy("#E33030"), plastic("#FFFFFF"), 25, mini)
    d.location = (0.22, -0.18, 0.12)
    root = group(parts + [d], loc=(0, 0, 0))
    from lib import turn
    turn(root, ("X", -40), ("Y", 8))
    root.location.y = 0.04


def mesa_b(mini):
    tile("#A0703E", "#2A170A")
    n = 4
    k = 0.72 / n
    parts = []
    for r in range(n):
        for c in range(n):
            dark = (r + c) % 2 == 1
            parts.append(rbox(((c - 1.5) * k, (1.5 - r) * k, 0.03), (k, k, 0.05), 0.005,
                              plastic("#3A2416" if dark else "#F2D8A8", 0.5)))
    for (r, c, col) in ((0, 1, "#E33030"), (1, 0, "#E33030"), (2, 3, "#1C1C1E"), (3, 2, "#1C1C1E")) + (
            ((1, 2, "#E33030"),) if not mini else ()):
        parts.append(cyl(((c - 1.5) * k, (1.5 - r) * k, 0.09), k * 0.38, 0.06, glossy(col), bevel=0.02))
    group(parts, loc=(0, 0, 0.17), rot=(-18, 10, 0), scale=0.95)


# -- Metegol ---------------------------------------------------------------

def _player(x, y, m, skin, s=1.0):
    return [rbox((x, y, 0.14), (0.15 * s, 0.2 * s, 0.11 * s), 0.05 * s, m),
            ball((x, y + 0.06 * s, 0.22), 0.055 * s, skin),
            rbox((x, y - 0.12 * s, 0.08), (0.12 * s, 0.08 * s, 0.06 * s), 0.025 * s, m)]


def metegol_a(mini):
    tile("#41B862", "#127231", stripes=0 if mini else 4)
    chalk = matte("#FFFFFF")
    slab([circle(0, 0, 0.14), circle(0, 0, 0.12)], 0.004, 0.0, chalk, z=0.0)
    for y0, y1 in ((0.14, 0.47), (-0.47, -0.14)):
        slab([[(-0.01, y0), (0.01, y0), (0.01, y1), (-0.01, y1)]], 0.004, 0.0, chalk, z=0.0)
    rodm, skin = metal("#E3E7EC", 0.2), plastic("#FFD2A8", 0.5)
    parts = []
    for x, c in ((-0.24, "#2E63F0"), (0.24, "#F2392C")):
        parts.append(cyl((x, 0, 0.14), 0.022, 0.97, rodm, rot=(R90, 0, 0)))
        for y in ((0.2, -0.22) if not mini else (0.0,)):
            parts += _player(x, y, glossy(c), skin)
    parts.append(ball((0.04, -0.12 if not mini else -0.26, 0.06), 0.06 if not mini else 0.085, glossy("#FFFFFF")))
    group(parts, scale=1.0 if not mini else 1.25)


def metegol_b(mini):
    tile("#41B862", "#127231", stripes=0 if mini else 4)
    parts = [cyl((-0.02, 0, 0.2), 0.035, 0.97, metal("#E3E7EC", 0.2), rot=(R90, 0, 0))]
    parts += _player(-0.02, 0.06, glossy("#F2392C"), plastic("#FFD2A8", 0.5), 2.0)
    for p in parts[1:]:
        p.location.z += 0.06
    parts.append(ball((0.2, -0.3, 0.08), 0.09, glossy("#FFFFFF")))
    group(parts, loc=(0, 0.02, 0.0), rot=(-12, 8, 0), scale=1.0)


MODELS = {
    "demo.chatarra": {"a": chatarra_a, "b": chatarra_b},
    "demo.cjump": {"a": cjump_a, "b": cjump_b},
    "demo.claudito": {"a": claudito_a, "b": claudito_b},
    "demo.doom": {"a": doom_a, "b": doom_b},
    "demo.duke3d": {"a": duke3d_a, "b": duke3d_b},
    "demo.emulador": {"a": emulador_a, "b": emulador_b},
    "demo.flappy": {"a": flappy_a, "b": flappy_b},
    "demo.gemas": {"a": gemas_a, "b": gemas_b},
    "demo.golf": {"a": golf_a, "b": golf_b},
    "demo.hockey": {"a": hockey_a, "b": hockey_b},
    "demo.mesa": {"a": mesa_a, "b": mesa_b},
    "demo.metegol": {"a": metegol_a, "b": metegol_b},
}
