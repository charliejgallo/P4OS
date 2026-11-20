"""
BASTION - the first five worlds (SPEC.md, "Worlds"): luna, selva, magma,
hielo and orbita, each built by code and rendered as the game's whole
screen with the depth of its pixels.

    /Applications/Blender.app/Contents/MacOS/Blender -b -P worlds_a.py -- [--only luna,selva] [--samples 96]
        [--out ../../assets/worlds]
    python3 preview.py --world ../../assets/worlds/luna --out ../../build/preview/luna.png

How a world is made here. The pads and the fort's plate come from
worlds_kit.py, exactly where ../bsworlds.py says; the road is cut here, on
the same centre lines and of the same width (road() says why not the
kit's). The layout is read at run time: nothing below copies a coordinate,
so a layout that is tuned later only needs this script run again.

Around them the ground is one height field, a vertex every 5 cm (about two
pixels), that is modelled AND painted in numpy: Ground knows how far every
vertex is from the road, the pads and the fort, so the relief fades to
Z = 0 where the game plays, and colour, glow and gloss are written per
vertex as colour attributes that one material reads. Props are then
scattered by a seeded random over the places Ground finds clear: clear of
the game's surfaces (K.clear), and not standing in front of them tall
enough to hide a road or a pad (hides()). After each render check() reads
the depth back and says how much of the road, the pads and the plate
something covers or has dug under.
"""
import math
import os
import random
import sys
import time

import bpy
import bmesh
import numpy as np
from mathutils import Euler, Matrix, Vector, noise as mnoise

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import bs_common as B  # noqa: E402
import worlds_kit as K  # noqa: E402
import bsworlds  # noqa: E402

HALF = bsworlds.PATH_W / 2.0
HUD_TOP_Y = (B.SCREEN_H - 96) / B.ROW_Y       # the ground row where the top HUD begins
HUD_BOT_Y = 190 / B.ROW_Y                     # and where the tower bar ends


# ---------------------------------------------------------------------------
# Numbers: colours, noises and cells in numpy (the ground is painted with
# them, a vertex at a time)
# ---------------------------------------------------------------------------

def C(c):
    """a colour as a linear RGB array"""
    return np.array(B.lin(c)[:3], np.float32)


def smooth(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3.0 - 2.0 * t)


def lerp(a, b, t):
    """two colours (or fields of colours) mixed by a field t"""
    t = np.asarray(t, np.float32)[..., None]
    return a * (1.0 - t) + b * t


_TAB = {}


def _tab(seed):
    if seed not in _TAB:
        r = np.random.RandomState(1000 + seed)
        a = r.rand(256, 256) * 2 * np.pi
        _TAB[seed] = (np.cos(a), np.sin(a), r.rand(256, 256), r.rand(256, 256), r.rand(256, 256))
    return _TAB[seed]


def perlin(x, y, seed=0):
    gx, gy = _tab(seed)[:2]
    xi = np.floor(x).astype(np.int64)
    yi = np.floor(y).astype(np.int64)
    fx, fy = x - xi, y - yi
    u = fx * fx * fx * (fx * (fx * 6 - 15) + 10)
    v = fy * fy * fy * (fy * (fy * 6 - 15) + 10)

    def dot(ix, iy, dx, dy):
        return gx[ix & 255, iy & 255] * dx + gy[ix & 255, iy & 255] * dy
    a = dot(xi, yi, fx, fy) * (1 - u) + dot(xi + 1, yi, fx - 1, fy) * u
    b = dot(xi, yi + 1, fx, fy - 1) * (1 - u) + dot(xi + 1, yi + 1, fx - 1, fy - 1) * u
    return a * (1 - v) + b * v


def fbm(x, y, size, octaves=4, seed=0, gain=0.5):
    """a cloudy field 0..1 (0.5 in the middle) whose biggest lumps are
    'size' metres across"""
    s, amp, tot, f = 0.0, 1.0, 0.0, 1.0 / size
    for o in range(octaves):
        s = s + amp * perlin(x * f + 31.7 * o, y * f - 17.3 * o, seed + o)
        tot += amp
        amp *= gain
        f *= 2.0
    return np.clip(0.5 + 1.15 * s / tot, 0.0, 1.0)


def ridged(x, y, size, octaves=4, seed=0):
    """ridges 0..1 (1 on the crest): dunes, veins, mountain lines"""
    s, amp, tot, f = 0.0, 1.0, 0.0, 1.0 / size
    for o in range(octaves):
        s = s + amp * (1.0 - np.abs(perlin(x * f + 13.1 * o, y * f + 7.7 * o, seed + o)) * 2.6)
        tot += amp
        amp *= 0.5
        f *= 2.0
    return np.clip(s / tot, 0.0, 1.0)


def blur(a, k):
    """a field smoothed over about k cells each way (a box, twice)"""
    for axis in (0, 1):
        for _ in range(2):
            pad = [(0, 0), (0, 0)]
            pad[axis] = (k + 1, k)
            c = np.cumsum(np.pad(a, pad, mode='edge'), axis=axis, dtype=np.float64)
            n = a.shape[axis]
            a = ((np.take(c, range(2 * k + 1, 2 * k + 1 + n), axis=axis) - np.take(c, range(0, n), axis=axis))
                 / (2 * k + 1)).astype(np.float32)
    return a


def grow(m):
    """a mask grown by one cell every way"""
    out = m.copy()
    out[1:, :] |= m[:-1, :]
    out[:-1, :] |= m[1:, :]
    out[:, 1:] |= out[:, :-1].copy()
    out[:, :-1] |= out[:, 1:].copy()
    return out


def spread(m, k):
    """a mask grown by a disc of k cells"""
    out = m.copy()
    ny, nx = m.shape
    for dy in range(-k, k + 1):
        for dx in range(-k, k + 1):
            if (dx or dy) and dx * dx + dy * dy <= k * k:
                out[max(0, dy):ny + min(0, dy), max(0, dx):nx + min(0, dx)] |= \
                    m[max(0, -dy):ny + min(0, -dy), max(0, -dx):nx + min(0, -dx)]
    return out


def cells(x, y, size, seed=0, jitter=0.9):
    """Voronoi cells 'size' metres across: (distance to the nearest seed,
    distance to the cell's border, a random 0..1 per cell, the seed's x, y)"""
    _, _, r0, r1, r2 = _tab(seed)
    px, py = x / size, y / size
    ix = np.floor(px).astype(np.int64)
    iy = np.floor(py).astype(np.int64)
    f1 = np.full(px.shape, 1e9)
    f2 = np.full(px.shape, 1e9)
    ax = np.zeros(px.shape)
    ay = np.zeros(px.shape)
    bx = np.zeros(px.shape)
    by = np.zeros(px.shape)
    rid = np.zeros(px.shape)
    for dx in (-1, 0, 1):
        for dy in (-1, 0, 1):
            jx, jy = ix + dx, iy + dy
            qx = jx + 0.5 + (r0[jx & 255, jy & 255] - 0.5) * jitter
            qy = jy + 0.5 + (r1[jx & 255, jy & 255] - 0.5) * jitter
            d = np.hypot(px - qx, py - qy)
            near = d < f1
            second = (~near) & (d < f2)
            bx = np.where(near, ax, np.where(second, qx, bx))
            by = np.where(near, ay, np.where(second, qy, by))
            f2 = np.where(near, f1, np.where(second, d, f2))
            ax = np.where(near, qx, ax)
            ay = np.where(near, qy, ay)
            rid = np.where(near, r2[jx & 255, jy & 255], rid)
            f1 = np.where(near, d, f1)
    edge = (f2 * f2 - f1 * f1) / (2.0 * np.maximum(np.hypot(ax - bx, ay - by), 1e-6))
    return f1 * size, edge * size, rid, ax * size, ay * size


# ---------------------------------------------------------------------------
# Materials: a small way of writing node trees
# ---------------------------------------------------------------------------

class Mat:
    """a Principled material and the nodes that feed it. Every method
    returns a socket, and takes sockets or plain values."""

    def __init__(self, name, emission_only=False):
        self.m = bpy.data.materials.new(name)
        self.m.use_nodes = True
        self.nt = self.m.node_tree
        self.b = self.nt.nodes['Principled BSDF']
        self.out = self.nt.nodes['Material Output']
        if emission_only:
            self.nt.nodes.remove(self.b)
            self.b = self.nt.nodes.new('ShaderNodeEmission')
            self.nt.links.new(self.b.outputs[0], self.out.inputs['Surface'])
        self._tc = None

    def put(self, sock, v):
        if isinstance(v, bpy.types.NodeSocket):
            self.nt.links.new(v, sock)
        elif isinstance(v, str):
            sock.default_value = B.lin(v)
        elif isinstance(v, (tuple, list, np.ndarray)) and len(v) == 3 and sock.type == 'RGBA':
            sock.default_value = (float(v[0]), float(v[1]), float(v[2]), 1.0)
        else:
            sock.default_value = v

    def node(self, kind, props=None, **ins):
        n = self.nt.nodes.new('ShaderNode' + kind)
        for k, v in (props or {}).items():
            setattr(n, k, v)
        for k, v in ins.items():
            self.put(n.inputs[k.replace('_', ' ')], v)
        return n

    def set(self, **ins):
        """the Principled inputs (Base_Color=..., Roughness=...)"""
        for k, v in ins.items():
            self.put(self.b.inputs[k.replace('_', ' ')], v)
        return self

    def coord(self, which='Object'):
        if self._tc is None:
            self._tc = self.nt.nodes.new('ShaderNodeTexCoord')
        return self._tc.outputs[which]

    def scaled(self, vec, s):
        """a vector stretched: s is (sx, sy, sz) multipliers of the pattern's frequency"""
        return self.node('Mapping', Vector=vec, Scale=s).outputs[0]

    def xyz(self, vec):
        return self.node('SeparateXYZ', Vector=vec).outputs

    def rgb(self, col):
        return self.node('SeparateRGB', Image=col).outputs

    def attr(self, name):
        return self.node('Attribute', dict(attribute_name=name)).outputs['Color']

    def noise(self, scale, detail=4.0, rough=0.5, distortion=0.0, vec=None, out='Fac'):
        n = self.node('TexNoise', Vector=vec if vec is not None else self.coord(), Scale=scale, Detail=detail,
                      Roughness=rough, Distortion=distortion)
        return n.outputs[out]

    def voronoi(self, scale, feature='F1', vec=None, rand=1.0, out='Distance'):
        n = self.node('TexVoronoi', dict(feature=feature), Vector=vec if vec is not None else self.coord(),
                      Scale=scale, Randomness=rand)
        return n.outputs[out]

    def math(self, op, a, b=None, c=None, clamp=False):
        n = self.node('Math', dict(operation=op, use_clamp=clamp))
        self.put(n.inputs[0], a)
        if b is not None:
            self.put(n.inputs[1], b)
        if c is not None:
            self.put(n.inputs[2], c)
        return n.outputs[0]

    def remap(self, v, a, b, lo=0.0, hi=1.0, smoothstep=False):
        n = self.node('MapRange', dict(interpolation_type='SMOOTHSTEP' if smoothstep else 'LINEAR'))
        self.put(n.inputs['Value'], v)
        n.inputs['From Min'].default_value = a
        n.inputs['From Max'].default_value = b
        n.inputs['To Min'].default_value = lo
        n.inputs['To Max'].default_value = hi
        return n.outputs[0]

    def ramp(self, fac, stops, interp='LINEAR'):
        n = self.node('ValToRGB', Fac=fac)
        cr = n.color_ramp
        cr.interpolation = interp
        while len(cr.elements) < len(stops):
            cr.elements.new(0.5)
        for e, (pos, col) in zip(cr.elements, stops):
            e.position = pos
            e.color = B.lin(col) if isinstance(col, str) else (*[float(v) for v in col[:3]], 1.0)
        return n.outputs['Color']

    def mix(self, fac, a, b, blend='MIX', clamp=False):
        n = self.node('MixRGB', dict(blend_type=blend, use_clamp=clamp), Fac=fac, Color1=a, Color2=b)
        return n.outputs[0]

    def bump(self, height, strength=0.5, distance=0.1, normal=None):
        n = self.node('Bump', Height=height, Strength=strength, Distance=distance)
        if normal is not None:
            self.put(n.inputs['Normal'], normal)
        return n.outputs[0]

    def facing(self, d):
        """how much the surface looks towards the direction d (-1..1): a
        cheap way of giving a shining thing a lit and a dark side"""
        g = self.node('NewGeometry')
        v = Vector(d).normalized()
        n = self.node('VectorMath', dict(operation='DOT_PRODUCT'))
        self.put(n.inputs[0], g.outputs['Normal'])
        n.inputs[1].default_value = v
        return n.outputs['Value']

    def hud_dim(self, k=0.3):
        """1 over the board, 1 - k in the rows under the HUD (for a sheet of
        lava or water lying at Z = 0 or so in world coordinates)"""
        y = self.xyz(self.coord())[1]
        a = self.remap(y, HUD_BOT_Y - 1.2, HUD_BOT_Y + 0.6, 1.0 - k, 1.0)
        b = self.remap(y, HUD_TOP_Y - 0.4, HUD_TOP_Y + 0.8, 1.0, 1.0 - k)
        return self.math('MULTIPLY', a, b)

    def camera_only(self, strength):
        """a strength that only the camera sees (a backdrop that must not light the scene)"""
        lp = self.node('LightPath')
        return self.math('MULTIPLY', strength, lp.outputs['Is Camera Ray'])


def ground_mat(name, rough=0.85, spec=0.25, grain=(14.0, 0.12), bump=(0.25, 0.04), lumps=None, aux=False,
               emit=False, tweak=None):
    """the material of a painted height field: its colour is the 'col'
    attribute, grained by a fine noise (scale, amount) that also dents it
    (strength, distance); 'lumps' adds a second, coarser bump (scale,
    strength). With aux the 'aux' attribute gives roughness (R), metal (G)
    and how much of the bump stays (B); with emit the 'emit' attribute
    shines. tweak(M, sockets) may rewire the rest."""
    M = Mat(name)
    col = M.attr('col')
    n = M.noise(grain[0], detail=6.0, rough=0.6)
    shade = M.remap(n, 0.25, 0.75, 1.0 - grain[1], 1.0 + grain[1])
    base = M.mix(1.0, col, shade, 'MULTIPLY')
    h = n
    if lumps:
        n2 = M.noise(lumps[0], detail=3.0)
        h = M.math('ADD', n, M.math('MULTIPLY', n2, lumps[1] / max(bump[0], 1e-3)))
    k = bump[0]
    M.set(Base_Color=base, Roughness=rough, Specular=spec)
    s = dict(col=col, base=base, grain=n, height=h)
    if aux:
        r, g, b = M.rgb(M.attr('aux'))
        M.set(Roughness=r, Metallic=g)
        k = M.math('MULTIPLY', b, bump[0])
        s.update(rough=r, metal=g, keep=b)
    M.set(Normal=M.bump(h, k, bump[1]))
    if emit:
        M.set(Emission=M.attr('emit'), Emission_Strength=1.0)
    if tweak:
        tweak(M, s)
    return M.m


def crystal_mat(name, low, high, k=1.6, top=2.0, rough=0.12, side=(-0.6, -0.5, 0.6)):
    """a shining crystal: its colour runs from 'low' at its foot to 'high'
    'top' metres up its own axis, and the faces that look towards 'side'
    shine brighter, so its facets show though it is a light"""
    M = Mat(name)
    z = M.xyz(M.coord())[2]
    col = M.ramp(M.remap(z, 0.0, top), [(0.0, low), (1.0, high)])
    f = M.remap(M.facing(side), -1.0, 1.0, 0.35, 1.0)
    M.set(Base_Color=col, Roughness=rough, Specular=0.9, Clearcoat=0.6, Emission=col,
          Emission_Strength=M.math('MULTIPLY', f, k))
    return M.m


# ---------------------------------------------------------------------------
# The ground: one painted height field that knows where the game plays
# ---------------------------------------------------------------------------

def _segments(w):
    """every stretch of road once (roads that share points share them
    here): [((ax, ay), (bx, by), metres from b to the fort), ...]"""
    seen, out = set(), []
    for p in w.paths:
        s = 0.0
        for i in range(len(p) - 1, 0, -1):
            a, b = p[i - 1], p[i]
            key = (round(a[0], 3), round(a[1], 3), round(b[0], 3), round(b[1], 3))
            if key not in seen:
                seen.add(key)
                out.append((a, b, s))
            s += math.hypot(b[0] - a[0], b[1] - a[1])
    return out


