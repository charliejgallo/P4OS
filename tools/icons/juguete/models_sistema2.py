"""'juguete' models: the group sistema-2 of apps.json."""

import math

from lib import (BOLT, ball, capsule, circle, cyl, glossy, glow, group, matte, metal, plastic, rbox,
                 rounded_poly, rrect, slab, text, tile, tube, waves)

R90 = math.radians(90)


# -- Terminal --------------------------------------------------------------

def serial_a(mini):
    tile("#41505E", "#10161B")
    shell, gold = metal("#E9EDF2", 0.28), metal("#F2B92C", 0.25)
    outer = rounded_poly([(-0.36, 0.17), (0.36, 0.17), (0.29, -0.15), (-0.29, -0.15)], 0.07)
    inner = rounded_poly([(-0.27, 0.11), (0.27, 0.11), (0.22, -0.09), (-0.22, -0.09)], 0.045)
    parts = [slab([outer], 0.16, 0.045, shell), slab([inner], 0.03, 0.01, plastic("#151C22", 0.5), z=0.15)]
    if not mini:
        parts += [ball((-0.16 + i * 0.08, 0.045, 0.18), 0.024, gold) for i in range(5)]
        parts += [ball((-0.12 + i * 0.08, -0.035, 0.18), 0.024, gold) for i in range(4)]
    else:
        parts += [ball((-0.12 + i * 0.12, 0.01, 0.18), 0.034, gold) for i in range(3)]
    parts.append(rbox((0, -0.2, 0.07), (0.16, 0.12, 0.12), 0.04, shell))
    parts.append(tube([(0, -0.24, 0.07), (0, -0.33, 0.06), (0.06, -0.42, 0.03), (0.02, -0.56, 0.02)],
                      0.05 if mini else 0.04, plastic("#2D7BF0", 0.45)))
    group(parts, loc=(0, 0.06, 0.02), rot=(-18, 0, 0), scale=1.25 if mini else 1.0)


def serial_b(mini):
    tile("#41505E", "#10161B")
    body = plastic("#E9E4D8", 0.4)
    parts = [rbox((0, 0.04, 0.1), (0.72, 0.6, 0.2), 0.1, body),
             rbox((0, 0.06, 0.2), (0.56, 0.42, 0.04), 0.06, glossy("#0E1A14")),
             text(">_", glow("#5CFF9A", 1.6), size=0.24 if not mini else 0.3, depth=0.02, bevel=0.004,
                  c=(-0.06 if not mini else -0.02, 0.07, 0.215))]
    parts.append(rbox((0, -0.3, 0.04), (0.4, 0.12, 0.08), 0.04, body))
    if not mini:
        parts.append(ball((0.26, -0.18, 0.2), 0.025, glow("#34C759", 1.5)))
    group(parts, loc=(0, 0.0, 0.0), rot=(-16, 10, 0), scale=1.08 if mini else 1.0)


# -- Programador -----------------------------------------------------------

def _chip(color, w=0.48, legs=6, mini=False, z=0.0):
    parts = [rbox((0, 0, z + 0.05), (w, w, 0.1), 0.04, plastic(color, 0.35))]
    n = 3 if mini else legs
    for s in (-1, 1):
        for i in range(n):
            y = (i - (n - 1) / 2) * (w * 0.8 / n)
            parts.append(rbox((s * (w / 2 + 0.04), y, z + 0.035), (0.1, w * 0.45 / n, 0.03), 0.01, metal()))
    return parts


def flasher_a(mini):
    tile("#A27BFF", "#4C1D95")
    parts = _chip("#2C2C2E", mini=mini)
    parts.append(slab([rounded_poly([(x * 0.62, y * 0.62) for x, y in BOLT], 0.02)], 0.08, 0.025,
                      glossy("#FFD60A"), z=0.1))
    group(parts, loc=(0, 0, 0.02), rot=(-18, 10, 0), scale=1.2 if mini else 1.05)


