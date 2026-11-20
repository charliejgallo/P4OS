"""
BASTION - the fort the player protects, and the two small extras that fly
(SPEC.md, "The fort" and "Aliens ... and extras").

    /Applications/Blender.app/Contents/MacOS/Blender -b -P fort.py -- [--only fort,missile,drone] [--samples 48]

    assets/fort/fort.png       3 cells of 320 x 352, anchor (160, 256): the
                               fort whole, damaged and nearly lost
    assets/extras/missile.png  16 yaws, 48 x 48, anchor (24, 24)
    assets/extras/drone.png    8 yaws x 2 frames of its rotors, 64 x 64,
                               anchor (32, 40)

The fort is a hexagonal keep of two storeys inside a ring of six bastions
joined by walls, with a shield projector on its roof and antennae on its
ledge. It wears the towers' dark armour; what tells its state at a glance
is its light (cyan, then amber, then red), its outline (the masts bend,
the projector tilts, a bastion and a corner of the keep are gone) and how
much of its plating is left. The wounds are real holes: jagged solids are
subtracted from the armour, and what shows inside is charred metal with
embers and, when the keep is torn open, the reactor in its cage.
"""
import math
import os
import random
import sys

import bpy
from mathutils import Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bs_common as B  # noqa: E402
import towers as T  # noqa: E402

FORT_CELL, FORT_ANCHOR = (320, 352), (160, 256)
MISSILE_CELL, MISSILE_ANCHOR = (48, 48), (24, 24)
DRONE_CELL, DRONE_ANCHOR = (64, 64), (32, 40)

# the fort's light in each state, and how hard it shines: the amber is kept
# low and orange because a strong amber clips to yellow in the Standard view
LIGHT = [('#35e6ff', 4.0), ('#ff8a14', 2.4), ('#ff2414', 3.6)]
C30 = math.cos(math.radians(30))
RB = 1.98      # the bastions stand this far from the axis


# ---------------------------------------------------------------------------
# Materials of the fort
# ---------------------------------------------------------------------------

def light(s):
    return B.glow('fort_light%d' % s, LIGHT[s][0], LIGHT[s][1])


def white():
    return B.mat('fort_white', '#e6ecf6', rough=0.3, metal=0.1, clear=0.7)


def frame():
    return B.mat('fort_frame', '#1a1d2a', rough=0.6, metal=0.4)


def glass_off():
    return B.mat('fort_glass_off', '#0b0e16', rough=0.15, metal=0.3, clear=1.0)


def soot_mat():
    return B.mat('fort_soot', '#08080a', rough=1.0, spec=0.05)


def char(s):
    """the inside of a wound: burnt metal, with patches of ember in the
    state's light where a noise says so"""
    name = 'fort_char%d' % s
    if name in B.MATS:
        return B.MATS[name]
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    b = nt.nodes['Principled BSDF']
    b.inputs['Base Color'].default_value = B.lin('#2b2321')
    b.inputs['Roughness'].default_value = 0.9
    b.inputs['Metallic'].default_value = 0.2
    tc = nt.nodes.new('ShaderNodeTexCoord')
    nz = nt.nodes.new('ShaderNodeTexNoise')
    nz.inputs['Scale'].default_value = 4.5
    nz.inputs['Detail'].default_value = 3.0
    ramp = nt.nodes.new('ShaderNodeValToRGB')
    ramp.color_ramp.elements[0].position = 0.61
    ramp.color_ramp.elements[1].position = 0.67
    ramp.color_ramp.elements[0].color = (0, 0, 0, 1)
    ramp.color_ramp.elements[1].color = (1, 1, 1, 1)
    mul = nt.nodes.new('ShaderNodeMath')
    mul.operation = 'MULTIPLY'
    mul.inputs[1].default_value = LIGHT[s][1] * 0.8
    nt.links.new(tc.outputs['Object'], nz.inputs['Vector'])
    nt.links.new(nz.outputs['Fac'], ramp.inputs['Fac'])
    nt.links.new(ramp.outputs['Color'], mul.inputs[0])
    b.inputs['Emission'].default_value = B.lin(LIGHT[s][0])
    nt.links.new(mul.outputs['Value'], b.inputs['Emission Strength'])
    B.MATS[name] = m
    return m


# ---------------------------------------------------------------------------
# Hexagon arithmetic. Every hexagon of the fort has its corners at 0, 60 ...
# degrees, so its faces look along 30, 90 ... : face 4 (270) faces the
# camera, faces 3 and 5 are the two beside it, and 0 to 2 are the back.
# ---------------------------------------------------------------------------

