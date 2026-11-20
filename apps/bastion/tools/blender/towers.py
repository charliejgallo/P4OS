"""
BASTION - the towers, built by code and rendered as sheets (SPEC.md).

    /Applications/Blender.app/Contents/MacOS/Blender -b -P towers.py -- [--only gatling,laser] [--tiers 1,2,3]
        [--samples 48] [--out ../../assets/towers]

Each tower has three tiers (1 the one bought, 3 fully upgraded: bigger,
more barrels, more light) and two sheets a tier:

    <id><tier>_base.png   1 cell: what does not turn (the mount on the pad)
    <id><tier>_head.png   16 cells: what turns, at yaws 0, 22.5 ... 337.5
                          (0 looks along +X, 90 away from the camera); a
                          tower that does not aim uses its 16 cells as the
                          frames of a loop instead (TOWERS[id]['spin'])

A tower is a function build_<id>(tier) returning (base, head): two
functions of a parent that build that half under it. The head is built
looking along +X and turning about the pad's centre.

The game draws the head over the base, so a head is rendered with its base
standing there as a holdout (ghost()): whatever the base hides of the head
is cut out of the head's cell, and the base's light and shadow fall on it.
"""
import math
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bs_common as B  # noqa: E402

CELL = (128, 176)
ANCHOR = (64, 136)
YAWS = [i * 22.5 for i in range(16)]

# the palette every tower shares: the fort's dark armour, and one accent a
# tower (its light, its stripe), so they read apart at a glance
ARMOR = '#3a4256'
ARMOR_L = '#5d6883'
STEEL = '#aab4c8'
DARK = '#1c2030'


def armor():
    return B.mat('armor', ARMOR, rough=0.35, metal=0.7, clear=0.3)


def armor_l():
    return B.mat('armor_l', ARMOR_L, rough=0.3, metal=0.6, clear=0.4)


def steel():
    return B.mat('steel', STEEL, rough=0.22, metal=1.0)


def dark():
    return B.mat('dark', DARK, rough=0.5, metal=0.5)


def paint(name, color):
    return B.mat('paint_' + name, color, rough=0.28, metal=0.2, clear=0.8)


def tint(color, k):
    """the colour pulled towards white by k (0..1), as '#rrggbb'"""
    c = B.rgb(color)
    return '#%02x%02x%02x' % tuple(int(round(255 * (v + (1.0 - v) * k))) for v in c)


def gl(acc, k=1.25):
    """a surface shining in the accent. Near k = 1 it keeps the accent's own
    hue; above that the Standard view transform clips the strongest channel
    and the colour drifts (orange turns yellow, violet pink), so the strong
    ones are kept for hot()"""
    return B.glow('gl_%s_%.2f' % (acc, k), acc, k)


def hot(acc, k=5.0, white=0.55):
    """the white-hot heart of a light: the accent washed out, and strong"""
    return B.glow('hot_%s_%.2f_%.2f' % (acc, k, white), tint(acc, white), k)


def xcyl(r, x0, x1, m, parent, y=0.0, z=0.0, r2=None, segs=24, bevel=0.0):
    """a cylinder lying along X from x0 to x1 (a barrel); r2 makes it a cone"""
    return B.cyl(r, x1 - x0, m, parent, loc=(x0, y, z), rot=(0, 90, 0), r2=r2, segs=segs, bevel=bevel)


def xring(R, r, m, parent, x, y=0.0, z=0.0, segs=32):
    """a ring about the X axis (a coil round a barrel)"""
    return B.torus(R, r, m, parent, loc=(x, y, z), rot=(0, 90, 0), segs=segs)


def fin(outline, t, m, parent, yaw=0.0, loc=(0, 0, 0), bevel=0.015):
    """an upright plate t thick: its outline is [(along, height), ...],
    counter clockwise, and yaw turns the plate about Z"""
    return B.prism(outline, -t / 2.0, t / 2.0, m, parent, loc=loc, rot=(90, 0, yaw), bevel=bevel)


def ghost(parent, base):
    """the base standing under a head as a holdout: it hides what it would
    hide and lights what it would light, and leaves no pixel of its own.
    It is turned back by the parent's yaw, so it stands as the base's own
    sheet shows it."""
    before = set(B.sc.objects)
    g = B.group('ghost', parent, rot=(0, 0, -math.degrees(parent.rotation_euler.z)))
    base(g)
    for o in B.sc.objects:
        if o not in before:
            o.is_holdout = True


def mount(parent, tier, accent, r=0.72, h=0.34):
    """the base most towers stand on: a low hexagonal plinth with a lit
    ring, one more step a tier"""
    B.prism(B.ngon(6, r + 0.10), 0.0, 0.10, dark(), parent, bevel=0.02)
    B.prism(B.ngon(6, r), 0.10, h, armor(), parent, bevel=0.04)
    B.torus(r * 0.80, 0.04, gl(accent, 1.4), parent, loc=(0, 0, h + 0.01))
    if tier >= 2:
        for i in range(6):
            a = math.radians(60 * i + 30)
            B.box((0.16, 0.10, 0.20), paint('m' + accent, accent), parent,
                  loc=(math.cos(a) * r * 0.88, math.sin(a) * r * 0.88, h * 0.62), rot=(0, 0, 60 * i + 30))
    if tier >= 3:
        B.prism(B.ngon(6, r * 0.62, 30), h, h + 0.12, armor_l(), parent, bevel=0.03)
    return h + (0.12 if tier >= 3 else 0.0)


def column(parent, tier, acc, top, r0=0.42, r1=0.30):
    """the mount and a hexagonal column on it up to 'top', where a head
    sits: a lit band round it, buttresses from tier 2, lamps on them at 3"""
    h = mount(parent, tier, acc)
    neck = max(h + 0.04, top - 0.16)
    B.lathe([(r0, h), (r1, neck), (r1 + 0.08, neck + 0.06), (r1 + 0.08, top)], armor_l(), parent, segs=6,
            smooth=False)

    def r_at(z):
        return r0 + (r1 - r0) * (z - h) / max(0.01, neck - h)
    if neck - h > 0.2:
        zb = h + (neck - h) * 0.5
        B.lathe([(r_at(zb) + 0.015, zb - 0.04), (r_at(zb + 0.04) + 0.015, zb + 0.04)], gl(acc), parent, segs=6,
                smooth=False)
    if tier >= 2:
        zm = h + (neck - h) * 0.8
        for a in (0, 120, 240):
            fin([(0.20, 0.30), (0.64, 0.30), (0.64, h + 0.10), (r_at(zm) + 0.04, zm), (0.20, zm)], 0.12, armor(),
                parent, yaw=a)
            if tier >= 3:
                ar = math.radians(a)
                B.sphere(0.07, gl(acc, 1.4), parent, loc=(math.cos(ar) * 0.60, math.sin(ar) * 0.60, h + 0.15))
    return top


# ---------------------------------------------------------------------------
# gatling: the cheap, fast gun. A rotary cannon on a yoke; two of them at
# tier 2, a third on top and an ammunition drum at tier 3.
# ---------------------------------------------------------------------------

