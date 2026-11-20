"""
BASTION - what every Blender script of the game shares (SPEC.md in this
folder): the game's camera, the studio's lights, materials, a kit of
primitives that take a parent, and the two ways of rendering: a sheet of
views of one model (towers, aliens, the fort, missiles) and a world's
whole picture with the depth of its pixels.

    /Applications/Blender.app/Contents/MacOS/Blender -b -P towers.py -- --out ../../assets

Blender 3.3.1, Cycles on Metal. Everything is built by code and can be run
again headless; nothing here reads a .blend.
"""
import json
import math
import os
import sys

import bpy
import bmesh
import numpy as np
from mathutils import Euler, Matrix, Vector

PX = 40.0               # pixels per metre across the screen
SIN_E, COS_E = 0.8, 0.6  # the camera looks down 53.13 degrees: sin and cos of it
ROW_Y = PX * SIN_E      # 32 px of screen per metre of ground, away from the camera
ROW_Z = PX * COS_E      # 24 px of screen per metre of height
SCREEN_W, SCREEN_H = 720, 1280
CAM_DIST = 80.0

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.normpath(os.path.join(HERE, '..', '..', 'assets'))

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


def flag(name):
    return name in argv()


# ---------------------------------------------------------------------------
# The scene
# ---------------------------------------------------------------------------

def setup(samples=None):
    """an empty scene with Cycles, the game's camera and no lights yet"""
    global sc, cam
    bpy.ops.wm.read_factory_settings(use_empty=True)
    MATS.clear()
    sc = bpy.context.scene
    sc.render.engine = 'CYCLES'
    ok = False
    if not flag('--cpu'):
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
    r.image_settings.file_format = 'OPEN_EXR'
    r.image_settings.color_mode = 'RGBA'
    r.image_settings.color_depth = '32'
    r.image_settings.exr_codec = 'ZIP'
    r.use_persistent_data = False
    sc.view_settings.view_transform = 'Standard'
    sc.view_settings.look = 'None'
    sc.view_settings.exposure = 0.0
    sc.cycles.samples = int(samples or arg('--samples', 48))
    sc.cycles.use_adaptive_sampling = False
    sc.cycles.seed = 7
    sc.cycles.max_bounces = 5
    sc.cycles.diffuse_bounces = 2
    sc.cycles.glossy_bounces = 3
    sc.cycles.transmission_bounces = 4
    sc.cycles.transparent_max_bounces = 6
    sc.cycles.caustics_reflective = False
    sc.cycles.caustics_refractive = False
    sc.cycles.sample_clamp_indirect = 3.0
    sc.cycles.use_denoising = True
    sc.cycles.denoiser = 'OPENIMAGEDENOISE'
    sc.cycles.pixel_filter_type = 'BLACKMAN_HARRIS'
    sc.cycles.filter_width = 1.5

    cd = bpy.data.cameras.new('cam')
    cam = bpy.data.objects.new('cam', cd)
    sc.collection.objects.link(cam)
    sc.camera = cam
    cd.type = 'ORTHO'
    cd.sensor_fit = 'HORIZONTAL'
    cd.clip_start = 1.0
    cd.clip_end = 400.0
    cam.rotation_euler = Euler((math.asin(COS_E), 0, 0), 'XYZ')
    world = bpy.data.worlds.new('world')
    world.use_nodes = True
    sc.world = world
    ambient((0.55, 0.62, 0.80), 0.35)
    return sc


def ambient(color, strength):
    """the sky's light (what fills the shadows)"""
    bg = sc.world.node_tree.nodes['Background']
    bg.inputs['Color'].default_value = (*color, 1)
    bg.inputs['Strength'].default_value = strength


def aim(w, h, ground_y):
    """the camera on a w x h picture whose centre is the ground point
    (0, ground_y, 0); 40 px a metre across"""
    sc.render.resolution_x, sc.render.resolution_y = int(w), int(h)
    cam.data.ortho_scale = w / PX
    fwd = Vector((0, COS_E, -SIN_E))
    cam.location = Vector((0, ground_y, 0)) - fwd * CAM_DIST


