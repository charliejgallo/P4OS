"""'juguete' models: the groups utiles-programar and pruebas of apps.json."""

import math

import bpy
import bmesh

from lib import (ball, blob, capsule, circle, cyl, glossy, glow, group, link, matte, metal, plastic, rbox,
                 rounded_poly, rrect, setmat, slab, star, text, tile, tube)

R90 = math.radians(90)


def crescent(R, r, dx, dy, n=96):
    """The part of a circle (R, at the origin) outside another (r, at dx, dy)."""
    out_pts = [(R * math.cos(2 * math.pi * i / n), R * math.sin(2 * math.pi * i / n)) for i in range(n)]
    keep = [(x - dx) ** 2 + (y - dy) ** 2 > r * r for x, y in out_pts]
    start = next(i for i in range(n) if keep[i] and not keep[i - 1])
    arc = []
    i = start
    while keep[i % n]:
        arc.append(out_pts[i % n])
        i += 1
    in_pts = [(dx + r * math.cos(2 * math.pi * k / n), dy + r * math.sin(2 * math.pi * k / n)) for k in range(n)]
    inside = [x * x + y * y < R * R for x, y in in_pts]
    k0 = next(k for k in range(n) if inside[k] and not inside[(k + 1) % n])
    back = []
    k = k0
    while inside[k % n]:
        back.append(in_pts[k % n])
        k -= 1
    return arc + back


def bubble(x, y, s, m, z=0.04, tail_left=True):
    t = -1 if tail_left else 1
    pts = [(x - 0.2 * s, y - 0.12 * s), (x + t * 0.06 * s, y - 0.12 * s), (x + t * 0.14 * s, y - 0.24 * s),
           (x + t * 0.14 * s, y - 0.12 * s), (x + 0.2 * s, y - 0.12 * s), (x + 0.2 * s, y + 0.12 * s),
           (x - 0.2 * s, y + 0.12 * s)]
    if not tail_left:
        pts = [pts[0], pts[3], pts[2], pts[1], pts[4], pts[5], pts[6]]
        pts = [(x - 0.2 * s, y - 0.12 * s), (x - 0.02 * s, y - 0.12 * s), (x + 0.1 * s, y - 0.24 * s),
               (x + 0.06 * s, y - 0.12 * s), (x + 0.2 * s, y - 0.12 * s), (x + 0.2 * s, y + 0.12 * s),
               (x - 0.2 * s, y + 0.12 * s)]
    return slab([rounded_poly(pts, [0.06 * s, 0.01, 0.01, 0.01, 0.06 * s, 0.06 * s, 0.06 * s])], 0.08, 0.025, m, z=z)


# -- Bolsa -----------------------------------------------------------------

def _arrow_up(x, y, s, m, z):
    pts = [(-0.05, -0.2), (0.05, -0.2), (0.05, 0.02), (0.13, 0.02), (0, 0.2), (-0.13, 0.02), (-0.05, 0.02)]
    return slab([rounded_poly([(x + px * s, y + py * s) for px, py in pts], 0.02)], 0.08, 0.025, m, z=z)