def build_gatling(tier):
    acc = '#ffb020'
    top = (1.00, 1.35, 1.70)[tier - 1]

    def base(p):
        column(p, tier, acc, top)

    def head(p):
        ghost(p, base)
        z = top
        B.cyl(0.40, 0.10, dark(), p, loc=(0, 0, z), bevel=0.02)
        wide = 0.54 if tier == 1 else 0.90
        B.box((0.66, wide, 0.48), paint('gat', acc), p, loc=(-0.14, 0, z + 0.36), bevel=0.08)
        B.box((0.30, wide + 0.05, 0.14), dark(), p, loc=(-0.14, 0, z + 0.36))
        guns = [(0.0, 0.0)] if tier == 1 else [(-0.26, 0.0), (0.26, 0.0)]
        if tier >= 3:
            guns.append((0.0, 0.40))
            B.box((0.56, 0.44, 0.40), paint('gat', acc), p, loc=(-0.14, 0, z + 0.74), bevel=0.08)
        for gy, gz in guns:
            g = B.group('gun', p, loc=(0.10, gy, z + 0.38 + gz))
            xcyl(0.17, 0.0, 0.38, dark(), g, bevel=0.02)
            xcyl(0.07, 0.30, 1.20, dark(), g, segs=12)
            for k in range(6):
                a = math.radians(60 * k)
                xcyl(0.036, 0.30, 1.26, steel(), g, y=math.cos(a) * 0.095, z=math.sin(a) * 0.095, segs=10)
            xcyl(0.155, 0.70, 0.78, armor(), g)
            xcyl(0.155, 1.08, 1.16, armor(), g)
            xcyl(0.065, 1.20, 1.23, gl(acc, 1.5), g, segs=12)
        # the sight, and what feeds the guns
        ez = z + (1.02 if tier >= 3 else 0.66)
        B.sphere(0.085, gl(acc, 1.5), p, loc=(0.14, 0, ez))
        if tier == 2:
            B.box((0.30, 0.50, 0.30), armor(), p, loc=(-0.52, 0, z + 0.36), bevel=0.05)
        if tier >= 3:
            B.cyl(0.27, 0.96, armor(), p, loc=(-0.56, 0.48, z + 0.40), rot=(90, 0, 0), bevel=0.03)
            B.cyl(0.19, 1.00, paint('gat', acc), p, loc=(-0.56, 0.50, z + 0.40), rot=(90, 0, 0), bevel=0.02)

    return base, head


# ---------------------------------------------------------------------------
# cannon: one heavy barrel in a wedge of a red turret, red bands on the
# steel and fire in the muzzle brake. The barrel grows fatter a tier; tier 2
# adds lit skirts and a cupola, tier 3 a bustle at the back, recoil
# cylinders over the sleeve and an aerial.
# ---------------------------------------------------------------------------

def build_cannon(tier):
    acc = '#ff4038'
    top = (1.00, 1.35, 1.70)[tier - 1]
    s = (1.0, 1.12, 1.25)[tier - 1]
    br = (0.125, 0.15, 0.18)[tier - 1]
    tip = (1.34, 1.40, 1.44)[tier - 1]

    def base(p):
        column(p, tier, acc, top, r0=0.46, r1=0.34)

    def head(p):
        ghost(p, base)
        red = paint('can', '#f23028')
        B.cyl(0.44, 0.10, dark(), p, loc=(0, 0, top), bevel=0.02)
        t = B.group('turret', p, loc=(0, 0, top + 0.10), scale=(s, s, s))
        fin([(-0.66, 0.0), (0.26, 0.0), (0.36, 0.16), (0.16, 0.46), (-0.46, 0.50), (-0.70, 0.26)], 0.76, red, t,
            bevel=0.05)
        B.box((0.46, 0.80, 0.10), dark(), t, loc=(-0.20, 0, 0.12), bevel=0.02)
        B.box((0.16, 0.46, 0.32), dark(), t, loc=(0.28, 0, 0.24), bevel=0.04)
        x0 = 0.26 * s
        zg = top + 0.10 + 0.25 * s
        g = B.group('gun', p, loc=(x0, 0, zg), rot=(0, -7, 0))
        L = tip - x0
        xcyl(br + 0.07, 0.0, L * 0.30, armor_l(), g, bevel=0.03)
        xcyl(br, L * 0.25, L - 0.18, steel(), g)
        xcyl(br + 0.035, L * 0.44, L * 0.44 + 0.09, red, g)
        xcyl(br + 0.035, L * 0.62, L * 0.62 + 0.09, red, g)
        xcyl(br + 0.07, L - 0.22, L, dark(), g, bevel=0.03)
        xcyl(br + 0.085, L - 0.15, L - 0.07, armor_l(), g)
        xcyl(br - 0.01, L - 0.01, L + 0.008, gl(acc, 1.5), g)
        B.sphere(br * 0.5, hot(acc, 5.0), g, loc=(L, 0, 0), scale=(0.3, 1, 1))
        if tier >= 2:
            for sy in (-1, 1):
                B.box((0.62, 0.10, 0.30), armor(), t, loc=(-0.20, sy * 0.44, 0.22), bevel=0.04)
                B.box((0.36, 0.11, 0.09), gl(acc), t, loc=(-0.20, sy * 0.45, 0.24), bevel=0.0)
            B.cyl(0.17, 0.14, armor(), t, loc=(-0.26, 0.16, 0.48), bevel=0.03)
            B.sphere(0.085, gl(acc, 1.5), t, loc=(-0.26, 0.16, 0.65))
        else:
            B.sphere(0.085, gl(acc, 1.5), t, loc=(-0.22, 0, 0.53))
        if tier >= 3:
            B.box((0.30, 0.66, 0.34), armor(), t, loc=(-0.80, 0, 0.26), bevel=0.06)
            B.box((0.04, 0.46, 0.10), gl(acc), t, loc=(-0.955, 0, 0.26), bevel=0.0)
            for sy in (-1, 1):
                xcyl(0.065, 0.0, L * 0.34, steel(), g, y=sy * (br + 0.02), z=br + 0.10, segs=12)
            B.tube([(-0.44, -0.20, 0.45), (-0.52, -0.22, 1.10)], 0.03, steel(), t)
            B.sphere(0.065, gl(acc, 1.5), t, loc=(-0.52, -0.22, 1.13))

    return base, head


# ---------------------------------------------------------------------------
# laser: an eye on a mast. A pod in a fork: cooling fins behind, a shining
# lens in front; focusing prongs from tier 2, a crystal and lit rods at 3.
# ---------------------------------------------------------------------------

def build_laser(tier):
    acc = '#ff3bd4'
    top = (1.10, 1.40, 1.70)[tier - 1]
    s = (1.15, 1.28, 1.40)[tier - 1]

    def base(p):
        column(p, tier, acc, top, r0=0.38, r1=0.24)

    def head(p):
        ghost(p, base)
        B.cyl(0.32, 0.10, dark(), p, loc=(0, 0, top), bevel=0.02)
        f = B.group('fork', p, loc=(0, 0, top + 0.10), scale=(s, s, s))
        B.box((0.34, 0.86, 0.14), armor(), f, loc=(-0.05, 0, 0.07), bevel=0.04)
        for sy in (-1, 1):
            B.box((0.26, 0.11, 0.56), armor(), f, loc=(-0.05, sy * 0.375, 0.30), bevel=0.04)
            B.cyl(0.10, 0.04, paint('las', acc), f, loc=(-0.05, sy * 0.43, 0.42), rot=(-90 * sy, 0, 0))
        g = B.group('pod', f, loc=(-0.05, 0, 0.42), rot=(0, -6, 0))
        xcyl(0.17, -0.62, 0.0, dark(), g, bevel=0.03)
        for k in range(5):
            xcyl(0.29, -0.56 + k * 0.105, -0.51 + k * 0.105, steel(), g)
        xcyl(0.235, -0.04, 0.24, paint('las', acc), g, bevel=0.03)
        xcyl(0.20, 0.22, 0.64, armor_l(), g, bevel=0.02)
        xring(0.20, 0.065, dark(), g, 0.66)
        B.sphere(0.19, gl(acc, 1.3), g, loc=(0.66, 0, 0), scale=(0.62, 1, 1))
        B.sphere(0.10, hot(acc, 6.0), g, loc=(0.735, 0, 0), scale=(0.6, 1, 1))
        if tier >= 2:
            n = 3 if tier == 2 else 4
            for k in range(n):
                a = math.radians(360.0 * k / n + (90 if n == 3 else 45))
                cy, cz = math.cos(a), math.sin(a)
                B.tube([(0.26, cy * 0.24, cz * 0.24), (0.62, cy * 0.34, cz * 0.34), (0.93, cy * 0.17, cz * 0.17)],
                       0.05, steel(), g, r_end=0.04)
                B.sphere(0.07, gl(acc, 1.5), g, loc=(0.95, cy * 0.17, cz * 0.17))
        if tier >= 3:
            for sy in (-1, 1):
                xcyl(0.04, 0.24, 0.60, gl(acc, 1.4), g, y=sy * 0.14, z=0.165, segs=10)
            B.ico(0.16, gl(acc, 1.1), g, loc=(-0.30, 0, 0.46), scale=(0.7, 0.7, 1.4))
            B.cyl(0.10, 0.20, armor(), g, loc=(-0.30, 0, 0.16), bevel=0.02)

    return base, head