def on_face(k, ap, t=0.0, z=0.0, out=0.0):
    """a place on face k of a hexagon whose faces are 'ap' from the axis:
    t along the face, z up, 'out' off it. Returns (loc, rot) for a part
    whose X runs along the face and whose Y is its thickness."""
    deg = 30 + 60 * k
    a = math.radians(deg)
    nx, ny = math.cos(a), math.sin(a)
    return ((ap + out) * nx - t * ny, (ap + out) * ny + t * nx, z), (0, 0, deg + 90)


def corner(v, r, z=0.0):
    a = math.radians(60 * v)
    return (r * math.cos(a), r * math.sin(a), z)


class Fort:
    """one fort being built under a parent, in a state: it remembers which
    parts a wound may cut, and the wounds themselves"""

    def __init__(self, parent, state):
        self.p = parent
        self.s = state
        self.rnd = random.Random(40 + state)
        self.cuttable = []
        self.cutters = []
        self.L = light(state)

    def C(self, o):
        self.cuttable.append(o)
        return o

    # -- wounds --------------------------------------------------------------

    def cutter(self, loc, r, seed=0, parent=None):
        """a jagged lump to subtract from the armour: an icosphere whose
        corners are pushed in and out"""
        o = B.ico(1.0, None, parent or self.p, loc=loc, subdiv=2, name='cut')
        rnd = random.Random(1000 + seed + 17 * len(self.cutters))
        for v in o.data.vertices:
            v.co = v.co * (r * rnd.uniform(0.74, 1.14))
        # Blender gives the faces a boolean makes the cutter's material,
        # found in the target by name or by slot according to its version:
        # with the char in slot 1 of both, either way lands on it
        o.data.materials.append(frame())
        o.data.materials.append(char(self.s))
        for pl in o.data.polygons:
            pl.material_index = 1
        o.hide_render = True
        o.display_type = 'WIRE'
        o['r'] = r * 1.16
        self.cutters.append(o)
        return o

    def bite(self):
        """subtract every cutter from the parts it reaches"""
        if not self.cutters:
            return
        bpy.context.view_layer.update()
        for o in self.cuttable:
            pts = [o.matrix_world @ Vector(b) for b in o.bound_box]
            lo = Vector([min(q[i] for q in pts) - 0.06 for i in range(3)])
            hi = Vector([max(q[i] for q in pts) + 0.06 for i in range(3)])
            for c in self.cutters:
                cc = c.matrix_world.translation
                d = Vector([max(lo[i] - cc[i], 0.0, cc[i] - hi[i]) for i in range(3)]).length
                if d > c['r']:
                    continue
                if len(o.data.materials) < 2:
                    o.data.materials.append(char(self.s))
                md = o.modifiers.new('bite', 'BOOLEAN')
                md.operation = 'DIFFERENCE'
                md.solver = 'EXACT'
                md.object = c

    def soot(self, k, ap, t, z, size=0.35, n=4):
        """a scorch mark on a wall: a few flat black blobs, smeared upwards"""
        for i in range(n):
            loc, rot = on_face(k, ap, t + self.rnd.uniform(-0.5, 0.5) * size, z + self.rnd.uniform(-0.3, 0.6) * size)
            r = size * self.rnd.uniform(0.35, 0.7)
            B.sphere(r, soot_mat(), self.p, loc=loc, rot=rot, scale=(1.0, 0.07 / r, self.rnd.uniform(1.0, 1.7)), segs=16)

    def rubble(self, x, y, z, n=5, spread=0.4, size=0.16):
        for i in range(n):
            m = (T.armor(), T.armor_l(), frame())[i % 3]
            r = size * self.rnd.uniform(0.6, 1.3)
            B.ico(r, m, self.p, loc=(x + self.rnd.uniform(-1, 1) * spread, y + self.rnd.uniform(-1, 1) * spread, z + r * 0.4),
                  rot=(self.rnd.uniform(0, 90), self.rnd.uniform(0, 90), self.rnd.uniform(0, 90)),
                  scale=(1.0, self.rnd.uniform(0.6, 1.0), self.rnd.uniform(0.4, 0.8)))

    # -- the parts -----------------------------------------------------------

    def plinth(self):
        p, s = self.p, self.s
        self.C(B.prism(B.ngon(6, 2.60), 0.0, 0.18, T.dark(), p, bevel=0.03))
        self.C(B.prism(B.ngon(6, 2.50), 0.18, 0.36, T.armor(), p, bevel=0.04))
        # the lit rim: three strips an edge; they go out one by one
        dead = [set(), {(3, 0), (5, 2), (4, 1)}, {(3, 0), (3, 1), (5, 2), (5, 1), (4, 0), (4, 2), (0, 0), (2, 1)}][s]
        for k in range(6):
            for i in range(3):
                loc, rot = on_face(k, 2.40 * C30, (i - 1) * 0.66, 0.365)
                B.box((0.54, 0.10, 0.035), glass_off() if (k, i) in dead else self.L, p, loc=loc, rot=rot, bevel=0.0)
        # the sloped foot of the walls, and the yard they close
        self.C(B.lathe([(2.26, 0.36), (2.10, 0.60)], T.armor_l(), p, segs=6, smooth=False))
        B.prism(B.ngon(6, 1.9), 0.36, 0.50, T.dark(), p, bevel=0.0)

    def bastion(self, v):
        """a corner tower: a hexagonal drum with a white band, slit lights
        and a lamp under a cap"""
        p, s = self.p, self.s
        g = B.group('bastion', p, loc=corner(v, RB))
        hb = 1.52
        # what each state has taken: the cap and lamp of these corners
        topless = [set(), {5}, {4, 5, 0}][s]
        self.C(B.prism(B.ngon(6, 0.50), 0.36, 0.58, T.dark(), g, bevel=0.03))
        self.C(B.prism(B.ngon(6, 0.42), 0.58, hb, T.armor(), g, bevel=0.03))
        self.C(B.prism(B.ngon(6, 0.445), hb - 0.34, hb - 0.20, white(), g, bevel=0.01))
        lit = self.L if not (s == 2 and v in (4, 0)) else glass_off()
        for k in range(6):
            loc, rot = on_face(k, 0.42 * C30, 0.0, 0.92, 0.0)
            self.C(B.box((0.13, 0.07, 0.36), lit, g, loc=loc, rot=rot, bevel=0.0))
        if v in topless:
            return
        self.C(B.prism(B.ngon(6, 0.52), hb, hb + 0.14, T.armor_l(), g, bevel=0.03))
        B.prism(B.ngon(6, 0.33), hb + 0.14, hb + 0.25, self.L, g, bevel=0.0)
        B.prism(B.ngon(6, 0.20), hb + 0.25, hb + 0.36, white(), g, bevel=0.02)

    def wall(self, k):
        """the curtain between two bastions: armour panels outside, a walk
        and merlons on top; the one that faces the camera has the gate"""
        p, s = self.p, self.s
        ap = RB * C30
        hw = 0.78
        loc, rot = on_face(k, ap, 0.0, 0.36 + hw / 2)
        self.C(B.box((1.60, 0.44, hw), T.armor(), p, loc=loc, rot=rot, bevel=0.03))
        top = 0.36 + hw
        loc, rot = on_face(k, ap, 0.0, top + 0.012, -0.05)
        B.box((1.30, 0.22, 0.03), T.dark(), p, loc=loc, rot=rot, bevel=0.0)
        gone = [set(), {(3, 0), (5, 1)}, {(3, -1), (3, 0), (3, 1), (5, 1), (5, 0), (4, -1)}][s]
        for i in (-1, 0, 1):
            if (k, i) in gone:
                continue
            loc, rot = on_face(k, ap, i * 0.40, top + 0.09, 0.14)
            self.C(B.box((0.26, 0.15, 0.20), T.armor_l(), p, loc=loc, rot=rot, bevel=0.03))
        if k == 4:
            return self.gate(ap)
        lost = [set(), {(5, 1)}, {(5, 1), (3, 1), (3, -1), (5, -1)}][s]
        for i in (-1, 1):
            if (k, i) in lost:
                # the wall's bare frame where a panel is gone
                for j in range(3):
                    loc, rot = on_face(k, ap, i * 0.34, 0.58 + 0.18 * j, 0.225)
                    B.box((0.46, 0.04, 0.05), frame(), p, loc=loc, rot=rot, bevel=0.0)
                continue
            loc, rot = on_face(k, ap, i * 0.34, 0.78, 0.23)
            self.C(B.box((0.50, 0.07, 0.52), T.armor_l(), p, loc=loc, rot=rot, bevel=0.03))
        loc, rot = on_face(k, ap, 0.0, 0.78, 0.23)
        self.C(B.box((0.09, 0.06, 0.46), self.L if not (s == 2 and k == 3) else glass_off(), p, loc=loc, rot=rot,
                     bevel=0.0))

    def gate(self, ap):
        p, s = self.p, self.s
        loc, rot = on_face(4, ap, 0.0, 0.36 + 0.50, 0.24)
        self.C(B.box((1.02, 0.14, 1.00), T.armor_l(), p, loc=loc, rot=rot, bevel=0.04))
        loc, rot = on_face(4, ap, 0.0, 1.39, 0.22)
        self.C(B.box((1.10, 0.22, 0.08), white(), p, loc=loc, rot=rot, bevel=0.02))
        loc, rot = on_face(4, ap, 0.0, 0.36 + 0.36, 0.30)
        self.C(B.box((0.70, 0.08, 0.72), T.dark(), p, loc=loc, rot=rot, bevel=0.02))
        # the door's lights: a lintel, two jambs and the seam between leaves
        loc, rot = on_face(4, ap, 0.0, 1.20, 0.31)
        self.C(B.box((0.72, 0.06, 0.09), self.L, p, loc=loc, rot=rot, bevel=0.0))
        for i in (-1, 1):
            loc, rot = on_face(4, ap, i * 0.41, 0.74, 0.31)
            self.C(B.box((0.07, 0.06, 0.66), self.L if not (s == 2 and i == -1) else glass_off(), p, loc=loc, rot=rot,
                         bevel=0.0))
        loc, rot = on_face(4, ap, 0.0, 0.70, 0.345)
        B.box((0.05, 0.03, 0.62), white() if s == 0 else frame(), p, loc=loc, rot=rot, bevel=0.0)
        # the ramp down to the plate
        loc, rot = on_face(4, ap, 0.0, 0.385, 0.50)
        B.box((0.86, 0.40, 0.05), T.steel(), p, loc=loc, rot=rot, bevel=0.015)

    def bare(self, k, ap, z, w, h):
        """the keep's frame where a plate is gone: ribs, and an ember of
        the state's light behind them"""
        loc, rot = on_face(k, ap, 0.0, z, 0.01)
        B.box((w * 0.62, 0.04, h * 0.55), self.L, self.p, loc=loc, rot=rot, bevel=0.0)
        n = max(2, int(h / 0.22))
        for j in range(n):
            loc, rot = on_face(k, ap, 0.0, z - h / 2 + (j + 0.5) * h / n, 0.035)
            B.box((w, 0.06, 0.07), T.dark(), self.p, loc=loc, rot=rot, bevel=0.0)
        for i in (-1, 0, 1):
            loc, rot = on_face(k, ap, i * w * 0.30, z, 0.05)
            B.box((0.07, 0.06, h), T.steel() if i else T.dark(), self.p, loc=loc, rot=rot, bevel=0.0)

    def keep(self):
        p, s = self.p, self.s
        rnd = self.rnd
        r1, z0, z1 = 1.35, 0.40, 2.50
        ap = r1 * C30
        self.C(B.prism(B.ngon(6, r1), z0, z1, frame(), p, bevel=0.0))
        gone = [set(), {(5, 1), (3, 0)}, {(5, 1), (3, 1), (4, 0), (5, 0)}][s]
        dark_share = [0.0, 0.35, 0.7][s]
        for k in range(6):
            self.C(B.cyl(0.115, z1 - z0, T.steel(), p, loc=corner(k, r1 + 0.03, z0), segs=12))
            for row, (zc, h) in enumerate(((1.00, 0.86), (1.97, 0.96))):
                if (k, row) in gone:
                    self.bare(k, ap, zc, 1.10, h)
                    continue
                loc, rot = on_face(k, ap, 0.0, zc, 0.05)
                self.C(B.box((1.15, 0.13, h), T.armor() if row == 0 else T.armor_l(), p, loc=loc, rot=rot, bevel=0.035))
                if row == 0:
                    loc, rot = on_face(k, ap, 0.0, 1.36, 0.115)
                    self.C(B.box((1.15, 0.03, 0.10), white(), p, loc=loc, rot=rot, bevel=0.0))
                    if k == 4:
                        # the fort's badge over the gate: a ring of light
                        loc, rot = on_face(k, ap, 0.0, 0.98, 0.115)
                        e = (90, 0, rot[2])
                        self.C(B.prism(B.ngon(6, 0.27), -0.03, 0.03, self.L, p, loc=loc, rot=e, bevel=0.0))
                        self.C(B.prism(B.ngon(6, 0.17), -0.05, 0.05, T.dark(), p, loc=loc, rot=e, bevel=0.0))
                        self.C(B.prism(B.ngon(6, 0.07), -0.07, 0.07, self.L, p, loc=loc, rot=e, bevel=0.0))
                    else:
                        loc, rot = on_face(k, ap, 0.0, 0.92, 0.105)
                        self.C(B.box((0.56, 0.05, 0.10), self.L if rnd.random() >= dark_share else glass_off(), p,
                                     loc=loc, rot=rot, bevel=0.0))
                    continue
                # three tall windows a face on the upper storey
                for i in (-1, 0, 1):
                    m = glass_off() if rnd.random() < dark_share else self.L
                    loc, rot = on_face(k, ap, i * 0.34, zc + 0.02, 0.105)
                    self.C(B.box((0.20, 0.06, 0.50), m, p, loc=loc, rot=rot, bevel=0.0))
        # the ledge between the storeys, with a lit edge and white corners
        r2 = 1.60
        self.C(B.prism(B.ngon(6, r2), z1, z1 + 0.17, T.armor_l(), p, bevel=0.035))
        for k in range(6):
            loc, rot = on_face(k, r2 * C30, 0.0, z1 + 0.085, 0.0)
            self.C(B.box((1.06, 0.07, 0.07), self.L if not (s == 2 and k in (3, 0)) else glass_off(), p, loc=loc,
                         rot=rot, bevel=0.0))
            if not (s >= 1 and k == 4) and not (s == 2 and k in (5, 0)):
                self.C(B.prism(B.ngon(6, 0.15), z1 + 0.17, z1 + 0.37, white(), p, loc=corner(k, r2 - 0.14), bevel=0.02))
        # the upper storey: one band of light a face
        r3, z2, z3 = 1.00, z1 + 0.17, 3.40
        ap3 = r3 * C30
        self.C(B.prism(B.ngon(6, r3), z2, z3, frame(), p, bevel=0.0))
        gone3 = [set(), {3}, {3, 4}][s]
        for k in range(6):
            self.C(B.cyl(0.09, z3 - z2, T.steel(), p, loc=corner(k, r3 + 0.03, z2), segs=12))
            if k in gone3:
                self.bare(k, ap3, (z2 + z3) / 2, 0.80, z3 - z2 - 0.1)
                continue
            loc, rot = on_face(k, ap3, 0.0, (z2 + z3) / 2, 0.04)
            self.C(B.box((0.84, 0.11, z3 - z2 - 0.06), T.armor(), p, loc=loc, rot=rot, bevel=0.03))
            m = glass_off() if rnd.random() < dark_share * 0.8 else self.L
            loc, rot = on_face(k, ap3, 0.0, (z2 + z3) / 2 + 0.05, 0.085)
            self.C(B.box((0.62, 0.06, 0.24), m, p, loc=loc, rot=rot, bevel=0.0))
        # the roof with its white eave, and the deck the projector stands on
        self.C(B.prism(B.ngon(6, 1.14), z3, z3 + 0.09, white(), p, bevel=0.02))
        self.C(B.lathe([(1.08, z3 + 0.09), (0.66, z3 + 0.34)], T.armor_l(), p, segs=6, smooth=False))
        self.C(B.cyl(0.64, 0.06, T.dark(), p, loc=(0, 0, z3 + 0.34), segs=24))
        return z3 + 0.40

    def reactor(self):
        """what a torn keep shows: a column of light in a cage"""
        p = self.p
        B.cyl(0.34, 3.0, self.L, p, loc=(0, 0, 0.5), segs=20)
        for z in (1.9, 2.3, 2.7, 3.1, 3.45):
            B.torus(0.40, 0.065, T.dark(), p, loc=(0, 0, z), segs=20, tsegs=8)
        for k in range(6):
            B.cyl(0.05, 3.0, T.steel(), p, loc=corner(k + 0.5, 0.42, 0.5), segs=8)

    def projector(self, z):
        """the shield projector: a dish, an orb of light held by three
        claws, a ring about it"""
        s = self.s
        g = B.group('proj', self.p, loc=(0, 0, z), rot=[(0, 0, 0), (0, 0, 0), (17, 21, 0)][s])
        B.cyl(0.30, 0.16, T.armor(), g, bevel=0.02)
        B.lathe([(0.22, 0.12), (0.50, 0.22), (0.64, 0.42), (0.58, 0.44), (0.34, 0.30), (0.0, 0.28)], T.steel(), g, segs=32)
        B.cyl(0.33, 0.03, self.L if s < 2 else glass_off(), g, loc=(0, 0, 0.30), segs=24)
        zo = 0.92
        for i, a in enumerate((90, 210, 330)):
            if s == 1 and i == 2:
                continue
            ca, sa = math.cos(math.radians(a)), math.sin(math.radians(a))
            if s == 2 and i != 0:
                # splayed open, their grip lost
                rr = [(0.56, 0.38), (0.80, 0.60), (0.98, 0.88), (1.02, 1.10)]
            else:
                rr = [(0.56, 0.38), (0.69, 0.70), (0.61, 1.10), (0.40, 1.34)]
            B.tube([(r * ca, r * sa, h) for (r, h) in rr], 0.075, T.armor_l(), g, r_end=0.05, segs=8)
            B.sphere(0.075, self.L, g, loc=(rr[-1][0] * ca, rr[-1][0] * sa, rr[-1][1]), segs=12)
        if s < 2:
            B.sphere(0.35, self.L, g, loc=(0, 0, zo))
            # a brighter heart towards the camera, so the orb is a ball and
            # not a flat disc
            B.sphere(0.14, B.glow('fort_heart%d' % s, ['#d8fbff', '#fff0b0'][s], 5.0), g, loc=(-0.07, -0.20, zo + 0.24),
                     scale=(1.1, 0.5, 0.8), segs=16)
            if s == 0:
                B.torus(0.58, 0.05, self.L, g, loc=(0, 0, zo), rot=(22, 10, 0), segs=40, tsegs=8)
            else:
                B.torus(0.58, 0.05, self.L, g, loc=(0, 0, zo - 0.1), rot=(38, -16, 60), segs=40, tsegs=8, arc=215)
        else:
            # the orb is out: a cracked dark shell around its last light
            shell = B.sphere(0.35, glass_off(), g, loc=(0, 0, zo - 0.1))
            B.sphere(0.26, self.L, g, loc=(0, 0, zo - 0.1), segs=16)
            n = len(self.cutters)
            c = self.cutter((0.05, -0.36, zo + 0.14), 0.33, seed=5, parent=g)
            del self.cutters[n:]
            self._orb = (shell, c)

    def masts(self):
        """a tall lattice mast, a dish and two whips on the ledge: straight,
        then bent, then broken"""
        p, s = self.p, self.s
        zb = 2.67
        # the tall mast at the back left
        x, y, _ = corner(2, 1.42)
        g = B.group('mast', p, loc=(x, y, zb))
        B.cyl(0.13, 0.14, T.armor(), g, segs=12)
        if s == 0:
            pts = [(0, 0, 0), (0, 0, 2.50)]
            bars = [1.5, 1.82, 2.14]
        elif s == 1:
            pts = [(0, 0, 0), (0, 0, 1.5), (-0.10, 0, 1.72), (-0.72, 0.1, 2.36)]
            bars = [1.2]
        else:
            pts = [(0, 0, 0), (0, 0, 0.95), (-0.12, 0, 1.18), (-0.80, 0.0, 1.22), (-1.30, 0.0, 0.84)]
            bars = []
        B.tube(pts, 0.06, T.steel(), g, r_end=0.045, segs=8)
        for i, zz in enumerate(bars):
            w = 0.32 - 0.07 * i
            B.tube([(-w, 0, zz), (w, 0, zz)], 0.04, T.steel(), g, segs=6)
        if s < 2:
            B.sphere(0.11, self.L, g, loc=pts[-1], segs=12)
        # the dish on the right corner
        x, y, _ = corner(0, 1.44)
        g = B.group('dish', p, loc=(x, y, zb))
        B.cyl(0.12, 0.14, T.armor(), g, segs=12)
        if s < 2:
            B.tube([(0, 0, 0), (0, 0, 0.95)], 0.06, T.steel(), g, segs=8)
            d = B.group('d', g, loc=(0, 0, 1.0), rot=(0, 55, -55) if s == 0 else (0, 80, -75))
        else:
            B.tube([(0, 0, 0), (0, 0, 0.45), (0.10, -0.05, 0.62), (0.50, -0.20, 0.52)], 0.06, T.steel(), g, segs=8)
            d = B.group('d', g, loc=(0.58, -0.22, 0.42), rot=(0, 150, -30))
        B.lathe([(0.03, 0.0), (0.22, 0.05), (0.38, 0.18), (0.35, 0.20), (0.18, 0.09), (0.0, 0.06)], white(), d, segs=20)
        B.tube([(0, 0, 0.05), (0, 0, 0.34)], 0.035, T.steel(), d, segs=6)
        B.sphere(0.075, self.L if s < 2 else glass_off(), d, loc=(0, 0, 0.36), segs=12)
        # two whips on the left corner
        x, y, _ = corner(3, 1.42)
        for i, dy in enumerate((-0.14, 0.14)):
            if s == 1 and i == 0:
                continue
            g = B.group('whip', p, loc=(x, y + dy, zb))
            h = 1.10 + 0.35 * i
            if s < 2:
                pts = [(0, 0, 0), (0, 0, h)]
            else:
                pts = [(0, 0, 0), (0, 0, 0.35), (-0.08, 0, 0.5), (-0.55 - 0.2 * i, 0.3 * i, 0.62 - 0.25 * i)]
            B.tube(pts, 0.045, T.steel(), g, r_end=0.035, segs=6)
            if s < 2:
                B.sphere(0.075, self.L, g, loc=pts[-1], segs=10)

    def damage(self):
        """the holes and the dirt of each state"""
        s = self.s
        bx, by, _ = corner(5, RB)
        wall_out = RB * C30 + 0.27
        keep_out = 1.35 * C30 + 0.118
        if s == 1:
            self.cutter((bx + 0.08, by - 0.08, 1.72), 0.48, 1)   # the top of the front right bastion
            self.cutter((-0.90, -1.48, 2.66), 0.40, 2)           # a corner of the ledge
            self.cutter((-1.60, -0.96, 1.26), 0.40, 3)           # the top of the front left wall
            self.cutter((2.26, -1.20, 0.30), 0.34, 4)            # a nick in the plinth
            self.soot(4, keep_out, -0.36, 1.85, 0.36)
            self.soot(4, keep_out, 0.40, 0.95, 0.26, 3)
            self.soot(5, wall_out, -0.30, 0.72, 0.28)
            self.soot(3, wall_out, 0.10, 0.80, 0.28)
            self.rubble(1.72, -1.52, 0.36, 3, 0.10, 0.11)
        elif s == 2:
            self.cutter((0.58, -0.66, 2.92), 1.08, 1)            # the keep torn open at the front right
            self.cutter((-1.48, -1.00, 1.10), 0.72, 2)           # a breach in the front left wall
            self.cutter((-bx, by - 0.04, 1.62), 0.72, 3)         # the front left bastion, down to a stump
            self.cutter((bx + 0.08, by - 0.08, 1.74), 0.50, 4)   # the top of the front right one
            self.cutter((RB + 0.10, 0.02, 1.74), 0.48, 5)        # and of the right one
            self.cutter((-1.28, -0.56, 1.85), 0.46, 6)           # a dent in the keep's left
            self.cutter((1.42, -2.38, 0.26), 0.42, 7)            # the plinth's corners
            self.cutter((-2.50, -0.30, 0.30), 0.36, 8)
            self.soot(4, keep_out, -0.36, 1.80, 0.42, 5)
            self.soot(5, wall_out, 0.20, 0.72, 0.30)
            self.soot(4, wall_out + 0.06, 0.52, 0.80, 0.28)
            self.soot(3, 1.00 * C30 + 0.10, 0.0, 2.95, 0.30)
            self.rubble(-1.80, -1.20, 0.36, 5, 0.16, 0.14)
            self.rubble(-1.10, -1.98, 0.36, 3, 0.10, 0.12)
            self.rubble(0.95, -1.05, 2.67, 4, 0.20, 0.12)

    def build(self):
        s = self.s
        self._orb = None
        self.plinth()
        for v in range(6):
            self.bastion(v)
        for k in range(6):
            self.wall(k)
        top = self.keep()
        if s == 2:
            self.reactor()
        self.projector(top)
        self.masts()
        self.damage()
        self.bite()
        if self._orb:
            # the dead orb's shell is cracked by a cutter of its own
            shell, c = self._orb
            shell.data.materials.append(char(s))
            md = shell.modifiers.new('bite', 'BOOLEAN')
            md.operation = 'DIFFERENCE'
            md.solver = 'EXACT'
            md.object = c


