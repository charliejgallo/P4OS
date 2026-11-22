"""
Juegos de mesa - what every Blender script of the app shares: the camera,
the lights, the art sets' materials, pieces turned on a lathe, and the
renders the packer (../pack_p4.py) reads.

    B=/Applications/Blender.app/Contents/MacOS/Blender
    $B -b -P ajedrez.py -- --set madera [--samples 64] [--only board|pieces|thumb]

Blender 3.3.1, Cycles on Metal. Everything is built by code; nothing reads
a .blend. One Blender unit is one cell of the board.

The camera is orthographic and fixed, so a point of the board (u, v) at a
height h lands on the picture at

    x = cx + S (u - u0)        y = cy - S (sin(E) (v - v0) + cos(E) h)

which is what the app (jm_view.c) computes: S pixels a unit, E the
camera's height over the table (90 from above, 0 from the front), (u0, v0)
the board's middle at the picture's (cx, cy). A piece is rendered alone
with its foot at a known pixel (its anchor), so one sprite serves every
cell.

Out: ../../assets/<set>/<name>.png and <name>.json (the anchor, S, E...).
"""
import json
import math
import os
import sys

import bpy
import bmesh
from mathutils import Euler, Vector

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.normpath(os.path.join(HERE, '..', '..', 'assets'))
PIC = 1280                  # a board's picture: square, cropped by the screen
E3D = 55.0                  # the 3D camera's height, degrees

sc = None
cam = None
MATS = {}


def argv():
    return sys.argv[sys.argv.index('--') + 1:] if '--' in sys.argv else []


def arg(name, default=None):
    a = argv()
    for i, x in enumerate(a):
        if x == name and i + 1 < len(a):
            return a[i + 1]
        if x.startswith(name + '='):
            return x.split('=', 1)[1]
    return default


def only(part):
    o = arg('--only')
    return o is None or part in o.split(',')


def out_dir():
    d = os.path.join(ASSETS, arg('--set', 'madera'))
    os.makedirs(d, exist_ok=True)
    return d


# ---------------------------------------------------------------------------
# The scene
# ---------------------------------------------------------------------------

def setup(samples=None):
    global sc, cam
    bpy.ops.wm.read_factory_settings(use_empty=True)
    MATS.clear()
    sc = bpy.context.scene
    sc.render.engine = 'CYCLES'
    ok = False
    try:
        pr = bpy.context.preferences.addons['cycles'].preferences
        pr.compute_device_type = 'METAL'
        pr.get_devices()
        for d in pr.devices:
            d.use = (d.type == 'METAL')
            ok = ok or d.use
    except Exception as e:  # noqa
        print('GPU setup failed:', e)
    sc.cycles.device = 'GPU' if ok else 'CPU'
    r = sc.render
    r.resolution_percentage = 100
    r.dither_intensity = 0.0
    r.image_settings.file_format = 'PNG'
    r.image_settings.color_mode = 'RGBA'
    r.image_settings.color_depth = '8'
    sc.view_settings.view_transform = 'Filmic'
    sc.view_settings.look = 'Medium Contrast'
    sc.cycles.samples = int(samples or arg('--samples', 64))
    sc.cycles.use_adaptive_sampling = True
    sc.cycles.seed = 3
    sc.cycles.max_bounces = 8
    sc.cycles.transmission_bounces = 8
    sc.cycles.transparent_max_bounces = 8
    sc.cycles.caustics_reflective = False
    sc.cycles.caustics_refractive = False
    sc.cycles.sample_clamp_indirect = 4.0
    sc.cycles.use_denoising = True
    sc.cycles.denoiser = 'OPENIMAGEDENOISE'
    cd = bpy.data.cameras.new('cam')
    cam = bpy.data.objects.new('cam', cd)
    sc.collection.objects.link(cam)
    sc.camera = cam
    cd.type = 'ORTHO'
    cd.sensor_fit = 'HORIZONTAL'
    cd.clip_start = 0.1
    cd.clip_end = 200.0
    world = bpy.data.worlds.new('world')
    world.use_nodes = True
    sc.world = world
    ambient((0.85, 0.80, 0.72), 0.35)
    return sc