# ---------------------------------------------------------------------------
# missile: a rack of launch tubes tilted up, the lime noses of its missiles
# showing. 2 x 2 tubes, then 3 x 2, then two pods of 2 x 3 with a seeker
# between them.
# ---------------------------------------------------------------------------

def build_missile(tier):
    acc = '#9be830'
    top = (1.00, 1.35, 1.70)[tier - 1]

    def base(p):
        column(p, tier, acc, top)

    def pod(parent, ny, nz, loc, tilt, k=1.0):
        """a box of ny x nz tubes looking along +X, tilted up; k scales it"""
        pitch = 0.27
        w, h, ln = ny * pitch + 0.10, nz * pitch + 0.10, 0.96
        g = B.group('pod', parent, loc=loc, rot=(0, -tilt, 0), scale=(k, k, k))
        B.box((ln, w, h), armor_l(), g, loc=(0.05, 0, 0), bevel=0.05)
        B.box((0.16, w + 0.03, h + 0.03), paint('mis', acc), g, loc=(0.30, 0, 0), bevel=0.02)
        B.box((0.10, w + 0.03, h + 0.03), dark(), g, loc=(-0.30, 0, 0), bevel=0.02)
        nose = B.mat('mis_nose', tint(acc, 0.25), rough=0.3, clear=0.6, emit=acc, emit_k=0.9)
        for iy in range(ny):
            for iz in range(nz):
                y = (iy - (ny - 1) / 2.0) * pitch
                zz = (iz - (nz - 1) / 2.0) * pitch
                xcyl(0.115, 0.50, 0.535, dark(), g, y=y, z=zz, segs=16)
                xcyl(0.085, 0.53, 0.72, nose, g, y=y, z=zz, r2=0.015, segs=16)
                xcyl(0.075, -0.445, -0.43, gl(acc, 1.1), g, y=y, z=zz, segs=12)
        return g

    def head(p):
        ghost(p, base)
        z = top
        B.cyl(0.40, 0.10, dark(), p, loc=(0, 0, z), bevel=0.02)
        if tier < 3:
            ny = 2 if tier == 1 else 3
            k = 1.30 if tier == 1 else 1.22
            w = (ny * 0.27 + 0.10) * k
            for sy in (-1, 1):
                fin([(-0.36, 0.0), (0.32, 0.0), (0.14, 0.62), (-0.18, 0.62)], 0.10, armor(), p,
                    loc=(-0.05, sy * (w / 2 + 0.07), z + 0.08))
            pod(p, ny, 2, (0.0, 0, z + 0.70), 30, k)
            B.sphere(0.09, gl(acc, 1.5), p, loc=(-0.44, 0, z + 0.98))
        else:
            B.box((0.44, 0.30, 0.80), armor(), p, loc=(-0.05, 0, z + 0.48), bevel=0.06)
            B.cyl(0.11, 1.30, steel(), p, loc=(-0.02, 0.65, z + 0.72), rot=(90, 0, 0))
            for sy in (-1, 1):
                pod(p, 2, 3, (0.0, sy * 0.47, z + 0.72), 34)
            B.cyl(0.07, 0.34, steel(), p, loc=(-0.05, 0, z + 0.86))
            B.sphere(0.17, armor_l(), p, loc=(-0.05, 0, z + 1.28))
            B.torus(0.17, 0.035, gl(acc, 1.4), p, loc=(-0.05, 0, z + 1.28))
            B.sphere(0.07, gl(acc, 1.5), p, loc=(-0.05, 0, z + 1.47))

    return base, head


# ---------------------------------------------------------------------------
# flame: a wide flattened nozzle with fire in its mouth, fed by two upright
# orange tanks through hoses. The tanks grow a tier; tier 3 lights their
# gauges and puts a shield over the nozzle.
# ---------------------------------------------------------------------------

def build_flame(tier):
    acc = '#ff7a1a'
    top = (1.00, 1.35, 1.70)[tier - 1]
    tr = (0.21, 0.25, 0.29)[tier - 1]
    th = (0.62, 0.86, 1.12)[tier - 1]
    ns = (1.0, 1.12, 1.22)[tier - 1]

    def base(p):
        column(p, tier, acc, top)

    def head(p):
        ghost(p, base)
        z = top
        B.cyl(0.40, 0.10, dark(), p, loc=(0, 0, z), bevel=0.02)
        B.box((0.60, 0.46, 0.44), armor(), p, loc=(-0.04, 0, z + 0.32), bevel=0.07)
        ty = tr + 0.20
        for sy in (-1, 1):
            tk = B.group('tank', p, loc=(-0.30, sy * ty, z + 0.14))
            B.cyl(tr, th, paint('fla', acc), tk, loc=(0, 0, tr * 0.6))
            B.sphere(tr, paint('fla', acc), tk, loc=(0, 0, tr * 0.6), scale=(1, 1, 0.6))
            B.sphere(tr, paint('fla', acc), tk, loc=(0, 0, tr * 0.6 + th), scale=(1, 1, 0.6))
            B.cyl(tr + 0.02, 0.07, dark(), tk, loc=(0, 0, tr * 0.6 + th * 0.25))
            B.cyl(tr + 0.02, 0.07, dark(), tk, loc=(0, 0, tr * 0.6 + th * 0.75))
            B.cyl(0.08, 0.12, steel(), tk, loc=(0, 0, tr * 1.2 + th - 0.02))
            zt = tr * 1.2 + th + 0.08
            B.tube([(0, 0, zt), (0.10, -sy * 0.04, zt + 0.12), (0.34, -sy * (ty - 0.10), zt - 0.02),
                    (0.50, -sy * (ty - 0.10), 0.42 - 0.14 + 0.10)], 0.05, dark(), tk)
            if tier >= 3:
                B.box((0.05, 0.12, th * 0.42), gl(acc, 1.4), tk, loc=(tr, 0, tr * 0.6 + th * 0.5), bevel=0.0)
        g = B.group('nozzle', p, loc=(0.20, 0, z + 0.40), rot=(0, -8, 0), scale=(ns, ns, ns))
        xcyl(0.12, 0.0, 0.50, steel(), g)
        xcyl(0.16, 0.10, 0.22, dark(), g)
        m = B.group('mouth', g, loc=(0.42, 0, 0), scale=(1.0, 1.55, 0.80))
        xcyl(0.13, 0.0, 0.46, armor_l(), m, r2=0.33)
        xring(0.33, 0.05, paint('fla', acc), m, 0.46)
        xcyl(0.30, 0.455, 0.47, gl(acc, 1.4), m)
        B.sphere(0.17, hot('#ffd040', 5.0, 0.3), m, loc=(0.46, 0, 0), scale=(0.5, 1, 1))
        if tier >= 2:
            xcyl(0.05, 0.10, 0.84, steel(), g, z=-0.30, segs=10)
            B.sphere(0.07, hot('#ffd040', 5.0, 0.3), g, loc=(0.88, 0, -0.30))
        if tier >= 3:
            B.box((0.50, 0.90, 0.07), armor(), g, loc=(0.52, 0, 0.36), rot=(0, -14, 0), bevel=0.02)
            B.box((0.12, 0.92, 0.08), paint('fla', acc), g, loc=(0.70, 0, 0.405), rot=(0, -14, 0), bevel=0.02)

    return base, head


