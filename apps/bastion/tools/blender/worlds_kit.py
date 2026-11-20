"""
BASTION - what every world shares (SPEC.md, "Worlds"): the road, the pads
and the fort's foundation, built exactly where ../bsworlds.py puts them,
because the game reads the same numbers. worlds_a.py and worlds_b.py build
the rest of each world around them.

    import worlds_kit as K
    w = K.world('luna')
    K.road(w, road_mat, edge_mat)        # the ribbon at Z = 0, with kerbs
    K.pads(w, plate_mat)                 # hexagonal pads, rim lit in w.accent
    K.foundation(w, plate_mat)           # the round plate the fort stands on
    K.clear(w, x, y, r)                  # True if a prop of radius r may stand there
"""
import math
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..'))
import bs_common as B  # noqa: E402
import bsworlds  # noqa: E402


def world(wid):
    return bsworlds.BY_ID[wid]


def _normals(p):
    out = []
    for i in range(len(p)):
        a, b = p[max(0, i - 1)], p[min(len(p) - 1, i + 1)]
        dx, dy = b[0] - a[0], b[1] - a[1]
        l = math.hypot(dx, dy) or 1.0
        out.append((-dy / l, dx / l))
    return out


def ribbon(p, half, z, m, z_side=None, name='road'):
    """a strip along the dense path p, 'half' wide each side, its top at z;
    with z_side it also gets walls down to that height"""
    n = _normals(p)
    verts, faces = [], []
    for (x, y), (nx, ny) in zip(p, n):
        verts.append((x + nx * half, y + ny * half, z))
        verts.append((x - nx * half, y - ny * half, z))
    for i in range(len(p) - 1):
        faces.append((2 * i, 2 * i + 1, 2 * i + 3, 2 * i + 2))
    if z_side is not None:
        k = len(verts)
        for (x, y), (nx, ny) in zip(p, n):
            verts.append((x + nx * half, y + ny * half, z_side))
            verts.append((x - nx * half, y - ny * half, z_side))
        for i in range(len(p) - 1):
            faces.append((2 * i, 2 * i + 2, k + 2 * i + 2, k + 2 * i))
            faces.append((2 * i + 1, k + 2 * i + 1, k + 2 * i + 3, 2 * i + 3))
    return B.mesh(verts, faces, m, name=name)


def road(w, road_mat, edge_mat=None, edge_w=0.14, edge_h=0.05, lift=0.004):
    """every road of the world: a flat ribbon PATH_W wide just above Z = 0
    (so it wins over the ground under it), with a thin kerb each side"""
    half = bsworlds.PATH_W / 2
    for i, p in enumerate(w.paths):
        ribbon(p, half, lift, road_mat, name='road%d' % i)
        if edge_mat is not None:
            n = _normals(p)
            for s in (1, -1):
                q = [(x + nx * s * (half + edge_w / 2), y + ny * s * (half + edge_w / 2)) for (x, y), (nx, ny) in zip(p, n)]
                ribbon(q, edge_w / 2, edge_h, edge_mat, z_side=0.0, name='kerb')


def pads(w, plate_mat, rim_k=4.0, inner_mat=None):
    """the build pads: a hexagon of radius 0.95 whose top is Z = 0.01, a
    lit rim in the world's accent, a darker centre"""
    rim = B.glow('pad_rim_' + w.id, w.accent, rim_k)
    for (x, y) in w.pads:
        g = B.group('pad', loc=(x, y, 0))
        B.prism(B.ngon(6, bsworlds.PAD_R), -0.20, 0.010, plate_mat, g, bevel=0.0)
        B.prism(B.ngon(6, bsworlds.PAD_R * 0.80), 0.010, 0.018, inner_mat or plate_mat, g, bevel=0.0)
        hexa = B.ngon(6, bsworlds.PAD_R * 0.90)
        for i in range(6):
            a, b = hexa[i], hexa[(i + 1) % 6]
            mx, my = (a[0] + b[0]) / 2, (a[1] + b[1]) / 2
            ang = math.degrees(math.atan2(b[1] - a[1], b[0] - a[0]))
            B.box((math.hypot(b[0] - a[0], b[1] - a[1]) * 0.72, 0.05, 0.02), rim, g, loc=(mx, my, 0.022), rot=(0, 0, ang),
                  bevel=0.0)


def foundation(w, plate_mat, rim_k=3.0):
    """the round plate the fort's sprite stands on, top at Z = 0.01"""
    x, y = w.fort
    g = B.group('found', loc=(x, y, 0))
    B.cyl(bsworlds.FORT_R, 0.21, plate_mat, g, loc=(0, 0, -0.20), segs=48)
    B.torus(bsworlds.FORT_R * 0.93, 0.03, B.glow('found_rim_' + w.id, w.accent, rim_k), g, loc=(0, 0, 0.015), segs=64)


def clear(w, x, y, r=0.5, road_gap=1.6):
    """may something of radius r stand at (x, y)? Not on the road (nor
    within road_gap of its centre), a pad or the fort's plate"""
    if w.dist_path(x, y) < road_gap + r:
        return False
    for (px, py) in w.pads:
        if math.hypot(x - px, y - py) < bsworlds.PAD_R + 0.25 + r:
            return False
    if math.hypot(x - w.fort[0], y - w.fort[1]) < bsworlds.FORT_R + 0.3 + r:
        return False
    return True
