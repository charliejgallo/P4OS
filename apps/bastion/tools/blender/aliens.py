"""
BASTION - the aliens and the bosses, built by code and rendered as sheets
(SPEC.md).

    /Applications/Blender.app/Contents/MacOS/Blender -b -P aliens.py -- [--only grub,queen] [--samples 48]
        [--out ../../assets/aliens]

One sheet an alien, <id>.png + <id>.json: 8 columns (yaws 0, 45 ... 315; 0
looks along +X, 90 away from the camera) by 4 rows, the frames of a walk
(or a flight) that loops: the frame after the fourth is the first again.

An alien is a function build_<id>(parent, frame) with frame 0..3 that
builds it under the parent, standing on the origin and looking along +X.
Every movement is a function of t = frame / 4 with period 1, which is what
makes the loop seamless. The ones that fly are built with their lowest
part at Z = 0.2 and nothing touching the ground.

What makes them one family, and not towers: round glossy bodies in one
saturated hue (a darker shade of it for limbs, a paler one for bellies),
neutral dark claws and bone horns, and one glow each for eyes, spots and
cores. Three of them have skin one sees through, which the sheet keeps as
alpha: the wasp's wings, the split's jelly and the healer's bell.

The .json of a sheet also says its size class ('n' 96 px, 'm' 144 px, 'b'
224 px, the bosses) and whether the alien flies.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bs_common as B  # noqa: E402

import bpy  # noqa: E402

from mathutils import Vector  # noqa: E402

YAWS = [i * 45.0 for i in range(8)]
# the three size classes of SPEC.md: the cell and where the origin lands
SIZES = {
    'n': dict(cell=(96, 96), anchor=(48, 70)),
    'm': dict(cell=(144, 144), anchor=(72, 108)),
    'b': dict(cell=(224, 224), anchor=(112, 170)),
}
PI2 = 2 * math.pi
CLAW = '#262430'    # the neutral dark of claws, mandibles and sockets
BONE = '#efe6d2'    # the neutral light of horns and teeth


# ---------------------------------------------------------------------------
# What the kit lacks for creatures: shades of a hue, a tapering round body
# along a curve, a leg that reaches its foot, the place of a foot in a step
# ---------------------------------------------------------------------------

def tint(color, k):
    """the same hue darker (k < 0, towards black) or paler (k > 0, towards
    white)"""
    c = B.rgb(color)
    if k < 0:
        return tuple(v * (1 + k) for v in c)
    return tuple(v + (1 - v) * k for v in c)


def skin(key, color, rough=0.25, clear=1.0, metal=0.0, alpha=1.0):
    """the glossy hide of an alien"""
    return B.mat('skin_' + key, color, rough=rough, metal=metal, clear=clear, spec=0.6, alpha=alpha)


def claw():
    return B.mat('claw', CLAW, rough=0.3, clear=0.6)


def bone():
    return B.mat('bone', BONE, rough=0.35, clear=0.5)


def spline(pts, n):
    """the points of a smooth curve through pts (Catmull-Rom), n to a span;
    pts are vectors or plain numbers"""
    if n <= 1:
        return list(pts)
    out = []
    last = len(pts) - 1
    for i in range(last):
        p0, p1, p2, p3 = pts[max(i - 1, 0)], pts[i], pts[i + 1], pts[min(i + 2, last)]
        for k in range(n):
            s = k / n
            out.append(0.5 * (2 * p1 + (p2 - p0) * s + (2 * p0 - 5 * p1 + 4 * p2 - p3) * s * s
                              + (3 * p1 - p0 - 3 * p2 + p3) * s * s * s))
    out.append(pts[last])
    return out


def worm(pts, radii, m, parent=None, loc=(0, 0, 0), rot=(0, 0, 0), n=4, segs=12, wide=1.0, tall=1.0, name='worm'):
    """a round body along the smooth curve through pts whose radius follows
    'radii' (one a point) and whose ends are rounded: tails, horns, legs,
    tentacles, abdomens. wide and tall squash its section (tall is the
    side that starts looking up)."""
    P = spline([Vector(p) for p in pts], n)
    R = [max(r, 0.004) for r in spline([float(r) for r in radii], n)]
    d0 = (P[1] - P[0]).normalized()
    d1 = (P[-1] - P[-2]).normalized()
    ring = [(P[0] - d0 * R[0] * math.sin(a), R[0] * math.cos(a)) for a in (1.45, 1.0, 0.5)]
    ring += list(zip(P, R))
    ring += [(P[-1] + d1 * R[-1] * math.sin(a), R[-1] * math.cos(a)) for a in (0.5, 1.0, 1.45)]
    verts, faces = [], []
    u = None
    last = len(ring) - 1
    for i, (p, r) in enumerate(ring):
        d = (ring[min(i + 1, last)][0] - ring[max(i - 1, 0)][0]).normalized()
        if u is None:
            u = d.cross(Vector((0, 0, 1)) if abs(d.z) < 0.9 else Vector((1, 0, 0))).normalized()
        else:
            u = (u - d * u.dot(d)).normalized()   # carried along the curve, so it never flips
        v = u.cross(d)
        for j in range(segs):
            a = PI2 * j / segs
            verts.append(p + u * (math.cos(a) * r * wide) + v * (math.sin(a) * r * tall))
    for i in range(last):
        for j in range(segs):
            k = (j + 1) % segs
            faces.append((i * segs + j, (i + 1) * segs + j, (i + 1) * segs + k, i * segs + k))
    faces.append(tuple(range(segs)))
    faces.append(tuple(reversed(range(last * segs, (last + 1) * segs))))
    return B.mesh(verts, faces, m, parent, loc=loc, rot=rot, smooth=True, name=name)


def knee(hip, foot, a, b, pole):
    """where the knee of a leg with bones a and b is, between its hip and
    its foot, bending towards 'pole'"""
    h, f, pole = Vector(hip), Vector(foot), Vector(pole)
    d = f - h
    L = min(d.length, (a + b) * 0.999)
    n = d.normalized()
    x = (a * a - b * b + L * L) / (2 * L)
    up = pole - n * pole.dot(n)
    return h + n * x + up.normalized() * math.sqrt(max(a * a - x * x, 0.0))


def leg(parent, m, hip, foot, a, b, pole, radii, n=3, segs=10):
    """a leg of two bones from hip to foot, bending towards pole"""
    k = knee(hip, foot, a, b, pole)
    worm([hip, k, foot], radii, m, parent, n=n, segs=segs, name='leg')
    return k


def step(t, stride, lift):
    """a foot at phase t of its step, as (how far ahead, how high): on the
    ground going back for half the loop, in the air coming forward for the
    other half"""
    a = PI2 * t
    return stride * math.cos(a), lift * max(0.0, -math.sin(a))


def wave(t, phase=0.0):
    return math.sin(PI2 * (t - phase))


def eye(parent, loc, r, g, out=(1, 0, 0)):
    """a glowing eye in a dark socket that looks along 'out'"""
    o = Vector(out).normalized()
    B.sphere(r * 1.25, claw(), parent, loc=Vector(loc) - o * r * 0.55, segs=16, name='socket')
    return B.sphere(r, g, parent, loc=loc, segs=16, name='eye')


# ---------------------------------------------------------------------------
# grub: the basic crawler. Five green segments that swell one after the
# other from head to tail, six stubby legs, two big eyes.
# ---------------------------------------------------------------------------

def build_grub(p, f):
    t = f / 4.0
    c = '#4fd620'
    body, dk, lt = skin('grub', c), skin('grub_d', tint(c, -0.6)), skin('grub_l', tint(c, 0.35))
    g = B.glow('grub_g', '#ffe81e', 2.4)
    segs = [(0.38, 0.19), (0.15, 0.24), (-0.10, 0.25), (-0.33, 0.21), (-0.51, 0.14)]
    for i, (x, r) in enumerate(segs):
        w = wave(t, i * 0.22)
        sz, sx = 1 + 0.18 * w, 1 - 0.12 * w
        z = r * 0.9 * sz + 0.05
        B.sphere(r, dk if i == 0 else body, p, loc=(x, 0, z), scale=(sx * 1.08, 1.0, sz * 0.92), segs=24)
        if i >= 1:
            # a paler saddle on each segment, and a light on the three big ones
            B.sphere(r * 0.80, lt, p, loc=(x, 0, z + r * sz * 0.27), scale=(sx * 0.75, 0.95, sz * 0.92), segs=16)
        if 1 <= i <= 3:
            B.sphere(0.042, g, p, loc=(x, 0, z + r * sz * 0.95), scale=(1.2, 1.5, 0.7), segs=12)
            for s in (-1, 1):
                fx, fz = step(t + i * 0.25 + (0.5 if s > 0 else 0), 0.08, 0.07)
                worm([(x, s * r * 0.7, 0.14), (x + fx, s * (r + 0.08), 0.03 + fz)], [0.07, 0.04], dk, p, n=1, segs=8)
    hx, hr = segs[0]
    hz = hr * 0.9 * (1 + 0.18 * wave(t)) + 0.05
    for s in (-1, 1):
        eye(p, (hx + 0.10, s * 0.105, hz + 0.09), 0.075, g, out=(1, s * 0.5, 0.6))
        worm([(hx + 0.10, s * 0.10, hz - 0.08), (hx + 0.22, s * 0.09, hz - 0.10), (hx + 0.27, s * 0.02, hz - 0.09)],
             [0.045, 0.04, 0.012], claw(), p, n=3, segs=8)
    tx = segs[4][0]
    worm([(tx - 0.05, 0, 0.14), (tx - 0.17, 0, 0.22), (tx - 0.21, 0, 0.36)], [0.065, 0.045, 0.012], dk, p, n=3,
         segs=8)


# ---------------------------------------------------------------------------
# runner: the sprinter. A yellow body held level on two long legs that
# bend backwards, a head thrust forward, a tail out behind for balance.
# ---------------------------------------------------------------------------

def build_runner(p, f):
    t = f / 4.0
    c = '#ffcc10'
    body, dk, lt = skin('run', c), skin('run_d', tint(c, -0.62)), skin('run_l', tint(c, 0.55))
    g = B.glow('run_g', '#ff2060', 2.6)
    bob = 0.045 * math.cos(2 * PI2 * t)
    b = B.group('body', p, loc=(0, 0, bob), rot=(0, 3 * wave(t, 0.1), 0))
    sway = 0.08 * wave(t)
    spine = [(-0.70, sway * 1.6, 0.74), (-0.42, sway, 0.63), (-0.12, 0, 0.60), (0.14, 0, 0.65), (0.32, 0, 0.74)]
    worm(spine, [0.035, 0.09, 0.21, 0.20, 0.13], body, b, wide=0.88)
    # a dark stripe down the back, from the crest to the tail
    worm([(x, y, z + r) for (x, y, z), r in zip(spine, [0.02, 0.055, 0.15, 0.14, 0.09])],
         [0.03, 0.06, 0.10, 0.10, 0.07], dk, b, wide=0.75, tall=0.7)
    B.sphere(0.17, body, b, loc=(0.46, 0, 0.80), rot=(0, 14, 0), scale=(1.45, 0.95, 0.85), segs=24)
    B.sphere(0.10, lt, b, loc=(0.60, 0, 0.72), rot=(0, 14, 0), scale=(1.3, 1.0, 0.7), segs=16)
    worm([(0.50, 0, 0.90), (0.28, 0, 1.04), (0.04, 0, 1.08)], [0.075, 0.065, 0.015], dk, b, wide=0.6)
    for s in (-1, 1):
        eye(b, (0.55, s * 0.125, 0.845), 0.065, g, out=(0.5, s, 0.5))
    B.sphere(0.045, g, b, loc=(-0.70, sway * 1.6, 0.74), segs=10)
    for s, ph in ((1, 0.0), (-1, 0.5)):
        fx, fz = step(t + ph, 0.28, 0.18)
        hip = (-0.05, s * 0.15, 0.55 + bob)
        foot = (0.0 + fx, s * 0.17, 0.07 + fz)
        k = knee(hip, foot, 0.33, 0.33, (-1, 0, 0.1))
        worm([hip, k], [0.13, 0.07], body, p, n=1, segs=10)
        worm([k, foot], [0.065, 0.05], dk, p, n=1, segs=10)
        B.sphere(0.08, claw(), p, loc=(foot[0] + 0.07, foot[1], foot[2] - 0.015), scale=(1.8, 1.0, 0.6), segs=12)


# ---------------------------------------------------------------------------
# swarm: the mite. A red bead with two eyes far too big for it and six
# legs that scuttle three at a time.
# ---------------------------------------------------------------------------

def build_swarm(p, f):
    t = f / 4.0
    c = '#ff3020'
    body, dk = skin('mite', c), skin('mite_d', tint(c, -0.55))
    g = B.glow('mite_g', '#ffe020', 2.6)
    bob = 0.02 * math.cos(2 * PI2 * t)
    B.sphere(0.19, body, p, loc=(-0.05, 0, 0.19 + bob), scale=(1.2, 1.0, 0.8), segs=20)
    B.sphere(0.12, dk, p, loc=(0.16, 0, 0.16 + bob), segs=16)
    for s in (-1, 1):
        eye(p, (0.22, s * 0.075, 0.21 + bob), 0.06, g, out=(1, s * 0.4, 0.5))
        worm([(0.22, s * 0.05, 0.11 + bob), (0.32, s * 0.07, 0.09), (0.36, s * 0.015, 0.08)], [0.035, 0.03, 0.01],
             claw(), p, n=3, segs=8)
    for i, ang in enumerate((48, 92, 136)):
        for s in (-1, 1):
            ca, sa = math.cos(math.radians(ang)), math.sin(math.radians(ang)) * s
            fx, fz = step(t + (0.5 if (i + (s > 0)) % 2 else 0.0), 0.06, 0.07)
            hip = (-0.03 + ca * 0.12, sa * 0.14, 0.15 + bob)
            foot = (-0.03 + ca * 0.33 + fx, sa * 0.36, 0.02 + fz)
            leg(p, dk, hip, foot, 0.19, 0.24, (0, 0, 1), [0.045, 0.04, 0.025], segs=8)
    B.sphere(0.05, g, p, loc=(-0.10, 0, 0.335 + bob), scale=(1.5, 1.0, 0.5), segs=10)


# ---------------------------------------------------------------------------
# shell: the beetle. Four thick steel-blue plates lapped over each other
# with a hot light in the seams, a horned head, six feet under the rim.
# ---------------------------------------------------------------------------

def build_shell(p, f):
    t = f / 4.0
    c = '#5a84cc'
    plate = skin('shell', c, rough=0.22, metal=0.35)
    dk = skin('shell_d', tint(c, -0.55))
    g = B.glow('shell_g', '#ff9010', 2.6)
    B.sphere(0.40, claw(), p, loc=(-0.06, 0, 0.24), scale=(1.3, 0.92, 0.55), segs=20)
    xs = [0.30, 0.07, -0.17, -0.40]
    rs = [0.36, 0.43, 0.41, 0.32]
    px = []
    for i in range(4):
        w = wave(t, i * 0.25)
        x = xs[i] + 0.025 * w
        px.append(x)
        B.sphere(rs[i], plate, p, loc=(x, 0, 0.20 + 0.012 * w), rot=(0, -8 + 4 * w, 0), scale=(0.62, 1.0, 0.86),
                 segs=28)
    for i in range(3):
        r = min(rs[i], rs[i + 1]) * 0.93
        o = B.torus(r, 0.035, g, p, loc=((px[i] + px[i + 1]) / 2 + 0.01, 0, 0.20), rot=(0, 90, 0), segs=28, tsegs=8)
        o.scale = (0.86, 1.0, 1.0)
    nod = 0.02 * wave(t, 0.1)
    B.sphere(0.17, dk, p, loc=(0.57, 0, 0.18 + nod), scale=(1.1, 1.0, 0.9), segs=16)
    worm([(0.62, 0, 0.27 + nod), (0.76, 0, 0.33 + nod), (0.84, 0, 0.50 + nod)], [0.07, 0.055, 0.015], plate, p)
    for s in (-1, 1):
        eye(p, (0.67, s * 0.11, 0.22 + nod), 0.055, g, out=(1, s * 0.6, 0.4))
    for i, x in enumerate((0.30, 0.0, -0.30)):
        for s in (-1, 1):
            fx, fz = step(t + (0.5 if (i + (s > 0)) % 2 else 0.0), 0.09, 0.08)
            hip = (x, s * 0.30, 0.16)
            foot = (x + fx, s * 0.50, 0.03 + fz)
            leg(p, dk, hip, foot, 0.16, 0.22, (0, 0, 1), [0.07, 0.06, 0.04], segs=8)


# ---------------------------------------------------------------------------
# wasp: the flier. An orange body banded in black that hangs under four
# clear wings; the front pair and the hind pair beat a little apart, so
# no two frames are alike. The sting hangs lowest, at Z = 0.2.
# ---------------------------------------------------------------------------

WING_F = [(0.07, 0.0), (0.15, 0.22), (0.13, 0.50), (0.02, 0.68), (-0.11, 0.56), (-0.13, 0.26), (-0.07, 0.0)]
WING_H = [(0.05, 0.0), (0.10, 0.18), (0.07, 0.38), (-0.02, 0.46), (-0.11, 0.36), (-0.11, 0.16), (-0.05, 0.0)]


def build_wasp(p, f):
    t = f / 4.0
    c = '#ff8612'
    body, dk = skin('wasp', c), skin('wasp_d', tint(c, -0.6))
    wing = skin('wasp_w', tint(c, 0.78), rough=0.12, alpha=0.62)
    g = B.glow('wasp_g', '#28e8ff', 2.6)
    b = B.group('body', p, loc=(0, 0, 0.03 * wave(t, 0.1)))
    B.sphere(0.18, body, b, loc=(0.08, 0, 0.66), scale=(1.15, 1.0, 0.95), segs=20)
    B.sphere(0.145, dk, b, loc=(0.33, 0, 0.63), segs=20)
    for s in (-1, 1):
        eye(b, (0.39, s * 0.095, 0.68), 0.08, g, out=(0.7, s, 0.5))
        worm([(0.40, s * 0.05, 0.74), (0.52, s * 0.10, 0.86), (0.64, s * 0.14, 0.84)], [0.03, 0.03, 0.025], claw(), b,
             segs=8)
        worm([(0.42, s * 0.06, 0.55), (0.50, s * 0.05, 0.49)], [0.035, 0.012], claw(), b, n=1, segs=8)
    # the abdomen curls under, more when the wings come down
    tuck = 0.04 * wave(t, 0.25)
    path = spline([Vector(v) for v in [(-0.10, 0, 0.63), (-0.30, 0, 0.60 - tuck), (-0.47, 0, 0.49 - 2 * tuck),
                                       (-0.56, 0, 0.36 - 2 * tuck)]], 1)
    for i, (q, r) in enumerate(zip(path, [0.14, 0.19, 0.16, 0.10])):
        B.sphere(r, claw() if i % 2 == 0 else body, b, loc=q, scale=(0.85, 1.0, 1.0), rot=(0, -20 * i, 0), segs=20)
    tip = (-0.60, 0, 0.21 - 0.03 * wave(t, 0.1))    # the body's bob is taken back: the sting stays at 0.2
    worm([(-0.58, 0, 0.36 - 2 * tuck), tip], [0.06, 0.02], g, b, n=1, segs=8)
    for i, x in enumerate((0.18, 0.08, -0.02)):
        for s in (-1, 1):
            d = 0.02 * wave(t, 0.15 * i)
            worm([(x, s * 0.10, 0.56), (x + 0.03, s * 0.20, 0.47), (x - 0.06 + d, s * 0.19, 0.34)],
                 [0.04, 0.035, 0.025], claw(), b, segs=8)
    a_f = 14 + 44 * math.cos(PI2 * t)
    a_h = 8 + 38 * math.cos(PI2 * (t - 0.2))
    for s in (-1, 1):
        w = B.group('wing', b, loc=(0.13, s * 0.09, 0.78), rot=(s * a_f, 0, s * 12), scale=(1, s, 1))
        B.prism(WING_F, -0.012, 0.012, wing, w, bevel=0.0)
        worm([(0.05, 0.0, 0), (0.10, 0.30, 0), (0.03, 0.62, 0)], [0.03, 0.025, 0.012], dk, w, segs=6)
        w = B.group('wing', b, loc=(0.00, s * 0.09, 0.77), rot=(s * a_h, 0, s * 48), scale=(1, s, 1))
        B.prism(WING_H, -0.012, 0.012, wing, w, bevel=0.0)


# ---------------------------------------------------------------------------
# split: the blob that is about to be two. Two purple lobes of clear jelly
# that swell in turn, a bright nucleus drifting in each, feelers ahead and
# a trail of slime behind.
# ---------------------------------------------------------------------------

def build_split(p, f):
    t = f / 4.0
    c = '#a23cf2'
    jelly = skin('split', c, rough=0.10, alpha=0.55)
    dk, lt = skin('split_d', tint(c, -0.45)), skin('split_l', tint(c, 0.5), alpha=0.8)
    g = B.glow('split_g', '#9cff00', 3.0)
    w = wave(t)
    B.sphere(0.46, dk, p, loc=(-0.04, 0, 0.10), scale=(1.08, 1.18, 0.24), segs=24)
    worm([(-0.25, 0, 0.13), (-0.48, 0.06 * w, 0.09), (-0.66, -0.05 * w, 0.06)], [0.20, 0.11, 0.03], dk, p, tall=0.6)
    for s in (-1, 1):
        k = 1 + 0.11 * w * s
        h = 1 - 0.13 * w * s
        y = s * (0.19 + 0.03 * w * s)
        z = 0.33 * h + 0.05
        B.sphere(0.35, jelly, p, loc=(0.0, y, z), scale=(k, k, h * 0.98), segs=28)
        a = PI2 * (t + (0.0 if s > 0 else 0.4))
        B.sphere(0.25, dk, p, loc=(-0.02, y, z - 0.10), scale=(k, k, h * 0.8), segs=16)
        n = (0.03 + 0.05 * math.cos(a), y + 0.04 * math.sin(a) * s, z + 0.33 * h - 0.115 + 0.02 * math.sin(a))
        B.sphere(0.15, g, p, loc=n, segs=16)
        B.sphere(0.05, lt, p, loc=(-0.16 + 0.03 * w, y * 1.5, z + 0.12), segs=10)
        B.sphere(0.04, lt, p, loc=(0.17, y * 0.6 - 0.03 * w, z + 0.19), segs=10)
        fw = 0.04 * wave(t, 0.25 * (s > 0))
        worm([(0.24, s * 0.13, 0.30), (0.40, s * 0.17 + fw, 0.46), (0.50, s * 0.20 + fw * 2, 0.62)],
             [0.07, 0.05, 0.035], dk, p, segs=8)
        B.sphere(0.06, g, p, loc=(0.51, s * 0.20 + fw * 2, 0.64), segs=10)


# ---------------------------------------------------------------------------
# shield: the walker that carries the shield. A cyan crab on four arched
# legs; three claws on its back hold a bright ball inside a ring that
# wobbles round it once a loop.
# ---------------------------------------------------------------------------

def build_shield(p, f):
    t = f / 4.0
    c = '#18cce6'
    body, dk, lt = skin('shd', c), skin('shd_d', tint(c, -0.58)), skin('shd_l', tint(c, 0.5))
    g = B.glow('shd_g', '#ffec30', 2.8)
    bob = 0.025 * math.cos(2 * PI2 * t)
    b = B.group('body', p, loc=(0, 0, bob))
    B.sphere(0.27, body, b, loc=(0, 0, 0.40), scale=(1.25, 1.0, 0.64), segs=24)
    B.sphere(0.16, dk, b, loc=(0.34, 0, 0.36), segs=16)
    for s in (-1, 1):
        eye(b, (0.43, s * 0.09, 0.42), 0.06, g, out=(1, s * 0.5, 0.5))
    for i, (hx, fx0) in enumerate(((0.17, 0.36), (-0.17, -0.36))):
        for s in (-1, 1):
            fx, fz = step(t + (0.5 if (i + (s > 0)) % 2 else 0.0), 0.11, 0.12)
            hip = (hx, s * 0.20, 0.40 + bob)
            foot = (fx0 + fx, s * 0.46, 0.03 + fz)
            leg(p, dk, hip, foot, 0.30, 0.46, (0, 0, 1), [0.095, 0.085, 0.04])
    # the generator: three claws, the ball, its ring
    cx, cz = -0.04, 0.92
    for i in range(3):
        a = math.radians(60 + 120 * i)
        ca, sa = math.cos(a), math.sin(a)
        worm([(cx + ca * 0.14, sa * 0.14, 0.52), (cx + ca * 0.34, sa * 0.34, 0.66), (cx + ca * 0.33, sa * 0.33, 0.86),
              (cx + ca * 0.24, sa * 0.24, 0.98)], [0.08, 0.07, 0.05, 0.015], dk, b, segs=8)
    pulse = 1 + 0.10 * wave(t)
    B.sphere(0.175 * pulse, g, b, loc=(cx, 0, cz), segs=20)
    B.torus(0.265, 0.04, lt, b, loc=(cx, 0, cz), rot=(22 * math.cos(PI2 * t), 22 * math.sin(PI2 * t), 0), segs=28,
            tsegs=8)


# ---------------------------------------------------------------------------
# healer: the jellyfish. A mint bell, lit from inside, that opens and
# closes as it swims leaning forward, its tentacles trailing; the four
# rings on its crown are its mark. Tentacle tips at Z = 0.2.
# ---------------------------------------------------------------------------

def build_healer(p, f):
    t = f / 4.0
    c = '#48eea8'
    bell = B.mat('skin_heal', c, rough=0.12, clear=1.0, spec=0.6, alpha=0.80, emit=c, emit_k=0.55)
    body, dk = skin('heal_o', c), skin('heal_d', tint(c, -0.5))
    g = B.glow('heal_g', '#f4ff9a', 2.4)
    w = wave(t)
    k, h = 1 + 0.13 * w, 1 - 0.16 * w
    b = B.group('bell', p, loc=(0, 0, 0.80 + 0.03 * wave(t, 0.25)), rot=(0, 20, 0))
    prof = [(0.06, 0.07), (0.33 * k, 0.0), (0.41 * k, 0.04 * h), (0.42 * k, 0.12 * h), (0.36 * k, 0.26 * h),
            (0.22 * k, 0.37 * h), (0.0, 0.41 * h)]
    B.lathe(prof, bell, b, segs=28)
    B.torus(0.415 * k, 0.05, body, b, loc=(0, 0, 0.07 * h), segs=28, tsegs=8)
    B.sphere(0.17, g, b, loc=(0, 0, 0.15 * h), scale=(k, k, h), segs=16)
    for i in range(4):
        a = math.radians(45 + 90 * i)
        B.torus(0.062, 0.028, g, b, loc=(math.cos(a) * 0.15 * k, math.sin(a) * 0.15 * k, 0.385 * h),
                rot=(-22 * math.sin(a), 22 * math.cos(a), 0), segs=14, tsegs=6)
    for s in (-1, 1):
        eye(b, (0.39 * k, s * 0.14, 0.19 * h), 0.065, g, out=(1, s * 0.3, 0.3))
    # the tentacles hang from under the bell and trail behind it; they are
    # built under the alien, not the leaning bell, so they hang plumb
    top = 0.80
    for i in range(7):
        a = math.radians(360 * i / 7 + 25)
        r = 0.25 * k
        x0, y0 = math.cos(a) * r - 0.03, math.sin(a) * r
        pts, rad = [], []
        for j in range(5):
            d = j / 4.0
            sw = 0.07 * d * wave(t, d * 0.7 + i * 0.13)
            pts.append((x0 - 0.20 * d * d + sw * math.cos(a + 1.3), y0 * (1 - 0.25 * d) + sw * math.sin(a + 1.3),
                        top + 0.04 - (top - 0.17) * d))
            rad.append(0.05 - 0.028 * d)
        worm(pts, rad, body if i % 2 else dk, p, n=3, segs=8)
    worm([(0, 0, top + 0.05), (-0.04, 0, 0.60), (-0.10 + 0.04 * w, 0, 0.42), (-0.13, 0, 0.29)],
         [0.10, 0.09, 0.07, 0.04], dk, p, segs=10)


# ---------------------------------------------------------------------------
# brute: the hulk. Dark red, all shoulders, on four legs with fists for
# front feet; a low head with a heavy jaw between two bone horns, and a
# row of embers down its back.
# ---------------------------------------------------------------------------

def build_brute(p, f):
    t = f / 4.0
    c = '#b4161e'
    body, dk, lt = skin('brute', c), skin('brute_d', tint(c, -0.55)), skin('brute_l', tint(c, 0.28))
    g = B.glow('brute_g', '#ffae18', 2.8)
    k = 0.86
    root = B.group('all', p, loc=(0.02, 0, 0), scale=(k, k, k))
    bob = 0.05 * math.cos(2 * PI2 * t)
    b = B.group('body', root, loc=(0, 0, bob), rot=(3.5 * wave(t), 0, 0))
    spine = [(-0.95, 0, 0.82), (-0.55, 0, 0.98), (0.05, 0, 1.20), (0.50, 0, 1.22), (0.82, 0, 1.04)]
    worm(spine, [0.24, 0.42, 0.62, 0.58, 0.36], body, b, wide=1.12, tall=0.95, segs=20)
    B.sphere(0.46, lt, b, loc=(0.46, 0, 0.92), scale=(1.0, 1.1, 0.7), segs=20)
    # spines down the back, and three gills of light on each flank
    for i, (x, z, r) in enumerate(((0.50, 1.22, 0.58), (0.08, 1.20, 0.62), (-0.30, 1.09, 0.53), (-0.62, 0.95, 0.40))):
        top = z + r * 0.95
        h = 0.34 - 0.04 * i
        worm([(x, 0, top - 0.10), (x - 0.06, 0, top + h * 0.5), (x - 0.18, 0, top + h)], [0.15 - 0.015 * i, 0.11, 0.02],
             dk, b, segs=8)
        a = math.radians(52)
        for s in (-1, 1):
            if i < 3:
                B.sphere(0.13 - 0.015 * i, g, b, loc=(x - 0.20, s * r * 1.12 * math.sin(a), z + r * 0.95 * math.cos(a)),
                         rot=(s * -52, 0, 0), scale=(0.42, 1.9, 0.4), segs=12)
    worm([(-1.00, 0, 0.84), (-1.20, 0.06 * wave(t), 0.80), (-1.32, -0.08 * wave(t), 0.90)], [0.16, 0.10, 0.03], dk, b)
    h = B.group('head', b, loc=(1.04, 0, 0.90), rot=(0, 6 + 5 * wave(t, 0.1), 0))
    B.sphere(0.33, body, h, scale=(1.15, 1.0, 0.9), segs=24)
    B.sphere(0.25, dk, h, loc=(0.13, 0, -0.17), scale=(1.25, 0.95, 0.6), segs=16)
    for s in (-1, 1):
        eye(h, (0.25, s * 0.20, 0.11), 0.085, g, out=(1, s * 0.5, 0.5))
        worm([(-0.02, s * 0.24, 0.14), (0.06, s * 0.52, 0.28), (0.32, s * 0.64, 0.50), (0.60, s * 0.54, 0.64)],
             [0.14, 0.12, 0.08, 0.015], bone(), h, segs=10)
        for x in (0.20, 0.36):
            worm([(x, s * 0.13, -0.10), (x + 0.02, s * 0.14, 0.04)], [0.05, 0.012], bone(), h, n=1, segs=6)
    worm([(0.30, 0, 0.14), (0.42, 0, 0.26)], [0.07, 0.015], bone(), h, n=1, segs=8)
    # it trots: the legs go in diagonal pairs, the front ones on their knuckles
    for s in (-1, 1):
        fx, fz = step(t + (0.0 if s > 0 else 0.5), 0.24, 0.20)
        hip = (0.30, s * 0.56, 1.00 + bob)
        foot = (0.40 + fx, s * 0.66, 0.16 + fz)
        leg(root, body, hip, foot, 0.56, 0.52, (-1, 0, 0.4), [0.30, 0.21, 0.19], segs=14)
        B.sphere(0.25, dk, root, loc=(foot[0] + 0.04, foot[1], foot[2] - 0.02), scale=(1.15, 1.0, 0.72), segs=16)
        fx, fz = step(t + (0.5 if s > 0 else 0.0), 0.20, 0.16)
        hip = (-0.74, s * 0.34, 0.86 + bob)
        foot = (-0.72 + fx, s * 0.44, 0.10 + fz)
        leg(root, body, hip, foot, 0.46, 0.44, (1, 0, 0.3), [0.24, 0.16, 0.13], segs=12)
        B.sphere(0.17, dk, root, loc=(foot[0] + 0.06, foot[1], foot[2] - 0.02), scale=(1.4, 1.0, 0.65), segs=14)


# ---------------------------------------------------------------------------
# phase: the stalker. Thin and upright, pale violet, with a long head
# swept back, one slit of an eye and two pale scythes held ahead of it.
# ---------------------------------------------------------------------------

def build_phase(p, f):
    t = f / 4.0
    c = '#c2aaff'
    body, dk, lt = skin('phase', c), skin('phase_d', tint(c, -0.55)), skin('phase_l', tint(c, 0.6))
    g = B.glow('phase_g', '#40ffe0', 2.6)
    k = 0.90
    bob = 0.03 * math.cos(2 * PI2 * t)
    root = B.group('all', p, scale=(k, k, k))
    b = B.group('body', root, loc=(0, 0, bob), rot=(0, 2 * wave(t), 0))
    sw = 0.06 * wave(t)
    worm([(-0.12, 0, 0.64), (-0.02, 0, 0.86), (0.10, 0, 1.08), (0.20, 0, 1.22)], [0.08, 0.12, 0.17, 0.08], body, b,
         wide=1.15, tall=0.72)
    worm([(-0.12, 0, 0.66), (-0.36, sw, 0.56), (-0.58, -sw, 0.60), (-0.74, sw * 1.5, 0.72)], [0.08, 0.06, 0.04, 0.012],
         dk, b)
    worm([(0.38, 0, 1.25), (0.24, 0, 1.36), (0.00, 0, 1.47), (-0.26, 0, 1.46)], [0.07, 0.13, 0.10, 0.02], body, b,
         wide=0.9)
    B.sphere(0.075, g, b, loc=(0.33, 0, 1.325), scale=(0.7, 1.55, 0.5), rot=(0, 30, 0), segs=14)
    B.sphere(0.06, g, b, loc=(0.20, 0, 1.03), scale=(0.6, 1.0, 1.5), segs=12)
    for s in (-1, 1):
        a = 0.08 * wave(t, 0.0 if s > 0 else 0.5)
        sh = (0.10, s * 0.19, 1.10)
        el = (0.10 - a, s * 0.34, 0.86)
        wr = (0.36 + a, s * 0.27, 0.92 + a)
        worm([sh, el, wr], [0.06, 0.05, 0.045], dk, b, n=3, segs=8)
        worm([wr, (0.56 + a, s * 0.22, 0.86 + a), (0.72 + a, s * 0.14, 0.62 + a)], [0.06, 0.05, 0.012], lt, b, n=3,
             segs=8, wide=0.6)
        fx, fz = step(t + (0.0 if s > 0 else 0.5), 0.20, 0.14)
        hip = (-0.10, s * 0.10, 0.66 + bob)
        foot = (-0.02 + fx, s * 0.15, 0.05 + fz)
        leg(root, dk, hip, foot, 0.36, 0.36, (1, 0, 0.2), [0.075, 0.055, 0.04], segs=8)
        B.sphere(0.06, claw(), root, loc=(foot[0] + 0.05, foot[1], foot[2] - 0.01), scale=(1.8, 1.0, 0.6), segs=10)


# ---------------------------------------------------------------------------
# jammer: the saboteur. A magenta ball with one great eye that floats
# under three whipping antennae; the lights on its brim run round it.
# The tendrils under it end at Z = 0.2.
# ---------------------------------------------------------------------------

def build_jammer(p, f):
    t = f / 4.0
    c = '#ff28b8'
    body, dk = skin('jam', c), skin('jam_d', tint(c, -0.6))
    g = B.glow('jam_g', '#b4ff10', 2.8)
    k = 1.14        # it grows about Z = 0.2, where its tendrils end
    b = B.group('body', p, loc=(0, 0, 0.03 * wave(t, 0.1) - 0.2 * (k - 1)), scale=(k, k, k))
    B.sphere(0.30, body, b, loc=(0, 0, 0.74), segs=28)
    B.sphere(0.25, dk, b, loc=(0, 0, 0.62), scale=(1, 1, 0.85), segs=20)
    eye(b, (0.25, 0, 0.78), 0.15, g, out=(1, 0, 0.25))
    B.sphere(0.065, claw(), b, loc=(0.375, 0, 0.815), segs=12)
    B.torus(0.345, 0.05, dk, b, loc=(-0.03, 0, 0.70), rot=(0, -14, 0), segs=28, tsegs=8)
    ring = B.group('ring', b, loc=(-0.03, 0, 0.70), rot=(0, -14, 0))
    for i in range(6):
        a = math.radians(60 * i + 60 * t + 30)     # one place on in a loop: the same picture again
        B.sphere(0.045, g, ring, loc=(0.37 * math.cos(a), 0.37 * math.sin(a), 0.0), segs=10)
    sw = 0.07 * wave(t)
    sw2 = 0.07 * wave(t, 0.3)
    whips = [[(-0.06, 0, 1.00), (-0.20 + sw, 0, 1.20), (-0.32 + 2 * sw, 0, 1.38)]]
    for s in (-1, 1):
        whips.append([(0.02, s * 0.15, 0.98), (-0.08, s * 0.32 + sw2 * s, 1.12),
                      (-0.24 + sw2, s * 0.42 + 2 * sw2 * s, 1.22)])
    for pts in whips:
        worm(pts, [0.055, 0.045, 0.035], dk, b, segs=8)
        B.sphere(0.07, g, b, loc=pts[-1], segs=12)
    for i in range(3):
        a = math.radians(120 * i + 60)
        x0, y0 = 0.14 * math.cos(a), 0.14 * math.sin(a)
        d = 0.05 * wave(t, i / 3.0)
        tip = (x0 - 0.10 - d, y0 * 1.2, 0.21 - 0.026 * wave(t, 0.1))    # the bob taken back
        worm([(x0, y0, 0.46), (x0 - 0.05 + d, y0 * 1.2, 0.34), tip], [0.055, 0.045, 0.02], dk, b, segs=8)


# ---------------------------------------------------------------------------
# carrier: the manta. A wide teal wing that rolls a wave out to its tips
# as it flies, two horns ahead of its eyes, a whip of a tail, and six
# amber brood sacs on its back where the mites ride. Lowest part at 0.2.
# ---------------------------------------------------------------------------

MANTA_SPAN = 1.17       # half of it: the wing is 2.34 m from tip to tip


def manta_z(t, au, zc):
    """how high the manta's wing is at the fraction au of its span: a wave
    that runs out to the tips, bigger the farther out"""
    return zc + 0.30 * au ** 1.4 * (0.30 + math.sin(PI2 * t - 1.5 * au))


def manta(parent, top, under, t, zc):
    """the manta's wing as two skins over a grid: u runs along the span, v
    from the leading to the trailing edge"""
    span = MANTA_SPAN
    NU, NV = 33, 9
    vt, vb, faces = [], [], []
    for i in range(NU):
        u = -1 + 2.0 * i / (NU - 1)
        au = abs(u)
        le = 0.66 - 0.58 * au ** 1.5
        te = -0.62 + 0.60 * au ** 2
        z = manta_z(t, au, zc)
        thick = 0.20 * (1 - au) ** 1.3 + 0.035 * (1 - au ** 6)
        for j in range(NV):
            v = j / (NV - 1.0)
            x = le + (te - le) * v
            th = thick * math.sin(math.pi * v) ** 0.6
            vt.append((x, u * span, z + th))
            vb.append((x, u * span, z - th * 0.55))
    for i in range(NU - 1):
        for j in range(NV - 1):
            a, b2, c2, d = i * NV + j, (i + 1) * NV + j, (i + 1) * NV + j + 1, i * NV + j + 1
            faces.append((a, b2, c2, d))
    B.mesh(vt, faces, top, parent, smooth=True, name='manta_top')
    B.mesh(vb, [tuple(reversed(q)) for q in faces], under, parent, smooth=True, name='manta_under')


def build_carrier(p, f):
    t = f / 4.0
    c = '#0cb89a'
    body, dk, lt = skin('carr', c), skin('carr_d', tint(c, -0.58)), skin('carr_l', tint(c, 0.55))
    g = B.glow('carr_g', '#ffb020', 2.8)
    zc = 0.47
    b = B.group('body', p, loc=(0, 0, 0.04 * wave(t, 0.35)))
    manta(b, body, lt, t, zc)
    worm([(0.74, 0, zc + 0.02), (0.40, 0, zc + 0.10), (-0.05, 0, zc + 0.12), (-0.45, 0, zc + 0.06), (-0.70, 0, zc)],
         [0.10, 0.25, 0.30, 0.18, 0.07], dk, b, wide=1.15, tall=0.75, segs=16)
    sw = 0.10 * wave(t, 0.2)
    worm([(-0.68, 0, zc), (-0.88, sw, zc + 0.04), (-1.06, -sw, zc + 0.08), (-1.22, sw * 1.5, zc + 0.16)],
         [0.08, 0.065, 0.05, 0.025], dk, b, segs=8)
    B.sphere(0.07, g, b, loc=(-1.24, sw * 1.5, zc + 0.17), segs=10)
    for s in (-1, 1):
        worm([(0.56, s * 0.20, zc + 0.02), (0.84, s * 0.30, zc + 0.02), (1.02, s * 0.22, zc + 0.00)],
             [0.10, 0.08, 0.03], body, b, segs=8, tall=0.7)
        eye(b, (0.60, s * 0.33, zc + 0.11), 0.075, g, out=(0.6, s, 0.6))
        for i in range(3):
            pulse = 1 + 0.12 * wave(t, i / 3.0 + (0.5 if s > 0 else 0.0))
            q = (0.30 - 0.29 * i, s * (0.17 + 0.02 * i), zc + 0.28 - 0.035 * i)
            B.torus(0.105, 0.035, dk, b, loc=(q[0], q[1], q[2] - 0.03), rot=(s * -18, 0, 0), segs=16, tsegs=6)
            B.sphere(0.10 * pulse, g, b, loc=q, segs=14)
    # pale marks out on the wings, which ride the wave with them
    for s in (-1, 1):
        for au, x, r in ((0.50, 0.05, 0.10), (0.72, 0.02, 0.075)):
            z = manta_z(t, au, zc) + 0.20 * (1 - au) ** 1.3 + 0.015
            B.sphere(r, lt, b, loc=(x, s * au * MANTA_SPAN, z), scale=(1.3, 1.0, 0.3), segs=12)


# ---------------------------------------------------------------------------
# The bosses (224 px cells)
#
# queen: the hive mother. A swollen abdomen of green rings under gold
# saddles, its flanks beaded with glowing eggs, that a wave runs down; six
# long legs with gold knees; a head with four eyes, gold shears and a
# crown of five gold spikes.
# ---------------------------------------------------------------------------

def build_queen(p, f):
    t = f / 4.0
    c = '#10b03c'
    body, dk, lt = skin('queen', c), skin('queen_d', tint(c, -0.6)), skin('queen_l', tint(c, 0.45))
    au = B.mat('skin_gold', '#ffbc14', rough=0.22, metal=0.65, clear=0.6)
    g = B.glow('queen_g', '#f0ff38', 2.8)
    k = 0.93
    root = B.group('all', p, loc=(-0.08, 0, 0), scale=(k, k, k))
    bob = 0.04 * math.cos(2 * PI2 * t)
    b = B.group('body', root, loc=(0, 0, bob))
    segs = [(-0.22, 1.00, 0.50), (-0.64, 1.10, 0.70), (-1.12, 1.13, 0.80), (-1.58, 1.08, 0.66), (-1.92, 1.16, 0.44),
            (-2.12, 1.36, 0.25)]
    for i, (x, z, r) in enumerate(segs):
        w = wave(t, i * 0.18)
        rr = r * (1 + 0.06 * w)
        B.sphere(rr, body, b, loc=(x, 0, z), rot=(0, -6 * i, 0), scale=(0.72 - 0.05 * w, 1.0, 0.92), segs=32)
        B.sphere(rr * 0.82, au, b, loc=(x, 0, z + rr * 0.27), rot=(0, -6 * i, 0), scale=(0.50, 0.80, 0.92), segs=24)
        if i <= 4:
            for s in (-1, 1):
                for phi in (64, 100):
                    a = math.radians(phi)
                    pulse = 1 + 0.16 * wave(t, i * 0.18 + (0.25 if phi > 80 else 0.0))
                    B.sphere(0.15 * r / 0.8 * pulse, g, b,
                             loc=(x, s * rr * math.sin(a) * 0.95, z + rr * 0.92 * math.cos(a) * 0.95), segs=14)
    worm([(-2.20, 0, 1.46), (-2.40, 0, 1.70), (-2.34, 0, 2.02)], [0.17, 0.11, 0.02], au, b, segs=10)
    B.sphere(0.50, body, b, loc=(0.42, 0, 1.02), scale=(1.2, 1.0, 0.88), segs=28)
    B.sphere(0.42, au, b, loc=(0.40, 0, 1.22), scale=(1.1, 0.85, 0.55), segs=24)
    worm([(0.80, 0, 1.08), (1.02, 0, 1.18), (1.20, 0, 1.28)], [0.27, 0.23, 0.22], dk, b, segs=12)
    h = B.group('head', b, loc=(1.32, 0, 1.36), rot=(0, 4 * wave(t, 0.15), 0), scale=(1.22, 1.22, 1.22))
    B.sphere(0.36, body, h, scale=(1.2, 1.05, 0.9), segs=28)
    B.sphere(0.22, lt, h, loc=(0.24, 0, -0.10), scale=(1.1, 1.1, 0.7), segs=16)
    op = 0.08 * wave(t, 0.3)
    for s in (-1, 1):
        eye(h, (0.26, s * 0.24, 0.10), 0.105, g, out=(0.8, s, 0.5))
        eye(h, (0.37, s * 0.095, 0.19), 0.06, g, out=(1, s * 0.3, 0.6))
        worm([(0.20, s * 0.22, -0.16), (0.54, s * (0.38 + op), -0.22), (0.78, s * (0.12 + op), -0.24)],
             [0.12, 0.10, 0.02], au, h, segs=10)
    B.sphere(0.075, g, h, loc=(0.20, 0, 0.30), segs=12)
    for a in (-66, -33, 0, 33, 66):
        ra = math.radians(a)
        L = 0.95 - 0.28 * abs(a) / 66.0
        base = Vector((-0.10, math.sin(ra) * 0.27, 0.20 + math.cos(ra) * 0.12))
        d = Vector((-0.32, math.sin(ra) * 0.92, math.cos(ra) * 0.92))
        worm([base, base + d * L * 0.5 + Vector((0.05, 0, 0)), base + d * L], [0.12, 0.085, 0.012], au, h, segs=10)
    # six legs, three at a time
    for i, (hx, hy, fx0, fy, ph) in enumerate(((0.72, 0.28, 1.00, 0.88, 0.0), (0.42, 0.40, 0.40, 1.36, 0.5),
                                               (0.12, 0.34, -0.42, 1.16, 0.0))):
        for s in (-1, 1):
            fx, fz = step(t + ph + (0.5 if s > 0 else 0.0), 0.24, 0.30)
            hip = (hx, s * hy, 0.95 + bob)
            foot = (fx0 + fx, s * fy, 0.04 + fz)
            kn = leg(root, dk, hip, foot, 0.80, 1.20, (0, s * 0.35, 1), [0.16, 0.12, 0.05], n=4, segs=10)
            B.sphere(0.16, au, root, loc=kn, segs=14)


# ---------------------------------------------------------------------------
# titan: the colossus. Steel shell over dark flesh, red where it burns
# inside: a furnace behind the ribs of its chest, a slit of an eye, its
# knees, its knuckles, the two stacks on its back. Red shoulders with bone
# spikes. It strides, swinging fists as big as its head.
# ---------------------------------------------------------------------------

def build_titan(p, f):
    t = f / 4.0
    c = '#90a6c8'
    plate = skin('titan', c, rough=0.20, metal=0.45)
    dk = B.mat('titan_d', '#2c3142', rough=0.4, clear=0.5)
    red = skin('titan_r', '#dc1c1c')
    g = B.glow('titan_g', '#ff2810', 3.0)
    hot = B.glow('titan_h%d' % f, '#ff5a18', 2.6 + 1.0 * wave(t))
    k = 0.95
    root = B.group('all', p, scale=(k, k, k))
    bob = 0.06 * math.cos(2 * PI2 * t)
    for s, ph in ((1, 0.0), (-1, 0.5)):
        fx, fz = step(t + ph, 0.36, 0.30)
        hip = Vector((0.0, s * 0.52, 2.0 + bob))
        foot = Vector((0.02 + fx, s * 0.74, 0.34 + fz))
        kn = knee(hip, foot, 0.95, 0.95, (1, 0, 0.15))
        worm([hip, kn], [0.38, 0.29], dk, root, n=1, segs=14)
        worm([kn, foot], [0.33, 0.42], plate, root, n=1, segs=16)
        B.sphere(0.25, g, root, loc=kn + Vector((0.14, 0, 0.0)), segs=14)
        B.torus(0.27, 0.07, plate, root, loc=kn + Vector((0.20, 0, 0)), rot=(0, 90, 0), segs=20, tsegs=8)
        B.sphere(0.48, plate, root, loc=foot + Vector((0.16, 0, -0.10)), scale=(1.35, 1.0, 0.5), segs=24)
        for j in (-1, 0, 1):
            worm([foot + Vector((0.58, j * 0.24, -0.14)), foot + Vector((0.86, j * 0.30, -0.28))], [0.13, 0.04], claw(),
                 root, n=1, segs=8)
    b = B.group('body', root, loc=(-0.30, 0, bob), rot=(3.0 * wave(t), 9, 0))
    B.sphere(0.62, dk, b, loc=(0, 0, 2.05), scale=(0.9, 1.25, 0.7), segs=24)
    o = B.torus(0.60, 0.075, g, b, loc=(0.03, 0, 2.36), segs=36, tsegs=8)
    o.scale = (0.92, 1.22, 1.0)
    worm([(0, 0, 2.25), (0.12, 0, 2.75), (0.16, 0, 3.15), (0.10, 0, 3.42)], [0.55, 0.92, 0.80, 0.45], plate, b,
         wide=1.30, tall=0.92, segs=24)
    # three plates lapped down its back, with fire in the seams
    for i, (x, z, r) in enumerate(((-0.34, 3.12, 0.74), (-0.44, 2.72, 0.80), (-0.38, 2.34, 0.64))):
        B.sphere(r, plate, b, loc=(x, 0, z), rot=(0, -12, 0), scale=(0.74, 1.28, 0.44), segs=28)
        if i < 2:
            B.sphere(r * 0.97, g, b, loc=(x - 0.03, 0, z - 0.21), rot=(0, -12, 0), scale=(0.74, 1.28, 0.16), segs=24)
    # the furnace in its chest, behind three pairs of ribs
    B.sphere(0.40, dk, b, loc=(0.80, 0, 2.78), segs=20)
    B.sphere(0.33, hot, b, loc=(0.90, 0, 2.78), segs=20)
    for s in (-1, 1):
        for dz in (-0.22, 0.0, 0.22):
            worm([(0.62, s * 0.62, 2.78 + dz * 1.3), (1.08, s * 0.42, 2.78 + dz * 1.1),
                  (1.22, s * 0.13, 2.78 + dz * 0.8)], [0.10, 0.085, 0.025], plate, b, segs=8)
    h = B.group('head', b, loc=(0.58, 0, 3.54), rot=(0, 12 + 3 * wave(t, 0.2), 0), scale=(1.15, 1.15, 1.15))
    B.sphere(0.37, dk, h, scale=(1.15, 1.0, 0.92), segs=24)
    B.sphere(0.34, plate, h, loc=(-0.06, 0, 0.12), scale=(1.15, 1.02, 0.72), segs=24)
    B.sphere(0.26, g, h, loc=(0.20, 0, -0.02), scale=(0.85, 1.32, 0.34), segs=16)
    B.sphere(0.22, dk, h, loc=(0.20, 0, -0.22), scale=(1.2, 1.0, 0.55), segs=16)
    worm([(0.00, 0, 0.30), (-0.30, 0, 0.52), (-0.66, 0, 0.52)], [0.13, 0.11, 0.02], red, h, segs=10, wide=0.7)
    for s in (-1, 1):
        worm([(0.0, s * 0.30, 0.10), (0.16, s * 0.52, 0.26), (0.46, s * 0.50, 0.44)], [0.12, 0.10, 0.02], bone(), h,
             segs=10)
    for s, ph in ((1, 0.0), (-1, 0.5)):
        sh = Vector((0.10, s * 1.18, 3.20))
        B.sphere(0.46, dk, b, loc=sh + Vector((0, 0, -0.12)), segs=20)
        B.sphere(0.58, red, b, loc=sh + Vector((0, s * 0.06, 0.10)), rot=(s * -14, 0, 0), scale=(1.08, 1.0, 0.60),
                 segs=28)
        o = B.torus(0.545, 0.08, plate, b, loc=sh + Vector((0, s * 0.06, 0.02)), rot=(s * -14, 0, 0), segs=32, tsegs=8)
        o.scale = (1.08, 1.0, 1.0)
        for dx, L in ((0.22, 1.0), (-0.30, 0.72)):
            base = sh + Vector((dx, s * 0.26, 0.34))
            worm([base, base + Vector((0.0, s * 0.42, 0.34)) * L, base + Vector((0.06, s * 0.52, 0.86)) * L],
                 [0.19, 0.14, 0.02], bone(), b, segs=10)
        ax = -0.30 * math.cos(PI2 * (t + ph))
        el = Vector((-0.08 + ax * 0.5, s * 1.52, 2.25))
        fist = Vector((0.35 + ax, s * 1.46, 1.40 + 0.12 * abs(ax)))
        worm([sh + Vector((0, s * 0.15, -0.25)), el], [0.30, 0.27], dk, b, n=1, segs=14)
        B.sphere(0.31, red, b, loc=el, segs=16)
        worm([el, fist], [0.30, 0.43], plate, b, n=1, segs=16)
        B.sphere(0.45, plate, b, loc=fist + Vector((0.06, 0, -0.12)), scale=(1.1, 1.0, 0.9), segs=20)
        for j in (-1, 0, 1):
            B.sphere(0.10, g, b, loc=fist + Vector((0.50, j * 0.20, -0.10)), segs=10)
        worm([(-0.55, s * 0.38, 3.25), (-0.76, s * 0.42, 3.75), (-0.82, s * 0.44, 4.08)], [0.21, 0.17, 0.14], dk, b,
             segs=10)
        B.sphere(0.135, hot, b, loc=(-0.83, s * 0.44, 4.16), segs=12)


# ---------------------------------------------------------------------------
# overmind: the brain that floats. Two violet lobes whose furrows shine,
# one great eye under a frown, held in a cage of dark ribs and three rings
# whose lights run round them; tendrils hang under it down to Z = 0.2 and
# two heavy arms reach ahead.
# ---------------------------------------------------------------------------

def brain_mat(name, c0, c1, light, k, scale=1.9):
    """the brain's skin: the level lines of a noise make meandering
    furrows, dark and sunk, that shine with 'light'"""
    if name in B.MATS:
        return B.MATS[name]
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    b = nt.nodes['Principled BSDF']
    tc = nt.nodes.new('ShaderNodeTexCoord')
    nz = nt.nodes.new('ShaderNodeTexNoise')
    nz.inputs['Scale'].default_value = scale
    nz.inputs['Detail'].default_value = 1.0
    nz.inputs['Distortion'].default_value = 1.2
    sub = nt.nodes.new('ShaderNodeMath')
    sub.operation = 'SUBTRACT'
    sub.inputs[1].default_value = 0.5
    ab = nt.nodes.new('ShaderNodeMath')
    ab.operation = 'ABSOLUTE'
    ramp = nt.nodes.new('ShaderNodeValToRGB')       # 0 in a furrow, 1 on a fold
    ramp.color_ramp.elements[0].position = 0.008
    ramp.color_ramp.elements[1].position = 0.034
    col = nt.nodes.new('ShaderNodeMixRGB')
    col.inputs['Color1'].default_value = B.lin(c1)
    col.inputs['Color2'].default_value = B.lin(c0)
    em = nt.nodes.new('ShaderNodeMixRGB')
    em.inputs['Color1'].default_value = B.lin(light)
    em.inputs['Color2'].default_value = (0, 0, 0, 1)
    bp = nt.nodes.new('ShaderNodeBump')
    bp.inputs['Strength'].default_value = 0.8
    bp.inputs['Distance'].default_value = 0.08
    L = nt.links.new
    L(tc.outputs['Object'], nz.inputs['Vector'])
    L(nz.outputs['Fac'], sub.inputs[0])
    L(sub.outputs[0], ab.inputs[0])
    L(ab.outputs[0], ramp.inputs['Fac'])
    L(ramp.outputs['Color'], col.inputs['Fac'])
    L(ramp.outputs['Color'], em.inputs['Fac'])
    L(ramp.outputs['Color'], bp.inputs['Height'])
    L(col.outputs['Color'], b.inputs['Base Color'])
    L(em.outputs['Color'], b.inputs['Emission'])
    L(bp.outputs['Normal'], b.inputs['Normal'])
    b.inputs['Emission Strength'].default_value = k
    b.inputs['Roughness'].default_value = 0.25
    b.inputs['Clearcoat'].default_value = 1.0
    B.MATS[name] = m
    return m


def build_overmind(p, f):
    t = f / 4.0
    c = '#9648ff'
    body, dk = skin('mind', c), skin('mind_d', tint(c, -0.62))
    light = '#30f2ff'
    g = B.glow('mind_g', light, 3.0)
    brain = brain_mat('mind_brain%d' % f, '#d98cff', tint(c, -0.45), light, 1.9 + 0.9 * wave(t))
    hover = 0.07 * wave(t, 0.1)
    k = 1.14        # it grows about Z = 0.2, where its lowest tendrils end
    b = B.group('body', p, loc=(0, 0, 0.2 * (1 - k) + hover), scale=(k, k, k))
    zc = 2.35
    for s in (-1, 1):
        o = B.sphere(0.80, brain, b, loc=(0.0, s * 0.33, zc), scale=(1.28, 0.80, 0.95), segs=48)
        B.displace(o, 0.10, 0.30)
    B.sphere(0.46, brain, b, loc=(-0.72, 0, 1.92), scale=(1.0, 1.3, 0.8), segs=32)
    worm([(-0.15, 0, 1.95), (-0.25, 0, 1.55), (-0.22, 0, 1.22)], [0.42, 0.30, 0.20], dk, b, segs=14)
    # the eye, looking a little up; its frown sits on the brain above it, where the
    # camera (which looks down) sees it over the eye and not across it
    B.sphere(0.52, dk, b, loc=(0.62, 0, 2.06), segs=24)
    B.sphere(0.41, g, b, loc=(0.90, 0, 2.08), segs=24)
    B.sphere(0.18, claw(), b, loc=(0.90 + 0.33 * 0.87, 0, 2.08 + 0.33 * 0.50), rot=(0, -30, 0), scale=(0.5, 0.5, 1.5),
             segs=14)
    for s in (-1, 1):
        worm([(0.60, s * 0.72, 2.68), (0.86, s * 0.40, 2.82), (0.97, s * 0.05, 2.66)], [0.09, 0.13, 0.10], dk, b,
             segs=10)
    # four claws from the stem cradle the brain
    for a in (58, 122, 238, 302):
        ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
        worm([(-0.2 + ca * 0.30, sa * 0.30, 1.42), (-0.1 + ca * 0.95, sa * 0.95, 1.50), (ca * 1.16, sa * 1.16, 1.95),
              (ca * 1.02, sa * 1.02, 2.40)], [0.15, 0.13, 0.10, 0.02], dk, b, segs=10)

    def ring(R, tube, rot, turn, n=6):
        gr = B.group('ring', b, loc=(0, 0, zc), rot=rot)
        B.torus(R, tube, dk, gr, segs=64, tsegs=10)
        for i in range(n):
            a = math.radians((i + 0.5 + turn) * 360.0 / n)
            B.sphere(tube * 1.5, g, gr, loc=(R * math.cos(a), R * math.sin(a), 0), segs=12)

    wob = 5 * wave(t)
    # two rings, both low before the eye and high behind: one nearly level, one over the crown
    ring(1.44, 0.09, (0, 30 + wob, 0), t, n=6)
    ring(1.58, 0.08, (0, 62 - wob, 0), -t, n=4)
    # the tendrils hang from the stem, and trail; they end at Z = 0.2 whatever the hover
    for i in range(8):
        a = math.radians(45 * i + 10)
        ca, sa = math.cos(a), math.sin(a)
        zend = (0.20 if i % 2 else 0.48) + (0.07 - hover) / k
        pts, rad = [], []
        for j in range(6):
            d = j / 5.0
            out = 0.14 + 0.42 * math.sin(d * 2.3)
            sw = 0.14 * d * wave(t, d * 0.6 + i / 8.0)
            pts.append((-0.22 + ca * out - 0.22 * d * d - sa * sw, sa * out + ca * sw, 1.30 - (1.30 - zend) * d))
            rad.append(0.12 - 0.085 * d)
        worm(pts, rad, body if i % 2 else dk, b, n=3, segs=8)
        B.sphere(0.065, g, b, loc=pts[-1], segs=10)
    for s in (-1, 1):
        w = 0.10 * wave(t, 0.0 if s > 0 else 0.35)
        worm([(0.10, s * 0.55, 1.78), (0.55, s * 1.00, 1.36 + w), (1.02, s * (1.06 + w), 1.02 + w),
              (1.40, s * (0.86 + w), 1.16 + 2 * w), (1.50, s * (0.62 + w), 1.44 + 2 * w)],
             [0.17, 0.14, 0.11, 0.07, 0.03], body, b, segs=10)
        B.sphere(0.075, g, b, loc=(1.50, s * (0.62 + w), 1.47 + 2 * w), segs=10)

ALIENS = {
    'grub': dict(build=build_grub, size='n'),
    'runner': dict(build=build_runner, size='n'),
    'swarm': dict(build=build_swarm, size='n'),
    'shell': dict(build=build_shell, size='n'),
    'wasp': dict(build=build_wasp, size='n', fly=True),
    'split': dict(build=build_split, size='n'),
    'shield': dict(build=build_shield, size='n'),
    'healer': dict(build=build_healer, size='n', fly=True),
    'brute': dict(build=build_brute, size='m'),
    'phase': dict(build=build_phase, size='n'),
    'jammer': dict(build=build_jammer, size='n', fly=True),
    'carrier': dict(build=build_carrier, size='m', fly=True),
    'queen': dict(build=build_queen, size='b'),
    'titan': dict(build=build_titan, size='b'),
    'overmind': dict(build=build_overmind, size='b', fly=True),
}


def main():
    out = B.arg('--out', os.path.join(B.ASSETS, 'aliens'))
    only = B.arg('--only')
    B.setup()
    B.studio()
    for aid, a in ALIENS.items():
        if only and aid not in only.split(','):
            continue
        s = SIZES[a['size']]
        build = a['build']
        B.render_sheet(aid, lambda p, c, r: build(p, r), out, 8, rows=4, cell=s['cell'], anchor=s['anchor'], yaws=YAWS,
                       meta=dict(size=a['size'], fly=bool(a.get('fly'))))


if __name__ == '__main__':
    main()