# ---------------------------------------------------------------------------
# rail: a very long gun. Two rails with a line of light between them, coils
# round them, and a violet capacitor block behind to balance it: more coils
# and more reach a tier, capacitor banks on the sides from tier 2.
# ---------------------------------------------------------------------------

def build_rail(tier):
    acc = '#a070ff'
    top = (1.00, 1.35, 1.70)[tier - 1]
    tip = (1.24, 1.34, 1.42)[tier - 1]
    ncoil = (2, 3, 4)[tier - 1]

    def base(p):
        column(p, tier, acc, top)

    def head(p):
        ghost(p, base)
        z = top
        B.cyl(0.40, 0.10, dark(), p, loc=(0, 0, z), bevel=0.02)
        B.box((0.50, 0.50, 0.30), armor(), p, loc=(-0.05, 0, z + 0.24), bevel=0.06)
        g = B.group('gun', p, loc=(0, 0, z + 0.52), rot=(0, -4, 0))
        back = -0.50
        ln = tip - back
        for sy in (-1, 1):
            B.box((ln, 0.15, 0.20), steel(), g, loc=(back + ln / 2, sy * 0.20, 0), bevel=0.03)
            B.box((0.20, 0.17, 0.24), armor(), g, loc=(tip - 0.09, sy * 0.20, 0), bevel=0.02)
        B.box((ln - 0.04, 0.26, 0.10), gl(acc, 1.4), g, loc=(back + ln / 2, 0, 0.07), bevel=0.0)
        x0, x1 = 0.20, tip - 0.36
        for k in range(ncoil):
            x = x0 + (x1 - x0) * k / max(1, ncoil - 1)
            B.box((0.20, 0.70, 0.42), armor(), g, loc=(x, 0, 0), bevel=0.05)
            B.box((0.06, 0.72, 0.30), gl(acc, 1.3), g, loc=(x, 0, 0), bevel=0.0)
        B.box((0.52, 0.60, 0.46), paint('rai', acc), g, loc=(-0.52, 0, 0.02), bevel=0.08)
        B.box((0.30, 0.64, 0.12), dark(), g, loc=(-0.52, 0, 0.02))
        B.sphere(0.08, gl(acc, 1.5), g, loc=(-0.40, 0, 0.30))
        if tier >= 2:
            for sy in (-1, 1):
                xcyl(0.13, -0.80, -0.22, armor(), g, y=sy * 0.40, z=-0.04, bevel=0.03)
                xcyl(0.135, -0.60, -0.42, gl(acc, 1.3), g, y=sy * 0.40, z=-0.04)
        if tier >= 3:
            for sy in (-1, 1):
                xcyl(0.11, -0.74, -0.24, armor(), g, y=sy * 0.22, z=0.32, bevel=0.03)
                xcyl(0.115, -0.56, -0.42, gl(acc, 1.3), g, y=sy * 0.22, z=0.32)
                B.box((0.07, 0.20, 0.34), gl(acc, 1.3), g, loc=(tip - 0.20, sy * 0.19, 0), bevel=0.0)

    return base, head


# ---------------------------------------------------------------------------
# mortar: a fat short barrel looking up at 60 degrees between two teal
# cheeks, plasma shining in its mouth. Fatter a tier; struts at tier 2, lit
# rings and plasma cells at tier 3.
# ---------------------------------------------------------------------------

def build_mortar(tier):
    acc = '#2ee6c0'
    top = (0.80, 1.05, 1.30)[tier - 1]
    R = (0.30, 0.34, 0.39)[tier - 1]
    L = (0.90, 1.00, 1.12)[tier - 1]

    def base(p):
        column(p, tier, acc, top, r0=0.50, r1=0.40)

    def head(p):
        ghost(p, base)
        z = top
        B.cyl(0.52, 0.10, dark(), p, loc=(0, 0, z), bevel=0.02)
        zp = z + 0.10 + R + 0.14
        for sy in (-1, 1):
            fin([(-0.40, 0.0), (0.40, 0.0), (0.16, R + 0.34), (-0.16, R + 0.34)], 0.12, paint('mor', acc), p,
                loc=(0, sy * (R + 0.10), z + 0.10))
            B.cyl(0.12, 0.05, steel(), p, loc=(0, sy * (R + 0.16), zp), rot=(-90 * sy, 0, 0))
        g = B.group('tube', p, loc=(0, 0, zp), rot=(0, -60, 0))
        xcyl(R, -0.26, L, armor(), g, bevel=0.03)
        B.sphere(R * 1.04, dark(), g, loc=(-0.26, 0, 0), scale=(0.7, 1, 1))
        xcyl(R + 0.04, 0.10, 0.24, armor_l(), g, bevel=0.02)
        xcyl(R + 0.05, L - 0.16, L + 0.02, armor_l(), g, bevel=0.03)
        xcyl(R - 0.04, L + 0.012, L + 0.026, gl(acc, 1.4), g)
        B.sphere(R * 0.55, hot(acc, 5.0), g, loc=(L + 0.02, 0, 0), scale=(0.35, 1, 1))
        if tier >= 2:
            for sy in (-1, 1):
                B.tube([(0.62, sy * 0.30, z + 0.12), (0.30, sy * (R + 0.02), zp + 0.30)], 0.05, steel(), p)
            xring(R + 0.03, 0.04, gl(acc, 1.4), g, L * 0.52)
        if tier >= 3:
            xring(R + 0.03, 0.04, gl(acc, 1.4), g, L * 0.70)
            for sy in (-1, 0, 1):
                B.cyl(0.11, 0.34, armor_l(), p, loc=(-0.44, sy * 0.25, z + 0.10), bevel=0.02)
                B.cyl(0.085, 0.20, gl(acc, 1.3), p, loc=(-0.44, sy * 0.25, z + 0.34))
                B.sphere(0.085, gl(acc, 1.3), p, loc=(-0.44, sy * 0.25, z + 0.54))

    return base, head


# ---------------------------------------------------------------------------
# The towers that do not aim. Their head is head(parent, phase): the 16
# cells are the frames of a loop, phase 0..1, and frame 16 would be frame 0
# again. What stands still is in the base, so it does not shimmer from frame
# to frame; ghost() lets the moving parts pass behind it.
# ---------------------------------------------------------------------------