class Ground:
    def __init__(self, w, step=0.05, x0=-10.0, x1=10.0, y0=-3.0, y1=44.0):
        self.w = w
        self.step = step
        self.x0, self.y0 = x0, y0
        xs = np.arange(x0, x1 + step / 2, step, dtype=np.float32)
        ys = np.arange(y0, y1 + step / 2, step, dtype=np.float32)
        self.X, self.Y = np.meshgrid(xs, ys)
        self.ny, self.nx = self.X.shape
        # how far each vertex is from the road's centre line (capped at
        # REACH), and how far along the road that nearest point is
        REACH = 8.0
        d = np.full(self.X.shape, REACH, np.float32)
        along = np.zeros(self.X.shape, np.float32)
        for (a, b, s) in _segments(w):
            sl = self.window((a[0] + b[0]) / 2, (a[1] + b[1]) / 2, REACH + 0.5)
            X, Y = self.X[sl], self.Y[sl]
            dx, dy = b[0] - a[0], b[1] - a[1]
            L2 = dx * dx + dy * dy
            t = np.clip(((X - a[0]) * dx + (Y - a[1]) * dy) / L2, 0.0, 1.0)
            dd = np.hypot(X - (a[0] + t * dx), Y - (a[1] + t * dy))
            near = dd < d[sl]
            along[sl] = np.where(near, s + (1.0 - t) * math.sqrt(L2), along[sl])
            d[sl] = np.where(near, dd, d[sl])
        self.d_road = d
        self.s_road = along
        dp = np.full(self.X.shape, REACH, np.float32)
        for (px, py) in w.pads:
            sl = self.window(px, py, REACH)
            dp[sl] = np.minimum(dp[sl], np.hypot(self.X[sl] - px, self.Y[sl] - py))
        self.d_pad = dp
        self.d_fort = np.hypot(self.X - w.fort[0], self.Y - w.fort[1])
        # metres of free ground between a vertex and the nearest thing the
        # game owns (negative on it)
        self.free = np.minimum(np.minimum(d - HALF, dp - bsworlds.PAD_R), self.d_fort - bsworlds.FORT_R)
        self.Z = np.zeros(self.X.shape, np.float32)
        self.room = None        # where set: metres of usable ground around each vertex (a deck's edge limits it)

    def window(self, x, y, r):
        """the slices of the grid within r of (x, y)"""
        i0 = max(0, int((y - r - self.y0) / self.step))
        i1 = min(self.ny, int((y + r - self.y0) / self.step) + 2)
        j0 = max(0, int((x - r - self.x0) / self.step))
        j1 = min(self.nx, int((x + r - self.x0) / self.step) + 2)
        return (slice(i0, i1), slice(j0, j1))

    def flat(self, a=0.25, b=1.5):
        """0 on the road, the pads and the fort's plate and 'a' metres
        round them, rising to 1 at 'b' metres: multiply any relief by it
        and the game's surfaces stay at Z = 0"""
        return smooth(a, b, self.free)

    def _ij(self, x, y):
        i = min(self.ny - 1, max(0, int(round((y - self.y0) / self.step))))
        j = min(self.nx - 1, max(0, int(round((x - self.x0) / self.step))))
        return i, j

    def height(self, x, y):
        return float(self.Z[self._ij(x, y)])

    def free_at(self, x, y):
        return float(self.free[self._ij(x, y)])

    def splat(self, field, x, y, r, value):
        """adds a soft round spot of 'value' (a number or a colour) to a
        field: the pool of light under a lamp, the stain under a plant"""
        sl = self.window(x, y, r * 2.5)
        f = np.exp(-((self.X[sl] - x) ** 2 + (self.Y[sl] - y) ** 2) / (r * r))
        if field.ndim == 3:
            field[sl] += f[..., None] * np.asarray(value, np.float32)
        else:
            field[sl] += f * value

    def build(self, mat, layers=None, name='ground', keep=None, XY=None, Z=None, uv=None):
        """the mesh of self.Z (or of Z); layers = {'col': (ny, nx, 3) or
        (ny, nx), ...} become colour attributes of its vertices (linear,
        unclamped). keep (bool per vertex) drops the faces that touch a
        vertex not kept; XY replaces the vertices' places (for an edge moved
        onto a contour); uv = (U, V) fields gives it texture coordinates."""
        ny, nx = self.ny, self.nx
        X, Y = (self.X, self.Y) if XY is None else XY
        co = np.stack([X, Y, self.Z if Z is None else Z], axis=-1).astype(np.float32).reshape(-1, 3)
        idx = np.arange(ny * nx, dtype=np.int32).reshape(ny, nx)
        quads = np.stack([idx[:-1, :-1], idx[:-1, 1:], idx[1:, 1:], idx[1:, :-1]], axis=-1).reshape(-1, 4)
        used = None
        if keep is not None:
            k = keep.reshape(-1)
            quads = quads[k[quads].all(axis=1)]
            used = np.unique(quads)
            remap = np.zeros(ny * nx, np.int32)
            remap[used] = np.arange(len(used), dtype=np.int32)
            quads = remap[quads]
            co = co[used]
        nq = len(quads)
        me = bpy.data.meshes.new(name)
        me.vertices.add(len(co))
        me.vertices.foreach_set('co', co.ravel())
        me.loops.add(nq * 4)
        me.loops.foreach_set('vertex_index', quads.ravel())
        me.polygons.add(nq)
        me.polygons.foreach_set('loop_start', np.arange(0, nq * 4, 4, dtype=np.int32))
        me.polygons.foreach_set('loop_total', np.full(nq, 4, dtype=np.int32))
        me.polygons.foreach_set('use_smooth', np.ones(nq, dtype=bool))
        me.update(calc_edges=True)
        for lname, a in (layers or {}).items():
            if a.ndim == 2:
                a = np.repeat(a[..., None], 3, axis=2)
            rgba = np.concatenate([a.astype(np.float32), np.ones((ny, nx, 1), np.float32)], axis=2).reshape(-1, 4)
            if used is not None:
                rgba = rgba[used]
            ca = me.color_attributes.new(lname, 'FLOAT_COLOR', 'POINT')
            ca.data.foreach_set('color', rgba.ravel())
        if uv is not None:
            t = np.stack([uv[0], uv[1]], axis=-1).astype(np.float32).reshape(-1, 2)
            if used is not None:
                t = t[used]
            layer = me.uv_layers.new(name='UVMap')
            layer.data.foreach_set('uv', t[quads.ravel()].ravel())
        me.materials.append(mat)
        o = bpy.data.objects.new(name, me)
        B.sc.collection.objects.link(o)
        return o

    def contour(self, f, level, inside, rings=1):
        """the vertices just outside the region 'inside' (where the field f
        passes 'level') moved onto that contour, so a sheet cut out of the
        grid ends in a smooth edge and not in the grid's steps: returns
        (XY, ring), ring being those vertices. With rings = 2 the next row
        out comes too, onto the same line: lowered, it makes a wall."""
        ring = inside
        for _ in range(rings):
            ring = grow(ring)
        ring &= ~inside
        gy, gx = np.gradient(f, self.step)
        g2 = gx * gx + gy * gy
        k = np.where(ring & (g2 > 0.05), (f - level) / np.maximum(g2, 1e-6), 0.0)
        lim = (1.5 + rings) * self.step / np.maximum(np.sqrt(g2), 1e-3)
        k = np.clip(k, -lim, lim)
        return (self.X - k * gx, self.Y - k * gy), ring

    # ---- where props may stand ------------------------------------------
    def ok(self, x, y, r, h, r_top=None):
        """may a prop of radius r and height h stand at (x, y)? Low things
        only keep off the game's surfaces; anything above half a metre also
        keeps the kit's distance from the road and must not hide what is
        behind it. r_top is its radius up high when that is wider than its
        foot (a mushroom's cap, a tree's crown)."""
        if self.room is not None and float(self.room[self._ij(x, y)]) < r + 0.1:
            return False
        if h <= 0.5:
            return self.free_at(x, y) > r + 0.30
        return K.clear(self.w, x, y, r) and not hides(self.w, x, y, r_top or r, h)

    def spots(self, rng, n, r, h=0.0, gap=3.0, taken=None, xr=(-8.8, 8.8), yr=None, weight=None, margin=0.0,
              r_top=None):
        """up to n places for props of radius r and height h, 'gap' apart
        and off whatever is in 'taken' [(x, y, r), ...], the roomiest
        first: [(x, y, room)], room being the free ground around. They are
        added to 'taken'."""
        yr = yr or (HUD_BOT_Y + 0.6, HUD_TOP_Y - 0.5)
        sub = max(1, int(round(0.25 / self.step)))
        F = (self.free if self.room is None else np.minimum(self.free, self.room - 0.4))[::sub, ::sub]
        Xs, Ys = self.X[::sub, ::sub], self.Y[::sub, ::sub]
        m = (F > r + 0.5 + margin) & (Xs > xr[0]) & (Xs < xr[1]) & (Ys > yr[0]) & (Ys < yr[1])
        if h > 0.5:
            m &= (Ys + 0.75 * h) < HUD_TOP_Y + 0.3
        ii, jj = np.nonzero(m)
        score = np.minimum(F[ii, jj], 4.0)
        if weight is not None:
            score = score * weight[::sub, ::sub][ii, jj]
        score = score * np.array([0.55 + 0.45 * rng.random() for _ in range(len(ii))])
        order = np.argsort(-score)
        out = []
        taken = taken if taken is not None else []
        for k in order:
            if len(out) >= n:
                break
            x, y = float(Xs[ii[k], jj[k]]), float(Ys[ii[k], jj[k]])
            if any(math.hypot(x - tx, y - ty) < r + tr + 0.2 for (tx, ty, tr) in taken):
                continue
            if any(math.hypot(x - ox, y - oy) < gap for (ox, oy, _) in out):
                continue
            if not self.ok(x, y, r, max(h, 0.6), r_top):
                continue
            out.append((x, y, float(F[ii[k], jj[k]])))
        taken.extend((x, y, r) for (x, y, _) in out)
        return out

    def sprinkle(self, rng, n, r_of, h_of, taken=None, xr=(-9.8, 9.8), yr=(-1.0, 41.5), weight=None, tries=30,
                 own_gap=1.0):
        """n small things anywhere they fit, the HUD's rows included:
        [(x, y, r)]. r_of(rng) gives each one's radius and h_of(r) its
        height; weight (a field 0..1) says where they like to be."""
        out = []
        taken = taken if taken is not None else []
        for _ in range(n):
            for _t in range(tries):
                x, y = rng.uniform(*xr), rng.uniform(*yr)
                r = r_of(rng)
                if weight is not None and rng.random() > float(weight[self._ij(x, y)]):
                    continue
                if not self.ok(x, y, r, h_of(r)):
                    continue
                if any(math.hypot(x - tx, y - ty) < (r + tr) * own_gap for (tx, ty, tr) in taken):
                    continue
                out.append((x, y, r))
                taken.append((x, y, r))
                break
        return out


def sockets(G, col, color, k=0.85, width=0.26):
    """paints a collar round every pad and round the fort's plate, so they
    read as set into the ground whatever the ground is doing there"""
    m = np.maximum(smooth(bsworlds.PAD_R + width + 0.04, bsworlds.PAD_R + width - 0.02, G.d_pad),
                   smooth(bsworlds.FORT_R + width + 0.09, bsworlds.FORT_R + width + 0.03, G.d_fort))
    return lerp(col, C(color), m * k)


def hud_calm(G, col, emit=None, dim=0.2, hush=0.5):
    """keeps the rows under the game's HUD quiet: the ground a little
    darker there, and what shines on it dimmer, so the bars read over it"""
    c = np.maximum(smooth(HUD_BOT_Y + 0.6, HUD_BOT_Y - 1.2, G.Y), smooth(HUD_TOP_Y - 0.4, HUD_TOP_Y + 0.8, G.Y))
    col *= (1.0 - dim * c)[..., None]
    if emit is not None:
        emit *= (1.0 - hush * c)[..., None]


def hides(w, x, y, r, h):
    """would a prop of radius r and height h standing at (x, y) hide the
    road, a pad or the fort's plate behind it? On the screen it rises
    0.75 h metres of ground above its foot; it may reach the near edge of
    what is behind and a little more, not further."""
    reach = 0.75 * h
    for p in w.paths:
        for (px, py) in p:
            if abs(px - x) < r + HALF - 0.1 and 0.0 < py - y < reach + HALF - 0.4:
                return True
    for (px, py) in w.pads:
        if abs(px - x) < r + 0.8 and 0.0 < py - y < reach + 0.5:
            return True
    fx, fy = w.fort
    if abs(fx - x) < r + 2.4 and 0.0 < fy - y < reach + 2.2:
        return True
    return False


# ---------------------------------------------------------------------------
# What the kit lacks: UVs on the road, kerbs that know about junctions,
# things placed along the road, a backdrop for the void
# ---------------------------------------------------------------------------

def road(G, mat, lift=0.004):
    """The road, cut out of the ground's grid: every vertex nearer than
    half the road's width to its centre line, with the rim moved onto that
    line exactly, laid flat just above Z = 0. It gets U = 0 on the centre
    line to 1 at the edge and V = the metres left to the fort, so its
    material can draw ruts, slabs and marks that follow the way.

    K.road() is not used: it lays one ribbon a path at the same height, so
    where two roads share their way (selva) two identical ribbons shadow
    each other and the stretch renders black, and on a bend tighter than
    half the road's width (selva's last one) the ribbon folds over itself
    and leaves a black wedge. Cut from the distance to the centre line the
    road is one sheet whatever the paths do."""
    inside = G.d_road < HALF
    XY, ring = G.contour(G.d_road, HALF, inside)
    return G.build(mat, name='road', keep=inside | ring, XY=XY, Z=np.full(G.X.shape, lift, np.float32),
                   uv=(np.clip(G.d_road / HALF, 0.0, 1.0), G.s_road))


def road_uv_sockets(M):
    """(0 on the centre line .. 1 at the edge, metres to the fort) in a road's material"""
    u, v, _ = M.xyz(M.coord('UV'))
    return u, v


def _normals(p):
    out = []
    for i in range(len(p)):
        a, b = p[max(0, i - 1)], p[min(len(p) - 1, i + 1)]
        dx, dy = b[0] - a[0], b[1] - a[1]
        l = math.hypot(dx, dy) or 1.0
        out.append((-dy / l, dx / l))
    return out


def kerbs(w, mat, width=0.14, height=0.05, offset=0.0, name='kerb'):
    """the kerbs K.road() would lay, but broken where they would run over a
    road's surface (the kit's kerb crosses the other road at a junction,
    and folds over its own on a bend tighter than the road is wide) and
    laid once where two roads share their way"""
    done = set()
    every = [q for p in w.paths for q in p]
    for i, p in enumerate(w.paths):
        for s in (1, -1):
            run = []
            for (x, y), (nx, ny) in zip(p, _normals(p)):
                k = HALF + offset + width / 2
                qx, qy = x + nx * s * k, y + ny * s * k
                key = (round(qx, 2), round(qy, 2))
                inside = any(abs(qx - ox) < k and abs(qy - oy) < k and math.hypot(qx - ox, qy - oy) < k - width * 0.15
                             for (ox, oy) in every)
                if inside or key in done:
                    if len(run) > 1:
                        K.ribbon(run, width / 2, height, mat, z_side=0.0, name=name)
                    run = []
                    if not inside:
                        run = [(qx, qy)]
                    continue
                done.add(key)
                run.append((qx, qy))
            if len(run) > 1:
                K.ribbon(run, width / 2, height, mat, z_side=0.0, name=name)


def along_road(w, spacing, side_offset, phase=0.0):
    """places beside the road every 'spacing' metres, side_offset from its
    centre on both sides: (x, y, yaw of the road there in degrees, side
    +1/-1). Those that would land on another road, a pad or the fort's
    plate are left out."""
    out, seen = [], set()
    for p in w.paths:
        n = _normals(p)
        s = [0.0] * len(p)
        for k in range(len(p) - 2, -1, -1):
            s[k] = s[k + 1] + math.hypot(p[k + 1][0] - p[k][0], p[k + 1][1] - p[k][1])
        for k in range(len(p) - 1):
            a = math.floor((s[k + 1] - phase) / spacing)
            b = math.floor((s[k] - phase) / spacing)
            if a == b:
                continue
            (x, y), (nx, ny) = p[k], n[k]
            for side in (1, -1):
                qx, qy = x + nx * side * side_offset, y + ny * side * side_offset
                key = (round(qx, 1), round(qy, 1))
                if key in seen:
                    continue
                seen.add(key)
                if w.dist_path(qx, qy) < side_offset - 0.12:
                    continue
                if any(math.hypot(qx - px, qy - py) < bsworlds.PAD_R + 0.15 for (px, py) in w.pads):
                    continue
                if math.hypot(qx - w.fort[0], qy - w.fort[1]) < bsworlds.FORT_R + 0.15:
                    continue
                out.append((qx, qy, math.degrees(math.atan2(nx, -ny)), side))
    return out


