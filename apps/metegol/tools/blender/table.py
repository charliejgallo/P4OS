"""
METEGOL - the table, its rods and the ball

    Blender -b -P table.py [-- --samples 128]

  assets/table/table.png        the table alone, 1280 x 720, on transparency,
                                with its shadow (a stadium's picture goes under
                                it: tools/pack_p4.py; stadiums.py renders it
                                again in each stadium's light)
  assets/table/near.png, far.png  which pixels are the near and the far band:
                                they are drawn again over the rods and the
                                figures they hide
  assets/rods/*.png             the parts of a rod: the shaft (one row of it is
                                the profile the board repeats), the bumpers,
                                the handles, the ends
  assets/ball/ball.png          the ball rolling, 8 frames

build_table() is what stadiums.py calls.
"""
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bpy  # noqa: E402
import bmesh  # noqa: E402
import numpy as np  # noqa: E402

import mg_common as C  # noqa: E402

FX, FY = C.FIELD_X, C.FIELD_Y
WH, WT, ET = C.WALL_H, C.WALL_T, C.END_T
BODY = -150.0                   # the box goes down to here


def field_image():
    """the playing surface: grass cut in bands, white lines"""
    w, h = 1200, 680            # a pixel a millimetre
    img = np.zeros((h, w, 4), np.float32)
    x = np.arange(w)[None, :] - w / 2 + 0.5
    y = np.arange(h)[:, None] - h / 2 + 0.5
    band = (np.floor((x + 600) / 100).astype(int) & 1)
    g1 = np.array(C.hexc('2F8F3A')), np.array(C.hexc('287F33'))
    base = np.where(band[..., None] == 1, g1[0], g1[1]) * np.ones((h, w, 1))
    # a little noise, so it is not plastic
    rng = np.random.default_rng(7)
    base *= (0.94 + 0.06 * rng.random((h, w, 1)))
    lw = 3.0
    line = np.zeros((h, w), bool)
    line |= np.abs(x) < lw / 2                                  # halfway
    r = np.sqrt(x * x + y * y)
    line |= np.abs(r - 90) < lw / 2                             # centre circle
    line |= r < 5                                               # the spot
    ax = np.abs(x)
    line |= (np.abs(ax - (FX - 110)) < lw / 2) & (np.abs(y) < 160)   # the boxes
    line |= (ax > FX - 110) & (np.abs(np.abs(y) - 160) < lw / 2)
    line |= (np.abs(ax - (FX - 45)) < lw / 2) & (np.abs(y) < 75)
    line |= (ax > FX - 45) & (np.abs(np.abs(y) - 75) < lw / 2)
    line |= np.sqrt((ax - (FX - 80)) ** 2 + y * y) < 4
    line |= (ax > FX - lw) & (np.abs(y) < FY)                  # the goal lines
    line |= (np.abs(y) > FY - lw)
    white = np.array(C.hexc('F2F2EA'))
    img[..., :3] = np.where(line[..., None], white, base)
    img[..., 3] = 1
    im = bpy.data.images.new('field', w, h, float_buffer=True)
    im.colorspace_settings.name = 'Linear'
    # Blender's rows go up: the far side (y < 0) is the image's top
    im.pixels.foreach_set(np.ascontiguousarray(img[::-1]).ravel())
    im.update()
    im.pack()
    return im


def mat_field():
    m = bpy.data.materials.new('field')
    m.use_nodes = True
    nt = m.node_tree
    b = nt.nodes['Principled BSDF']
    b.inputs['Roughness'].default_value = 0.85
    b.inputs['Specular'].default_value = 0.2
    t = nt.nodes.new('ShaderNodeTexImage')
    t.image = field_image()
    t.interpolation = 'Closest'
    nt.links.new(t.outputs[0], b.inputs['Base Color'])
    return m


def mat_wood(name, col, dark):
    """lacquered wood: rings from a wave texture"""
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    b = nt.nodes['Principled BSDF']
    b.inputs['Roughness'].default_value = 0.32
    b.inputs['Specular'].default_value = 0.55
    wv = nt.nodes.new('ShaderNodeTexWave')
    wv.wave_type = 'RINGS'
    wv.inputs['Scale'].default_value = 2.5
    wv.inputs['Distortion'].default_value = 3.0
    wv.inputs['Detail'].default_value = 2.0
    wv.inputs['Detail Scale'].default_value = 1.5
    ramp = nt.nodes.new('ShaderNodeValToRGB')
    ramp.color_ramp.elements[0].color = (*C.hexc(dark), 1)
    ramp.color_ramp.elements[1].color = (*C.hexc(col), 1)
    nt.links.new(wv.outputs['Fac'], ramp.inputs[0])
    nt.links.new(ramp.outputs[0], b.inputs['Base Color'])
    return m