def flasher_b(mini):
    tile("#A27BFF", "#4C1D95")
    parts = _chip("#2C2C2E", 0.44, mini=mini, z=0.0)
    for p in parts:
        p.location.y -= 0.14
    arrow = rounded_poly([(-0.07, 0.42), (0.07, 0.42), (0.07, 0.2), (0.16, 0.2), (0, 0.04), (-0.16, 0.2),
                          (-0.07, 0.2)], 0.02)
    parts.append(slab([arrow], 0.1, 0.03, glossy("#FFFFFF"), z=0.12))
    group(parts, loc=(0, -0.02, 0.02), rot=(-18, 10, 0), scale=1.12 if mini else 1.0)


# -- Home Assistant --------------------------------------------------------

def _house(c_wall, c_roof, mini, s=1.0):
    wall = rounded_poly([(-0.28 * s, -0.3 * s), (0.28 * s, -0.3 * s), (0.28 * s, 0.06 * s),
                         (0, 0.3 * s), (-0.28 * s, 0.06 * s)], [0.05, 0.05, 0.03, 0.06, 0.03])
    roof = rounded_poly([(-0.38 * s, 0.02 * s), (0, 0.36 * s), (0.38 * s, 0.02 * s), (0.3 * s, -0.04 * s),
                         (0, 0.24 * s), (-0.3 * s, -0.04 * s)], 0.03)
    parts = [slab([wall], 0.16, 0.04, plastic(c_wall, 0.4)),
             slab([roof], 0.08, 0.03, glossy(c_roof), z=0.12)]
    parts.append(rbox((0.0, -0.2 * s, 0.16), (0.14 * s, 0.22 * s, 0.03), 0.04 * s, plastic("#8A5A3C")))
    if not mini:
        parts.append(rbox((0.15 * s, -0.04 * s, 0.16), (0.1 * s, 0.1 * s, 0.03), 0.02, glow("#FFE9A6", 0.6)))
        parts.append(rbox((-0.15 * s, -0.04 * s, 0.16), (0.1 * s, 0.1 * s, 0.03), 0.02, glow("#FFE9A6", 0.6)))
    return parts


def ha_a(mini):
    tile("#5CCBFA", "#0B6FB0")
    group(_house("#FFFFFF", "#FF7A45", mini, 1.1), loc=(0, -0.02, 0.02), rot=(-18, 10, 0),
          scale=1.15 if mini else 1.0)


def ha_b(mini):
    tile("#5CCBFA", "#0B6FB0")
    parts = _house("#FFFFFF", "#2D7BF0", True, 0.85)
    for p in parts:
        p.location.y += 0.1
    nodes = [(-0.24, -0.33), (0.0, -0.36), (0.24, -0.33)]
    for x, y in nodes:
        parts.append(ball((x, y, 0.08), 0.06, glossy("#FFD60A")))
    parts.append(tube([(-0.24, -0.33, 0.07), (0, -0.36, 0.07), (0.24, -0.33, 0.07)], 0.018, metal()))
    parts.append(capsule((0, -0.36, 0.07), (0, -0.2, 0.07), 0.018, metal()))
    group(parts, loc=(0, 0.02, 0.02), rot=(-16, 10, 0), scale=1.1 if mini else 1.0)


# -- Bus -------------------------------------------------------------------

def bus_a(mini):
    tile("#FBBF3C", "#B45309")
    white, dark = plastic("#FFFFFF", 0.35), plastic("#2C2C2E", 0.4)
    plug = [rbox((-0.2, 0.0, 0.08), (0.26, 0.3, 0.16), 0.07, white),
            capsule((-0.36, 0.0, 0.08), (-0.44, -0.0, 0.06), 0.05, dark)]
    for y in (-0.07, 0.07):
        plug.append(capsule((-0.06, y, 0.08), (0.04, y, 0.08), 0.025, metal("#F2C14E")))
    sock = [rbox((0.22, 0.0, 0.08), (0.24, 0.32, 0.16), 0.07, glossy("#FF6A1A")),
            capsule((0.36, 0.0, 0.08), (0.44, 0.0, 0.06), 0.05, dark)]
    if not mini:
        for y in (-0.07, 0.07):
            sock.append(cyl((0.1, y, 0.08), 0.03, 0.02, matte("#1C1C1E"), rot=(0, R90, 0)))
    group(plug + sock, loc=(0, 0, 0.02), rot=(-18, 0, -20), scale=1.05 if mini else 0.95)