def offset_runs(w, off, test):
    """lines running beside every road, 'off' from its centre on both
    sides, as lists of points: cut where test(x, y) fails, where another
    road is nearer than 'off', and on the inside of a bend too tight for
    them (pipes, rails, rows of lights)"""
    runs, seen = [], set()
    for p in w.paths:
        n = _normals(p)
        for side in (1, -1):
            run = []
            for (x, y), (nx, ny) in zip(p, n):
                q = (x + nx * side * off, y + ny * side * off)
                key = (round(q[0], 1), round(q[1], 1))
                good = key not in seen and test(*q) and w.dist_path(*q) > off - 0.12
                if good and run and (q[0] - run[-1][0]) * ny - (q[1] - run[-1][1]) * nx <= 0.02:
                    continue
                if good:
                    run.append(q)
                    seen.add(key)
                else:
                    if len(run) >= 3:
                        runs.append(run)
                    run = []
            if len(run) >= 3:
                runs.append(run)
    return runs


def backdrop(mat, dist=150.0, name='backdrop'):
    """a sheet filling the whole screen far behind everything, square to
    the camera: the void. Its object coordinates are metres on the screen
    (X right, Y up, the origin at the screen's centre), 40 px each."""
    fwd = Vector((0, B.COS_E, -B.SIN_E))
    c = Vector((0, B.SCREEN_H / 2.0 / B.ROW_Y, 0)) + fwd * dist
    hw, hh = B.SCREEN_W / B.PX / 2 + 0.3, B.SCREEN_H / B.PX / 2 + 0.3
    o = B.mesh([(-hw, -hh, 0), (hw, -hh, 0), (hw, hh, 0), (-hw, hh, 0)], [(0, 1, 2, 3)], mat, name=name)
    o.location = c
    o.rotation_euler = Euler((math.asin(B.COS_E), 0, 0), 'XYZ')
    o.visible_shadow = False
    return o


# ---------------------------------------------------------------------------
# Props shared by more than one world
# ---------------------------------------------------------------------------

def _link(name, bm, m, smooth=False):
    me = bpy.data.meshes.new(name)
    bm.normal_update()
    bm.to_mesh(me)
    bm.free()
    if smooth:
        for p in me.polygons:
            p.use_smooth = True
    if m is not None:
        me.materials.append(m)
    o = bpy.data.objects.new(name, me)
    B.sc.collection.objects.link(o)
    return o


class Heap:
    """many small things in one mesh (pebbles, shards, leaves): scattering
    a thousand objects one by one is slow, a thousand shapes in one is not"""

    def __init__(self):
        self.bm = bmesh.new()

    @staticmethod
    def _m(loc, rot, scale):
        return (Matrix.Translation(loc) @ Euler([math.radians(a) for a in rot], 'XYZ').to_matrix().to_4x4()
                @ Matrix.Diagonal((*scale, 1.0)))

    def rock(self, r, loc, rot=(0, 0, 0), scale=(1, 1, 1), subdiv=2, lump=0.35, seed=0.0):
        """a lumpy stone: an icosphere pushed about by a noise"""
        ret = bmesh.ops.create_icosphere(self.bm, subdivisions=subdiv, radius=1.0)
        M = self._m(loc, rot, (r * scale[0], r * scale[1], r * scale[2]))
        for v in ret['verts']:
            k = 1.0 + lump * mnoise.noise(v.co * 1.3 + Vector((seed, seed * 1.7, -seed)))
            k += lump * 0.4 * mnoise.noise(v.co * 3.1 + Vector((-seed, seed, seed * 0.3)))
            v.co = M @ (v.co * k)

    def cone(self, r1, r2, h, loc, rot=(0, 0, 0), segs=6, scale=(1, 1, 1)):
        """a cone or a prism standing on loc"""
        M = self._m(loc, rot, scale) @ Matrix.Translation((0, 0, h / 2))
        bmesh.ops.create_cone(self.bm, cap_ends=True, cap_tris=False, segments=segs, radius1=r1,
                              radius2=max(r2, 1e-4), depth=h, matrix=M)

    def shard(self, r, h, loc, rot=(0, 0, 0), segs=6, waist=0.72, tip=0.8):
        """a crystal: a prism of 'segs' sides that narrows to a point, a
        little thicker at 'waist' of its height than at its foot"""
        M = self._m(loc, rot, (1, 1, 1))
        rings = []
        for (rr, z) in ((r * tip, 0.0), (r, h * waist), (0.0, h)):
            rings.append([self.bm.verts.new(M @ Vector((max(rr, 1e-4) * math.cos(2 * math.pi * j / segs),
                                                        max(rr, 1e-4) * math.sin(2 * math.pi * j / segs), z)))
                          for j in range(segs)])
        for i in range(2):
            a, b = rings[i], rings[i + 1]
            for j in range(segs):
                self.bm.faces.new((a[j], a[(j + 1) % segs], b[(j + 1) % segs], b[j]))
        self.bm.faces.new(list(reversed(rings[0])))

    def quad(self, pts):
        self.bm.faces.new([self.bm.verts.new(p) for p in pts])

    def done(self, name, mat, smooth=False):
        bmesh.ops.remove_doubles(self.bm, verts=self.bm.verts, dist=1e-5)
        return _link(name, self.bm, mat, smooth)


def rocks(G, rng, heap, n, rmin, rmax, taken, squash=(0.55, 0.9), weight=None, sink=0.25, big_gap=1.0):
    """n stones between rmin and rmax (mostly small) dropped on the ground
    into 'heap'; returns where they landed"""
    def r_of(g):
        return rmin + (rmax - rmin) * g.random() ** 2.2
    pts = G.sprinkle(rng, n, r_of, lambda r: r * 1.5, taken, weight=weight, own_gap=big_gap)
    for (x, y, r) in pts:
        sz = rng.uniform(*squash)
        heap.rock(r, (x, y, G.height(x, y) + r * sz * (1 - 2 * sink) * 0.5),
                  rot=(rng.uniform(-15, 15), rng.uniform(-15, 15), rng.uniform(0, 360)),
                  scale=(rng.uniform(0.8, 1.2), rng.uniform(0.8, 1.2), sz), subdiv=2 if r > 0.25 else 1,
                  seed=rng.uniform(0, 50))
    return pts


def crystal_cluster(heap, rng, x, y, z, size, n=None, lean=28.0, segs=6):
    """a burst of crystals into 'heap': one tall in the middle, shorter
    ones leaning out around it; 'size' is the height of the tallest"""
    n = n or rng.randint(4, 7)
    heap.shard(size * 0.16, size, (x, y, z - 0.05), rot=(rng.uniform(-8, 8), rng.uniform(-8, 8), rng.uniform(0, 60)),
               segs=segs)
    for i in range(n):
        a = 2 * math.pi * (i + rng.uniform(-0.3, 0.3)) / n
        d = size * rng.uniform(0.10, 0.20)
        hh = size * rng.uniform(0.35, 0.7)
        tilt = rng.uniform(lean * 0.5, lean)
        heap.shard(hh * rng.uniform(0.14, 0.2), hh, (x + math.cos(a) * d, y + math.sin(a) * d, z - 0.05),
                   rot=(-tilt * math.sin(a), tilt * math.cos(a), rng.uniform(0, 60)), segs=segs)


def lamp(loc, power, color, radius=0.25):
    """a point light that casts the pool of colour under a shining prop"""
    return B.point(loc, power, B.rgb(color), radius)


# ---------------------------------------------------------------------------
# luna: grey regolith pocked with craters under a hard sun and the blue
# light of a planet; bursts of cyan crystal, and the gold and white of the
# first outpost
# ---------------------------------------------------------------------------

def build_luna(w):
    rng = random.Random(1969)
    G = Ground(w)
    X, Y = G.X, G.Y
    flat = G.flat(0.25, 1.7)
    taken = []

    # --- relief: rolling ground, then craters of every size -------------
    Z = (fbm(X, Y, 8.0, 4, seed=1) - 0.5) * 1.1 + (fbm(X, Y, 1.5, 3, seed=2) - 0.5) * 0.14
    bowl = np.zeros(X.shape, np.float32)      # 1 on a crater's floor
    rim = np.zeros(X.shape, np.float32)       # 1 on its rim
    rays = np.zeros(X.shape, np.float32)      # the bright ejecta of the fresh ones
    sites = outpost_sites(G, rng, taken)
    craters = []
    for k in range(260):
        r = 0.45 + 3.0 * rng.random() ** 3.0
        x, y = rng.uniform(-10, 10), rng.uniform(-2, 42)
        room = G.free_at(x, y)
        if r > 1.0 and room < r * 0.75:
            continue
        if r <= 1.0 and room < 0.3:
            continue
        if any(math.hypot(x - cx, y - cy) < (r + cr) * 0.8 for (cx, cy, cr) in craters):
            continue
        if any(math.hypot(x - tx, y - ty) < r + tr + 0.4 for (tx, ty, tr) in taken):
            continue
        craters.append((x, y, r))
        sl = G.window(x, y, r * 3.2)
        dx, dy = X[sl] - x, Y[sl] - y
        t = np.hypot(dx, dy) / r
        wob = 1.0 + 0.10 * perlin(dx / r * 1.5 + k, dy / r * 1.5 - k, 5)
        t = t * wob
        depth, lip = min(0.26 * r, 0.75), min(0.10 * r + 0.03, 0.30)
        inside = np.clip(1.0 - t * t, 0, 1)
        Z[sl] += -depth * inside ** 1.3 + lip * np.exp(-((t - 1.02) / 0.20) ** 2) \
            + lip * 0.5 * np.exp(-np.maximum(t - 1.0, 0) / 0.5) * (t > 1.0)
        bowl[sl] = np.maximum(bowl[sl], smooth(1.0, 0.55, t))
        rim[sl] = np.maximum(rim[sl], np.exp(-((t - 1.0) / 0.16) ** 2))
        if r > 0.8 and rng.random() < 0.45:
            ang = np.arctan2(dy, dx)
            streak = fbm(ang * 2.6 + k, t * 0.25, 1.0, 3, seed=7) ** 2
            rays[sl] = np.maximum(rays[sl], streak * np.exp(-np.maximum(t - 1.0, 0) / 0.9) * (t > 0.9) * 1.6)
    G.Z = Z * flat

    # --- paint -------------------------------------------------------------
    tone = fbm(X, Y, 9.0, 4, seed=11)
    col = lerp(C('#59617a'), C('#8d94aa'), tone)
    mare = smooth(0.52, 0.66, fbm(X, Y, 14.0, 3, seed=12))
    col = lerp(col, C('#454c66'), mare * 0.8)
    col = lerp(col, C('#3d4258'), bowl * 0.55)
    col = lerp(col, C('#c2c8d8'), np.clip(rim * 0.75 + rays * 0.5, 0, 1))
    speck = fbm(X, Y, 0.35, 2, seed=13)
    col *= (0.9 + 0.2 * speck)[..., None]
    # the verge beside the road is trodden darker, which also sets the road off
    verge = smooth(0.9, 0.15, G.free) * (G.free > -0.2)
    col = lerp(col, C('#4a5068'), verge * 0.6)
    col = sockets(G, col, '#9aa2b8', 0.7)
    emit = np.zeros(col.shape, np.float32)

    # --- crystals: big bursts where there is room, small ones all over -----
    cyan = crystal_mat('luna_crystal', '#0a58c8', '#7af4ff', k=1.9, top=2.2)
    heap = Heap()
    for (x, y, room) in G.spots(rng, 9, 0.7, h=3.4, gap=5.0, taken=taken):
        size = min(3.6, 1.9 + room * 0.5) * rng.uniform(0.8, 1.0)
        while size > 1.0 and not G.ok(x, y, 0.7, size):
            size -= 0.3
        crystal_cluster(heap, rng, x, y, G.height(x, y), size)
        lamp((x, y, G.height(x, y) + size * 0.5), 260 * size, '#40d8ff', 0.4)
        G.splat(emit, x, y, 1.0 + size * 0.35, C('#1890c8') * 0.30)
    # crags: big stones standing out of the dust, crystals grown on their heads
    crag = Heap()
    for (x, y, room) in G.spots(rng, 7, 1.3, h=4.0, gap=4.5, taken=taken, xr=(-9.3, 9.3)):
        R = min(1.6, 0.9 + room * 0.2) * rng.uniform(0.85, 1.0)
        z = G.height(x, y)
        crag.rock(R, (x, y, z + R * 0.45), rot=(rng.uniform(-10, 10), rng.uniform(-10, 10), rng.uniform(0, 360)),
                  scale=(1.0, 0.85, 1.2), subdiv=3, lump=0.5, seed=rng.uniform(0, 50))
        for i in range(3):
            a = rng.uniform(0, 6.28)
            rr = R * rng.uniform(0.3, 0.5)
            crag.rock(rr, (x + math.cos(a) * R * 0.95, y + math.sin(a) * R * 0.8, z + rr * 0.4),
                      rot=(0, 0, rng.uniform(0, 360)), scale=(1, 1, 0.8), subdiv=2, lump=0.45, seed=rng.uniform(0, 50))
        for i in range(rng.randint(2, 3)):
            a = rng.uniform(0, 6.28)
            crystal_cluster(heap, rng, x + math.cos(a) * R * 0.35, y + math.sin(a) * R * 0.3, z + R * 1.35,
                            R * rng.uniform(0.6, 0.95), n=rng.randint(3, 5), lean=38.0)
        lamp((x, y - R * 0.4, z + R * 2.2), 160, '#40d8ff', 0.4)
        G.splat(emit, x, y, R * 1.4, C('#1890c8') * 0.14)
    crag.done('crags', B.noise_mat('luna_crag', '#3e4458', '#8a90a6', scale=2.2, rough=0.9, bump=0.7), smooth=False)
    for (x, y, r) in G.sprinkle(rng, 46, lambda g: g.uniform(0.2, 0.34), lambda r: r * 3.2, taken):
        size = r * 3.2
        crystal_cluster(heap, rng, x, y, G.height(x, y), size, n=rng.randint(2, 4))
        G.splat(emit, x, y, 0.5 + size * 0.3, C('#1890c8') * 0.22)
    for (x, y, r) in G.sprinkle(rng, 70, lambda g: g.uniform(0.08, 0.14), lambda r: 0.4, taken):
        crystal_cluster(heap, rng, x, y, G.height(x, y), r * 3.6, n=rng.randint(1, 3))
        G.splat(emit, x, y, 0.4, C('#1890c8') * 0.16)
    heap.done('crystals', cyan)

    # --- the outpost, and the tracks its rover left between its parts ------
    stops = [q for k in ('lander', 'dome', 'solar', 'mast') for q in sites[k]]
    for (a, b) in zip(stops, stops[1:]):
        sl = G.window((a[0] + b[0]) / 2, (a[1] + b[1]) / 2, math.hypot(b[0] - a[0], b[1] - a[1]) / 2 + 1.0)
        dx, dy = b[0] - a[0], b[1] - a[1]
        t = np.clip(((X[sl] - a[0]) * dx + (Y[sl] - a[1]) * dy) / (dx * dx + dy * dy), 0.0, 1.0)
        d = np.hypot(X[sl] - (a[0] + t * dx), Y[sl] - (a[1] + t * dy))
        rut = smooth(0.07, 0.03, np.abs(d - 0.30)) * (G.free[sl] > 0.3)
        col[sl] = lerp(col[sl], C('#343a50'), rut * 0.7)
    outpost(G, rng, sites, taken, emit)

    # --- stones --------------------------------------------------------------
    stone = B.noise_mat('luna_rock', '#4c5268', '#8a90a4', scale=5.0, rough=0.9, bump=0.5)
    heap = Heap()
    rocks(G, rng, heap, 260, 0.06, 0.55, taken)
    heap.done('rocks', stone, smooth=False)

    hud_calm(G, col, emit, dim=0.15, hush=0.4)
    G.build(ground_mat('luna_ground', rough=0.92, spec=0.15, grain=(22.0, 0.10), bump=(0.35, 0.03),
                       lumps=(3.5, 0.25), emit=True), dict(col=col, emit=emit))

    # --- the road, the pads, the plate -----------------------------------
    M = Mat('luna_road')
    u, v = road_uv_sockets(M)
    n = M.noise(9.0, detail=5.0)
    base = M.ramp(n, [(0.3, '#8f97ae'), (0.7, '#b4bbcc')])
    # two wheel ruts and a seam every two metres, as on a laid track
    rut = M.math('MULTIPLY', M.remap(M.math('ABSOLUTE', M.math('SUBTRACT', u, 0.54)), 0.0, 0.18, 1.0, 0.0,
                                     smoothstep=True), 0.22)
    seam = M.remap(M.math('ABSOLUTE', M.math('SUBTRACT', M.math('FRACT', M.math('MULTIPLY', v, 0.5)), 0.5)),
                   0.0, 0.03, 1.0, 0.0)
    dark = M.math('MAXIMUM', rut, M.math('MULTIPLY', seam, 0.35))
    M.set(Base_Color=M.mix(dark, base, '#59607a'), Roughness=0.8, Specular=0.2,
          Normal=M.bump(M.math('SUBTRACT', n, dark), 0.25, 0.03))
    road(G, M.m)
    kerbs(w, B.mat('luna_kerb', '#e4e8f2', rough=0.5, metal=0.2))
    plate = B.mat('luna_plate', '#3a4258', rough=0.4, metal=0.7)
    K.pads(w, plate, rim_k=4.0, inner_mat=B.mat('luna_plate_in', '#262c3c', rough=0.5, metal=0.6))
    K.foundation(w, plate)
    # small cyan studs along the road, like runway lights
    stud = B.glow('luna_stud', '#60e8ff', 3.0)
    post = B.mat('luna_post', '#d8dce8', rough=0.4, metal=0.5)
    for (x, y, yaw, side) in along_road(w, 2.4, HALF + 0.36):
        B.cyl(0.07, 0.10, post, loc=(x, y, G.height(x, y) - 0.02), segs=8)
        B.sphere(0.065, stud, loc=(x, y, G.height(x, y) + 0.10), segs=10)

    # --- light: a low hard sun, the planet's blue from behind ---------------
    B.sun(205, 32, 4.0, (1.0, 0.92, 0.78), angle=3.0, name='key')
    B.sun(35, 28, 1.0, (0.30, 0.55, 1.0), angle=25, name='planet')
    B.sun(300, 40, 0.3, (0.7, 0.8, 1.0), angle=40, name='fill')
    B.ambient((0.30, 0.38, 0.62), 0.24)


