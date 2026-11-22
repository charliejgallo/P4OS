"""
HOCKEY - what every Blender script of the game shares: the game's camera,
the table's measures (the same as main/hk_game.h and hk_view.h),
materials, a kit of primitives and the way of rendering.

    /Applications/Blender.app/Contents/MacOS/Blender -b -P table.py -- --theme bar

Blender 3.3.1, Cycles on Metal. Everything is built by code and can be run
again headless; nothing here reads a .blend.

Coordinates: the game's, in millimetres (x across the table, y along it
growing towards the camera, z up), turned into Blender's metres by W():
Blender's X is x, its Y is -y, its Z is z. The camera looks down 16
degrees from the vertical, orthographic, at 1 pixel a millimetre: a point
(x, y, z) is at screen (360 + x, 630 + (y cos16 - z sin16)).
"""
import math
import os
import sys

import bpy
import bmesh
from mathutils import Vector

S = 1.0                         # pixels a millimetre
TILT = math.radians(16.0)       # from the vertical
COS_T, SIN_T = math.cos(TILT), math.sin(TILT)
SCREEN_W, SCREEN_H = 720, 1280
CX, CY = 360, 630               # where the table's centre is on the screen

FX, FY = 280.0, 520.0           # half the playing surface
GOAL_X = 92.0
CORNER_R = 60.0
PUCK_R = 26.0
PUCK_H = 6.0
RAIL_H = 32.0                   # the rails over the surface
SIDE_W = 62.0                   # the side rails' width
END_W = 70.0                    # the end rails'
SLOT_H = 15.0                   # the goal's slot in the end rail
OUT_X, OUT_Y = FX + SIDE_W, FY + END_W
MALLETS = {'s': 33.0, 'm': 40.0, 'l': 49.0}
MALLET_H = 0.0                  # set per size: see mallet()

HERE = os.path.dirname(os.path.abspath(__file__))
ASSETS = os.path.normpath(os.path.join(HERE, '..', '..', 'assets'))

sc = None
cam = None


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


def W(x, y, z):
    """the game's millimetres to Blender's metres"""
    return Vector((x / 1000.0, -y / 1000.0, z / 1000.0))


def screen(x, y, z):
    return CX + S * x, CY + S * (y * COS_T - z * SIN_T)


# ---------------------------------------------------------------------------
# The scene
# ---------------------------------------------------------------------------

def setup(samples=64, transparent=True):
    global sc
    bpy.ops.wm.read_factory_settings(use_empty=True)
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
    sc.cycles.samples = samples
    sc.cycles.use_denoising = True
    sc.cycles.max_bounces = 6
    sc.render.film_transparent = transparent
    sc.view_settings.view_transform = 'Standard'
    sc.view_settings.look = 'None'
    sc.render.image_settings.file_format = 'PNG'
    sc.render.image_settings.color_mode = 'RGBA'
    sc.render.image_settings.color_depth = '8'
    sc.render.resolution_percentage = 100
    w = bpy.data.worlds.new('world')
    sc.world = w
    w.use_nodes = True
    w.node_tree.nodes['Background'].inputs[0].default_value = (0.05, 0.055, 0.07, 1)
    w.node_tree.nodes['Background'].inputs[1].default_value = 1.0
    return sc


def world(col, strength):
    bg = sc.world.node_tree.nodes['Background']
    bg.inputs[0].default_value = (*col, 1)
    bg.inputs[1].default_value = strength


def camera(cx_px, cy_px, w_px, h_px):
    """the game's camera, framing w x h pixels whose centre is the screen's
    point (cx_px, cy_px) of the 720 x 1280 screen (it may be anywhere: a
    sprite is framed around its own anchor with the same scale)"""
    global cam
    sc.render.resolution_x = int(w_px)
    sc.render.resolution_y = int(h_px)
    cd = bpy.data.cameras.new('cam')
    cd.type = 'ORTHO'
    cd.ortho_scale = max(w_px, h_px) / S / 1000.0
    cd.clip_start = 0.01
    cd.clip_end = 50.0
    cam = bpy.data.objects.new('cam', cd)
    sc.collection.objects.link(cam)
    sc.camera = cam
    gx = (cx_px - CX) / S
    gy = (cy_px - CY) / (S * COS_T)
    target = W(gx, gy, 0)
    d = Vector((0, -math.sin(TILT), math.cos(TILT))) * 10.0
    cam.location = target + d
    cam.rotation_euler = (TILT, 0, 0)
    return cam