def ambient(color, strength):
    bg = sc.world.node_tree.nodes['Background']
    bg.inputs['Color'].default_value = (*color, 1)
    bg.inputs['Strength'].default_value = strength


def aim(w, h, S, E, target, ax=None, ay=None):
    """the camera: a w x h picture, S px a unit, height E degrees, with the
    world point 'target' at pixel (ax, ay) (the middle by default)"""
    sc.render.resolution_x, sc.render.resolution_y = int(w), int(h)
    cam.data.ortho_scale = w / S
    e = math.radians(E)
    cam.rotation_euler = Euler((math.pi / 2 - e, 0, 0), 'XYZ')
    fwd = Vector((0, math.cos(e), -math.sin(e)))
    up = Vector((0, math.sin(e), math.cos(e)))
    right = Vector((1, 0, 0))
    if ax is None:
        ax = w / 2
    if ay is None:
        ay = h / 2
    centre = Vector(target) - right * ((ax - w / 2) / S) - up * ((h / 2 - ay) / S)
    cam.location = centre - fwd * 40.0


def sun(az, el, energy, color=(1.0, 0.96, 0.9), angle=4.0, name='sun'):
    """a sun FROM the direction az (degrees, 0 = +X, counter-clockwise seen
    from above; 270 is from the camera's side) at elevation el"""
    ld = bpy.data.lights.new(name, 'SUN')
    ld.energy = energy
    ld.color = color
    ld.angle = math.radians(angle)
    o = bpy.data.objects.new(name, ld)
    sc.collection.objects.link(o)
    a, e = math.radians(az), math.radians(el)
    d = Vector((-math.cos(a) * math.cos(e), -math.sin(a) * math.cos(e), -math.sin(e)))
    o.rotation_euler = d.to_track_quat('-Z', 'Y').to_euler()
    return o


def area(loc, size, energy, color=(1, 0.97, 0.92), name='area'):
    ld = bpy.data.lights.new(name, 'AREA')
    ld.energy = energy
    ld.size = size
    ld.color = color
    o = bpy.data.objects.new(name, ld)
    sc.collection.objects.link(o)
    o.location = loc
    o.rotation_euler = (Vector((0, 0, 0)) - Vector(loc)).to_track_quat('-Z', 'Y').to_euler()
    return o


def studio(scale=8.0):
    """a warm lamp over the table from the front left, a cooler one from the
    back right, the room"""
    sun(235, 58, 2.6, (1.0, 0.93, 0.82), angle=8, name='key')
    sun(60, 35, 0.8, (0.75, 0.85, 1.0), angle=25, name='rim')
    area((-scale, -scale * 1.2, scale * 1.6), scale * 1.2, 300.0 * scale * scale / 64.0, name='soft')
    ambient((0.80, 0.76, 0.70), 0.30)


def transparent(on=True):
    sc.render.film_transparent = on


def render(path):
    sc.render.filepath = path
    bpy.ops.render.render(write_still=True)


def clear_objects(keep=('cam',)):
    for o in list(bpy.data.objects):
        if o.name in keep or o.type == 'LIGHT':
            continue
        bpy.data.objects.remove(o, do_unlink=True)


# ---------------------------------------------------------------------------
# Materials
# ---------------------------------------------------------------------------

def rgb(c):
    if isinstance(c, str):
        c = c.lstrip('#')
        c = tuple(int(c[i:i + 2], 16) / 255.0 for i in (0, 2, 4))
    return tuple(c[:3])


def lin(c):
    def f(v):
        return v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4
    c = rgb(c)
    return (f(c[0]), f(c[1]), f(c[2]), 1.0)


def _principled(name):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    return m, m.node_tree, m.node_tree.nodes['Principled BSDF']


