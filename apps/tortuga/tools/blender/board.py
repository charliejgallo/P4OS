"""
TORTUGA - the board: the ground tiles and what stands on them.

    /Applications/Blender.app/Contents/MacOS/Blender -b -P board.py -- --out <dir> [--samples 32] [--only ground]

The board is a toy: every tile is a little block, grass on top and soil on
the sides, and water sits lower, so the front of a grass block shows above
the water in front of it. The game draws the rows from the back to the
front, each tile and then what stands on it and the turtle, and any map
comes out of the same few pictures. Levels are data; the art is not drawn
per map.

| Sheet | Cell | Anchor (the tile's centre on the ground) | Columns | Rows |
|---|---|---|---|---|
| tg_ground | 64 x 96 | (32, 40) | 1 | grass 0-3 (2 and 3 with flowers), water frames 0-3 (a ping-pong loop) |
| tg_rock, tg_lettuce | 96 x 160 | (48, 120) | variants | 1 |
| tg_key | 96 x 160 | (48, 120) | 4 frames, it bobs | 1 |
| tg_door | 96 x 160 | (48, 120) | closed, open (passage north-south), closed, open (east-west) | 1 |
| tg_goal | 96 x 160 | (48, 120) | 4 frames of the flag | 1 |
| tg_bridge | 96 x 160 | (48, 120) | east-west, north-south | 1 |
"""
import math
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tg_common as B  # noqa: E402

GROUND_CELL, GROUND_ANCHOR = (64, 96), (32, 40)
DECOR_CELL, DECOR_ANCHOR = (96, 160), (48, 120)
TOP, BOTTOM, WATER = 0.0, -0.6, -0.22


def noise4(name, c0, c1, w, scale=4.0, rough=0.7, bump=0.0, clear=0.0, contrast=(0.3, 0.7), detail=4.0):
    """two colours mixed by a 4D noise in world coordinates; w picks the
    slice, so two tiles (or two frames) differ"""
    if name in B.MATS:
        return B.MATS[name]
    m = B.bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    b = nt.nodes['Principled BSDF']
    tc = nt.nodes.new('ShaderNodeTexCoord')
    nz = nt.nodes.new('ShaderNodeTexNoise')
    nz.noise_dimensions = '4D'
    nz.inputs['W'].default_value = w
    nz.inputs['Scale'].default_value = scale
    nz.inputs['Detail'].default_value = detail
    ramp = nt.nodes.new('ShaderNodeValToRGB')
    ramp.color_ramp.elements[0].position = contrast[0]
    ramp.color_ramp.elements[1].position = contrast[1]
    ramp.color_ramp.elements[0].color = B.lin(c0)
    ramp.color_ramp.elements[1].color = B.lin(c1)
    nt.links.new(tc.outputs['Object'], nz.inputs['Vector'])
    nt.links.new(nz.outputs['Fac'], ramp.inputs['Fac'])
    nt.links.new(ramp.outputs['Color'], b.inputs['Base Color'])
    b.inputs['Roughness'].default_value = rough
    b.inputs['Clearcoat'].default_value = clear
    if bump:
        bp = nt.nodes.new('ShaderNodeBump')
        bp.inputs['Strength'].default_value = bump
        bp.inputs['Distance'].default_value = 0.05
        nt.links.new(nz.outputs['Fac'], bp.inputs['Height'])
        nt.links.new(bp.outputs['Normal'], b.inputs['Normal'])
    B.MATS[name] = m
    return m


def checker(name, c0, c1, scale):
    if name in B.MATS:
        return B.MATS[name]
    m = B.bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    b = nt.nodes['Principled BSDF']
    tc = nt.nodes.new('ShaderNodeTexCoord')
    ck = nt.nodes.new('ShaderNodeTexChecker')
    ck.inputs['Scale'].default_value = scale
    ck.inputs['Color1'].default_value = B.lin(c0)
    ck.inputs['Color2'].default_value = B.lin(c1)
    nt.links.new(tc.outputs['Object'], ck.inputs['Vector'])
    nt.links.new(ck.outputs['Color'], b.inputs['Base Color'])
    b.inputs['Roughness'].default_value = 0.6
    B.MATS[name] = m
    return m