def sun(az=205.0, el=52.0, energy=3.2, color=(1.0, 0.96, 0.90), angle=6.0, name='sun'):
    """a sun shining FROM the compass direction az (degrees, 0 = +X, counter
    clockwise seen from above; 205 is from the left and a little in front,
    towards the camera) at elevation el"""
    ld = bpy.data.lights.new(name, 'SUN')
    ld.energy = energy
    ld.color = color
    ld.angle = math.radians(angle)
    o = bpy.data.objects.new(name, ld)
    sc.collection.objects.link(o)
    a, e = math.radians(az), math.radians(el)
    d = Vector((-math.cos(a) * math.cos(e), -math.sin(a) * math.cos(e), -math.sin(e)))  # the light's way
    o.rotation_euler = d.to_track_quat('-Z', 'Y').to_euler()
    return o


def studio():
    """the lights every sprite is rendered under, so they sit together in
    any world: a warm key from the front left, a cool rim from the back
    right, the sky"""
    sun(205, 52, 3.4, (1.0, 0.96, 0.90), name='key')
    sun(40, 35, 1.3, (0.60, 0.78, 1.0), angle=20, name='rim')
    sun(300, 30, 0.7, (1.0, 0.85, 0.95), angle=30, name='fill')
    ambient((0.55, 0.62, 0.80), 0.40)


def point(loc, power, color, radius=0.15, parent=None, name='pt'):
    ld = bpy.data.lights.new(name, 'POINT')
    ld.energy = power
    ld.color = color
    ld.shadow_soft_size = radius
    o = bpy.data.objects.new(name, ld)
    sc.collection.objects.link(o)
    o.location = loc
    if parent is not None:
        o.parent = parent
    return o


# ---------------------------------------------------------------------------
# Materials (colours are sRGB 0..1 tuples or '#rrggbb')
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


def mat(name, color, rough=0.45, metal=0.0, clear=0.0, emit=None, emit_k=0.0, trans=0.0, spec=0.5, alpha=1.0,
        ior=1.45):
    """a Principled material, made once by name"""
    if name in MATS:
        return MATS[name]
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    b = m.node_tree.nodes['Principled BSDF']
    b.inputs['Base Color'].default_value = lin(color)
    b.inputs['Roughness'].default_value = rough
    b.inputs['Metallic'].default_value = metal
    b.inputs['Clearcoat'].default_value = clear
    b.inputs['Specular'].default_value = spec
    b.inputs['IOR'].default_value = ior
    if trans:
        b.inputs['Transmission'].default_value = trans
    if alpha < 1.0:
        b.inputs['Alpha'].default_value = alpha
    if emit is not None:
        b.inputs['Emission'].default_value = lin(emit)
        b.inputs['Emission Strength'].default_value = emit_k
    MATS[name] = m
    return m


def glow(name, color, k=4.0):
    """a surface that only shines (lamps, energy cores, eyes)"""
    if name in MATS:
        return MATS[name]
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    e = nt.nodes.new('ShaderNodeEmission')
    o = nt.nodes.new('ShaderNodeOutputMaterial')
    e.inputs['Color'].default_value = lin(color)
    e.inputs['Strength'].default_value = k
    nt.links.new(e.outputs['Emission'], o.inputs['Surface'])
    MATS[name] = m
    return m