def outpost_sites(G, rng, taken):
    """where the outpost's parts stand, chosen before the craters are dug so
    that none of them ends up on a crater's wall"""
    return dict(lander=G.spots(rng, 1, 1.5, h=2.6, taken=taken),
                dome=G.spots(rng, 2, 1.3, h=1.5, gap=3.0, taken=taken),
                solar=G.spots(rng, 3, 1.1, h=1.5, gap=2.4, taken=taken),
                mast=G.spots(rng, 1, 0.5, h=4.6, taken=taken))


def outpost(G, rng, sites, taken, emit):
    """what people left on the moon: a lander in gold foil, two domes with
    lit windows, a field of solar panels, a mast, crates"""
    gold = B.noise_mat('foil', '#b87a14', '#ffd866', scale=11.0, rough=0.32, metal=1.0, bump=0.7)
    white = B.mat('hull_white', '#eef1f7', rough=0.45, clear=0.3)
    dark = B.mat('hull_dark', '#252a38', rough=0.5, metal=0.6)
    steel = B.mat('hull_steel', '#aab4c8', rough=0.3, metal=1.0)
    red = B.mat('hull_red', '#ff4a38', rough=0.4, clear=0.5)
    orange = B.mat('hull_orange', '#ff8a1e', rough=0.4, clear=0.5)
    warm = B.glow('win_warm', '#ffb648', 3.0)
    blink = B.glow('blink_red', '#ff3028', 4.0)
    cyan = B.glow('lamp_cyan', '#60e8ff', 3.0)

    for (x, y, room) in sites['lander']:
        z = G.height(x, y)
        g = B.group('lander', loc=(x, y, z + 0.05), rot=(0, 0, rng.uniform(0, 90)))
        for a in (45, 135, 225, 315):
            ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
            B.tube([(0.6 * ca, 0.6 * sa, 0.95), (1.30 * ca, 1.30 * sa, 0.08)], 0.05, steel, g)
            B.tube([(0.6 * ca, 0.6 * sa, 0.55), (1.30 * ca, 1.30 * sa, 0.08)], 0.035, steel, g)
            B.cyl(0.22, 0.06, gold, g, loc=(1.30 * ca, 1.30 * sa, 0.0), segs=12)
        B.prism(B.ngon(8, 0.92, 22.5), 0.5, 1.15, gold, g, bevel=0.04)
        B.cyl(0.36, 0.34, dark, g, loc=(0, 0, 0.18), r2=0.2)
        B.lathe([(0.62, 1.15), (0.78, 1.42), (0.78, 1.85), (0.48, 2.2), (0.2, 2.26)], white, g, segs=8, smooth=False)
        for a in (-90, 180):
            ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
            B.box((0.34, 0.06, 0.26), warm, g, loc=(0.74 * ca, 0.74 * sa, 1.64), rot=(0, 0, a + 90), bevel=0.0)
        B.tube([(0.3, 0.2, 2.2), (0.55, 0.35, 2.75)], 0.03, steel, g)
        B.lathe([(0.02, 0.0), (0.2, 0.05), (0.34, 0.16)], white, g, loc=(0.55, 0.35, 2.72), rot=(25, 30, 0), segs=16)
        B.sphere(0.07, blink, g, loc=(0, 0, 2.34))
        lamp((x, y - 1.2, z + 1.6), 60, '#ffb648', 0.3)

    for (x, y, room) in sites['dome']:
        z = G.height(x, y)
        g = B.group('dome', loc=(x, y, z), rot=(0, 0, rng.uniform(-30, 30)))
        B.cyl(1.2, 0.22, dark, g, loc=(0, 0, -0.1), segs=24)
        B.lathe([(1.12, 0.1), (1.08, 0.5), (0.9, 0.9), (0.55, 1.2), (0.0, 1.32)], white, g, segs=24)
        B.torus(1.115, 0.05, orange, g, loc=(0, 0, 0.5), segs=32)
        for a in range(-150, -20, 30):
            ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
            B.box((0.26, 0.08, 0.18), warm, g, loc=(1.09 * ca, 1.09 * sa, 0.30), rot=(0, 0, a + 90), bevel=0.0)
        B.box((0.7, 0.8, 0.6), white, g, loc=(1.25, 0, 0.3), bevel=0.06)
        B.box((0.06, 0.4, 0.42), dark, g, loc=(1.61, 0, 0.26), bevel=0.0)
        B.sphere(0.06, cyan, g, loc=(0, 0, 1.36))
        lamp((x, y - 1.6, z + 0.5), 40, '#ffb648', 0.3)
        G.splat(emit, x, y - 1.3, 0.8, C('#ff9a30') * 0.10)

    panel = Mat('solar')
    bricks = panel.node('TexBrick', dict(offset=0.0), Vector=panel.coord(), Color1=B.lin('#1438a8'),
                        Color2=B.lin('#1e50d0'), Mortar=B.lin('#b8c4e0'), Scale=5.0, Mortar_Size=0.02,
                        Brick_Width=0.5, Row_Height=0.5)
    panel.set(Base_Color=bricks.outputs['Color'], Roughness=0.12, Metallic=0.6, Specular=0.9, Clearcoat=1.0)
    for (x, y, room) in sites['solar']:
        z = G.height(x, y)
        g = B.group('solar', loc=(x, y, z), rot=(0, 0, 25))
        B.cyl(0.07, 0.9, steel, g, loc=(0, 0, 0), segs=10)
        t = B.group('tilt', g, loc=(0, 0, 0.95), rot=(38, 0, 180))
        B.box((2.0, 1.2, 0.05), panel.m, t, bevel=0.0)
        B.box((2.08, 1.28, 0.03), steel, t, loc=(0, 0, -0.03), bevel=0.0)

    for (x, y, room) in sites['mast']:
        z = G.height(x, y)
        g = B.group('mast', loc=(x, y, z))
        for a in (30, 150, 270):
            ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
            B.tube([(0.42 * ca, 0.42 * sa, 0.0), (0.06 * ca, 0.06 * sa, 4.0)], 0.035, steel, g)
        for zz in (0.9, 1.8, 2.7, 3.5):
            B.torus(0.42 - 0.09 * zz, 0.025, steel, g, loc=(0, 0, zz), segs=12)
        B.cyl(0.05, 0.6, red, g, loc=(0, 0, 3.9), segs=8)
        B.sphere(0.09, blink, g, loc=(0, 0, 4.56))
        B.lathe([(0.02, 0.0), (0.26, 0.06), (0.42, 0.2)], white, g, loc=(0.1, -0.15, 3.3), rot=(60, 0, 20), segs=16)

    crate = [orange, red, B.mat('crate_y', '#ffd23a', rough=0.4, clear=0.4), white]
    for (x, y, r) in G.sprinkle(rng, 9, lambda g_: 0.28, lambda r: 0.45, taken):
        s = rng.uniform(0.34, 0.46)
        B.box((s, s, s), rng.choice(crate), loc=(x, y, G.height(x, y) + s / 2 - 0.02), rot=(0, 0, rng.uniform(0, 90)),
              bevel=0.03)


# ---------------------------------------------------------------------------
# selva: a jungle at night that makes its own light. Dark teal earth under
# moss, pools of shining water, giant mushrooms in magenta, cyan, orange
# and violet, lantern plants, ferns, and umbrella trees at the sides
# ---------------------------------------------------------------------------

def frond(heap, x, y, z, L, yaw, rise, width, droop=0.78, n=6):
    """a leaf arching out of (x, y, z) towards yaw (radians): a strip with
    a keel, widest in the middle, drooping at its tip"""
    ca, sa = math.cos(yaw), math.sin(yaw)
    prev = None
    for i in range(n + 1):
        t = i / n
        r = L * t
        zz = z + rise * math.sin(math.pi * t * droop)
        wd = width * math.sin(math.pi * min(1.0, t * 0.9 + 0.1)) ** 0.7
        c = (x + ca * r, y + sa * r, zz)
        cur = ((c[0] - sa * wd, c[1] + ca * wd, zz + wd * 0.4), c, (c[0] + sa * wd, c[1] - ca * wd, zz + wd * 0.4))
        if prev:
            heap.quad((prev[0], prev[1], cur[1], cur[0]))
            heap.quad((prev[1], prev[2], cur[2], cur[1]))
        prev = cur


def fern(heap, rng, x, y, z, size, n=None):
    n = n or rng.randint(6, 9)
    a0 = rng.uniform(0, 6.28)
    for i in range(n):
        frond(heap, x, y, z, size * rng.uniform(0.7, 1.0), a0 + 6.283 * i / n + rng.uniform(-0.25, 0.25),
              size * rng.uniform(0.35, 0.7), size * 0.15)


def cap_mat(name, c, spot, k=0.55):
    """a mushroom's cap: its own colour, with pale spots that shine"""
    M = Mat(name)
    d = M.voronoi(3.0, 'F1')
    sp = M.remap(d, 0.10, 0.17, 1.0, 0.0, smoothstep=True)
    col = M.mix(sp, c, spot)
    M.set(Base_Color=col, Roughness=0.3, Clearcoat=0.6, Specular=0.6, Emission=col,
          Emission_Strength=M.math('ADD', k, M.math('MULTIPLY', sp, 1.5)))
    return M.m


def mushroom(g, h, R, cap, gills, stem):
    """a mushroom h tall to under its cap, the cap R wide, under the group g"""
    sr = R * 0.15 + 0.05
    B.lathe([(sr * 1.8, 0.0), (sr * 1.25, h * 0.12), (sr * 0.9, h * 0.5), (sr * 0.8, h * 0.9), (sr * 0.9, h)], stem, g,
            segs=14)
    B.lathe([(sr * 0.85, h * 0.70), (sr * 2.0, h * 0.62), (sr * 0.9, h * 0.74)], stem, g, segs=14)
    B.lathe([(sr * 0.8, h * 0.93), (R * 0.55, h * 0.93), (R * 0.97, h * 0.99), (R * 0.9, h * 1.02)], gills, g, segs=28)
    B.lathe([(R, h), (R * 0.99, h + R * 0.06), (R * 0.88, h + R * 0.26), (R * 0.62, h + R * 0.44),
             (R * 0.3, h + R * 0.54), (0.0, h + R * 0.57)], cap, g, segs=28)