def build_fort(parent, col, row):
    Fort(parent, col).build()


# ---------------------------------------------------------------------------
# The missile: 0.6 m, white with a lime band, looking along +X. It is
# centred on its origin, which lands on the middle of its cell.
# ---------------------------------------------------------------------------

def build_missile(p, col, row):
    body = B.mat('ms_white', '#f4f6fa', rough=0.25, metal=0.0, clear=0.8)
    lime = B.mat('ms_lime', '#9be830', rough=0.3, clear=0.6, emit='#9be830', emit_k=0.6)
    B.cyl(0.085, 0.34, body, p, loc=(-0.18, 0, 0), rot=(0, 90, 0), segs=16)
    B.lathe([(0.085, 0.0), (0.075, 0.07), (0.045, 0.13), (0.0, 0.16)], body, p, loc=(0.16, 0, 0), rot=(0, 90, 0), segs=16)
    B.cyl(0.095, 0.13, lime, p, loc=(-0.09, 0, 0), rot=(0, 90, 0), segs=16)
    B.cyl(0.07, 0.09, T.dark(), p, loc=(-0.27, 0, 0), rot=(0, 90, 0), r2=0.055, segs=12)
    B.cyl(0.05, 0.02, B.glow('ms_fire', '#ffb040', 6.0), p, loc=(-0.285, 0, 0), rot=(0, 90, 0), segs=12)
    for i in range(4):
        g = B.group('fin', p, rot=(45 + 90 * i, 0, 0))
        B.prism([(-0.27, 0.07), (-0.10, 0.07), (-0.20, 0.17), (-0.27, 0.17)], -0.02, 0.02, T.armor(), g, rot=(90, 0, 0),
                bevel=0.0)


