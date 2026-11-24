"""'juguete' models: the second round for the apps still sent back (g, h, i).

Doom: the app's own icon (DOOM_ICON in apps/doom/main/doom.c, a round red
monster with one green eye, two horns and teeth) made a toy - that icon, not
the game's art. Claudito: a soft orange starburst with a face, which evokes
Claude's mark without copying it (eight even, rounded rays). Duke Nukem 3D:
the radiation sign, black and yellow on orange. Topos: three new ideas.
"""

import math

from lib import (arc_band, ball, blob, capsule, circle, cyl, glossy, glow, group, matte, metal, plastic, rbox,
                 rounded_poly, slab, star, text, tile, tube)

R90 = math.radians(90)


# -- Doom: the app's own monster -------------------------------------------

DO_TOP, DO_BOT = "#8A2A1A", "#140404"


def _monster(mini, angry=False):
    red = glossy("#D42A1E")
    horn = glossy("#EFE4C8")
    parts = [ball((0, -0.02, 0.2), 0.3, red, scale=(1, 0.95, 0.9))]
    for e in (-1, 1):
        parts.append(cyl((e * 0.17, 0.27, 0.3), 0.07, 0.2, horn, r2=0.012,
                         rot=(math.radians(-60), e * math.radians(28), 0)))
    # one big eye: white, a green iris, a dark pupil
    parts += [ball((0, 0.05, 0.4), 0.12, glossy("#F6F1DC"), scale=(1, 0.9, 0.6)),
              ball((0, 0.04, 0.46), 0.06, glow("#3CD050", 0.6), scale=(1, 1, 0.5)),
              ball((0, 0.04, 0.49), 0.028, plastic("#101010"))]
    if angry and not mini:
        brow = rbox((0, 0.17, 0.44), (0.26, 0.05, 0.05), 0.02, glossy("#8A1810"))
        parts.append(brow)
    # the mouth and its teeth
    mouth = rbox((0, -0.2, 0.37), (0.3 if not angry else 0.36, 0.1 if not angry else 0.14, 0.06), 0.04,
                 plastic("#2A0404", 0.5))
    parts.append(mouth)
    n = 4 if not mini else 2
    for i in range(n):
        x = (i - (n - 1) / 2) * (0.26 / n)
        parts.append(cyl((x, -0.16, 0.405), 0.022, 0.05, glossy("#F6F1DC"), rot=(R90, 0, 0), r2=0.004))
        if angry:
            parts.append(cyl((x + 0.03, -0.26, 0.405), 0.02, 0.045, glossy("#F6F1DC"), rot=(-R90, 0, 0), r2=0.004))
    return parts


def doom_g(mini):
    """The app's icon as a toy: round, red, one green eye, horns, teeth."""
    tile(DO_TOP, DO_BOT)
    group(_monster(mini), loc=(0, 0.0, 0.0), rot=(-12, 8, 0), scale=1.05)


def doom_h(mini):
    """The same monster, cross: a brow and a wider mouth."""
    tile("#3A0A06", "#0A0202")
    group(_monster(mini, angry=True), loc=(0, 0.0, 0.0), rot=(-12, -8, 0), scale=1.05)


# -- Claudito: a starburst with a face --------------------------------------

def _burst(n=8, r_out=0.36, r_in=0.2, round_=0.07):
    pts = []
    for i in range(2 * n):
        r = r_out if i % 2 == 0 else r_in
        a = math.radians(90 + i * 180 / n)
        pts.append((r * math.cos(a), r * math.sin(a)))
    return rounded_poly(pts, [round_ if i % 2 == 0 else round_ * 0.6 for i in range(2 * n)], seg=8)