def build_selva(w):
    rng = random.Random(4242)
    G = Ground(w)
    X, Y = G.X, G.Y
    flat = G.flat(0.25, 1.8)
    taken = []

    # --- relief: mounds, and hollows that hold shining water --------------
    Z = (fbm(X, Y, 6.0, 4, seed=21) - 0.45) * 1.5 + (fbm(X, Y, 1.1, 3, seed=22) - 0.5) * 0.22
    pool = smooth(0.36, 0.30, fbm(X, Y, 5.5, 3, seed=23)) * smooth(1.3, 2.3, G.free)
    WATER = -0.20
    G.Z = Z * flat * (1.0 - pool) - pool * 0.6
    wet = smooth(WATER + 0.30, WATER + 0.02, G.Z) * smooth(0.5, 1.0, G.free)     # the shore

    # --- paint: earth, moss, lichen, the shore, fields of spores -----------
    tone = fbm(X, Y, 5.0, 4, seed=24)
    col = lerp(C('#0b2a30'), C('#144846'), tone)
    moss = smooth(0.50, 0.66, fbm(X, Y, 2.6, 4, seed=25))
    col = lerp(col, lerp(C('#17704c'), C('#3aa858'), fbm(X, Y, 1.0, 3, seed=26)), moss * 0.8)
    lichen = smooth(0.64, 0.74, fbm(X, Y, 3.4, 4, seed=27))
    col = lerp(col, C('#4a2c8a'), lichen * 0.7)
    col = lerp(col, C('#16a898'), wet * 0.7)
    col *= (0.88 + 0.24 * fbm(X, Y, 0.3, 2, seed=28))[..., None]
    # the earth beside the trail and round the pads is bare and dark: a calm
    # margin that sets them off from the busy ground
    verge = smooth(1.3, 0.2, G.free) * (G.free > -0.2)
    col = lerp(col, C('#0a2228'), verge * 0.8)
    col = sockets(G, col, '#5f8a7c', 0.8)
    emit = np.zeros(col.shape, np.float32)
    emit += (wet ** 2)[..., None] * C('#20e8d0') * 0.35
    f1, edge, rid, cx, cy = cells(X, Y, 0.5, seed=29)
    field = smooth(0.56, 0.66, fbm(X, Y, 4.0, 3, seed=30)) * (G.free > 0.25)
    dot = smooth(0.07, 0.03, f1) * (rid > 0.55) * field
    emit += dot[..., None] * np.where((rid > 0.78)[..., None], C('#ff40d8'), C('#40f0ff')) * 2.5

    # --- giant mushrooms ----------------------------------------------------
    kinds = [('#ff2fc8', '#ffd0f4'), ('#22d8ff', '#d8fbff'), ('#ff8a1e', '#fff0b0'), ('#9a5cff', '#e8dcff'),
             ('#b8f03c', '#f8ffd0')]
    caps = [cap_mat('cap%d' % i, c, s) for i, (c, s) in enumerate(kinds)]
    gills = [B.glow('gill%d' % i, s, 2.2) for i, (c, s) in enumerate(kinds)]
    stem = B.mat('stem', '#d8c8e8', rough=0.6, emit='#b8a0e0', emit_k=0.25)
    order = 0
    for (R, h, n, gap) in ((1.9, 3.6, 5, 6.5), (1.3, 2.6, 8, 4.0), (0.85, 1.7, 14, 2.6)):
        for (x, y, room) in G.spots(rng, n, 0.45, h=h + R * 0.9, gap=gap, taken=taken, r_top=R, xr=(-9.2, 9.2)):
            k = order % len(kinds)
            order += 1
            hh, RR = h * rng.uniform(0.85, 1.1), R * rng.uniform(0.9, 1.05)
            z = G.height(x, y)
            g = B.group('mushroom', loc=(x, y, z - 0.08), rot=(rng.uniform(-7, 7), rng.uniform(-7, 7), rng.uniform(0, 360)))
            mushroom(g, hh, RR, caps[k], gills[k], stem)
            lamp((x, y - RR * 0.2, z + hh * 0.72), 220 * RR, kinds[k][0], 0.3)
            G.splat(emit, x, y, RR * 1.2, C(kinds[k][0]) * 0.16)
    # small ones in troops, a heap a colour
    heaps = [Heap() for _ in kinds]
    stems = Heap()
    for (x, y, r) in G.sprinkle(rng, 70, lambda g_: g_.uniform(0.25, 0.45), lambda r: 0.45, taken):
        k = rng.randrange(len(kinds))
        for i in range(rng.randint(2, 5)):
            a, d = rng.uniform(0, 6.28), rng.uniform(0, r)
            mx, my = x + math.cos(a) * d, y + math.sin(a) * d
            if G.free_at(mx, my) < 0.35:
                continue
            mh, mr = rng.uniform(0.12, 0.34), rng.uniform(0.09, 0.2)
            z = G.height(mx, my)
            stems.cone(mr * 0.3, mr * 0.22, mh, (mx, my, z - 0.03), segs=6)
            heaps[k].rock(mr, (mx, my, z + mh), scale=(1, 1, 0.55), subdiv=2, lump=0.0)
        G.splat(emit, x, y, r * 1.6, C(kinds[k][0]) * 0.22)
    stems.done('stems', stem, smooth=True)
    for k, hp in enumerate(heaps):
        hp.done('troop%d' % k, B.mat('troop%d' % k, kinds[k][0], rough=0.3, clear=0.5, emit=kinds[k][0], emit_k=1.3),
                smooth=True)

    # --- umbrella trees where there is room for their crowns ----------------
    bark = B.noise_mat('bark', '#2a1838', '#5a3a6a', scale=6.0, rough=0.8, bump=0.6)
    leaf_dark = Heap()
    pod = B.glow('pod', '#ffb030', 2.6)
    for (x, y, room) in G.spots(rng, 5, 0.5, h=6.2, gap=7.0, taken=taken, r_top=1.9, xr=(-9.6, 9.6)):
        z = G.height(x, y)
        h = rng.uniform(4.6, 5.6)
        bx, by = rng.uniform(-0.7, 0.7), rng.uniform(-0.3, 0.3)
        top = (x + bx, y + by, z + h)
        B.tube([(x, y, z - 0.3), (x + bx * 0.2, y, z + h * 0.35), (x + bx * 0.7, y + by * 0.6, z + h * 0.72), top],
               0.36, bark, r_end=0.14, segs=10)
        for i in range(5):
            a = 6.283 * i / 5 + rng.uniform(-0.3, 0.3)
            B.tube([(x, y, z + 0.25), (x + math.cos(a) * 0.6, y + math.sin(a) * 0.6, z + 0.05),
                    (x + math.cos(a) * 1.0, y + math.sin(a) * 1.0, G.height(x + math.cos(a), y + math.sin(a)) - 0.08)],
                   0.15, bark, r_end=0.05, segs=8)
        a0 = rng.uniform(0, 6.28)
        for i in range(8):
            frond(leaf_dark, top[0], top[1], top[2] - 0.05, rng.uniform(1.6, 2.1), a0 + 6.283 * i / 8 + rng.uniform(-0.2, 0.2),
                  rng.uniform(0.25, 0.7), 0.42, droop=0.85, n=7)
        for i in range(3):
            a = rng.uniform(0, 6.28)
            px, py, pz = top[0] + math.cos(a) * 0.55, top[1] + math.sin(a) * 0.55, top[2] - rng.uniform(0.7, 1.3)
            B.tube([(top[0], top[1], top[2] - 0.1), (px, py, pz + 0.5), (px, py, pz)], 0.03, bark, segs=6)
            B.sphere(0.17, pod, loc=(px, py, pz - 0.1), scale=(1, 1, 1.3), segs=12)
        lamp((top[0], top[1] - 0.5, top[2] - 1.2), 120, '#ffb030', 0.3)
    leaf_dark.done('crowns', B.mat('crown', '#149a8a', rough=0.4, clear=0.4, emit='#10d8b0', emit_k=0.30), smooth=True)

    # --- lantern plants: a stalk that bows under a bulb of light ------------
    stalk = B.mat('stalk', '#2a7a5a', rough=0.5)
    bulbs = [B.glow('bulb_c', '#50f0ff', 3.0), B.glow('bulb_l', '#c8ff50', 2.6), B.glow('bulb_m', '#ff60e0', 2.8)]
    bcol = ['#50f0ff', '#c8ff50', '#ff60e0']
    for (x, y, r) in G.sprinkle(rng, 34, lambda g_: 0.3, lambda r: 1.6, taken):
        k = rng.randrange(3)
        z = G.height(x, y)
        for i in range(rng.randint(2, 3)):
            a = rng.uniform(0, 6.28)
            hh = rng.uniform(0.7, 1.5)
            ex, ey = x + math.cos(a) * hh * 0.35, y + math.sin(a) * hh * 0.35
            B.tube([(x, y, z - 0.05), (x + math.cos(a) * 0.08, y + math.sin(a) * 0.08, z + hh * 0.6),
                    (x + math.cos(a) * hh * 0.22, y + math.sin(a) * hh * 0.22, z + hh), (ex, ey, z + hh * 0.9)],
                   0.035, stalk, r_end=0.02, segs=6)
            B.sphere(0.11, bulbs[k], loc=(ex, ey, z + hh * 0.84), scale=(1, 1, 1.25), segs=12)
        G.splat(emit, x, y, 0.9, C(bcol[k]) * 0.14)

    # --- ferns, low and many, in three greens --------------------------------
    for k, (c, e, n) in enumerate((('#1a9a5e', '#30ffa0', 80), ('#108a94', '#20f0f0', 50), ('#6a40c0', '#b070ff', 30))):
        hp = Heap()
        for (x, y, r) in G.sprinkle(rng, n, lambda g_: g_.uniform(0.3, 0.75), lambda r: r * 0.6, taken, own_gap=0.7):
            fern(hp, rng, x, y, G.height(x, y) + 0.02, r * 1.15)
        hp.done('ferns%d' % k, B.mat('fern%d' % k, c, rough=0.45, clear=0.3, emit=e, emit_k=0.12), smooth=True)

    # --- stones under the moss, lily pads on the water, fireflies ------------
    hp = Heap()
    rocks(G, rng, hp, 90, 0.08, 0.5, taken)
    hp.done('rocks', B.noise_mat('selva_rock', '#2a4a50', '#4f9a6a', scale=3.0, rough=0.85, bump=0.4), smooth=False)
    pads, blooms = Heap(), Heap()
    for _ in range(400):
        x, y = rng.uniform(-9.8, 9.8), rng.uniform(0, 40)
        if G.height(x, y) > WATER - 0.12 or G.free_at(x, y) < 1.0:
            continue
        r = rng.uniform(0.14, 0.3)
        pads.cone(r, r, 0.02, (x, y, WATER + 0.005), segs=10)
        if rng.random() < 0.3:
            blooms.rock(0.07, (x, y, WATER + 0.07), subdiv=1, lump=0.0)
    pads.done('lily', B.mat('lily', '#2fc878', rough=0.4, emit='#30ff90', emit_k=0.2))
    blooms.done('bloom', B.glow('bloom', '#ff70e0', 3.0))
    flies = Heap()
    for _ in range(140):
        x, y = rng.uniform(-9.5, 9.5), rng.uniform(1, 39)
        if G.free_at(x, y) < 0.9:
            continue
        flies.rock(rng.uniform(0.025, 0.04), (x, y, G.height(x, y) + rng.uniform(0.3, 1.2)), subdiv=1, lump=0.0)
    flies.done('flies', B.glow('fly', '#e8ff70', 4.0))

    hud_calm(G, col, emit, dim=0.25, hush=0.6)
    G.build(ground_mat('selva_ground', rough=0.8, spec=0.3, grain=(18.0, 0.16), bump=(0.4, 0.04), lumps=(4.0, 0.3),
                       emit=True), dict(col=col, emit=emit))

    # --- the water: one sheet under everything, seen where the ground dips ---
    M = Mat('selva_water')
    n = M.noise(1.4, detail=3.0, distortion=0.8)
    glow = M.ramp(n, [(0.30, '#0a6a9a'), (0.60, '#14b8c0'), (0.80, '#50f0d8')])
    M.set(Base_Color='#06303c', Roughness=0.06, Specular=0.8, Emission=glow, Emission_Strength=M.hud_dim(0.45),
          Normal=M.bump(M.noise(9.0, detail=2.0), 0.15, 0.02))
    B.mesh([(-11, -4, WATER), (11, -4, WATER), (11, 45, WATER), (-11, 45, WATER)], [(0, 1, 2, 3)], M.m, name='water')

    # --- the trail: flagstones, pale against the dark earth ------------------
    M = Mat('selva_road')
    vor = M.node('TexVoronoi', dict(feature='DISTANCE_TO_EDGE'), Vector=M.coord(), Scale=1.9)
    cell = M.node('TexVoronoi', dict(feature='F1'), Vector=M.coord(), Scale=1.9)
    joint = M.remap(vor.outputs['Distance'], 0.02, 0.07, 1.0, 0.0, smoothstep=True)
    tint = M.xyz(cell.outputs['Color'])[0]
    stone = M.ramp(M.math('ADD', M.math('MULTIPLY', M.noise(7.0, detail=4.0), 0.6), M.math('MULTIPLY', tint, 0.4)),
                   [(0.25, '#54786c'), (0.75, '#8fae9e')])
    M.set(Base_Color=M.mix(joint, stone, '#1c3c38'), Roughness=0.75, Specular=0.25,
          Normal=M.bump(M.math('SUBTRACT', M.noise(7.0, detail=4.0), joint), 0.4, 0.03))
    road(G, M.m)
    kerbs(w, B.noise_mat('selva_kerb', '#8fb89c', '#c4dcc0', scale=4.0, rough=0.7), width=0.16, height=0.07)
    plate = B.mat('selva_plate', '#1a2c36', rough=0.4, metal=0.6)
    K.pads(w, plate, rim_k=4.0, inner_mat=B.mat('selva_plate_in', '#0e1a22', rough=0.5, metal=0.5))
    K.foundation(w, plate)
    # little lights beside the trail, magenta and cyan in turn
    side = [Heap(), Heap()]
    for i, (x, y, yaw, sd) in enumerate(along_road(w, 2.0, HALF + 0.42)):
        z = G.height(x, y)
        side[i % 2].cone(0.03, 0.03, 0.14, (x, y, z - 0.02), segs=6)
        side[i % 2].rock(0.085, (x, y, z + 0.14), scale=(1, 1, 0.6), subdiv=1, lump=0.0)
    side[0].done('side0', B.glow('side_m', '#ff50e0', 3.0), smooth=True)
    side[1].done('side1', B.glow('side_c', '#50f0ff', 3.0), smooth=True)

    # --- light: a cool moon from the kit's side, magenta from behind --------
    B.sun(205, 50, 2.5, (0.72, 0.92, 1.0), angle=8.0, name='key')
    B.sun(40, 30, 0.7, (1.0, 0.35, 0.85), angle=25, name='rim')
    B.sun(300, 35, 0.4, (0.3, 1.0, 0.8), angle=40, name='fill')
    B.ambient((0.12, 0.34, 0.42), 0.40)


# ---------------------------------------------------------------------------
# magma: slabs of black basalt afloat on a sea of lava. Light comes from
# below, through every crack; rivers of it run between the road's bends,
# with volcanoes, fire crystals among obsidian, and the ribs of something
# that died here
# ---------------------------------------------------------------------------