def noise_mat(name, c0, c1, scale=3.0, rough=0.7, metal=0.0, detail=6.0, bump=0.0, emit=None, emit_k=0.0,
              distortion=0.0, contrast=None):
    """two colours mixed by a noise in object coordinates (rock, skin,
    ground); bump > 0 also dents the surface with it. contrast=(lo, hi)
    squeezes the noise into veins or patches."""
    if name in MATS:
        return MATS[name]
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    b = nt.nodes['Principled BSDF']
    tc = nt.nodes.new('ShaderNodeTexCoord')
    nz = nt.nodes.new('ShaderNodeTexNoise')
    nz.inputs['Scale'].default_value = scale
    nz.inputs['Detail'].default_value = detail
    nz.inputs['Distortion'].default_value = distortion
    ramp = nt.nodes.new('ShaderNodeValToRGB')
    lo, hi = contrast if contrast else (0.35, 0.65)
    ramp.color_ramp.elements[0].position = lo
    ramp.color_ramp.elements[1].position = hi
    ramp.color_ramp.elements[0].color = lin(c0)
    ramp.color_ramp.elements[1].color = lin(c1)
    nt.links.new(tc.outputs['Object'], nz.inputs['Vector'])
    nt.links.new(nz.outputs['Fac'], ramp.inputs['Fac'])
    nt.links.new(ramp.outputs['Color'], b.inputs['Base Color'])
    b.inputs['Roughness'].default_value = rough
    b.inputs['Metallic'].default_value = metal
    if bump > 0:
        bp = nt.nodes.new('ShaderNodeBump')
        bp.inputs['Strength'].default_value = bump
        bp.inputs['Distance'].default_value = 0.1
        nt.links.new(nz.outputs['Fac'], bp.inputs['Height'])
        nt.links.new(bp.outputs['Normal'], b.inputs['Normal'])
    if emit is not None:
        b.inputs['Emission'].default_value = lin(emit)
        b.inputs['Emission Strength'].default_value = emit_k
    MATS[name] = m
    return m


# ---------------------------------------------------------------------------
# Primitives. Every one takes: mat, parent (an object or None), loc, rot
# (degrees, XYZ), and returns the object. Sizes in metres, Z up.
# ---------------------------------------------------------------------------

def _place(o, parent, loc, rot, scale=None):
    sc.collection.objects.link(o)
    o.location = loc
    o.rotation_euler = Euler([math.radians(a) for a in rot], 'XYZ')
    if scale is not None:
        o.scale = scale
    if parent is not None:
        o.parent = parent
    return o


def group(name='g', parent=None, loc=(0, 0, 0), rot=(0, 0, 0), scale=None):
    """an empty to hang parts from (turn it and they all turn)"""
    return _place(bpy.data.objects.new(name, None), parent, loc, rot, scale)


def _finish(name, bm, m, parent, loc, rot, smooth, bevel, scale=None):
    me = bpy.data.meshes.new(name)
    bm.normal_update()
    bm.to_mesh(me)
    bm.free()
    if smooth:
        for p in me.polygons:
            p.use_smooth = True
        me.use_auto_smooth = True
        me.auto_smooth_angle = math.radians(50)
    if m is not None:
        me.materials.append(m)
    o = bpy.data.objects.new(name, me)
    if bevel:
        md = o.modifiers.new('bevel', 'BEVEL')
        md.width = bevel
        md.segments = 2
        md.limit_method = 'ANGLE'
        md.angle_limit = math.radians(40)
        for p in me.polygons:
            p.use_smooth = True
        me.use_auto_smooth = True
        me.auto_smooth_angle = math.radians(50)
    return _place(o, parent, loc, rot, scale)


def box(size, m, parent=None, loc=(0, 0, 0), rot=(0, 0, 0), bevel=0.03, name='box'):
    """a box of size (sx, sy, sz) centred on loc"""
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    bmesh.ops.scale(bm, vec=Vector(size), verts=bm.verts)
    return _finish(name, bm, m, parent, loc, rot, False, bevel)


def cyl(r, h, m, parent=None, loc=(0, 0, 0), rot=(0, 0, 0), r2=None, segs=32, bevel=0.0, name='cyl', smooth=True):
    """a cylinder (or a cone with r2) standing on loc: its base at loc.z,
    its top h above; rot turns it about its base centre"""
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=segs, radius1=r,
                          radius2=r if r2 is None else max(r2, 1e-4), depth=h)
    bmesh.ops.translate(bm, vec=Vector((0, 0, h / 2)), verts=bm.verts)
    return _finish(name, bm, m, parent, loc, rot, smooth, bevel)


def sphere(r, m, parent=None, loc=(0, 0, 0), rot=(0, 0, 0), scale=None, segs=32, name='sph'):
    """a ball centred on loc; scale=(x, y, z) makes it an ellipsoid"""
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=segs, v_segments=segs // 2, radius=r)
    return _finish(name, bm, m, parent, loc, rot, True, 0.0, scale)