def bolt(parent, a, b, rng, m, r=0.035, n=5, amp=0.10):
    """a jagged spark from a to b"""
    pts = [tuple(a)]
    for i in range(1, n):
        t = i / float(n)
        pts.append(tuple(a[j] + (b[j] - a[j]) * t + rng.uniform(-amp, amp) for j in range(3)))
    pts.append(tuple(b))
    return B.tube(pts, r, m, parent, segs=6)


def veil(name, color, alpha, k=2.0):
    """light with nothing behind it (a beam, a wave leaving a tower): it
    shines and lets the ground show through, 'alpha' of it"""
    a = max(0.05, min(1.0, round(alpha * 10) / 10.0))
    return B.mat('veil_%s_%.1f' % (name, a), color, rough=1.0, spec=0.0, emit=color, emit_k=k, alpha=a)


# ---------------------------------------------------------------------------
# tesla: a coil that narrows upwards under a steel toroid, and a ball of
# light on top that beats twice a loop and throws sparks: at the toroid,
# into the air, and from tier 2 at the small pylons standing round it.
# ---------------------------------------------------------------------------

def build_tesla(tier):
    acc = '#40b8ff'
    zt = (1.50, 1.95, 2.40)[tier - 1]
    rb = (0.24, 0.28, 0.33)[tier - 1]
    zb = zt + 0.15 + rb
    pyl = [(), (0, 180), (30, 150, 270)][tier - 1]
    rp = 0.64
    zp = (0.0, 1.10, 1.50)[tier - 1]

    def base(p):
        h = mount(p, tier, acc)
        z0, z1 = h + 0.16, zt - 0.10
        B.lathe([(0.52, h), (0.45, z0), (0.21, z1), (0.21, zt)], dark(), p, segs=24)
        B.torus(0.50, 0.08, steel(), p, loc=(0, 0, h + 0.09))
        n = (7, 9, 11)[tier - 1]
        for k in range(n):
            z = z0 + 0.08 + (z1 - z0 - 0.26) * k / (n - 1.0)
            r = 0.45 + (0.21 - 0.45) * (z - z0) / (z1 - z0)
            B.torus(r + 0.012, 0.055, gl(acc, 1.2) if k % 3 == 1 else steel(), p, loc=(0, 0, z))
        B.torus(0.38, 0.13, steel(), p, loc=(0, 0, zt))
        B.cyl(0.10, 0.22, dark(), p, loc=(0, 0, zt))
        for a in pyl:
            x, y = math.cos(math.radians(a)) * rp, math.sin(math.radians(a)) * rp
            B.cyl(0.12, zp - 0.44, armor_l(), p, loc=(x, y, 0.30), r2=0.07)
            B.torus(0.14, 0.05, steel(), p, loc=(x, y, zp - 0.15))
            B.sphere(0.125, gl(acc, 1.2), p, loc=(x, y, zp))

    def head(p, phase):
        ghost(p, base)
        f = int(round(phase * 16)) % 16
        rng = random.Random(977 * tier + f)
        beat = round(0.5 + 0.5 * math.cos(2 * math.pi * 2 * phase), 2)
        B.sphere(rb * (1.0 + 0.07 * beat), hot(acc, 1.2 + 2.8 * beat, 0.05 + 0.60 * beat), p, loc=(0, 0, zb))
        white = hot(acc, 6.0, 0.80)
        ends = []
        for _ in range(rng.choice((1, 2, 2)) if tier == 1 else rng.choice((1, 1, 2))):
            a = rng.uniform(0, 2 * math.pi)
            ends.append((math.cos(a) * 0.50, math.sin(a) * 0.46, zt + rng.uniform(-0.04, 0.06)))
        for _ in range(rng.choice((0, 1, 1)) + (1 if tier >= 3 else 0)):
            a = rng.uniform(0, 2 * math.pi)
            e = rng.uniform(-0.1, 0.9)
            ln = rb + rng.uniform(0.30, 0.55)
            ends.append((math.cos(a) * math.cos(e) * ln, math.sin(a) * math.cos(e) * ln, zb + math.sin(e) * ln))
        for a in pyl:
            if rng.random() < 0.6:
                ends.append((math.cos(math.radians(a)) * rp, math.sin(math.radians(a)) * rp, zp + 0.09))
        for e in ends:
            d = (e[0], e[1], e[2] - zb)
            ln = math.sqrt(d[0] ** 2 + d[1] ** 2 + d[2] ** 2)
            st = (d[0] / ln * rb * 0.9, d[1] / ln * rb * 0.9, zb + d[2] / ln * rb * 0.9)
            bolt(p, st, e, rng, white, r=0.045, n=4 if ln < 0.7 else 6, amp=0.11)

    return base, head


# ---------------------------------------------------------------------------
# cryo: a spire of ice in a claw, shards of ice round its foot, and a ring
# of vents breathing frost that turns about the spire (a step of its
# symmetry or two a loop). Tier 3 has a second, smaller ring turning the
# other way.
# ---------------------------------------------------------------------------

def crystal(parent, r, h, m, loc, rot=(0, 0, 0)):
    """a six-sided crystal standing on loc, r wide and h tall"""
    return B.lathe([(r * 0.6, 0.0), (r, h * 0.22), (r * 0.8, h * 0.70), (0.0, h)], m, parent, loc=loc, rot=rot,
                   segs=6, smooth=False)


def build_cryo(tier):
    acc = '#a8f4ff'
    top = (0.80, 0.95, 1.10)[tier - 1]
    hs = (1.50, 1.85, 2.10)[tier - 1]
    rs = (0.27, 0.30, 0.34)[tier - 1]

    def ice():
        return B.mat('ice', '#7fdcf2', rough=0.10, clear=1.0, emit=acc, emit_k=0.45)

    def base(p):
        column(p, tier, acc, top, r0=0.44, r1=0.30)
        rng = random.Random(40 + tier)
        n = (5, 7, 9)[tier - 1]
        for k in range(n):
            a = 360.0 * k / n + rng.uniform(-10, 10) + 20
            r = rng.uniform(0.44, 0.56)
            hh = rng.uniform(0.45, 0.68) * (1.0 + 0.15 * (tier - 1))
            ar = math.radians(a)
            crystal(p, rng.uniform(0.12, 0.17), hh, ice(), (math.cos(ar) * r, math.sin(ar) * r, 0.30),
                    rot=(0, rng.uniform(10, 20), a))
        for a in (60, 180, 300):
            fin([(0.20, top - 0.04), (0.44, top - 0.04), (0.36, top + 0.40), (0.24, top + 0.40)], 0.10, armor(), p,
                yaw=a)
        crystal(p, rs, hs, ice(), (0, 0, top - 0.04), rot=(0, 0, 30))

    def ring(p, R, n, z, ang):
        g = B.group('ring', p, loc=(0, 0, z), rot=(0, 0, ang))
        B.torus(R, 0.065, armor_l(), g)
        for k in range(n):
            v = B.group('vent', g, rot=(0, 0, 360.0 * k / n))
            B.tube([(rs + 0.02, 0, 0.0), (R, 0, 0)], 0.035, steel(), v, segs=8)
            nz = B.group('nozzle', v, loc=(R, 0, 0), rot=(0, 22, 0))
            B.box((0.32, 0.27, 0.21), armor_l(), nz, loc=(0.04, 0, 0), bevel=0.04)
            B.box((0.06, 0.24, 0.18), gl(acc, 1.15), nz, loc=(0.195, 0, 0), bevel=0.0)
            xcyl(0.09, 0.22, 0.50, veil('cryo', tint(acc, 0.2), 0.5, 1.5), nz, r2=0.17, segs=16)
        B.torus(rs + 0.03, 0.045, armor(), g)

    def head(p, phase):
        ghost(p, base)
        if tier == 1:
            ring(p, 0.56, 4, top + 0.52, 90.0 * phase)
        elif tier == 2:
            ring(p, 0.60, 6, top + 0.56, 120.0 * phase)
        else:
            ring(p, 0.62, 6, top + 0.58, 120.0 * phase)
            ring(p, 0.44, 3, top + 1.30, -120.0 * phase)

    return base, head


