"""
METEGOL - the stadiums: what is seen around the table

    Blender -b -P stadiums.py [-- --only rio,tokio] [--samples 64]

Each stadium is built at its real size around a pitch, with the table
standing at the centre spot, and seen from a camera at the side of the
pitch, high up: the far stand, its roof and its lights, the sky and what
stands beyond it fill the band over the table, the grass and the
stadium's ends its sides. tools/pack_p4.py puts the table (table.py) over
it. Nothing has a name, a logo or an advert: a stadium is known by its
shape and its place.

  assets/stadiums/<id>/backdrop.png    1280 x 720
  assets/stadiums/<id>/crowd<k>.png    the band of the stands, the crowd
                                       moving, 4 frames
  assets/stadiums/<id>/crowd.json      where that band goes, the flashes
"""
import json
import math
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bpy  # noqa: E402
import bmesh  # noqa: E402
from mathutils import Vector  # noqa: E402

import mg_common as C  # noqa: E402

PITCH_X, PITCH_Y = 52.5, 34.0
CROWD_H = 180            # the rows of the screen the crowd's frames cover
FRAMES = 4

# ---------------------------------------------------------------------------
# Materials
# ---------------------------------------------------------------------------


def mat(name, col, rough=0.6, metal=0.0, emit=None):
    return C.mat_principled(name, C.hexc(col), rough=rough, metal=metal,
                            emit=(C.hexc(emit[0]), emit[1]) if emit else None)


def mat_vcol(name):
    """a material that takes each face's colour from the mesh"""
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    b = nt.nodes['Principled BSDF']
    b.inputs['Roughness'].default_value = 0.8
    at = nt.nodes.new('ShaderNodeAttribute')
    at.attribute_name = 'Col'
    nt.links.new(at.outputs['Color'], b.inputs['Base Color'])
    return m


def mat_grass():
    m = bpy.data.materials.new('grass')
    m.use_nodes = True
    nt = m.node_tree
    b = nt.nodes['Principled BSDF']
    b.inputs['Roughness'].default_value = 0.9
    b.inputs['Specular'].default_value = 0.15
    tc = nt.nodes.new('ShaderNodeTexCoord')
    sep = nt.nodes.new('ShaderNodeSeparateXYZ')
    nt.links.new(tc.outputs['Object'], sep.inputs[0])
    wave = nt.nodes.new('ShaderNodeMath')
    wave.operation = 'SINE'
    mul = nt.nodes.new('ShaderNodeMath')
    mul.operation = 'MULTIPLY'
    mul.inputs[1].default_value = math.pi / 5.25
    nt.links.new(sep.outputs[0], mul.inputs[0])
    nt.links.new(mul.outputs[0], wave.inputs[0])
    gt = nt.nodes.new('ShaderNodeMath')
    gt.operation = 'GREATER_THAN'
    gt.inputs[1].default_value = 0.0
    nt.links.new(wave.outputs[0], gt.inputs[0])
    mix = nt.nodes.new('ShaderNodeMixRGB')
    mix.inputs[1].default_value = (*C.hexc('2C7A30'), 1)
    mix.inputs[2].default_value = (*C.hexc('348A38'), 1)
    nt.links.new(gt.outputs[0], mix.inputs[0])
    nt.links.new(mix.outputs[0], b.inputs['Base Color'])
    return m


def sky(top, horizon, strength=1.0):
    w = C.sc.world
    nt = w.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    tc = nt.nodes.new('ShaderNodeTexCoord')
    sep = nt.nodes.new('ShaderNodeSeparateXYZ')
    nt.links.new(tc.outputs['Generated'], sep.inputs[0])
    ramp = nt.nodes.new('ShaderNodeValToRGB')
    ramp.color_ramp.elements[0].position = 0.0
    ramp.color_ramp.elements[0].color = (*C.hexc(horizon), 1)
    ramp.color_ramp.elements[1].position = 0.45
    ramp.color_ramp.elements[1].color = (*C.hexc(top), 1)
    nt.links.new(sep.outputs[2], ramp.inputs[0])
    bg = nt.nodes.new('ShaderNodeBackground')
    bg.inputs[1].default_value = strength
    nt.links.new(ramp.outputs[0], bg.inputs[0])
    out = nt.nodes.new('ShaderNodeOutputWorld')
    nt.links.new(bg.outputs[0], out.inputs[0])