def ico(r, m, parent=None, loc=(0, 0, 0), rot=(0, 0, 0), scale=None, subdiv=1, name='ico', smooth=False):
    """a faceted ball (crystals, rocks, low-poly armour)"""
    bm = bmesh.new()
    bmesh.ops.create_icosphere(bm, subdivisions=subdiv, radius=r)
    return _finish(name, bm, m, parent, loc, rot, smooth, 0.0, scale)


def torus(R, r, m, parent=None, loc=(0, 0, 0), rot=(0, 0, 0), segs=40, tsegs=12, arc=360.0, name='torus'):
    """a ring of radius R and tube r lying flat (its axis is Z); arc < 360
    leaves it open"""
    bm = bmesh.new()
    n = segs if arc >= 360 else max(3, int(segs * arc / 360))
    rings = []
    cnt = n if arc >= 360 else n + 1
    for i in range(cnt):
        a = math.radians(arc) * i / n
        ring = []
        for j in range(tsegs):
            b = 2 * math.pi * j / tsegs
            rr = R + r * math.cos(b)
            ring.append(bm.verts.new((rr * math.cos(a), rr * math.sin(a), r * math.sin(b))))
        rings.append(ring)
    for i in range(n):
        a, b = rings[i], rings[(i + 1) % cnt]
        for j in range(tsegs):
            bm.faces.new((a[j], b[j], b[(j + 1) % tsegs], a[(j + 1) % tsegs]))
    return _finish(name, bm, m, parent, loc, rot, True, 0.0)


def tube(pts, r, m, parent=None, loc=(0, 0, 0), rot=(0, 0, 0), segs=10, name='tube', r_end=None):
    """a round bar through the points (cables, legs, tentacles, barrels);
    r_end tapers it"""
    bm = bmesh.new()
    P = [Vector(p) for p in pts]
    rings = []
    for i, p in enumerate(P):
        d = (P[min(i + 1, len(P) - 1)] - P[max(i - 1, 0)]).normalized()
        up = Vector((0, 0, 1)) if abs(d.z) < 0.95 else Vector((1, 0, 0))
        u = d.cross(up).normalized()
        v = d.cross(u).normalized()
        k = i / max(1, len(P) - 1)
        rr = r if r_end is None else r + (r_end - r) * k
        rings.append([bm.verts.new(p + (u * math.cos(2 * math.pi * j / segs) + v * math.sin(2 * math.pi * j / segs)) * rr)
                      for j in range(segs)])
    for i in range(len(P) - 1):
        a, b = rings[i], rings[i + 1]
        for j in range(segs):
            bm.faces.new((a[j], a[(j + 1) % segs], b[(j + 1) % segs], b[j]))
    bm.faces.new(list(reversed(rings[0])))
    bm.faces.new(rings[-1])
    return _finish(name, bm, m, parent, loc, rot, True, 0.0)


def lathe(profile, m, parent=None, loc=(0, 0, 0), rot=(0, 0, 0), segs=32, name='lathe', smooth=True):
    """a profile [(radius, z), ...] from bottom to top turned about Z
    (domes, nozzles, bodies, pads); the ends are capped"""
    bm = bmesh.new()
    rings = []
    for (r, z) in profile:
        rings.append([bm.verts.new((max(r, 1e-4) * math.cos(2 * math.pi * j / segs),
                                    max(r, 1e-4) * math.sin(2 * math.pi * j / segs), z)) for j in range(segs)])
    for i in range(len(rings) - 1):
        a, b = rings[i], rings[i + 1]
        for j in range(segs):
            bm.faces.new((a[j], a[(j + 1) % segs], b[(j + 1) % segs], b[j]))
    bm.faces.new(list(reversed(rings[0])))
    bm.faces.new(rings[-1])
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-5)
    return _finish(name, bm, m, parent, loc, rot, smooth, 0.0)