def build_magma(w):
    rng = random.Random(1883)
    G = Ground(w)
    X, Y = G.X, G.Y
    flat = G.flat(0.25, 1.4)
    taken = []
    LAVA = -0.30

    # --- relief: slabs with cracks between them, and rivers ----------------
    wx = (fbm(X, Y, 3.0, 2, seed=41) - 0.5) * 1.8
    wy = (fbm(X, Y, 3.0, 2, seed=42) - 0.5) * 1.8
    f1, edge, rid, cx, cy = cells(X + wx, Y + wy, 2.3, seed=43)
    river = smooth(0.50, 0.43, fbm(X, Y, 8.0, 3, seed=44)) * smooth(1.5, 2.4, G.free)
    far = smooth(0.45, 1.1, G.free)
    slab = 0.08 + 0.55 * rid + (fbm(X, Y, 0.8, 3, seed=45) - 0.5) * 0.14 + (ridged(X, Y, 0.7, 2, seed=46) - 0.5) * 0.08
    gap = smooth(0.15, 0.04, edge)
    land = flat * slab - gap * far * 1.0
    island = rid > 0.70
    sea = np.where(island, 0.10 + 0.25 * rid - smooth(0.55, 0.30, edge) * 1.4, -1.2)
    G.Z = land * (1.0 - river) + sea * river
    shore = blur(river, 10)

    # --- paint -------------------------------------------------------------
    tone = fbm(X, Y, 4.0, 4, seed=47)
    col = lerp(C('#1a1620'), C('#383240'), tone)
    col = lerp(col, C('#575062'), rid * 0.6 + 0.2 * ridged(X, Y, 0.5, 2, seed=48))
    col *= (0.8 + 0.4 * fbm(X, Y, 0.25, 2, seed=49))[..., None]
    heat = np.exp(-np.maximum(edge, 0) / 0.11) * far
    heat = np.maximum(heat, np.exp(-np.maximum(edge, 0) / 0.20) * river)
    glow = np.clip(heat + smooth(0.08, 0.6, shore) * (1.0 - river) * 0.35, 0, 1.5)
    col = lerp(col, C('#7a1c08'), np.clip(glow, 0, 1) * 0.8)
    emit = glow[..., None] ** 1.5 * C('#ff5208') * 1.5
    verge = smooth(0.9, 0.15, G.free) * (G.free > -0.2)
    col = lerp(col, C('#1a1218'), verge * 0.5)
    col = sockets(G, col, '#6a5a60', 0.8)

    # --- volcanoes --------------------------------------------------------
    rock = B.noise_mat('magma_rock', '#1c141c', '#4a3a40', scale=3.0, rough=0.85, bump=0.8)
    stream = B.glow('lava_stream', '#ff7a14', 2.4)
    hot = B.glow('lava_hot', '#ffd850', 3.2)
    for (x, y, room) in G.spots(rng, 4, 1.5, h=3.6, gap=7.5, taken=taken, r_top=0.9, xr=(-9.2, 9.2)):
        R = min(2.2, 1.2 + room * 0.25)
        h = R * rng.uniform(1.5, 1.9)
        while h > 1.5 and not G.ok(x, y, 1.5, h, 0.9):
            h -= 0.3
        z = G.height(x, y)
        g = B.group('volcano', loc=(x, y, z - 0.25), rot=(0, 0, rng.uniform(0, 360)))
        prof = [(R, 0.0), (R * 0.80, h * 0.22), (R * 0.58, h * 0.50), (R * 0.42, h * 0.78), (R * 0.36, h),
                (R * 0.26, h * 0.97), (R * 0.20, h * 0.86), (0.02, h * 0.84)]
        o = B.lathe(prof, rock, g, segs=20)
        B.displace(o, 0.22, 0.7)
        B.cyl(R * 0.25, 0.05, hot, g, loc=(0, 0, h * 0.88), segs=16)
        for i in range(rng.randint(5, 6)):
            a0 = rng.uniform(0, 6.28)
            pts = []
            for (rr, zz) in prof[:5][::-1]:
                a0 += rng.uniform(-0.25, 0.25)
                pts.append((math.cos(a0) * (rr + 0.04), math.sin(a0) * (rr + 0.04), zz))
            pts.append((math.cos(a0) * (R + 0.5), math.sin(a0) * (R + 0.5), 0.12))
            B.tube(pts, 0.04, stream, g, r_end=0.09, segs=8)
        for i in range(7):
            B.sphere(rng.uniform(0.04, 0.09), hot, g, loc=(rng.uniform(-0.3, 0.3), rng.uniform(-0.3, 0.3),
                                                             h + rng.uniform(0.1, 1.3)), segs=8)
        lamp((x, y, z + h + 0.5), 500, '#ff7a20', 0.4)
        lamp((x, y - R - 0.4, z + 0.8), 160, '#ff6a10', 0.4)
        G.splat(emit, x, y, R * 1.3, C('#ff5208') * 0.25)

    # --- the ribs ------------------------------------------------------------
    bone = B.noise_mat('bone', '#cfc0a0', '#f4ead0', scale=4.0, rough=0.6, bump=0.3)
    for (x, y, room) in G.spots(rng, 1, 1.7, h=2.4, gap=5.0, taken=taken, xr=(-6.8, 6.8)):
        z = G.height(x, y)
        g = B.group('ribs', loc=(x, y, z - 0.1), rot=(0, 0, rng.choice((20, 160, -20, 200))))
        n = 6
        B.tube([(-1.9, 0, 0.25), (-0.6, 0, 0.42), (0.8, 0, 0.36), (1.9, 0, 0.12)], 0.17, bone, g, r_end=0.07)
        for i in range(n):
            sx = -1.5 + 2.9 * i / (n - 1)
            R = 0.95 * (1.0 - 0.45 * abs(i - 1.6) / n) * (1.0 if i < 4 else 0.8)
            for side in (-1, 1):
                pts = []
                for k in range(7):
                    ph = math.radians(150.0 * k / 6)
                    pts.append((sx + 0.12 * k / 6, side * R * math.sin(ph), 0.3 + R * (1 - math.cos(ph)) * 1.05))
                B.tube(pts, 0.085, bone, g, r_end=0.03, segs=8)
        sk = B.ico(0.5, bone, g, loc=(-2.35, 0, 0.42), scale=(1.25, 0.85, 0.75), subdiv=2, smooth=True)
        B.displace(sk, 0.08, 0.5)
        for side in (-1, 1):
            B.tube([(-2.3, side * 0.3, 0.6), (-2.5, side * 0.75, 1.0), (-2.9, side * 0.85, 1.45)], 0.11, bone, g,
                   r_end=0.02, segs=8)
            B.sphere(0.1, B.glow('socket', '#ff8a20', 2.6), g, loc=(-2.72, side * 0.24, 0.52), segs=10)

    # --- obsidian, and the fire crystals that grow at its feet ---------------
    obs = B.mat('obsidian', '#1a1028', rough=0.07, clear=1.0, spec=1.0, emit='#5a1408', emit_k=0.5)
    fire = crystal_mat('fire_crystal', '#e02408', '#ffe060', k=2.0, top=1.2)
    ho, hf = Heap(), Heap()
    for (x, y, room) in G.spots(rng, 9, 0.6, h=3.4, gap=4.0, taken=taken):
        size = rng.uniform(2.0, 3.6)
        while size > 1.0 and not G.ok(x, y, 0.6, size):
            size -= 0.3
        z = G.height(x, y)
        crystal_cluster(ho, rng, x, y, z, size, lean=20.0, segs=5)
        for i in range(rng.randint(3, 5)):
            a = rng.uniform(0, 6.28)
            d = size * rng.uniform(0.18, 0.3)
            crystal_cluster(hf, rng, x + math.cos(a) * d, y + math.sin(a) * d, z, size * rng.uniform(0.2, 0.36),
                            n=rng.randint(2, 3))
        lamp((x, y - 0.3, z + 0.6), 150, '#ff8020', 0.3)
        G.splat(emit, x, y, 1.1, C('#ff5a10') * 0.22)
    for (x, y, r) in G.sprinkle(rng, 60, lambda g_: g_.uniform(0.12, 0.3), lambda r: r * 3.0, taken):
        z = G.height(x, y)
        if z < LAVA + 0.1:
            continue
        if rng.random() < 0.5:
            crystal_cluster(hf, rng, x, y, z, r * 3.0, n=rng.randint(2, 4))
            G.splat(emit, x, y, 0.6, C('#ff5a10') * 0.22)
        else:
            crystal_cluster(ho, rng, x, y, z, r * 3.6, n=rng.randint(2, 3), lean=20.0, segs=5)
    ho.done('obsidian', obs)
    hf.done('fire', fire)

    # --- boulders; bubbles on the lava; embers in the air ----------------------
    hp = Heap()
    for (x, y, r) in G.sprinkle(rng, 150, lambda g_: 0.07 + 0.5 * g_.random() ** 2.5, lambda r: r * 1.5, taken):
        z = G.height(x, y)
        if z < LAVA + 0.05:
            continue
        hp.rock(r, (x, y, z + r * 0.3), rot=(rng.uniform(-15, 15), rng.uniform(-15, 15), rng.uniform(0, 360)),
                scale=(rng.uniform(0.8, 1.2), rng.uniform(0.8, 1.2), rng.uniform(0.6, 0.9)), subdiv=2 if r > 0.25 else 1,
                seed=rng.uniform(0, 50))
    hp.done('boulders', rock)
    bub, emb = Heap(), Heap()
    for _ in range(900):
        x, y = rng.uniform(-9.8, 9.8), rng.uniform(0, 40.5)
        i, j = G._ij(x, y)
        if G.Z[i, j] < LAVA - 0.2 and river[i, j] > 0.9:
            if rng.random() < 0.25:
                bub.rock(rng.uniform(0.10, 0.32), (x, y, LAVA - 0.02), scale=(1, 1, 0.5), subdiv=2, lump=0.0)
            if rng.random() < 0.7:
                emb.rock(rng.uniform(0.025, 0.05), (x, y, LAVA + rng.uniform(0.2, 1.6)), subdiv=1, lump=0.0)
    bub.done('bubbles', hot, smooth=True)
    emb.done('embers', B.glow('ember', '#ffb030', 4.0))

    hud_calm(G, col, emit, dim=0.2, hush=0.4)
    G.build(ground_mat('magma_ground', rough=0.6, spec=0.4, grain=(16.0, 0.18), bump=(0.5, 0.05), lumps=(3.0, 0.35),
                       emit=True), dict(col=col, emit=emit))

    # --- the lava: one sheet under everything ---------------------------------
    M = Mat('lava')
    flow = M.noise(0.32, detail=4.0, rough=0.55, distortion=2.4)
    fine = M.noise(1.6, detail=3.0, distortion=1.0)
    heatv = M.math('ADD', M.math('MULTIPLY', flow, 0.8), M.math('MULTIPLY', fine, 0.2))
    lava = M.ramp(heatv, [(0.30, '#6a0e02'), (0.42, '#d83006'), (0.52, '#ff6a0e'), (0.64, '#ff9a1c'), (0.78, '#ffd050')])
    M.set(Base_Color='#200804', Roughness=0.4, Emission=lava,
          Emission_Strength=M.math('MULTIPLY', M.remap(heatv, 0.3, 0.8, 1.0, 2.1), M.hud_dim(0.35)))
    B.mesh([(-11, -4, LAVA), (11, -4, LAVA), (11, 45, LAVA), (-11, 45, LAVA)], [(0, 1, 2, 3)], M.m, name='lava')

    # --- the road: slabs of grey stone edged with a line of fire --------------
    M = Mat('magma_road')
    u, v = road_uv_sockets(M)
    n = M.noise(8.0, detail=5.0)
    base = M.ramp(n, [(0.3, '#69666e'), (0.7, '#8f8b94')])
    seam = M.remap(M.math('ABSOLUTE', M.math('SUBTRACT', M.math('FRACT', M.math('MULTIPLY', v, 0.7)), 0.5)),
                   0.0, 0.035, 1.0, 0.0)
    mid = M.remap(u, 0.0, 0.036, 1.0, 0.0)
    line = M.math('MAXIMUM', seam, mid)
    M.set(Base_Color=M.mix(M.math('MULTIPLY', line, 0.75), base, '#2a1c1c'), Roughness=0.7, Specular=0.3,
          Normal=M.bump(M.math('SUBTRACT', n, line), 0.4, 0.03))
    road(G, M.m)
    kerbs(w, B.glow('magma_kerb', '#ff7a1a', 1.7), width=0.10, height=0.04)
    plate = B.mat('magma_plate', '#3c2e34', rough=0.4, metal=0.7)
    K.pads(w, plate, rim_k=4.0, inner_mat=B.mat('magma_plate_in', '#241a20', rough=0.5, metal=0.6))
    K.foundation(w, plate)

    # --- light: a pale cold key so the rock has form, fire everywhere else ---
    B.sun(205, 48, 2.7, (0.78, 0.84, 1.0), angle=6.0, name='key')
    B.sun(40, 26, 0.8, (1.0, 0.42, 0.12), angle=25, name='rim')
    B.ambient((0.50, 0.26, 0.24), 0.40)


# ---------------------------------------------------------------------------
# hielo: a glacier in full daylight. Snow drifts, frozen lakes like glass,
# crevasses with light deep inside, spires and slabs of ice, frost trees,
# and vents that breathe a pink glow
# ---------------------------------------------------------------------------

def build_hielo(w):
    rng = random.Random(2077)
    G = Ground(w)
    X, Y = G.X, G.Y
    flat = G.flat(0.25, 1.8)
    taken = []

    # --- relief: drifts blown across, lakes, crevasses -------------------
    drift = ridged(X * 0.45 + Y * 0.2, Y, 2.6, 3, seed=51)
    Z = (fbm(X, Y, 7.0, 4, seed=52) - 0.42) * 1.3 + drift * 0.40 + (fbm(X, Y, 0.9, 3, seed=53) - 0.5) * 0.10
    lake = smooth(0.40, 0.34, fbm(X, Y, 6.0, 3, seed=54)) * smooth(0.9, 1.8, G.free)
    wx = (fbm(X, Y, 4.0, 3, seed=55) - 0.5) * 3.0
    wy = (fbm(X, Y, 4.0, 3, seed=56) - 0.5) * 3.0
    f1, edge, rid, cx, cy = cells(X + wx, Y + wy, 7.5, seed=57)
    crev = smooth(0.20, 0.05, edge) * smooth(0.5, 1.2, G.free) * (1.0 - lake)
    G.Z = Z * flat * (1.0 - lake) - lake * 0.06 - crev * 1.8
    hgt = np.clip(G.Z / 1.2, 0, 1)

    # --- paint -------------------------------------------------------------
    tone = fbm(X, Y, 5.0, 4, seed=58)
    col = lerp(C('#9dbfe4'), C('#d4e3f4'), np.clip(tone * 0.6 + hgt * 0.6 + drift * 0.25, 0, 1))
    col *= (0.93 + 0.14 * fbm(X, Y, 0.3, 2, seed=59))[..., None]
    # the sky's colours lie on the snow in wide soft bands: lilac and mint
    col = lerp(col, C('#c9b4f2'), smooth(0.52, 0.75, fbm(X, Y, 9.0, 2, seed=63)) * 0.45)
    col = lerp(col, C('#a8ecdc'), smooth(0.55, 0.78, fbm(X, Y, 9.0, 2, seed=64)) * 0.40)
    ice = lerp(C('#2f8fd0'), C('#7fd4f2'), fbm(X, Y, 1.6, 4, seed=60))
    f1b, edgeb, ridb, _, _ = cells(X + wx * 0.3, Y + wy * 0.3, 1.3, seed=61)
    crack = smooth(0.035, 0.008, edgeb)
    ice = lerp(ice, C('#e8f8ff'), crack * 0.85)
    col = lerp(col, ice, lake)
    col = lerp(col, C('#1478d8'), np.clip(crev * 1.2, 0, 1))
    verge = smooth(0.8, 0.15, G.free) * (G.free > -0.2)
    col = lerp(col, C('#88a8cc'), verge * 0.7)
    emit = (crev ** 2)[..., None] * C('#28d0ff') * 2.2
    emit += lake[..., None] * C('#0a70b8') * 0.10
    f1c, _, ridc, _, _ = cells(X, Y, 0.6, seed=62)
    spark = smooth(0.05, 0.02, f1c) * (ridc > 0.6) * (1.0 - lake) * (G.free > 0.3)
    emit += spark[..., None] * C('#ffffff') * 0.8
    aux = np.zeros(col.shape, np.float32)
    aux[..., 0] = 0.62 - 0.56 * lake         # roughness: snow, then glass
    aux[..., 2] = 1.0 - 0.9 * lake           # the snow's grain does not dent the lakes

    # --- spires of ice: tall, thin, in stands ------------------------------
    icy = crystal_mat('hielo_ice', '#2a8ae8', '#f0fcff', k=0.85, top=3.2, rough=0.05)
    hs = Heap()
    for (x, y, room) in G.spots(rng, 10, 0.8, h=4.6, gap=4.6, taken=taken, xr=(-9.3, 9.3)):
        size = rng.uniform(3.0, 5.2)
        while size > 1.2 and not G.ok(x, y, 0.8, size):
            size -= 0.3
        z = G.height(x, y)
        for i in range(rng.randint(5, 8)):
            a, d = rng.uniform(0, 6.28), rng.uniform(0, 0.7) * (i > 0)
            hh = size * (1.0 if i == 0 else rng.uniform(0.3, 0.75))
            hs.shard(hh * rng.uniform(0.07, 0.11) + 0.05, hh, (x + math.cos(a) * d, y + math.sin(a) * d, z - 0.1),
                     rot=(rng.uniform(-9, 9), rng.uniform(-9, 9), rng.uniform(0, 90)), segs=rng.choice((4, 5)),
                     waist=rng.uniform(0.25, 0.5), tip=1.25)
        lamp((x, y - 0.4, z + size * 0.4), 60 * size, '#60d8ff', 0.4)
    for (x, y, r) in G.sprinkle(rng, 70, lambda g_: g_.uniform(0.12, 0.3), lambda r: r * 3.4, taken):
        z = G.height(x, y)
        for i in range(rng.randint(2, 4)):
            a, d = rng.uniform(0, 6.28), rng.uniform(0, r)
            hh = r * rng.uniform(1.6, 3.4)
            hs.shard(hh * 0.12 + 0.03, hh, (x + math.cos(a) * d, y + math.sin(a) * d, z - 0.05),
                     rot=(rng.uniform(-14, 14), rng.uniform(-14, 14), rng.uniform(0, 90)), segs=rng.choice((4, 5)),
                     waist=0.4, tip=1.2)
    hs.done('spires', icy)

    # --- frost trees: tiers of teal under caps of snow -----------------------
    needles = B.mat('needles', '#149a9a', rough=0.5, clear=0.3)
    snowcap = B.mat('snowcap', '#f4f9ff', rough=0.5, emit='#a8d8ff', emit_k=0.12)
    trunk = B.mat('trunk', '#3a3050', rough=0.8)
    ht, hc, hk = Heap(), Heap(), Heap()
    for (x, y, r) in G.sprinkle(rng, 34, lambda g_: g_.uniform(0.5, 0.8), lambda r: r * 3.6, taken, own_gap=0.8):
        z = G.height(x, y)
        if lake[G._ij(x, y)] > 0.3:
            continue
        h = r * rng.uniform(3.0, 3.6)
        hk.cone(0.09, 0.07, h * 0.25, (x, y, z - 0.05), segs=6)
        for i in range(3):
            zz = z + h * (0.18 + 0.25 * i)
            rr = r * (1.0 - 0.27 * i)
            hh = h * 0.36
            ht.cone(rr, rr * 0.08, hh, (x, y, zz), segs=8)
            hc.cone(rr * (1.0 - 0.92 * 0.42) + 0.035, 0.0, hh * 0.60 + 0.03, (x, y, zz + hh * 0.42), segs=8)
    ht.done('needles', needles)
    hc.done('snowcaps', snowcap)
    hk.done('trunks', trunk)

    # --- vents: low cones of ice with a pink breath -----------------------------
    pink = B.glow('vent_pink', '#ff60c8', 3.0)
    ventm = B.noise_mat('vent_ice', '#9ac8f0', '#eef8ff', scale=4.0, rough=0.4, bump=0.4)
    for (x, y, room) in G.spots(rng, 6, 1.0, h=1.5, gap=5.0, taken=taken):
        z = G.height(x, y)
        g = B.group('vent', loc=(x, y, z - 0.1), scale=(1.5, 1.5, 1.5))
        B.lathe([(0.75, 0.0), (0.5, 0.35), (0.34, 0.62), (0.27, 0.70), (0.2, 0.62), (0.02, 0.5)], ventm, g, segs=12)
        B.cyl(0.21, 0.04, pink, g, loc=(0, 0, 0.58), segs=12)
        for i in range(5):
            B.sphere(0.09 - 0.012 * i, pink, g, loc=(rng.uniform(-0.1, 0.1), rng.uniform(-0.1, 0.1), 0.85 + 0.22 * i), segs=8)
        lamp((x, y, z + 1.6), 260, '#ff60c8', 0.3)
        G.splat(emit, x, y, 1.3, C('#ff50c0') * 0.20)

    # --- slabs of broken ice, and dark stones under snow ---------------------
    slab = B.mat('serac', '#8fd0f4', rough=0.08, clear=1.0, spec=0.9, emit='#3aa8f0', emit_k=0.35)
    for (x, y, r) in G.sprinkle(rng, 26, lambda g_: g_.uniform(0.35, 0.6), lambda r: r * 2.2, taken):
        z = G.height(x, y)
        for i in range(rng.randint(2, 3)):
            sz = r * rng.uniform(1.2, 2.2)
            B.box((sz * 0.9, sz * 0.22, sz), slab, loc=(x + rng.uniform(-r, r) * 0.5, y + rng.uniform(-r, r) * 0.5, z + sz * 0.3),
                  rot=(rng.uniform(-30, 30), rng.uniform(-30, 30), rng.uniform(0, 180)), bevel=0.04)
    M = Mat('snow_rock')
    g = M.node('NewGeometry')
    up = M.remap(M.xyz(g.outputs['Normal'])[2], 0.35, 0.6, 0.0, 1.0, smoothstep=True)
    M.set(Base_Color=M.mix(up, M.ramp(M.noise(4.0), [(0.3, '#2c3a58'), (0.7, '#5a6c90')]), '#f2f8ff'), Roughness=0.7)
    hp = Heap()
    rocks(G, rng, hp, 110, 0.08, 0.6, taken)
    hp.done('rocks', M.m, smooth=False)

    hud_calm(G, col, emit, dim=0.12, hush=0.3)
    G.build(ground_mat('hielo_ground', spec=0.5, grain=(26.0, 0.05), bump=(0.25, 0.03), lumps=(5.0, 0.2), aux=True,
                       emit=True), dict(col=col, emit=emit, aux=aux))

    # --- the road: ice scraped to a blue-grey, darker than the snow ----------
    M = Mat('hielo_road')
    u, v = road_uv_sockets(M)
    streak = M.noise(1.0, detail=4.0, vec=M.node('CombineXYZ', X=M.math('MULTIPLY', u, 4.5),
                                                 Y=M.math('MULTIPLY', v, 0.35)).outputs[0])
    n = M.noise(7.0, detail=4.0)
    base = M.ramp(M.math('ADD', M.math('MULTIPLY', streak, 0.6), M.math('MULTIPLY', n, 0.4)),
                  [(0.3, '#6484ac'), (0.7, '#8fabc8')])
    rim = M.remap(u, 0.72, 1.0, 0.0, 0.5, smoothstep=True)
    M.set(Base_Color=M.mix(rim, base, '#dcecfa'), Roughness=0.35, Specular=0.5, Normal=M.bump(streak, 0.2, 0.02))
    road(G, M.m)
    kerbs(w, B.mat('hielo_kerb', '#34588e', rough=0.4, metal=0.3), width=0.14, height=0.06)
    plate = B.mat('hielo_plate', '#2e4870', rough=0.35, metal=0.7)
    K.pads(w, plate, rim_k=4.0, inner_mat=B.mat('hielo_plate_in', '#1c2e4c', rough=0.5, metal=0.6))
    K.foundation(w, plate)

    # --- light: a clear cold day, the sky in the shadows, pink from behind ---
    B.sun(205, 40, 2.9, (1.0, 0.95, 0.86), angle=4.0, name='key')
    B.sun(40, 26, 0.9, (1.0, 0.45, 0.80), angle=25, name='rim')
    B.sun(300, 35, 0.4, (0.5, 0.8, 1.0), angle=40, name='fill')
    B.ambient((0.45, 0.66, 1.0), 0.55)


