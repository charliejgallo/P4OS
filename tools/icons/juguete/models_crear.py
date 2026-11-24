"""'juguete' models: the group crear-medios of apps.json."""

import math

from lib import (arc_band, ball, capsule, circle, cyl, glossy, glow, group, matte, metal, plastic, rbox,
                 rounded_poly, rrect, slab, text, tile, tube)

R90 = math.radians(90)


def _knob(x, y, z, r, m):
    return [cyl((x, y, z), r, r * 0.8, m, bevel=r * 0.25, verts=48),
            rbox((x, y + r * 0.5, z + r * 0.42), (r * 0.22, r * 0.6, 0.01), 0.005, plastic("#FFFFFF"))]


# -- Dibujo ----------------------------------------------------------------

def dibujo_a(mini):
    tile("#F58A6E", "#7A2E8C")
    parts = [capsule((0, -0.12, 0), (0, -0.5, 0), 0.06, glossy("#FF453A")),
             cyl((0, -0.06, 0), 0.065, 0.12, metal(), rot=(R90, 0, 0), bevel=0.01),
             cyl((0, 0.06, 0), 0.06, 0.16, plastic("#3A2A20", 0.6), rot=(-R90, 0, 0), r2=0.015),
             ball((0, 0.12, 0), 0.05, glossy("#2D7BF0"), scale=(1, 1.3, 1))]
    brush = group(parts, loc=(0.06, 0.12, 0.14), rot=(0, 0, -35), name="brush")
    drop = [ball((-0.2, -0.2, 0.05), 0.11, glossy("#2D7BF0"), scale=(1, 1, 0.5))]
    if not mini:
        drop.append(ball((-0.06, -0.32, 0.04), 0.05, glossy("#2D7BF0"), scale=(1, 1, 0.5)))
    group([brush] + drop, loc=(0, 0.0, 0.02), rot=(-16, 10, 0), scale=1.1 if mini else 1.0)


def dibujo_b(mini):
    tile("#F58A6E", "#7A2E8C")
    pal = rounded_poly([(-0.36, 0.1), (-0.2, 0.32), (0.18, 0.34), (0.38, 0.12), (0.32, -0.22), (0.05, -0.34),
                        (-0.12, -0.18), (-0.3, -0.22)], 0.1, seg=10)
    parts = [slab([pal, circle(-0.2, -0.06, 0.06)], 0.06, 0.02, plastic("#F2D2A0", 0.45))]
    blobs = ((-0.14, 0.18, "#FF453A"), (0.06, 0.22, "#FFD60A"), (0.22, 0.08, "#34C759"), (0.18, -0.14, "#2D7BF0"),
             (0.0, -0.18, "#BF5AF2"))
    for x, y, c in (blobs if not mini else blobs[:4]):
        parts.append(ball((x, y, 0.06), 0.07 if not mini else 0.085, glossy(c), scale=(1, 1, 0.55)))
    group(parts, loc=(0, 0.0, 0.08), rot=(-16, 10, 0), scale=1.1 if mini else 1.0)


# -- Notas -----------------------------------------------------------------

def notas_a(mini):
    tile("#FFE55E", "#FFA20A")
    line = plastic("#C3C8D1", 0.5)
    parts = [rbox((0, -0.01, 0.03), (0.58, 0.7, 0.07), 0.035, plastic("#FFFFFF", 0.5)),
             rbox((0, 0.29, 0.06), (0.6, 0.14, 0.1), 0.04, glossy("#FF8A00"))]
    rows = ((0.1, 0.4), (-0.07, 0.4), (-0.24, 0.26)) if not mini else ((0.06, 0.38), (-0.16, 0.26))
    for y, w in rows:
        parts.append(rbox((-0.2 + w / 2, y, 0.07), (w, 0.035 if not mini else 0.06, 0.012), 0.012, line))
    if not mini:
        r = 0.05
        pp = [cyl((0, 0, 0), r, 0.42, glossy("#FF6A3D"), rot=(R90, 0, 0), verts=6, bevel=0.012),
              cyl((0, -0.25, 0), r * 0.98, 0.09, matte("#F2C38C"), r2=0.012, rot=(R90, 0, 0), verts=24),
              cyl((0, -0.293, 0), 0.016, 0.02, plastic("#3A3A3C"), r2=0.003, rot=(R90, 0, 0), verts=16),
              cyl((0, 0.235, 0), r * 1.02, 0.05, metal(), rot=(R90, 0, 0), bevel=0.008),
              cyl((0, 0.285, 0), r, 0.06, plastic("#FF7DA8", 0.5), rot=(R90, 0, 0), bevel=0.02)]
        parts.append(group(pp, loc=(0.2, -0.16, 0.16), rot=(0, 18, 40), name="pencil"))
    group(parts, loc=(0, 0.0, 0.02), rot=(-10, 6, -4), scale=1.12 if mini else 1.0)