# ---------------------------------------------------------------------------
# Ground
# ---------------------------------------------------------------------------

def grass_tile(g, variant):
    """a block: a slab of grass over soil; a little wider than its metre so
    neighbours meet without a seam"""
    grass = noise4('grass%d' % variant, '#5cae3e', '#84c94f', 0.37 * variant + 0.1, scale=3.0, rough=0.8,
                   bump=0.25)
    soil = noise4('soil', '#8a5631', '#a8703f', 0.0, scale=6.0, rough=0.9, bump=0.3)
    B.box((1.04, 1.02, -BOTTOM - 0.1), soil, g, loc=(0, 0.01, (BOTTOM - 0.1) / 2), bevel=0)
    B.box((1.04, 1.02, 0.12), grass, g, loc=(0, 0.01, -0.06), bevel=0)
    rnd = random.Random(11 + variant)
    tuft = B.mat('tuft', '#4f9a33', rough=0.7)
    for _ in range(5):
        x, y = rnd.uniform(-0.36, 0.36), rnd.uniform(-0.36, 0.36)
        for k in range(3):
            B.cyl(0.025, rnd.uniform(0.07, 0.11), tuft, g, loc=(x + 0.03 * (k - 1), y + rnd.uniform(-0.02, 0.02), 0),
                  rot=(rnd.uniform(-20, 20), rnd.uniform(-20, 20), 0), r2=0.002, segs=6)
    if variant >= 2:
        petals = [B.mat('flw', '#ffffff', rough=0.5), B.mat('fly', '#ffd84a', rough=0.5),
                  B.mat('flp', '#ff8fb8', rough=0.5)]
        heart = B.mat('flc', '#f2a33a', rough=0.5)
        for i in range(3 if variant == 2 else 4):
            x, y = rnd.uniform(-0.34, 0.34), rnd.uniform(-0.34, 0.34)
            pm = petals[(i + variant) % 3]
            for k in range(5):
                a = 2 * math.pi * k / 5
                B.sphere(0.028, pm, g, loc=(x + 0.03 * math.cos(a), y + 0.03 * math.sin(a), 0.045),
                         scale=(1, 1, 0.5), segs=10)
            B.sphere(0.02, heart, g, loc=(x, y, 0.055), segs=10)


def water_tile(g, frame):
    w = [0.0, 0.12, 0.24, 0.12][frame]
    water = noise4('water%d' % frame, '#2d8fd6', '#5cc3f0', w, scale=2.5, rough=0.1, bump=0.0, clear=0.6,
                   contrast=(0.35, 0.75))
    deep = B.mat('deep', '#1f6fae', rough=0.2)
    # the surface is a box of its own on top of the deep one: a surface
    # inside the deep box's top renders black in Cycles
    B.box((1.04, 1.02, WATER - 0.02 - BOTTOM), deep, g, loc=(0, 0.01, (WATER - 0.02 + BOTTOM) / 2), bevel=0)
    B.box((1.04, 1.02, 0.02), water, g, loc=(0, 0.01, WATER - 0.01), bevel=0)


def ground(parent, col, row):
    if row < 4:
        grass_tile(parent, row)
    else:
        water_tile(parent, row - 4)


# ---------------------------------------------------------------------------
# What stands on a tile
# ---------------------------------------------------------------------------

def rock(parent, col, row):
    stone = noise4('stone', '#9b958b', '#cdc6b8', 0.3, scale=5.0, rough=0.8, bump=0.4)
    moss = B.mat('moss', '#6aa84a', rough=0.9)
    rnd = random.Random(5 + col)
    big = B.ico(1.0, stone, parent, loc=(0.02, 0.02, 0.2), scale=(0.40, 0.34, 0.34), subdiv=3, smooth=True)
    B.displace(big, 0.10, 0.7)
    small = B.ico(1.0, stone, parent, loc=(-0.24 if col == 0 else 0.26, -0.22, 0.1), scale=(0.2, 0.18, 0.16),
                  subdiv=3, smooth=True)
    B.displace(small, 0.06, 0.5)
    for _ in range(3):
        a = rnd.uniform(0, 2 * math.pi)
        B.sphere(0.06, moss, parent, loc=(0.25 * math.cos(a), 0.2 * math.sin(a), 0.36), scale=(1.2, 1, 0.35),
                 segs=10)