def mat(name, color, rough=0.4, metal=0.0, clear=0.0, trans=0.0, ior=1.45, spec=0.5):
    if name in MATS:
        return MATS[name]
    m, nt, b = _principled(name)
    b.inputs['Base Color'].default_value = lin(color)
    b.inputs['Roughness'].default_value = rough
    b.inputs['Metallic'].default_value = metal
    b.inputs['Clearcoat'].default_value = clear
    b.inputs['Specular'].default_value = spec
    b.inputs['IOR'].default_value = ior
    if trans:
        b.inputs['Transmission'].default_value = trans
    MATS[name] = m
    return m


def wood(name, c0, c1, scale=2.0, rough=0.35, clear=0.4, ring=8.0, along='X', seed=0.0):
    """wood: rings by a wave texture bent by noise, two colours, a varnish"""
    if name in MATS:
        return MATS[name]
    m, nt, b = _principled(name)
    tc = nt.nodes.new('ShaderNodeTexCoord')
    mp = nt.nodes.new('ShaderNodeMapping')
    mp.inputs['Location'].default_value = (seed * 3.7, seed * 1.3, seed * 2.1)
    mp.inputs['Scale'].default_value = (1.0, 6.0, 1.0) if along == 'X' else (6.0, 1.0, 1.0)
    nt.links.new(tc.outputs['Object'], mp.inputs['Vector'])
    wv = nt.nodes.new('ShaderNodeTexWave')
    wv.wave_type = 'RINGS'
    wv.rings_direction = 'X' if along == 'X' else 'Y'
    wv.inputs['Scale'].default_value = scale
    wv.inputs['Distortion'].default_value = ring
    wv.inputs['Detail'].default_value = 3.0
    wv.inputs['Detail Scale'].default_value = 1.5
    nt.links.new(mp.outputs['Vector'], wv.inputs['Vector'])
    nz = nt.nodes.new('ShaderNodeTexNoise')
    nz.inputs['Scale'].default_value = 30.0
    nz.inputs['Detail'].default_value = 4.0
    nt.links.new(mp.outputs['Vector'], nz.inputs['Vector'])
    mix = nt.nodes.new('ShaderNodeMixRGB')
    mix.blend_type = 'MULTIPLY'
    mix.inputs['Fac'].default_value = 0.15
    ramp = nt.nodes.new('ShaderNodeValToRGB')
    ramp.color_ramp.elements[0].position = 0.25
    ramp.color_ramp.elements[1].position = 0.85
    ramp.color_ramp.elements[0].color = lin(c0)
    ramp.color_ramp.elements[1].color = lin(c1)
    nt.links.new(wv.outputs['Fac'], ramp.inputs['Fac'])
    nt.links.new(ramp.outputs['Color'], mix.inputs['Color1'])
    nt.links.new(nz.outputs['Color'], mix.inputs['Color2'])
    nt.links.new(mix.outputs['Color'], b.inputs['Base Color'])
    b.inputs['Roughness'].default_value = rough
    b.inputs['Clearcoat'].default_value = clear
    b.inputs['Clearcoat Roughness'].default_value = 0.15
    MATS[name] = m
    return m