def rounded_corner(name, cx, cy, sx, sy, mat):
    """a quarter of a column filling a corner of the field, radius CORNER_R"""
    R = C.CORNER_R
    bm = bmesh.new()
    corner = (cx + sx * R, cy + sy * R)     # the field's true corner
    centre = (cx, cy)                       # the arc's centre
    arc = [(centre[0] + sx * R * math.cos(a), centre[1] + sy * R * math.sin(a))
           for a in np.linspace(0, math.pi / 2, 13)]
    ring = [corner] + arc
    vb = [bm.verts.new(C.W(px, py, 0)) for (px, py) in ring]
    vt = [bm.verts.new(C.W(px, py, WH)) for (px, py) in ring]
    bm.faces.new(vb)
    bm.faces.new(list(reversed(vt)))
    n = len(ring)
    for i in range(n):
        j = (i + 1) % n
        bm.faces.new([vb[i], vb[j], vt[j], vt[i]])
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    return C.mesh_obj(name, bm, mat, smooth=False)


def build_table():
    """the table at the field's place; returns {'near': [...], 'far': [...], 'all': [...]}"""
    wood = mat_wood('wood', 'A8673A', '8C5230')
    wood_top = mat_wood('wood_top', 'C08452', 'A86E40')
    inner = C.mat_principled('inner', C.hexc('235C2E'), rough=0.7)
    metal = C.mat_principled('metal', C.hexc('D8DCE2'), rough=0.18, metal=1.0)
    white = C.mat_principled('post', C.hexc('F4F4F4'), rough=0.3)
    dark = C.mat_principled('goal_in', C.hexc('15181C'), rough=0.8)
    net = C.mat_principled('net', C.hexc('EDEDED'), rough=0.6)
    groups = {'near': [], 'far': [], 'all': []}

    def add(o, g=None):
        groups['all'].append(o)
        if g:
            groups[g].append(o)
        return o
    # the field, a little bigger than its lines so no gap shows under the bands
    bm = bmesh.new()
    bmesh.ops.create_grid(bm, x_segments=1, y_segments=1, size=0.5)
    for v in bm.verts:
        v.co.x *= 2 * FX / 1000.0
        v.co.y *= 2 * FY / 1000.0
    uv = bm.loops.layers.uv.new()
    for f in bm.faces:
        for lp in f.loops:
            lp[uv].uv = (lp.vert.co.x / (2 * FX / 1000.0) + 0.5, lp.vert.co.y / (2 * FY / 1000.0) + 0.5)
    add(C.mesh_obj('field', bm, mat_field(), smooth=False))
    # the long bands: inner face dark, the rest wood, a lighter top
    for side, g in ((-1, 'far'), (1, 'near')):
        y0, y1 = (FY, FY + WT) if side > 0 else (-FY - WT, -FY)
        add(C.box('band_%s' % g, -FX - ET, y0, BODY, FX + ET, y1, WH - 4, wood, bevel=1.5), g)
        add(C.box('top_%s' % g, -FX - ET - 2, y0 - 1, WH - 5, FX + ET + 2, y1 + 1, WH, wood_top, bevel=2.5, seg=3), g)
        # the bushings where the rods go through, inside and outside
        for k in range(4):
            for s2 in (1, -1):
                x = C.ROD_X[k] * s2
                for yy in (y0 - 0.5, y1 + 0.5):
                    o = C.cyl('bush', (x, yy - 1.5 * side, C.ROD_Z), (x, yy + 1.5 * side, C.ROD_Z), C.ROD_R + 5, metal, verts=24)
                    add(o, g)
    # the ends, with the goal's mouth
    for sx in (-1, 1):
        x0, x1 = (FX, FX + ET) if sx > 0 else (-FX - ET, -FX)
        add(C.box('end_a', x0, -FY - WT, BODY, x1, -C.GOAL_Y, WH, wood, bevel=1.5))
        add(C.box('end_b', x0, C.GOAL_Y, BODY, x1, FY + WT, WH, wood, bevel=1.5))
        add(C.box('end_top', x0, -C.GOAL_Y, 58, x1, C.GOAL_Y, WH, wood, bevel=1.5))
        add(C.box('end_low', x0, -C.GOAL_Y, BODY, x1, C.GOAL_Y, -2, wood))
        # the goal: a box behind the mouth, dark inside, white posts and a net on top
        gx0, gx1 = (FX + ET, FX + C.GOAL_D + 10) if sx > 0 else (-FX - C.GOAL_D - 10, -FX - ET)
        add(C.box('goal_floor', min(gx0, gx1) - (ET if sx > 0 else 0), -C.GOAL_Y - 8, BODY, max(gx0, gx1), C.GOAL_Y + 8, -1, dark))
        add(C.box('goal_back', (FX + C.GOAL_D) * sx - 5, -C.GOAL_Y - 8, -1, (FX + C.GOAL_D) * sx + 5, C.GOAL_Y + 8, 60, wood))
        for sy in (-1, 1):
            add(C.box('goal_side', min(gx0, gx1), sy * C.GOAL_Y - 5 + (5 if sy > 0 else -3), -1,
                      max(gx0, gx1), sy * C.GOAL_Y + 5 + (5 if sy > 0 else -3), 60, wood))
            add(C.cyl('post', (FX * sx, sy * C.GOAL_Y, 0), (FX * sx, sy * C.GOAL_Y, 60), 5.0, white, verts=16))
        add(C.cyl('bar', (FX * sx, -C.GOAL_Y, 60), (FX * sx, C.GOAL_Y, 60), 5.0, white, verts=16))
        # the net: a grid of thin bars over the goal
        for i in range(9):
            t = i / 8
            xx = FX * sx + sx * (C.GOAL_D + 5) * t
            add(C.cyl('net', (xx, -C.GOAL_Y, 62 - 4 * t), (xx, C.GOAL_Y, 62 - 4 * t), 0.9, net, verts=6))
        for j in range(13):
            yy = -C.GOAL_Y + 2 * C.GOAL_Y * j / 12
            add(C.cyl('net', (FX * sx, yy, 62), (FX * sx + sx * (C.GOAL_D + 5), yy, 58), 0.9, net, verts=6))
    # the outer corners, where a band meets an end
    for sx in (-1, 1):
        for sy in (-1, 1):
            x0, x1 = sorted((sx * FX, sx * (FX + ET + 2)))
            y0, y1 = sorted((sy * FY, sy * (FY + WT + 1)))
            add(C.box('cornerpost', x0, y0, BODY, x1, y1, WH + 1.5, wood_top, bevel=2.5, seg=3), 'near' if sy > 0 else 'far')
    # the rounded corners, inside
    R = C.CORNER_R
    for sx in (-1, 1):
        for sy in (-1, 1):
            o = rounded_corner('corner', sx * (FX - R), sy * (FY - R), sx, sy, inner)
            add(o, 'near' if sy > 0 else 'far')
    # the inner faces of the long bands, the field's colour darker
    for side, g in ((-1, 'far'), (1, 'near')):
        yy = FY * side
        add(C.box('lining', -FX, yy - 0.8 * (1 if side > 0 else -1) - (0.6 if side > 0 else 0), 0, FX,
                  yy + (0.6 if side < 0 else 0), WH - 5, inner), g)
    # the abacus of the score on the ends: beads on a wire, left to count
    bead = C.mat_principled('bead', C.hexc('F0F0F0'), rough=0.25)
    for sx in (-1, 1):
        x = sx * (FX + ET * 0.5)
        add(C.cyl('wire', (x, -FY + 20, WH + 14), (x, -C.GOAL_Y - 10, WH + 14), 1.2, metal, verts=8))
        for i in range(5):
            add(C.sphere('bead', (x, -FY + 30 + i * 13, WH + 14), 6.0, bead, seg=16))
    # what the table stands on, to catch its shadow
    bm = bmesh.new()
    bmesh.ops.create_grid(bm, x_segments=1, y_segments=1, size=4.0)
    for v in bm.verts:
        v.co.z = -0.75
    g = C.mesh_obj('ground', bm, C.mat_principled('ground', (0.2, 0.3, 0.2)), smooth=False)
    g.is_shadow_catcher = True
    return groups


