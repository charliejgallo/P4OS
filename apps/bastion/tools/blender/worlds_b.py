"""
BASTION - the second five worlds (SPEC.md, "Worlds"): crystal dunes, the
toxic swamp, the platforms over a nebula, the hive and the dark core.

    /Applications/Blender.app/Contents/MacOS/Blender -b -P worlds_b.py -- [--only cristal,nucleo] [--samples 96]
        [--out ../../assets/worlds]

Each world is a function build_<id>(w) that fills the scene around the
road, the pads and the fort's plate that worlds_kit puts where
../bsworlds.py says. Nothing here copies a coordinate of a layout: the
ground flattens itself around whatever road and pads the layout has when
the script runs, the props look for room with K.clear() and the voids of
the floating worlds open wherever the layout leaves the board empty.

What this file adds to the kit, because five worlds wanted it:

  Field    the board as a grid of heights: how far every point is from the
           road, a pad or the fort, a relief that flattens itself near
           them, colours and glow painted per vertex, and the mesh of it
           (whole, or cut into islands with rock under their edges)
  Batch    hundreds of small things (crystals, rocks, pods) as one mesh,
           each with its own colour
  N, ramp, mix, math_  a short way of writing node materials
  sites    where a tall prop may stand without its picture covering the
           road or a pad from the game's camera
  backdrop a picture painted with numpy, hung far behind the ground and
           facing the camera (the nebula, the black hole)
"""
import math
import os
import random
import sys
import time

import bpy
import bmesh
import numpy as np
from mathutils import Matrix, Vector

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bs_common as B  # noqa: E402
import worlds_kit as K  # noqa: E402
import bsworlds  # noqa: E402

HALF = bsworlds.PATH_W / 2
PAD_R = bsworlds.PAD_R
FORT_R = bsworlds.FORT_R
BASE = -0.02            # the ground under the road and the pads: just below them
HUD_TOP_Y = 37.0        # the ground rows the HUD covers (SPEC: top 96, bottom 190 px)
HUD_BOT_Y = 5.95


# ---------------------------------------------------------------------------
# Noise with numpy (so a relief is the same on every run)
# ---------------------------------------------------------------------------

def _hash(ix, iy, seed):
    n = (ix * 73856093) ^ (iy * 19349663) ^ (seed * 83492791 + 1013)
    n = ((n ^ (n >> 13)) * 1274126177) & 0x7fffffff
    n = ((n ^ (n >> 15)) * 668265263) & 0x7fffffff
    return n


def pnoise(x, y, seed=0):
    """gradient noise, about 0..1 around 0.5"""
    x = np.asarray(x, np.float64)
    y = np.asarray(y, np.float64)
    xi = np.floor(x).astype(np.int64)
    yi = np.floor(y).astype(np.int64)
    fx, fy = x - xi, y - yi
    u = fx * fx * fx * (fx * (fx * 6 - 15) + 10)
    v = fy * fy * fy * (fy * (fy * 6 - 15) + 10)

    def g(ix, iy, dx, dy):
        a = _hash(ix, iy, seed) * (2 * math.pi / 0x7fffffff)
        return np.cos(a) * dx + np.sin(a) * dy
    a = g(xi, yi, fx, fy)
    b = g(xi + 1, yi, fx - 1, fy)
    c = g(xi, yi + 1, fx, fy - 1)
    d = g(xi + 1, yi + 1, fx - 1, fy - 1)
    n = (a + (b - a) * u) + ((c + (d - c) * u) - (a + (b - a) * u)) * v
    return np.clip(n * 0.72 + 0.5, 0, 1)


def fbm(x, y, seed=0, octaves=4, gain=0.5, lac=2.0):
    s, amp, tot = 0.0, 1.0, 0.0
    x = np.asarray(x, np.float64)
    y = np.asarray(y, np.float64)
    for o in range(octaves):
        s = s + amp * pnoise(x, y, seed + o * 17)
        tot += amp
        amp *= gain
        x, y = x * lac + 13.7, y * lac + 7.3
    return s / tot


def cells(x, y, seed=0):
    """the distance to the nearest of a jittered grid of points and to the
    second nearest, the nearest's own random number and where it is
    (Voronoi)"""
    x = np.asarray(x, np.float64)
    y = np.asarray(y, np.float64)
    xi = np.floor(x).astype(np.int64)
    yi = np.floor(y).astype(np.int64)
    d1 = np.full(x.shape, 9.0)
    d2 = np.full(x.shape, 9.0)
    idn = np.zeros(x.shape)
    nx = np.zeros(x.shape)
    ny = np.zeros(x.shape)
    for ox in (-1, 0, 1):
        for oy in (-1, 0, 1):
            cx, cy = xi + ox, yi + oy
            h = _hash(cx, cy, seed)
            px = cx + (h & 0xffff) / 65535.0
            py = cy + ((h >> 15) & 0xffff) / 65535.0
            d = np.hypot(x - px, y - py)
            near = d < d1
            d2 = np.where(near, d1, np.minimum(d2, d))
            idn = np.where(near, (h % 9973) / 9973.0, idn)
            nx = np.where(near, px, nx)
            ny = np.where(near, py, ny)
            d1 = np.where(near, d, d1)
    return d1, d2, idn, nx, ny