def marble(name, c0, c1, scale=1.5, rough=0.12):
    """marble: an even stone (c0, a little mottled) crossed by thin veins of c1"""
    if name in MATS:
        return MATS[name]
    m, nt, b = _principled(name)
    tc = nt.nodes.new('ShaderNodeTexCoord')
    warp = nt.nodes.new('ShaderNodeTexNoise')
    warp.inputs['Scale'].default_value = scale * 0.8
    warp.inputs['Detail'].default_value = 3.0
    nt.links.new(tc.outputs['Object'], warp.inputs['Vector'])
    mix = nt.nodes.new('ShaderNodeMixRGB')
    mix.blend_type = 'LINEAR_LIGHT'
    mix.inputs['Fac'].default_value = 0.25
    nt.links.new(tc.outputs['Object'], mix.inputs['Color1'])
    nt.links.new(warp.outputs['Color'], mix.inputs['Color2'])
    vein = nt.nodes.new('ShaderNodeTexNoise')
    vein.inputs['Scale'].default_value = scale
    vein.inputs['Detail'].default_value = 10.0
    vein.inputs['Distortion'].default_value = 0.6
    nt.links.new(mix.outputs['Color'], vein.inputs['Vector'])
    ramp = nt.nodes.new('ShaderNodeValToRGB')
    cr = ramp.color_ramp
    cr.elements[0].position = 0.488
    cr.elements[0].color = lin(c0)
    cr.elements[1].position = 0.512
    cr.elements[1].color = lin(c0)
    e = cr.elements.new(0.500)
    e.color = lin(c1)
    nt.links.new(vein.outputs['Fac'], ramp.inputs['Fac'])
    # a soft mottle over the stone
    mot = nt.nodes.new('ShaderNodeTexNoise')
    mot.inputs['Scale'].default_value = scale * 4
    nt.links.new(tc.outputs['Object'], mot.inputs['Vector'])
    mm = nt.nodes.new('ShaderNodeMixRGB')
    mm.blend_type = 'MULTIPLY'
    mm.inputs['Fac'].default_value = 0.12
    nt.links.new(ramp.outputs['Color'], mm.inputs['Color1'])
    nt.links.new(mot.outputs['Color'], mm.inputs['Color2'])
    nt.links.new(mm.outputs['Color'], b.inputs['Base Color'])
    b.inputs['Roughness'].default_value = rough
    b.inputs['Specular'].default_value = 0.6
    b.inputs['Clearcoat'].default_value = 0.3
    MATS[name] = m
    return m


def glass(name, color, rough=0.04):
    if name in MATS:
        return MATS[name]
    m, nt, b = _principled(name)
    b.inputs['Base Color'].default_value = lin(color)
    b.inputs['Roughness'].default_value = rough
    b.inputs['Transmission'].default_value = 1.0
    b.inputs['IOR'].default_value = 1.5
    MATS[name] = m
    return m


def metal(name, color, rough=0.25):
    return mat(name, color, rough=rough, metal=1.0)


# the sets: the light and dark of the pieces and the board, in each
SETS = ('madera', 'marmol', 'vidrio', 'metal')


def set_name():
    s = arg('--set', 'madera')
    return s if s in SETS else 'madera'


def piece_mats(prefix='p'):
    """the two sides' materials in the set chosen: (light, dark)"""
    s = set_name()
    if s == 'marmol':
        return marble(prefix + '_l', '#f2eee6', '#9a948a', 2.5), marble(prefix + '_d', '#2a2622', '#8a7a68', 2.5)
    if s == 'vidrio':
        return glass(prefix + '_l', '#eaf4ff'), glass(prefix + '_d', '#3a6a9a')
    if s == 'metal':
        return metal(prefix + '_l', '#d8d8d2', 0.22), metal(prefix + '_d', '#b88a3a', 0.28)
    return (wood(prefix + '_l', '#e8cf9a', '#c9a368', 3.0, rough=0.3, clear=0.6, ring=6.0),
            wood(prefix + '_d', '#4a2a16', '#2a160a', 3.0, rough=0.3, clear=0.6, ring=6.0))


def board_mats(prefix='b'):
    """the board's light and dark squares and its frame"""
    s = set_name()
    if s == 'marmol':
        return (marble(prefix + '_l', '#efe9dd', '#b8ae9c', 1.2), marble(prefix + '_d', '#5a5048', '#2a2420', 1.2),
                marble(prefix + '_f', '#3a3630', '#7a7064', 0.8))
    if s == 'vidrio':
        return (mat(prefix + '_l', '#dfe8ee', rough=0.08, trans=0.3), mat(prefix + '_d', '#28465e', rough=0.08, trans=0.3),
                mat(prefix + '_f', '#1a2630', rough=0.2, metal=0.6))
    if s == 'metal':
        return (metal(prefix + '_l', '#c8c8c0', 0.35), metal(prefix + '_d', '#4a4a48', 0.35),
                metal(prefix + '_f', '#2a2a28', 0.4))
    return (wood(prefix + '_l', '#e2c290', '#c8a06a', 2.2, ring=5.0),
            wood(prefix + '_d', '#6a3e1e', '#3e200e', 2.2, ring=5.0, seed=1.7),
            wood(prefix + '_f', '#5a3216', '#2e1606', 1.4, ring=7.0, seed=3.1))