# ---------------------------------------------------------------------------
# drone: a hexagonal landing deck round a mast, its lights lit and drones
# parked on it; a second deck at tier 2 and a third at tier 3, the mast
# taller each time. On the mast a two-sided beacon turns (half a turn a loop).
# ---------------------------------------------------------------------------

def build_drone(tier):
    acc = '#ff6fa8'
    decks = [(0.78, 0.80), (0.54, 1.52), (0.38, 2.18)][:tier]
    zm = (1.80, 2.45, 3.00)[tier - 1]

    def quad(parent, loc, yaw):
        g = B.group('quad', parent, loc=loc, rot=(0, 0, yaw))
        B.box((0.24, 0.24, 0.11), paint('dro', acc), g, loc=(0, 0, 0.12), bevel=0.04)
        for sx in (-1, 1):
            for sy in (-1, 1):
                B.cyl(0.115, 0.035, dark(), g, loc=(sx * 0.16, sy * 0.16, 0.13))
        B.sphere(0.05, gl(acc, 1.4), g, loc=(0.07, 0, 0.19))

    def base(p):
        h = mount(p, tier, acc)
        B.lathe([(0.42, h), (0.26, h + 0.20), (0.26, decks[0][1] - 0.10)], armor_l(), p, segs=6, smooth=False)
        B.cyl(0.11, zm - decks[0][1], armor_l(), p, loc=(0, 0, decks[0][1]))
        for i, (R, z) in enumerate(decks):
            B.lathe([(0.13, z - 0.44), (R * 0.72, z - 0.12)], armor(), p, segs=6, smooth=False, rot=(0, 0, 30))
            B.prism(B.ngon(6, R, 30), z - 0.13, z, armor(), p, bevel=0.03)
            B.prism(B.ngon(6, R - 0.07, 30), z - 0.01, z + 0.012, dark(), p, bevel=0.0)
            B.torus(R * 0.60, 0.03, gl(acc, 1.3), p, loc=(0, 0, z + 0.02))
            for k in range(6):
                a = math.radians(60 * k + 30)
                B.sphere(0.055, gl(acc, 1.4), p, loc=(math.cos(a) * (R - 0.05), math.sin(a) * (R - 0.05), z + 0.02))
            B.cyl(0.17, 0.10, dark(), p, loc=(0, 0, z), bevel=0.02)
        quad(p, (0.34, -0.36, decks[0][1]), 20)
        if tier >= 2:
            quad(p, (-0.42, -0.26, decks[0][1]), -30)
            quad(p, (0.0, -0.32, decks[1][1]), 0)
        B.cyl(0.15, 0.10, dark(), p, loc=(0, 0, zm - 0.02), bevel=0.02)

    def head(p, phase):
        ghost(p, base)
        s = (1.15, 1.25, 1.35)[tier - 1]
        g = B.group('lamp', p, loc=(0, 0, zm + 0.08 + 0.17 * s), rot=(0, 0, 180.0 * phase), scale=(s, s, s))
        B.sphere(0.14, hot(acc, 4.0, 0.5), g)
        B.cyl(0.16, 0.05, dark(), g, loc=(0, 0, 0.12))
        B.cyl(0.16, 0.05, dark(), g, loc=(0, 0, -0.17))
        for sx in (-1, 1):
            B.cyl(0.12, 0.20, armor(), g, loc=(sx * 0.10, 0, 0), rot=(0, 90 * sx, 0), r2=0.17)
            B.cyl(0.155, 0.025, gl(acc, 1.4), g, loc=(sx * 0.30, 0, 0), rot=(0, 90 * sx, 0))
            B.cyl(0.13, 0.26, veil('dro', acc, 0.5), g, loc=(sx * 0.33, 0, 0), rot=(0, 90 * sx, 0), r2=0.18)
            B.cyl(0.18, 0.30, veil('dro', acc, 0.2), g, loc=(sx * 0.59, 0, 0), rot=(0, 90 * sx, 0), r2=0.25)

    return base, head


# ---------------------------------------------------------------------------
# emp: a heavy ring held up by three claws over a core. Once a loop the core
# swells and a wave of light leaves it, crosses the ring and fades in the
# air beyond; a second wave follows half a loop behind. Tier 3 carries a
# second ring higher up.
# ---------------------------------------------------------------------------

def build_emp(tier):
    acc = '#5a6cff'
    zr = (1.45, 1.80, 2.05)[tier - 1]
    R = (0.56, 0.62, 0.66)[tier - 1]
    R2, z2 = 0.40, zr + 0.66

    def base(p):
        h = mount(p, tier, acc)
        B.lathe([(0.40, h), (0.30, h + 0.20), (0.15, zr - 0.46), (0.15, zr - 0.30)], armor_l(), p, segs=6,
                smooth=False)
        B.torus(0.22, 0.05, gl(acc, 1.2), p, loc=(0, 0, h + 0.40))
        for a in (90, 210, 330):
            fin([(0.34, 0.30), (0.62, 0.30), (R + 0.16, zr * 0.55), (R + 0.12, zr + 0.11), (R - 0.05, zr + 0.11),
                 (R - 0.03, zr - 0.12), (R + 0.02, zr * 0.55), (0.34, h + 0.26)], 0.13, armor(), p, yaw=a)
            ar = math.radians(a)
            B.sphere(0.07, gl(acc, 1.4), p, loc=(math.cos(ar) * (R + 0.04), math.sin(ar) * (R + 0.04), zr + 0.15))
            if tier >= 3:
                fin([(R - 0.02, zr + 0.06), (R + 0.10, zr + 0.06), (R2 + 0.10, z2 + 0.08), (R2 - 0.02, z2 + 0.08)],
                    0.10, armor(), p, yaw=a)
        B.torus(R, 0.09, armor_l(), p, loc=(0, 0, zr))
        for a in (30, 150, 270):
            ar = math.radians(a)
            B.box((0.20, 0.30, 0.22), paint('emp', acc), p, loc=(math.cos(ar) * R, math.sin(ar) * R, zr),
                  rot=(0, 0, a), bevel=0.04)
        if tier >= 3:
            B.torus(R2, 0.075, armor_l(), p, loc=(0, 0, z2))

    def wave(p, z, r0, r1, u, tube):
        r = r0 + (r1 - r0) * u
        fade = min(1.0, (1.0 - u) * 2.0)
        B.torus(r, tube * (0.60 + 0.40 * (1.0 - u)), veil('emp', tint(acc, 0.10), fade, 1.5),
                p, loc=(0, 0, z), segs=48, tsegs=8)

    def head(p, phase):
        ghost(p, base)
        beat = round(0.5 + 0.5 * math.cos(2 * math.pi * phase), 2)
        cs = (1.0, 1.12, 1.25)[tier - 1]
        B.sphere(0.19 * cs * (1.0 + 0.22 * beat), hot(acc, 1.6 + 2.6 * beat, 0.10 + 0.55 * beat), p,
                 loc=(0, 0, zr - 0.14))
        B.torus(R - 0.085, 0.04, gl(acc, round(1.0 + 0.5 * beat, 1)), p, loc=(0, 0, zr + 0.02))
        for i in range(2):
            wave(p, zr - 0.02, 0.24, 1.22, (phase + i / 2.0) % 1.0, 0.055)
        if tier >= 3:
            b2 = round(0.5 + 0.5 * math.cos(2 * math.pi * (phase + 0.5)), 2)
            B.torus(R2 - 0.07, 0.035, gl(acc, round(1.0 + 0.5 * b2, 1)), p, loc=(0, 0, z2 + 0.02))
            B.sphere(0.12 * (1.0 + 0.25 * b2), hot(acc, 1.6 + 2.6 * b2, 0.10 + 0.55 * b2), p, loc=(0, 0, z2 + 0.02))
            for i in range(2):
                wave(p, z2, 0.14, 0.86, (phase + 0.25 + i / 2.0) % 1.0, 0.045)

    return base, head