def smooth(a, b, x):
    t = np.clip((np.asarray(x, np.float64) - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)


def lerp(a, b, t):
    return a + (b - a) * t


def lin3(c):
    """a '#colour' as linear RGB; a tuple is taken as linear already"""
    return np.array(B.lin(c)[:3]) if isinstance(c, str) else np.array(c, np.float64)


def shade(stops, t):
    """colours (linear) along t: stops is [(t0, '#hex'), ...]"""
    t = np.asarray(t, np.float64)
    out = np.zeros(t.shape + (3,))
    out[...] = lin3(stops[0][1])
    for (t0, c0), (t1, c1) in zip(stops[:-1], stops[1:]):
        k = smooth(t0, t1, t)[..., None]
        out = out * (1 - k) + lin3(c1) * k
    return out


# ---------------------------------------------------------------------------
# Node materials, written short
# ---------------------------------------------------------------------------

def new_mat(name):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    return m, m.node_tree, m.node_tree.nodes['Principled BSDF']


def N(nt, kind, props=None, **ins):
    """a node: props are its own settings, the rest its inputs by name
    (Base_Color for 'Base Color', i0 for the first input), each a value, a
    '#colour' or another node's output"""
    n = nt.nodes.new(kind)
    for k, v in (props or {}).items():
        setattr(n, k, v)
    for k, v in ins.items():
        s = n.inputs[int(k[1:])] if (k[0] == 'i' and k[1:].isdigit()) else n.inputs[k.replace('_', ' ')]
        if isinstance(v, bpy.types.NodeSocket):
            nt.links.new(v, s)
        elif isinstance(v, str):
            s.default_value = B.lin(v)
        else:
            s.default_value = v
    return n


def ins(nt, node, **kw):
    """set inputs of a node that exists already (the Principled)"""
    for k, v in kw.items():
        s = node.inputs[k.replace('_', ' ')]
        if isinstance(v, bpy.types.NodeSocket):
            nt.links.new(v, s)
        elif isinstance(v, str):
            s.default_value = B.lin(v)
        else:
            s.default_value = v


def ramp(nt, fac, stops, interp='LINEAR'):
    """a colour ramp: stops is [(position, '#colour' or grey 0..1), ...]"""
    n = nt.nodes.new('ShaderNodeValToRGB')
    cr = n.color_ramp
    cr.interpolation = interp

    def col(c):
        return B.lin(c) if isinstance(c, str) else (c, c, c, 1.0)
    cr.elements[0].position = stops[0][0]
    cr.elements[0].color = col(stops[0][1])
    cr.elements[1].position = stops[-1][0]
    cr.elements[1].color = col(stops[-1][1])
    for (p, c) in stops[1:-1]:
        e = cr.elements.new(p)
        e.color = col(c)
    nt.links.new(fac, n.inputs['Fac'])
    return n.outputs['Color']


def mix(nt, fac, a, b, blend='MIX'):
    return N(nt, 'ShaderNodeMixRGB', {'blend_type': blend}, Fac=fac, Color1=a, Color2=b).outputs['Color']


def math_(nt, op, a, b=0.0, clamp=False):
    return N(nt, 'ShaderNodeMath', {'operation': op, 'use_clamp': clamp}, i0=a, i1=b).outputs[0]


def position(nt, scale=None, rot_z=0.0):
    """the point being shaded, in the world's metres (so a pattern runs on
    across objects), optionally turned and scaled"""
    p = N(nt, 'ShaderNodeNewGeometry').outputs['Position']
    if scale is None and not rot_z:
        return p
    return N(nt, 'ShaderNodeMapping', Vector=p, Rotation=(0, 0, math.radians(rot_z)),
             Scale=scale or (1, 1, 1)).outputs[0]


def attr(nt, name):
    return N(nt, 'ShaderNodeAttribute', {'attribute_name': name}).outputs['Color']


def bump(nt, height, strength=0.3, dist=0.05, normal=None):
    n = N(nt, 'ShaderNodeBump', Strength=strength, Distance=dist, Height=height)
    if normal is not None:
        nt.links.new(normal, n.inputs['Normal'])
    return n.outputs['Normal']


def tint_mat(name, rough=0.4, metal=0.0, clear=0.0, emit=0.0, edge=0.0, grain=0.0, grain_scale=6.0, spec=0.5):
    """the material of a Batch: every piece wears the colour painted on its
    own vertices. emit makes it shine in that colour, edge adds light at
    grazing angles (a gem's facets), grain breaks the colour with a noise."""
    if name in B.MATS:
        return B.MATS[name]
    m, nt, b = new_mat(name)
    col = attr(nt, 'Col')
    if grain:
        nz = N(nt, 'ShaderNodeTexNoise', Vector=position(nt), Scale=grain_scale, Detail=4.0).outputs['Fac']
        col = mix(nt, math_(nt, 'MULTIPLY', nz, grain), col, '#000000', 'MULTIPLY')
        ins(nt, b, Normal=bump(nt, nz, 0.35, 0.06))
    ins(nt, b, Base_Color=col, Roughness=rough, Metallic=metal, Clearcoat=clear, Specular=spec)
    if emit or edge:
        k = emit
        if edge:
            lw = N(nt, 'ShaderNodeLayerWeight', Blend=0.35).outputs['Facing']
            k = math_(nt, 'ADD', math_(nt, 'MULTIPLY', lw, edge), emit)
        ins(nt, b, Emission=col, Emission_Strength=k)
    B.MATS[name] = m
    return m


def cobble_mat(name, c0, c1, groove, scale=2.4, rough=0.6, depth=0.5, seed_off=0.0, emit=None, emit_k=0.0):
    """paving for a road: stones of two tones with darker joints, laid in
    the world's own coordinates so two roads that merge agree"""
    if name in B.MATS:
        return B.MATS[name]
    m, nt, b = new_mat(name)
    p = N(nt, 'ShaderNodeMapping', Vector=position(nt), Location=(seed_off, seed_off * 0.7, 0)).outputs[0]
    edge = N(nt, 'ShaderNodeTexVoronoi', {'feature': 'DISTANCE_TO_EDGE'}, Vector=p, Scale=scale).outputs['Distance']
    cell = N(nt, 'ShaderNodeTexVoronoi', {'feature': 'F1'}, Vector=p, Scale=scale).outputs['Color']
    tone = N(nt, 'ShaderNodeSeparateRGB', Image=cell).outputs[0]
    stone = mix(nt, tone, B.lin(c0), B.lin(c1))
    nz = N(nt, 'ShaderNodeTexNoise', Vector=p, Scale=14.0, Detail=3.0).outputs['Fac']
    stone = mix(nt, math_(nt, 'MULTIPLY', nz, 0.25), stone, '#000000', 'MULTIPLY')
    joint = ramp(nt, edge, [(0.0, 0.0), (0.06, 1.0)])
    ins(nt, b, Base_Color=mix(nt, joint, B.lin(groove), stone), Roughness=rough,
        Normal=bump(nt, joint, depth, 0.04))
    if emit is not None:
        ins(nt, b, Emission=mix(nt, joint, B.lin(emit), '#000000'), Emission_Strength=emit_k)
    B.MATS[name] = m
    return m


# ---------------------------------------------------------------------------
# The board as a field of heights
# ---------------------------------------------------------------------------

class Field:
    """A grid over the whole board (and a little beyond the screen's
    edges). gap is how far each point is from the nearest thing of the
    game's: the road's edge, a pad's edge, the fort's plate."""

    def __init__(self, w, step=0.1, x0=-10.6, x1=10.6, y0=-1.6, y1=42.4):
        self.w = w
        self.step, self.x0, self.y0 = step, x0, y0
        self.xs = np.arange(x0, x1 + step / 2, step)
        self.ys = np.arange(y0, y1 + step / 2, step)
        self.X, self.Y = np.meshgrid(self.xs, self.ys)
        pts = []
        for p in w.paths:
            for a, b in zip(p[:-1], p[1:]):
                pts.append(a)
                pts.append(((a[0] + b[0]) / 2, (a[1] + b[1]) / 2))
            pts.append(p[-1])
        self.d_road = self._dist(np.array(pts))
        self.d_pad = self._dist(np.array(w.pads))
        self.d_fort = np.hypot(self.X - w.fort[0], self.Y - w.fort[1])
        self.gap = np.minimum(np.minimum(self.d_road - HALF, self.d_pad - PAD_R), self.d_fort - FORT_R)
        self.Z = np.full(self.X.shape, BASE)
        self.col = np.zeros(self.X.shape + (3,))
        self.glow = np.zeros(self.X.shape + (3,))

    def _dist(self, pts):
        d = np.full(self.X.shape, 1e9)
        ch = max(4, int(4.5e6 / self.X.size))
        for i in range(0, len(pts), ch):
            c = pts[i:i + ch]
            dd = np.sqrt((self.X[..., None] - c[:, 0]) ** 2 + (self.Y[..., None] - c[:, 1]) ** 2).min(-1)
            d = np.minimum(d, dd)
        return d

    def at(self, a, x, y):
        """the grid a read at a point (bilinear)"""
        fx = min(max((x - self.x0) / self.step, 0), a.shape[1] - 1.001)
        fy = min(max((y - self.y0) / self.step, 0), a.shape[0] - 1.001)
        ix, iy = int(fx), int(fy)
        tx, ty = fx - ix, fy - iy
        return ((a[iy, ix] * (1 - tx) + a[iy, ix + 1] * tx) * (1 - ty) +
                (a[iy + 1, ix] * (1 - tx) + a[iy + 1, ix + 1] * tx) * ty)

    def z(self, x, y):
        return float(self.at(self.Z, x, y))

    def free(self, a=0.12, ramp_w=2.2):
        """0 on the road and the pads, 1 where the ground may do as it
        likes"""
        return smooth(a, a + ramp_w, self.gap)

    def relief(self, H, a=0.12, ramp_w=2.2, cap=True):
        """the ground's heights: H where there is room, flat at Z = 0 under
        the road, the pads and the fort, and never so high beside them
        that it would hide an alien (SPEC: 0.5 m within 1.6 m of the
        road's centre)"""
        Z = BASE + H * self.free(a, ramp_w)
        if cap:
            Z = np.minimum(Z, 0.22 + 0.9 * np.maximum(self.gap - 0.5, 0))
        self.Z = Z
        return Z

    def add_glow(self, x, y, r, color, k=1.0):
        """light painted on the ground around a glowing thing (the render
        has no bloom, and this costs no samples)"""
        c = lin3(color) if isinstance(color, str) else np.array(color)
        d2 = (self.X - x) ** 2 + (self.Y - y) ** 2
        self.glow += (k * np.exp(-d2 / (r * r)))[..., None] * c

    def mesh(self, mat, name='ground', mask=None, Z=None, smooth_faces=True, XY=None):
        """the grid as one mesh, with the painted colours as the attributes
        'Col' and 'Glow'; mask leaves out the cells outside it"""
        Z = self.Z if Z is None else Z
        ny, nx = self.X.shape
        gx, gy = XY if XY is not None else (self.X, self.Y)
        co = np.stack([gx, gy, Z], -1).reshape(-1, 3)
        idx = np.arange(ny * nx).reshape(ny, nx)
        quads = np.stack([idx[:-1, :-1], idx[:-1, 1:], idx[1:, 1:], idx[1:, :-1]], -1).reshape(-1, 4)
        if mask is not None:
            keep = (mask[:-1, :-1] & mask[:-1, 1:] & mask[1:, 1:] & mask[1:, :-1]).ravel()
            quads = quads[keep]
        me = bpy.data.meshes.new(name)
        me.vertices.add(len(co))
        me.vertices.foreach_set('co', co.ravel())
        me.loops.add(len(quads) * 4)
        me.loops.foreach_set('vertex_index', quads.ravel())
        me.polygons.add(len(quads))
        me.polygons.foreach_set('loop_start', np.arange(0, len(quads) * 4, 4))
        me.polygons.foreach_set('loop_total', np.full(len(quads), 4))
        me.polygons.foreach_set('use_smooth', np.full(len(quads), smooth_faces))
        me.update(calc_edges=True)
        for nm, a in (('Col', self.col), ('Glow', self.glow)):
            ca = me.attributes.new(nm, 'FLOAT_COLOR', 'POINT')
            rgba = np.concatenate([a.reshape(-1, 3), np.ones((ny * nx, 1))], 1).astype(np.float32)
            ca.data.foreach_set('color', rgba.ravel())
        me.materials.append(mat)
        o = bpy.data.objects.new(name, me)
        B.sc.collection.objects.link(o)
        return o


def calm_rows(Y):
    """1 over the rows where the game is played, falling to 0 under the
    HUD at the top and the tower bar at the bottom"""
    return smooth(HUD_BOT_Y - 1.5, HUD_BOT_Y + 1.0, Y) * (1 - smooth(HUD_TOP_Y - 0.5, HUD_TOP_Y + 1.5, Y))


def road_ok(F, x, y, margin=0.3):
    """not on the road, a pad or the fort's plate (for small things lower
    than half a metre, which may stand near the road)"""
    return F.at(F.gap, x, y) > margin


def max_height(F, x, y, r, top=6.0, taper=0.6):
    """How tall a prop of radius r standing at (x, y) may be before its
    picture, which rises 0.75 m up the board for every metre of height,
    covers the road, a pad, the fort's plate or the HUD's rows."""
    t, lim = 0.0, top * 0.75
    while t <= lim:
        yy = y + t
        if yy > HUD_TOP_Y:
            break
        rr = r * (1 - taper * t / lim)
        hit = False
        for dx in (-rr, 0.0, rr):
            if (F.at(F.d_road, x + dx, yy) < HALF + 0.15 or F.at(F.d_pad, x + dx, yy) < PAD_R + 0.1 or
                    F.at(F.d_fort, x + dx, yy) < FORT_R + 0.1):
                hit = True
        if hit:
            break
        t += 0.2
    return max(0.0, (t - 0.2) / 0.75)


def sites(F, rnd, n, r, hmin=1.5, top=6.0, sep=3.0, tries=2500, xmax=9.2, y0=6.6, y1=36.0, test=None, taken=None):
    """n places for tall props: (x, y, the height the place allows), the
    roomiest first and none nearer than sep to another. taken is a list of
    (x, y, r) already used, which grows with the ones chosen here."""
    cand = []
    for _ in range(tries):
        x, y = rnd.uniform(-xmax, xmax), rnd.uniform(y0, y1)
        if not K.clear(F.w, x, y, r):
            continue
        if test is not None and not test(x, y):
            continue
        h = max_height(F, x, y, r, top)
        if h < hmin:
            continue
        cand.append((h + rnd.uniform(0, 2.0), x, y, h))
    cand.sort(reverse=True)
    out = []
    taken = taken if taken is not None else []
    for (_, x, y, h) in cand:
        if len(out) >= n:
            break
        if all(math.hypot(x - tx, y - ty) >= max(sep, r + tr + 0.2) for (tx, ty, tr) in taken):
            out.append((x, y, h))
            taken.append((x, y, r))
    return out


def scatter(F, rnd, n, margin=0.3, y0=0.0, y1=41.0, xmax=10.0, test=None, taken=None, r=0.0, tries=40):
    """n places for small things anywhere off the road and the pads"""
    out = []
    for _ in range(n * tries):
        if len(out) >= n:
            break
        x, y = rnd.uniform(-xmax, xmax), rnd.uniform(y0, y1)
        if not road_ok(F, x, y, margin + r):
            continue
        if test is not None and not test(x, y):
            continue
        if taken is not None and any(math.hypot(x - tx, y - ty) < r + tr for (tx, ty, tr) in taken):
            continue
        out.append((x, y))
    return out


def _seg_dist(q, p):
    """how far the point q is from the polyline p"""
    a = np.array(p[:-1])
    b = np.array(p[1:])
    ab = b - a
    t = np.clip(((q[0] - a[:, 0]) * ab[:, 0] + (q[1] - a[:, 1]) * ab[:, 1]) / np.maximum((ab ** 2).sum(1), 1e-9), 0, 1)
    return float(np.hypot(a[:, 0] + ab[:, 0] * t - q[0], a[:, 1] + ab[:, 1] * t - q[1]).min())


def ribbon(p, half, z, m, z_side=None, name='road'):
    """K.ribbon (the same strip, 'half' wide each side of the dense path
    p), mended for bends tighter than the strip is wide. There the inner
    edge of the kit's ribbon runs backwards and its faces fold over each
    other in one plane, which Cycles renders as black slivers (the last
    bend before the fort in colmena and cienaga). Here an inner edge that
    would run backwards waits at the bend instead, and every face has a
    height of its own, a fraction of a millimetre from its neighbours',
    so no two can lie in the same plane."""
    n = K._normals(p)
    L, R = [], []
    for (x, y), (nx, ny) in zip(p, n):
        for side, s_ in ((L, 1), (R, -1)):
            pt = (x + nx * s_ * half, y + ny * s_ * half)
            if side and (pt[0] - side[-1][0]) * ny - (pt[1] - side[-1][1]) * nx < 1e-4:
                pt = side[-1]
            side.append(pt)
    verts, faces = [], []
    for i in range(len(p) - 1):
        zz = z + 0.0007 * (i % 4)
        k = len(verts)
        verts += [(L[i][0], L[i][1], zz), (R[i][0], R[i][1], zz), (R[i + 1][0], R[i + 1][1], zz), (L[i + 1][0], L[i + 1][1], zz)]
        faces.append((k, k + 1, k + 2, k + 3))
        if z_side is not None:
            verts += [(L[i][0], L[i][1], z_side), (R[i][0], R[i][1], z_side), (R[i + 1][0], R[i + 1][1], z_side),
                      (L[i + 1][0], L[i + 1][1], z_side)]
            faces.append((k, k + 3, k + 7, k + 4))
            faces.append((k + 1, k + 5, k + 6, k + 2))
    return B.mesh(verts, faces, m, name=name)


def lay_road(w, road_mat, edge_mat=None, edge_w=0.14, edge_h=0.05, lift=0.004, side=None):
    """K.road for a world whose roads join or bend sharply. The kit lays
    one ribbon a path, and where two paths share a stretch the two
    ribbons lie in the same plane, which Cycles renders black; its kerbs
    also run across the mouth where one road meets another. Here every
    path's ribbon lies a few millimetres above the one before, and a kerb
    is left out wherever it would stand on another path's road or repeat
    that path's kerb. The strip is the kit's (the path's own points and
    normals, PATH_W wide at Z = 0), so the road is where the game has it.
    side, if given, is the height the ribbon's walls go down to (a
    bridge's deck)."""
    for i, p in enumerate(w.paths):
        ribbon(p, HALF, lift + 0.004 * i, road_mat, z_side=side, name='road%d' % i)
        if edge_mat is None:
            continue
        n = K._normals(p)
        for s in (1, -1):
            off = HALF + edge_w / 2
            q = []
            for (x, y), (nx, ny) in zip(p, n):
                pt = (x + nx * s * off, y + ny * s * off)
                # the same on the inside of a tight bend: the kerb waits
                if q and (pt[0] - q[-1][0]) * ny - (pt[1] - q[-1][1]) * nx < 0.02:
                    continue
                q.append(pt)
            run = []
            for pt in q + [None]:
                keep = pt is not None
                if keep:
                    for k, other in enumerate(w.paths):
                        if k != i and _seg_dist(pt, other) < HALF + (edge_w + 0.03 if k < i else 0.02):
                            keep = False
                            break
                if keep:
                    run.append(pt)
                else:
                    if len(run) > 1:
                        ribbon(run, edge_w / 2, edge_h, edge_mat, z_side=0.0, name='kerb')
                    run = []


# ---------------------------------------------------------------------------
# Many small things as one mesh
# ---------------------------------------------------------------------------

def TM(x, y, z, yaw=0.0, tilt=0.0, tilt_dir=0.0, scale=(1, 1, 1)):
    """where a piece stands: turned yaw about Z, then leant tilt degrees
    towards the compass direction tilt_dir"""
    return (Matrix.Translation((x, y, z)) @ Matrix.Rotation(math.radians(tilt_dir), 4, 'Z') @
            Matrix.Rotation(math.radians(tilt), 4, 'Y') @ Matrix.Rotation(math.radians(yaw - tilt_dir), 4, 'Z') @
            Matrix.Diagonal((scale[0], scale[1], scale[2], 1.0)))


def _c3(c):
    return B.rgb(c)


class Batch:
    def __init__(self, name, mat, smooth_faces=False):
        self.name, self.mat, self.smooth = name, mat, smooth_faces
        self.bm = bmesh.new()
        self.col = self.bm.loops.layers.color.new('Col')

    def _done(self, verts, faces, M, c0, c1=None, h=1.0, z0=0.0, rnd=None, vary=0.0):
        """paint the new faces (c0 at the bottom to c1 at height h) and put
        the piece in its place"""
        a = _c3(c0)
        b = _c3(c1) if c1 is not None else a
        for f in faces:
            k = 1.0 + (rnd.uniform(-vary, vary) if (rnd and vary) else 0.0)
            for lp in f.loops:
                t = min(max((lp.vert.co.z - z0) / h, 0.0), 1.0)
                lp[self.col] = (min(1, (a[0] + (b[0] - a[0]) * t) * k), min(1, (a[1] + (b[1] - a[1]) * t) * k),
                                min(1, (a[2] + (b[2] - a[2]) * t) * k), 1.0)
        bmesh.ops.transform(self.bm, matrix=M, verts=verts)

    def spire(self, M, r, h, c0, c1=None, segs=6, tip=0.28, base=0.75, sink=0.4, lean=0.0, rnd=None, vary=0.0):
        """a crystal: a prism of 'segs' sides with a point"""
        bm = self.bm
        ang = [2 * math.pi * i / segs for i in range(segs)]
        r0 = [bm.verts.new((r * base * math.cos(a), r * base * math.sin(a), -sink)) for a in ang]
        r1 = [bm.verts.new((r * math.cos(a), r * math.sin(a), h * (1 - tip))) for a in ang]
        ap = bm.verts.new((r * lean, 0, h))
        faces = []
        for i in range(segs):
            j = (i + 1) % segs
            faces.append(bm.faces.new((r0[i], r0[j], r1[j], r1[i])))
            faces.append(bm.faces.new((r1[i], r1[j], ap)))
        self._done(r0 + r1 + [ap], faces, M, c0, c1, h, rnd=rnd, vary=vary)

    def rock(self, M, r, c0, c1=None, rnd=None, subdiv=2, jitter=0.2, vary=0.12):
        """a faceted boulder"""
        vs = bmesh.ops.create_icosphere(self.bm, subdivisions=subdiv, radius=r)['verts']
        if rnd is not None and jitter:
            for v in vs:
                v.co *= 1 + rnd.uniform(-jitter, jitter)
        faces = list({f for v in vs for f in v.link_faces})
        self._done(vs, faces, M, c0, c1, 2 * r, z0=-r, rnd=rnd, vary=vary)

    def ball(self, M, r, c0, c1=None, segs=14):
        vs = bmesh.ops.create_uvsphere(self.bm, u_segments=segs, v_segments=max(6, segs // 2), radius=r)['verts']
        faces = list({f for v in vs for f in v.link_faces})
        self._done(vs, faces, M, c0, c1, 2 * r, z0=-r)

    def cone(self, M, r, h, c0, c1=None, r2=0.0, segs=10, rnd=None, vary=0.0):
        """a cone or a tapered post standing on its base"""
        vs = bmesh.ops.create_cone(self.bm, cap_ends=True, cap_tris=False, segments=segs, radius1=r,
                                   radius2=max(r2, 1e-4), depth=h)['verts']
        for v in vs:
            v.co.z += h / 2
        faces = list({f for v in vs for f in v.link_faces})
        self._done(vs, faces, M, c0, c1, h, rnd=rnd, vary=vary)

    def lathe(self, M, profile, c0, c1=None, segs=16):
        """a profile [(radius, z), ...] turned about Z"""
        bm = self.bm
        rings = [[bm.verts.new((max(r, 1e-4) * math.cos(2 * math.pi * j / segs),
                                max(r, 1e-4) * math.sin(2 * math.pi * j / segs), z)) for j in range(segs)]
                 for (r, z) in profile]
        faces = []
        for a, b in zip(rings[:-1], rings[1:]):
            for j in range(segs):
                faces.append(bm.faces.new((a[j], a[(j + 1) % segs], b[(j + 1) % segs], b[j])))
        faces.append(bm.faces.new(list(reversed(rings[0]))))
        faces.append(bm.faces.new(rings[-1]))
        zs = [z for (_, z) in profile]
        self._done([v for r in rings for v in r], faces, M, c0, c1, max(zs) - min(zs) or 1.0, z0=min(zs))

    def tube(self, pts, r, c0, c1=None, r_end=None, segs=8):
        """a round bar through the points, in the world's own coordinates,
        painted from c0 at its start to c1 at its end"""
        bm = self.bm
        P = [Vector(p) for p in pts]
        a3, b3 = _c3(c0), _c3(c1) if c1 is not None else _c3(c0)
        rings = []
        for i, p in enumerate(P):
            d = (P[min(i + 1, len(P) - 1)] - P[max(i - 1, 0)]).normalized()
            up = Vector((0, 0, 1)) if abs(d.z) < 0.95 else Vector((1, 0, 0))
            u = d.cross(up).normalized()
            v = d.cross(u).normalized()
            k = i / max(1, len(P) - 1)
            rr = r if r_end is None else r + (r_end - r) * k
            rings.append([bm.verts.new(p + (u * math.cos(2 * math.pi * j / segs) + v * math.sin(2 * math.pi * j / segs)) * rr)
                          for j in range(segs)])
        faces = []
        for i in range(len(P) - 1):
            a, b = rings[i], rings[i + 1]
            for j in range(segs):
                f = bm.faces.new((a[j], a[(j + 1) % segs], b[(j + 1) % segs], b[j]))
                k = (i + 0.5) / (len(P) - 1)
                for lp in f.loops:
                    lp[self.col] = (a3[0] + (b3[0] - a3[0]) * k, a3[1] + (b3[1] - a3[1]) * k,
                                    a3[2] + (b3[2] - a3[2]) * k, 1.0)
                faces.append(f)
        for ring, c in ((list(reversed(rings[0])), a3), (rings[-1], b3)):
            f = bm.faces.new(ring)
            for lp in f.loops:
                lp[self.col] = (c[0], c[1], c[2], 1.0)

    def finish(self):
        me = bpy.data.meshes.new(self.name)
        self.bm.normal_update()
        self.bm.to_mesh(me)
        self.bm.free()
        if self.smooth:
            for p in me.polygons:
                p.use_smooth = True
            me.use_auto_smooth = True
            me.auto_smooth_angle = math.radians(50)
        me.materials.append(self.mat)
        o = bpy.data.objects.new(self.name, me)
        B.sc.collection.objects.link(o)
        return o


# ---------------------------------------------------------------------------
# cristal: pink dunes, amethyst spires, glass
# ---------------------------------------------------------------------------

def sand_mat():
    m, nt, b = new_mat('sand')
    p = position(nt)
    col = attr(nt, 'Col')
    # the wind's ripples, across the dunes
    wp = position(nt, rot_z=-28)
    wave = N(nt, 'ShaderNodeTexWave', {'wave_type': 'BANDS', 'bands_direction': 'X', 'wave_profile': 'SIN'},
             Vector=wp, Scale=1.15, Distortion=3.2, Detail=2.0, Detail_Scale=0.8).outputs['Fac']
    grain = N(nt, 'ShaderNodeTexNoise', Vector=p, Scale=38.0, Detail=2.0).outputs['Fac']
    col = mix(nt, math_(nt, 'MULTIPLY', wave, 0.16), col, '#7a2c78', 'MIX')
    col = mix(nt, math_(nt, 'MULTIPLY', grain, 0.22), col, '#fff0e0', 'MIX')
    # grains of glass that catch the light
    spark = N(nt, 'ShaderNodeTexVoronoi', {'feature': 'F1'}, Vector=p, Scale=11.0).outputs['Distance']
    spark = ramp(nt, spark, [(0.0, 1.0), (0.07, 1.0), (0.10, 0.0)])
    h = math_(nt, 'ADD', math_(nt, 'MULTIPLY', wave, 1.0), math_(nt, 'MULTIPLY', grain, 0.25))
    ins(nt, b, Base_Color=col, Roughness=0.78, Specular=0.3, Normal=bump(nt, h, 0.42, 0.05))
    em = mix(nt, spark, attr(nt, 'Glow'), '#ffd8f4', 'ADD')
    ins(nt, b, Emission=em, Emission_Strength=1.0)
    return m


def gem_mat(name, emit=0.55, edge=1.3, rough=0.08):
    return tint_mat(name, rough=rough, clear=1.0, emit=emit, edge=edge, spec=0.8)


def crystal_cluster(bt, rnd, x, y, z, h, r, c_lo, c_hi, n=6, spread=1.0):
    """one tall spire and a ring of smaller ones leaning away from it"""
    bt.spire(TM(x, y, z, rnd.uniform(0, 60), rnd.uniform(0, 7), rnd.uniform(0, 360)), r, h, c_lo, c_hi,
             tip=rnd.uniform(0.18, 0.3), lean=rnd.uniform(-0.3, 0.3), rnd=rnd, vary=0.10)
    for i in range(n):
        a = 360.0 * i / n + rnd.uniform(-25, 25)
        d = r * rnd.uniform(0.7, 1.5) * spread
        hh = h * rnd.uniform(0.25, 0.68)
        rr = r * rnd.uniform(0.35, 0.62)
        bt.spire(TM(x + d * math.cos(math.radians(a)), y + d * math.sin(math.radians(a)), z, rnd.uniform(0, 60),
                    rnd.uniform(12, 34), a), rr, hh, c_lo, c_hi, tip=rnd.uniform(0.2, 0.4), rnd=rnd, vary=0.10)


def build_cristal(w):
    rnd = random.Random(606)
    F = Field(w)
    X, Y = F.X, F.Y

    # Dunes: long crests across the wind (which blows with the light, from
    # the left), each with a gentle windward slope and a steep lee, broken
    # up so no two are alike. A second, smaller train rides on them.
    warp = (fbm(X / 9.0, Y / 9.0, 11, 3) - 0.5) * 2.6
    u = (X * 0.88 + Y * 0.42) / 5.2 + warp
    pfrac = u - np.floor(u)
    prof = smooth(0.0, 0.72, pfrac) * (1 - smooth(0.72, 1.0, pfrac))
    amp = 0.35 + 1.25 * smooth(0.3, 0.7, fbm(X / 6.0 + 40, Y / 6.0, 12, 3))
    amp = amp * (0.4 + 0.6 * calm_rows(Y))       # lower dunes under the HUD
    u2 = (X * 0.8 + Y * 0.55) / 1.7 + warp * 2.0 + 3.0 * fbm(X / 4.0, Y / 4.0, 13, 2)
    p2 = u2 - np.floor(u2)
    prof2 = smooth(0.0, 0.7, p2) * (1 - smooth(0.7, 1.0, p2))
    H = prof * amp * 1.35 + prof2 * 0.16 + (fbm(X / 2.5, Y / 2.5, 14, 3) - 0.5) * 0.25 + 0.12
    # hollows where the sand has fused into pools of glass
    basin = smooth(0.57, 0.68, fbm(X / 5.0 + 7, Y / 5.0 + 3, 15, 2)) * smooth(1.0, 2.2, F.gap)
    H = H * (1 - basin) - 0.55 * basin
    F.relief(H)

    crest = smooth(0.25, 1.3, F.Z)
    tone = np.clip(crest * 0.8 + (fbm(X / 3.0, Y / 3.0, 16, 3) - 0.5) * 0.6 + prof * 0.25, 0, 1)
    F.col = shade([(0.0, '#8a3c96'), (0.35, '#c85596'), (0.7, '#ec7a9a'), (1.0, '#ffb497')], tone)
    wet = smooth(-0.06, -0.3, F.Z)[..., None]
    F.col = F.col * (1 - wet) + lin3('#5a2a8a') * wet

    # crystals
    gem = gem_mat('gem')
    glass = tint_mat('glass', rough=0.05, clear=1.0, emit=0.35, edge=1.6, spec=1.0)
    rockm = tint_mat('rock', rough=0.75, grain=0.5, grain_scale=5.0)
    bt, bg, br = Batch('gems', gem), Batch('glass', glass), Batch('rocks', rockm)
    AMETHYST = [('#4a1fb0', '#b070ff'), ('#5a20c0', '#d088ff'), ('#3a28c8', '#8a8cff'), ('#7a20b8', '#ff80f0')]
    AQUA = ('#0a8aa8', '#90fff0')
    taken = []
    big = sites(F, rnd, 11, 1.3, hmin=2.2, top=6.0, sep=4.2, taken=taken)
    for (x, y, h) in big:
        h = min(h, rnd.uniform(4.2, 6.0))
        z = F.z(x, y) - 0.1
        c = AQUA if len(taken) % 4 == 2 else rnd.choice(AMETHYST)
        for k in range(5):
            a = rnd.uniform(0, 360)
            d = rnd.uniform(0.5, 1.0)
            br.rock(TM(x + d * math.cos(math.radians(a)), y + d * math.sin(math.radians(a)), z, rnd.uniform(0, 360),
                       scale=(1, 1, 0.6)), rnd.uniform(0.3, 0.5), '#4a2c5c', '#6a3c78', rnd)
        crystal_cluster(bt, rnd, x, y, z, h, 0.42 + 0.03 * h, c[0], c[1], n=rnd.randint(5, 8))
        F.add_glow(x, y, 1.7, c[1], 0.28)
    for (x, y, h) in sites(F, rnd, 22, 0.75, hmin=0.8, top=2.6, sep=2.2, taken=taken):
        h = min(h, rnd.uniform(1.0, 2.4))
        c = rnd.choice(AMETHYST + [('#c03a90', '#ffa0d8')])
        crystal_cluster(bt, rnd, x, y, F.z(x, y) - 0.05, h, 0.24 + 0.04 * h, c[0], c[1], n=rnd.randint(3, 5))
        F.add_glow(x, y, 1.0, c[1], 0.18)
    # citrine: a few golden clusters
    for (x, y, h) in sites(F, rnd, 6, 0.45, hmin=0.8, top=2.2, sep=2.0, taken=taken):
        h = min(h, rnd.uniform(0.9, 1.7))
        crystal_cluster(bt, rnd, x, y, F.z(x, y) - 0.05, h, 0.26, '#e07810', '#ffe060', n=4)
        F.add_glow(x, y, 1.0, '#ffb040', 0.2)
    # glass shards lying about, low enough to stand by the road
    for (x, y) in scatter(F, rnd, 170, margin=0.35, taken=taken, r=0.15):
        c = rnd.choice([('#18b8c8', '#a8fff4'), ('#18b8c8', '#a8fff4'), ('#7038d8', '#d0a0ff'), ('#e04aa0', '#ffc0e8')])
        hh = min(rnd.uniform(0.18, 0.46), 0.25 + 0.6 * F.at(F.gap, x, y))
        (bg if c[0] == '#18b8c8' else bt).spire(
            TM(x, y, F.z(x, y) - 0.03, rnd.uniform(0, 60), rnd.uniform(5, 40), rnd.uniform(0, 360)),
            rnd.uniform(0.06, 0.13), hh, c[0], c[1], segs=rnd.choice([4, 5, 6]), tip=0.5, sink=0.1)
    for (x, y) in scatter(F, rnd, 70, margin=0.5, taken=taken, r=0.2):
        br.rock(TM(x, y, F.z(x, y), rnd.uniform(0, 360), scale=(1, rnd.uniform(0.7, 1.2), 0.55)),
                rnd.uniform(0.1, 0.24), '#5a3068', '#8a4c88', rnd, subdiv=1)

    # the glass pools: one level sheet under the dunes that shows in the
    # hollows, with shards standing in it
    m, nt, b = new_mat('pool')
    nz = N(nt, 'ShaderNodeTexNoise', Vector=position(nt), Scale=1.6, Detail=3.0, Distortion=1.5).outputs['Fac']
    ins(nt, b, Base_Color=ramp(nt, nz, [(0.3, '#0a8a9a'), (0.6, '#30e0d0'), (0.75, '#c8fff0')]), Roughness=0.04,
        Clearcoat=1.0, Emission=ramp(nt, nz, [(0.3, '#064a5a'), (0.7, '#20b8b0')]), Emission_Strength=0.55)
    B.mesh([(-11, -2, -0.2), (11, -2, -0.2), (11, 43, -0.2), (-11, 43, -0.2)], [(0, 1, 2, 3)], m, name='pool')
    for (x, y) in scatter(F, rnd, 60, margin=1.2, test=lambda x, y: F.z(x, y) < -0.3):
        bg.spire(TM(x, y, -0.25, rnd.uniform(0, 60), rnd.uniform(0, 25), rnd.uniform(0, 360)), rnd.uniform(0.07, 0.16),
                 rnd.uniform(0.3, 0.9), '#18b8c8', '#d0fff8', segs=5, tip=0.45, sink=0.1)
    bt.finish()
    bg.finish()
    br.finish()
    F.mesh(sand_mat())

    road = cobble_mat('road_cristal', '#d8b8a4', '#f2d6c0', '#8a5a78', scale=2.1, rough=0.55, depth=0.35)
    kerb = B.mat('kerb_cristal', '#6a3a7a', rough=0.4, clear=0.6)
    lay_road(w, road, kerb, edge_w=0.16, edge_h=0.06)
    plate = B.mat('plate_cristal', '#3a2a50', rough=0.35, metal=0.6, clear=0.4)
    inner = B.mat('plate_in_cristal', '#2a1c3c', rough=0.5, metal=0.5)
    K.pads(w, plate, rim_k=5.0, inner_mat=inner)
    K.foundation(w, plate)

    B.sun(205, 38, 3.5, (1.0, 0.90, 0.78), angle=5)
    B.sun(35, 30, 1.0, (0.62, 0.55, 1.0), angle=25, name='rim')
    B.ambient((0.62, 0.45, 0.85), 0.55)


# ---------------------------------------------------------------------------
# cienaga: mud, pools of acid, twisted trees and great poisonous blooms
# ---------------------------------------------------------------------------

def mud_mat():
    m, nt, b = new_mat('mud')
    p = position(nt)
    col = attr(nt, 'Col')
    n1 = N(nt, 'ShaderNodeTexNoise', Vector=p, Scale=2.3, Detail=6.0, Distortion=0.9).outputs['Fac']
    n2 = N(nt, 'ShaderNodeTexNoise', Vector=p, Scale=11.0, Detail=4.0).outputs['Fac']
    crack = N(nt, 'ShaderNodeTexVoronoi', {'feature': 'DISTANCE_TO_EDGE'}, Vector=p, Scale=3.2).outputs['Distance']
    crack = ramp(nt, crack, [(0.0, 0.0), (0.05, 1.0)])
    col = mix(nt, ramp(nt, n1, [(0.35, 0.55), (0.7, 0.0)]), col, '#0c0a06', 'MIX')
    col = mix(nt, math_(nt, 'MULTIPLY', n2, 0.35), col, '#6a7a30', 'MIX')
    col = mix(nt, crack, '#0a0c06', col)
    h = math_(nt, 'ADD', math_(nt, 'MULTIPLY', n1, 0.7), math_(nt, 'MULTIPLY', n2, 0.3))
    h = math_(nt, 'MULTIPLY', h, math_(nt, 'ADD', math_(nt, 'MULTIPLY', crack, 0.5), 0.5))
    ins(nt, b, Base_Color=col, Roughness=ramp(nt, n1, [(0.35, 0.18), (0.62, 0.7)]), Specular=0.6,
        Normal=bump(nt, h, 0.7, 0.07), Emission=attr(nt, 'Glow'), Emission_Strength=1.0)
    return m


def acid_mat():
    m, nt, b = new_mat('acid')
    p = position(nt)
    nz = N(nt, 'ShaderNodeTexNoise', Vector=p, Scale=0.85, Detail=5.0, Distortion=2.6).outputs['Fac']
    foam = N(nt, 'ShaderNodeTexVoronoi', {'feature': 'F1'}, Vector=p, Scale=2.6).outputs['Distance']
    ring = ramp(nt, foam, [(0.0, 0.0), (0.16, 0.0), (0.20, 1.0), (0.24, 0.0)])
    c = mix(nt, ramp(nt, nz, [(0.3, 0.75), (0.5, 0.25), (0.7, 0.0)]), attr(nt, 'Col'), '#0c5a06', 'MIX')
    c = mix(nt, ramp(nt, nz, [(0.62, 0.0), (0.74, 0.8)]), c, '#f0ffa0', 'MIX')
    c = mix(nt, math_(nt, 'MULTIPLY', ring, 0.45), c, '#f4ffb0')
    ins(nt, b, Base_Color='#0c3004', Roughness=0.06, Emission=c, Emission_Strength=0.95)
    return m


def swamp_tree(bt, bp, rnd, x, y, z, h, pod):
    """a dead tree that grew twisting: the trunk wanders as it climbs,
    roots sprawl from its foot, and the few branches end in a pod of light"""
    BARK0, BARK1 = '#2a1c26', '#6a5440'
    pts, tips = [], []
    px, py, ang = x, y, rnd.uniform(0, 360)
    n = 8
    top = h * 0.72
    for i in range(n + 1):
        pts.append((px, py, z - 0.3 + (top + 0.3) * i / n))
        ang += rnd.uniform(-70, 70)
        px += 0.045 * h * math.cos(math.radians(ang))
        py += 0.045 * h * math.sin(math.radians(ang))
    r0 = 0.15 + 0.04 * h
    bt.tube(pts, r0, BARK0, BARK1, r_end=r0 * 0.45, segs=9)
    for k in range(rnd.randint(4, 6)):
        a = math.radians(360.0 * k / 5 + rnd.uniform(-25, 25))
        L = rnd.uniform(0.8, 1.5)
        bend = rnd.uniform(-0.5, 0.5)
        rp = []
        for i in range(6):
            t = i / 5.0
            aa = a + bend * t
            rp.append((x + L * t * math.cos(aa), y + L * t * math.sin(aa), z + 0.55 * (1 - t) ** 2 - 0.12 * t + 0.1 * math.sin(t * 6)))
        bt.tube(rp, r0 * 0.6, BARK0, BARK0, r_end=0.05, segs=7)
    for k in range(rnd.randint(3, 5)):
        i0 = rnd.randint(n // 2, n)
        bx, by, bz = pts[i0]
        a = math.radians(rnd.uniform(0, 360))
        L = h * rnd.uniform(0.18, 0.32)
        curl = rnd.uniform(-1.4, 1.4)
        bp_ = []
        for i in range(6):
            t = i / 5.0
            aa = a + curl * t
            bp_.append((bx + L * t * math.cos(aa), by + L * t * math.sin(aa), bz + L * (0.9 * t - 0.35 * t * t)))
        bt.tube(bp_, r0 * 0.4, BARK1, BARK0, r_end=0.04, segs=6)
        tips.append(bp_[-1])
    tips.append(pts[-1])
    for (tx, ty, tz) in tips:
        r = rnd.uniform(0.13, 0.2)
        bp.lathe(TM(tx, ty, tz - r * 2.2), [(0.02, 0), (r * 0.8, r * 0.5), (r, r * 1.2), (r * 0.6, r * 2.0), (0.03, r * 2.4)],
                 pod[0], pod[1], segs=10)


def toxic_bloom(bl, bp, rnd, x, y, z, r, c_out, c_in):
    """a flower as wide as a tower, open on the acid: two rings of thick
    petals round a heart that shines"""
    for ring, (n, rr, tilt, cc) in enumerate(((7, 1.0, -20.0, c_out), (5, 0.62, -46.0, c_in))):
        for k in range(n):
            a = 360.0 * k / n + ring * 26 + rnd.uniform(-7, 7)
            d = r * rr * 0.52
            lift = 0.10 + 0.16 * ring + d * math.sin(math.radians(-tilt)) * 0.9
            bl.ball(TM(x + d * math.cos(math.radians(a)), y + d * math.sin(math.radians(a)), z + lift, a, tilt + rnd.uniform(-6, 6), a,
                       scale=(1.0, 0.52, 0.16)), r * rr * 0.56, cc[0], cc[1], segs=14)
    bp.ball(TM(x, y, z + 0.26 * r, scale=(1, 1, 0.8)), r * 0.25, '#ffa010', '#ffff90', segs=12)
    for k in range(6):
        a = math.radians(60 * k + 20)
        bp.ball(TM(x + r * 0.2 * math.cos(a), y + r * 0.2 * math.sin(a), z + 0.36 * r), r * 0.06, '#fff080', '#ffffff', segs=8)


def build_cienaga(w):
    rnd = random.Random(707)
    F = Field(w)
    X, Y = F.X, F.Y

    # Mud in lumps, and wide hollows where the acid stands. One level
    # sheet of acid lies under the whole swamp and shows wherever the mud
    # dips below it.
    P = fbm(X / 5.2 + 3, Y / 5.2 + 9, 21, 3) + 0.10 * (fbm(X / 1.6, Y / 1.6, 22, 2) - 0.5)
    pool = smooth(0.555, 0.61, P)
    bank = smooth(0.47, 0.555, P) * (1 - pool)
    lumps = 0.12 + 0.75 * fbm(X / 2.6, Y / 2.6, 23, 4) ** 1.5 + 0.5 * smooth(0.5, 0.8, fbm(X / 4.0 + 9, Y / 4.0, 24, 2))
    H = (lumps + 0.18 * bank) * (1 - pool) - 0.6 * pool
    F.relief(H, ramp_w=1.5)
    LEVEL = -0.16
    wet = smooth(0.25, -0.1, F.Z - LEVEL)

    t = np.clip(F.Z * 1.1 + (fbm(X / 1.8, Y / 1.8, 25, 3) - 0.5) * 0.9, 0, 1)
    F.col = shade([(0.0, '#2a2416'), (0.3, '#3c3420'), (0.6, '#3a4a20'), (1.0, '#5c7028')], t)
    F.col = F.col * (1 - 0.6 * wet[..., None]) + lin3('#141c08') * 0.6 * wet[..., None]
    # the acid's light on its banks
    shore = smooth(0.49, 0.575, P) * (F.Z > LEVEL - 0.05) * F.free(0.3, 1.2)
    F.glow += shore[..., None] * lin3('#7cf018') * 0.30
    in_pool = lambda x, y: F.z(x, y) < LEVEL - 0.12  # noqa: E731

    bark = tint_mat('bark', rough=0.7, grain=0.6, grain_scale=9.0)
    petal = tint_mat('petal', rough=0.3, clear=0.5, emit=0.38, edge=0.5)
    podm = tint_mat('pod', rough=0.25, clear=0.5, emit=1.6, edge=0.6)
    leaf = tint_mat('leaf', rough=0.45, clear=0.3, emit=0.12)
    rockm = tint_mat('swamp_rock', rough=0.6, grain=0.55, grain_scale=5.0)
    bt, bb, bp, bl, br = (Batch('trees', bark, True), Batch('blooms', petal, True), Batch('pods', podm, True),
                          Batch('leaves', leaf, True), Batch('rocks', rockm))
    PODS = [('#ff6a10', '#ffd040'), ('#ff8a18', '#fff060'), ('#b040ff', '#f0a0ff'), ('#ff3a6a', '#ffb080')]
    taken = []

    # the great blooms first: they want the roomiest places
    BLOOMS = [(('#5a1ca8', '#d070ff'), ('#c02a90', '#ff9ae0')), (('#a01c70', '#ff70c0'), ('#e06a10', '#ffd060')),
              (('#3a2cc0', '#9a90ff'), ('#8a30d0', '#f0a0ff'))]
    for i, (x, y, h) in enumerate(sites(F, rnd, 4, 1.5, hmin=1.1, top=1.5, sep=7.0, taken=taken, xmax=8.2, y0=8.0, y1=35.0)):
        c = BLOOMS[i % len(BLOOMS)]
        toxic_bloom(bb, bp, rnd, x, y, max(F.z(x, y), LEVEL) - 0.02, rnd.uniform(1.15, 1.45), c[0], c[1])
        F.add_glow(x, y, 1.6, c[0][1], 0.16)
    for (x, y, h) in sites(F, rnd, 12, 1.25, hmin=2.4, top=6.0, sep=3.6, taken=taken,
                           test=lambda x, y: not in_pool(x, y)):
        h = min(h, rnd.uniform(3.6, 5.8))
        pod = rnd.choice(PODS)
        swamp_tree(bt, bp, rnd, x, y, F.z(x, y), h, pod)
        F.add_glow(x, y, 1.3, pod[0], 0.09)
    # roots that arch out of the mud and dive back
    for (x, y, h) in sites(F, rnd, 22, 0.8, hmin=0.9, top=1.4, sep=1.7, taken=taken, y0=1.0, y1=39.5):
        a = math.radians(rnd.uniform(0, 180))
        L, hh = rnd.uniform(1.0, 1.6), min(h, rnd.uniform(0.6, 1.1))
        pts = []
        for i in range(9):
            t = i / 8.0
            s = (t - 0.5) * L
            wob = 0.12 * math.sin(t * 9 + x)
            pts.append((x + math.cos(a) * s - math.sin(a) * wob, y + math.sin(a) * s + math.cos(a) * wob,
                        F.z(x, y) - 0.25 + (hh + 0.25) * math.sin(math.pi * t) ** 0.8))
        bt.tube(pts, rnd.uniform(0.09, 0.15), '#2a1c26', '#5a4838', r_end=0.07, segs=7)
        if rnd.random() < 0.6:
            pod = rnd.choice(PODS)
            px, py, pz = pts[4]
            bp.ball(TM(px, py, pz + 0.2), 0.13, pod[0], pod[1], segs=10)
            F.add_glow(px, py, 0.6, pod[0], 0.12)
    # clumps of glowing bulbs on stalks, in violet and orange
    for (x, y) in scatter(F, rnd, 46, margin=0.45, taken=taken, r=0.3, test=lambda x, y: not in_pool(x, y)):
        pod = rnd.choice(PODS)
        z = F.z(x, y)
        lim = 0.2 + 0.5 * F.at(F.gap, x, y)
        for k in range(rnd.randint(2, 5)):
            dx, dy = rnd.uniform(-0.22, 0.22), rnd.uniform(-0.22, 0.22)
            hh = min(rnd.uniform(0.18, 0.55), lim)
            bl.cone(TM(x + dx, y + dy, z - 0.05, 0, rnd.uniform(0, 14), rnd.uniform(0, 360)), 0.035, hh, '#1c3010', '#4a6a1c',
                    r2=0.025, segs=5)
            bp.ball(TM(x + dx, y + dy, z + hh, scale=(1, 1, 1.25)), rnd.uniform(0.06, 0.1), pod[0], pod[1], segs=8)
        F.add_glow(x, y, 0.5, pod[0], 0.13)
    # reeds at the pools' edges
    for (x, y) in scatter(F, rnd, 150, margin=0.4, test=lambda x, y: -0.05 < F.z(x, y) - LEVEL < 0.22):
        z = F.z(x, y)
        lim = 0.2 + 0.5 * F.at(F.gap, x, y)
        for k in range(rnd.randint(2, 4)):
            hh = min(rnd.uniform(0.3, 0.8), lim)
            bl.cone(TM(x + rnd.uniform(-0.12, 0.12), y + rnd.uniform(-0.12, 0.12), z - 0.05, 0, rnd.uniform(2, 16),
                       rnd.uniform(0, 360)), 0.032, hh, '#28401a', '#9ad040', r2=0.02, segs=5)
    # pads of leaves floating on the acid, some with a flower, and bubbles
    for (x, y) in scatter(F, rnd, 70, margin=1.0, test=in_pool):
        r = rnd.uniform(0.16, 0.34)
        c = rnd.choice([('#0c5a40', '#1c8a5c'), ('#12643a', '#30a060'), ('#3a2a6a', '#6a4aa8')])
        bl.lathe(TM(x, y, LEVEL, rnd.uniform(0, 360)), [(r * 0.9, 0.0), (r, 0.025), (r * 0.9, 0.04), (0.01, 0.03)], c[0], c[1],
                 segs=9)
        if rnd.random() < 0.3:
            bp.ball(TM(x, y, LEVEL + 0.09, scale=(1, 1, 0.8)), r * 0.32, '#ff5aa0', '#ffe0f0', segs=8)
    for (x, y) in scatter(F, rnd, 110, margin=1.0, test=in_pool):
        bp.ball(TM(x, y, LEVEL + 0.01), rnd.uniform(0.04, 0.1), '#b8ff40', '#f8ffc0', segs=8)
    # stones, mossy on top
    for (x, y) in scatter(F, rnd, 90, margin=0.45, taken=taken, r=0.2):
        r = min(rnd.uniform(0.12, 0.34), 0.14 + 0.3 * F.at(F.gap, x, y))
        br.rock(TM(x, y, F.z(x, y), rnd.uniform(0, 360), scale=(1, rnd.uniform(0.7, 1.2), 0.6)), r, '#3a3a34', '#6a8a3a', rnd,
                subdiv=1 if r < 0.2 else 2)

    for b_ in (bt, bb, bp, bl, br):
        b_.finish()
    F.mesh(mud_mat())
    # the acid: shallow and bright at the shore, deep green in the middle
    deep = smooth(0.0, 0.42, LEVEL - F.Z)
    F.col = shade([(0.0, '#d8ff50'), (0.35, '#74e812'), (1.0, '#178a0a')], deep)
    F.glow[...] = 0
    low = F.Z.copy()
    for ax in (0, 1):
        low = np.minimum(low, np.minimum(np.roll(low, 1, ax), np.roll(low, -1, ax)))
    F.mesh(acid_mat(), name='acid', mask=low < LEVEL + 0.02, Z=np.full(F.Z.shape, LEVEL))

    road = cobble_mat('road_cienaga', '#8c8660', '#aaa47a', '#3a3620', scale=1.9, rough=0.6, depth=0.45, seed_off=3.0)
    kerb = B.mat('kerb_cienaga', '#4a4630', rough=0.6)
    lay_road(w, road, kerb, edge_w=0.16, edge_h=0.07)
    plate = B.mat('plate_cienaga', '#59634f', rough=0.4, metal=0.6, clear=0.3)
    inner = B.mat('plate_in_cienaga', '#2a3226', rough=0.5, metal=0.5)
    K.pads(w, plate, rim_k=5.0, inner_mat=inner)
    K.foundation(w, plate)

    B.sun(205, 44, 2.9, (1.0, 0.93, 0.72), angle=6)
    B.sun(40, 32, 0.9, (0.45, 0.9, 0.75), angle=25, name='rim')
    B.ambient((0.38, 0.62, 0.42), 0.5)


# ---------------------------------------------------------------------------
# Islands and what lies beyond them (nebula, nucleo)
# ---------------------------------------------------------------------------

def smax(*a, k=5.0):
    """a smooth maximum: the union of shapes given as distances, with the
    inner corners between them rounded"""
    m = a[0]
    for b in a[1:]:
        h = np.clip(0.5 + 0.5 * (b - m) * k, 0, 1)
        m = m * (1 - h) + b * h + h * (1 - h) / k
    return m


def island(F, sd, top_mat, side_mat, levels, name='land', seed=0):
    """The ground where sd > 0 (sd is a distance to the shore, in metres,
    negative outside), its edge following the shore smoothly, and rock
    hanging under the edge: levels is [(drop, inset, jitter), ...], each a
    ring of the cliff further down and further in."""
    gy, gx = np.gradient(sd, F.step)
    gl = np.maximum(np.hypot(gx, gy), 1e-6)
    nx_, ny_ = gx / gl, gy / gl
    inside = sd > -0.8 * F.step
    push = np.clip(-sd, 0, F.step)
    o = F.mesh(top_mat, name, mask=inside, XY=(F.X + nx_ * push, F.Y + ny_ * push))
    o.data.materials.append(side_mat)
    jit = fbm(F.X / 1.1, F.Y / 1.1, 900 + seed, 3)
    bm = bmesh.new()
    bm.from_mesh(o.data)
    bmesh.ops.delete(bm, geom=[v for v in bm.verts if not v.link_faces], context='VERTS')
    edges = [e for e in bm.edges if e.is_boundary]
    for (dz, inset, jitter) in levels:
        ret = bmesh.ops.extrude_edge_only(bm, edges=edges)
        vs = [g for g in ret['geom'] if isinstance(g, bmesh.types.BMVert)]
        edges = [g for g in ret['geom'] if isinstance(g, bmesh.types.BMEdge)]
        for f in ret['geom']:
            if isinstance(f, bmesh.types.BMFace):
                f.material_index = 1
                f.smooth = False
        for v in vs:
            x, y = v.co.x, v.co.y
            k = 1 + jitter * (float(F.at(jit, x, y)) - 0.5) * 3.0
            v.co.x += float(F.at(nx_, x, y)) * inset * k
            v.co.y += float(F.at(ny_, x, y)) * inset * k
            v.co.z -= dz * k
    bm.to_mesh(o.data)
    bm.free()
    return o


def cliff_mat(name, strata, glow_lo=None, glow_k=0.0, z0=-6.0, rough=0.75):
    """the rock under an island's edge: strata of colour by depth, and
    optionally the light from below growing towards its foot"""
    m, nt, b = new_mat(name)
    p = position(nt)
    z = N(nt, 'ShaderNodeSeparateXYZ', Vector=p).outputs['Z']
    nz = N(nt, 'ShaderNodeTexNoise', Vector=p, Scale=1.3, Detail=4.0).outputs['Fac']
    t = math_(nt, 'ADD', math_(nt, 'DIVIDE', z, z0), math_(nt, 'MULTIPLY', math_(nt, 'SUBTRACT', nz, 0.5), 0.35))
    n2 = N(nt, 'ShaderNodeTexNoise', Vector=p, Scale=7.0, Detail=3.0).outputs['Fac']
    ins(nt, b, Base_Color=ramp(nt, t, strata), Roughness=rough, Normal=bump(nt, n2, 0.5, 0.08))
    if glow_lo is not None:
        ins(nt, b, Emission=ramp(nt, t, [(0.1, '#000000'), (1.0, glow_lo)]), Emission_Strength=glow_k)
    return m


def backdrop(name, rgb, t=170.0, k=1.0):
    """A picture painted with numpy (rgb: 1280 x 720 x 3, linear, its
    first row the top of the screen) hung t metres behind the ground,
    facing the camera and filling the screen: what shows wherever there
    is no ground. Only the camera sees it, so it adds no noise."""
    h, w_ = rgb.shape[:2]
    img = bpy.data.images.new(name, w_, h, alpha=False, float_buffer=True)
    img.colorspace_settings.name = 'Linear'
    px = np.concatenate([rgb[::-1], np.ones((h, w_, 1))], 2).astype(np.float32)
    img.pixels.foreach_set(px.ravel())
    try:
        img.pack()
    except Exception as e:  # noqa
        print('backdrop: not packed:', e)
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    tex = N(nt, 'ShaderNodeTexImage', {'image': img, 'extension': 'EXTEND', 'interpolation': 'Linear'})
    em = N(nt, 'ShaderNodeEmission', Color=tex.outputs['Color'], Strength=k)
    out = nt.nodes.new('ShaderNodeOutputMaterial')
    nt.links.new(em.outputs[0], out.inputs['Surface'])
    m.cycles.sample_as_light = False
    fwd = Vector((0, B.COS_E, -B.SIN_E))
    c = Vector((0, B.SCREEN_H / 2.0 / B.ROW_Y, 0)) + fwd * t
    rx = Vector((B.SCREEN_W / 2.0 / B.PX, 0, 0))
    up = Vector((0, B.SIN_E, B.COS_E)) * (B.SCREEN_H / 2.0 / B.PX)
    over = 1.06
    bm = bmesh.new()
    uvl = bm.loops.layers.uv.new('UV')
    vs = [bm.verts.new(c + rx * sx * over + up * sy * over) for (sx, sy) in ((-1, -1), (1, -1), (1, 1), (-1, 1))]
    f = bm.faces.new(vs)
    for lp, (sx, sy) in zip(f.loops, ((-1, -1), (1, -1), (1, 1), (-1, 1))):
        lp[uvl].uv = (0.5 + 0.5 * sx * over, 0.5 + 0.5 * sy * over)
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    me.materials.append(m)
    o = bpy.data.objects.new(name, me)
    B.sc.collection.objects.link(o)
    o.visible_diffuse = False
    o.visible_glossy = False
    o.visible_shadow = False
    return o


def stars(img, n, seed, k=1.0):
    """points of light on a backdrop, a few of them big"""
    rs = np.random.RandomState(seed)
    h, w_ = img.shape[:2]
    tints = np.array([(1, 1, 1), (0.7, 0.85, 1.0), (1.0, 0.85, 0.65), (1.0, 0.7, 0.95)])
    for i in range(n):
        x, y = rs.randint(1, w_ - 1), rs.randint(1, h - 1)
        b = k * (0.25 + 1.6 * rs.rand() ** 4)
        c = tints[rs.randint(0, 4)] * b
        img[y, x] += c
        if b > 0.7 * k:
            for (dx, dy) in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                img[y + dy, x + dx] += c * 0.4


def under(x, y, z):
    """where to put something at height z so the camera sees it over the
    ground point (x, y)"""
    return (x, y - z * B.COS_E / B.SIN_E, z)


def void_spots(F, sd, n, sep, y0=7.5, y1=35.5, xmax=8.6):
    """the emptiest places off the ground: (x, y, how far from the shore)"""
    m = np.where((np.abs(F.X) < xmax) & (F.Y > y0) & (F.Y < y1), -sd, -9.0)
    out = []
    for _ in range(n):
        i = np.unravel_index(np.argmax(m), m.shape)
        if m[i] < 0.6:
            break
        x, y = F.X[i], F.Y[i]
        out.append((float(x), float(y), float(m[i])))
        m = np.where(np.hypot(F.X - x, F.Y - y) < sep, -9.0, m)
    return out


# ---------------------------------------------------------------------------
# nebula: platforms of old stone over a nebula, joined by the road
# ---------------------------------------------------------------------------

def bridge_sites(w, F, margin, sep=6.0, half=1.1):
    """Where the road may cross a gap: the middles of its straight
    stretches that have nothing of the layout to either side, so that
    cutting the ground away there leaves a bridge between two platforms.
    Returns [(x, y, dx, dy)] (a point of the road and its direction)."""
    pts = np.array([q for p in w.paths for q in p])
    pads = np.array(w.pads)

    def other(q, c):
        far = np.hypot(pts[:, 0] - c[0], pts[:, 1] - c[1]) > 3.4
        d = np.hypot(pts[far, 0] - q[0], pts[far, 1] - q[1]).min() - HALF - margin if far.any() else 9.0
        d = min(d, np.hypot(pads[:, 0] - q[0], pads[:, 1] - q[1]).min() - PAD_R - margin - 0.35)
        return min(d, math.hypot(q[0] - w.fort[0], q[1] - w.fort[1]) - FORT_R - margin - 0.3)
    out = []
    for p in w.paths:
        run = []
        for i in range(6, len(p) - 6):
            x, y = p[i]
            ok = HUD_BOT_Y + 2.5 < y < HUD_TOP_Y - 1.5 and abs(x) < 7.9
            if ok:
                ax, ay = p[i][0] - p[i - 5][0], p[i][1] - p[i - 5][1]
                bx, by = p[i + 5][0] - p[i][0], p[i + 5][1] - p[i][1]
                la, lb = math.hypot(ax, ay), math.hypot(bx, by)
                ok = (ax * bx + ay * by) / (la * lb) > 0.997
            if ok:
                dx, dy = ax / la, ay / la
                for s in (1, -1):
                    for a in (-half, 0.0, half):
                        t = HALF + margin + 0.45
                        if other((x - dy * s * t + dx * a, y + dx * s * t + dy * a), (x, y)) < 0.15:
                            ok = False
            if ok:
                run.append((x, y, dx, dy))
            if (not ok or i == len(p) - 7) and run:
                c = run[len(run) // 2]
                if all(math.hypot(c[0] - o[0], c[1] - o[1]) > sep for o in out):
                    out.append(c)
                run = []
    return out


def nebula_picture():
    yy, xx = np.mgrid[0:B.SCREEN_H, 0:B.SCREEN_W].astype(np.float64)
    u, v = xx / B.PX, yy / B.PX
    q1 = fbm(u / 7.0, v / 7.0, 51, 3)
    q2 = fbm(u / 7.0 + 9, v / 7.0 + 4, 52, 3)
    wu, wv = u + 5 * (q1 - 0.5), v + 5 * (q2 - 0.5)
    a = fbm(wu / 8.0, wv / 8.0, 53, 6)
    b = fbm(wu / 6.0 + 3, wv / 6.0 + 8, 54, 6)
    c = fbm(wu / 4.0 + 1, wv / 4.0 + 5, 55, 5)
    img = np.zeros((B.SCREEN_H, B.SCREEN_W, 3))
    img[...] = (0.006, 0.004, 0.030)
    body = smooth(0.34, 0.72, a)
    img += body[..., None] * np.array((0.22, 0.03, 0.50))
    img += (smooth(0.50, 0.80, a) ** 1.5)[..., None] * np.array((0.85, 0.05, 0.42))
    img += (smooth(0.48, 0.78, b) ** 1.5)[..., None] * np.array((0.00, 0.38, 0.80))
    img += (smooth(0.60, 0.85, c) * smooth(0.45, 0.7, a))[..., None] * np.array((1.0, 0.55, 0.16))
    img += (smooth(0.66, 0.9, b) * smooth(0.5, 0.8, c))[..., None] * np.array((0.5, 0.9, 1.0)) * 0.7
    dust = smooth(0.50, 0.70, fbm(wu / 4.0 + 20, wv / 4.0, 57, 5))
    img *= (1 - 0.6 * dust)[..., None]
    img *= 1.45
    stars(img, 1300, 58, 1.3)
    return img


def stone_mat():
    """the platforms' tops: flagstones in the vertex colour, their joints
    lit where the old magic still runs ('Glow')"""
    m, nt, b = new_mat('stone')
    p = position(nt)
    col = attr(nt, 'Col')
    edge = N(nt, 'ShaderNodeTexVoronoi', {'feature': 'DISTANCE_TO_EDGE'}, Vector=p, Scale=1.5).outputs['Distance']
    cell = N(nt, 'ShaderNodeSeparateRGB', Image=N(nt, 'ShaderNodeTexVoronoi', {'feature': 'F1'}, Vector=p,
                                                  Scale=1.5).outputs['Color']).outputs[0]
    joint = ramp(nt, edge, [(0.0, 0.0), (0.05, 1.0)])
    nz = N(nt, 'ShaderNodeTexNoise', Vector=p, Scale=9.0, Detail=4.0).outputs['Fac']
    col = mix(nt, math_(nt, 'MULTIPLY', cell, 0.30), col, '#5a5cb0', 'MIX')
    col = mix(nt, math_(nt, 'MULTIPLY', nz, 0.35), col, '#080818', 'MIX')
    col = mix(nt, joint, '#080818', col)
    glow = attr(nt, 'Glow')
    lines = mix(nt, joint, glow, '#000000')
    ins(nt, b, Base_Color=col, Roughness=0.55, Specular=0.5, Clearcoat=0.2,
        Normal=bump(nt, math_(nt, 'ADD', joint, math_(nt, 'MULTIPLY', nz, 0.3)), 0.6, 0.05),
        Emission=mix(nt, 0.35, lines, glow), Emission_Strength=1.0)
    return m


def float_rock(bt, rnd, x, y, z, r, c0, c1):
    """a rock adrift: flat and mossy on top, a ragged point below"""
    vs = bmesh.ops.create_icosphere(bt.bm, subdivisions=2, radius=r)['verts']
    for v_ in vs:
        v_.co *= 1 + rnd.uniform(-0.22, 0.22)
        v_.co.z = v_.co.z * 0.22 if v_.co.z > 0 else v_.co.z * rnd.uniform(1.3, 2.1)
    faces = list({f for v_ in vs for f in v_.link_faces})
    bt._done(vs, faces, TM(x, y, z, rnd.uniform(0, 360)), c0, c1, r * 1.2, z0=-r, rnd=rnd, vary=0.12)


def stone_ring(x, y, z, R, tube, tilt, yaw, stone, glow, rnd, lamps=10):
    """one of the rings the world is named after: a hoop of carved stone
    with a line of light inside it and lamps round its face"""
    g = B.group('ring', loc=(x, y, z), rot=(tilt, 0, yaw))
    B.torus(R, tube, stone, g, segs=72, tsegs=6)
    B.torus(R - tube * 0.92, tube * 0.22, glow, g, segs=72, tsegs=6)
    for i in range(lamps):
        a = 2 * math.pi * (i + 0.5) / lamps
        B.box((tube * 0.5, tube * 0.9, tube * 0.3), glow, g, loc=(R * math.cos(a), R * math.sin(a), tube * 0.86),
              rot=(0, 0, math.degrees(a)), bevel=0.0)
    return g


def build_nebula(w):
    rnd = random.Random(808)
    F = Field(w)
    X, Y = F.X, F.Y

    # The ground is only what the game needs: a causeway under the road,
    # a plate under every pad and the fort. Whatever the layout leaves
    # empty is open sky, and where the road has nothing to either side it
    # is cut loose into a bridge.
    rag = (fbm(X / 1.6, Y / 1.6, 31, 3) - 0.5) * 2
    MARGIN = 0.42
    road_sd = HALF + MARGIN + 0.22 * rag - F.d_road
    cuts = bridge_sites(w, F, MARGIN)
    print('nebula: bridges at', [(round(c[0], 1), round(c[1], 1)) for c in cuts])
    for (cx, cy, dx, dy) in cuts:
        along = (X - cx) * dx + (Y - cy) * dy
        across = -(X - cx) * dy + (Y - cy) * dx
        road_sd = np.minimum(road_sd, np.maximum(np.abs(along) - 1.15 - 0.15 * rag, np.abs(across) - 3.2))
    pad_sd = PAD_R + MARGIN + 0.08 + 0.25 * rag - F.d_pad
    fort_sd = FORT_R + 0.75 + 0.2 * rag - F.d_fort
    sd = smax(road_sd, pad_sd, fort_sd, k=3.0)

    # gardens: round terraces grown from the platforms' edges into the
    # empty places, for the ruins to stand on
    gardens = []
    for _ in range(4000):
        if len(gardens) >= 7:
            break
        x, y = rnd.uniform(-8.6, 8.6), rnd.uniform(7.2, 36.0)
        s0 = float(F.at(sd, x, y))
        if not (-2.1 < s0 < -1.0) or not K.clear(w, x, y, 1.0):
            continue
        R = min(1.8, -s0 + 0.45)
        if any(math.hypot(x - c[0], y - c[1]) < R + 2.0 for c in cuts):
            continue
        if any(math.hypot(x - g[0], y - g[1]) < 6.0 for g in gardens):
            continue
        if max_height(F, x, y, 0.5) < 1.6:
            continue
        gardens.append((x, y, R))
    for (x, y, R) in gardens:
        sd = smax(sd, R * (1 + 0.12 * rag) - np.hypot(X - x, Y - y), k=4.0)

    H = 0.10 * fbm(X / 1.3, Y / 1.3, 32, 3) + 0.05
    for (x, y, R) in gardens:
        H = H + 0.28 * smooth(R * 0.95, R * 0.55, np.hypot(X - x, Y - y))
    F.relief(H, ramp_w=1.0)

    tone = fbm(X / 2.2, Y / 2.2, 33, 3)
    F.col = shade([(0.25, '#1a1b40'), (0.6, '#25265a'), (0.9, '#33347a')], tone)
    moss = (smooth(1.3, 0.25, sd) * smooth(0.42, 0.6, fbm(X / 1.4 + 5, Y / 1.4, 34, 3)) * smooth(0.25, 0.7, F.gap))[..., None]
    F.col = F.col * (1 - moss) + lin3('#18b89a') * moss
    F.glow += moss * lin3('#18e0b0') * 0.10
    # light in the joints of the paving round every pad and round the fort
    halo = smooth(1.0, 0.0, F.d_pad - PAD_R) * smooth(-0.05, 0.1, F.d_pad - PAD_R)
    halo = np.maximum(halo, smooth(1.3, 0.0, F.d_fort - FORT_R) * smooth(-0.05, 0.1, F.d_fort - FORT_R))
    F.glow += halo[..., None] * lin3(w.accent) * 0.9

    cliff = cliff_mat('cliff_nebula', [(0.0, '#3c3c80'), (0.10, '#232350'), (0.25, '#34306a'), (0.45, '#1a1838'),
                                       (0.8, '#281c4c')], glow_lo='#7a20c0', glow_k=0.22, z0=-5.5)
    island(F, sd, stone_mat(), cliff, [(0.18, -0.04, 0.0), (0.9, 0.22, 0.25), (1.6, 0.6, 0.4), (2.2, 0.9, 0.5)])

    marble = tint_mat('marble', rough=0.32, clear=0.3, grain=0.3, grain_scale=6.0)
    lamp = tint_mat('lamp', rough=0.2, emit=2.2, edge=0.5)
    gem = gem_mat('gem_nebula', emit=0.7, edge=1.4)
    rockm = tint_mat('rock_nebula', rough=0.7, grain=0.5, grain_scale=4.0)
    bs, bl, bg, br = Batch('marble', marble), Batch('lamps', lamp, True), Batch('gems', gem), Batch('rocks', rockm)
    S0, S1 = '#8a84c0', '#e4e0ff'
    LIGHTS = ['#40e8ff', '#c080ff', '#ff70d0', '#ffd060']
    ring_stone = B.noise_mat('ring_stone', '#5a5390', '#9a94d0', scale=2.5, rough=0.45, bump=0.3)

    def column(x, y, z, h, light, whole=True):
        r = 0.24
        bs.cone(TM(x, y, z - 0.15), r * 1.7, 0.33, S0, S0, r2=r * 1.45, segs=8)
        bs.cone(TM(x, y, z + 0.18), r, h - 0.18, S0, S1, r2=r * 0.84, segs=10)
        bl.cone(TM(x, y, z + h * 0.62), r * 0.95, 0.09, light, light, r2=r * 0.94, segs=10)
        if whole:
            bs.cone(TM(x, y, z + h), r * 0.9, 0.2, S1, S1, r2=r * 1.6, segs=8)
            bl.ball(TM(x, y, z + h + 0.42), 0.16, light, '#ffffff', segs=10)
        else:
            bs.rock(TM(x, y, z + h, rnd.uniform(0, 360), scale=(1, 1, 0.5)), r * 0.9, S0, S1, rnd, subdiv=1)

    kinds = ['gate', 'columns', 'beacon', 'crystal']
    F_lamps = []
    for i, (x, y, R) in enumerate(gardens):
        z = F.z(x, y)
        hmax = max_height(F, x, y, 0.6)
        light = LIGHTS[i % len(LIGHTS)]
        kind = kinds[i % len(kinds)]
        F_glow = (x, y, R * 0.9, light, 0.22)
        if kind == 'gate':
            # a ring standing on its edge, a gate to nowhere
            Rr = min(1.25, R * 0.62, hmax / 2.2)
            yaw = rnd.choice([-1, 1]) * rnd.uniform(48, 62)
            stone_ring(x, y, z + Rr * 0.86, Rr, 0.17, 90, yaw, ring_stone, B.glow('glow_' + light, light, 5.0), rnd, lamps=8)
            bs.cone(TM(x, y, z - 0.1, yaw, scale=(1.6, 0.6, 1)), 0.5, 0.36, S0, S1, r2=0.4, segs=8)
        elif kind == 'columns':
            n = rnd.randint(3, 4)
            for k in range(n):
                a = math.radians(360.0 * k / n + rnd.uniform(-15, 15))
                cx, cy = x + R * 0.5 * math.cos(a), y + R * 0.5 * math.sin(a)
                whole = rnd.random() < 0.6
                column(cx, cy, F.z(cx, cy), min(hmax, rnd.uniform(2.6, 4.2) if whole else rnd.uniform(0.9, 1.8)), light, whole)
            bl.ball(TM(x, y, z + 0.5), 0.2, light, '#ffffff')
        elif kind == 'beacon':
            # a shard of crystal held in the air over a plinth, in a hoop
            hh = min(hmax, 3.6)
            bs.cone(TM(x, y, z - 0.1), 0.62, 0.5, S0, S1, r2=0.42, segs=6)
            cz = z + 0.5 + hh * 0.45
            bg.spire(TM(x, y, cz), 0.3, hh * 0.5, light, '#ffffff', segs=4, tip=0.75, sink=0.0, base=1.0)
            bg.spire(TM(x, y, cz, 0, 180, 0), 0.3, hh * 0.3, light, '#ffffff', segs=4, tip=0.75, sink=0.0, base=1.0)
            stone_ring(x, y, cz, 0.62, 0.07, rnd.uniform(15, 35), rnd.uniform(0, 360), ring_stone,
                       B.glow('glow_' + light, light, 5.0), rnd, lamps=6)
        else:
            c = rnd.choice([('#2a50d0', '#70e8ff'), ('#6a28d0', '#e090ff')])
            crystal_cluster(bg, rnd, x, y, z - 0.05, min(hmax, rnd.uniform(2.4, 3.8)), 0.36, c[0], c[1], n=5)
        F.add_glow(*F_glow)

    # rubble, small crystals and little lights wherever there is ground
    on_land = lambda x, y: F.at(sd, x, y) > 0.35  # noqa: E731
    taken = [(g[0], g[1], g[2] * 0.6) for g in gardens]
    for (x, y) in scatter(F, rnd, 110, margin=0.4, test=on_land, taken=taken, r=0.15, xmax=9.0):
        z = F.z(x, y)
        k = rnd.random()
        lim = 0.2 + 0.5 * F.at(F.gap, x, y)
        if k < 0.4:
            br.rock(TM(x, y, z, rnd.uniform(0, 360), scale=(1, 0.8, 0.6)), rnd.uniform(0.08, 0.2), '#3a3468', '#8a84c0', rnd,
                    subdiv=1)
        elif k < 0.75:
            c = rnd.choice([('#2a50d0', '#70e8ff'), ('#6a28d0', '#e090ff'), ('#1a9a90', '#90ffe0')])
            for j in range(rnd.randint(2, 3)):
                bg.spire(TM(x + rnd.uniform(-0.1, 0.1), y + rnd.uniform(-0.1, 0.1), z - 0.03, 0, rnd.uniform(5, 30),
                            rnd.uniform(0, 360)), rnd.uniform(0.06, 0.1), min(lim, rnd.uniform(0.2, 0.5)), c[0], c[1],
                         segs=5, tip=0.5, sink=0.1)
        else:
            c = rnd.choice(LIGHTS)
            bs.cone(TM(x, y, z - 0.05), 0.07, min(lim, 0.32), S0, S1, r2=0.05, segs=6)
            bl.ball(TM(x, y, z + min(lim, 0.32) + 0.06), 0.08, c, '#ffffff', segs=8)
    # low lanterns along the causeway, on one side and then the other
    for p in w.paths:
        nrm = K._normals(p)
        for i in range(4, len(p) - 2, 9):
            s_ = 1 if (i // 9) % 2 else -1
            x, y = p[i][0] + nrm[i][0] * s_ * (HALF + 0.27), p[i][1] + nrm[i][1] * s_ * (HALF + 0.27)
            if F.at(sd, x, y) < 0.08 or F.at(F.d_pad, x, y) < PAD_R + 0.2 or F.at(F.d_fort, x, y) < FORT_R + 0.2:
                continue
            if any(abs((x - c[0]) * c[2] + (y - c[1]) * c[3]) < 2.0 and math.hypot(x - c[0], y - c[1]) < 3.0 for c in cuts):
                continue
            bs.cone(TM(x, y, -0.05), 0.09, 0.30, S0, S1, r2=0.07, segs=6)
            bl.ball(TM(x, y, 0.33), 0.085, '#ffd060', '#ffffff', segs=8)
            F_lamps.append((x, y))
    # lanterns at both ends of every bridge
    for (cx, cy, dx, dy) in cuts:
        for a in (-1.55, 1.55):
            for s in (-1, 1):
                x, y = cx + dx * a - dy * s * (HALF + 0.38), cy + dy * a + dx * s * (HALF + 0.38)
                bs.cone(TM(x, y, -0.05), 0.11, 0.38, S0, S1, r2=0.08, segs=6)
                bl.ball(TM(x, y, 0.42), 0.1, '#ffd060', '#ffffff', segs=8)

    # the void: great rings adrift below, and rocks at every depth
    for i, (x, y, room) in enumerate(void_spots(F, sd, 4, 7.0)):
        z = -rnd.uniform(2.5, 4.5)
        R = max(1.6, min(4.2, room * 1.25))
        light = LIGHTS[i % len(LIGHTS)]
        px, py, pz = under(x, y, z)
        stone_ring(px, py, pz, R, 0.26 + 0.03 * R, rnd.uniform(25, 50), rnd.uniform(0, 360), ring_stone,
                   B.glow('glow_' + light, light, 5.0), rnd, lamps=int(R * 5))
        bg.spire(TM(px, py, pz), R * 0.16, R * 0.55, light, '#ffffff', segs=4, tip=0.75, sink=0.0, base=1.0)
        bg.spire(TM(px, py, pz, 0, 180, 0), R * 0.16, R * 0.4, light, '#ffffff', segs=4, tip=0.75, sink=0.0, base=1.0)
    n = 0
    for _ in range(3000):
        if n >= 46:
            break
        x, y = rnd.uniform(-9.5, 9.5), rnd.uniform(1.0, 40.0)
        s0 = float(F.at(sd, x, y))
        if s0 > -0.7 or (y < HUD_BOT_Y and rnd.random() < 0.6):
            continue
        z = -rnd.uniform(0.6, 7.0)
        r = rnd.uniform(0.2, min(0.9, 0.25 - s0 * 0.45))
        float_rock(br, rnd, *under(x, y, z), r, '#2a1c48', rnd.choice(['#18b89a', '#6a60b0', '#6a60b0']))
        if r > 0.5 and rnd.random() < 0.6:
            px, py, pz = under(x, y, z)
            c = rnd.choice([('#2a50d0', '#70e8ff'), ('#6a28d0', '#e090ff')])
            bg.spire(TM(px, py, pz + r * 0.1, 0, rnd.uniform(0, 15), rnd.uniform(0, 360)), r * 0.22, r * 1.3, c[0], c[1])
        n += 1
    for b_ in (bs, bl, bg, br):
        b_.finish()
    backdrop('nebula', nebula_picture())

    roadm = cobble_mat('road_nebula', '#9a92c4', '#b8b0de', '#3a3068', scale=2.2, rough=0.5, depth=0.4, seed_off=5.0)
    kerb = B.mat('kerb_nebula', '#e4e0ff', rough=0.3, clear=0.5)
    lay_road(w, roadm, kerb, edge_w=0.14, edge_h=0.07, side=-0.5)
    plate = B.mat('plate_nebula', '#7c78ac', rough=0.35, metal=0.7, clear=0.4)
    inner = B.mat('plate_in_nebula', '#231c3c', rough=0.5, metal=0.5)
    K.pads(w, plate, rim_k=5.0, inner_mat=inner)
    K.foundation(w, plate)

    B.sun(205, 46, 3.3, (0.92, 0.90, 1.0), angle=5)
    B.sun(40, 30, 1.3, (1.0, 0.45, 0.9), angle=25, name='rim')
    B.ambient((0.42, 0.32, 0.85), 0.5)


# ---------------------------------------------------------------------------
# colmena: the hive. A floor of wax cells, honey, brood, resin
# ---------------------------------------------------------------------------

def hive_cells(F, rnd, R, wax, lit):
    """The honeycomb: one hexagonal cell on every place of a lattice that
    does not touch the road, a pad or the fort, and smaller cells in the
    room that is left beside them. A cell is a column with a rim; inside
    it is capped with wax, or open on honey, on a grub's light, or on the
    dark. Returns the big cells as (x, y, top, kind, room)."""
    bm = bmesh.new()
    col = bm.loops.layers.color.new('Col')
    out = []
    ang = [math.radians(60 * k) for k in range(6)]
    WALLS = ['#b87414', '#cf8a1c', '#e09c2a', '#c27a10']

    def paint(f, c, k=1.0):
        c = B.rgb(c)
        for lp in f.loops:
            lp[col] = (min(1, c[0] * k), min(1, c[1] * k), min(1, c[2] * k), 1.0)

    def add(cx, cy, R, h, kind):
        ro, ri = R * 0.955, R * (0.74 if R > 0.4 else 0.66)
        d = {'cap': -0.02, 'honey': 0.09, 'grub': 0.16, 'dark': 0.38}[kind]
        d = min(d, h + 0.3)
        A = [bm.verts.new((cx + ro * math.cos(a), cy + ro * math.sin(a), -0.7)) for a in ang]
        Bt = [bm.verts.new((cx + ro * math.cos(a), cy + ro * math.sin(a), h)) for a in ang]
        C = [bm.verts.new((cx + ri * math.cos(a), cy + ri * math.sin(a), h + 0.015)) for a in ang]
        D = [bm.verts.new((cx + ri * 0.94 * math.cos(a), cy + ri * 0.94 * math.sin(a), h - d)) for a in ang]
        wall = rnd.choice(WALLS)
        tone = rnd.uniform(0.85, 1.1)
        for a_ in range(6):
            b_ = (a_ + 1) % 6
            f = bm.faces.new((A[a_], A[b_], Bt[b_], Bt[a_]))
            for lp in f.loops:
                c = B.rgb('#4a2408') if lp.vert.co.z < 0 else B.rgb(wall)
                lp[col] = (c[0] * tone, c[1] * tone, c[2] * tone, 1.0)
            paint(bm.faces.new((Bt[a_], Bt[b_], C[b_], C[a_])), '#f0b840', tone)
            paint(bm.faces.new((C[a_], C[b_], D[b_], D[a_])), '#6a3a0c' if kind != 'cap' else '#f0d070', tone)
        if kind == 'cap':
            top = bm.verts.new((cx, cy, h + 0.02 + rnd.uniform(0.04, 0.10) * R / 0.58))
            for a_ in range(6):
                paint(bm.faces.new((D[a_], D[(a_ + 1) % 6], top)), rnd.choice(['#f2d478', '#eccb68', '#f6dc8c']), tone)
        else:
            f = bm.faces.new(D)
            if kind == 'honey':
                paint(f, rnd.choice(['#ff9a08', '#ffb010', '#ff8400']))
                f.material_index = 1
            elif kind == 'grub':
                paint(f, '#0a6a5c')
                f.material_index = 1
            else:
                paint(f, '#1c0c04')
    for i in range(-13, 14):
        for j in range(-2, 44):
            cx = 1.5 * R * i
            cy = math.sqrt(3) * R * (j + 0.5 * (i & 1))
            g = float(F.at(F.gap, cx, cy)) - R
            if g < 0.07:
                continue
            calm = float(calm_rows(cy))
            region = float(fbm(cx / 6.0 + 3, cy / 6.0, 71, 3))
            h = 0.10 + 2.3 * float(smooth(0.42, 0.78, region)) ** 1.4 + 1.5 * float(smooth(6.8, 9.6, abs(cx)))
            h = h * (0.35 + 0.65 * calm) + rnd.uniform(-0.10, 0.14)
            h = max(0.05, min(h, 0.16 + 0.85 * max(g - 0.25, 0.0)))
            brood = float(fbm(cx / 4.0 + 20, cy / 4.0 + 7, 72, 2))
            k = rnd.random()
            if brood > 0.60 and k < 0.55 and calm > 0.5:
                kind = 'grub'
            elif brood < 0.44 and k < 0.6:
                kind = 'honey'
            elif k < 0.22:
                kind = 'dark'
            else:
                kind = 'cap'
            add(cx, cy, R, h, kind)
            out.append((cx, cy, h, kind, g))
    # the small cells, wherever a big one found no room
    big = np.array([(c[0], c[1]) for c in out])
    r = R * 0.5
    for i in range(-26, 27):
        for j in range(-4, 88):
            cx = 1.5 * r * i + 0.11
            cy = math.sqrt(3) * r * (j + 0.5 * (i & 1)) + 0.07
            g = float(F.at(F.gap, cx, cy)) - r
            if g < 0.06 or np.hypot(big[:, 0] - cx, big[:, 1] - cy).min() < 0.9 * (R + r):
                continue
            k = rnd.random()
            add(cx, cy, r, max(0.03, min(rnd.uniform(0.03, 0.14), 0.05 + 0.5 * g)), 'honey' if k < 0.15 else 'dark' if k < 0.35 else 'cap')
    me = bpy.data.meshes.new('cells')
    bm.normal_update()
    bm.to_mesh(me)
    bm.free()
    me.materials.append(wax)
    me.materials.append(lit)
    o = bpy.data.objects.new('cells', me)
    B.sc.collection.objects.link(o)
    return out


def hive_spire(x, y, z, h, r, rnd, body, window, dark):
    """a tower of the hive: a lumpy cone of chewed wax with lit openings
    and a chimney"""
    ph = rnd.uniform(0, 6)
    prof = []
    n = 12
    for k in range(n + 1):
        t = k / float(n)
        rad = r * (1 - 0.72 * t ** 1.25) * (1 + 0.16 * math.sin(t * 15 + ph))
        prof.append((rad, -0.3 + (h + 0.3) * t))
    g = B.group('spire', loc=(x, y, z))
    o = B.lathe(prof, body, g, segs=28)
    B.subsurf(o, 1)
    B.displace(o, 0.22, 0.55, seed=1)
    top = prof[-1][0]
    B.torus(top * 0.95, top * 0.33, dark, g, loc=(0, 0, h), segs=20, tsegs=8)
    B.cyl(top * 0.8, 0.05, window, g, loc=(0, 0, h - 0.02), segs=16)
    for row, t in enumerate((0.2, 0.42, 0.63, 0.8)):
        k = int(t * n)
        rad = prof[k][0]
        cnt = 4 - row // 2
        for c in range(cnt):
            a = math.radians(190 + 160.0 * (c + 0.5 + 0.35 * (row % 2) - 0.2) / cnt + rnd.uniform(-8, 8))
            s = 0.11 + 0.035 * h * (1 - t)
            B.sphere(1.0, dark, g, loc=(rad * 0.93 * math.cos(a), rad * 0.93 * math.sin(a), h * t), rot=(0, 0, math.degrees(a)),
                     scale=(s * 0.8, s * 1.25, s * 1.7), segs=12)
            B.sphere(1.0, window, g, loc=(rad * 0.96 * math.cos(a), rad * 0.96 * math.sin(a), h * t), rot=(0, 0, math.degrees(a)),
                     scale=(s * 0.7, s * 0.9, s * 1.3), segs=12)
    return g


def build_colmena(w):
    rnd = random.Random(909)
    F = Field(w)
    X, Y = F.X, F.Y

    # the floor under the cells: dark resin, a little lighter by the road
    F.relief(0.03 * fbm(X / 0.8, Y / 0.8, 73, 2), ramp_w=0.6)
    F.col = shade([(0.2, '#2c1608'), (0.8, '#4c2a0e')], fbm(X / 1.5, Y / 1.5, 74, 3))
    m, nt, b = new_mat('resin_floor')
    p = position(nt)
    nz = N(nt, 'ShaderNodeTexNoise', Vector=p, Scale=5.0, Detail=4.0, Distortion=1.0).outputs['Fac']
    ins(nt, b, Base_Color=mix(nt, math_(nt, 'MULTIPLY', nz, 0.5), attr(nt, 'Col'), '#8a4a10', 'MIX'), Roughness=0.3, Clearcoat=0.5,
        Normal=bump(nt, nz, 0.5, 0.05), Emission=attr(nt, 'Glow'), Emission_Strength=1.0)

    wax = tint_mat('wax', rough=0.42, clear=0.25, grain=0.28, grain_scale=7.0)
    lit = tint_mat('honey', rough=0.08, clear=1.0, emit=1.5)
    R = 0.58
    cell = hive_cells(F, rnd, R, wax, lit)

    podm = tint_mat('hive_pod', rough=0.22, clear=0.6, emit=0.9, edge=0.9)
    resin = tint_mat('resin', rough=0.08, clear=1.0, emit=0.55, edge=1.2, spec=0.9)
    chitin = tint_mat('chitin', rough=0.3, clear=0.6, grain=0.3, grain_scale=8.0)
    bp, bres, bc = Batch('pods', podm, True), Batch('resin', resin, True), Batch('chitin', chitin, True)

    # a grub's egg in every brood cell that has the height for it
    for (x, y, h, kind, g) in cell:
        if kind == 'grub':
            r = min(0.30, 0.14 + 0.25 * g)
            bp.ball(TM(x, y, h - 0.16 + r * 0.9, scale=(1, 1, 1.25)), r, '#0fae8c', '#b8ffe0', segs=12)
            F.add_glow(x, y, 0.9, '#20e0b0', 0.10)
        elif kind == 'honey' and rnd.random() < 0.3:
            F.add_glow(x, y, 0.9, '#ff9a10', 0.10)

    def top_at(x, y):
        """the top of the cell nearest to a point"""
        best = min(cell, key=lambda c: (c[0] - x) ** 2 + (c[1] - y) ** 2)
        return best[2]

    body = B.noise_mat('hive_body', '#9a5a12', '#e0a030', scale=2.2, rough=0.55, bump=0.5, distortion=1.5)
    window = B.glow('hive_window', '#ffd040', 4.0)
    dark = B.mat('hive_dark', '#3a1a08', rough=0.5)
    taken = []
    # towers at the mouths of the tunnels the roads come out of
    for p in w.paths:
        q = next(((i, a) for i, a in enumerate(p) if abs(a[0]) < 8.8 and a[1] < 39.5), None)
        if q is None or q[1][1] > HUD_TOP_Y - 1.0:
            continue
        i, (qx, qy) = q
        nx_, ny_ = K._normals(p)[i]
        for s_ in (1, -1):
            x, y = qx + nx_ * s_ * 2.75, qy + ny_ * s_ * 2.75
            if K.clear(w, x, y, 0.95):
                h = min(max_height(F, x, y, 0.9), rnd.uniform(3.6, 4.6))
                if h > 1.5:
                    hive_spire(x, y, top_at(x, y) - 0.1, h, 0.95, rnd, body, window, dark)
                    taken.append((x, y, 0.95))
    for (x, y, h) in sites(F, rnd, 8, 1.0, hmin=2.2, top=6.0, sep=4.6, taken=taken):
        h = min(h, rnd.uniform(3.4, 5.6))
        hive_spire(x, y, top_at(x, y) - 0.1, h, 0.75 + 0.07 * h, rnd, body, window, dark)
        F.add_glow(x, y, 1.6, '#ffb020', 0.12)
    # brood sacs in a cradle of claws
    SACS = [('#8a1cb0', '#ff80ec'), ('#b01c70', '#ff9ac0'), ('#5a2ad0', '#c0a0ff')]
    for (x, y, h) in sites(F, rnd, 9, 0.85, hmin=1.2, top=2.6, sep=3.0, taken=taken):
        z = top_at(x, y)
        c = rnd.choice(SACS)
        r = min(0.55, h * 0.3)
        bp.ball(TM(x, y, z + r * 1.15, scale=(1, 1, 1.35)), r, c[0], c[1], segs=16)
        for k in range(rnd.randint(1, 3)):
            a = rnd.uniform(0, 6.28)
            rr = r * rnd.uniform(0.45, 0.65)
            bp.ball(TM(x + (r + rr * 0.6) * math.cos(a), y + (r + rr * 0.6) * math.sin(a), z + rr, scale=(1, 1, 1.3)), rr, c[0], c[1],
                    segs=12)
        n = rnd.randint(4, 5)
        for k in range(n):
            a = 2 * math.pi * k / n + rnd.uniform(-0.2, 0.2)
            pts = []
            for i in range(7):
                t = i / 6.0
                d = r * (1.45 - 1.0 * t * t)
                pts.append((x + d * math.cos(a), y + d * math.sin(a), z - 0.15 + min(h, r * 3.3) * math.sin(t * 1.45)))
            bc.tube(pts, 0.10, '#3a1206', '#b0421a', r_end=0.02, segs=7)
        F.add_glow(x, y, 1.3, c[1], 0.2)
    # resin: stalagmites of amber glass
    for (x, y, h) in sites(F, rnd, 16, 0.5, hmin=0.8, top=2.4, sep=2.0, taken=taken, y0=1.0, y1=39.0):
        z = top_at(x, y)
        for k in range(rnd.randint(3, 5)):
            a = rnd.uniform(0, 6.28)
            d = rnd.uniform(0.0, 0.35) if k else 0.0
            hh = min(h, rnd.uniform(0.9, 2.0)) * (1.0 if k == 0 else rnd.uniform(0.35, 0.7))
            bres.cone(TM(x + d * math.cos(a), y + d * math.sin(a), z - 0.1, 0, rnd.uniform(0, 8), rnd.uniform(0, 360)),
                      0.11 + 0.09 * hh, hh, '#b84a04', '#ffd050', r2=0.02, segs=9)
        F.add_glow(x, y, 0.9, '#ff9a10', 0.16)
    # drops of resin and small eggs on the cells, low enough for the road's side
    for (x, y) in scatter(F, rnd, 120, margin=0.4, taken=taken, r=0.15):
        z = top_at(x, y)
        lim = 0.12 + 0.4 * F.at(F.gap, x, y)
        if rnd.random() < 0.55:
            r = min(rnd.uniform(0.07, 0.16), lim)
            bres.ball(TM(x, y, z + r * 0.5, scale=(1, 1, 0.75)), r, '#d86a08', '#ffd050', segs=10)
        else:
            c = rnd.choice(SACS + [('#0fae8c', '#b8ffe0')])
            for k in range(rnd.randint(2, 4)):
                r = min(rnd.uniform(0.06, 0.11), lim)
                bp.ball(TM(x + rnd.uniform(-0.15, 0.15), y + rnd.uniform(-0.15, 0.15), z + r * 0.8, scale=(1, 1, 1.3)), r, c[0],
                        c[1], segs=8)
    for b_ in (bp, bres, bc):
        b_.finish()
    F.mesh(m)

    roadm = cobble_mat('road_colmena', '#9c907c', '#b8ac96', '#42301c', scale=2.0, rough=0.55, depth=0.4, seed_off=7.0)
    kerb = B.mat('kerb_colmena', '#3e2010', rough=0.4, clear=0.4)
    lay_road(w, roadm, kerb, edge_w=0.16, edge_h=0.07)
    plate = B.mat('plate_colmena', '#454a5c', rough=0.35, metal=0.7, clear=0.4)
    inner = B.mat('plate_in_colmena', '#22242e', rough=0.5, metal=0.5)
    K.pads(w, plate, rim_k=5.0, inner_mat=inner)
    K.foundation(w, plate)

    B.sun(205, 42, 3.3, (1.0, 0.90, 0.72), angle=5)
    B.sun(40, 30, 1.1, (0.62, 0.45, 1.0), angle=25, name='rim')
    B.ambient((0.58, 0.42, 0.72), 0.45)


# ---------------------------------------------------------------------------
# nucleo: a shelf of obsidian over the void, and the black hole below
# ---------------------------------------------------------------------------

def blackhole_picture(cx, cy, Rh, tilt=-16.0, q=0.30):
    """The void behind the last world, painted: dust, a few stars and, if
    Rh > 0, a black hole at the screen's pixel (cx, cy) with a horizon of
    radius Rh pixels: the disc of fire round it seen nearly edge on, its
    far side bent over the top and under the bottom by the hole's
    gravity."""
    yy, xx = np.mgrid[0:B.SCREEN_H, 0:B.SCREEN_W].astype(np.float64)
    img = np.zeros((B.SCREEN_H, B.SCREEN_W, 3))
    dust = fbm(xx / 240.0, yy / 240.0, 81, 5)
    img += (smooth(0.48, 0.8, dust) ** 1.5)[..., None] * np.array((0.10, 0.008, 0.03))
    img += (smooth(0.5, 0.8, fbm(xx / 300.0 + 7, yy / 300.0, 84, 4)) ** 2)[..., None] * np.array((0.02, 0.0, 0.07))
    stars(img, 420, 82, 0.7)
    if Rh <= 0:
        return img
    HEAT = [(0.0, (0, 0, 0)), (0.12, (0.22, 0.0, 0.06)), (0.40, (0.95, 0.03, 0.10)), (0.66, (1.0, 0.33, 0.06)),
            (0.88, (1.0, 0.8, 0.32)), (1.1, (1.0, 1.0, 0.9))]
    dx, dy = xx - cx, yy - cy
    r = np.hypot(dx, dy) / Rh
    ca, sa = math.cos(math.radians(tilt)), math.sin(math.radians(tilt))
    ex, ey = (dx * ca + dy * sa) / Rh, (-dx * sa + dy * ca) / Rh
    rho = np.hypot(ex, ey / q)
    th = np.arctan2(ey / q, ex)
    th2 = np.mod(th, 2 * math.pi)
    wgt = 0.5 + 0.5 * np.cos(th)
    streak = wgt * fbm(rho * 4.5, th * 1.1 + rho * 0.9, 83, 4) + (1 - wgt) * fbm(rho * 4.5 + 9, th2 * 1.1 + rho * 0.9, 85, 4)
    prof = smooth(1.30, 1.65, rho) * (1 - smooth(1.6, 4.8, rho)) ** 1.5
    dop = 0.62 + 0.48 * np.cos(th - 2.7)
    inten = np.clip(prof * (0.35 + 1.25 * streak) * dop * 1.7, 0, 1.1)
    disc = shade(HEAT, inten)
    alpha = smooth(0.02, 0.22, inten)
    img += np.exp(-r / 2.4)[..., None] * np.array((0.34, 0.03, 0.02))
    ang = np.abs(np.sin(np.arctan2(dy, dx)))
    ring = np.exp(-((r - 1.07) / 0.05) ** 2) * 1.1 + np.exp(-((r - 1.30) / 0.26) ** 2) * 0.62 * (0.45 + 0.55 * ang)
    img += shade(HEAT, np.clip(ring, 0, 1.1))
    img *= smooth(1.0 - 1.0 / Rh, 1.0 + 1.0 / Rh, r)[..., None]
    back = (alpha * smooth(0.02, -0.02, ey) * smooth(1.0, 1.0 + 2.0 / Rh, r))[..., None]
    img += disc * back
    front = (alpha * smooth(-0.02, 0.02, ey))[..., None]
    img = img * (1 - front) + disc * front
    return img


def obsidian_mat():
    m, nt, b = new_mat('obsidian')
    p = position(nt)
    facet = N(nt, 'ShaderNodeTexVoronoi', {'feature': 'F1'}, Vector=p, Scale=3.2).outputs['Color']
    fine = N(nt, 'ShaderNodeTexNoise', Vector=p, Scale=16.0, Detail=3.0).outputs['Fac']
    tone = N(nt, 'ShaderNodeSeparateRGB', Image=facet).outputs[0]
    col = mix(nt, math_(nt, 'MULTIPLY', tone, 0.4), attr(nt, 'Col'), '#2a2244', 'MIX')
    h = math_(nt, 'ADD', math_(nt, 'MULTIPLY', tone, 0.8), math_(nt, 'MULTIPLY', fine, 0.2))
    ins(nt, b, Base_Color=col, Roughness=ramp(nt, fine, [(0.3, 0.10), (0.7, 0.28)]), Specular=0.9, Clearcoat=1.0,
        Clearcoat_Roughness=0.05, Normal=bump(nt, h, 0.5, 0.06), Emission=attr(nt, 'Glow'), Emission_Strength=1.0)
    return m


def build_nucleo(w):
    rnd = random.Random(1010)
    F = Field(w, step=0.05)
    X, Y = F.X, F.Y

    # The shelf reaches a few metres beyond whatever the layout uses and
    # then breaks off: the largest empty part of the board becomes the bay
    # the black hole is seen through.
    rag = (fbm(X / 2.2, Y / 2.2, 91, 4) - 0.5) * 2
    sd = 1.7 + 0.9 * rag - F.gap
    sd = np.minimum(sd, 6.0)

    # Plates of obsidian, each at its own height and tilt, with seams
    # between them; in the seams the red light of the core.
    d1, d2, idn, px, py = cells(X / 2.1, Y / 2.1, 92)
    seam_d = (d2 - d1) * 2.1
    tx, ty = np.sin(idn * 91.0), np.cos(idn * 57.0)
    lift = smooth(0.52, 0.8, fbm(X / 5.5 + 2, Y / 5.5, 93, 3)) * calm_rows(Y)
    H = 0.10 + 0.45 * idn + 0.16 * (tx * (X / 2.1 - px) + ty * (Y / 2.1 - py)) * 2.1 + 1.5 * lift * (0.4 + idn)
    H = H - 0.22 * smooth(0.16, 0.0, seam_d)
    F.relief(H, ramp_w=1.8)
    tint = shade([(0.0, '#0a0a14'), (0.35, '#141024'), (0.7, '#0c1222'), (1.0, '#1c0c18')], idn)
    F.col = tint
    vein = smooth(0.42, 0.62, fbm(X / 4.5 + 11, Y / 4.5 + 5, 94, 3))
    near_core = np.exp(-F.d_fort / 7.0)
    seam = smooth(0.10, 0.02, seam_d)
    hot = smooth(0.045, 0.0, seam_d)
    power = 0.10 + 0.9 * np.maximum(vein, near_core)
    F.glow += (seam * power)[..., None] * lin3('#ff1830') * 1.2
    F.glow += (hot * power * power)[..., None] * lin3('#ff8a90') * 1.0
    # light round the fort's plate and round every pad
    halo = smooth(1.1, 0.0, F.d_fort - FORT_R) * smooth(-0.05, 0.08, F.d_fort - FORT_R)
    F.glow += halo[..., None] * lin3('#ff2030') * 0.55
    halo = smooth(0.55, 0.0, F.d_pad - PAD_R) * smooth(-0.05, 0.06, F.d_pad - PAD_R)
    F.glow += halo[..., None] * lin3('#ff2030') * 0.16

    # the black hole, in the emptiest place
    spot = void_spots(F, sd, 1, 5.0, y0=HUD_BOT_Y + 5.0, y1=HUD_TOP_Y - 4.0, xmax=6.6)
    if spot:
        bx, by, room = spot[0]
        Rh = min(2.0, max(1.2, room * 0.7))
        # its light on the shelf's edge
        F.glow += (np.exp(-np.hypot(X - bx, Y - by) / (2.6 * Rh)) * smooth(2.5, 0.0, sd))[..., None] * lin3('#ff3818') * 0.5
        backdrop('void', blackhole_picture(B.SCREEN_W / 2 + bx * B.PX, B.SCREEN_H - by * B.ROW_Y, Rh * B.PX))
    else:
        backdrop('void', blackhole_picture(0, 0, 0))

    cliff = cliff_mat('cliff_nucleo', [(0.0, '#221a30'), (0.1, '#0c0a14'), (0.3, '#1c1226'), (0.55, '#08060c'), (0.9, '#1a0810')],
                      glow_lo='#e01808', glow_k=0.28, z0=-6.0, rough=0.3)
    island(F, sd, obsidian_mat(), cliff, [(0.25, -0.05, 0.0), (1.2, 0.25, 0.3), (2.0, 0.7, 0.45), (2.6, 1.1, 0.5)], seed=3)

    shard = tint_mat('shard', rough=0.1, clear=1.0, emit=2.2, spec=0.9)
    hotm = tint_mat('hot', rough=0.3, emit=3.0)
    gem = gem_mat('gem_nucleo', emit=0.8, edge=1.4)
    rockm = tint_mat('rock_nucleo', rough=0.25, clear=0.6, grain=0.3, grain_scale=5.0)
    bo, bh, bg, br = Batch('shards', shard), Batch('hot', hotm, True), Batch('gems', gem), Batch('rocks', rockm)
    ringm = B.glow('ring_red', '#ff2a30', 5.0)
    BLACK = '#07060c'
    on_land = lambda x, y: F.at(sd, x, y) > 0.45  # noqa: E731
    taken = []
    big = sites(F, rnd, 12, 0.95, hmin=2.4, top=6.0, sep=4.2, taken=taken, test=on_land)
    for i, (x, y, h) in enumerate(big):
        z = F.z(x, y)
        if i % 2 == 0:
            # a pylon: a spike of black glass that holds a ring of light
            h = min(h, rnd.uniform(4.0, 5.6))
            bo.cone(TM(x, y, z - 0.2), 0.62, 0.5, '#1a1420', '#2a2030', r2=0.48, segs=6)
            bo.spire(TM(x, y, z + 0.2, 30), 0.36, h * 0.78, '#ff2a30', BLACK, segs=6, tip=0.5, sink=0.0, base=1.0)
            for k in range(3):
                a = math.radians(120 * k + 30)
                bh.cone(TM(x + 0.3 * math.cos(a), y + 0.3 * math.sin(a), z + 0.3), 0.05, h * 0.3, '#ff5030', '#ff1020', r2=0.03,
                        segs=5)
            zc = z + h * 0.88
            bh.ball(TM(x, y, zc), 0.17, '#ff5030', '#ffd0a0', segs=12)
            B.torus(0.48, 0.045, ringm, None, loc=(x, y, zc), rot=(rnd.uniform(-20, 20), rnd.uniform(-20, 20), 0), segs=32, tsegs=8)
            B.torus(0.66, 0.03, ringm, None, loc=(x, y, zc - h * 0.14), rot=(rnd.uniform(-14, 14), rnd.uniform(-14, 14), 0), segs=32,
                    tsegs=8)
            F.add_glow(x, y, 1.6, '#ff2030', 0.30)
        else:
            # a burst of obsidian blades, hot at the root
            h = min(h, rnd.uniform(3.4, 5.2))
            crystal_cluster(bo, rnd, x, y, z - 0.1, h, 0.36 + 0.03 * h, '#ff2438', BLACK, n=rnd.randint(6, 8), spread=1.1)
            F.add_glow(x, y, 1.7, '#ff2030', 0.38)
    for (x, y, h) in sites(F, rnd, 24, 0.55, hmin=0.8, top=2.4, sep=2.3, taken=taken, test=on_land):
        h = min(h, rnd.uniform(0.9, 2.0))
        if rnd.random() < 0.35:
            c = rnd.choice([('#3a1cc8', '#b89cff'), ('#5a18c0', '#e088ff')])
            crystal_cluster(bg, rnd, x, y, F.z(x, y) - 0.05, h * 0.8, 0.22, c[0], c[1], n=4)
            F.add_glow(x, y, 1.0, c[1], 0.16)
        else:
            crystal_cluster(bo, rnd, x, y, F.z(x, y) - 0.05, h, 0.22 + 0.03 * h, '#ff2438', BLACK, n=rnd.randint(3, 5))
            F.add_glow(x, y, 1.0, '#ff2030', 0.28)
    # splinters and embers, low enough for the road's side
    for (x, y) in scatter(F, rnd, 150, margin=0.35, taken=taken, r=0.15, test=on_land):
        z = F.z(x, y)
        lim = 0.2 + 0.5 * F.at(F.gap, x, y)
        k = rnd.random()
        if k < 0.5:
            bo.spire(TM(x, y, z - 0.03, rnd.uniform(0, 60), rnd.uniform(5, 35), rnd.uniform(0, 360)), rnd.uniform(0.06, 0.12),
                     min(lim, rnd.uniform(0.2, 0.48)), '#ff2438', BLACK, segs=rnd.choice([4, 5]), tip=0.5, sink=0.1)
        elif k < 0.8:
            br.rock(TM(x, y, z, rnd.uniform(0, 360), scale=(1, 0.8, 0.6)), rnd.uniform(0.08, 0.2), '#0c0a14', '#2a2038', rnd,
                    subdiv=1)
        else:
            bh.ball(TM(x, y, z + 0.05), rnd.uniform(0.04, 0.07), '#ff5030', '#ffc060', segs=8)

    # wreckage of the shelf adrift over the void, black against the fire
    n = 0
    for _ in range(4000):
        if n >= 40:
            break
        x, y = rnd.uniform(-9.5, 9.5), rnd.uniform(1.0, 40.0)
        s0 = float(F.at(sd, x, y))
        if s0 > -0.6 or (y < HUD_BOT_Y and rnd.random() < 0.6):
            continue
        z = -rnd.uniform(0.5, 8.0)
        r = rnd.uniform(0.15, min(0.8, 0.2 - s0 * 0.4))
        float_rock(br, rnd, *under(x, y, z), r, '#050408', '#1c1428')
        if r > 0.4 and rnd.random() < 0.5:
            qx, qy, qz = under(x, y, z)
            bo.spire(TM(qx, qy, qz + r * 0.1, 0, rnd.uniform(0, 15), rnd.uniform(0, 360)), r * 0.2, r * 1.4, '#ff2438', BLACK)
        n += 1
    for b_ in (bo, bh, bg, br):
        b_.finish()

    roadm = cobble_mat('road_nucleo', '#7c7078', '#988c94', '#1a1016', scale=2.1, rough=0.5, depth=0.4, seed_off=9.0)
    kerb = B.glow('kerb_nucleo', '#ff2a28', 3.2)
    lay_road(w, roadm, kerb, edge_w=0.09, edge_h=0.05)
    plate = B.mat('plate_nucleo', '#5e5868', rough=0.3, metal=0.7, clear=0.4)
    inner = B.mat('plate_in_nucleo', '#16121c', rough=0.5, metal=0.5)
    K.pads(w, plate, rim_k=6.0, inner_mat=inner)
    K.foundation(w, plate, rim_k=5.0)

    B.sun(205, 46, 2.7, (0.80, 0.82, 1.0), angle=5)
    B.sun(325, 16, 1.7, (1.0, 0.16, 0.08), angle=20, name='hole')
    B.sun(40, 34, 0.7, (0.6, 0.3, 1.0), angle=25, name='rim')
    B.ambient((0.30, 0.16, 0.36), 0.35)


# ---------------------------------------------------------------------------

BUILDERS = {
    'cristal': build_cristal,
    'cienaga': build_cienaga,
    'nebula': build_nebula,
    'colmena': build_colmena,
    'nucleo': build_nucleo,
}


def main():
    only = B.arg('--only')
    ids = [s for s in only.split(',') if s] if only else list(BUILDERS)
    out = B.arg('--out', os.path.join(B.ASSETS, 'worlds'))
    samples = int(B.arg('--samples', 96))
    times = []
    for wid in ids:
        t0 = time.time()
        B.setup(samples)
        # B.setup() empties the file, and render_world's own material went
        # with it: make it build a new one
        B._POS = None
        BUILDERS[wid](K.world(wid))
        t1 = time.time()
        B.render_world(wid, out)
        times.append((wid, t1 - t0, time.time() - t1))
        print('TIME %s build %.1f s render %.1f s' % (wid, t1 - t0, time.time() - t1))
    for (wid, tb, tr) in times:
        print('%-8s build %5.1f s  render %5.1f s' % (wid, tb, tr))


if __name__ == '__main__':
    main()