def table():
    """the table under the board: a big plane of dark wood (or cloth)"""
    s = set_name()
    if s == 'marmol':
        m = marble('table', '#3a3632', '#6a625a', 0.35, rough=0.3)
    elif s == 'vidrio':
        m = mat('table', '#10161c', rough=0.5)
    elif s == 'metal':
        m = mat('table', '#1c1c1e', rough=0.6, metal=0.3)
    else:
        m = wood('table', '#3a2414', '#24140a', 0.6, rough=0.45, clear=0.2, ring=4.0, seed=5.0)
    return m


# ---------------------------------------------------------------------------
# Shapes
# ---------------------------------------------------------------------------

def link(o, loc=(0, 0, 0), rot=(0, 0, 0), parent=None):
    sc.collection.objects.link(o)
    o.location = loc
    o.rotation_euler = Euler([math.radians(a) for a in rot], 'XYZ')
    if parent is not None:
        o.parent = parent
    return o


def mesh_obj(name, bm, m, smooth=True):
    me = bpy.data.meshes.new(name)
    bm.normal_update()
    bm.to_mesh(me)
    bm.free()
    if smooth:
        for p in me.polygons:
            p.use_smooth = True
        me.use_auto_smooth = True
        me.auto_smooth_angle = math.radians(40)
    if m is not None:
        me.materials.append(m)
    return bpy.data.objects.new(name, me)


def lathe(name, prof, m, segs=64, loc=(0, 0, 0), parent=None):
    """a solid of turning: prof is [(radius, z)] from the bottom up"""
    bm = bmesh.new()
    rings = []
    for (r, z) in prof:
        ring = []
        for i in range(segs):
            a = 2 * math.pi * i / segs
            ring.append(bm.verts.new((r * math.cos(a), r * math.sin(a), z)))
        rings.append(ring)
    for k in range(len(rings) - 1):
        for i in range(segs):
            j = (i + 1) % segs
            bm.faces.new((rings[k][i], rings[k][j], rings[k + 1][j], rings[k + 1][i]))
    bot = bm.verts.new((0, 0, prof[0][1]))
    top = bm.verts.new((0, 0, prof[-1][1]))
    for i in range(segs):
        j = (i + 1) % segs
        bm.faces.new((rings[0][j], rings[0][i], bot))
        bm.faces.new((rings[-1][i], rings[-1][j], top))
    o = mesh_obj(name, bm, m)
    return link(o, loc, parent=parent)


def smooth_profile(pts, steps=6):
    """a profile [(r, z)] rounded with Chaikin's corners"""
    for _ in range(steps // 3 + 1):
        out = [pts[0]]
        for a, b in zip(pts, pts[1:]):
            out.append((a[0] * 0.75 + b[0] * 0.25, a[1] * 0.75 + b[1] * 0.25))
            out.append((a[0] * 0.25 + b[0] * 0.75, a[1] * 0.25 + b[1] * 0.75))
        out.append(pts[-1])
        pts = out
    return pts


def box(name, size, m, loc=(0, 0, 0), rot=(0, 0, 0), bevel=0.02, parent=None):
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=Vector(size), verts=bm.verts)
    o = mesh_obj(name, bm, m, smooth=False)
    if bevel:
        md = o.modifiers.new('bevel', 'BEVEL')
        md.width = bevel
        md.segments = 3
        md.limit_method = 'ANGLE'
    return link(o, loc, rot, parent)