def notas_b(mini):
    tile("#FFE55E", "#FFA20A")
    parts = [rbox((0, -0.04, 0.03), (0.62, 0.62, 0.04), 0.02, plastic("#FFF27A", 0.5))]
    for i, w in enumerate((0.44, 0.44, 0.3) if not mini else (0.42, 0.28)):
        parts.append(rbox((-0.22 + w / 2, 0.06 - i * (0.14 if not mini else 0.2), 0.055),
                          (w, 0.04 if not mini else 0.06, 0.01), 0.01, plastic("#C9A640", 0.5)))
    parts += [ball((0, 0.2, 0.16), 0.08, glossy("#FF453A")),
              cyl((0, 0.2, 0.08), 0.02, 0.1, metal(), r2=0.005)]
    group(parts, loc=(0, 0.0, 0.06), rot=(-16, 10, 6), scale=1.1 if mini else 1.0)


# -- Oficina ---------------------------------------------------------------

def oficina_a(mini):
    tile("#4CD472", "#0A7A3A")
    parts = []
    for i, (dx, dy, rz) in enumerate(((0.06, 0.05, 8), (0.03, 0.02, -5), (0.0, 0.0, 0))):
        sh = rbox((dx, dy, 0.03 + i * 0.03), (0.56, 0.7, 0.02), 0.012, plastic("#FFFFFF" if i == 2 else "#E9EEF5", 0.5))
        sh.rotation_euler.z = math.radians(rz)
        parts.append(sh)
    n, m = (3, 4) if not mini else (2, 3)
    for r in range(m):
        for c in range(n):
            x = -0.15 + c * (0.3 / max(1, n - 1))
            y = 0.18 - r * (0.42 / max(1, m - 1))
            parts.append(rbox((x, y, 0.11), (0.3 / n + 0.02, 0.42 / m * 0.7, 0.015), 0.008,
                              glossy("#34C759") if r == 0 else plastic("#BFE8CB", 0.5)))
    group(parts, loc=(0, 0.0, 0.06), rot=(-16, 10, 0), scale=1.1 if mini else 1.0)