# ---------------------------------------------------------------------------
# gravity: a black ball in a fork, and rings tumbling round it, each about
# its own axis and half a turn a loop (a ring turned half way round is the
# same ring). Tier 3 has a third ring and a crown of shards circling above.
# ---------------------------------------------------------------------------

def build_gravity(tier):
    acc = '#c040ff'
    z0 = (0.80, 1.05, 1.35)[tier - 1]
    R = (0.56, 0.60, 0.64)[tier - 1]
    zc = z0 + R + 0.16
    rs = (0.27, 0.30, 0.31)[tier - 1]

    def base(p):
        column(p, tier, acc, z0, r0=0.44, r1=0.30)
        for a in (0, 180):
            fin([(0.14, z0 - 0.02), (0.44, z0 - 0.02), (R + 0.20, zc - 0.32), (R + 0.20, zc + 0.16),
                 (R + 0.07, zc + 0.16), (R + 0.07, zc - 0.22), (0.30, z0 + 0.28), (0.14, z0 + 0.28)], 0.16, armor(), p,
                yaw=a)
            sx = 1 if a == 0 else -1
            B.box((0.10, 0.20, 0.30), paint('gra', acc), p, loc=(sx * (R + 0.21), 0, zc - 0.04), bevel=0.03)
            B.sphere(0.085, gl(acc, 1.3), p, loc=(sx * (R + 0.135), 0, zc + 0.22))
            if tier >= 3:
                crystal(p, 0.07, 0.34, gl(acc, 1.15), (sx * (R + 0.135), 0, zc + 0.24))

    def band(parent, R_, r, lit):
        """a ring: steel with a lit stripe on both faces, or all light"""
        if lit:
            B.torus(R_, r, gl(acc, 1.2), parent, segs=48)
        else:
            B.torus(R_, r, armor_l(), parent, segs=48)
            for sz in (-1, 1):
                B.torus(R_, r * 0.60, gl(acc, 1.2), parent, loc=(0, 0, sz * r * 0.62), segs=48)

    def head(p, phase):
        ghost(p, base)
        c = B.group('centre', p, loc=(0, 0, zc))
        B.sphere(rs, B.mat('void', '#030106', rough=0.06, clear=1.0, spec=0.8), c)
        ga = B.group('a', c, rot=(25 + 180.0 * phase, 0, 0))
        band(ga, R, 0.065, False)
        gb = B.group('b', c, rot=(0, 70 + 180.0 * phase, 0))
        band(gb, R - 0.14, 0.055, True)
        if tier >= 3:
            gc = B.group('c', c, rot=(90, 0, 180.0 * phase + 40))
            band(gc, R - 0.26, 0.045, False)
            for k in range(3):
                a = math.radians(120 * k + 120 * phase)
                crystal(p, 0.07, 0.30, gl(acc, 1.15), (math.cos(a) * 0.30, math.sin(a) * 0.30, zc + R + 0.14))
        else:
            gd = B.group('disc', c, rot=(18, 0, 360.0 * phase))
            B.torus(rs + 0.09, 0.035, hot(acc, 3.0, 0.45), gd, segs=48)

    return base, head


# ---------------------------------------------------------------------------
# radar: a dish on a yoke, turning once a loop: dark behind, white inside
# with green rings and a shining feed at its focus. Bigger a tier; whips
# with lit tips behind it from tier 2, lit side panels at tier 3.
# ---------------------------------------------------------------------------

def build_radar(tier):
    acc = '#38ff9c'
    top = (0.95, 1.25, 1.50)[tier - 1]
    ds = (0.92, 1.10, 1.28)[tier - 1]

    def base(p):
        column(p, tier, acc, top, r0=0.42, r1=0.28)

    def head(p, phase):
        ghost(p, base)
        g = B.group('turn', p, loc=(0, 0, top), rot=(0, 0, 360.0 * phase))
        B.cyl(0.36, 0.10, dark(), g, bevel=0.02)
        zp = 0.10 + 0.50 * ds
        for sy in (-1, 1):
            fin([(-0.24, 0.08), (0.24, 0.08), (0.11, zp + 0.10), (-0.11, zp + 0.10)], 0.10, armor(), g,
                loc=(0, sy * 0.30 * ds, 0))
        B.cyl(0.07, 0.66 * ds, steel(), g, loc=(0, 0.33 * ds, zp), rot=(90, 0, 0))
        d = B.group('dish', g, loc=(0, 0, zp), rot=(0, 55, 0), scale=(ds, ds, ds))
        B.cyl(0.19, 0.20, armor_l(), d, loc=(0, 0, -0.04), bevel=0.03)
        q = B.group('bowl', d, loc=(0, 0, 0.14))
        B.lathe([(0.0, 0.0), (0.25, 0.03), (0.50, 0.14), (0.68, 0.30), (0.66, 0.33), (0.48, 0.19), (0.24, 0.09),
                 (0.0, 0.06)], armor(), q, segs=40)
        B.lathe([(0.0, 0.062), (0.24, 0.092), (0.48, 0.192), (0.645, 0.322), (0.645, 0.327), (0.48, 0.197),
                 (0.24, 0.097), (0.0, 0.067)], paint('rad_w', '#e4ecf2'), q, segs=40)
        B.torus(0.67, 0.04, gl(acc, 1.3), q, loc=(0, 0, 0.32), segs=48)
        B.torus(0.36, 0.035, gl(acc, 1.2), q, loc=(0, 0, 0.15), segs=40)
        B.torus(0.50, 0.05, paint('rad', acc), q, loc=(0, 0, 0.115), segs=40)
        for k in range(3):
            a = math.radians(90 + 120 * k)
            B.tube([(math.cos(a) * 0.62, math.sin(a) * 0.62, 0.30), (math.cos(a) * 0.05, math.sin(a) * 0.05, 0.68)],
                   0.03, steel(), q, segs=8)
        B.sphere(0.10, hot(acc, 4.0, 0.45), q, loc=(0, 0, 0.70))
        if tier >= 2:
            for sy in ((1,) if tier == 2 else (-1, 1)):
                B.tube([(-0.26, sy * 0.20, 0.10), (-0.34, sy * 0.22, zp + 0.55 * ds)], 0.03, steel(), g, segs=8)
                B.sphere(0.065, gl(acc, 1.4), g, loc=(-0.34, sy * 0.22, zp + 0.58 * ds))
        if tier >= 3:
            for sy in (-1, 1):
                B.box((0.40, 0.08, 0.30), armor_l(), g, loc=(0, sy * (0.30 * ds + 0.09), 0.34), bevel=0.03)
                B.box((0.26, 0.09, 0.08), gl(acc, 1.3), g, loc=(0, sy * (0.30 * ds + 0.10), 0.36), bevel=0.0)

    return base, head