def bus_b(mini):
    tile("#FBBF3C", "#B45309")
    parts = []
    cols = ("#FF453A", "#2D7BF0", "#34C759") if not mini else ("#FF453A", "#2D7BF0")
    for i, c in enumerate(cols):
        x = (i - (len(cols) - 1) / 2) * 0.2
        parts.append(tube([(x - 0.1, -0.4, 0.04), (x + 0.12, -0.12, 0.1), (x - 0.08, 0.1, 0.1),
                           (x, 0.22, 0.08)], 0.035 if not mini else 0.05, glossy(c)))
        parts.append(rbox((x, 0.3, 0.08), (0.08 if not mini else 0.11, 0.16, 0.08), 0.02, plastic("#1C1C1E")))
        parts.append(capsule((x, 0.38, 0.08), (x, 0.44, 0.08), 0.012, metal()))
    group(parts, loc=(0, 0, 0.02), rot=(-16, 10, 0), scale=1.1 if mini else 1.0)


# -- Banco -----------------------------------------------------------------

def bench_a(mini):
    tile("#F46B6B", "#991B1B")
    red, ink = glossy("#E33030"), plastic("#1C1C1E")
    parts = [cyl((0, 0.04, 0.06), 0.34, 0.12, metal("#E3E7EC", 0.25), bevel=0.04, verts=96),
             cyl((0, 0.04, 0.1), 0.29, 0.06, plastic("#FFFFFF", 0.45), verts=96),
             rbox((0, -0.36, 0.05), (0.12, 0.14, 0.08), 0.03, metal("#E9C46A", 0.3))]
    n = 5 if mini else 9
    for i in range(n):
        a = math.radians(225 - i * 270 / (n - 1))
        t = rbox((0.22 * math.cos(a), 0.04 + 0.22 * math.sin(a), 0.135), (0.06, 0.025, 0.02), 0.01,
                 red if i >= n - 2 else ink)
        t.rotation_euler.z = a
        parts.append(t)
    a = math.radians(20)
    nd = rbox((0.09 * math.cos(a), 0.04 + 0.09 * math.sin(a), 0.15), (0.22, 0.04, 0.025), 0.015, red)
    nd.rotation_euler.z = a
    parts += [nd, cyl((0, 0.04, 0.16), 0.045, 0.04, ink, bevel=0.012)]
    group(parts, loc=(0, 0.0, 0.02), rot=(-16, 10, 0), scale=1.1 if mini else 1.0)


def bench_b(mini):
    tile("#F46B6B", "#991B1B")
    parts = [rbox((0, 0.02, 0.07), (0.5, 0.74, 0.14), 0.08, glossy("#FFC62E")),
             rbox((0, 0.22, 0.15), (0.38, 0.18, 0.03), 0.03, glossy("#3E4B36")),
             cyl((0, -0.08, 0.16), 0.11, 0.05, plastic("#2C2C2E"), bevel=0.02, verts=64),
             rbox((0, -0.02, 0.2), (0.03, 0.1, 0.02), 0.01, plastic("#FFFFFF"))]
    if not mini:
        parts.append(text("8.8", glow("#D9FFB0", 0.4), size=0.12, depth=0.012, bevel=0.003, c=(0, 0.22, 0.165)))
        for x, c in ((-0.12, "#E33030"), (0.12, "#1C1C1E")):
            parts.append(cyl((x, -0.27, 0.15), 0.035, 0.03, plastic(c)))
    for x, c in ((-0.3, "#E33030"), (0.32, "#1C1C1E")):
        parts.append(capsule((x, -0.18, 0.1), (x * 1.2, -0.42, 0.06), 0.03, glossy(c)))
    group(parts, loc=(0, 0.02, 0.0), rot=(-16, 10, 0), scale=1.08 if mini else 1.0)


# -- Electrónica -----------------------------------------------------------

def elec_a(mini):
    tile("#FFD84A", "#CA8A04")
    b = slab([rounded_poly(BOLT, 0.025)], 0.14, 0.045, glossy("#FFFFFF"))
    group([b], loc=(0, 0, 0.03), rot=(-18, 12, 0), scale=1.2 if mini else 1.1)