def sun(rot, energy, col='FFF4E0', angle=2.0):
    ld = bpy.data.lights.new('sun', 'SUN')
    ld.energy = energy
    ld.color = C.hexc(col)
    ld.angle = math.radians(angle)
    o = bpy.data.objects.new('sun', ld)
    o.rotation_euler = [math.radians(v) for v in rot]
    C.sc.collection.objects.link(o)


def area(loc, size, energy, col='FFFFFF', rot=(0, 0, 0)):
    ld = bpy.data.lights.new('area', 'AREA')
    ld.energy = energy
    ld.size = size
    ld.color = C.hexc(col)
    o = bpy.data.objects.new('area', ld)
    o.location = loc
    o.rotation_euler = [math.radians(v) for v in rot]
    C.sc.collection.objects.link(o)


# ---------------------------------------------------------------------------
# Geometry in metres (Blender's own: X along the pitch, Y away from the
# camera, Z up)
# ---------------------------------------------------------------------------

def obj(name, bm, m, smooth=False):
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    o = bpy.data.objects.new(name, me)
    if m:
        if isinstance(m, list):
            for mm in m:
                o.data.materials.append(mm)
        else:
            o.data.materials.append(m)
    if smooth:
        for p in me.polygons:
            p.use_smooth = True
    C.sc.collection.objects.link(o)
    return o


def cube(name, c, s, m, rot_z=0.0):
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    for v in bm.verts:
        v.co = Vector((v.co.x * s[0], v.co.y * s[1], v.co.z * s[2]))
    o = obj(name, bm, m)
    o.location = c
    o.rotation_euler = (0, 0, rot_z)
    return o


def ground(m, size=900):
    bm = bmesh.new()
    bmesh.ops.create_grid(bm, x_segments=1, y_segments=1, size=size)
    return obj('ground', bm, m)


def pitch(m_grass, m_line, track=None):
    bm = bmesh.new()
    bmesh.ops.create_grid(bm, x_segments=1, y_segments=1, size=1.0)
    for v in bm.verts:
        v.co.x *= PITCH_X + 6
        v.co.y *= PITCH_Y + 6
        v.co.z = 0.02
    obj('pitch', bm, m_grass)
    # a few lines: the touchlines and the boxes show at the sides
    for (x0, y0, x1, y1) in ((-PITCH_X, -PITCH_Y, PITCH_X, -PITCH_Y), (-PITCH_X, PITCH_Y, PITCH_X, PITCH_Y),
                             (-PITCH_X, -PITCH_Y, -PITCH_X, PITCH_Y), (PITCH_X, -PITCH_Y, PITCH_X, PITCH_Y),
                             (-PITCH_X + 16.5, -20.15, -PITCH_X + 16.5, 20.15), (PITCH_X - 16.5, -20.15, PITCH_X - 16.5, 20.15),
                             (-PITCH_X, -20.15, -PITCH_X + 16.5, -20.15), (-PITCH_X, 20.15, -PITCH_X + 16.5, 20.15),
                             (PITCH_X, -20.15, PITCH_X - 16.5, -20.15), (PITCH_X, 20.15, PITCH_X - 16.5, 20.15),
                             (0, -PITCH_Y, 0, PITCH_Y)):
        cube('line', ((x0 + x1) / 2, (y0 + y1) / 2, 0.03), (abs(x1 - x0) + 0.12, abs(y1 - y0) + 0.12, 0.01), m_line)
    if track:
        bm = bmesh.new()
        segs = 64
        for i in range(segs):
            a0, a1 = 2 * math.pi * i / segs, 2 * math.pi * (i + 1) / segs
            pts = []
            for (r, a) in ((1.0, a0), (1.0, a1), (1.18, a1), (1.18, a0)):
                pts.append(bm.verts.new((math.cos(a) * (PITCH_X + 8) * r, math.sin(a) * (PITCH_Y + 18) * r, 0.015)))
            bm.faces.new(pts)
        obj('track', bm, track)


def stand_points(shape, i, n, ring_x, ring_y):
    """the stand's front edge as points, the far half only (0..pi) and the ends"""
    a = math.pi * (-0.18 + 1.36 * i / (n - 1))
    if shape == 'round':
        return math.cos(a) * ring_x, math.sin(a) * ring_x
    if shape == 'oval':
        return math.cos(a) * ring_x, math.sin(a) * ring_y
    # a rectangle with rounded corners: the far side and the two ends
    t = i / (n - 1)
    L = 2 * ring_y * 0.6 + 2 * ring_x
    d = t * L
    if d < ring_y * 0.6:
        return ring_x, -ring_y * 0.0 + d * 1.0 - ring_y * 0.6 + ring_y * 0.6 * 1.0 if False else ring_x, ring_y * 0.0 + d
    d -= ring_y * 0.6
    if d < 2 * ring_x:
        return ring_x - d, ring_y
    d -= 2 * ring_x
    return -ring_x, ring_y - d


