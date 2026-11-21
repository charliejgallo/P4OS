"""
METEGOL - the figure, rendered once in 32 turns about its rod, as three
pictures the pack puts together (tools/pack_p4.py, main/mg_kit.h):

  figures.png        the light it takes, all of it white (shade, alpha)
  figures_part.png   which part each pixel is (skin, hair, shirt, sleeve,
                     collar, shorts, socks, boots), red = part / 16
  figures_uvw.png    where on the body: forward, left (the figure's own
                     space) and the height within its part

The kits are painted on the board from these: one render for every team.

    Blender -b -P figures.py [-- --samples 64]

The figure is a table-football player as they are moulded: legs together
down to the boots, the rod through the waist, the arms along the body,
and the upper body a little longer than life so that the shirt reads from
the camera's height.
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bpy  # noqa: E402
import bmesh  # noqa: E402
from mathutils import Vector  # noqa: E402

import mg_common as C  # noqa: E402

FRAMES, COLS = 32, 8
CELL_W, CELL_H = 176, 144
ANCHOR = (88, 88)               # the ground under the rod, inside a cell

PARTS = {'skin': 1, 'hair': 2, 'shirt': 3, 'sleeve': 4, 'collar': 5, 'shorts': 6, 'socks': 7, 'boots': 8, 'raw': 9}
# the height that counts for each part's design (main/mg_kit.c): the torso's
# for the shirt, the cuff's for the sleeves, the socks' own
ZSPAN = {'shirt': (58, 124), 'collar': (58, 124), 'sleeve': (104, 118), 'socks': (9, 31)}


def materials(root):
    """three materials a part: white for the light, its number, its place"""
    out = {}
    for name, pid in PARTS.items():
        beauty = C.mat_principled('w_' + name, (0.8, 0.8, 0.8), rough=0.42 if name != 'hair' else 0.7, spec=0.45)
        part = C.mat_emit('p_' + name, (pid / 16.0, 0, 0))
        m = bpy.data.materials.new('u_' + name)
        m.use_nodes = True
        nt = m.node_tree
        for n in list(nt.nodes):
            nt.nodes.remove(n)
        tc = nt.nodes.new('ShaderNodeTexCoord')
        tc.object = root
        sep = nt.nodes.new('ShaderNodeSeparateXYZ')
        nt.links.new(tc.outputs['Object'], sep.inputs[0])
        z0, z1 = ZSPAN.get(name, (58, 124))

        def lin(inp, mul, add):
            n = nt.nodes.new('ShaderNodeMath')
            n.operation = 'MULTIPLY_ADD'
            n.use_clamp = True
            nt.links.new(inp, n.inputs[0])
            n.inputs[1].default_value = mul
            n.inputs[2].default_value = add
            return n.outputs[0]
        # the root sits on the rod's axis: x forward, y left, z up, in metres
        f = lin(sep.outputs[0], 1000.0 / 13.5 * 0.5, 0.5)
        l = lin(sep.outputs[1], 1000.0 / 25.0 * 0.5, 0.5)
        z = lin(sep.outputs[2], 1000.0 / (z1 - z0), (C.ROD_Z - z0) / (z1 - z0))
        comb = nt.nodes.new('ShaderNodeCombineXYZ')
        nt.links.new(f, comb.inputs[0])
        nt.links.new(l, comb.inputs[1])
        nt.links.new(z, comb.inputs[2])
        e = nt.nodes.new('ShaderNodeEmission')
        nt.links.new(comb.outputs[0], e.inputs[0])
        o = nt.nodes.new('ShaderNodeOutputMaterial')
        nt.links.new(e.outputs[0], o.inputs[0])
        out[name] = (beauty, part, m)
    return out


def tapered_torso(name, z0, z1, d, wb, wt, mat):
    """a box, narrower at the waist than at the shoulders"""
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    for v in bm.verts:
        top = v.co.z > 0
        w = wt if top else wb
        v.co.x = v.co.x * d / 1000.0
        v.co.y = v.co.y * w / 1000.0
        v.co.z = (z1 if top else z0) / 1000.0
    o = C.mesh_obj(name, bm, mat, smooth=True)
    m = o.modifiers.new('bevel', 'BEVEL')
    m.width = 0.0075
    m.segments = 4
    m.limit_method = 'NONE'
    return o


def figure():
    """the figure at the origin, facing +x, its rod's axis at ROD_Z; its
    objects, each with the part it is"""
    objs = []

    def add(o, part):
        objs.append((o, part))
        return o
    for s in (-1, 1):
        y = s * 7.0
        add(C.box('boot', -7, y - 6, 1, 13, y + 6, 11, bevel=3.5, seg=3), 'boots')
        add(C.box('sock', -6, y - 5.5, 9, 6, y + 5.5, 31, bevel=4.5, seg=3), 'socks')
        add(C.box('knee', -5.5, y - 5, 29, 5.5, y + 5, 37, bevel=4, seg=3), 'skin')
    add(C.box('shorts', -12, -17, 34, 12, 17, 61, bevel=6, seg=3), 'shorts')
    add(tapered_torso('torso', 58, 124, 27, 34, 50, None), 'shirt')
    add(C.cyl('collar', (0, 0, 119), (0, 0, 127), 10.0, verts=28), 'collar')
    add(C.cyl('neck', (0, 0, 122), (0, 0, 133), 6.5, verts=20), 'skin')
    add(C.sphere('head', (0.5, 0, 143), 13.0, scale=(1.0, 0.95, 1.08)), 'skin')
    add(C.sphere('hair', (-2.0, 0, 146.5), 13.3, scale=(1.0, 0.98, 0.95)), 'hair')
    add(C.sphere('nose', (12.5, 0, 141), 2.6), 'skin')
    for s in (-1, 1):
        y = s * 27.5
        add(C.cyl('sleeve', (0, y, 119), (1, y * 1.02, 103), 7.2, verts=20), 'sleeve')
        add(C.cyl('arm', (1, y * 1.02, 104), (2, y * 0.98, 82), 5.4, verts=16), 'skin')
        add(C.sphere('hand', (2.5, y * 0.97, 79), 6.0), 'skin')
    return objs


def main():
    samples = int(C.arg('--samples', 64))
    C.setup(samples)
    W_PX, H_PX = CELL_W * COLS, CELL_H * (FRAMES // COLS)
    # the sheet's centre is the screen's (640, 456): the field's centre
    C.camera(C.CX, C.CY, W_PX, H_PX)
    C.studio()

    roots = []
    mats = None
    parts_of = []
    for k in range(FRAMES):
        c, r = k % COLS, k // COLS
        ax, ay = c * CELL_W + ANCHOR[0], r * CELL_H + ANCHOR[1]
        sx = C.CX + ax - W_PX / 2
        sy = C.CY + ay - H_PX / 2
        gx = (sx - C.CX) / C.S
        gy = (sy - C.CY) / (C.S * C.COS_T)
        root = bpy.data.objects.new('fig%02d' % k, None)
        C.sc.collection.objects.link(root)
        if mats is None:
            mats = materials(root)
        objs = figure()
        for (o, part) in objs:
            o.data.materials.clear()
            o.data.materials.append(mats[part][0])
            o.parent = root
        root.location = C.W(gx, gy, C.ROD_Z)
        # an angle k / 32 of a turn, the foot towards +x first: about the
        # rod (Blender's Y), negative is the foot forward
        a = 2.0 * math.pi * k / FRAMES
        root.rotation_euler = (0, -a, 0)
        # each figure's own coordinates for the third picture
        for (o, part) in objs:
            o.location = o.location - C.W(0, 0, C.ROD_Z)
        roots.append(root)
        parts_of.append(objs)
    # the coordinates' materials read the first root: every figure needs its own
    uv_mats = {}
    for k, objs in enumerate(parts_of):
        for (o, part) in objs:
            m = mats[part][2]
            key = (k, part)
            if key not in uv_mats:
                mm = m.copy()
                mm.node_tree.nodes['Texture Coordinate'].object = roots[k]
                uv_mats[key] = mm

    out = os.path.join(C.ASSETS, 'figures')
    # 1: the light
    C.render(os.path.join(out, 'figures.png'))
    # 2 and 3: flat, one sample, no filter: values, not pictures
    C.sc.cycles.samples = 1
    C.sc.cycles.use_denoising = False
    C.sc.cycles.filter_width = 0.01
    C.sc.view_settings.view_transform = 'Raw'
    for k, objs in enumerate(parts_of):
        for (o, part) in objs:
            o.data.materials.clear()
            o.data.materials.append(mats[part][1])
    C.render(os.path.join(out, 'figures_part.png'))
    for k, objs in enumerate(parts_of):
        for (o, part) in objs:
            o.data.materials.clear()
            o.data.materials.append(uv_mats[(k, part)])
    C.render(os.path.join(out, 'figures_uvw.png'))
    import json
    json.dump(dict(frames=FRAMES, cols=COLS, cell=[CELL_W, CELL_H], anchor=list(ANCHOR)),
              open(os.path.join(out, 'figures.json'), 'w'))


main()