def holdout_mat():
    m = bpy.data.materials.new('holdout')
    m.use_nodes = True
    nt = m.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    h = nt.nodes.new('ShaderNodeHoldout')
    o = nt.nodes.new('ShaderNodeOutputMaterial')
    nt.links.new(h.outputs[0], o.inputs[0])
    return m


def render_masks(groups, out):
    """near.png and far.png: each band alone, white, the rest held out"""
    hold = holdout_mat()
    white = C.mat_emit('maskwhite', (1, 1, 1))
    keep = {o.name: [s.material for s in o.material_slots] for o in groups['all']}
    C.sc.cycles.samples = 16
    C.sc.cycles.use_denoising = False
    gnd = bpy.data.objects.get('ground')
    if gnd:
        gnd.hide_render = True
    for which in ('near', 'far'):
        names = {o.name for o in groups[which]}
        for o in groups['all']:
            m = white if o.name in names else hold
            for s in o.material_slots:
                s.material = m
            if not o.material_slots:
                o.data.materials.append(m)
        C.render(os.path.join(out, which + '.png'))
    for o in groups['all']:
        for s, m in zip(o.material_slots, keep[o.name]):
            s.material = m
    if gnd:
        gnd.hide_render = False


# ---------------------------------------------------------------------------
# The rods and the ball: small sheets, framed around their own anchors
# ---------------------------------------------------------------------------