def cyl(name, r, h, m, loc=(0, 0, 0), segs=64, bevel=0.0, parent=None):
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, segments=segs, radius1=r, radius2=r, depth=h)
    o = mesh_obj(name, bm, m)
    if bevel:
        md = o.modifiers.new('bevel', 'BEVEL')
        md.width = bevel
        md.segments = 3
        md.limit_method = 'ANGLE'
    return link(o, (loc[0], loc[1], loc[2] + h / 2), parent=parent)


def extrude(name, poly, thick, m, loc=(0, 0, 0), rot=(0, 0, 0), bevel=0.02, parent=None):
    """a flat outline (in x, z) given a thickness along y"""
    bm = bmesh.new()
    vs = [bm.verts.new((x, -thick / 2, z)) for (x, z) in poly]
    f = bm.faces.new(vs)
    ext = bmesh.ops.extrude_face_region(bm, geom=[f])
    moved = [v for v in ext['geom'] if isinstance(v, bmesh.types.BMVert)]
    bmesh.ops.translate(bm, vec=(0, thick, 0), verts=moved)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces)
    o = mesh_obj(name, bm, m, smooth=False)
    if bevel:
        md = o.modifiers.new('bevel', 'BEVEL')
        md.width = bevel
        md.segments = 3
        md.limit_method = 'ANGLE'
    return link(o, loc, rot, parent)


def grid_board(prefix='aj', n=8, frame=0.5, light=None, dark=None):
    """an n x n board of tiles, its top at z = 0, on a table"""
    l0, d0, fr = board_mats(prefix)
    light = light or l0
    dark = dark or d0
    for r in range(n):
        for f in range(n):
            m = dark if (r + f) % 2 == 0 else light
            box('sq', (1.0, 1.0, 0.1), m, loc=(f + 0.5, r + 0.5, -0.05), bevel=0.004)
    w = n + 2 * frame
    t = 0.32
    c = n / 2
    box('fs', (w, frame, t), fr, loc=(c, -frame / 2, -t / 2 + 0.02), bevel=0.04)
    box('fn', (w, frame, t), fr, loc=(c, n + frame / 2, -t / 2 + 0.02), bevel=0.04)
    box('fw', (frame, n, t), fr, loc=(-frame / 2, c, -t / 2 + 0.02), bevel=0.04)
    box('fe', (frame, n, t), fr, loc=(n + frame / 2, c, -t / 2 + 0.02), bevel=0.04)
    box('under', (n, n, 0.2), fr, loc=(c, c, -0.2), bevel=0)
    box('table', (60, 60, 0.2), table(), loc=(c, c, -0.4), bevel=0)



# ---------------------------------------------------------------------------
# Renders
# ---------------------------------------------------------------------------

def write_json(path, d):
    with open(path, 'w') as f:
        json.dump(d, f, indent=1)


def render_board(name, S, E, centre, w=PIC, h=PIC, rot=0):
    """the table and the board, opaque; the board's middle at the picture's"""
    transparent(False)
    aim(w, h, S, E, centre)
    d = out_dir()
    render(os.path.join(d, name + '.png'))
    write_json(os.path.join(d, name + '.json'), dict(S=S, E=E, rot=rot, cx=w / 2, cy=h / 2, w=w, h=h))


def render_sprite(name, objs, S, E, w, h, ax, ay, frames=None):
    """one piece alone, transparent around, its foot (the world's origin) at
    pixel (ax, ay); frames: a list of functions that set up each frame"""
    transparent(True)
    aim(w, h, S, E, (0, 0, 0), ax, ay)
    d = out_dir()
    files = []
    for i, fn in enumerate(frames or [None]):
        if fn:
            fn()
        p = os.path.join(d, '%s.%d.png' % (name, i) if frames else name + '.png')
        render(p)
        files.append(os.path.basename(p))
    write_json(os.path.join(d, name + '.json'), dict(S=S, E=E, w=w, h=h, ax=ax, ay=ay, frames=files))


def hide(objs, hidden):
    for o in objs:
        o.hide_render = hidden
        for c in o.children_recursive:
            c.hide_render = hidden