# ---------------------------------------------------------------------------
# bank: a small refinery that makes credits. A gold tank, a stack, and in
# front of them a pump whose beam nods once a loop, driven by a crank; lit
# rings climb the stack and a coin turns over the tank. Tier 2 lights a
# flare on the stack; everything is bigger a tier.
# ---------------------------------------------------------------------------

def build_bank(tier):
    acc = '#ffd83a'
    k = (1.0, 1.15, 1.30)[tier - 1]
    th = (0.70, 0.95, 1.20)[tier - 1]
    zs = (1.80, 2.35, 2.90)[tier - 1]
    tx, ty, tr = -0.30, 0.24, 0.30 * k
    sx, sy = 0.44, 0.34
    px, py = 0.06, -0.36

    def gold():
        return paint('ban', '#f2c21c')

    def base(p):
        h = mount(p, tier, acc)
        B.cyl(tr, th, gold(), p, loc=(tx, ty, h), bevel=0.03)
        B.sphere(tr, gold(), p, loc=(tx, ty, h + th), scale=(1, 1, 0.5))
        for kz in (0.28, 0.74):
            B.cyl(tr + 0.02, 0.07, dark(), p, loc=(tx, ty, h + th * kz))
        B.box((0.11, 0.06, th * 0.34), gl(acc, 1.3), p, loc=(tx, ty - tr - 0.01, h + th * 0.51), bevel=0.0)
        B.cyl(0.045, 0.16, steel(), p, loc=(tx, ty, h + th + tr * 0.45))
        B.cyl(0.21, 0.26, armor(), p, loc=(sx, sy, h), bevel=0.03)
        B.cyl(0.12, zs - h, armor_l(), p, loc=(sx, sy, h))
        B.cyl(0.21, 0.14, dark(), p, loc=(sx, sy, zs - 0.12), bevel=0.03)
        zt = h + th + tr * 0.2
        B.tube([(tx + tr * 0.5, ty, zt), (tx + tr * 0.5 + 0.10, ty + 0.02, zt + 0.22), (sx - 0.16, sy, zt + 0.22),
                (sx, sy, zt + 0.10)], 0.05, steel(), p)
        fin([(-0.26, 0.0), (0.26, 0.0), (0.07, 0.76 * k), (-0.07, 0.76 * k)], 0.14, armor(), p, loc=(px, py, h))
        B.cyl(0.10, 0.18, dark(), p, loc=(px + 0.52 * k + 0.12, py, h), bevel=0.02)
        B.box((0.22, 0.22, 0.26), armor(), p, loc=(px - 0.50 * k, py + 0.04, h + 0.13), bevel=0.04)

    def head(p, phase):
        ghost(p, base)
        h = 0.34 + (0.12 if tier >= 3 else 0.0)
        w = 2 * math.pi * phase
        a = math.radians(13.0 * math.sin(w))
        zpv = h + 0.78 * k
        g = B.group('beam', p, loc=(px, py, zpv), rot=(0, math.degrees(a), 0))
        B.box((1.04 * k, 0.14, 0.14), armor(), g, loc=(0.02 * k, 0, 0.03), bevel=0.03)
        B.box((0.50 * k, 0.15, 0.06), gl(acc, 1.2), g, loc=(0.05 * k, 0, 0.04), bevel=0.0)
        fin([(0.0, -0.30), (0.17, -0.25), (0.21, 0.12), (0.0, 0.18)], 0.16, steel(), g, loc=(0.52 * k, 0, 0))
        B.box((0.20, 0.22, 0.22), dark(), g, loc=(-0.44 * k, 0, 0.0), bevel=0.04)
        B.cyl(0.07, 0.16, steel(), g, loc=(0, 0.08, 0.02), rot=(90, 0, 0))

        def world(x, z):
            return (px + x * math.cos(a) + z * math.sin(a), zpv - x * math.sin(a) + z * math.cos(a))
        hx, hz = world(0.52 * k + 0.12, -0.24)
        B.tube([(hx, py, hz), (px + 0.52 * k + 0.12, py, h + 0.16)], 0.03, steel(), p, segs=8)
        cx, cz, cr = px - 0.50 * k, h + 0.30, 0.13 * k
        B.cyl(0.19 * k, 0.07, steel(), p, loc=(cx, py - 0.10, cz), rot=(90, 0, 0), bevel=0.02)
        qx, qz = cx + cr * math.cos(w), cz + cr * math.sin(w)
        B.sphere(0.055, steel(), p, loc=(qx, py - 0.19, qz))
        rx, rz = world(-0.50 * k, 0.0)
        B.tube([(qx, py - 0.19, qz), (rx, py - 0.14, rz)], 0.03, steel(), p, segs=8)
        n = (2, 3, 3)[tier - 1]
        z0, z1 = h + 0.16, zs - 0.08
        for i in range(n):
            u = (phase + float(i) / n) % 1.0
            B.torus(0.145, 0.04, gl(acc, 1.3), p, loc=(sx, sy, z0 + (z1 - z0) * u))
        r = 0.27 * k
        cg = B.group('coin', p, loc=(tx, ty, h + th + tr * 0.45 + 0.18 + r), rot=(0, 0, 180.0 * phase))
        B.prism(B.ngon(6, r), -0.045, 0.045, gl(acc, 1.2), cg, rot=(90, 0, 0), bevel=0.0)
        B.prism(B.ngon(6, r * 0.50), -0.06, 0.06, paint('ban_d', '#b07a08'), cg, rot=(90, 0, 0), bevel=0.0)
        if tier >= 2:
            beat = round(0.5 + 0.5 * math.cos(2 * w), 2)
            B.sphere(0.13 * k, hot(acc, 3.0 + 2.0 * beat, 0.3 + 0.3 * beat), p, loc=(sx, sy, zs + 0.14 * k),
                     scale=(0.8, 0.8, 1.2 + 0.3 * beat))

    return base, head


TOWERS = {
    'gatling': dict(build=build_gatling),
    'cannon': dict(build=build_cannon),
    'laser': dict(build=build_laser),
    'missile': dict(build=build_missile),
    'tesla': dict(build=build_tesla, spin=True),
    'cryo': dict(build=build_cryo, spin=True),
    'flame': dict(build=build_flame),
    'rail': dict(build=build_rail),
    'mortar': dict(build=build_mortar),
    'drone': dict(build=build_drone, spin=True),
    'emp': dict(build=build_emp, spin=True),
    'gravity': dict(build=build_gravity, spin=True),
    'radar': dict(build=build_radar, spin=True),
    'bank': dict(build=build_bank, spin=True),
}


def main():
    out = B.arg('--out', os.path.join(B.ASSETS, 'towers'))
    only = B.arg('--only')
    tiers = [int(t) for t in B.arg('--tiers', '1,2,3').split(',')]
    B.setup()
    B.studio()
    for tid, t in TOWERS.items():
        if only and tid not in only.split(','):
            continue
        for tier in tiers:
            base, head = t['build'](tier)
            B.render_sheet('%s%d_base' % (tid, tier), lambda p, c, r: base(p), out, 1, cell=CELL, anchor=ANCHOR)
            if t.get('spin'):
                B.render_sheet('%s%d_head' % (tid, tier), lambda p, c, r: head(p, c / 16.0), out, 16, cell=CELL,
                               anchor=ANCHOR, meta=dict(spin=True))
            else:
                B.render_sheet('%s%d_head' % (tid, tier), lambda p, c, r: head(p), out, 16, cell=CELL, anchor=ANCHOR,
                               yaws=YAWS)


if __name__ == '__main__':
    main()