def lettuce(parent, col, row):
    heart = B.mat('lett_in', '#c8ec7a', rough=0.6)
    leaf = [B.mat('lett_a', '#5fb83c', rough=0.55), B.mat('lett_b', '#86d24f', rough=0.55)]
    B.sphere(0.21, heart, parent, loc=(0, 0, 0.2), scale=(1, 1, 0.9))
    n = 9
    for i in range(n):
        a = 360.0 * i / n + 13 * col
        g = B.group('lf', parent, loc=(0, 0, 0.08), rot=(0, 0, a))
        lf = B.sphere(1.0, leaf[i % 2], g, loc=(0.19, 0, 0.11), rot=(0, -55, 0), scale=(0.21, 0.16, 0.05), segs=16)
        B.subsurf(lf, 1)


def key(parent, col, row):
    gold = B.mat('gold', '#f2c14e', rough=0.22, metal=1.0)
    bob = 0.04 * math.sin(2 * math.pi * col / 4)
    k = B.group('key', parent, loc=(0, 0, 0.42 + bob), rot=(0, -15, 20 + 10 * math.sin(2 * math.pi * col / 4)),
                scale=(1.5, 1.5, 1.5))
    B.torus(0.09, 0.03, gold, k, loc=(-0.15, 0, 0), rot=(90, 0, 0), segs=28, tsegs=10)
    B.cyl(0.028, 0.32, gold, k, loc=(-0.06, 0, 0), rot=(0, 90, 0), segs=14)
    B.box((0.05, 0.03, 0.09), gold, k, loc=(0.18, 0, -0.05), bevel=0.01)
    B.box((0.05, 0.03, 0.06), gold, k, loc=(0.24, 0, -0.035), bevel=0.01)
    B.sphere(0.018, B.glow('kspark', '#fff6d0', 6.0), k, loc=(-0.15, -0.04, 0.09))


def door(parent, col, row):
    stone = noise4('stone', '#9b958b', '#cdc6b8', 0.3, scale=5.0, rough=0.8, bump=0.4)
    wood = noise4('wood', '#a35f2b', '#c47c3c', 0.5, scale=9.0, rough=0.7)
    iron = B.mat('iron', '#454552', rough=0.4, metal=0.8)
    gold = B.mat('gold', '#f2c14e', rough=0.22, metal=1.0)
    g = B.group('door', parent, rot=(0, 0, 0 if col < 2 else 90))
    for sx in (-1, 1):
        B.box((0.16, 0.3, 1.0), stone, g, loc=(0.43 * sx, 0, 0.5), bevel=0.04)
    B.box((1.04, 0.32, 0.18), stone, g, loc=(0, 0, 1.06), bevel=0.04)
    hinge = B.group('hinge', g, loc=(-0.34, 0, 0), rot=(0, 0, 0 if col % 2 == 0 else -100))
    for i in range(4):
        B.box((0.17, 0.08, 0.94), wood, hinge, loc=(0.085 + 0.17 * i, 0, 0.48), bevel=0.015)
    for z in (0.2, 0.75):
        B.box((0.68, 0.1, 0.06), iron, hinge, loc=(0.34, 0, z), bevel=0.01)
    B.sphere(0.035, gold, hinge, loc=(0.58, -0.07, 0.48))
    B.box((0.03, 0.1, 0.07), iron, hinge, loc=(0.58, 0, 0.38), bevel=0.005)