def rod_parts(out, samples):
    """each part of a rod in its own picture with its anchor written down"""
    info = {}
    M = {}
    # each part is built around the world's origin at the rod's height and
    # rendered in a frame whose centre is that point's screen position
    parts = {
        'shaft': lambda: [C.cyl('s', (0, -60, C.ROD_Z), (0, 60, C.ROD_Z), C.ROD_R, M['steel'], verts=32)],
        'bumper': lambda: [C.cyl('b', (0, -6, C.ROD_Z), (0, 6, C.ROD_Z), C.ROD_R + 6.5, M['bumper'], verts=32)],
        'handle_near': lambda: [C.cyl('h', (0, 0, C.ROD_Z), (0, 115, C.ROD_Z), 13.0, M['grip'], verts=32),
                                C.cyl('hc', (0, 115, C.ROD_Z), (0, 122, C.ROD_Z), 11.0, M['cap'], verts=32, r1=9.0)],
        'handle_far': lambda: [C.cyl('h', (0, 0, C.ROD_Z), (0, -115, C.ROD_Z), 13.0, M['grip'], verts=32),
                               C.cyl('hc', (0, -115, C.ROD_Z), (0, -122, C.ROD_Z), 11.0, M['cap'], verts=32, r1=9.0)],
        'end_near': lambda: [C.cyl('e', (0, 0, C.ROD_Z), (0, 8, C.ROD_Z), C.ROD_R + 1.5, M['grip'], verts=32)],
        'end_far': lambda: [C.cyl('e', (0, 0, C.ROD_Z), (0, -8, C.ROD_Z), C.ROD_R + 1.5, M['grip'], verts=32)],
    }
    for name, make in parts.items():
        C.setup(samples)
        C.studio()
        globals_ = dict(steel=C.mat_principled('steel', C.hexc('CDD3DA'), rough=0.16, metal=1.0),
                        grip=C.mat_principled('grip', C.hexc('1B1B1F'), rough=0.55),
                        cap=C.mat_principled('cap', C.hexc('C9302C'), rough=0.4),
                        bumper=C.mat_principled('bumper', C.hexc('111111'), rough=0.7))
        M.update(globals_)
        objs = make()
        # big enough for the handle
        w, h = 48, 128
        cx, cy = C.screen(0, 0, C.ROD_Z)
        C.camera(cx, cy, w, h)
        C.render(os.path.join(out, name + '.png'))
        info[name] = dict(anchor=[w // 2, h // 2])
        for o in objs:
            bpy.data.objects.remove(o)
    json.dump(info, open(os.path.join(out, 'rods.json'), 'w'))


def ball(out, samples):
    C.setup(samples)
    C.studio()
    shell = C.mat_principled('ballw', C.hexc('F7F3E8'), rough=0.3, spec=0.6)
    stripe = C.mat_principled('balls', C.hexc('1E4DB0'), rough=0.3, spec=0.6)
    n, cell = 8, 40
    C.camera(C.CX, C.CY, cell * n, cell)
    for k in range(n):
        sx = C.CX - cell * n / 2 + cell * (k + 0.5)
        gx = (sx - C.CX) / C.S
        # the ball's centre lands on the frame's middle row
        gy = C.BALL_R * C.SIN_T / C.COS_T
        o = C.sphere('ball', (gx, gy, C.BALL_R), C.BALL_R, shell, seg=48)
        o.data.materials.append(stripe)
        # a band round it, to see it roll: faces near a great circle
        for p in o.data.polygons:
            c = p.center
            if abs(c.x) < 0.0035:
                p.material_index = 1
        o.rotation_euler = (0, 0, 0)
        o.rotation_mode = 'XYZ'
        o.rotation_euler = (2 * math.pi * k / n, 0.6, 0.3)
    C.render(os.path.join(out, 'ball.png'))
    json.dump(dict(frames=n, cell=[cell, cell], anchor=[cell // 2, cell // 2]), open(os.path.join(out, 'ball.json'), 'w'))


def main():
    samples = int(C.arg('--samples', 96))
    out = os.path.join(C.ASSETS, 'table')
    if not C.flag('--no-table'):
        C.setup(samples)
        C.camera(C.SCREEN_W / 2, C.SCREEN_H / 2, C.SCREEN_W, C.SCREEN_H)
        C.studio()
        groups = build_table()
        C.render(os.path.join(out, 'table.png'))
        render_masks(groups, out)
    rod_parts(os.path.join(C.ASSETS, 'rods'), samples)
    ball(os.path.join(C.ASSETS, 'ball'), samples)


if __name__ == '__main__':
    main()