def elec_b(mini):
    tile("#FFD84A", "#CA8A04")
    body = plastic("#E8D3A8", 0.4)
    parts = [capsule((-0.18, 0, 0.12), (0.18, 0, 0.12), 0.12, body)]
    bands = ("#8B4513", "#1C1C1E", "#FF453A", "#E9C46A") if not mini else ("#8B4513", "#FF453A")
    for i, c in enumerate(bands):
        x = -0.16 + i * (0.32 / max(1, len(bands) - 1)) if len(bands) > 2 else (-0.08 + i * 0.16)
        parts.append(cyl((x, 0, 0.12), 0.125, 0.045, glossy(c), rot=(0, R90, 0)))
    parts += [capsule((-0.3, 0, 0.1), (-0.46, 0, 0.06), 0.022, metal()),
              capsule((0.3, 0, 0.1), (0.46, 0, 0.06), 0.022, metal())]
    group(parts, loc=(0, 0, 0.0), rot=(-16, 10, 30), scale=1.15 if mini else 1.0)


def elec_c(mini):
    tile("#FFD84A", "#CA8A04")
    parts = [cyl((0, 0.06, 0.14), 0.16, 0.48, glossy("#2D5BD8"), rot=(R90, 0, 0), bevel=0.05, verts=64),
             cyl((0, 0.31, 0.14), 0.13, 0.03, metal(), rot=(R90, 0, 0), verts=64)]
    if not mini:
        parts.append(rbox((0.1, 0.06, 0.29), (0.07, 0.4, 0.01), 0.005, plastic("#DCE6FF")))
        parts.append(text("-", plastic("#2D5BD8"), size=0.12, depth=0.01, bevel=0.002, c=(0.1, 0.18, 0.3)))
    parts += [capsule((-0.07, -0.18, 0.1), (-0.09, -0.44, 0.05), 0.02, metal()),
              capsule((0.07, -0.18, 0.1), (0.09, -0.4, 0.05), 0.02, metal())]
    group(parts, loc=(0, 0.02, 0.0), rot=(-16, 10, -20), scale=1.12 if mini else 1.0)


# -- Tiras LED -------------------------------------------------------------

def leds_a(mini):
    tile("#FFB347", "#B45309")
    n = 7 if not mini else 4
    cols = ("#FF453A", "#FFD60A", "#34C759", "#5EE7FF", "#2D7BF0", "#BF5AF2", "#FF375F")
    parts = []
    for i in range(n):
        t = i / (n - 1)
        x = -0.34 + 0.68 * t
        y = 0.18 * math.sin(t * math.pi * 1.6) - 0.02
        seg = rbox((x, y, 0.06), (0.68 / (n - 1) + 0.02, 0.16, 0.04), 0.02, plastic("#FFFFFF", 0.35))
        nx = math.cos(t * math.pi * 1.6) * 0.18 * math.pi * 1.6 / 0.68
        seg.rotation_euler.z = math.atan(nx)
        parts += [seg, ball((x, y, 0.1), 0.045 if not mini else 0.06, glow(cols[i % len(cols)], 2.0),
                            scale=(1, 1, 0.6))]
    group(parts, loc=(0, 0, 0.0), rot=(-18, 10, 0), scale=1.15 if mini else 1.05)


def leds_b(mini):
    tile("#FFB347", "#B45309")
    red = glow("#FF3B30", 1.2)
    parts = [ball((0, 0.12, 0.12), 0.18, red, scale=(1, 1.0, 0.9)),
             cyl((0, -0.02, 0.12), 0.18, 0.28, red, rot=(R90, 0, 0), verts=64),
             cyl((0, -0.17, 0.12), 0.21, 0.04, glossy("#D9261C"), rot=(R90, 0, 0), bevel=0.012, verts=64),
             capsule((-0.07, -0.2, 0.1), (-0.07, -0.46, 0.06), 0.02, metal()),
             capsule((0.07, -0.2, 0.1), (0.07, -0.4, 0.06), 0.02, metal())]
    if not mini:
        parts.append(ball((-0.06, 0.18, 0.26), 0.04, glow("#FFFFFF", 1.0), scale=(1, 1.4, 0.5)))
    group(parts, loc=(0, 0.04, 0.0), rot=(-16, 10, -18), scale=1.1 if mini else 1.0)