def area(name, loc_mm, size_mm, energy, col=(1, 1, 1)):
    ld = bpy.data.lights.new(name, 'AREA')
    ld.shape = 'RECTANGLE'
    ld.size = size_mm[0] / 1000.0
    ld.size_y = size_mm[1] / 1000.0
    ld.energy = energy
    ld.color = col
    o = bpy.data.objects.new(name, ld)
    o.location = W(*loc_mm)
    sc.collection.objects.link(o)
    return o


def point(name, loc_mm, energy, col=(1, 1, 1), radius=40.0):
    ld = bpy.data.lights.new(name, 'POINT')
    ld.energy = energy
    ld.color = col
    ld.shadow_soft_size = radius / 1000.0
    o = bpy.data.objects.new(name, ld)
    o.location = W(*loc_mm)
    sc.collection.objects.link(o)
    return o


# ---------------------------------------------------------------------------
# Materials
# ---------------------------------------------------------------------------

def srgb_to_lin(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def hexc(h):
    h = h.lstrip('#')
    return tuple(srgb_to_lin(int(h[i:i + 2], 16) / 255.0) for i in (0, 2, 4))


def mat_principled(name, col, rough=0.5, metal=0.0, spec=0.5, emit=None, clearcoat=0.0, transmission=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    b = m.node_tree.nodes['Principled BSDF']
    b.inputs['Base Color'].default_value = (*col, 1)
    b.inputs['Roughness'].default_value = rough
    b.inputs['Metallic'].default_value = metal
    b.inputs['Specular'].default_value = spec
    b.inputs['Clearcoat'].default_value = clearcoat
    b.inputs['Transmission'].default_value = transmission
    if emit:
        b.inputs['Emission'].default_value = (*emit[0], 1)
        b.inputs['Emission Strength'].default_value = emit[1]
    return m


def mat_emit(name, col, strength=1.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    e = nt.nodes.new('ShaderNodeEmission')
    e.inputs[0].default_value = (*col, 1)
    e.inputs[1].default_value = strength
    o = nt.nodes.new('ShaderNodeOutputMaterial')
    nt.links.new(e.outputs[0], o.inputs[0])
    return m


def mat_wood(name, col, dark, scale=2.5):
    """lacquered wood: rings from a wave texture"""
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    b = nt.nodes['Principled BSDF']
    b.inputs['Roughness'].default_value = 0.3
    b.inputs['Specular'].default_value = 0.55
    b.inputs['Clearcoat'].default_value = 0.3
    wv = nt.nodes.new('ShaderNodeTexWave')
    wv.wave_type = 'RINGS'
    wv.inputs['Scale'].default_value = scale
    wv.inputs['Distortion'].default_value = 3.0
    wv.inputs['Detail'].default_value = 2.0
    wv.inputs['Detail Scale'].default_value = 1.5
    ramp = nt.nodes.new('ShaderNodeValToRGB')
    ramp.color_ramp.elements[0].color = (*hexc(dark), 1)
    ramp.color_ramp.elements[1].color = (*hexc(col), 1)
    nt.links.new(wv.outputs['Fac'], ramp.inputs[0])
    nt.links.new(ramp.outputs[0], b.inputs['Base Color'])
    return m


def mat_image(name, img, rough=0.4, spec=0.4, emit_img=None, emit_strength=0.0, clearcoat=0.0):
    """a surface painted by an image (UVs 0..1 over it)"""
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    nt = m.node_tree
    b = nt.nodes['Principled BSDF']
    b.inputs['Roughness'].default_value = rough
    b.inputs['Specular'].default_value = spec
    b.inputs['Clearcoat'].default_value = clearcoat
    t = nt.nodes.new('ShaderNodeTexImage')
    t.image = img
    t.interpolation = 'Linear'
    nt.links.new(t.outputs[0], b.inputs['Base Color'])
    if emit_img is not None:
        e = nt.nodes.new('ShaderNodeTexImage')
        e.image = emit_img
        nt.links.new(e.outputs[0], b.inputs['Emission'])
        b.inputs['Emission Strength'].default_value = emit_strength
    return m


def image(name, rgba):
    """a numpy (h, w, 4) float image in linear light, row 0 the far side"""
    h, w = rgba.shape[:2]
    im = bpy.data.images.new(name, w, h, float_buffer=True)
    im.colorspace_settings.name = 'Linear'
    im.pixels.foreach_set(rgba[::-1].astype('float32').ravel())
    im.update()
    im.pack()
    return im


# ---------------------------------------------------------------------------
# Primitives (millimetres in, objects out)
# ---------------------------------------------------------------------------

def link(o, parent=None):
    sc.collection.objects.link(o)
    if parent:
        o.parent = parent
    return o


def mesh_obj(name, bm, mat=None, parent=None, smooth=True):
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    if smooth:
        for p in me.polygons:
            p.use_smooth = True
    o = bpy.data.objects.new(name, me)
    if mat:
        o.data.materials.append(mat)
    return link(o, parent)


def box(name, x0, y0, z0, x1, y1, z1, mat=None, parent=None, bevel=0.0, seg=2):
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    for v in bm.verts:
        v.co.x = (x0 + x1) / 2000.0 + v.co.x * (x1 - x0) / 1000.0
        v.co.y = -((y0 + y1) / 2000.0) - v.co.y * (y1 - y0) / 1000.0
        v.co.z = (z0 + z1) / 2000.0 + v.co.z * (z1 - z0) / 1000.0
    o = mesh_obj(name, bm, mat, parent, smooth=bevel > 0)
    if bevel > 0:
        m = o.modifiers.new('bevel', 'BEVEL')
        m.width = bevel / 1000.0
        m.segments = seg
        m.limit_method = 'NONE'
    return o


def prism(name, pts, z0, z1, mat=None, bevel=0.0, seg=3, smooth=False):
    """a polygon of game (x, y) points extruded from z0 to z1"""
    bm = bmesh.new()
    vs = [bm.verts.new(W(x, y, z0)) for (x, y) in pts]
    f = bm.faces.new(vs)
    bmesh.ops.recalc_face_normals(bm, faces=[f])
    if f.normal.z > 0:
        f.normal_flip()
    r = bmesh.ops.extrude_face_region(bm, geom=[f])
    top = [e for e in r['geom'] if isinstance(e, bmesh.types.BMVert)]
    bmesh.ops.translate(bm, verts=top, vec=Vector((0, 0, (z1 - z0) / 1000.0)))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    o = mesh_obj(name, bm, mat, smooth=smooth)
    if bevel > 0:
        m = o.modifiers.new('bevel', 'BEVEL')
        m.width = bevel / 1000.0
        m.segments = seg
        m.limit_method = 'ANGLE'
        m.angle_limit = math.radians(40)
        m.harden_normals = False
    return o


def cyl(name, x, y, z0, z1, r, mat=None, verts=64, r1=None):
    """an upright cylinder (or a cone, r1 at the top), game millimetres"""
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, segments=verts, radius1=r / 1000.0,
                          radius2=(r if r1 is None else r1) / 1000.0, depth=(z1 - z0) / 1000.0)
    o = mesh_obj(name, bm, mat)
    o.location = W(x, y, (z0 + z1) / 2.0)
    return o


def lathe(name, profile, mat=None, segs=96, x=0.0, y=0.0):
    """a body of revolution about the vertical: profile is (r, z) points in mm
    from the bottom's centre up to the top's"""
    bm = bmesh.new()
    rings = []
    for k in range(segs):
        a = 2 * math.pi * k / segs
        ring = []
        for (r, z) in profile:
            ring.append(bm.verts.new(W(x + r * math.cos(a), y + r * math.sin(a), z)))
        rings.append(ring)
    n = len(profile)
    for k in range(segs):
        a, b = rings[k], rings[(k + 1) % segs]
        for i in range(n - 1):
            if a[i].co == a[i + 1].co:
                continue
            try:
                bm.faces.new((a[i], b[i], b[i + 1], a[i + 1]))
            except ValueError:
                pass
    bmesh.ops.remove_doubles(bm, verts=bm.verts[:], dist=1e-6)
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    return mesh_obj(name, bm, mat, smooth=True)


def render(path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    sc.render.filepath = path
    bpy.ops.render.render(write_still=True)
    print('wrote', path)