def _face(z, s, m_eye, happy=True, cheeks=True, mini=False):
    parts = []
    for e in (-1, 1):
        parts.append(ball((e * 0.07 * s, 0.03 * s, z), 0.035 * s, m_eye, scale=(0.8, 1.2, 0.5)))
        if cheeks and not mini:
            parts.append(ball((e * 0.13 * s, -0.05 * s, z - 0.005), 0.035 * s, glossy("#FF8FA0"), scale=(1.3, 1, 0.3)))
    if happy:
        pts = [(0.05 * s * math.cos(a), -0.05 * s + 0.04 * s * math.sin(a), z) for a in
               [math.radians(d) for d in (200, 240, 270, 300, 340)]]
        parts.append(tube(pts, 0.012 * s, m_eye))
    return parts


def claudito_g(mini):
    """A soft orange starburst with a face, on cream."""
    tile("#FFF1E4", "#F0C8A8")
    parts = [slab([_burst(8, 0.38, 0.25, 0.06)], 0.12, 0.025, glossy("#D97757"))]
    parts += _face(0.13, 2.1, plastic("#3A1A10"), mini=mini)
    group(parts, loc=(0, 0.0, 0.04), rot=(-14, 10, 0), scale=1.08 if mini else 1.0)


def claudito_h(mini):
    """A cream starburst with a face, on Claudito's orange."""
    tile("#EA906C", "#9C4A32")
    parts = [slab([_burst(8, 0.38, 0.25, 0.06)], 0.12, 0.025, glossy("#FFF1E4"))]
    parts += _face(0.13, 2.1, plastic("#7A3A20"), mini=mini)
    group(parts, loc=(0, 0.0, 0.04), rot=(-14, 10, 0), scale=1.08 if mini else 1.0)


def claudito_i(mini):
    """The starburst as a little creature: feet, and one ray waving."""
    tile("#FFF1E4", "#F0C8A8")
    body = slab([_burst(8, 0.3, 0.18, 0.06)], 0.14, 0.05, glossy("#D97757"))
    body.location.y = 0.06
    body.rotation_euler.z = math.radians(8)
    parts = [body]
    parts += [p for p in _face(0.15, 1.35, plastic("#3A1A10"), mini=mini)]
    for p in parts[1:]:
        p.location.y += 0.06
    for e in (-1, 1):
        parts.append(ball((e * 0.1, -0.3, 0.05), 0.07, glossy("#B85A3C"), scale=(1.3, 1, 0.7)))
    group(parts, loc=(0, 0.0, 0.04), rot=(-14, 10, 0), scale=1.05 if mini else 1.0)


# -- Duke Nukem 3D: the radiation sign --------------------------------------

def duke3d_g(mini):
    """The radiation sign, black and yellow, on orange."""
    tile("#FF9A2E", "#C24A08")
    black = glossy("#16161A")
    parts = [cyl((0, 0, 0.04), 0.38, 0.08, glossy("#FFD60A"), bevel=0.03, verts=96),
             slab([circle(0, 0, 0.38), circle(0, 0, 0.34)], 0.09, 0.02, black, z=0.0)]
    for c in (90, 210, 330):
        parts.append(slab([arc_band(0, 0, 0.1, 0.31, c - 30, c + 30, 20)], 0.04, 0.012, black, z=0.07))
    parts.append(cyl((0, 0, 0.09), 0.065, 0.04, black, bevel=0.012, verts=48))
    group(parts, loc=(0, 0, 0.04), rot=(-16, 10, 0), scale=1.0)


# -- Topos: a third round ---------------------------------------------------

TO_TOP, TO_BOT = "#7ACB4A", "#3A7A2A"