# ---------------------------------------------------------------------------
# orbita: a station's deck in the void. Plates of blue-grey metal with lit
# seams, hazard stripes at every edge, pipes beside the corridors, solar
# wings and glowing radiators out over nothing; beyond the edges the
# stars, and the limb of an amber planet turning below
# ---------------------------------------------------------------------------

def space_mat(planet=(-6.0, -31.0, 27.5)):
    """the void behind a world that floats: stars of two sizes, a faint
    nebula, and a banded planet as a disc (x, y, radius in the backdrop's
    metres) lit from the upper left, with a thin shining atmosphere"""
    M = Mat('space', emission_only=True)
    p = M.coord()
    neb = M.noise(0.11, detail=5.0, distortion=1.2)
    neb2 = M.noise(0.07, detail=3.0, vec=M.scaled(p, (1.0, 1.0, 1.0)))
    sky = M.ramp(neb, [(0.35, '#02030a'), (0.55, '#0c0a30'), (0.70, '#261050'), (0.85, '#0a3a5a')])
    sky = M.mix(M.remap(neb2, 0.4, 0.7), sky, '#02030a')
    stars = None
    for (scale, thr, k) in ((2.3, 0.050, 3.5), (5.5, 0.060, 1.2)):
        v = M.node('TexVoronoi', dict(feature='F1'), Vector=p, Scale=scale)
        rnd = M.xyz(v.outputs['Color'])
        size = M.math('MULTIPLY', M.math('POWER', rnd[0], 3.0), thr)
        dot = M.remap(M.math('DIVIDE', v.outputs['Distance'], M.math('ADD', size, 0.004)), 0.6, 1.0, 1.0, 0.0,
                      smoothstep=True)
        tint = M.ramp(rnd[1], [(0.0, '#9ab8ff'), (0.5, '#ffffff'), (1.0, '#ffd8a0')])
        st = M.mix(1.0, tint, M.math('MULTIPLY', dot, k), 'MULTIPLY')
        stars = st if stars is None else M.mix(1.0, stars, st, 'ADD')
    space = M.mix(1.0, sky, stars, 'ADD')
    cx, cy, R = planet
    x, y, _ = M.xyz(p)
    dx, dy = M.math('SUBTRACT', x, cx), M.math('SUBTRACT', y, cy)
    d = M.math('SQRT', M.math('ADD', M.math('MULTIPLY', dx, dx), M.math('MULTIPLY', dy, dy)))
    t = M.math('DIVIDE', d, R)
    nz = M.math('SQRT', M.math('MAXIMUM', M.math('SUBTRACT', 1.0, M.math('MULTIPLY', t, t)), 0.0))
    L = Vector((-0.45, 0.55, 0.70)).normalized()
    lam = M.math('ADD', M.math('ADD', M.math('MULTIPLY', dx, L.x / R), M.math('MULTIPLY', dy, L.y / R)),
                 M.math('MULTIPLY', nz, L.z))
    lam = M.math('POWER', M.math('MAXIMUM', lam, 0.0), 0.8)
    swirl = M.noise(0.22, detail=6.0, distortion=2.2, vec=M.scaled(p, (0.35, 1.0, 1.0)))
    band = M.math('ADD', M.math('MULTIPLY', M.math('ADD', M.math('MULTIPLY', dx, 0.34), M.math('MULTIPLY', dy, 0.94)),
                                4.2 / R), M.math('MULTIPLY', swirl, 0.9))
    surf = M.ramp(M.math('FRACT', M.math('MULTIPLY', band, 0.5)),
                  [(0.00, '#ffb060'), (0.16, '#fff0c8'), (0.30, '#e8703a'), (0.46, '#ffd088'), (0.60, '#c8486a'),
                   (0.74, '#ffc878'), (0.88, '#f08a40'), (1.00, '#ffb060')])
    lit = M.mix(1.0, surf, M.math('ADD', M.math('MULTIPLY', lam, 1.05), 0.05), 'MULTIPLY')
    rim = M.math('POWER', M.math('SUBTRACT', 1.0, nz), 4.0)
    lit = M.mix(M.math('MULTIPLY', rim, M.math('ADD', lam, 0.15), clamp=True), lit, '#9fe0ff')
    on = M.remap(d, R - 0.06, R, 1.0, 0.0)
    halo = M.math('MULTIPLY', M.math('POWER', 2.718, M.math('DIVIDE', M.math('SUBTRACT', R, d), 0.30)), 0.8)
    halo = M.math('MULTIPLY', M.math('MINIMUM', halo, 0.8), M.math('SUBTRACT', 1.0, on))
    out = M.mix(on, space, lit)
    out = M.mix(halo, out, '#70c8ff', 'ADD')
    M.put(M.b.inputs['Color'], out)
    M.put(M.b.inputs['Strength'], M.camera_only(1.0))
    return M.m