# ---------------------------------------------------------------------------
# The drone: a 0.6 m quadcopter with a pink light, its belly at Z = 0.2.
# Two frames: the rotors a quarter turn apart.
# ---------------------------------------------------------------------------

def build_drone(p, col, row):
    acc = '#ff6fa8'
    pink = B.glow('dr_pink', acc, 6.0)
    blur = B.mat('dr_blur', '#dfe8ff', rough=0.4, alpha=0.22)
    B.sphere(0.15, T.armor(), p, loc=(0, 0, 0.31), scale=(1.2, 0.9, 0.62), segs=20)
    B.sphere(0.10, T.paint('dr', acc), p, loc=(-0.02, 0, 0.36), scale=(1.1, 0.85, 0.6), segs=16)
    B.sphere(0.055, pink, p, loc=(0.15, 0, 0.31), segs=12)
    B.sphere(0.05, pink, p, loc=(-0.04, 0, 0.425), segs=12)
    for i, (sx, sy) in enumerate(((1, 1), (-1, 1), (-1, -1), (1, -1))):
        x, y = 0.19 * sx, 0.19 * sy
        B.tube([(0.05 * sx, 0.05 * sy, 0.31), (x, y, 0.33)], 0.032, T.armor_l(), p, segs=6)
        B.cyl(0.05, 0.09, T.dark(), p, loc=(x, y, 0.28), segs=10)
        B.torus(0.125, 0.022, T.steel(), p, loc=(x, y, 0.385), segs=20, tsegs=6)
        B.cyl(0.115, 0.008, blur, p, loc=(x, y, 0.385), segs=16)
        B.box((0.23, 0.045, 0.014), white(), p, loc=(x, y, 0.392), rot=(0, 0, 90 * row + 45 * (i % 2) + 20), bevel=0.0)


def main():
    only = B.arg('--only')
    want = only.split(',') if only else ['fort', 'missile', 'drone']
    B.setup()
    B.studio()
    if 'fort' in want:
        B.render_sheet('fort', build_fort, os.path.join(B.ASSETS, 'fort'), 3, cell=FORT_CELL, anchor=FORT_ANCHOR,
                       meta=dict(states=['whole', 'damaged', 'nearly_lost']))
    if 'missile' in want:
        B.render_sheet('missile', build_missile, os.path.join(B.ASSETS, 'extras'), 16, cell=MISSILE_CELL,
                       anchor=MISSILE_ANCHOR, yaws=[i * 22.5 for i in range(16)])
    if 'drone' in want:
        B.render_sheet('drone', build_drone, os.path.join(B.ASSETS, 'extras'), 8, rows=2, cell=DRONE_CELL,
                       anchor=DRONE_ANCHOR, yaws=[i * 45.0 for i in range(8)])


if __name__ == '__main__':
    main()
