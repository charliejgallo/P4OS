"""
TORTUGA - the turtle, modelled by code, and its sheets of sprites.

    /Applications/Blender.app/Contents/MacOS/Blender -b -P turtle.py -- --out <dir> [--samples 32] [--only walk]

A warm toy turtle: a round caramel shell with cream seams between its plates, a big
head with big eyes, stubby legs and a red scarf (the one accent colour that
tells it apart on any board). It stands on its origin and looks along +X.

Sheets (cell 112 x 112, the origin at pixel (56, 72) of each cell):

| Sheet | Columns | Rows |
|---|---|---|
| tg_walk  | 16 yaws, 0, 22.5 ... (0 looks right, 90 away, 270 at you) | 6 frames of the walk, a loop; frame 0 is it standing |
| tg_bump  | 4 yaws, 0 90 180 270 | 4 frames: it hits, squashes, bounces back, shakes it off |
| tg_eat   | 4 yaws | 4 frames: head down, bite, chew, up |
| tg_party | 4 yaws | 6 frames: crouch, jump, up with its arms open, land |

The turns are the walk's yaws played in a row; the drawing world uses them
for any heading (the nearest of 16).
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tg_common as B  # noqa: E402

CELL = (112, 112)
ANCHOR = (56, 72)

SKIN = '#9fdc7c'
SKIN_DARK = '#74b85a'
SHELL_A = '#c7772a'
SHELL_B = '#e3a043'
SEAM = '#fff0c4'
RIM = '#8a4f24'
BELLY = '#f6e3a0'
SCARF = '#e8443a'
EYE = '#ffffff'
PUPIL = '#1d1a22'
CHEEK = '#ff9b9b'


def shell_mat():
    """the plates: a Voronoi pattern in the shell's own coordinates, with
    cream seams where the cells meet"""
    name = 'shell'
    if name in B.MATS:
        return B.MATS[name]
    m = B.bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    b = nt.nodes['Principled BSDF']
    tc = nt.nodes.new('ShaderNodeTexCoord')
    vo = nt.nodes.new('ShaderNodeTexVoronoi')
    vo.feature = 'DISTANCE_TO_EDGE'
    vo.inputs['Scale'].default_value = 1.35
    vo.inputs['Randomness'].default_value = 0.55
    vo2 = nt.nodes.new('ShaderNodeTexVoronoi')      # the plates' own colour
    vo2.inputs['Scale'].default_value = 1.35
    vo2.inputs['Randomness'].default_value = 0.55
    edge = nt.nodes.new('ShaderNodeValToRGB')
    edge.color_ramp.elements[0].position = 0.035
    edge.color_ramp.elements[1].position = 0.075
    edge.color_ramp.elements[0].color = B.lin(SEAM)
    edge.color_ramp.elements[1].color = (1, 1, 1, 1)
    plate = nt.nodes.new('ShaderNodeValToRGB')
    plate.color_ramp.elements[0].color = B.lin(SHELL_A)
    plate.color_ramp.elements[1].color = B.lin(SHELL_B)
    mul = nt.nodes.new('ShaderNodeMixRGB')
    mul.blend_type = 'MULTIPLY'
    mul.inputs['Fac'].default_value = 1.0
    mix = nt.nodes.new('ShaderNodeMixRGB')         # seam colour over the plates
    nt.links.new(tc.outputs['Object'], vo.inputs['Vector'])
    nt.links.new(tc.outputs['Object'], vo2.inputs['Vector'])
    nt.links.new(vo2.outputs['Distance'], plate.inputs['Fac'])
    nt.links.new(vo.outputs['Distance'], edge.inputs['Fac'])
    inv = nt.nodes.new('ShaderNodeMath')
    inv.operation = 'LESS_THAN'
    inv.inputs[1].default_value = 0.06
    nt.links.new(vo.outputs['Distance'], inv.inputs[0])
    nt.links.new(inv.outputs[0], mix.inputs['Fac'])
    nt.links.new(plate.outputs['Color'], mix.inputs['Color1'])
    mix.inputs['Color2'].default_value = B.lin(SEAM)
    nt.links.new(mix.outputs['Color'], b.inputs['Base Color'])
    b.inputs['Roughness'].default_value = 0.35
    b.inputs['Clearcoat'].default_value = 0.6
    bp = nt.nodes.new('ShaderNodeBump')
    bp.inputs['Strength'].default_value = 0.35
    bp.inputs['Distance'].default_value = 0.02
    nt.links.new(vo.outputs['Distance'], bp.inputs['Height'])
    nt.links.new(bp.outputs['Normal'], b.inputs['Normal'])
    B.MATS[name] = m
    return m


def mats():
    return dict(
        skin=B.mat('skin', SKIN, rough=0.55, spec=0.35),
        skin_dark=B.mat('skin_dark', SKIN_DARK, rough=0.6, spec=0.3),
        shell=shell_mat(),
        rim=B.mat('rim', RIM, rough=0.4, clear=0.4),
        belly=B.mat('belly', BELLY, rough=0.5),
        scarf=B.mat('scarf', SCARF, rough=0.6, spec=0.3),
        eye=B.mat('eye', EYE, rough=0.15, clear=1.0),
        pupil=B.mat('pupil', PUPIL, rough=0.1, clear=1.0),
        spark=B.glow('spark', '#ffffff', 3.0),
        cheek=B.mat('cheek', CHEEK, rough=0.6),
        mouth=B.mat('mouth', '#5a2230', rough=0.5),
    )


def turtle(parent, M, pose):
    """The turtle under 'parent' in a pose: a dict of offsets.

    body_z, body_x: the shell's lift and push; pitch: the shell's tilt
    (degrees, nose up > 0); head (dx, dz) and head_pitch; legs: four
    (dx, dz) for front-left, front-right, back-left, back-right; eyes:
    'open', 'shut' (squeezed) or 'happy' (arcs); mouth: None, 'smile' or
    'open'; arms_up: the front legs raised (the party)."""
    p = dict(body_z=0.0, body_x=0.0, pitch=0.0, head=(0.0, 0.0), head_pitch=0.0,
             legs=((0, 0),) * 4, eyes='open', mouth='smile', arms_up=0.0)
    p.update(pose)
    body = B.group('body', parent, loc=(p['body_x'], 0, p['body_z']), rot=(0, -p['pitch'], 0))

    # legs first: they hang from the body but stand on the ground
    feet = [(0.21, 0.20), (0.21, -0.20), (-0.20, 0.19), (-0.20, -0.19)]
    for i, (fx, fy) in enumerate(feet):
        dx, dz = p['legs'][i]
        front = i < 2
        up = p['arms_up'] if front else 0.0
        leg = B.group('leg%d' % i, body, loc=(fx + dx, fy, 0.11 + dz - p['body_z'] * 0.0))
        if up:
            # a front leg raised and open, like a little arm waving
            B.tube([(0, 0, 0), (0.06, fy * 0.35, 0.10 * up), (0.10, fy * 0.6, 0.22 * up)], 0.065, M['skin'],
                   leg, segs=12)
            B.sphere(0.075, M['skin'], leg, loc=(0.10, fy * 0.6, 0.22 * up))
        else:
            B.sphere(0.085, M['skin'], leg, loc=(0, 0, -0.03), scale=(1.15, 1.0, 1.15))
            B.sphere(0.08, M['skin_dark'], leg, loc=(0.02, 0, -0.075), scale=(1.25, 1.1, 0.55))

    # the tail
    B.cyl(0.06, 0.14, M['skin'], body, loc=(-0.33, 0, 0.17), rot=(0, -100, 0), r2=0.01, segs=16)

    # the belly, the rim and the shell
    B.sphere(1.0, M['belly'], body, loc=(0, 0, 0.17), scale=(0.37, 0.31, 0.09))
    B.torus(1.0, 0.11, M['rim'], body, loc=(0, 0, 0.19), segs=48, tsegs=14).scale = (0.38, 0.32, 0.45)
    sh = B.sphere(1.0, M['shell'], body, loc=(0, 0, 0.17), scale=(0.37, 0.31, 0.30), segs=48)
    sh.name = 'shell'

    # the neck, the scarf and the head
    hx, hz = p['head']
    head = B.group('head', body, loc=(0.38 + hx, 0, 0.30 + hz), rot=(0, -p['head_pitch'], 0))
    B.tube([(-0.17 - hx * 0.5, 0, -0.09 - hz * 0.6), (-0.06, 0, -0.03)], 0.075, M['skin'], head, segs=14)
    B.torus(0.11, 0.055, M['scarf'], head, loc=(-0.05, 0, -0.09), rot=(0, 70, 0), segs=24, tsegs=12)
    B.prism([(0, -0.05), (0.17, -0.07), (0.16, 0.05), (0, 0.05)], -0.02, 0.02, M['scarf'], head,
            loc=(-0.10, 0.02, -0.02), rot=(0, 20, 165), bevel=0.012)
    B.sphere(0.19, M['skin'], head, loc=(0.05, 0, 0.03), scale=(1.1, 1.0, 0.92))

    # the eyes: big, on the front of the head, a little apart
    for side in (1, -1):
        ex, ey, ez = 0.20, 0.085 * side, 0.10
        if p['eyes'] == 'open':
            B.sphere(0.07, M['eye'], head, loc=(ex, ey, ez), scale=(0.8, 1.0, 1.15))
            B.sphere(0.042, M['pupil'], head, loc=(ex + 0.04, ey * 1.06, ez + 0.002))
            B.sphere(0.014, M['spark'], head, loc=(ex + 0.085, ey * 1.06 - 0.012 * side, ez + 0.025))
        elif p['eyes'] == 'shut':
            B.tube([(ex + 0.03, ey - 0.03, ez + 0.02), (ex + 0.045, ey, ez - 0.005),
                    (ex + 0.03, ey + 0.03, ez + 0.02)], 0.011, M['pupil'], head, segs=8)
        else:  # happy: an upturned arc
            B.tube([(ex + 0.035, ey - 0.032, ez - 0.01), (ex + 0.05, ey, ez + 0.022),
                    (ex + 0.035, ey + 0.032, ez - 0.01)], 0.012, M['pupil'], head, segs=8)
        B.sphere(0.035, M['cheek'], head, loc=(ex + 0.005, ey * 1.4, ez - 0.09), scale=(0.5, 1.0, 0.7))

    # the mouth
    if p['mouth'] == 'smile':
        B.tube([(0.22, -0.04, -0.035), (0.245, 0, -0.055), (0.22, 0.04, -0.035)], 0.01, M['mouth'], head,
               segs=6)
    elif p['mouth'] == 'open':
        B.sphere(0.042, M['mouth'], head, loc=(0.235, 0, -0.05), scale=(0.6, 1.0, 0.8))
    return body


# ---------------------------------------------------------------------------
# The poses of each sheet
# ---------------------------------------------------------------------------

def walk_pose(f, n=6):
    """frame f of the walk: diagonal pairs of legs swing together"""
    a = 2 * math.pi * f / n
    s, c = math.sin(a), math.cos(a)
    if f == 0:
        return dict()
    step, lift = 0.085, 0.06
    pair1 = (step * s, max(0.0, lift * c))       # front-left and back-right
    pair2 = (-step * s, max(0.0, -lift * c))     # front-right and back-left
    return dict(legs=(pair1, pair2, pair2, pair1), body_z=0.012 * abs(s), pitch=1.5 * s,
                head=(0.01 * c, 0.01 * abs(s)))


BUMP = [
    dict(body_x=0.05, head=(0.03, -0.01), eyes='open', mouth='smile'),
    dict(body_x=-0.02, pitch=-6, head=(-0.06, -0.03), head_pitch=-8, eyes='shut', mouth='open'),
    dict(body_x=-0.07, pitch=4, head=(-0.02, 0.0), eyes='shut', mouth=None),
    dict(body_x=-0.03, head=(0.0, 0.01), head_pitch=6, eyes='open', mouth=None),
]

EAT = [
    dict(head=(0.04, -0.06), head_pitch=-25, pitch=-3, mouth='open'),
    dict(head=(0.06, -0.09), head_pitch=-32, pitch=-4, mouth=None, eyes='happy'),
    dict(head=(0.04, -0.05), head_pitch=-15, pitch=-2, mouth='smile', eyes='happy'),
    dict(head=(0.0, 0.0), mouth='smile', eyes='happy'),
]

PARTY = [
    dict(body_z=-0.02, legs=((0, -0.01),) * 4, pitch=-3, eyes='happy', mouth='open'),
    dict(body_z=0.10, pitch=8, arms_up=0.6, head=(0, 0.02), head_pitch=10, eyes='happy', mouth='open',
         legs=((0, 0.06), (0, 0.06), (0, 0.04), (0, 0.04))),
    dict(body_z=0.20, pitch=14, arms_up=1.0, head=(0, 0.04), head_pitch=16, eyes='happy', mouth='open',
         legs=((0, 0.08), (0, 0.08), (0, 0.06), (0, 0.06))),
    dict(body_z=0.18, pitch=12, arms_up=1.0, head=(0, 0.04), head_pitch=12, eyes='happy', mouth='smile',
         legs=((0, 0.08), (0, 0.08), (0, 0.06), (0, 0.06))),
    dict(body_z=0.08, pitch=5, arms_up=0.5, eyes='happy', mouth='smile'),
    dict(body_z=-0.01, eyes='happy', mouth='smile'),
]

YAW16 = [i * 22.5 for i in range(16)]
YAW4 = [0, 90, 180, 270]


def sheet(name, poses, yaws, out):
    M = {}
    get = poses if callable(poses) else (lambda r: poses[r])
    rows = poses.n if callable(poses) else len(poses)

    def build(parent, col, row):
        if not M:
            M.update(mats())
        turtle(parent, M, get(row))
    B.render_sheet(name, build, out, len(yaws), rows, CELL, ANCHOR, yaws, meta=dict(kind=name))


def main():
    out = B.arg('--out', os.path.join(B.HERE, '..', '..', 'build', 'art'))
    only = B.arg('--only')
    B.setup()
    B.studio()

    class Walk:
        n = 6

        def __call__(self, f):
            return walk_pose(f, self.n)

    jobs = [('tg_walk', Walk(), YAW16), ('tg_bump', BUMP, YAW4), ('tg_eat', EAT, YAW4),
            ('tg_party', PARTY, YAW4)]
    for name, poses, yaws in jobs:
        if only and only not in name:
            continue
        sheet(name, poses, yaws, out)


if __name__ == '__main__':
    main()
