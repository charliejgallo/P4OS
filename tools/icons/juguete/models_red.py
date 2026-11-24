"""'juguete' models: the group red of apps.json."""

import math

from lib import (arc_band, ball, capsule, circle, cyl, glass, glossy, glow, group, matte, metal, plastic,
                 rbox, rounded_poly, rrect, slab, text, tile, tube, waves)

R90 = math.radians(90)


# -- BLE -------------------------------------------------------------------

def ble_a(mini):
    tile("#5A9BFF", "#1E3A8A")
    parts = [slab([rrect(-0.08, -0.06, 0.4, 0.4, 0.12), circle(-0.2, 0.06, 0.04)], 0.1, 0.035,
                  plastic("#FFFFFF", 0.35)),
             cyl((-0.04, -0.1, 0.1), 0.08, 0.03, glossy("#2D7BF0"), bevel=0.012)]
    if not mini:
        parts.append(tube([(-0.2, 0.06, 0.05), (-0.26, 0.2, 0.08), (-0.38, 0.22, 0.05), (-0.36, 0.08, 0.03)],
                          0.018, metal()))
    parts += waves(0.06, 0.06, glossy("#FFFFFF"), 1 if mini else 2, 0.2, 0.09, 0.045, -10, 80, z=0.06)
    group(parts, loc=(0.0, 0.0, 0.0), rot=(-16, 10, 0), scale=1.15 if mini else 1.05)