# -- Macro pad -------------------------------------------------------------

def macropad_a(mini):
    tile("#4FE0F2", "#0E7490")
    parts = [rbox((0, 0, 0.04), (0.72, 0.72, 0.08), 0.08, plastic("#1F2A33", 0.4))]
    n = 2 if mini else 3
    k = 0.26 if mini else 0.18
    for r in range(n):
        for c in range(n):
            x, y = (c - (n - 1) / 2) * (k + 0.04), ((n - 1) / 2 - r) * (k + 0.04)
            hot = (r, c) == (0, n - 1)
            parts.append(rbox((x, y, 0.12), (k, k, 0.08), 0.04, glossy("#FF9F0A" if hot else "#E9F7FA")))
    group(parts, loc=(0, 0, 0.0), rot=(-18, 10, 0), scale=1.0)


def macropad_b(mini):
    tile("#4FE0F2", "#0E7490")
    parts = [rbox((0, 0, 0.04), (0.74, 0.6, 0.08), 0.08, plastic("#1F2A33", 0.4)),
             cyl((0.2, 0.0, 0.14), 0.13, 0.14, metal("#C9CED6", 0.3), bevel=0.03, verts=64),
             rbox((0.2, 0.08, 0.215), (0.03, 0.08, 0.01), 0.008, plastic("#FF9F0A"))]
    for r in range(2):
        for c in range(1 if mini else 2):
            x = -0.22 + c * 0.17 if not mini else -0.15
            k = 0.14 if not mini else 0.22
            parts.append(rbox((x, 0.1 - r * (k + 0.04) + (0.02 if mini else 0), 0.12), (k, k, 0.08), 0.035,
                              glossy("#E9F7FA" if (r + c) % 2 else "#5EE7FF")))
    group(parts, loc=(0, 0, 0.0), rot=(-18, 10, 0), scale=1.05 if mini else 1.0)


# -- Módulos ---------------------------------------------------------------

def modules_a(mini):
    tile("#AAB8CB", "#475569")
    parts = [rbox((0, 0.03, 0.03), (0.74, 0.5, 0.05), 0.03, plastic("#1F9D55", 0.4)),
             rbox((-0.1, 0.06, 0.09), (0.24, 0.24, 0.07), 0.03, plastic("#1C1C1E", 0.35))]
    if not mini:
        parts.append(rbox((0.2, 0.12, 0.08), (0.12, 0.08, 0.05), 0.02, plastic("#2C2C2E")))
        parts.append(ball((0.22, -0.05, 0.08), 0.035, glow("#5EE7FF", 1.5)))
    n = 6 if not mini else 3
    for i in range(n):
        parts.append(rbox((-0.3 + i * 0.6 / (n - 1), -0.2, 0.06), (0.05, 0.08, 0.012), 0.008, metal("#F2C14E")))
    group(parts, loc=(0, 0.0, 0.0), rot=(-18, 10, 6), scale=1.1 if mini else 1.0)


def modules_b(mini):
    tile("#AAB8CB", "#475569")
    parts = []
    for (x, y, c) in ((-0.13, 0.1, "#2D7BF0"), (0.13, -0.12, "#FF9F0A")):
        parts.append(rbox((x, y, 0.08), (0.42, 0.26, 0.16), 0.05, glossy(c)))
        for k in (-0.1, 0.1):
            parts.append(cyl((x + k, y, 0.18), 0.06, 0.05, glossy(c), bevel=0.015))
    group(parts, loc=(0, 0.0, 0.0), rot=(-22, 12, 0), scale=1.12 if mini else 1.0)


# -- MQTT ------------------------------------------------------------------

def mqtt_a(mini):
    tile("#B9A2FF", "#6D28D9")
    w = glossy("#FFFFFF")
    parts = [capsule((0, -0.36, 0.06), (0, 0.06, 0.08), 0.04, w),
             ball((0, 0.12, 0.09), 0.08, glossy("#FFD60A")),
             cyl((0, -0.38, 0.03), 0.13, 0.05, w, bevel=0.02)]
    n = 1 if mini else 2
    parts += waves(0, 0.12, w, n, 0.16, 0.1, 0.05, -45, 45, 0.06, 0.06)
    parts += [p for p in waves(0, 0.12, w, n, 0.16, 0.1, 0.05, 135, 225, 0.06, 0.06)]
    group(parts, loc=(0, 0.0, 0.0), rot=(-16, 10, 0), scale=1.15 if mini else 1.0)


