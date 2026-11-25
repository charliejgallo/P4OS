"""'juguete' models: the group taller of apps.json."""

import math

from lib import (BOLT, ball, capsule, circle, cyl, glossy, glow, group, matte, metal, plastic, rbox,
                 rounded_poly, slab, text, tile, tube, waves)

R90 = math.radians(90)


def _knob(x, y, z, r, m, mark="#FFFFFF"):
    return [cyl((x, y, z), r, r * 0.8, m, bevel=r * 0.25, verts=48),
            rbox((x, y + r * 0.5, z + r * 0.42), (r * 0.22, r * 0.6, 0.01), 0.005, plastic(mark))]


# -- CAN -------------------------------------------------------------------

def can_a(mini):
    tile("#2CC2B2", "#0B4A44")
    parts = []
    n = 10
    for k, c in ((0, "#FF9F0A"), (math.pi, "#34C759")):
        pts = [(-0.3 + 0.6 * i / n, 0.05 * math.sin(i * 1.3 + k), 0.1 + 0.03 * math.cos(i * 1.3 + k))
               for i in range(n + 1)]
        parts.append(tube(pts, 0.035 if not mini else 0.05, glossy(c)))
    for s in (-1, 1):
        parts.append(rbox((s * 0.33, 0, 0.1), (0.14, 0.26, 0.16), 0.05, plastic("#FFFFFF", 0.35)))
        if not mini:
            parts.append(rbox((s * 0.42, 0, 0.1), (0.04, 0.16, 0.1), 0.015, metal("#F2C14E")))
    group(parts, loc=(0, 0, 0), rot=(-16, 10, 25), scale=1.05 if mini else 1.0)


def can_b(mini):
    tile("#2CC2B2", "#0B4A44")
    red = glossy("#FF453A")
    parts = [rbox((0, 0.05, 0.1), (0.64, 0.32, 0.14), 0.07, red),
             rbox((-0.04, 0.05, 0.2), (0.34, 0.26, 0.1), 0.06, glossy("#9FD3FF"))]
    for x in (-0.2, 0.2):
        for y in (-0.12, 0.22):
            parts.append(cyl((x, y, 0.06), 0.07, 0.08, plastic("#1C1C1E", 0.5), rot=(R90, 0, 0), bevel=0.02))
    if not mini:
        parts.append(tube([(0.32, 0.0, 0.1), (0.38, -0.2, 0.06), (0.15, -0.3, 0.04), (-0.15, -0.34, 0.04)],
                          0.025, plastic("#FFD60A")))
        parts.append(rbox((-0.2, -0.34, 0.05), (0.1, 0.08, 0.06), 0.02, plastic("#FFFFFF")))
    group(parts, loc=(0, 0.04, 0.0), rot=(-20, 10, 0), scale=1.15 if mini else 1.0)


# -- Datalog ---------------------------------------------------------------

def _chart_line(xs, ys, z, r, m):
    return tube([(x, y, z) for x, y in zip(xs, ys)], r, m, order=3)