def stand(name, shape, ring_x, ring_y, rows, rise, depth, seat_cols, crowd, frame_jump, rng, tiers=1, gap=3.0,
          people=0.65, team_cols=None):
    """steps going back and up from a front edge; seats in stripes of the
    given colours; people on them (crowd: list to fill with their boxes)"""
    bm = bmesh.new()
    colattr = bm.faces.layers.color.new('Col') if hasattr(bm.faces.layers, 'color') else None
    n = 160
    for tier in range(tiers):
        z0 = tier * (rows * rise + gap)
        off = tier * (rows * depth * 0.55)
        for r in range(rows):
            zz = z0 + 1.2 + r * rise
            dd = 4.0 + off + r * depth
            for i in range(n - 1):
                if shape == 'rect':
                    (x0, y0), (x1, y1) = rect_edge(i, n, ring_x, ring_y), rect_edge(i + 1, n, ring_x, ring_y)
                    nx0, ny0 = rect_out(i, n, ring_x, ring_y)
                    nx1, ny1 = rect_out(i + 1, n, ring_x, ring_y)
                else:
                    x0, y0 = stand_points(shape, i, n, ring_x, ring_y)
                    x1, y1 = stand_points(shape, i + 1, n, ring_x, ring_y)
                    l0 = math.hypot(x0, y0) or 1
                    l1 = math.hypot(x1, y1) or 1
                    nx0, ny0, nx1, ny1 = x0 / l0, y0 / l0, x1 / l1, y1 / l1
                p = [Vector((x0 + nx0 * dd, y0 + ny0 * dd, zz)), Vector((x1 + nx1 * dd, y1 + ny1 * dd, zz)),
                     Vector((x1 + nx1 * (dd + depth), y1 + ny1 * (dd + depth), zz)),
                     Vector((x0 + nx0 * (dd + depth), y0 + ny0 * (dd + depth), zz))]
                q = [Vector((v.x, v.y, zz - rise)) for v in p[:2]]
                vs = [bm.verts.new(v) for v in p]
                f = bm.faces.new(vs)
                riser = bm.faces.new([bm.verts.new(q[0]), bm.verts.new(q[1]), vs[1], vs[0]])
                col = seat_cols[(i // 6 + r // 3 + tier) % len(seat_cols)]
                if colattr is not None:
                    c = (*C.hexc(col), 1.0)
                    f[colattr] = c
                    riser[colattr] = tuple(v * 0.55 for v in c[:3]) + (1.0,)
                # people
                if rng.random() < people:
                    t = rng.random()
                    cx = p[0].x + (p[1].x - p[0].x) * t + (p[3].x - p[0].x) * 0.4
                    cy = p[0].y + (p[1].y - p[0].y) * t + (p[3].y - p[0].y) * 0.4
                    pc = rng.choice(team_cols or ['E8E8E8', '1A1A1A', 'C8302C', '2050C0', 'F0C020', '808890'])
                    crowd.append((cx, cy, zz, pc, rng.random()))
    o = obj(name, bm, mat_vcol(name + '_m'))
    return o


def rect_edge(i, n, rx, ry):
    """the far side and the ends of a rectangle, as one line from the left end"""
    t = i / (n - 1)
    total = ry * 2 + rx * 2
    d = t * total
    if d < ry:
        return -rx, -ry * 0.0 + d
    d -= ry
    if d < 2 * rx:
        return -rx + d, ry
    d -= 2 * rx
    return rx, ry - d


def rect_out(i, n, rx, ry):
    t = i / (n - 1)
    total = ry * 2 + rx * 2
    d = t * total
    if d < ry:
        return -1.0, 0.0
    if d < ry + 2 * rx:
        return 0.0, 1.0
    return 1.0, 0.0


def crowd_mesh(people, frame, rng_seed):
    """the people as small boxes, a body and a head each, built at once from
    arrays; in frames > 0 some of them jump"""
    import numpy as np
    rng = np.random.default_rng(rng_seed + frame * 7919)
    n = len(people)
    P = np.array([(x, y, z) for (x, y, z, c, ph) in people], np.float32).reshape(-1, 3)
    ph = np.array([p[4] for p in people], np.float32)
    jump = np.zeros(n, np.float32)
    if frame > 0:
        on = (((ph * 10 + frame) % 2.0) < 1.0) & (rng.random(n) < 0.7)
        jump[on] = 0.35 + 0.25 * rng.random(int(on.sum()))
    cube = np.array([(x, y, z) for x in (-0.5, 0.5) for y in (-0.5, 0.5) for z in (-0.5, 0.5)], np.float32)
    quads = [(0, 1, 3, 2), (4, 6, 7, 5), (0, 4, 5, 1), (2, 3, 7, 6), (0, 2, 6, 4), (1, 5, 7, 3)]
    body = P[:, None, :] + cube[None] * np.array([0.42, 0.42, 1.15], np.float32)
    body[..., 2] += 0.575 + jump[:, None]
    head = P[:, None, :] + cube[None] * np.array([0.26, 0.26, 0.3], np.float32)
    head[..., 2] += 1.35 + jump[:, None]
    verts = np.concatenate([body, head], 0).reshape(-1, 3)
    base = np.arange(2 * n)[:, None, None] * 8 + np.array(quads)[None]
    faces = base.reshape(-1, 4)
    me = bpy.data.meshes.new('crowd%d' % frame)
    me.vertices.add(len(verts))
    me.vertices.foreach_set('co', verts.ravel())
    me.loops.add(len(faces) * 4)
    me.loops.foreach_set('vertex_index', faces.ravel().astype(np.int32))
    me.polygons.add(len(faces))
    me.polygons.foreach_set('loop_start', np.arange(0, len(faces) * 4, 4, dtype=np.int32))
    me.polygons.foreach_set('loop_total', np.full(len(faces), 4, np.int32))
    me.update()
    cols = np.array([C.hexc(c) + (1.0,) for (x, y, z, c, p) in people], np.float32)
    skin = np.array(C.hexc('B07A58') + (1.0,), np.float32)
    fc = np.concatenate([np.repeat(cols, 6, 0), np.tile(skin, (n * 6, 1))], 0)
    at = me.attributes.new('Col', 'FLOAT_COLOR', 'FACE')
    at.data.foreach_set('color', fc.ravel())
    o = bpy.data.objects.new('crowd%d' % frame, me)
    o.data.materials.append(mat_vcol('crowd_m%d' % frame))
    C.sc.collection.objects.link(o)
    return o


def floodlights(positions, height, col='FFFFFF', energy=3.0):
    pole = mat('pole', '6A7078', rough=0.4, metal=0.6)
    lamp = mat('lamp', 'FFFFFF', emit=(col, 25.0))
    for (x, y) in positions:
        cube('pole', (x, y, height / 2), (1.2, 1.2, height), pole)
        cube('lamp', (x, y - 0.8, height + 2.5), (8.0, 0.6, 5.0), lamp)
        area((x * 0.8, y * 0.8, height), 8, energy * 40000, col, rot=(50 if y > 0 else -50, 0, 0))


def roof_ring(shape, rx, ry, z, depth, m, m_edge=None, thick=1.2):
    bm = bmesh.new()
    n = 96
    for i in range(n):
        a0, a1 = 2 * math.pi * i / n, 2 * math.pi * (i + 1) / n
        if shape == 'round':
            e = lambda a, k: (math.cos(a) * (rx + k), math.sin(a) * (rx + k))  # noqa: E731
        else:
            e = lambda a, k: (math.cos(a) * (rx + k), math.sin(a) * (ry + k))  # noqa: E731
        p = [e(a0, 0), e(a1, 0), e(a1, depth), e(a0, depth)]
        vs = [bm.verts.new((px, py, z)) for (px, py) in p]
        bm.faces.new(vs)
    o = obj('roof', bm, m)
    s = o.modifiers.new('solid', 'SOLIDIFY')
    s.thickness = thick
    if m_edge:
        bm = bmesh.new()
        for i in range(n):
            a0, a1 = 2 * math.pi * i / n, 2 * math.pi * (i + 1) / n
            p0 = (math.cos(a0) * rx, math.sin(a0) * (rx if shape == 'round' else ry))
            p1 = (math.cos(a1) * rx, math.sin(a1) * (rx if shape == 'round' else ry))
            vs = [bm.verts.new((p0[0], p0[1], z - 0.2)), bm.verts.new((p1[0], p1[1], z - 0.2)),
                  bm.verts.new((p1[0], p1[1], z - 1.4)), bm.verts.new((p0[0], p0[1], z - 1.4))]
            bm.faces.new(vs)
        obj('roof_edge', bm, m_edge)
    return o


def houses(rng, x0, x1, y0, y1, n, cols, hmin=6, hmax=14, lit=None):
    for i in range(n):
        x = rng.uniform(x0, x1)
        y = rng.uniform(y0, y1)
        w = rng.uniform(5, 10)
        d = rng.uniform(5, 10)
        h = rng.uniform(hmin, hmax)
        cube('house', (x, y, h / 2), (w, d, h), mat('h%d' % i, rng.choice(cols), rough=0.8,
                                                      emit=(lit, 0.6) if lit and rng.random() < 0.4 else None))


def city(rng, y0, y1, n, night=True, hmax=60):
    win = mat('windows', '1A2030', emit=('FFD890', 0.6) if night else None)
    dark = mat('blocks', '252A36' if night else '9AA4B4', rough=0.7)
    for i in range(n):
        x = rng.uniform(-420, 420)
        y = rng.uniform(y0, y1)
        h = rng.uniform(15, hmax)
        cube('bld', (x, y, h / 2), (rng.uniform(12, 30), rng.uniform(12, 30), h), win if rng.random() < 0.5 else dark)


# ---------------------------------------------------------------------------
# The stadiums
# ---------------------------------------------------------------------------

def common_scene(samples):
    C.setup(samples)
    C.sc.render.film_transparent = False
    m_grass = mat_grass()
    m_line = mat('pline', 'E8E8E8', rough=0.8)
    return m_grass, m_line


def camera():
    cd = bpy.data.cameras.new('cam')
    cd.lens = float(C.arg('--lens', 22))
    cd.sensor_width = 36
    cd.clip_end = 3000
    cam = bpy.data.objects.new('cam', cd)
    C.sc.collection.objects.link(cam)
    C.sc.camera = cam
    cam.location = (0, -float(C.arg('--back', 115)), float(C.arg('--up', 32)))
    cam.rotation_euler = (math.radians(float(C.arg('--tilt', 83))), 0, 0)
    C.sc.render.resolution_x = 1280
    C.sc.render.resolution_y = 720
    return cam


def build(sid, rng):
    """the stadium sid; returns its settings for the crowd and the flashes"""
    crowd = []
    g = mat('ground', '3C4A3A', rough=0.95)
    st = {}
    if sid == 'laboca':
        # the straight stand of boxes on one side, three steep ones round the rest, blue and gold
        sky('050812', '1A2238', 1.0)
        ground(g)
        stand('st', 'rect', PITCH_X + 3, PITCH_Y + 3, 34, 0.75, 0.55, ['0B3D91', 'FCD116'], crowd, 0, rng, tiers=3,
              gap=2.0, team_cols=['0B3D91', 'FCD116', 'FCD116', '0B3D91', 'E8E8E8'])
        boxes = mat('boxes', '202838', emit=('FFE6A8', 0.9))
        cube('palcos', (0, PITCH_Y + 26, 34), (110, 3, 22), boxes)
        for k in range(9):
            cube('balc', (0, PITCH_Y + 24.4, 26 + k * 2.4), (110, 0.6, 0.25), mat('bal%d' % k, '0B3D91'))
        houses(rng, -170, -80, 30, 140, 40, ['E5403A', 'F2C230', '2E8BD8', '3BB273', 'E87A2E'], lit='FFD28A')
        houses(rng, 80, 170, 30, 140, 40, ['E5403A', 'F2C230', '2E8BD8', '3BB273', 'E87A2E'], lit='FFD28A')
        floodlights([(-70, 60), (70, 60), (-75, -30), (75, -30)], 55)
        st['night'] = True
    elif sid == 'nunez':
        # the wide oval with its running track, open to the sky, white and red
        sky('0A1330', '3A4F7A', 1.0)
        ground(g)
        trk = mat('track', 'B04A2A', rough=0.9)
        stand('st', 'oval', PITCH_X + 18, PITCH_Y + 30, 34, 0.85, 0.7, ['F4F4F4', 'F4F4F4', 'D21F1F'], crowd, 0, rng,
              tiers=3, gap=3.0, team_cols=['F4F4F4', 'D21F1F', 'F4F4F4', '1A1A1A'])
        st['track'] = trk
        floodlights([(-90, 70), (90, 70), (-95, -40), (95, -40)], 50, col='F0F4FF')
        st['night'] = True
    elif sid == 'rio':
        # the great ring under its roof, the hill with the statue behind, an evening sky
        sky('3A6EB0', 'F2B27A', 1.2)
        sun((80, 0, 200), 3.0, 'FFD0A0')
        ground(g)
        stand('st', 'oval', PITCH_X + 14, PITCH_Y + 22, 34, 0.8, 0.75, ['2A8A3A', 'F2D21A', '1E54B0', 'F4F4F4'], crowd, 0,
              rng, tiers=3)
        roof_ring('oval', PITCH_X + 40, PITCH_Y + 46, 30, 26, mat('ringroof', 'EDEDED', rough=0.4))
        hill = mat('hill', '2F5A2C', rough=0.9)
        bm = bmesh.new()
        bmesh.ops.create_uvsphere(bm, u_segments=32, v_segments=16, radius=1.0)
        o = obj('morro', bm, hill, smooth=True)
        o.scale = (170, 80, 255)
        o.location = (230, 700, -30)
        stone = mat('statue', 'F2F0EA', emit=('FFF4E0', 2.0))
        cube('body', (230, 700, 241), (9, 9, 40), stone)
        cube('arms', (230, 700, 255), (42, 6, 6), stone)
    elif sid == 'madrid':
        # towers at the corners, the city lit at night
        sky('060914', '1C2A48', 1.0)
        ground(g)
        stand('st', 'rect', PITCH_X + 4, PITCH_Y + 4, 30, 0.85, 0.6, ['F4F4F4', 'D8D8D8', '7A2BA0'], crowd, 0, rng, tiers=3,
              gap=2.5)
        tw = mat('tower', 'C8CCD4', rough=0.5)
        ramp = mat('ramp', '8A90A0', rough=0.6)
        for (x, y) in ((-PITCH_X - 40, PITCH_Y + 50), (PITCH_X + 40, PITCH_Y + 50)):
            bm = bmesh.new()
            bmesh.ops.create_cone(bm, cap_ends=True, segments=32, radius1=11, radius2=11, depth=120)
            o = obj('tw', bm, tw, smooth=True)
            o.location = (x, y, 60)
            for k in range(20):
                c = cube('rmp', (x, y, 4 + k * 5.8), (23, 23, 0.9), ramp)
                c.rotation_euler = (0, 0, k * 0.4)
        roof_ring('round', 80, 80, 62, 30, mat('roofm', 'BFC4CC', rough=0.4), mat('roofl', 'FFFFFF', emit=('FFFFFF', 8)))
        city(rng, 320, 700, 160, night=True)
        st['night'] = True
    elif sid == 'munich':
        # a shell of cushions that lights up in colour
        sky('0A0A18', '22203A', 1.0)
        ground(g)
        stand('st', 'oval', PITCH_X + 8, PITCH_Y + 12, 30, 0.85, 0.65, ['C8302C', 'B0B4BC'], crowd, 0, rng, tiers=3,
              gap=2.0)
        shell = bpy.data.materials.new('shell')
        shell.use_nodes = True
        nt = shell.node_tree
        b = nt.nodes['Principled BSDF']
        vor = nt.nodes.new('ShaderNodeTexVoronoi')
        vor.feature = 'DISTANCE_TO_EDGE'
        vor.inputs['Scale'].default_value = 0.18
        ramp = nt.nodes.new('ShaderNodeValToRGB')
        ramp.color_ramp.elements[0].color = (0.05, 0.0, 0.0, 1)
        ramp.color_ramp.elements[1].position = 0.12
        ramp.color_ramp.elements[1].color = (*C.hexc('FF2A1C'), 1)
        nt.links.new(vor.outputs['Distance'], ramp.inputs[0])
        nt.links.new(ramp.outputs[0], b.inputs['Emission'])
        b.inputs['Emission Strength'].default_value = 3.5
        b.inputs['Base Color'].default_value = (0.9, 0.9, 0.9, 1)
        bm = bmesh.new()
        bmesh.ops.create_uvsphere(bm, u_segments=64, v_segments=32, radius=1.0)
        bmesh.ops.delete(bm, geom=[v for v in bm.verts if v.co.z > 0.55 or v.co.z < -0.2 or v.co.y < 0.05], context='VERTS')
        o = obj('shell', bm, shell, smooth=True)
        o.scale = (150, 110, 105)
        o.location = (0, 20, 0)
        st['night'] = True
    elif sid == 'londres':
        # the white arch over the stadium
        sky('2A4A80', 'B8C8DC', 1.1)
        sun((60, 0, 160), 2.5)
        ground(g)
        stand('st', 'oval', PITCH_X + 8, PITCH_Y + 12, 32, 0.8, 0.65, ['C8102E', 'C8102E', 'B0B4BC'], crowd, 0, rng,
              tiers=3, gap=2.5)
        roof_ring('oval', PITCH_X + 40, PITCH_Y + 40, 58, 22, mat('roofw', 'D8DCE2', rough=0.4))
        arch = mat('arch', 'F6F6F6', rough=0.3)
        pts = []
        for i in range(41):
            a = math.pi * i / 40
            pts.append((math.cos(a) * 165, 90 + math.sin(a) * 10, math.sin(a) * 105))
        for i in range(40):
            p0, p1 = Vector(pts[i]), Vector(pts[i + 1])
            d = p1 - p0
            bm = bmesh.new()
            bmesh.ops.create_cone(bm, cap_ends=True, segments=12, radius1=3.5, radius2=3.5, depth=d.length)
            o = obj('arch', bm, arch, smooth=True)
            o.location = (p0 + p1) / 2
            o.rotation_mode = 'QUATERNION'
            o.rotation_quaternion = Vector((0, 0, 1)).rotation_difference(d.normalized())
        city(rng, 200, 600, 120, night=False, hmax=40)
    elif sid == 'mexico':
        # the huge steep stand, the mountains behind, a clear high sky
        sky('2C6AC8', 'C8DCEC', 1.2)
        sun((55, 0, 140), 3.2)
        ground(g)
        stand('st', 'rect', PITCH_X + 4, PITCH_Y + 4, 44, 0.8, 0.55, ['E8E8E8', '1A7A4A', 'C8302C'], crowd, 0, rng,
              tiers=3, gap=3.5)
        roof_ring('round', 115, 115, 88, 18, mat('roofmx', 'A8ACB4', rough=0.5))
        mnt = mat('mount', '5A6A7A', rough=0.95)
        for k in range(7):
            bm = bmesh.new()
            bmesh.ops.create_cone(bm, cap_ends=True, segments=6, radius1=1.0, radius2=0.0, depth=1.0)
            o = obj('mnt', bm, mnt)
            o.scale = (rng.uniform(200, 380), rng.uniform(120, 200), rng.uniform(140, 260))
            o.location = (-700 + k * 240 + rng.uniform(-60, 60), 800, 0)
    elif sid == 'tokio':
        # the roof of wooden slats, cherry trees, the tower far off at dusk
        sky('3A2E60', 'F0A890', 1.1)
        sun((80, 0, 210), 2.0, 'FFC0A0')
        ground(g)
        stand('st', 'oval', PITCH_X + 10, PITCH_Y + 14, 30, 0.75, 0.65, ['E8E4DC', 'C8C0B0', '2A5A8A'], crowd, 0, rng,
              tiers=3, gap=2.5)
        wood = mat('slat', '9A6A3A', rough=0.6)
        for i in range(120):
            a = math.pi * (-0.15 + 1.3 * i / 119)
            x, y = math.cos(a) * (PITCH_X + 55), math.sin(a) * (PITCH_Y + 55)
            o = cube('slat', (x, y, 48), (1.0, 28, 0.8), wood)
            o.rotation_euler = (0, 0, a - math.pi / 2)
        roof_ring('oval', PITCH_X + 42, PITCH_Y + 42, 50, 26, mat('roofjp', 'D8D0C0', rough=0.6))
        blossom = mat('blossom', 'F7B8CC', rough=0.8)
        trunk = mat('trunk', '4A3020')
        for k in range(26):
            x = rng.uniform(-260, 260)
            y = rng.uniform(140, 260) if abs(x) < 160 else rng.uniform(-60, 220)
            cube('trunk', (x, y, 4), (1.2, 1.2, 8), trunk)
            bm = bmesh.new()
            bmesh.ops.create_icosphere(bm, subdivisions=2, radius=rng.uniform(6, 9))
            o = obj('bl', bm, blossom, smooth=True)
            o.location = (x, y, 12)
        red = mat('towerred', 'E8502A', rough=0.5, emit=('FF6030', 0.3))
        white = mat('towerw', 'F4F4F4', rough=0.5)
        for k in range(10):
            r0 = 22 - k * 2.0
            bm = bmesh.new()
            bmesh.ops.create_cone(bm, cap_ends=True, segments=4, radius1=r0, radius2=r0 - 2.0, depth=36)
            o = obj('tower', bm, red if k % 2 == 0 else white)
            o.location = (-200, 600, 18 + k * 34)
        city(rng, 500, 1000, 140, night=False, hmax=50)
    elif sid in ('avella_rojo', 'avella_celeste'):
        rojo = sid == 'avella_rojo'
        sky('050812', '1E2840', 1.0)
        ground(g)
        if rojo:
            # modern, red, under a roof; the sky-blue round one a block away on the left
            stand('st', 'rect', PITCH_X + 4, PITCH_Y + 4, 30, 0.8, 0.6, ['D7141A', 'D7141A', 'F4F4F4'], crowd, 0, rng,
                  tiers=3, gap=2.5, team_cols=['D7141A', 'D7141A', 'F4F4F4', '1A2A5A'])
            roof_ring('round', 82, 82, 52, 26, mat('roofr', 'E8E8EC', rough=0.4), mat('rl', 'FFFFFF', emit=('FFFFFF', 8)))
            other = (-230, 260)
            oc = ['7FBCE8', 'F4F4F4']
        else:
            # round, sky-blue and white; the red one a block away on the right
            stand('st', 'round', 70, 70, 34, 0.75, 0.6, ['7FBCE8', 'F4F4F4'], crowd, 0, rng, tiers=3, gap=2.0,
                  team_cols=['7FBCE8', 'F4F4F4', '7FBCE8', '1A1A1A'])
            other = (230, 260)
            oc = ['D7141A', 'F4F4F4']
            floodlights([(-80, 55), (80, 55), (-85, -35), (85, -35)], 52)
        # the neighbour: its stands' back, its roof and lights over the roofs
        nb = mat('nb', oc[0], rough=0.7)
        bm = bmesh.new()
        bmesh.ops.create_cone(bm, cap_ends=True, segments=48, radius1=75, radius2=82, depth=38)
        o = obj('neigh', bm, nb, smooth=True)
        o.location = (other[0], other[1], 19)
        cube('neigh_roof', (other[0], other[1], 40), (150, 150, 2), mat('nbr', oc[1], emit=('FFFFFF', 0.4)))
        floodlights([(other[0] - 60, other[1] + 50), (other[0] + 60, other[1] + 50)], 60, energy=1.0)
        houses(rng, -200, 200, 120, 200, 70, ['8A7A6A', 'A89A88', '6A6058', 'B0A090'], lit='FFD28A')
        st['night'] = True
    return crowd, st


def render_stadium(sid, samples):
    rng = random.Random(hash(sid) & 0xFFFF)
    m_grass, m_line = common_scene(samples)
    crowd, st = build(sid, rng)
    pitch(m_grass, m_line, st.get('track'))
    if st.get('night'):
        area((0, 0, 60), 120, 60000, 'F4F8FF')
    else:
        area((0, 0, 80), 120, 15000, 'FFFFFF')
    camera()
    out = os.path.join(C.ASSETS, 'stadiums', sid)
    os.makedirs(out, exist_ok=True)
    # the backdrop with the crowd still
    o = crowd_mesh(crowd, 0, 1)
    C.render(os.path.join(out, 'backdrop.png'))
    # the crowd's frames: only the band of the stands
    C.sc.render.use_border = True
    C.sc.render.use_crop_to_border = True
    C.sc.render.border_min_x, C.sc.render.border_max_x = 0.0, 1.0
    C.sc.render.border_min_y, C.sc.render.border_max_y = 1.0 - CROWD_H / 720.0, 1.0
    C.sc.render.film_transparent = False
    for k in range(FRAMES):
        bpy.data.objects.remove(o)
        o = crowd_mesh(crowd, k, 1)
        C.render(os.path.join(out, 'crowd%d.png' % k))
    # flashes: a few spots of the stands, for the goals
    flashes = [(int(rng.uniform(60, 1220)), int(rng.uniform(40, CROWD_H - 30))) for _ in range(10)]
    json.dump(dict(x=0, y=0, w=1280, h=CROWD_H, frames=FRAMES, flashes=flashes), open(os.path.join(out, 'crowd.json'), 'w'))


ALL = ['laboca', 'nunez', 'rio', 'madrid', 'munich', 'londres', 'mexico', 'tokio', 'avella_rojo', 'avella_celeste']


def main():
    only = C.arg('--only')
    samples = int(C.arg('--samples', 64))
    for sid in (only.split(',') if only else ALL):
        render_stadium(sid, samples)


main()
