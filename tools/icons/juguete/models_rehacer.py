"""'juguete' models: new variants (d, e, f) for the apps the user sent back.

Home Assistant, Modbus, Bastión, Burbujas, Claudito, Claude Jump, Duke Nukem
3D, Doom and Topos are modelled here. Mila and Monster Hop use their own
games' art: their builds here only make the tile, and sprites.py lays the
game's sprite on it (SPRITE marks them for render.py).

Brands: Home Assistant's icon evokes a house with a network in HA's blue, not
its logo; Doom and Duke Nukem 3D get generic things (a corridor, a sight,
keycards), nothing of their characters, logos or art.
"""

import math

from lib import (BOLT, arc_band, ball, blob, capsule, circle, cyl, glass, glossy, glow, group, matte, metal,
                 plastic, rbox, rounded_poly, rrect, slab, star, text, tile, tube, turn)

R90 = math.radians(90)


def _eyes(x, y, z, s, gap=0.08):
    out = []
    for e in (-1, 1):
        out.append(ball((x + e * gap * s, y, z), 0.05 * s, plastic("#FFFFFF"), scale=(1, 1.2, 0.6)))
        out.append(ball((x + e * gap * s + 0.01 * s, y + 0.01 * s, z + 0.025 * s), 0.025 * s, plastic("#1C1C1E")))
    return out


def _house_loop(s=1.0, cx=0.0, cy=0.0):
    return rounded_poly([(cx - 0.3 * s, cy - 0.3 * s), (cx + 0.3 * s, cy - 0.3 * s), (cx + 0.3 * s, cy + 0.06 * s),
                         (cx, cy + 0.33 * s), (cx - 0.3 * s, cy + 0.06 * s)], [0.06, 0.06, 0.04, 0.07, 0.04])


def _net(nodes, links, m_node, m_link, z, r_node, r_link):
    parts = [tube([(*nodes[a], z), (*nodes[b], z)], r_link, m_link, order=2) for a, b in links]
    parts += [ball((x, y, z), r_node, m_node) for x, y in nodes]
    return parts


# -- Home Assistant --------------------------------------------------------

HA_TOP, HA_BOT = "#5CCBFA", "#0B6FB0"