def datalog_a(mini):
    tile("#4FE3CF", "#0F766E")
    parts = [rbox((0, 0.02, 0.04), (0.78, 0.6, 0.08), 0.05, plastic("#7A5A3C", 0.45)),
             rbox((0, 0.02, 0.07), (0.68, 0.5, 0.04), 0.03, plastic("#FFFFFF", 0.5))]
    xs = [-0.27, -0.15, -0.04, 0.07, 0.17, 0.27]
    ys = [-0.12, 0.02, -0.04, 0.12, 0.05, 0.16]
    parts.append(_chart_line(xs, ys, 0.1, 0.022 if not mini else 0.035, glossy("#0FB5A0")))
    if not mini:
        for x, y in zip(xs, ys):
            parts.append(ball((x, y, 0.1), 0.03, glossy("#FF9F0A")))
    group(parts, loc=(0, 0.0, 0.0), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


def datalog_b(mini):
    tile("#4FE3CF", "#0F766E")
    parts = [rbox((-0.04, 0.0, 0.04), (0.6, 0.72, 0.07), 0.04, plastic("#FFFFFF", 0.45)),
             rbox((-0.04, 0.31, 0.08), (0.62, 0.1, 0.06), 0.03, glossy("#FF9F0A"))]
    xs = [-0.26, -0.14, -0.02, 0.1, 0.2]
    parts.append(_chart_line(xs, [-0.2, -0.05, -0.12, 0.08, 0.12], 0.09, 0.025 if not mini else 0.04,
                             glossy("#FF453A")))
    clock = [cyl((0.24, -0.24, 0.1), 0.14, 0.08, glossy("#FFD60A"), bevel=0.03),
             cyl((0.24, -0.24, 0.14), 0.11, 0.02, plastic("#FFFFFF"))]
    if not mini:
        clock += [rbox((0.24, -0.2, 0.16), (0.02, 0.08, 0.01), 0.005, plastic("#1C1C1E")),
                  rbox((0.27, -0.24, 0.16), (0.06, 0.02, 0.01), 0.005, plastic("#1C1C1E"))]
    group(parts + clock, loc=(0, 0.0, 0.0), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


# -- DMX -------------------------------------------------------------------

def dmx_a(mini):
    tile("#9B6BFF", "#3B0764")
    dark = plastic("#2C2C2E", 0.35)
    parts = [cyl((0, 0.02, 0.16), 0.2, 0.4, dark, rot=(R90, 0, 0), bevel=0.05, verts=64),
             cyl((0, -0.195, 0.16), 0.17, 0.03, glow("#FFE9A6", 2.5), rot=(R90, 0, 0), verts=64),
             capsule((-0.26, 0.04, 0.16), (0.26, 0.04, 0.16), 0.03, metal("#8E93A0"))]
    if not mini:
        parts += [rbox((0, 0.26, 0.16), (0.24, 0.1, 0.18), 0.04, dark),
                  slab([circle(0, 0, 0.215), circle(0, 0, 0.165)], 0.04, 0.012, metal("#8E93A0"))]
        parts[-1].rotation_euler.x = R90
        parts[-1].location.y, parts[-1].location.z = -0.2, 0.16
    group(parts, loc=(0, 0.02, 0.0), rot=(-34, 0, -20), scale=1.2 if mini else 1.1)


def dmx_b(mini):
    tile("#9B6BFF", "#3B0764")
    parts = [cyl((0, 0, 0.1), 0.32, 0.2, metal("#D7DBE2", 0.25), bevel=0.05, verts=96),
             cyl((0, 0, 0.19), 0.26, 0.04, plastic("#1C1C1E", 0.5), verts=96)]
    n = 5 if not mini else 3
    for i in range(n):
        a = math.radians(90 + i * 360 / n)
        parts.append(ball((0.15 * math.cos(a), 0.15 * math.sin(a), 0.21), 0.04, metal("#F2C14E", 0.2)))
    if not mini:
        parts.append(rbox((0, 0.29, 0.2), (0.08, 0.05, 0.03), 0.01, metal("#B0B6C0")))
    group(parts, loc=(0, 0, 0.0), rot=(-18, 10, 0), scale=1.1 if mini else 1.0)


# -- EEPROM ----------------------------------------------------------------

def _dip(n, w=0.36, h=0.56, z=0.0, label="#34C759", mini=False):
    parts = [rbox((0, 0, z + 0.06), (w, h, 0.1), 0.03, plastic("#1C1C1E", 0.35)),
             ball((0, h / 2 - 0.06, z + 0.11), 0.035, plastic("#3A3A3C"), scale=(1, 1, 0.3))]
    if label:
        parts.append(rbox((0, -0.04, z + 0.115), (w * 0.7, h * 0.5, 0.01), 0.01, plastic(label, 0.5)))
    k = 2 if mini else n
    for s in (-1, 1):
        for i in range(k):
            y = (i - (k - 1) / 2) * (h * 0.8 / k)
            parts.append(rbox((s * (w / 2 + 0.05), y, z + 0.04), (0.1, h * 0.45 / k, 0.025), 0.008, metal()))
    return parts


def eeprom_a(mini):
    tile("#34D399", "#047857")
    group(_dip(4, mini=mini, label="#FFFFFF"), loc=(0, 0, 0.02), rot=(-18, 10, 0), scale=1.25 if mini else 1.15)


def eeprom_b(mini):
    tile("#34D399", "#047857")
    parts = [rbox((0, 0, 0.03), (0.6, 0.72, 0.06), 0.03, plastic("#F2E7C9", 0.45))]
    parts += _dip(4, 0.34, 0.5, z=0.06, label=None, mini=mini)
    if not mini:
        parts += [rbox((-0.22, 0.4, 0.08), (0.06, 0.1, 0.05), 0.02, plastic("#3A6FD8")),
                  capsule((-0.22, 0.42, 0.12), (-0.22, 0.5, 0.18), 0.02, metal())]
    group(parts, loc=(0, -0.02, 0.0), rot=(-18, 10, 0), scale=1.15 if mini else 1.0)


# -- Equipos ---------------------------------------------------------------

def equipos_a(mini):
    tile("#FBBF3C", "#92400E")
    parts = [rbox((0, 0, 0.09), (0.78, 0.6, 0.18), 0.06, plastic("#E9EDF2", 0.35)),
             rbox((-0.12, 0.12, 0.19), (0.42, 0.18, 0.03), 0.03, glossy("#1C1C1E"))]
    parts.append(text("12.0", glow("#5CFF9A", 1.2), size=0.12, depth=0.01, bevel=0.002, c=(-0.12, 0.12, 0.205))
                 if not mini else rbox((-0.12, 0.12, 0.21), (0.28, 0.06, 0.01), 0.01, glow("#5CFF9A", 1.2)))
    parts += _knob(0.24, 0.12, 0.2, 0.08, plastic("#2C2C2E"))
    for x, c in ((-0.2, "#FF453A"), (0.0, "#1C1C1E")):
        parts.append(cyl((x, -0.14, 0.2), 0.055, 0.05, glossy(c), bevel=0.015))
    if not mini:
        parts += _knob(0.24, -0.14, 0.2, 0.07, plastic("#2C2C2E"))
    group(parts, loc=(0, 0.0, 0.0), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


def equipos_b(mini):
    tile("#FBBF3C", "#92400E")
    parts = [rbox((0, -0.06, 0.1), (0.42, 0.42, 0.2), 0.1, plastic("#FFFFFF", 0.35))]
    for x in (-0.09, 0.09):
        parts.append(rbox((x, 0.22, 0.1), (0.05, 0.18, 0.05), 0.02, metal()))
    parts.append(capsule((0, -0.27, 0.1), (0.05, -0.44, 0.06), 0.05, plastic("#2C2C2E")))
    parts.append(slab([rounded_poly([(x * 0.45, y * 0.45 - 0.06) for x, y in BOLT], 0.015)], 0.05, 0.015,
                      glossy("#FFC62E"), z=0.19))
    group(parts, loc=(0, 0.02, 0.0), rot=(-18, 10, -10), scale=1.1 if mini else 1.0)


# -- Infrarrojo ------------------------------------------------------------

def infrarrojo_a(mini):
    tile("#F05A62", "#5C0E26")
    parts = [rbox((0, -0.08, 0.07), (0.3, 0.7, 0.12), 0.1, plastic("#2C2C2E", 0.35)),
             ball((0, 0.27, 0.1), 0.05, glow("#FF3B30", 2.5))]
    rows = 3 if not mini else 2
    for r in range(rows):
        for c in (-1, 1):
            parts.append(cyl((c * 0.07, 0.1 - r * 0.13, 0.14), 0.045, 0.03, plastic("#D9DCE3"), bevel=0.012))
    if not mini:
        parts.append(cyl((0, -0.32, 0.14), 0.06, 0.03, glossy("#FF453A"), bevel=0.012))
    parts += waves(0, 0.27, glossy("#FFFFFF"), 1 if mini else 2, 0.1, 0.08, 0.04, 50, 130, z=0.06)
    group(parts, loc=(0, -0.04, 0.0), rot=(-16, 10, -25), scale=1.1 if mini else 1.0)


def infrarrojo_b(mini):
    tile("#F05A62", "#5C0E26")
    dome = glow("#FF6B6B", 1.0)
    parts = [ball((-0.16, 0.0, 0.12), 0.14, dome),
             cyl((-0.27, 0.0, 0.12), 0.14, 0.22, dome, rot=(0, R90, 0), verts=64),
             cyl((-0.38, 0.0, 0.12), 0.165, 0.03, glossy("#C0303A"), rot=(0, R90, 0), bevel=0.01, verts=64),
             capsule((-0.4, 0.05, 0.1), (-0.48, 0.05, 0.08), 0.018, metal()),
             capsule((-0.4, -0.05, 0.1), (-0.48, -0.05, 0.08), 0.018, metal())]
    parts += waves(-0.12, 0.0, glossy("#FFFFFF"), 2 if mini else 3, 0.16, 0.1, 0.045, -40, 40, z=0.06)
    group(parts, loc=(0.0, 0.0, 0.0), rot=(-16, 10, 20), scale=1.05 if mini else 1.0)


# -- Lógica ----------------------------------------------------------------

def logica_a(mini):
    tile("#5BC45F", "#103A14")
    ys = (-0.12, 0.12)
    xs = [-0.36, -0.2, -0.2, 0.0, 0.0, 0.2, 0.2, 0.36] if not mini else [-0.34, -0.1, -0.1, 0.14, 0.14, 0.34]
    pts = []
    for i, x in enumerate(xs):
        pts.append((x, ys[(i // 2) % 2], 0.08))
    parts = [tube(pts, 0.045 if not mini else 0.06, glossy("#FFFFFF"), order=2)]
    group(parts, loc=(0, 0.0, 0.0), rot=(-18, 10, 0), scale=1.0)


def logica_b(mini):
    tile("#5BC45F", "#103A14")
    parts = [capsule((0.02, 0.24, 0.12), (0.02, -0.12, 0.12), 0.09, glossy("#FF453A")),
             capsule((0.02, 0.33, 0.12), (0.02, 0.42, 0.12), 0.025, metal()),
             tube([(0.02, -0.12, 0.1), (0.0, -0.3, 0.08), (-0.2, -0.36, 0.05), (-0.42, -0.3, 0.04)], 0.03,
                  plastic("#2C2C2E"))]
    if not mini:
        parts.append(capsule((-0.05, 0.42, 0.12), (0.09, 0.42, 0.12), 0.018, metal()))
        parts += [rbox((0.02, 0.05, 0.215), (0.12, 0.04, 0.01), 0.01, plastic("#FFFFFF"))]
    group(parts, loc=(0.04, 0.0, 0.0), rot=(-16, 10, -30), scale=1.1 if mini else 1.0)


# -- Luces -----------------------------------------------------------------

def _bulb(x, y, s, c, k=2.0, mini=False):
    parts = [ball((x, y + 0.08 * s, 0.14 * s), 0.2 * s, glow(c, k)),
             cyl((x, y - 0.12 * s, 0.14 * s), 0.1 * s, 0.14 * s, metal("#D7DBE2"), rot=(R90, 0, 0), bevel=0.02 * s),
             ball((x, y - 0.21 * s, 0.14 * s), 0.05 * s, plastic("#2C2C2E"))]
    if not mini:
        for i in range(3):
            parts.append(cyl((x, y - 0.07 * s - i * 0.04 * s, 0.14 * s), 0.108 * s, 0.015 * s, metal("#B0B6C0"),
                             rot=(R90, 0, 0)))
    return parts


def luces_a(mini):
    tile("#F472B6", "#6D28D9")
    group(_bulb(0, 0.04, 1.35, "#FFE07A", 2.0, mini), loc=(0, 0, 0), rot=(-14, 10, -12), scale=1.0)


def luces_b(mini):
    tile("#F472B6", "#6D28D9")
    parts = (_bulb(-0.22, 0.1, 0.75, "#FF6B6B", 2.2, mini) + _bulb(0.0, 0.0, 0.82, "#FFE07A", 2.0, mini) +
             _bulb(0.22, 0.1, 0.75, "#6BD6FF", 2.2, mini))
    group(parts, loc=(0, 0, 0), rot=(-14, 10, 0), scale=1.1 if mini else 1.0)


# -- Modbus ----------------------------------------------------------------

def modbus_a(mini):
    tile("#3FD6A0", "#05664A")
    parts = [rbox((0, 0, 0.03), (0.92, 0.18, 0.05), 0.02, metal("#C9CED6", 0.3)),
             rbox((-0.05, 0.0, 0.14), (0.3, 0.6, 0.2), 0.05, plastic("#F4F5F8", 0.35)),
             rbox((-0.05, 0.12, 0.25), (0.22, 0.14, 0.02), 0.02, plastic("#1F9D55"))]
    n = 3 if not mini else 2
    for i in range(n):
        x = -0.05 + (i - (n - 1) / 2) * 0.08
        parts.append(rbox((x, -0.22, 0.25), (0.06, 0.08, 0.03), 0.015, plastic("#FF9F0A")))
    if not mini:
        parts += [ball((-0.11, 0.0, 0.25), 0.025, glow("#34C759", 2.5)),
                  rbox((0.26, 0.0, 0.12), (0.18, 0.5, 0.16), 0.05, plastic("#DCE2EA", 0.35))]
    group(parts, loc=(0, 0.0, 0.0), rot=(-18, 10, 0), scale=1.1 if mini else 1.0)


def modbus_b(mini):
    tile("#3FD6A0", "#05664A")
    parts = []
    for x in (-0.24, 0.24):
        parts.append(rbox((x, 0.0, 0.1), (0.26, 0.34, 0.16), 0.05, glossy("#1F9D55")))
        for y in (-0.07, 0.07):
            parts.append(cyl((x, y, 0.19), 0.04, 0.03, metal(), bevel=0.01))
    parts.append(tube([(-0.12, 0.0, 0.1), (0.0, 0.12, 0.12), (0.12, 0.0, 0.1)], 0.03, glossy("#FFD60A")))
    parts.append(tube([(-0.12, -0.08, 0.1), (0.0, -0.2, 0.12), (0.12, -0.08, 0.1)], 0.03, glossy("#2D7BF0")))
    group(parts, loc=(0, 0.0, 0.0), rot=(-18, 10, 0), scale=1.15 if mini else 1.0)


# -- Monitor serie ---------------------------------------------------------

def _sine(x0, x1, y, amp, z, r, m, cycles=1.5, n=14):
    return tube([(x0 + (x1 - x0) * i / n, y + amp * math.sin(i / n * cycles * 2 * math.pi), z)
                 for i in range(n + 1)], r, m, order=4)


def plotter_a(mini):
    tile("#2A8FA6", "#061A20")
    parts = [rbox((0, 0, 0.09), (0.8, 0.62, 0.18), 0.07, plastic("#D9DEE5", 0.35)),
             rbox((-0.1, 0.02, 0.19), (0.5, 0.46, 0.03), 0.04, glossy("#0E1A14")),
             _sine(-0.3, 0.1, 0.02, 0.12, 0.215, 0.018 if not mini else 0.03, glow("#5CFF9A", 2.0))]
    if not mini:
        parts += _knob(0.27, 0.13, 0.2, 0.06, plastic("#2C2C2E"))
        parts += _knob(0.27, -0.1, 0.2, 0.06, plastic("#2C2C2E"))
    group(parts, loc=(0, 0.0, 0.0), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


def plotter_b(mini):
    tile("#2A8FA6", "#061A20")
    parts = [_sine(-0.36, 0.36, 0.0, 0.2, 0.08, 0.05 if not mini else 0.065, glossy("#5CFF9A"),
                   cycles=1.0 if mini else 1.5)]
    group(parts, loc=(0, 0.0, 0.0), rot=(-18, 10, 0), scale=1.0)


# -- PWM -------------------------------------------------------------------

def pwm_a(mini):
    tile("#FFB347", "#B34700")
    parts = [cyl((0, 0, 0.12), 0.28, 0.2, glossy("#FF7A1A"), bevel=0.06, verts=96),
             cyl((0, 0, 0.22), 0.2, 0.02, plastic("#FFFFFF", 0.4), verts=64),
             rbox((0, 0.12, 0.235), (0.05, 0.14, 0.02), 0.012, plastic("#2C2C2E"))]
    n = 7 if not mini else 0
    for i in range(n):
        a = math.radians(225 - i * 270 / (n - 1))
        t = rbox((0.36 * math.cos(a), 0.36 * math.sin(a), 0.03), (0.07, 0.03, 0.03), 0.012, plastic("#FFFFFF"))
        t.rotation_euler.z = a
        parts.append(t)
    group(parts, loc=(0, 0.0, 0.0), rot=(-16, 10, 0), scale=1.25 if mini else 1.0)


def pwm_b(mini):
    tile("#FFB347", "#B34700")
    blue = glossy("#2D7BF0")
    parts = [rbox((0, -0.06, 0.1), (0.4, 0.5, 0.2), 0.05, blue),
             rbox((0, -0.06, 0.1), (0.62, 0.12, 0.06), 0.03, blue),
             cyl((0, 0.08, 0.22), 0.09, 0.06, plastic("#FFFFFF"), bevel=0.02),
             rbox((0.0, 0.08, 0.26), (0.42, 0.08, 0.03), 0.04, plastic("#FFFFFF", 0.4))]
    if not mini:
        for s in (-1, 1):
            parts.append(cyl((s * 0.26, -0.06, 0.14), 0.025, 0.02, plastic("#1C1C1E")))
        parts.append(tube([(0, -0.31, 0.06), (-0.05, -0.4, 0.04), (-0.2, -0.44, 0.03)], 0.025, plastic("#FF9F0A")))
    group(parts, loc=(0, 0.04, 0.0), rot=(-18, 10, 14), scale=1.12 if mini else 1.0)


# -- RF --------------------------------------------------------------------

def rf_a(mini):
    tile("#5A8BFF", "#7A2BD9")
    parts = [rbox((-0.16, -0.22, 0.08), (0.24, 0.24, 0.14), 0.06, plastic("#2C2C2E", 0.35)),
             capsule((-0.16, -0.16, 0.12), (0.12, 0.2, 0.16), 0.03, metal()),
             ball((0.12, 0.2, 0.16), 0.05, glossy("#FF453A"))]
    parts += waves(0.12, 0.2, glossy("#FFFFFF"), 1 if mini else 2, 0.1, 0.08, 0.04, -60, 30, z=0.12)
    group(parts, loc=(0, 0.0, 0.0), rot=(-16, 10, 0), scale=1.15 if mini else 1.0)


def rf_b(mini):
    tile("#5A8BFF", "#7A2BD9")
    parts = [rbox((0, -0.06, 0.1), (0.76, 0.5, 0.2), 0.08, glossy("#FFFFFF")),
             rbox((-0.12, 0.04, 0.2), (0.4, 0.14, 0.03), 0.03, plastic("#1C1C1E")),
             rbox((-0.08, 0.04, 0.22), (0.02, 0.12, 0.01), 0.005, glossy("#FF453A"))]
    parts += _knob(0.24, -0.12, 0.2, 0.08, plastic("#2C2C2E"))
    if not mini:
        for i in range(4):
            parts.append(rbox((-0.12, -0.12 - i * 0.04, 0.205), (0.36, 0.015, 0.01), 0.006, plastic("#C9CED6")))
        for i in range(5):
            parts.append(rbox((-0.28 + i * 0.08, 0.04, 0.22), (0.01, 0.05 if i % 2 else 0.08, 0.01), 0.004,
                              plastic("#FFFFFF")))
    parts.append(capsule((0.3, 0.16, 0.12), (0.4, 0.42, 0.12), 0.02, metal()))
    group(parts, loc=(0, 0.0, 0.0), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


MODELS = {
    "aos.can": {"a": can_a, "b": can_b},
    "aos.datalog": {"a": datalog_a, "b": datalog_b},
    "aos.dmx": {"a": dmx_a, "b": dmx_b},
    "aos.eeprom": {"a": eeprom_a, "b": eeprom_b},
    "aos.equipos": {"a": equipos_a, "b": equipos_b},
    "aos.infrarrojo": {"a": infrarrojo_a, "b": infrarrojo_b},
    "aos.logica": {"a": logica_a, "b": logica_b},
    "aos.luces": {"a": luces_a, "b": luces_b},
    "aos.modbus": {"a": modbus_a, "b": modbus_b},
    "aos.plotter": {"a": plotter_a, "b": plotter_b},
    "aos.pwm": {"a": pwm_a, "b": pwm_b},
    "aos.rf": {"a": rf_a, "b": rf_b},
}