def goal(parent, col, row):
    gold = B.mat('goalpad', '#ffcf4a', rough=0.35, clear=0.5)
    white = B.mat('goalrim', '#fff7e6', rough=0.4)
    pole = B.mat('pole', '#e9e4dc', rough=0.3, metal=0.3)
    B.cyl(0.4, 0.05, white, parent, loc=(0, 0, 0), segs=40)
    B.cyl(0.34, 0.07, gold, parent, loc=(0, 0, 0), segs=40)
    star = [(0.2 * (1 if k % 2 == 0 else 0.45) * math.cos(math.pi / 2 + k * math.pi / 5),
             0.2 * (1 if k % 2 == 0 else 0.45) * math.sin(math.pi / 2 + k * math.pi / 5)) for k in range(10)]
    B.prism(star, 0.07, 0.085, white, parent, bevel=0.008)
    px, py = 0.28, 0.3
    B.cyl(0.03, 1.5, pole, parent, loc=(px, py, 0), segs=12)
    B.sphere(0.055, gold, parent, loc=(px, py, 1.52))
    # the flag: a waving cloth, checkered like a finish line
    cloth = checker('flag', '#1d1a22', '#ffffff', 9.0)
    W, H, nx, nz = 0.62, 0.4, 12, 6
    ph = 2 * math.pi * col / 4
    verts, faces = [], []
    for j in range(nz + 1):
        for i in range(nx + 1):
            u = i / nx
            x = u * W
            y = 0.06 * u * math.sin(2 * math.pi * u * 1.2 - ph)
            verts.append((x, y, 1.46 - H * j / nz))
    for j in range(nz):
        for i in range(nx):
            a = j * (nx + 1) + i
            faces.append((a, a + 1, a + nx + 2, a + nx + 1))
    f = B.mesh(verts, faces, cloth, parent, loc=(px, py, 0), rot=(0, 0, 150), smooth=True)
    f.modifiers.new('solid', 'SOLIDIFY').thickness = 0.012


def bridge(parent, col, row):
    wood = noise4('plank', '#b0703a', '#cf9152', 0.7, scale=7.0, rough=0.75)
    dark = B.mat('beam', '#7a4824', rough=0.8)
    rope = B.mat('rope', '#e8d3a2', rough=0.9)
    g = B.group('bridge', parent, rot=(0, 0, 0 if col == 0 else 90))
    for sy in (-1, 1):
        B.box((1.06, 0.08, 0.12), dark, g, loc=(0, 0.42 * sy, -0.09), bevel=0.01)
    for i in range(7):
        x = -0.45 + 0.15 * i
        B.box((0.13, 0.9, 0.05), wood, g, loc=(x, 0, -0.025), rot=(0, 0, (i % 3 - 1) * 1.5), bevel=0.012)
    for sx in (-1, 1):
        for sy in (-1, 1):
            B.cyl(0.035, 0.26, dark, g, loc=(0.42 * sx, 0.44 * sy, -0.05), segs=10)
    for sy in (-1, 1):
        B.tube([(-0.42, 0.44 * sy, 0.18), (0, 0.44 * sy, 0.12), (0.42, 0.44 * sy, 0.18)], 0.015, rope, g, segs=6)


SHEETS = [
    ('tg_ground', ground, 1, 8, GROUND_CELL, GROUND_ANCHOR),
    ('tg_rock', rock, 2, 1, DECOR_CELL, DECOR_ANCHOR),
    ('tg_lettuce', lettuce, 1, 1, DECOR_CELL, DECOR_ANCHOR),
    ('tg_key', key, 4, 1, DECOR_CELL, DECOR_ANCHOR),
    ('tg_door', door, 4, 1, DECOR_CELL, DECOR_ANCHOR),
    ('tg_goal', goal, 4, 1, DECOR_CELL, DECOR_ANCHOR),
    ('tg_bridge', bridge, 2, 1, DECOR_CELL, DECOR_ANCHOR),
]


def main():
    out = B.arg('--out', os.path.join(B.HERE, '..', '..', 'build', 'art'))
    only = B.arg('--only')
    B.setup()
    B.studio()
    for name, build, cols, rows, cell, anchor in SHEETS:
        if only and only not in name:
            continue
        B.render_sheet(name, build, out, cols, rows, cell, anchor, meta=dict(kind=name))


if __name__ == '__main__':
    main()