def ha_d(mini):
    """A white house, and inside it a little network in HA's blue."""
    tile(HA_TOP, HA_BOT)
    parts = [slab([_house_loop(1.25)], 0.14, 0.045, glossy("#FFFFFF"))]
    nodes = [(0.0, -0.22), (0.0, 0.02), (-0.15, 0.13), (0.15, 0.1)] if not mini else [(0.0, -0.2), (0.0, 0.05)]
    links = [(0, 1), (1, 2), (1, 3)] if not mini else [(0, 1)]
    parts += _net(nodes, links, glossy("#18BCF2"), glossy("#18BCF2"), 0.16, 0.055 if not mini else 0.075,
                  0.022 if not mini else 0.035)
    group(parts, loc=(0, 0.02, 0.04), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


def ha_e(mini):
    """The house as a thick outline, a glowing network inside it."""
    tile("#1F2A3A", "#0B1622")
    outer, inner = _house_loop(1.3), _house_loop(1.02)
    parts = [slab([outer, inner[::-1]], 0.12, 0.035, glossy("#41BDF5"))]
    nodes = [(-0.12, -0.18), (0.12, -0.18), (0.0, 0.0), (0.0, 0.16)]
    links = [(0, 2), (1, 2), (2, 3)]
    if mini:
        nodes, links = [(0.0, -0.16), (0.0, 0.08)], [(0, 1)]
    parts += _net(nodes, links, glow("#7FE3FF", 1.6), glow("#41BDF5", 1.0), 0.08, 0.05 if not mini else 0.07,
                  0.018 if not mini else 0.03)
    group(parts, loc=(0, 0.02, 0.04), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


def ha_f(mini):
    """A blue house with a round window, sitting on a network of nodes."""
    tile(HA_TOP, HA_BOT)
    parts = [slab([_house_loop(0.95, 0, 0.08)], 0.16, 0.045, glossy("#18BCF2"), z=0.04),
             cyl((0, 0.06, 0.2), 0.07, 0.03, plastic("#FFFFFF"), verts=48),
             rbox((0, -0.12, 0.2), (0.1, 0.12, 0.02), 0.03, plastic("#FFFFFF"))]
    nodes = [(-0.32, -0.32), (0.0, -0.36), (0.32, -0.32)]
    parts += _net(nodes, [(0, 1), (1, 2)], glossy("#FFFFFF"), metal(), 0.06, 0.06 if not mini else 0.075, 0.018)
    parts.append(capsule((0, -0.36, 0.06), (0, -0.2, 0.08), 0.02, metal()))
    group(parts, loc=(0, 0.02, 0.04), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


# -- Modbus ----------------------------------------------------------------

MB_TOP, MB_BOT = "#3FD6A0", "#05664A"


def modbus_d(mini):
    """A factory: a sawtooth roof, a chimney, a signal leaving it."""
    tile(MB_TOP, MB_BOT)
    wall = plastic("#F4F5F8", 0.4)
    teeth = [(-0.34, -0.3), (0.3, -0.3), (0.3, 0.06)]
    for i in range(3):
        x0 = 0.3 - i * 0.213
        teeth += [(x0 - 0.213, 0.06), (x0 - 0.213, 0.2)] if i < 2 else [(x0 - 0.213 + 0.0, 0.06), (-0.34, 0.2)]
    parts = [slab([rounded_poly([(-0.34, -0.3), (0.3, -0.3), (0.3, 0.06), (0.17, 0.2), (0.087, 0.06),
                                 (-0.04, 0.2), (-0.126, 0.06), (-0.25, 0.2), (-0.34, 0.06)], 0.02)], 0.16, 0.04, wall),
             rbox((0.22, 0.22, 0.12), (0.1, 0.3, 0.12), 0.03, plastic("#E33030", 0.4)),
             rbox((-0.04, -0.2, 0.17), (0.14, 0.18, 0.03), 0.02, plastic("#2C3A44"))]
    if not mini:
        for x in (-0.24, 0.14):
            parts.append(rbox((x, -0.06, 0.17), (0.1, 0.08, 0.02), 0.015, glow("#FFE9A6", 0.6)))
        parts += [ball((0.22, 0.42, 0.14), 0.05, plastic("#E9EDF2", 0.5)), ball((0.28, 0.46, 0.14), 0.035, plastic("#E9EDF2", 0.5))]
    group(parts, loc=(0, 0.0, 0.04), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


def modbus_e(mini):
    """A green terminal block: three screws, three wires in."""
    tile(MB_TOP, MB_BOT)
    parts = [rbox((0, 0.04, 0.1), (0.66, 0.34, 0.2), 0.05, glossy("#1F9D55"))]
    cols = ("#FF453A", "#FFD60A", "#2C2C2E")
    for i, c in enumerate(cols):
        x = -0.21 + i * 0.21
        parts += [cyl((x, 0.1, 0.21), 0.07, 0.04, metal("#C9CED6", 0.25), bevel=0.012),
                  rbox((x, 0.1, 0.235), (0.1, 0.022, 0.01), 0.008, plastic("#5A5E66"))]
        if not mini or i != 1:
            parts.append(tube([(x, -0.12, 0.1), (x + 0.02, -0.28, 0.08), (x - 0.04, -0.44, 0.05)], 0.035, glossy(c)))
    group(parts, loc=(0, 0.04, 0.02), rot=(-18, 10, 0), scale=1.05 if mini else 1.0)


def modbus_f(mini):
    """A PLC: a grey box, a display with an address, a row of lights."""
    tile(MB_TOP, MB_BOT)
    parts = [rbox((0, 0, 0.1), (0.62, 0.72, 0.2), 0.06, plastic("#DCE2EA", 0.35)),
             rbox((0, 0.16, 0.2), (0.44, 0.2, 0.03), 0.03, glossy("#0E1A14")),
             text("01", glow("#5CFF9A", 1.5), size=0.16 if not mini else 0.2, depth=0.015, bevel=0.003,
                  c=(0, 0.16, 0.215))]
    n = 4 if not mini else 2
    for i in range(n):
        c = ("#34C759", "#34C759", "#FFD60A", "#FF453A")[i if not mini else i * 2]
        parts.append(ball((-0.18 + i * (0.36 / max(1, n - 1)), -0.08, 0.2), 0.035, glow(c, 2.0)))
    if not mini:
        for i in range(5):
            parts.append(rbox((-0.2 + i * 0.1, -0.26, 0.19), (0.07, 0.07, 0.03), 0.015, plastic("#FF9F0A")))
    group(parts, loc=(0, 0.0, 0.04), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


# -- Bastión ---------------------------------------------------------------

BA_TOP, BA_BOT = "#34609E", "#070A18"


def bastion_d(mini):
    """A shield with a star: holding the line."""
    tile(BA_TOP, BA_BOT)
    shield = rounded_poly([(-0.3, 0.3), (0.3, 0.3), (0.3, 0.0), (0.0, -0.36), (-0.3, 0.0)], [0.05, 0.05, 0.12, 0.05, 0.12])
    inner = rounded_poly([(-0.22, 0.22), (0.22, 0.22), (0.22, 0.0), (0.0, -0.26), (-0.22, 0.0)], [0.04, 0.04, 0.09, 0.04, 0.09])
    parts = [slab([shield], 0.12, 0.04, metal("#D7DBE2", 0.25)),
             slab([inner], 0.04, 0.015, glossy("#2D5BD8"), z=0.12),
             slab([rounded_poly(star(0, 0.02, 0.13, 0.055), 0.012)], 0.04, 0.012, glossy("#FFD60A"), z=0.16)]
    group(parts, loc=(0, 0.02, 0.04), rot=(-16, 10, 0), scale=1.05)


def bastion_e(mini):
    """A tower with battlements, and a flying saucer over it."""
    tile(BA_TOP, BA_BOT)
    stone = plastic("#D9DEE5", 0.4)
    top = 0.12
    pts = [(-0.2, -0.38), (0.2, -0.38), (0.2, top)]
    for i in range(3):                                  # merlons, right to left
        x1 = 0.2 - i * 0.16
        pts += [(x1, top + 0.08), (x1 - 0.08, top + 0.08), (x1 - 0.08, top)]
    pts += [(-0.2, top + 0.08)]
    tower = slab([rounded_poly(pts, 0.012)], 0.16, 0.03, stone)
    parts = [tower,
             rbox((0, -0.3, 0.17), (0.12, 0.16, 0.02), 0.05, plastic("#5A5E66")),
             rbox((0, -0.04, 0.17), (0.07, 0.1, 0.02), 0.03, glow("#5EE7FF", 1.5))]
    ufo = [cyl((0, 0, 0), 0.18, 0.05, metal("#B0B6C0", 0.25), bevel=0.02, verts=64),
           ball((0, 0, 0.03), 0.08, glow("#9CFF5A", 0.8), scale=(1, 1, 0.7))]
    if not mini:
        for i in range(5):
            a = 2 * math.pi * i / 5
            ufo.append(ball((0.15 * math.cos(a), 0.15 * math.sin(a), 0.0), 0.02, glow("#FFD60A", 2.0)))
    parts.append(group(ufo, loc=(0.2, 0.3, 0.26), rot=(14, -10, 0), name="ufo"))
    group(parts, loc=(-0.06, 0.0, 0.02), rot=(-12, 8, 0), scale=1.05 if mini else 1.0)


def bastion_f(mini):
    """A small planet with a ring, and a domed base on it."""
    tile(BA_TOP, BA_BOT)
    parts = [ball((0, -0.04, 0.2), 0.26, glossy("#FF8A4A")),
             slab([circle(0, 0, 0.42), circle(0, 0, 0.33)], 0.02, 0.008, glossy("#FFD9A8"), z=0.0)]
    parts[-1].location = (0, -0.04, 0.2)
    parts[-1].rotation_euler = (math.radians(68), math.radians(-18), 0)
    parts += [ball((0.04, 0.16, 0.38), 0.09, glow("#7FD4FF", 0.6)),
              cyl((0.04, 0.16, 0.36), 0.1, 0.03, metal("#D7DBE2"), verts=48)]
    if not mini:
        parts += [ball((-0.12, -0.16, 0.42), 0.04, matte("#C8642A"), scale=(1, 1, 0.3)),
                  ball((0.14, -0.08, 0.44), 0.03, matte("#C8642A"), scale=(1, 1, 0.3))]
    group(parts, loc=(0, 0.02, 0.0), rot=(-10, 6, 0), scale=1.05)


# -- Burbujas --------------------------------------------------------------

BU_TOP, BU_BOT = "#3A4CA8", "#101430"


def burbujas_d(mini):
    """One big soap bubble and two small ones."""
    tile(BU_TOP, BU_BOT)
    parts = [ball((-0.04, 0.02, 0.28), 0.28, glass("#C8E8FF")),
             ball((-0.14, 0.12, 0.5), 0.06, glow("#FFFFFF", 0.8), scale=(1.6, 1, 0.4)),
             ball((0.1, -0.14, 0.5), 0.03, glow("#FF9AD8", 0.6), scale=(1.6, 1, 0.4))]
    if not mini:
        parts += [ball((0.28, 0.26, 0.1), 0.08, glass("#FFD6F0")), ball((0.28, -0.3, 0.08), 0.06, glass("#D6FFE8"))]
    group(parts, loc=(0, 0.0, 0.0), rot=(-10, 6, 0), scale=1.0)


def burbujas_e(mini):
    """The board: rows of coloured bubbles at the top, an arrow aiming at them."""
    tile(BU_TOP, BU_BOT)
    cols = ("#FF3B30", "#FFD60A", "#34C759", "#5EE7FF", "#BF5AF2")
    parts = []
    r = 0.09 if not mini else 0.12
    rows = ((0.28, 4), (0.12, 3)) if not mini else ((0.24, 3),)
    k = 0
    for y, n in rows:
        for i in range(n):
            x = (i - (n - 1) / 2) * (2 * r + 0.01)
            parts.append(ball((x, y, r), r, glossy(cols[k % 5])))
            k += 1
    arrow = rounded_poly([(-0.035, -0.4), (0.035, -0.4), (0.035, -0.18), (0.09, -0.18), (0, -0.06), (-0.09, -0.18),
                          (-0.035, -0.18)], 0.015)
    a = slab([arrow], 0.05, 0.015, glossy("#FFFFFF"), z=0.03)
    a.rotation_euler.z = math.radians(-14)
    parts += [a, ball((0.0, -0.36, 0.1), r, glossy("#FF3B30"))]
    group(parts, loc=(0, 0.0, 0.02), rot=(-16, 10, 0), scale=1.0)


def burbujas_f(mini):
    """A bubble wand and the bubbles it blew."""
    tile(BU_TOP, BU_BOT)
    parts = [slab([circle(0, 0, 0.13), circle(0, 0, 0.09)], 0.05, 0.018, glossy("#FF6BC2"), z=0.0),
             capsule((0.0, -0.13, 0.025), (0.0, -0.42, 0.025), 0.03, glossy("#FF6BC2"))]
    wand = group(parts, loc=(-0.14, -0.04, 0.06), rot=(0, 0, 30), name="wand")
    bub = [ball((0.12, 0.2, 0.18), 0.15, glass("#C8E8FF")), ball((0.06, 0.29, 0.3), 0.035, glow("#FFFFFF", 0.8), scale=(1.5, 1, 0.4))]
    if not mini:
        bub += [ball((0.3, -0.08, 0.1), 0.07, glass("#FFD6F0")), ball((-0.04, 0.4, 0.08), 0.05, glass("#D6FFE8"))]
    group([wand] + bub, loc=(0, 0, 0.02), rot=(-14, 8, 0), scale=1.05 if mini else 1.0)


# -- Claudito --------------------------------------------------------------

CL_TOP, CL_BOT = "#EA906C", "#9C4A32"


def claudito_d(mini):
    """A pocket pet: an egg-shaped toy with a screen and three buttons."""
    tile(CL_TOP, CL_BOT)
    parts = [ball((0, 0.0, 0.14), 0.34, glossy("#FFF4EC"), scale=(0.9, 1.05, 0.45)),
             rbox((0, 0.06, 0.26), (0.32, 0.26, 0.03), 0.06, glossy("#B8D8A0"))]
    pix = [(-1, 1), (1, 1), (-1, -1), (0, -1), (1, -1)] if not mini else [(-1, 1), (1, 1), (0, -1)]
    for px, py in pix:
        parts.append(rbox((px * 0.05, 0.06 + py * 0.05, 0.285), (0.04, 0.04, 0.015), 0.006, plastic("#2A3A20")))
    for i in range(3 if not mini else 1):
        parts.append(cyl(((i - 1) * 0.1 if not mini else 0, -0.18, 0.26), 0.04, 0.04, glossy("#FF7A45"), bevel=0.012))
    group(parts, loc=(0, 0.02, 0.0), rot=(-14, 8, 0), scale=1.05)


def _critter(x, y, s, c="#FFA06A", mini=False, sleepy=False):
    body = glossy(c)
    parts = [blob([(x, y, 0.16 * s, 0.3 * s), (x, y + 0.08 * s, 0.18 * s, 0.22 * s)], body, scale=(1, 1, 0.85)),
             ball((x - 0.14 * s, y + 0.22 * s, 0.14 * s), 0.07 * s, body),
             ball((x + 0.14 * s, y + 0.22 * s, 0.14 * s), 0.07 * s, body)]
    if sleepy:
        for e in (-1, 1):
            parts.append(capsule((x + e * 0.11 * s, y + 0.06 * s, 0.31 * s), (x + e * 0.05 * s, y + 0.06 * s, 0.31 * s),
                                 0.014 * s, plastic("#5A2A1A")))
    else:
        parts += _eyes(x, y + 0.06 * s, 0.31 * s, s, 0.08)
    return parts


def claudito_e(mini):
    """The pet asleep in its little bed."""
    tile(CL_TOP, CL_BOT)
    parts = [rbox((0, -0.12, 0.07), (0.72, 0.46, 0.14), 0.06, glossy("#5A7AE8")),
             rbox((0, -0.1, 0.15), (0.64, 0.38, 0.05), 0.04, plastic("#F4E6FF", 0.5)),
             rbox((0, 0.16, 0.14), (0.72, 0.1, 0.26), 0.05, glossy("#5A7AE8"))]
    pet = _critter(0, 0, 0.62, mini=mini, sleepy=True)
    parts.append(group(pet, loc=(0.0, 0.0, 0.1), name="pet"))
    parts.append(rbox((0, -0.24, 0.2), (0.66, 0.2, 0.06), 0.05, glossy("#FF8FB0")))
    if not mini:
        for x, y, s in ((0.24, 0.3, 0.1), (0.32, 0.4, 0.08)):
            parts.append(text("z", plastic("#FFFFFF"), size=s, depth=0.02, bevel=0.004, c=(x, y, 0.3)))
    group(parts, loc=(0, 0.04, 0.04), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


def _heart(cx, cy, s):
    pts = []
    for i in range(40):
        t = 2 * math.pi * i / 40
        x = 16 * math.sin(t) ** 3
        y = 13 * math.cos(t) - 5 * math.cos(2 * t) - 2 * math.cos(3 * t) - math.cos(4 * t)
        pts.append((cx + x * s / 17, cy + y * s / 17))
    return pts


def claudito_f(mini):
    """The pet hugging a big heart."""
    tile(CL_TOP, CL_BOT)
    parts = _critter(0, 0.08, 0.8, mini=mini)
    parts.append(slab([_heart(0.0, -0.2, 0.2)], 0.1, 0.035, glossy("#FF4D7A"), z=0.16))
    group(parts, loc=(0, 0.0, 0.02), rot=(-14, 8, 0), scale=1.05 if mini else 1.0)


# -- Claude Jump -----------------------------------------------------------

CJ_TOP, CJ_BOT = "#5AB0FF", "#2E9E52"


def cjump_d(mini):
    """Platforms climbing up in a zigzag, a star at the top."""
    tile(CJ_TOP, CJ_BOT)
    parts = []
    steps = ((-0.2, -0.32), (0.16, -0.14), (-0.14, 0.06)) if not mini else ((-0.16, -0.26), (0.12, -0.02))
    for i, (x, y) in enumerate(steps):
        parts.append(rbox((x, y, 0.06 + i * 0.03), (0.3, 0.08, 0.08), 0.04, glossy("#34C759")))
    parts.append(slab([rounded_poly(star(0.14, 0.26, 0.13, 0.06), 0.015)], 0.07, 0.02, glossy("#FFD60A"), z=0.14))
    group(parts, loc=(0, 0.0, 0.02), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


def cjump_e(mini):
    """The little jumper in the air, arms up, over a platform."""
    tile(CJ_TOP, CJ_BOT)
    c = glossy("#FF8A4A")
    parts = [ball((0, 0.08, 0.26), 0.17, c, scale=(1, 1.1, 0.95))]
    parts += _eyes(0, 0.12, 0.4, 1.0, 0.06)
    for e in (-1, 1):
        parts.append(capsule((e * 0.13, 0.16, 0.26), (e * 0.24, 0.36, 0.24), 0.035, c))
        parts.append(capsule((e * 0.06, -0.06, 0.2), (e * 0.1, -0.16, 0.16), 0.035, c))
    parts.append(rbox((0, -0.34, 0.06), (0.44, 0.09, 0.09), 0.045, glossy("#34C759")))
    if not mini:
        for x in (-0.24, 0.24):
            parts.append(capsule((x, -0.2, 0.08), (x, -0.08, 0.08), 0.012, glow("#FFFFFF", 0.6)))
    group(parts, loc=(0, 0.0, 0.0), rot=(-14, 8, 0), scale=1.05)


def cjump_f(mini):
    """A fat arrow pointing up, made of three platforms."""
    tile(CJ_TOP, CJ_BOT)
    parts = []
    for i, w in enumerate((0.62, 0.42, 0.22)):
        parts.append(rbox((0, -0.22 + i * 0.2, 0.06 + i * 0.04), (w, 0.12, 0.1), 0.05,
                          glossy(("#34C759", "#7AD94A", "#FFD60A")[i])))
    group(parts, loc=(0, 0.02, 0.02), rot=(-16, 10, 0), scale=1.05)


# -- Duke Nukem 3D (generic) -----------------------------------------------

DK_TOP, DK_BOT = "#FFD84A", "#B06A00"


def _sight(c_ring, c_dot, mini):
    parts = [slab([circle(0, 0, 0.32), circle(0, 0, 0.26)], 0.08, 0.025, glossy(c_ring))]
    for a in (0, 90, 180, 270):
        r = math.radians(a)
        b = rbox((0.25 * math.cos(r), 0.25 * math.sin(r), 0.04), (0.18, 0.05, 0.08), 0.02, glossy(c_ring))
        b.rotation_euler.z = r
        parts.append(b)
    parts.append(ball((0, 0, 0.06), 0.06 if not mini else 0.08, glow(c_dot, 1.5)))
    return parts


def duke3d_d(mini):
    """A sight."""
    tile(DK_TOP, DK_BOT)
    group(_sight("#E33030", "#E33030", mini), loc=(0, 0, 0.04), rot=(-16, 10, 0), scale=1.05)


def _keycard(x, y, rz, c, z):
    parts = [rbox((0, 0, 0), (0.24, 0.36, 0.03), 0.035, glossy(c)),
             rbox((0, 0.1, 0.017), (0.16, 0.05, 0.01), 0.01, plastic("#FFFFFF")),
             rbox((0, -0.07, 0.017), (0.1, 0.1, 0.01), 0.012, metal("#F2C14E", 0.25))]
    return group(parts, loc=(x, y, z), rot=(0, 0, rz), name="key")


def duke3d_e(mini):
    """Three keycards, red, blue and yellow, fanned out."""
    tile(DK_TOP, DK_BOT)
    cards = [_keycard(-0.14, -0.02, 18, "#2D5BD8", 0.04), _keycard(0.0, 0.02, 0, "#E33030", 0.08),
             _keycard(0.14, -0.02, -18, "#2C2C2E", 0.12)]
    if mini:
        cards = [_keycard(-0.1, 0, 12, "#2D5BD8", 0.04), _keycard(0.1, 0, -12, "#E33030", 0.08)]
    group(cards, loc=(0, 0, 0.02), rot=(-16, 10, 0), scale=1.15 if mini else 1.05)


def _corridor(wall, light, mini, n=5):
    parts = []
    for i in range(n if not mini else 3):
        s = 1.0 - i * (0.16 if not mini else 0.25)
        outer = rrect(0, 0.02, 0.72 * s, 0.72 * s, 0.05 * s)
        inner = rrect(0, 0.02, 0.6 * s, 0.6 * s, 0.04 * s)
        parts.append(slab([outer, inner[::-1]], 0.04, 0.012, plastic(wall if i % 2 == 0 else "#5A5E66", 0.45),
                          z=-0.12 * i + 0.3))
    parts.append(rbox((0, 0.02, -0.3 + 0.3 + 0.02 - 0.12 * (n - 1 if not mini else 2)), (0.24, 0.28, 0.02), 0.03,
                      glow(light, 2.0)))
    return parts


def duke3d_f(mini):
    """A corridor going away, a light at its end."""
    tile(DK_TOP, DK_BOT)
    group(_corridor("#3A3F4A", "#FFE07A", mini), loc=(0, 0, 0), rot=(-8, 6, 0), scale=0.98)


# -- Doom (generic) --------------------------------------------------------

DO_TOP, DO_BOT = "#8A2A1A", "#140404"


def doom_d(mini):
    """A dark corridor, a red glow at its end."""
    tile(DO_TOP, DO_BOT)
    group(_corridor("#4A3A34", "#FF4A2A", mini), loc=(0, 0, 0), rot=(-8, 6, 0), scale=0.98)


def doom_e(mini):
    """Three red shells with brass bases."""
    tile(DO_TOP, DO_BOT)
    parts = []
    xs = (-0.18, 0.0, 0.18) if not mini else (-0.12, 0.12)
    for x in xs:
        parts += [cyl((x, 0.06, 0.1), 0.08, 0.42, glossy("#D9261C"), rot=(R90, 0, 0), bevel=0.02),
                  cyl((x, -0.19, 0.1), 0.085, 0.1, metal("#E0B050", 0.25), rot=(R90, 0, 0), bevel=0.012)]
    group(parts, loc=(0, 0.04, 0.0), rot=(-18, 10, -14), scale=1.08 if mini else 1.0)


def doom_f(mini):
    """A first-aid box: white, a red cross."""
    tile(DO_TOP, DO_BOT)
    parts = [rbox((0, -0.04, 0.12), (0.64, 0.5, 0.22), 0.08, glossy("#F4F5F8")),
             rbox((0, 0.24, 0.12), (0.26, 0.08, 0.12), 0.04, plastic("#B0B6C0"))]
    for rz in (0, 90):
        b = rbox((0, -0.04, 0.24), (0.3, 0.1, 0.04), 0.03, glossy("#E33030"))
        b.rotation_euler.z = math.radians(rz)
        parts.append(b)
    group(parts, loc=(0, 0.0, 0.0), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


# -- Topos -----------------------------------------------------------------

TO_TOP, TO_BOT = "#5AAE3E", "#6B4020"


def _mole(x, y, s, mini, helmet=False, dizzy=False):
    fur = glossy("#8A5A3C")
    parts = [ball((x, y - 0.16 * s, 0.01), 0.3 * s, matte("#3A2414"), scale=(1, 0.5, 0.1)),
             blob([(x, y, 0.12 * s, 0.24 * s), (x, y + 0.1 * s, 0.14 * s, 0.2 * s)], fur, scale=(1, 1, 0.9)),
             ball((x, y + 0.04 * s, 0.3 * s), 0.05 * s, glossy("#FF8FB0"))]
    for e in (-1, 1):
        if dizzy:
            parts.append(slab([rounded_poly(star(x + e * 0.08 * s, y + 0.12 * s, 0.035 * s, 0.015 * s), 0.004)],
                              0.01, 0.003, plastic("#1C1C1E"), z=0.3 * s))
        else:
            parts.append(ball((x + e * 0.08 * s, y + 0.12 * s, 0.3 * s), 0.03 * s, plastic("#1C1C1E")))
    if helmet:
        parts += [ball((x, y + 0.24 * s, 0.2 * s), 0.17 * s, glossy("#FFC62E"), scale=(1.1, 0.7, 0.8)),
                  rbox((x, y + 0.17 * s, 0.24 * s), (0.4 * s, 0.05 * s, 0.04 * s), 0.02 * s, glossy("#FFC62E"))]
    return parts


def topos_d(mini):
    """Three holes in the grass, a mole up in one of them."""
    tile(TO_TOP, TO_BOT)
    parts = []
    for x, y in ((-0.24, 0.2), (0.24, 0.2)) if not mini else ():
        parts.append(ball((x, y, 0.01), 0.14, matte("#3A2414"), scale=(1, 0.55, 0.1)))
    parts += _mole(0.0, -0.12, 0.8, True)
    group(parts, loc=(0, 0.02, 0.0), rot=(-14, 8, 0), scale=1.15 if mini else 1.0)


def topos_e(mini):
    """The mole with the hard hat (the one that takes two hits)."""
    tile(TO_TOP, TO_BOT)
    group(_mole(0, -0.02, 1.1, mini, helmet=True), loc=(0, 0, 0), rot=(-14, 8, 0), scale=1.05)


def topos_f(mini):
    """A dizzy mole, stars turning over its head."""
    tile(TO_TOP, TO_BOT)
    parts = _mole(0, -0.1, 1.0, mini, dizzy=True)
    n = 3 if not mini else 2
    for i in range(n):
        a = math.radians(90 + (i - (n - 1) / 2) * 50)
        parts.append(slab([rounded_poly(star(0.2 * math.cos(a), 0.2 + 0.08 * math.sin(a), 0.06, 0.026), 0.008)],
                          0.04, 0.012, glossy("#FFD60A"), z=0.34))
    group(parts, loc=(0, 0, 0), rot=(-14, 8, 0), scale=1.05)


# -- Mila and Monster Hop: the tile only (sprites.py adds the game's art) --

def mila_tile(mini):
    tile("#F58AB0", "#7A3A6A")


def monsterhop_tile(mini):
    tile("#B07AFF", "#3A1A7A")


def monsterhop_tile_night(mini):
    tile("#3A4A9A", "#0E1230")


for f in (mila_tile, monsterhop_tile, monsterhop_tile_night):
    f.SPRITE = True


MODELS = {
    "aos.ha": {"d": ha_d, "e": ha_e, "f": ha_f},
    "aos.modbus": {"d": modbus_d, "e": modbus_e, "f": modbus_f},
    "demo.bastion": {"d": bastion_d, "e": bastion_e, "f": bastion_f},
    "demo.burbujas": {"d": burbujas_d, "e": burbujas_e, "f": burbujas_f},
    "demo.claudito": {"d": claudito_d, "e": claudito_e, "f": claudito_f},
    "demo.cjump": {"d": cjump_d, "e": cjump_e, "f": cjump_f},
    "demo.duke3d": {"d": duke3d_d, "e": duke3d_e, "f": duke3d_f},
    "demo.doom": {"d": doom_d, "e": doom_e, "f": doom_f},
    "demo.topos": {"d": topos_d, "e": topos_e, "f": topos_f},
    "demo.mila": {"d": mila_tile, "e": mila_tile, "f": mila_tile},
    "demo.monsterhop": {"d": monsterhop_tile, "e": monsterhop_tile_night, "f": monsterhop_tile},
}