def prism(outline, z0, z1, m, parent=None, loc=(0, 0, 0), rot=(0, 0, 0), bevel=0.02, name='prism'):
    """a flat outline [(x, y), ...] (counter clockwise) raised from z0 to z1
    (plates, fins, hexagons, wings)"""
    bm = bmesh.new()
    lo = [bm.verts.new((x, y, z0)) for (x, y) in outline]
    hi = [bm.verts.new((x, y, z1)) for (x, y) in outline]
    n = len(outline)
    bm.faces.new(list(reversed(lo)))
    bm.faces.new(hi)
    for i in range(n):
        bm.faces.new((lo[i], lo[(i + 1) % n], hi[(i + 1) % n], hi[i]))
    return _finish(name, bm, m, parent, loc, rot, False, bevel)


def ngon(n, r, start=0.0):
    """the outline of a regular polygon, for prism()"""
    return [(r * math.cos(math.radians(start) + 2 * math.pi * i / n),
             r * math.sin(math.radians(start) + 2 * math.pi * i / n)) for i in range(n)]


def mesh(verts, faces, m, parent=None, loc=(0, 0, 0), rot=(0, 0, 0), smooth=False, name='mesh', bevel=0.0):
    """any mesh from vertices and faces (index tuples)"""
    bm = bmesh.new()
    vs = [bm.verts.new(v) for v in verts]
    for f in faces:
        try:
            bm.faces.new([vs[i] for i in f])
        except ValueError:
            pass
    return _finish(name, bm, m, parent, loc, rot, smooth, bevel)


def subsurf(o, levels=2):
    md = o.modifiers.new('sub', 'SUBSURF')
    md.levels = levels
    md.render_levels = levels
    for p in o.data.polygons:
        p.use_smooth = True
    return o


def displace(o, strength=0.2, scale=1.0, seed=0):
    """a lumpy surface (rocks, organic bodies): a cloud texture pushes it"""
    tex = bpy.data.textures.new('disp', 'CLOUDS')
    tex.noise_scale = scale
    md = o.modifiers.new('disp', 'DISPLACE')
    md.texture = tex
    md.strength = strength
    md.texture_coords = 'GLOBAL' if seed else 'LOCAL'
    return o


def clear_objects(keep=('cam',)):
    """everything built so far goes; the camera and lights of 'keep' stay"""
    for o in list(sc.objects):
        if o.name in keep or o.type == 'LIGHT':
            continue
        bpy.data.objects.remove(o, do_unlink=True)
    for me in list(bpy.data.meshes):
        if me.users == 0:
            bpy.data.meshes.remove(me)


# ---------------------------------------------------------------------------
# Rendering
# ---------------------------------------------------------------------------

def read_exr(path):
    img = bpy.data.images.load(path, check_existing=False)
    w, h = img.size
    a = np.empty(w * h * 4, np.float32)
    img.pixels.foreach_get(a)
    bpy.data.images.remove(img)
    return a.reshape(h, w, 4)[::-1].copy()


def srgb(x):
    x = np.clip(x, 0, 1)
    return np.where(x <= 0.0031308, x * 12.92, 1.055 * np.power(x, 1 / 2.4) - 0.055)


def write_png(a, path, alpha=True):
    """a float (h, w, 4) array, linear and premultiplied as Cycles gives
    it -> an 8-bit sRGB PNG with straight alpha"""
    os.makedirs(os.path.dirname(path), exist_ok=True)
    h, w = a.shape[:2]
    al = np.clip(a[..., 3:4], 0, 1)
    c = a[..., :3] / np.maximum(al, 1e-4) if alpha else a[..., :3]
    out = np.concatenate([srgb(c), al if alpha else np.ones((h, w, 1), np.float32)], axis=2)
    img = bpy.data.images.new('out', w, h, alpha=True, float_buffer=False)
    img.alpha_mode = 'STRAIGHT'
    img.pixels.foreach_set(out[::-1].astype(np.float32).ravel())
    img.filepath_raw = path
    img.file_format = 'PNG'
    img.save()
    bpy.data.images.remove(img)


def render(transparent=True, tmp=None):
    """the picture the camera sees now: float (h, w, 4), linear"""
    tmp = tmp or os.path.join(bpy.app.tempdir, 'bs_render.exr')
    sc.render.film_transparent = transparent
    sc.render.filepath = tmp
    bpy.ops.render.render(write_still=True)
    return read_exr(tmp)