def oficina_b(mini):
    tile("#4CD472", "#0A7A3A")
    brown = plastic("#8A5A3C", 0.4)
    parts = [rbox((0, -0.08, 0.12), (0.74, 0.5, 0.22), 0.08, brown),
             tube([(-0.12, 0.17, 0.13), (-0.1, 0.32, 0.14), (0.1, 0.32, 0.14), (0.12, 0.17, 0.13)], 0.03,
                  plastic("#5A3A24", 0.4)),
             rbox((0, 0.02, 0.235), (0.76, 0.04, 0.02), 0.01, plastic("#6E4630", 0.45))]
    for x in (-0.22, 0.22):
        parts.append(rbox((x, 0.02, 0.245), (0.09, 0.08, 0.03), 0.015, metal("#F2C14E", 0.25)))
    group(parts, loc=(0, 0.0, 0.02), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


# -- Pixel Art -------------------------------------------------------------

HEART = ["0110110", "1111111", "1111111", "0111110", "0011100", "0001000"]
HEART_MINI = ["01010", "11111", "01110", "00100"]


def pixel_a(mini):
    tile("#E85A88", "#5B2A86")
    pat = HEART_MINI if mini else HEART
    k = 0.8 / (len(pat[0]) + 0.5)
    parts = []
    for r, row in enumerate(pat):
        for c, ch in enumerate(row):
            if ch == "1":
                hl = (r, c) in ((1, 1), (1, 2), (2, 1)) if not mini else (r, c) == (1, 1)
                parts.append(rbox(((c - (len(row) - 1) / 2) * k, ((len(pat) - 1) / 2 - r) * k, k / 2),
                                  (k * 0.92, k * 0.92, k), k * 0.16, glossy("#FFD0DD" if hl else "#FF2D55")))
    group(parts, loc=(0, 0.0, 0.0), rot=(-18, 12, 0), scale=1.0)


def pixel_b(mini):
    tile("#E85A88", "#5B2A86")
    pp = [cyl((0, 0, 0), 0.06, 0.5, glossy("#FFD60A"), rot=(R90, 0, 0), verts=6, bevel=0.014),
          cyl((0, -0.3, 0), 0.059, 0.1, matte("#F2C38C"), r2=0.014, rot=(R90, 0, 0), verts=24),
          cyl((0, -0.35, 0), 0.018, 0.02, plastic("#3A3A3C"), r2=0.003, rot=(R90, 0, 0)),
          cyl((0, 0.28, 0), 0.06, 0.06, plastic("#FF7DA8", 0.5), rot=(R90, 0, 0), bevel=0.02)]
    parts = [group(pp, loc=(0.04, 0.06, 0.12), rot=(0, 0, -40), name="pencil")]
    cubes = ((-0.26, -0.22, "#34C759"), (-0.12, -0.3, "#2D7BF0"), (-0.27, -0.06, "#FF9F0A"))
    for x, y, c in (cubes if not mini else cubes[:2]):
        parts.append(rbox((x, y, 0.06), (0.12, 0.12, 0.12), 0.025, glossy(c)))
    group(parts, loc=(0, 0.0, 0.02), rot=(-16, 10, 0), scale=1.1 if mini else 1.0)


# -- DJ --------------------------------------------------------------------

def dj_a(mini):
    tile("#3DA5FF", "#FF7A3D")
    parts = [rbox((0, 0, 0.05), (0.78, 0.66, 0.1), 0.06, plastic("#E9EDF2", 0.35)),
             cyl((-0.08, 0.0, 0.12), 0.28, 0.04, glossy("#16161A"), verts=96, bevel=0.01),
             cyl((-0.08, 0.0, 0.145), 0.09, 0.012, glossy("#FF7A3D"), verts=64)]
    if not mini:
        for r in (0.16, 0.22):
            parts.append(slab([circle(-0.08, 0, r + 0.004), circle(-0.08, 0, r)], 0.004, 0.0, glossy("#3A3A3C"), z=0.14))
        parts.append(cyl((0.3, 0.22, 0.13), 0.05, 0.05, metal(), bevel=0.015))
    parts.append(tube([(0.3, 0.22, 0.17), (0.3, -0.05, 0.17), (0.12, -0.16, 0.17)], 0.02, metal()))
    parts.append(rbox((0.1, -0.17, 0.17), (0.08, 0.06, 0.04), 0.015, plastic("#2C2C2E")))
    group(parts, loc=(0, 0.0, 0.06), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


def dj_b(mini):
    tile("#3DA5FF", "#FF7A3D")
    parts = [rbox((0, 0, 0.05), (0.62, 0.74, 0.1), 0.06, plastic("#2C2F36", 0.35))]
    for x in (-0.15, 0.15):
        parts += _knob(x, 0.22, 0.12, 0.07, plastic("#E9EDF2"))
        if not mini:
            parts.append(rbox((x, -0.02, 0.105), (0.03, 0.26, 0.01), 0.01, plastic("#111111")))
            parts.append(rbox((x, 0.04, 0.12), (0.1, 0.05, 0.04), 0.015, glossy("#3DA5FF" if x < 0 else "#FF7A3D")))
    parts.append(rbox((0, -0.26, 0.105), (0.4, 0.03, 0.01), 0.01, plastic("#111111")))
    parts.append(rbox((0.04, -0.26, 0.13), (0.08, 0.1, 0.05), 0.02, glossy("#FFFFFF")))
    group(parts, loc=(0, 0.0, 0.06), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


# -- Estudio ---------------------------------------------------------------

def estudio_a(mini):
    tile("#FF7A50", "#7A2BD8")
    parts = [rbox((0, 0.0, 0.05), (0.8, 0.5, 0.1), 0.05, plastic("#2C2F36", 0.35))]
    n = 7 if not mini else 5
    w = 0.7 / n
    for i in range(n):
        parts.append(rbox((-0.35 + w * (i + 0.5), -0.08, 0.11), (w * 0.9, 0.28, 0.04), 0.015, plastic("#FFFFFF", 0.4)))
    for i in range(n - 1):
        if i % 7 in (2, 6) and not mini:
            continue
        parts.append(rbox((-0.35 + w * (i + 1), -0.02, 0.14), (w * 0.55, 0.16, 0.04), 0.012, plastic("#1C1C1E")))
    if not mini:
        for i, c in enumerate(("#FF7A50", "#5EE7FF", "#BF5AF2")):
            parts += _knob(-0.24 + i * 0.12, 0.16, 0.12, 0.04, glossy(c))
    group(parts, loc=(0, 0.0, 0.06), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


def estudio_b(mini):
    tile("#FF7A50", "#7A2BD8")
    n = 3 if mini else 4
    k = 0.6 / n
    parts = [rbox((0, 0, 0.04), (0.74, 0.74, 0.08), 0.07, plastic("#2C2F36", 0.35))]
    cols = ("#FF7A50", "#FFD60A", "#5EE7FF", "#BF5AF2")
    lit = {(0, 1), (1, 3), (2, 0), (3, 2), (1, 0)} if not mini else {(0, 0), (1, 2), (2, 1)}
    for r in range(n):
        for c in range(n):
            x, y = (c - (n - 1) / 2) * (k + 0.02), ((n - 1) / 2 - r) * (k + 0.02)
            parts.append(rbox((x, y, 0.1), (k, k, 0.05), 0.03,
                              glow(cols[r % 4], 1.4) if (r, c) in lit else plastic("#4A505C", 0.4)))
    group(parts, loc=(0, 0.0, 0.06), rot=(-16, 10, 0), scale=1.0)


# -- Radio -----------------------------------------------------------------

def radio_a(mini):
    tile("#FFB347", "#8A4B00")
    parts = [rbox((0, -0.06, 0.1), (0.78, 0.54, 0.2), 0.1, glossy("#FF8A1A")),
             rbox((-0.13, -0.08, 0.2), (0.38, 0.38, 0.03), 0.05, plastic("#F2E2C4", 0.5)),
             tube([(-0.22, 0.2, 0.12), (-0.18, 0.36, 0.14), (0.18, 0.36, 0.14), (0.22, 0.2, 0.12)], 0.025,
                  plastic("#5A3A24"))]
    if not mini:
        for i in range(5):
            parts.append(rbox((-0.13, 0.06 - i * 0.07, 0.22), (0.3, 0.025, 0.01), 0.01, plastic("#B08A60")))
        parts += _knob(0.22, -0.2, 0.2, 0.06, plastic("#5A3A24"))
    parts += [cyl((0.22, 0.02, 0.21), 0.1, 0.03, plastic("#FFF2D6"), verts=48),
              rbox((0.22, 0.05, 0.23), (0.015, 0.1, 0.01), 0.005, glossy("#FF453A"))]
    group(parts, loc=(0, 0.04, 0.0), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


def radio_b(mini):
    tile("#FFB347", "#8A4B00")
    parts = [rbox((0, -0.08, 0.1), (0.5, 0.56, 0.2), 0.08, plastic("#2C2F36", 0.35)),
             cyl((0, -0.12, 0.22), 0.19, 0.05, plastic("#4A505C", 0.5), bevel=0.02, verts=64),
             cyl((0, -0.12, 0.24), 0.11, 0.06, plastic("#1C1C1E", 0.5), r2=0.04, verts=64),
             ball((0, -0.12, 0.27), 0.05, glossy("#FF8A1A")),
             capsule((0.18, 0.16, 0.12), (0.32, 0.42, 0.14), 0.02, metal()),
             ball((0.32, 0.42, 0.14), 0.035, metal())]
    if not mini:
        parts.append(ball((-0.16, 0.12, 0.21), 0.03, glow("#34C759", 2.0)))
    group(parts, loc=(0, 0.0, 0.0), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


# -- Afinador --------------------------------------------------------------

def tuner_a(mini):
    tile("#5FD6EC", "#0A6070")
    steel = metal("#E3E7EC", 0.18)
    parts = [capsule((-0.09, -0.06, 0.1), (-0.09, 0.36, 0.1), 0.045, steel),
             capsule((0.09, -0.06, 0.1), (0.09, 0.36, 0.1), 0.045, steel),
             tube([(-0.09, -0.06, 0.1), (-0.09, -0.16, 0.1), (0.09, -0.16, 0.1), (0.09, -0.06, 0.1)], 0.045, steel),
             capsule((0, -0.18, 0.1), (0, -0.42, 0.1), 0.04, steel)]
    if not mini:
        for i, r in enumerate((0.2, 0.28)):
            from lib import waves
            parts += waves(0, 0.2, glossy("#FFFFFF"), 1, r, 0, 0.035, 20, 70, z=0.06)
            parts += waves(0, 0.2, glossy("#FFFFFF"), 1, r, 0, 0.035, 110, 160, z=0.06)
    group(parts, loc=(0, 0.0, 0.0), rot=(-16, 10, -15), scale=1.1 if mini else 1.0)


def tuner_b(mini):
    tile("#5FD6EC", "#0A6070")
    parts = [rbox((0, -0.05, 0.05), (0.76, 0.56, 0.1), 0.08, plastic("#1F2A33", 0.35))]
    parts.append(slab([arc_band(0, -0.25, 0.3, 0.36, 30, 150)], 0.03, 0.01, plastic("#FFFFFF"), z=0.1))
    parts.append(rbox((0, 0.08, 0.12), (0.04, 0.08, 0.03), 0.015, glow("#5CFF9A", 2.0)))
    nd = rbox((0.03, -0.1, 0.14), (0.03, 0.34, 0.02), 0.012, glossy("#FF453A"))
    nd.rotation_euler.z = math.radians(-8)
    parts += [nd, cyl((0, -0.25, 0.13), 0.04, 0.04, metal(), bevel=0.012)]
    if not mini:
        parts.append(text("A", glow("#5CFF9A", 1.2), size=0.14, depth=0.015, bevel=0.003, c=(-0.25, 0.12, 0.1)))
    group(parts, loc=(0, 0.02, 0.06), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


# -- Video -----------------------------------------------------------------

def video_a(mini):
    tile("#FF6A60", "#8E1A12")
    dark, white = plastic("#2C2C2E", 0.35), plastic("#FFFFFF", 0.4)
    parts = [rbox((0, -0.1, 0.06), (0.7, 0.44, 0.1), 0.04, dark)]
    if not mini:
        for i in range(3):
            parts.append(rbox((-0.2, -0.04 - i * 0.1, 0.115), (0.22, 0.03, 0.01), 0.01, plastic("#8E93A0")))
    top = [rbox((0, 0, 0), (0.7, 0.13, 0.08), 0.03, dark)]
    for i in range(4):
        st = rbox((-0.24 + i * 0.16, 0, 0.042), (0.07, 0.13, 0.01), 0.005, white)
        st.rotation_euler.z = math.radians(35)
        top.append(st)
    parts.append(group(top, loc=(0, 0.2, 0.12), rot=(0, 0, 12), name="clap"))
    group(parts, loc=(0, 0.02, 0.04), rot=(-16, 10, 0), scale=1.1 if mini else 1.0)


def video_b(mini):
    tile("#FF6A60", "#8E1A12")
    parts = [rbox((0, -0.06, 0.08), (0.78, 0.58, 0.16), 0.1, glossy("#FFFFFF")),
             rbox((-0.05, -0.06, 0.16), (0.56, 0.44, 0.03), 0.06, glossy("#1C2A44")),
             slab([rounded_poly([(-0.12, 0.06), (0.06, -0.06), (-0.12, -0.18)], 0.025)], 0.04, 0.012,
                  glow("#FFFFFF", 0.6), z=0.17)]
    for p in parts[-1:]:
        p.location.x, p.location.y = 0.0, 0.0
    if not mini:
        parts += [capsule((-0.04, 0.24, 0.1), (-0.2, 0.42, 0.12), 0.015, metal()),
                  capsule((0.04, 0.24, 0.1), (0.2, 0.42, 0.12), 0.015, metal()),
                  cyl((0.3, 0.08, 0.17), 0.035, 0.03, plastic("#2C2C2E")),
                  cyl((0.3, -0.06, 0.17), 0.035, 0.03, plastic("#2C2C2E"))]
    group(parts, loc=(0, 0.0, 0.02), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


# -- Grabadora -------------------------------------------------------------

def recorder_a(mini):
    tile("#FF6A60", "#7A1512")
    mesh = metal("#C9CED6", 0.35)
    parts = [ball((0, 0.16, 0.14), 0.19, mesh, scale=(1, 1.15, 1)),
             cyl((0, -0.06, 0.14), 0.15, 0.2, glossy("#2C2C2E"), rot=(R90, 0, 0), bevel=0.04),
             capsule((0, -0.18, 0.1), (0, -0.4, 0.06), 0.035, metal()),
             rbox((0, -0.42, 0.04), (0.3, 0.06, 0.04), 0.025, glossy("#2C2C2E"))]
    if not mini:
        for i in range(4):
            parts.append(cyl((0, 0.04 + i * 0.07, 0.14), 0.2 if i in (1, 2) else 0.17, 0.008, metal("#8E93A0"),
                             rot=(R90, 0, 0)))
        parts.append(ball((0, -0.06, 0.29), 0.025, glow("#FF3B30", 2.5)))
    group(parts, loc=(0, 0.02, 0.0), rot=(-16, 10, -10), scale=1.1 if mini else 1.0)


def recorder_b(mini):
    tile("#FF6A60", "#7A1512")
    parts = [rbox((0, 0, 0.05), (0.8, 0.52, 0.1), 0.05, plastic("#2C2C2E", 0.35)),
             rbox((0, 0.06, 0.105), (0.66, 0.3, 0.02), 0.03, plastic("#FFF2D6", 0.5)),
             rbox((0, 0.03, 0.12), (0.36, 0.14, 0.02), 0.03, glossy("#1C1C1E"))]
    for x in (-0.11, 0.11):
        parts.append(cyl((x, 0.03, 0.13), 0.05, 0.02, plastic("#FFFFFF"), verts=6))
    if not mini:
        parts.append(rbox((0, 0.17, 0.12), (0.5, 0.03, 0.01), 0.008, glossy("#FF453A")))
        parts.append(rbox((0, -0.2, 0.11), (0.44, 0.08, 0.03), 0.02, plastic("#3A3A3C")))
    group(parts, loc=(0, 0.0, 0.06), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


MODELS = {
    "aos.dibujo": {"a": dibujo_a, "b": dibujo_b},
    "aos.notas": {"a": notas_a, "b": notas_b},
    "aos.oficina": {"a": oficina_a, "b": oficina_b},
    "aos.pixel": {"a": pixel_a, "b": pixel_b},
    "aos.dj": {"a": dj_a, "b": dj_b},
    "aos.estudio": {"a": estudio_a, "b": estudio_b},
    "aos.radio": {"a": radio_a, "b": radio_b},
    "aos.tuner": {"a": tuner_a, "b": tuner_b},
    "aos.video": {"a": video_a, "b": video_b},
    "app.recorder": {"a": recorder_a, "b": recorder_b},
}