def ble_b(mini):
    tile("#5A9BFF", "#1E3A8A")
    parts = [rbox((-0.1, -0.1, 0.05), (0.34, 0.34, 0.08), 0.04, plastic("#1C1C1E", 0.35)),
             rbox((-0.1, -0.1, 0.1), (0.22, 0.22, 0.02), 0.02, metal("#C9CED6"))]
    if not mini:
        for s in (-1, 1):
            for i in range(4):
                parts.append(rbox((-0.1 + s * 0.2, -0.22 + i * 0.08, 0.03), (0.06, 0.04, 0.02), 0.008,
                                  metal("#F2C14E")))
    parts += waves(-0.1, -0.1, glossy("#FFFFFF"), 2 if mini else 3, 0.24, 0.1, 0.05, 5, 85, z=0.04)
    group(parts, loc=(0, 0, 0), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


# -- Cámaras ---------------------------------------------------------------

def cameras_a(mini):
    tile("#56657A", "#0F172A")
    white = plastic("#F4F5F8", 0.35)
    parts = [rbox((0.04, 0.06, 0.18), (0.62, 0.3, 0.26), 0.1, white),
             cyl((-0.28, 0.06, 0.18), 0.12, 0.06, plastic("#1C1C1E"), rot=(0, R90, 0), bevel=0.02),
             cyl((-0.3, 0.06, 0.18), 0.08, 0.04, glass("#6FA8FF"), rot=(0, R90, 0)),
             rbox((0.2, -0.16, 0.08), (0.08, 0.2, 0.08), 0.03, white),
             rbox((0.22, -0.3, 0.04), (0.28, 0.12, 0.06), 0.04, white)]
    if not mini:
        parts += [rbox((0.04, 0.22, 0.2), (0.66, 0.04, 0.2), 0.02, plastic("#D9DCE3")),
                  ball((0.12, 0.06, 0.32), 0.025, glow("#FF3B30", 2.5))]
    group(parts, loc=(0.02, 0.0, 0.0), rot=(-28, 0, -24), scale=1.1 if mini else 1.0)


def cameras_b(mini):
    tile("#56657A", "#0F172A")
    parts = [cyl((0, 0, 0.04), 0.36, 0.08, plastic("#F4F5F8", 0.35), bevel=0.03, verts=96),
             ball((0, 0, 0.08), 0.26, glass("#2A3444"), scale=(1, 1, 0.9)),
             ball((0.04, -0.03, 0.16), 0.1, plastic("#1C1C1E", 0.3)),
             ball((0.06, -0.06, 0.25), 0.05, glossy("#3A6FD8"))]
    if not mini:
        parts.append(ball((-0.1, 0.1, 0.28), 0.035, glow("#FFFFFF", 0.8), scale=(1.6, 1, 0.4)))
    group(parts, loc=(0, 0.0, 0.0), rot=(-20, 10, 0), scale=1.1 if mini else 1.0)


# -- Cartero ---------------------------------------------------------------

def cartero_a(mini):
    tile("#7E7CFF", "#2C2A8C")
    nose, tail_l, tail_r, fold = (0.36, 0.28), (-0.36, -0.06), (-0.12, -0.34), (-0.16, -0.12)
    parts = [slab([rounded_poly([nose, tail_l, fold], 0.015)], 0.03, 0.008, plastic("#FFFFFF", 0.35), z=0.1),
             slab([rounded_poly([nose, fold, tail_r], 0.015)], 0.03, 0.008, plastic("#DCDDF5", 0.35), z=0.06),
             slab([rounded_poly([nose, (-0.2, -0.16), (-0.14, -0.2)], 0.01)], 0.05, 0.01, plastic("#B9BAE8"), z=0.02)]
    group(parts, loc=(0.0, 0.0, 0.0), rot=(-20, 14, 0), scale=1.1 if mini else 1.0)


def cartero_b(mini):
    tile("#7E7CFF", "#2C2A8C")
    parts = [rbox((-0.04, -0.06, 0.05), (0.62, 0.42, 0.08), 0.04, plastic("#FFFFFF", 0.4)),
             slab([rounded_poly([(-0.33, 0.13), (0.25, 0.13), (-0.04, -0.08)], 0.03)], 0.03, 0.01,
                  plastic("#E4E5FA", 0.4), z=0.09)]
    arrow = rounded_poly([(0.08, -0.32), (0.26, -0.32), (0.26, -0.4), (0.42, -0.24), (0.26, -0.08), (0.26, -0.16),
                          (0.08, -0.16)], 0.02)
    parts.append(slab([arrow], 0.09, 0.03, glossy("#FF9F0A"), z=0.1))
    group(parts, loc=(0, 0.06, 0.0), rot=(-16, 10, 0), scale=1.08 if mini else 1.0)


# -- Mi casa ---------------------------------------------------------------

def estado_a(mini):
    tile("#50DDA8", "#047857")
    wall = rounded_poly([(-0.36, -0.32), (0.06, -0.32), (0.06, 0.0), (-0.15, 0.2), (-0.36, 0.0)],
                        [0.05, 0.04, 0.03, 0.05, 0.03])
    parts = [slab([wall], 0.14, 0.035, plastic("#FFFFFF", 0.4)),
             slab([rounded_poly([(-0.44, -0.03), (-0.15, 0.27), (0.14, -0.03), (0.08, -0.08), (-0.15, 0.16),
                                 (-0.38, -0.08)], 0.025)], 0.07, 0.025, glossy("#2C2F36"), z=0.1),
             rbox((-0.15, -0.22, 0.15), (0.12, 0.18, 0.03), 0.03, plastic("#8A5A3C"))]
    light = [rbox((0.26, 0.0, 0.12), (0.2, 0.56, 0.16), 0.08, plastic("#2C2F36", 0.35))]
    for i, c in enumerate(("#FF453A", "#FFD60A", "#34C759")):
        on = c == "#34C759"
        light.append(ball((0.26, 0.17 - i * 0.17, 0.2), 0.065, glow(c, 2.5) if on else glossy(c)))
    group(parts + light, loc=(0, 0.02, 0.0), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


def estado_b(mini):
    tile("#50DDA8", "#047857")
    parts = [rbox((0, 0, 0.05), (0.66, 0.54, 0.1), 0.06, plastic("#2C2F36", 0.35))]
    rows = (("#34C759", 0.34), ("#FFD60A", 0.22), ("#34C759", 0.28)) if not mini else (("#34C759", 0.3), ("#FF453A", 0.3))
    for i, (c, w) in enumerate(rows):
        y = 0.17 - i * (0.34 / max(1, len(rows) - 1))
        parts.append(ball((-0.24, y, 0.12), 0.06, glow(c, 2.0)))
        parts.append(rbox((-0.1 + w / 2, y, 0.11), (w, 0.07, 0.03), 0.03, plastic("#5A6170")))
    group(parts, loc=(0, 0, 0.08), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


# -- Impresoras 3D ---------------------------------------------------------

def impresoras_a(mini):
    tile("#FF9547", "#9A3412")
    frame = plastic("#2C2F36", 0.35)
    parts = [rbox((-0.3, 0.0, 0.1), (0.07, 0.7, 0.07), 0.025, frame),
             rbox((0.3, 0.0, 0.1), (0.07, 0.7, 0.07), 0.025, frame),
             rbox((0, 0.3, 0.13), (0.68, 0.07, 0.08), 0.025, frame),
             rbox((0, -0.18, 0.05), (0.52, 0.3, 0.05), 0.02, metal("#C9CED6", 0.3)),
             rbox((0.0, -0.18, 0.12), (0.18, 0.16, 0.1), 0.03, glossy("#FF7A1A")),
             rbox((0.0, 0.08, 0.2), (0.16, 0.12, 0.1), 0.03, plastic("#F4F5F8")),
             cyl((0.0, 0.0, 0.19), 0.03, 0.08, metal("#F2C14E"), rot=(R90, 0, 0), r2=0.01)]
    if not mini:
        parts.append(capsule((-0.3, 0.08, 0.2), (0.3, 0.08, 0.2), 0.015, metal()))
    group(parts, loc=(0, 0.02, 0.0), rot=(-22, 10, 0), scale=1.05 if mini else 1.0)


def impresoras_b(mini):
    tile("#FF9547", "#9A3412")
    side = plastic("#F4F5F8", 0.35)
    parts = [cyl((0, 0, 0.04), 0.36, 0.04, side, bevel=0.012, verts=96),
             cyl((0, 0, 0.14), 0.3, 0.18, glossy("#2DA8FF"), verts=96),
             cyl((0, 0, 0.24), 0.36, 0.04, side, bevel=0.012, verts=96),
             cyl((0, 0, 0.26), 0.1, 0.03, plastic("#2C2F36"), verts=48)]
    if not mini:
        for r in (0.2, 0.26):
            parts.append(slab([circle(0, 0, r + 0.006), circle(0, 0, r)], 0.004, 0.0, plastic("#D9DCE3"), z=0.26))
    parts.append(tube([(0.3, -0.06, 0.14), (0.38, -0.3, 0.1), (0.2, -0.42, 0.04)], 0.022, glossy("#2DA8FF")))
    group(parts, loc=(-0.02, 0.04, 0.0), rot=(-20, 10, 0), scale=1.0 if mini else 0.9)


# -- SSH -------------------------------------------------------------------

def ssh_a(mini):
    tile("#4A5361", "#0D1117")
    gold = metal("#F2C14E", 0.22)
    parts = [slab([circle(-0.2, 0.0, 0.17), circle(-0.2, 0.0, 0.07)], 0.08, 0.03, gold),
             rbox((0.12, 0.0, 0.04), (0.5, 0.08, 0.07), 0.03, gold),
             rbox((0.3, -0.07, 0.04), (0.06, 0.12, 0.06), 0.02, gold)]
    if not mini:
        parts.append(rbox((0.18, -0.06, 0.04), (0.05, 0.1, 0.06), 0.02, gold))
    group(parts, loc=(0, 0, 0.02), rot=(-18, 10, 30), scale=1.15 if mini else 1.05)


def ssh_b(mini):
    tile("#4A5361", "#0D1117")
    parts = [rbox((-0.04, 0.06, 0.05), (0.66, 0.5, 0.1), 0.06, plastic("#2C2F36", 0.35)),
             rbox((-0.04, 0.06, 0.1), (0.56, 0.4, 0.02), 0.04, glossy("#0E1A14")),
             text(">_", glow("#5CFF9A", 1.5), size=0.2, depth=0.015, bevel=0.003, c=(-0.12, 0.08, 0.11))]
    gold = metal("#F2C14E", 0.25)
    lock = [rbox((0.2, -0.2, 0.16), (0.24, 0.2, 0.12), 0.05, gold),
            tube([(0.13, -0.12, 0.16), (0.13, 0.0, 0.16), (0.27, 0.0, 0.16), (0.27, -0.12, 0.16)], 0.025,
                 metal("#D7DBE2"))]
    if not mini:
        lock.append(ball((0.2, -0.2, 0.225), 0.025, plastic("#5A3E10")))
    group(parts + lock, loc=(0, 0.0, 0.0), rot=(-16, 10, 0), scale=1.1 if mini else 1.0)


# -- VNC -------------------------------------------------------------------

def _pointer(x, y, z, s, m):
    pts = [(0, 0), (0, -0.3), (0.07, -0.23), (0.12, -0.33), (0.17, -0.31), (0.12, -0.21), (0.21, -0.21)]
    return slab([rounded_poly([(x + px * s, y + py * s) for px, py in pts], 0.012)], 0.05, 0.015, m, z=z)


def vnc_a(mini):
    tile("#4F86FF", "#1E3A8A")
    parts = [rbox((0, 0.08, 0.06), (0.76, 0.52, 0.1), 0.05, plastic("#2C2F36", 0.35)),
             rbox((0, 0.09, 0.11), (0.66, 0.42, 0.02), 0.03, glossy("#5EC8FF")),
             rbox((0, -0.24, 0.04), (0.1, 0.16, 0.06), 0.02, plastic("#2C2F36")),
             rbox((0, -0.33, 0.03), (0.34, 0.06, 0.05), 0.025, plastic("#2C2F36")),
             _pointer(0.02, 0.2, 0.12, 1.0 if not mini else 1.15, glossy("#FFFFFF"))]
    group(parts, loc=(0, 0.0, 0.0), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


def vnc_b(mini):
    tile("#4F86FF", "#1E3A8A")
    alu = metal("#D7DBE2", 0.3)
    base = [rbox((0, -0.2, 0.04), (0.74, 0.34, 0.05), 0.04, alu)]
    if not mini:
        base.append(rbox((0, -0.18, 0.07), (0.6, 0.18, 0.01), 0.01, plastic("#2C2F36")))
        base.append(rbox((0, -0.33, 0.07), (0.18, 0.05, 0.01), 0.01, plastic("#B0B6C0")))
    screen = [rbox((0, 0.0, 0.0), (0.74, 0.48, 0.04), 0.04, alu),
              rbox((0, 0.0, 0.022), (0.66, 0.4, 0.01), 0.02, glossy("#5EC8FF"))]
    lid = group(screen, loc=(0, 0.17, 0.12), rot=(-25, 0, 0), name="lid")
    group(base + [lid], loc=(0, 0.02, 0.0), rot=(-14, 10, 0), scale=1.08 if mini else 1.0)


# -- Vuelos ----------------------------------------------------------------

def _plane(m_body, m_wing, mini, s=1.0):
    parts = [capsule((0, -0.34 * s, 0.1), (0, 0.32 * s, 0.1), 0.065 * s, m_body),
             slab([rounded_poly([(-0.42 * s, -0.06 * s), (0, 0.12 * s), (0.42 * s, -0.06 * s), (0.4 * s, -0.12 * s),
                                 (0, -0.02 * s), (-0.4 * s, -0.12 * s)], 0.02)], 0.04, 0.012, m_wing, z=0.06),
             slab([rounded_poly([(-0.16 * s, -0.36 * s), (0, -0.26 * s), (0.16 * s, -0.36 * s), (0.15 * s, -0.4 * s),
                                 (0, -0.33 * s), (-0.15 * s, -0.4 * s)], 0.015)], 0.03, 0.01, m_wing, z=0.08)]
    if not mini:
        for x in (-0.2, 0.2):
            parts.append(cyl((x * s, 0.03 * s, 0.05), 0.04 * s, 0.12 * s, plastic("#B0B6C0"), rot=(R90, 0, 0),
                             bevel=0.01))
        parts.append(ball((0, 0.33 * s, 0.13), 0.03 * s, glossy("#1C2A44"), scale=(1.4, 1, 0.5)))
    return parts


def vuelos_a(mini):
    tile("#2C6E9E", "#081826")
    group(_plane(glossy("#FFFFFF"), plastic("#DCE6F2"), mini, 1.0), loc=(0, 0.02, 0.0), rot=(-16, 10, -35),
          scale=1.15 if mini else 1.05)


def vuelos_b(mini):
    tile("#2C6E9E", "#081826")
    parts = [cyl((0, 0, 0.03), 0.37, 0.06, glossy("#0F3D2A"), bevel=0.02, verts=96)]
    for r in ((0.13, 0.25) if not mini else (0.2,)):
        parts.append(slab([circle(0, 0, r + 0.012), circle(0, 0, r)], 0.006, 0.0, glow("#5CFF9A", 0.8), z=0.06))
    sweep = rbox((0.12, 0.12, 0.07), (0.34, 0.025, 0.01), 0.008, glow("#5CFF9A", 1.5))
    sweep.rotation_euler.z = math.radians(45)
    parts.append(sweep)
    plane = group(_plane(glossy("#FFD60A"), glossy("#FFD60A"), True, 1.0),
                  loc=(-0.13, -0.1, 0.05), rot=(0, 0, -30), scale=0.26 if not mini else 0.34, name="plane")
    parts.append(plane)
    group(parts, loc=(0, 0, 0.1), rot=(-16, 10, 0), scale=1.08 if mini else 1.0)


MODELS = {
    "aos.ble": {"a": ble_a, "b": ble_b},
    "aos.cameras": {"a": cameras_a, "b": cameras_b},
    "aos.cartero": {"a": cartero_a, "b": cartero_b},
    "aos.estado": {"a": estado_a, "b": estado_b},
    "aos.impresoras": {"a": impresoras_a, "b": impresoras_b},
    "aos.ssh": {"a": ssh_a, "b": ssh_b},
    "aos.vnc": {"a": vnc_a, "b": vnc_b},
    "aos.vuelos": {"a": vuelos_a, "b": vuelos_b},
}