def topos_g(mini):
    """A close-up: the mole's face filling the tile, peeking over the dirt."""
    tile("#8FD6FF", "#3A9AE0")
    fur = glossy("#8A5A3C")
    parts = [ball((0, 0.02, 0.1), 0.34, fur, scale=(1, 0.95, 0.7)),
             ball((0, -0.06, 0.32), 0.09, glossy("#FF8FB0"), scale=(1.2, 0.9, 0.6)),
             ball((0, 0.0, 0.3), 0.14, plastic("#C89A72", 0.5), scale=(1.2, 0.8, 0.5))]
    for e in (-1, 1):
        parts.append(ball((e * 0.13, 0.12, 0.32), 0.04, plastic("#1C1C1E")))
        parts.append(ball((e * 0.12, 0.14, 0.355), 0.012, glow("#FFFFFF", 1.0)))
    parts.append(rbox((0, -0.13, 0.33), (0.08, 0.05, 0.03), 0.012, plastic("#FFFFFF")))
    dirt = rbox((0, -0.36, 0.14), (0.86, 0.28, 0.24), 0.1, plastic("#6B4020", 0.6))
    parts.append(dirt)
    for e in (-1, 1):
        for k in range(3 if not mini else 2):
            parts.append(capsule((e * (0.16 + k * 0.04), -0.2, 0.27), (e * (0.17 + k * 0.045), -0.27, 0.28), 0.02,
                                 plastic("#F2E7D8")))
    group(parts, loc=(0, 0.04, 0.0), rot=(-10, 6, 0), scale=1.0)


def _mole_small(x, y, mini=False):
    fur = glossy("#8A5A3C")
    parts = [blob([(x, y, 0.08, 0.13), (x, y + 0.05, 0.1, 0.1)], fur, scale=(1, 1, 0.9)),
             ball((x, y + 0.02, 0.18), 0.035, glossy("#FF8FB0"))]
    if not mini:
        for e in (-1, 1):
            parts.append(ball((x + e * 0.045, y + 0.07, 0.18), 0.018, plastic("#1C1C1E")))
    return parts


def topos_h(mini):
    """The game board: a grid of holes, two moles up."""
    tile(TO_TOP, TO_BOT)
    parts = [rbox((0, 0, 0.04), (0.8, 0.8, 0.08), 0.08, plastic("#C8874A", 0.45))]
    n = 3 if not mini else 2
    k = 0.66 / n
    up = {(0, 1), (2, 2)} if not mini else {(0, 1)}
    for r in range(n):
        for c in range(n):
            x, y = (c - (n - 1) / 2) * k, ((n - 1) / 2 - r) * k
            parts.append(cyl((x, y, 0.08), k * 0.32, 0.02, matte("#2A1608"), verts=48))
            if (r, c) in up:
                parts += _mole_small(x, y, mini)
    group(parts, loc=(0, 0, 0.06), rot=(-16, 10, 0), scale=1.0)


def topos_i(mini):
    """A big wooden mallet, a 'bonk' star where it lands."""
    tile(TO_TOP, TO_BOT)
    head = [cyl((0, 0, 0), 0.13, 0.36, glossy("#FF3B30"), rot=(0, R90, 0), bevel=0.05),
            cyl((-0.19, 0, 0), 0.13, 0.03, plastic("#FFD60A"), rot=(0, R90, 0)),
            cyl((0.19, 0, 0), 0.13, 0.03, plastic("#FFD60A"), rot=(0, R90, 0)),
            capsule((0, -0.06, 0), (0, -0.5, 0), 0.035, plastic("#C8874A", 0.45))]
    parts = [group(head, loc=(0.06, 0.16, 0.24), rot=(0, 0, -35), name="mallet")]
    parts.append(slab([rounded_poly(star(-0.2, -0.22, 0.17, 0.08, 8), 0.02)], 0.06, 0.02, glossy("#FFD60A"), z=0.04))
    group(parts, loc=(0, 0, 0.02), rot=(-14, 8, 0), scale=1.05)


MODELS = {
    "demo.doom": {"g": doom_g, "h": doom_h},
    "demo.claudito": {"g": claudito_g, "h": claudito_h, "i": claudito_i},
    "demo.duke3d": {"g": duke3d_g},
    "demo.topos": {"g": topos_g, "h": topos_h, "i": topos_i},
}