def build_orbita(w):
    rng = random.Random(2001)
    G = Ground(w)
    X, Y = G.X, G.Y
    taken = []
    WALL = 0.9

    # --- the deck's outline: all the game needs and a margin, modules on the
    # roomiest free places, each tied to a corridor by a gangway -------------
    deck = G.free < 1.25
    pts = [q for p in w.paths for q in p]
    mods = []
    for i, (x, y, room) in enumerate(G.spots(rng, 6, 1.4, h=0.6, gap=5.5, xr=(-7.6, 7.6),
                                             yr=(HUD_BOT_Y + 0.8, HUD_TOP_Y - 2.0))):
        r = max(1.7, min(2.5, room - 0.2))
        square = i % 2 == 1
        mods.append((x, y, r, square))
        deck |= ((np.abs(X - x) < r) & (np.abs(Y - y) < r)) if square else (np.hypot(X - x, Y - y) < r)
        qx, qy = min(pts, key=lambda q: math.hypot(q[0] - x, q[1] - y))
        dx, dy = qx - x, qy - y
        t = np.clip(((X - x) * dx + (Y - y) * dy) / (dx * dx + dy * dy), 0, 1)
        deck |= np.hypot(X - (x + t * dx), Y - (y + t * dy)) < 0.65
    k = int(round(0.5 / G.step))
    deck = ~spread(~spread(deck, k), k)                 # no slit narrower than a metre
    f = blur(deck.astype(np.float32), 3)
    inside = f >= 0.5
    XY, ring = G.contour(f, 0.5, inside, rings=2)
    lip = grow(inside) & ~inside
    Zd = np.where(ring & ~lip, -WALL, 0.0).astype(np.float32)
    room = np.zeros(X.shape, np.float32)                # metres from each deck vertex to the void
    cur = inside.copy()
    for _ in range(12):
        cur = ~spread(~cur, 4)
        room += cur * (4 * G.step)
    G.room = room
    G.free = np.where(inside, G.free, -1.0)

    # --- paint: plates, wear, stripes at the edge, sockets, the modules ------
    h = np.sin(np.floor(X / 2.0) * 127.1 + np.floor(Y / 2.0) * 311.7) * 43758.5453
    tint = h - np.floor(h)
    col = lerp(C('#36415c'), C('#53607e'), tint)
    col *= (0.90 + 0.20 * fbm(X, Y, 2.5, 3, seed=71))[..., None]
    col = lerp(col, C('#232a3c'), smooth(0.45, 0.2, G.free) * (G.free > -0.3))
    col = lerp(col, C('#6d7b9a'), smooth(1.22, 1.16, G.d_pad) * smooth(0.90, 0.96, G.d_pad))
    stripes = ((X + Y) * 2.0) % 1.0 < 0.5
    band = (room < 0.28) & inside
    col = np.where(band[..., None], np.where(stripes[..., None], C('#ffc21a'), C('#171b28')), col)
    emit = np.zeros(col.shape, np.float32)
    aux = np.zeros(col.shape, np.float32)
    aux[..., 0] = 0.42 + 0.2 * tint
    aux[..., 1] = 0.55
    aux[..., 2] = 1.0
    for i, (x, y, r, square) in enumerate(mods):
        sl = G.window(x, y, r + 0.3)
        d = np.maximum(np.abs(X[sl] - x), np.abs(Y[sl] - y)) if square else np.hypot(X[sl] - x, Y[sl] - y)
        floor = (('#2a3146', '#ff9a1e'), ('#4a5876', '#40d8ff'), ('#2c3850', '#ffffff'), ('#40506e', '#ff5a3c'))[i % 4]
        c = col[sl]
        c[:] = np.where((d < r - 0.3)[..., None], lerp(c, C(floor[0]), 0.8), c)
        line = (np.abs(d - (r - 0.55)) < 0.07)
        c[:] = np.where(line[..., None], C(floor[1]), c)
        c[:] = np.where(line[..., None], C(floor[1]) * 0.8, c)
    # dashes of light in the floor beside the corridors
    dash = (np.abs(G.free - 0.42) < 0.045) & ((G.s_road % 1.2) < 0.6) & (G.d_road - HALF < 0.6) & (G.d_pad > 1.3)
    emit += dash[..., None] * C('#6aa0ff') * 2.6
    col = np.where((ring & ~lip)[..., None], C('#141826'), col)

    def tweak(M, s_):
        brick = M.node('TexBrick', dict(offset=0.5), Vector=M.coord(), Color1=(1, 1, 1, 1), Color2=(0.84, 0.86, 0.9, 1),
                       Mortar=(0.22, 0.24, 0.3, 1), Scale=1.0, Mortar_Size=0.012, Brick_Width=1.0, Row_Height=1.0)
        base = M.mix(1.0, s_['base'], brick.outputs['Color'], 'MULTIPLY')
        x, y, z = M.xyz(M.node('NewGeometry').outputs['Position'])
        wall = M.math('LESS_THAN', z, -0.04)
        rowz = M.math('MULTIPLY', M.math('GREATER_THAN', z, -0.56), M.math('LESS_THAN', z, -0.30))
        seg = M.math('LESS_THAN', M.math('FRACT', M.math('MULTIPLY', M.math('ADD', x, M.math('MULTIPLY', y, 0.6)), 0.8)),
                     0.62)
        win = M.math('MULTIPLY', M.math('MULTIPLY', wall, rowz), seg)
        M.set(Base_Color=M.mix(wall, base, '#1a2032'),
              Emission=M.mix(win, M.attr('emit'), B.lin('#ffcf78')),
              Emission_Strength=M.math('ADD', 1.0, M.math('MULTIPLY', win, 1.4)),
              Normal=M.bump(M.math('SUBTRACT', M.math('MULTIPLY', s_['grain'], 0.3), brick.outputs['Fac']), 0.35, 0.02),
              Clearcoat=0.3)
    G.build(ground_mat('orbita_deck', spec=0.5, grain=(30.0, 0.05), bump=(0.3, 0.02), aux=True, emit=True, tweak=tweak),
            dict(col=col, emit=emit, aux=aux), name='deck', keep=inside | ring, XY=XY, Z=Zd)

    backdrop(space_mat())

    # --- materials of the station's things -----------------------------------
    white = B.mat('st_white', '#e6eaf2', rough=0.4, clear=0.4)
    dark = B.mat('st_dark', '#232838', rough=0.45, metal=0.7)
    steel = B.mat('st_steel', '#9aa6be', rough=0.3, metal=1.0)
    paints = [B.mat('st_p%d' % i, c, rough=0.35, clear=0.6) for i, c in
              enumerate(('#ff8a1e', '#ffd23a', '#22c8b8', '#ff4a3c', '#4a7cff', '#e6eaf2'))]
    orange, yellow, teal, red, blue = paints[:5]
    gold = B.mat('st_gold', '#e8b040', rough=0.3, metal=1.0)
    cyan = B.glow('st_cyan', '#60d8ff', 2.6)
    warm = B.glow('st_warm', '#ffc060', 2.6)
    redl = B.glow('st_red', '#ff3028', 3.5)
    greenl = B.glow('st_green', '#40ff80', 3.0)
    P = Mat('st_solar')
    br = P.node('TexBrick', dict(offset=0.0), Vector=P.coord(), Color1=B.lin('#12309a'), Color2=B.lin('#1c4ad0'),
                Mortar=B.lin('#c8d4f0'), Scale=4.0, Mortar_Size=0.02, Brick_Width=0.5, Row_Height=0.5)
    P.set(Base_Color=br.outputs['Color'], Roughness=0.15, Metallic=0.5, Specular=0.9, Clearcoat=1.0,
          Emission=br.outputs['Color'], Emission_Strength=0.5)
    Rm = Mat('st_radiator')
    rx = Rm.xyz(Rm.coord())[0]
    rc = Rm.ramp(Rm.remap(rx, 0.0, 3.2), [(0.0, '#5a1408'), (0.5, '#ff5a14'), (1.0, '#ffb040')])
    Rm.set(Base_Color='#30120c', Roughness=0.4, Emission=rc, Emission_Strength=2.0)

    # --- what stands on the modules ---------------------------------------
    def dish(g):
        for a in (30, 150, 270):
            ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
            B.tube([(0.7 * ca, 0.7 * sa, 0.0), (0.12 * ca, 0.12 * sa, 3.0)], 0.05, steel, g)
        for zz in (0.8, 1.6, 2.4):
            B.torus(0.7 - 0.19 * zz, 0.03, steel, g, loc=(0, 0, zz), segs=12)
        B.cyl(0.16, 0.4, dark, g, loc=(0, 0, 2.9), segs=12)
        t = B.group('tilt', g, loc=(0, 0, 3.4), rot=(40, 0, 25))
        B.lathe([(0.05, 0.0), (0.7, 0.12), (1.15, 0.42), (1.2, 0.42), (0.7, 0.07)], white, t, segs=28)
        B.tube([(0, 0, 0.05), (0, 0, 0.95)], 0.035, steel, t)
        B.sphere(0.1, redl, t, loc=(0, 0, 1.0))
        B.torus(1.17, 0.035, orange, t, loc=(0, 0, 0.42), segs=32)
        return 4.6

    def reactor(g):
        B.cyl(1.05, 0.3, dark, g, segs=6, smooth=False)
        B.cyl(0.52, 2.0, cyan, g, loc=(0, 0, 0.3), segs=20)
        for a in range(0, 360, 60):
            ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
            B.box((0.16, 0.16, 2.2), dark, g, loc=(0.72 * ca, 0.72 * sa, 1.35), rot=(0, 0, a))
        for zz in (0.75, 1.3, 1.85):
            B.torus(0.74, 0.07, steel, g, loc=(0, 0, zz), segs=6, tsegs=8)
        B.lathe([(0.9, 2.4), (0.8, 2.6), (0.4, 2.8), (0.0, 2.84)], blue, g, segs=6, smooth=False)
        B.sphere(0.12, cyan, g, loc=(0, 0, 2.92))
        return 3.0

    def shuttle(g):
        B.sphere(1.0, white, g, loc=(0, 0, 0.62), scale=(1.5, 0.52, 0.42))
        B.sphere(0.5, B.mat('st_glass', '#0a1838', rough=0.05, clear=1.0, emit='#3060ff', emit_k=0.5), g,
                 loc=(0.75, 0, 0.78), scale=(1.1, 0.62, 0.5))
        for sy in (-1, 1):
            B.prism([(-1.2, 0.0), (0.5, 0.0), (-0.6, sy * 1.25), (-1.3, sy * 1.25)][::sy], 0.50, 0.58, orange, g)
            B.cyl(0.2, 0.9, dark, g, loc=(-1.6, sy * 0.42, 0.62), rot=(0, 90, 0), segs=16)
            B.cyl(0.15, 0.03, cyan, g, loc=(-1.63, sy * 0.42, 0.62), rot=(0, 90, 0), segs=16)
            B.tube([(-0.5, sy * 0.3, 0.3), (-0.6, sy * 0.4, 0.04), (0.6, sy * 0.4, 0.04), (0.5, sy * 0.3, 0.3)], 0.035,
                   steel, g)
        B.prism([(-1.4, 0.0), (-0.6, 0.0), (-1.3, 0.75), (-1.6, 0.75)], -0.04, 0.04, orange, g, loc=(0, 0, 0.9),
                rot=(90, 0, 0))
        return 1.7

    def tanks(g):
        for i, m in enumerate((orange, white, teal)):
            x = (i - 1) * 1.15
            B.lathe([(0.0, 0.1), (0.42, 0.18), (0.52, 0.4), (0.52, 1.7), (0.42, 1.95), (0.0, 2.05)], m, g, loc=(x, 0, 0),
                    segs=20)
            B.cyl(0.535, 0.12, dark, g, loc=(x, 0, 0.6), segs=20)
            B.cyl(0.535, 0.12, dark, g, loc=(x, 0, 1.45), segs=20)
            B.sphere(0.07, warm, g, loc=(x, -0.5, 1.05))
        B.tube([(-1.15, -0.2, 2.02), (-1.15, -0.2, 2.3), (1.15, -0.2, 2.3), (1.15, -0.2, 2.02)], 0.06, yellow, g)
        return 2.4

    def dome(g):
        B.cyl(1.35, 0.25, dark, g, segs=24)
        B.cyl(1.2, 0.06, B.mat('st_soil', '#3a2a20', rough=0.9), g, loc=(0, 0, 0.25), segs=24)
        leaf = B.mat('st_leaf', '#30c060', rough=0.5, emit='#30ff70', emit_k=0.35)
        for i in range(9):
            a, d = rng.uniform(0, 6.28), rng.uniform(0.1, 0.85)
            B.ico(rng.uniform(0.22, 0.4), leaf, g, loc=(math.cos(a) * d, math.sin(a) * d, 0.5 + rng.uniform(0, 0.25)),
                  scale=(1, 1, 1.2), subdiv=2, smooth=True)
        B.sphere(0.1, warm, g, loc=(0, 0, 1.15))
        for a in range(0, 180, 30):
            B.torus(1.25, 0.035, steel, g, loc=(0, 0, 0.25), rot=(90, 0, a), segs=32, arc=180)
        B.torus(1.25, 0.05, steel, g, loc=(0, 0, 0.27), segs=32)
        B.torus(0.88, 0.03, steel, g, loc=(0, 0, 1.14), segs=24)
        return 1.6

    def cargo(g):
        for i in range(7):
            m = rng.choice(paints)
            lay = i // 4
            x, y = ((i % 4) % 2 - 0.5) * 1.5 + rng.uniform(-0.1, 0.1), ((i % 4) // 2 - 0.5) * 0.85
            B.box((1.3, 0.62, 0.58), m, g, loc=(x, y, 0.3 + lay * 0.6), rot=(0, 0, rng.uniform(-4, 4)), bevel=0.03)
            B.box((1.32, 0.08, 0.6), dark, g, loc=(x, y, 0.3 + lay * 0.6), bevel=0.0)
        return 1.3

    # the tallest thing goes to the first module where it hides nothing; a
    # module nothing fits gets cargo
    left = list(mods)
    for kind, hgt in ((dish, 4.6), (reactor, 3.0), (tanks, 2.4), (shuttle, 1.7), (dome, 1.6), (cargo, 1.3)):
        for m in left:
            if kind is cargo or G.ok(m[0], m[1], 1.2, hgt):
                left.remove(m)
                kind(B.group('module', loc=(m[0], m[1], 0.0), rot=(0, 0, 0 if m[3] else rng.choice((0, 30, -30, 90)))))
                lamp((m[0], m[1] - 1.0, 1.6), 120, '#a0c8ff', 0.4)
                break
    for m in left:
        cargo(B.group('module', loc=(m[0], m[1], 0.0)))
    taken.extend((x, y, r) for (x, y, r, _) in mods)

    # --- pipes beside the corridors, with a riser where each run ends --------
    def on_deck(x, y):
        i, j = G._ij(x, y)
        return room[i, j] > 0.2 and G.d_pad[i, j] > bsworlds.PAD_R + 0.3 and G.d_fort[i, j] > bsworlds.FORT_R + 0.3 \
            and not any(math.hypot(x - mx, y - my) < mr for (mx, my, mr, _) in mods)
    for off, rad, m in ((1.66, 0.085, orange), (1.86, 0.06, teal)):
        for run in offset_runs(w, off, on_deck):
            zz = rad + 0.07
            B.tube([(x, y, zz) for (x, y) in run], rad, m, segs=8)
            for (x, y) in (run[0], run[-1]):
                B.cyl(rad * 1.5, zz + 0.04, dark, loc=(x, y, 0.0), segs=10)
            for (x, y) in run[4::8]:
                B.box((rad * 3.2, rad * 3.2, zz + rad), dark, loc=(x, y, (zz + rad) / 2), bevel=0.01)

    # --- wings out over the void: solar panels and radiators -----------------
    gy, gx = np.gradient(f, G.step)
    ii, jj = np.nonzero(lip[::4, ::4])
    cand = [(int(i) * 4, int(j) * 4) for i, j in zip(ii, jj)]
    rng.shuffle(cand)
    wings = []
    for (i, j) in cand:
        if len(wings) >= 9:
            break
        n = Vector((-gx[i, j], -gy[i, j]))
        if n.length < 0.2:
            continue
        n.normalize()
        yaw = round(math.degrees(math.atan2(n.y, n.x)) / 90.0) * 90.0
        ax = Vector((math.cos(math.radians(yaw)), math.sin(math.radians(yaw))))
        if n.dot(ax) < 0.96:
            continue
        x0, y0 = float(XY[0][i, j]), float(XY[1][i, j])
        L, hw = 3.6, 1.0
        if not (HUD_BOT_Y - 2 < y0 < HUD_TOP_Y + 1) or abs(x0) > 9.3:
            continue
        if any(math.hypot(x0 - wx, y0 - wy) < 3.6 for (wx, wy, _) in wings):
            continue
        clear = True
        for a in np.arange(0.35, L + 0.3, 0.3):
            for b in (-hw, 0.0, hw):
                qx, qy = x0 + ax.x * a - ax.y * b, y0 + ax.y * a + ax.x * b
                if abs(qx) < 9.9 and -2.5 < qy < 43.5 and f[G._ij(qx, qy)] > 0.02:
                    clear = False
        if clear:
            wings.append((x0, y0, yaw))
    for k, (x0, y0, yaw) in enumerate(wings):
        g = B.group('wing', loc=(x0, y0, -0.38), rot=(0, 0, yaw))
        B.box((3.5, 0.12, 0.12), steel, g, loc=(1.75, 0, 0.0), bevel=0.02)
        if k % 3 == 2:
            for b in (-0.62, 0.0, 0.62):
                B.box((3.1, 0.5, 0.05), Rm.m, g, loc=(1.85, b, 0.06), bevel=0.0)
            lamp((x0 + math.cos(math.radians(yaw)) * 2, y0 + math.sin(math.radians(yaw)) * 2, 0.4), 120, '#ff6a20', 0.4)
        else:
            for a in (1.05, 2.75):
                for b in (-0.62, 0.62):
                    B.box((1.55, 1.0, 0.04), P.m, g, loc=(a, b, 0.07), bevel=0.0)
                    B.box((1.63, 1.08, 0.03), gold, g, loc=(a, b, 0.04), bevel=0.0)
        B.sphere(0.08, redl if k % 2 else greenl, g, loc=(3.55, 0, 0.12), segs=10)

    # --- small things about the deck ---------------------------------------
    for (x, y, r) in G.sprinkle(rng, 46, lambda g_: g_.uniform(0.3, 0.45), lambda r: 0.9, taken):
        kind = rng.random()
        if kind < 0.45:
            for i in range(rng.randint(1, 3)):
                sz = rng.uniform(0.34, 0.5)
                B.box((sz, sz, sz), rng.choice(paints), loc=(x + rng.uniform(-r, r) * 0.6, y + rng.uniform(-r, r) * 0.6,
                                                              sz / 2), rot=(0, 0, rng.uniform(0, 90)), bevel=0.03)
        elif kind < 0.7:
            for i in range(rng.randint(2, 3)):
                bx, by = x + rng.uniform(-r, r) * 0.6, y + rng.uniform(-r, r) * 0.6
                B.cyl(0.17, 0.5, rng.choice(paints), loc=(bx, by, 0.0), segs=12)
                B.cyl(0.18, 0.05, dark, loc=(bx, by, 0.2), segs=12)
        elif kind < 0.88:
            B.cyl(0.05, 0.75, steel, loc=(x, y, 0.0), segs=8)
            B.sphere(0.1, cyan if rng.random() < 0.6 else warm, loc=(x, y, 0.82), segs=10)
        else:
            B.box((0.7, 0.5, 0.28), dark, loc=(x, y, 0.14), bevel=0.04)
            B.box((0.5, 0.3, 0.02), warm, loc=(x, y, 0.29), bevel=0.0)

    # --- the corridors, the pads, the plate --------------------------------
    M = Mat('orbita_road')
    u, v = road_uv_sockets(M)
    n = M.noise(11.0, detail=4.0)
    base = M.ramp(n, [(0.3, '#63718f'), (0.7, '#7887a6')])
    seam = M.remap(M.math('ABSOLUTE', M.math('SUBTRACT', M.math('FRACT', M.math('MULTIPLY', v, 0.5)), 0.5)),
                   0.0, 0.02, 1.0, 0.0)
    chev = M.math('FRACT', M.math('MULTIPLY', M.math('SUBTRACT', v, M.math('MULTIPLY', u, 0.7)), 0.25))
    chev = M.math('MULTIPLY', M.remap(chev, 0.0, 0.07, 1.0, 0.0), M.math('LESS_THAN', u, 0.62))
    colr = M.mix(M.math('MULTIPLY', seam, 0.6), M.mix(M.math('MULTIPLY', chev, 0.55), base, '#b4c2e0'), '#2a3248')
    M.set(Base_Color=colr, Roughness=0.4, Metallic=0.4, Specular=0.5, Normal=M.bump(M.math('SUBTRACT', n, seam), 0.2, 0.02))
    road(G, M.m)
    kerbs(w, B.glow('orbita_kerb', '#8ab8ff', 2.0), width=0.08, height=0.04)
    plate = B.mat('orbita_plate', '#2a3248', rough=0.35, metal=0.7)
    K.pads(w, plate, rim_k=4.0, inner_mat=B.mat('orbita_plate_in', '#1a2030', rough=0.5, metal=0.6))
    K.foundation(w, plate)

    # --- light: a white sun, the planet's amber from below and behind --------
    B.sun(205, 50, 3.4, (1.0, 0.97, 0.92), angle=3.0, name='key')
    B.sun(40, 30, 0.9, (0.45, 0.62, 1.0), angle=25, name='rim')
    B.sun(250, 22, 0.5, (1.0, 0.62, 0.32), angle=30, name='planet')
    B.ambient((0.30, 0.36, 0.56), 0.32)


# ---------------------------------------------------------------------------
# Rendering, and checking what was rendered
# ---------------------------------------------------------------------------

def check(wid, out):
    """reads the depth back and says how much of the road, the pads and the
    fort's plate is covered by something that stands in front (or dug
    below Z = 0): the game's surfaces must be seen whole"""
    w = bsworlds.BY_ID[wid]
    d = np.load(os.path.join(out, wid + '_depth.npy'))
    ys, xs = np.mgrid[0:B.SCREEN_H, 0:B.SCREEN_W]
    gx = (xs + 0.5 - B.SCREEN_W / 2) / B.PX
    gy = (B.SCREEN_H - ys - 0.5) / B.ROW_Y
    dev = B.COS_E * gy - d                      # metres the surface stands nearer than the ground plane
    road = np.zeros(d.shape, bool)
    for (a, b, s) in _segments(w):
        dx, dy = b[0] - a[0], b[1] - a[1]
        t = np.clip(((gx - a[0]) * dx + (gy - a[1]) * dy) / (dx * dx + dy * dy), 0, 1)
        road |= np.hypot(gx - (a[0] + t * dx), gy - (a[1] + t * dy)) < HALF - 0.08
    pad = np.zeros(d.shape, bool)
    for (px, py) in w.pads:
        pad |= np.hypot(gx - px, gy - py) < bsworlds.PAD_R * 0.80
    fort = np.hypot(gx - w.fort[0], gy - w.fort[1]) < bsworlds.FORT_R - 0.15
    play = (ys > 96) & (ys < B.SCREEN_H - 190)
    msg = []
    for name, m in (('road', road), ('pads', pad), ('plate', fort)):
        m = m & play
        over = (dev > 0.10) & m
        under = (dev < -0.10) & m
        msg.append('%s %.2f%% covered, %.2f%% sunk' % (name, 100.0 * over.sum() / max(1, m.sum()),
                                                       100.0 * under.sum() / max(1, m.sum())))
    print('check %-7s %s' % (wid, '; '.join(msg)))


WORLDS = [
    ('luna', build_luna),
    ('selva', build_selva),
    ('magma', build_magma),
    ('hielo', build_hielo),
    ('orbita', build_orbita),
]


def main():
    out = B.arg('--out', os.path.join(B.ASSETS, 'worlds'))
    only = B.arg('--only')
    only = only.split(',') if only else None
    os.makedirs(out, exist_ok=True)
    times = []
    for wid, build in WORLDS:
        if only and wid not in only:
            continue
        t0 = time.time()
        B.setup(int(B.arg('--samples', 96)))
        B._POS = None       # the kit keeps its depth material across setup(), which frees it: make it again
        build(K.world(wid))
        t1 = time.time()
        B.render_world(wid, out)
        check(wid, out)
        times.append((wid, t1 - t0, time.time() - t1))
        print('built %s in %.1f s, rendered in %.1f s' % (wid, t1 - t0, time.time() - t1))
    for (wid, tb, tr) in times:
        print('time %-7s build %5.1f s  render %5.1f s' % (wid, tb, tr))


if __name__ == '__main__':
    main()