def mqtt_b(mini):
    tile("#B9A2FF", "#6D28D9")
    def bubble(x, y, c, s=1.0):
        out = rounded_poly([(x - 0.15 * s, y - 0.09 * s), (x - 0.05 * s, y - 0.09 * s), (x - 0.1 * s, y - 0.17 * s),
                            (x + 0.02 * s, y - 0.09 * s), (x + 0.15 * s, y - 0.09 * s), (x + 0.15 * s, y + 0.09 * s),
                            (x - 0.15 * s, y + 0.09 * s)], [0.04, 0.01, 0.01, 0.01, 0.04, 0.04, 0.04])
        return slab([out], 0.08, 0.025, glossy(c), z=0.04)
    spots = [(-0.18, 0.2, "#FFFFFF"), (0.2, 0.08, "#FFD60A"), (-0.08, -0.22, "#5EE7FF")]
    parts = [tube([(-0.18, 0.2, 0.03), (0.2, 0.08, 0.03), (-0.08, -0.22, 0.03)], 0.02, metal())]
    s = 1.25 if mini else 1.0
    parts += [bubble(x, y, c, s) for x, y, c in spots]
    group(parts, loc=(0, 0.0, 0.0), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


# -- Red -------------------------------------------------------------------

def net_a(mini):
    tile("#5FCBFA", "#0369A1")
    parts = [rbox((0, 0, 0.08), (0.56, 0.74, 0.16), 0.06, plastic("#2C2F36", 0.4))]
    n = 3 if not mini else 2
    for i in range(n):
        y = 0.22 - i * (0.44 / max(1, n - 1)) if n > 1 else 0
        parts.append(rbox((0, y, 0.17), (0.46, 0.16, 0.03), 0.03, plastic("#454A55", 0.35)))
        parts.append(ball((-0.15, y, 0.19), 0.03, glow("#34C759", 2.0)))
        if not mini:
            parts.append(ball((-0.07, y, 0.19), 0.03, glow("#5EE7FF" if i != 1 else "#FF9F0A", 2.0)))
            for k in range(3):
                parts.append(rbox((0.07 + k * 0.05, y, 0.19), (0.025, 0.09, 0.01), 0.008, matte("#1C1C1E")))
    group(parts, loc=(0, 0.0, 0.0), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


def net_b(mini):
    tile("#5FCBFA", "#0369A1")
    pts = [(0, 0.24), (-0.25, -0.18), (0.25, -0.18)]
    parts = [tube([(*pts[0], 0.06), (*pts[1], 0.06)], 0.025, metal(), order=2),
             tube([(*pts[1], 0.06), (*pts[2], 0.06)], 0.025, metal(), order=2),
             tube([(*pts[2], 0.06), (*pts[0], 0.06)], 0.025, metal(), order=2)]
    for (x, y), c in zip(pts, ("#FFFFFF", "#FFD60A", "#FF7A45")):
        parts.append(ball((x, y, 0.1), 0.13 if not mini else 0.15, glossy(c)))
    group(parts, loc=(0, 0.0, 0.0), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


MODELS = {
    "aos.serial": {"a": serial_a, "b": serial_b},
    "aos.flasher": {"a": flasher_a, "b": flasher_b},
    "aos.ha": {"a": ha_a, "b": ha_b},
    "aos.bus": {"a": bus_a, "b": bus_b},
    "aos.bench": {"a": bench_a, "b": bench_b},
    "aos.elec": {"a": elec_a, "b": elec_b, "c": elec_c},
    "aos.leds": {"a": leds_a, "b": leds_b},
    "aos.macropad": {"a": macropad_a, "b": macropad_b},
    "aos.modules": {"a": modules_a, "b": modules_b},
    "aos.mqtt": {"a": mqtt_a, "b": mqtt_b},
    "aos.net": {"a": net_a, "b": net_b},
}