def render_sheet(name, build, out_dir, cols, rows=1, cell=(128, 160), anchor=(64, 120), yaws=None, meta=None):
    """One model seen in cols x rows cells -> <out_dir>/<name>.png + .json

    build(parent, col, row) builds the model under 'parent', standing on
    the origin (Z = 0 is the ground it stands on), as cell (col, row) wants
    it; it is called once per cell with a fresh parent. With yaws (degrees,
    one per column; 0 looks along +X, 90 away from the camera) the parent
    is already turned, so build() ignores col and uses row as the frame of
    an animation. Without yaws, col is whatever the sheet says it is (a
    frame, a state).

    'anchor' is the pixel of the cell where the model's origin lands. Every
    row is its own render (so no cell shadows another)."""
    cw, ch = cell
    ax, ay = anchor
    strip = []
    for row in range(rows):
        clear_objects()
        for col in range(cols):
            x = (col - (cols - 1) / 2.0) * (cw / PX) + (ax - cw / 2.0) / PX
            g = group('cell%d' % col, loc=(x, 0, 0), rot=(0, 0, yaws[col] if yaws else 0))
            build(g, col, row)
        aim(cw * cols, ch, (ay - ch / 2.0) / ROW_Y)
        strip.append(render(True))
    a = np.concatenate(strip, axis=0)
    path = os.path.join(out_dir, name + '.png')
    write_png(a, path)
    info = dict(cell=[cw, ch], anchor=[ax, ay], cols=cols, rows=rows, yaws=list(yaws) if yaws else None)
    if meta:
        info.update(meta)
    with open(os.path.join(out_dir, name + '.json'), 'w') as f:
        json.dump(info, f)
    print('sheet', path, a.shape)
    return path


def screen_of(x, y, z=0.0):
    """where the world point lands on the 720 x 1280 screen"""
    return (SCREEN_W / 2 + x * PX, SCREEN_H - y * ROW_Y - z * ROW_Z)


_POS = None


def render_world(name, out_dir):
    """The scene as the game's screen: <name>_bg.png (720 x 1280, opaque)
    and <name>_depth.npy, float32 (1280, 720): for every pixel
    0.6 * Y - 0.8 * Z of the surface seen there, in metres (the distance
    along the camera's way, zero at the world's origin), which is what the
    game compares a sprite against to draw it behind a rock or an arch."""
    global _POS
    aim(SCREEN_W, SCREEN_H, SCREEN_H / 2.0 / ROW_Y)
    a = render(False)
    write_png(a, os.path.join(out_dir, name + '_bg.png'), alpha=False)
    if _POS is None:
        _POS = bpy.data.materials.new('pos')
        _POS.use_nodes = True
        nt = _POS.node_tree
        for n in list(nt.nodes):
            nt.nodes.remove(n)
        g = nt.nodes.new('ShaderNodeNewGeometry')
        e = nt.nodes.new('ShaderNodeEmission')
        o = nt.nodes.new('ShaderNodeOutputMaterial')
        nt.links.new(g.outputs['Position'], e.inputs['Color'])
        nt.links.new(e.outputs['Emission'], o.inputs['Surface'])
    vl = sc.view_layers[0]
    keep = (sc.cycles.samples, sc.cycles.use_denoising, sc.cycles.filter_width, sc.cycles.pixel_filter_type)
    vl.material_override = _POS
    sc.cycles.samples = 1
    sc.cycles.use_denoising = False
    sc.cycles.pixel_filter_type = 'BOX'
    sc.cycles.filter_width = 0.01
    p = render(True)
    vl.material_override = None
    sc.cycles.samples, sc.cycles.use_denoising, sc.cycles.filter_width, sc.cycles.pixel_filter_type = keep
    d = COS_E * p[..., 1] - SIN_E * p[..., 2]
    d = np.where(p[..., 3] > 0.5, d, 200.0).astype(np.float32)
    np.save(os.path.join(out_dir, name + '_depth.npy'), d)
    print('world', name, 'depth', float(d.min()), float(d[d < 199].max()))