def bolsa_a(mini):
    tile("#3A6A9E", "#0A1424")
    parts = []
    for i, h in enumerate((0.2, 0.34, 0.5)):
        parts.append(rbox((-0.22 + i * 0.2, -0.36 + h / 2, 0.08), (0.15, h, 0.12), 0.04, glossy("#34C759")))
    ar = _arrow_up(0.0, 0.0, 1.0, glossy("#FFD60A"), 0.16)
    ar.rotation_euler.z = math.radians(-40)
    ar.location.x, ar.location.y = 0.04, 0.16
    parts.append(ar)
    group(parts, loc=(0, 0.02, 0.02), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


def bolsa_b(mini):
    tile("#3A6A9E", "#0A1424")
    parts = []
    for i, h in enumerate((0.24, 0.4, 0.56)):
        parts.append(rbox((-0.24 + i * 0.16, -0.3 + h / 2, 0.05), (0.12, h, 0.08), 0.03, glossy("#34C759")))
    gold = metal("#F2C14E", 0.22)
    parts += [cyl((0.12, -0.12, 0.16), 0.22, 0.08, gold, bevel=0.03, verts=96),
              text("$", plastic("#B8860B", 0.3), size=0.26, depth=0.03, bevel=0.008, c=(0.12, -0.12, 0.19))]
    group(parts, loc=(0, 0.02, 0.02), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


# -- CAS -------------------------------------------------------------------

def cas_a(mini):
    tile("#7A78F0", "#2B2A7A")
    parts = [text("f(x)", glossy("#FFFFFF"), size=0.38 if not mini else 0.42, depth=0.1, bevel=0.02, c=(0, 0.02, 0.02))]
    group(parts, loc=(0, 0, 0.02), rot=(-18, 12, 0), scale=1.0)


def cas_b(mini):
    tile("#7A78F0", "#2B2A7A")
    parts = [rbox((0, 0.02, 0.04), (0.8, 0.6, 0.08), 0.05, plastic("#8A5A3C", 0.45)),
             rbox((0, 0.02, 0.07), (0.7, 0.5, 0.04), 0.03, matte("#1F5A3E")),
             text("x", plastic("#FFFFFF", 0.5), size=0.32, depth=0.02, bevel=0.004, c=(-0.08, -0.02, 0.09)),
             text("2", plastic("#FFFFFF", 0.5), size=0.16, depth=0.02, bevel=0.004, c=(0.08, 0.1, 0.09))]
    if not mini:
        parts.append(rbox((0.24, -0.28, 0.1), (0.12, 0.04, 0.04), 0.015, plastic("#FFFFFF")))
    group(parts, loc=(0, 0, 0.06), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


# -- Clima -----------------------------------------------------------------

def clima_a(mini):
    tile("#45A6FF", "#0A57D0")
    sunm = glow("#FFC21A", 0.35)
    parts = [ball((0.12, 0.15, 0.05), 0.18, sunm)]
    if not mini:
        for i in range(8):
            a = math.radians(i * 45)
            parts.append(cyl((0.12 + 0.265 * math.cos(a), 0.15 + 0.265 * math.sin(a), 0.06), 0.03, 0.07, sunm,
                             rot=(0, R90, a), bevel=0.02))
    parts.append(blob([(-0.2, -0.12, 0.15, 0.19), (-0.04, -0.02, 0.15, 0.25), (0.15, -0.1, 0.15, 0.2),
                       (0.27, -0.16, 0.15, 0.14), (-0.3, -0.17, 0.15, 0.12), (0.04, -0.17, 0.15, 0.19)],
                      plastic("#FFFFFF", 0.45), scale=(1, 1, 0.7)))
    group(parts, loc=(0, 0, 0), rot=(-8, 0, 0), scale=1.15 if mini else 1.0)


def clima_b(mini):
    tile("#45A6FF", "#0A57D0")
    R = 0.38
    top = [(R * math.cos(math.radians(a)), 0.02 + R * 0.8 * math.sin(math.radians(a))) for a in range(0, 181, 10)]
    bottom = []
    for i in range(4):                       # four scallops along the rim, right to left
        x0 = -R + i * (2 * R / 4)
        for k in range(1, 7):
            t = k / 6
            bottom.append((x0 + t * (2 * R / 4), 0.02 + 0.05 * math.sin(t * math.pi)))
    canopy = top + bottom[:-1]
    parts = [slab([canopy], 0.12, 0.04, glossy("#FF453A"), z=0.08),
             tube([(0, 0.0, 0.12), (0, -0.28, 0.12), (0, -0.34, 0.12), (0.09, -0.34, 0.12), (0.09, -0.27, 0.12)],
                  0.028, plastic("#2C2C2E")),
             ball((0, 0.33, 0.14), 0.035, plastic("#2C2C2E"))]
    if not mini:
        for x, y in ((-0.3, -0.18), (0.3, -0.26), (-0.18, -0.36), (0.24, -0.08)):
            parts.append(ball((x, y, 0.08), 0.035, glossy("#D6ECFF"), scale=(1, 1.5, 1)))
    group(parts, loc=(0, 0.02, 0.04), rot=(-16, 10, 0), scale=1.08 if mini else 1.0)


# -- Cotizaciones ----------------------------------------------------------

def cotiz_a(mini):
    tile("#4CDC72", "#0B5227")
    parts = []
    for i, rz in enumerate((-12, 6)):
        b = rbox((-0.04, 0.06 - i * 0.04, 0.04 + i * 0.03), (0.62, 0.34, 0.025), 0.015, plastic("#9BE3A6", 0.5))
        b.rotation_euler.z = math.radians(rz)
        parts.append(b)
        if not mini:
            c = cyl((-0.04, 0.06 - i * 0.04, 0.055 + i * 0.03), 0.08, 0.01, plastic("#5BB86A", 0.5))
            parts.append(c)
    gold = metal("#F2C14E", 0.22)
    parts += [cyl((0.2, -0.2, 0.14), 0.17, 0.06, gold, bevel=0.025, verts=96),
              text("$", plastic("#B8860B", 0.3), size=0.2, depth=0.025, bevel=0.006, c=(0.2, -0.2, 0.165))]
    group(parts, loc=(0, 0.02, 0.04), rot=(-16, 10, 0), scale=1.08 if mini else 1.0)


def cotiz_b(mini):
    tile("#4CDC72", "#0B5227")
    gold = metal("#F2C14E", 0.2)
    parts = [cyl((0, 0, 0.08), 0.34, 0.12, gold, bevel=0.04, verts=96),
             slab([circle(0, 0, 0.29), circle(0, 0, 0.26)], 0.02, 0.006, metal("#D9A53A", 0.25), z=0.14),
             text("$", plastic("#B8860B", 0.3), size=0.4, depth=0.04, bevel=0.01, c=(0, 0, 0.14))]
    group(parts, loc=(0, 0, 0.06), rot=(-18, 14, 0), scale=1.1 if mini else 1.0)


# -- Pato goma -------------------------------------------------------------

def _duck(x, y, s, mini):
    yel = glossy("#FFD60A")
    parts = [blob([(x - 0.06 * s, y - 0.08 * s, 0.12 * s, 0.26 * s), (x + 0.12 * s, y - 0.06 * s, 0.12 * s, 0.2 * s),
                   (x - 0.2 * s, y - 0.02 * s, 0.14 * s, 0.14 * s)], yel, scale=(1, 0.85, 0.75)),
             ball((x + 0.1 * s, y + 0.14 * s, 0.2 * s), 0.14 * s, yel),
             ball((x + 0.22 * s, y + 0.12 * s, 0.2 * s), 0.07 * s, glossy("#FF8A00"), scale=(1.4, 0.8, 0.6)),
             ball((x + 0.13 * s, y + 0.2 * s, 0.3 * s), 0.028 * s, plastic("#1C1C1E"))]
    return parts


def pato_a(mini):
    tile("#3FA0FF", "#0050A0")
    group(_duck(0, 0, 1.15, mini), loc=(-0.02, -0.02, 0.0), rot=(-12, 8, 0), scale=1.1 if mini else 1.0)


def pato_b(mini):
    tile("#3FA0FF", "#0050A0")
    parts = _duck(-0.06, 0.08, 0.85, mini)
    usb = [rbox((0.2, -0.24, 0.06), (0.36, 0.14, 0.08), 0.04, plastic("#FFFFFF", 0.35)),
           rbox((0.42, -0.24, 0.06), (0.12, 0.1, 0.05), 0.012, metal())]
    if not mini:
        usb.append(rbox((0.12, -0.24, 0.105), (0.08, 0.06, 0.01), 0.01, glossy("#2D7BF0")))
    group(parts + usb, loc=(0, 0, 0), rot=(-14, 8, 0), scale=1.05 if mini else 1.0)


# -- Traductor -------------------------------------------------------------

def traductor_a(mini):
    tile("#3A6390", "#0B1622")
    s = 1.0 if not mini else 1.1
    parts = [bubble(-0.13, 0.13, s, glossy("#FFFFFF"), z=0.04),
             text("A", plastic("#2D7BF0", 0.35), size=0.22 * s, depth=0.03, bevel=0.006, c=(-0.13, 0.13, 0.11)),
             bubble(0.13, -0.13, s, glossy("#FFD60A"), z=0.12, tail_left=False),
             text("Z", plastic("#5A3A10", 0.35), size=0.22 * s, depth=0.03, bevel=0.006, c=(0.13, -0.13, 0.19))]
    group(parts, loc=(0, 0, 0.02), rot=(-16, 10, 0), scale=1.0)


def traductor_b(mini):
    tile("#3A6390", "#0B1622")
    pages = []
    for s in (-1, 1):
        p = rbox((s * 0.17, -0.12, 0.06), (0.34, 0.44, 0.03), 0.02, plastic("#FFFFFF", 0.45))
        p.rotation_euler.y = math.radians(-s * 10)
        pages.append(p)
        if not mini:
            for i in range(3):
                ln = rbox((s * 0.17, -0.04 - i * 0.09, 0.085), (0.22, 0.025, 0.01), 0.01, plastic("#C3C8D1"))
                ln.rotation_euler.y = math.radians(-s * 10)
                pages.append(ln)
    pages.append(rbox((0, -0.12, 0.03), (0.74, 0.48, 0.03), 0.02, glossy("#D9261C")))
    b = bubble(0.08, 0.24, 0.8, glossy("#FFD60A"), z=0.12)
    t = text("A", plastic("#5A3A10", 0.35), size=0.16, depth=0.025, bevel=0.005, c=(0.08, 0.24, 0.18))
    group(pages + [b, t], loc=(0, 0.02, 0.02), rot=(-16, 10, 0), scale=1.08 if mini else 1.0)


# -- Mapas -----------------------------------------------------------------

def _pin(x, y, z, s, m):
    return [ball((x, y + 0.12 * s, z + 0.1 * s), 0.13 * s, m),
            cyl((x, y - 0.04 * s, z + 0.1 * s), 0.1 * s, 0.22 * s, m, rot=(R90, 0, 0), r2=0.01),
            ball((x, y + 0.12 * s, z + 0.22 * s), 0.05 * s, plastic("#FFFFFF"))]


def mapas_a(mini):
    tile("#3A9A62", "#0B2418")
    parts = []
    cols = ("#DCEFD0", "#BFE3F7", "#DCEFD0")
    for i, c in enumerate(cols):
        p = rbox((-0.24 + i * 0.24, -0.04, 0.08), (0.25, 0.6, 0.025), 0.012, plastic(c, 0.5))
        p.rotation_euler.y = math.radians(14 if i % 2 == 0 else -14)
        parts.append(p)
    if not mini:
        parts.append(tube([(-0.32, -0.2, 0.11), (-0.1, 0.0, 0.11), (0.1, -0.12, 0.11), (0.3, 0.14, 0.11)], 0.015,
                          glossy("#FF9F0A")))
    parts += _pin(0.1, 0.06, 0.1, 0.9 if not mini else 1.1, glossy("#FF3B30"))
    group(parts, loc=(0, 0.0, 0.04), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


def mapas_b(mini):
    tile("#3A9A62", "#0B2418")
    parts = _pin(0, -0.05, 0.02, 1.7, glossy("#FF3B30"))
    parts.append(ball((0, -0.38, 0.02), 0.12, matte("#0B2418"), scale=(1, 0.5, 0.1)))
    group(parts, loc=(0, 0.0, 0.0), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


# -- Visor 3D --------------------------------------------------------------

def visor3d_a(mini):
    tile("#3A64A8", "#0A1428")
    e = 0.22
    w = plastic("#FFFFFF", 0.35)
    corners = [(x, y, z) for x in (-e, e) for y in (-e, e) for z in (-e, e)]
    parts = []
    for a in corners:
        for b in corners:
            if a < b and sum(abs(p - q) > 1e-6 for p, q in zip(a, b)) == 1:
                parts.append(capsule(a, b, 0.025 if not mini else 0.035, w))
    if not mini:
        for d, c in (((0.38, 0, 0), "#FF453A"), ((0, 0.38, 0), "#34C759"), ((0, 0, 0.38), "#2D7BF0")):
            parts.append(capsule((-e, -e, -e), tuple(-e + v for v in d), 0.03, glossy(c)))
    root = group(parts, loc=(0, 0, 0.3), scale=1.0)
    from lib import turn
    turn(root, ("Z", 35), ("X", -55))


def visor3d_b(mini):
    tile("#3A64A8", "#0A1428")
    bm = bmesh.new()
    bmesh.ops.create_icosphere(bm, subdivisions=1, radius=0.32)
    me = bpy.data.meshes.new("ico")
    bm.to_mesh(me)
    ob = link(bpy.data.objects.new("ico", me))
    md = ob.modifiers.new("bev", "BEVEL")
    md.width = 0.02
    md.segments = 3
    setmat(ob, glossy("#FF9F0A"))
    ob.location = (0, 0, 0.3)
    ob.rotation_euler = (0.4, 0.3, 0.2)


# -- Lua -------------------------------------------------------------------

def lua_a(mini):
    tile("#4A4CB8", "#10105A")
    moon = slab([crescent(0.3, 0.26, 0.15, 0.1)], 0.12, 0.04, glossy("#FFE680"))
    parts = [moon]
    st = slab([rounded_poly(star(0.18, 0.2, 0.1, 0.045), 0.012)], 0.05, 0.015, glow("#FFFFFF", 0.8), z=0.04)
    parts.append(st)
    if not mini:
        parts.append(slab([rounded_poly(star(0.3, -0.12, 0.05, 0.022), 0.006)], 0.04, 0.01, glow("#FFFFFF", 0.8),
                          z=0.04))
    group(parts, loc=(-0.04, 0.0, 0.02), rot=(-16, 10, 0), scale=1.15 if mini else 1.05)


def lua_b(mini):
    tile("#4A4CB8", "#10105A")
    paper = plastic("#F6E7C1", 0.5)
    parts = [rbox((0, 0, 0.05), (0.56, 0.62, 0.03), 0.015, paper),
             cyl((0, 0.32, 0.08), 0.06, 0.66, paper, rot=(0, R90, 0), bevel=0.02),
             cyl((0, -0.32, 0.08), 0.06, 0.66, paper, rot=(0, R90, 0), bevel=0.02)]
    rows = ((0.16, 0.3, "#2D7BF0"), (0.06, 0.38, "#BF5AF2"), (-0.04, 0.24, "#34C759"), (-0.14, 0.34, "#2D7BF0"))
    for i, (y, w, c) in enumerate(rows if not mini else rows[::2]):
        x0 = -0.2 + (0.06 if i in (1, 2) else 0)
        parts.append(rbox((x0 + w / 2, y, 0.07), (w, 0.045 if not mini else 0.07, 0.015), 0.015, glossy(c)))
    group(parts, loc=(0, 0, 0.04), rot=(-16, 10, 0), scale=1.08 if mini else 1.0)


# -- Python ----------------------------------------------------------------

def python_a(mini):
    tile("#4F90CC", "#1E4A75")
    green = glossy("#5BD45A")
    body = [(-0.3, -0.34, 0.08), (0.2, -0.3, 0.09), (0.28, -0.08, 0.1), (-0.22, 0.0, 0.1), (-0.24, 0.2, 0.11),
            (0.06, 0.22, 0.12)]
    parts = [tube(body, 0.075 if not mini else 0.09, green, order=4),
             ball((0.12, 0.24, 0.14), 0.13, green, scale=(1.2, 1, 0.85))]
    for x in (0.08, 0.18):
        parts += [ball((x, 0.3, 0.22), 0.04, plastic("#FFFFFF")), ball((x + 0.01, 0.31, 0.25), 0.02, plastic("#1C1C1E"))]
    if not mini:
        parts.append(capsule((0.26, 0.2, 0.14), (0.36, 0.18, 0.14), 0.012, glossy("#FF453A")))
        for p in ((-0.06, -0.33), (0.18, -0.24), (-0.1, 0.02)):
            parts.append(ball((p[0], p[1], 0.17), 0.03, glossy("#FFD60A"), scale=(1, 1, 0.4)))
    group(parts, loc=(0, 0.02, 0.0), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


def python_b(mini):
    tile("#4F90CC", "#1E4A75")
    parts = [text(">>>", glossy("#FFD60A"), size=0.34 if not mini else 0.4, depth=0.1, bevel=0.02, c=(0, 0.02, 0.02))]
    group(parts, loc=(0, 0, 0.02), rot=(-18, 12, 0), scale=1.0)


# -- Tortuga ---------------------------------------------------------------

def _turtle(x, y, s, mini):
    shell, skin = glossy("#3FA34D"), plastic("#9BE07A", 0.4)
    parts = [ball((x, y, 0.1 * s), 0.25 * s, shell, scale=(1, 1.1, 0.6))]
    if not mini:
        for dx, dy in ((0, 0), (-0.11, 0.08), (0.11, 0.08), (-0.11, -0.1), (0.11, -0.1), (0, 0.17), (0, -0.18)):
            parts.append(ball((x + dx * s, y + dy * s, 0.22 * s), 0.055 * s, plastic("#2E7D32", 0.4),
                              scale=(1, 1, 0.35)))
    parts.append(ball((x, y + 0.33 * s, 0.1 * s), 0.11 * s, skin))
    for ex in (-0.04, 0.04):
        parts.append(ball((x + ex * s, y + 0.39 * s, 0.17 * s), 0.022 * s, plastic("#1C1C1E")))
    for lx, ly in ((-0.22, 0.16), (0.22, 0.16), (-0.22, -0.18), (0.22, -0.18)):
        parts.append(ball((x + lx * s, y + ly * s, 0.06 * s), 0.08 * s, skin, scale=(1, 1.2, 0.7)))
    return parts


def tortuga_a(mini):
    tile("#7CCB5A", "#2E7D32")
    group(_turtle(0, -0.04, 1.05, mini), loc=(0, 0, 0), rot=(-14, 8, -20), scale=1.1 if mini else 1.0)


def tortuga_b(mini):
    tile("#7CCB5A", "#2E7D32")
    parts = _turtle(0.12, 0.08, 0.7, True)
    blk = [rbox((-0.18, -0.24, 0.07), (0.36, 0.2, 0.12), 0.04, glossy("#FF9F0A")),
           rbox((-0.26, -0.13, 0.07), (0.1, 0.05, 0.12), 0.02, glossy("#FF9F0A"))]
    if not mini:
        blk.append(text("▶", plastic("#FFFFFF"), size=0.1, depth=0.02, bevel=0.004, c=(-0.18, -0.24, 0.13)))
    group(parts + blk, loc=(0, 0, 0), rot=(-14, 8, 0), scale=1.08 if mini else 1.0)


# -- Hola ------------------------------------------------------------------

def hello_a(mini):
    tile("#3FA0FF", "#0050A0")
    s = 1.6
    parts = [bubble(0, 0.06, s, glossy("#FFFFFF"), z=0.03),
             text("Hola" if not mini else "Hi", plastic("#0A84FF", 0.35), size=0.2 if not mini else 0.26, depth=0.03,
                  bevel=0.006, c=(0, 0.06, 0.1))]
    group(parts, loc=(0, 0.02, 0.04), rot=(-16, 10, 0), scale=1.0)


# -- Blit test -------------------------------------------------------------

def blittest_a(mini):
    tile("#8A8A90", "#2C2C2E")
    cols = ("#FFFFFF", "#FFD60A", "#40E0F0", "#34C759", "#FF2DFF", "#FF3B30", "#2D5BFF")
    if mini:
        cols = cols[1::2]
    w = 0.66 / len(cols)
    parts = [rbox((-0.33 + w * (i + 0.5), 0.0, 0.06), (w * 0.9, 0.6, 0.1), 0.02, glossy(c)) for i, c in enumerate(cols)]
    group(parts, loc=(0, 0, 0.06), rot=(-16, 10, 0), scale=1.0)


MODELS = {
    "aos.bolsa": {"a": bolsa_a, "b": bolsa_b},
    "aos.cas": {"a": cas_a, "b": cas_b},
    "aos.clima": {"a": clima_a, "b": clima_b},
    "aos.cotiz": {"a": cotiz_a, "b": cotiz_b},
    "aos.pato": {"a": pato_a, "b": pato_b},
    "aos.traductor": {"a": traductor_a, "b": traductor_b},
    "demo.mapas": {"a": mapas_a, "b": mapas_b},
    "demo.visor3d": {"a": visor3d_a, "b": visor3d_b},
    "aos.lua": {"a": lua_a, "b": lua_b},
    "aos.python": {"a": python_a, "b": python_b},
    "demo.tortuga": {"a": tortuga_a, "b": tortuga_b},
    "demo.hello": {"a": hello_a},
    "dev.blittest": {"a": blittest_a},
}
