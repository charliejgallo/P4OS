"""
METEGOL - what every Blender script of the game shares: the game's
camera, the table's measures (the same as main/mg_game.h and mg_view.h),
materials, a kit of primitives and the ways of rendering.

    /Applications/Blender.app/Contents/MacOS/Blender -b -P figures.py

Blender 3.3.1, Cycles on Metal. Everything is built by code and can be run
again headless; nothing here reads a .blend.

Coordinates: the game's, in millimetres (x along the field, y across it
growing towards the camera, z up), turned into Blender's metres by W():
Blender's X is x, its Y is -y, its Z is z. The camera looks down 32
degrees from the vertical, orthographic, at 0.8 pixels a millimetre: a
point (x, y, z) is at screen (640 + 0.8 x, 450 + 0.8 (y cos32 - z sin32)).
"""
import math
import os
import sys

import bpy
import bmesh
from mathutils import Matrix, Vector

S = 0.8                         # pixels a millimetre
TILT = math.radians(32.0)       # from the vertical
COS_T, SIN_T = math.cos(TILT), math.sin(TILT)
SCREEN_W, SCREEN_H = 1280, 720
CX, CY = 640, 450               # where the field's centre is on the screen

FIELD_X, FIELD_Y = 600.0, 340.0
GOAL_Y, GOAL_D = 100.0, 70.0
CORNER_R = 55.0
BALL_R = 17.5
ROD_Z = 58.0
WALL_H = 72.0                   # the bands over the field
WALL_T = 26.0                   # their thickness
END_T = 30.0
ROD_X = [-560.0, -390.0, -75.0, 225.0]      # side 0; side 1 is the mirror
ROD_N = [1, 2, 5, 3]
ROD_SPACING = [0.0, 240.0, 122.0, 205.0]
ROD_TRAVEL = [160.0, 190.0, 71.0, 110.0]
ROD_R = 7.0                     # the rod's radius
HANDLE = 190.0                  # how far a handle stands out of the band

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

def setup(samples=64):
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
    sc.render.film_transparent = True
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


def camera(cx_px, cy_px, w_px, h_px):
    """the game's camera, framing w x h pixels whose centre is the screen's
    point (cx_px, cy_px) of a 1280 x 720 screen (it may be anywhere: a sheet
    is framed around 0, 0 in its own space with the same scale)"""
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
    # the ground point that lands at the frame's centre
    gx = (cx_px - CX) / S
    gy = (cy_px - CY) / (S * COS_T)
    target = W(gx, gy, 0)
    d = Vector((0, -math.sin(TILT), math.cos(TILT))) * 10.0
    cam.location = target + d
    cam.rotation_euler = (TILT, 0, 0)
    return cam


def studio(strength=1.0, warm=(1.0, 0.97, 0.92)):
    """a key light from above and from the camera's side, a fill, a rim:
    symmetric in x, so that a sprite mirrored is the same figure turned"""
    def sun(name, rot, energy, col):
        ld = bpy.data.lights.new(name, 'SUN')
        ld.energy = energy * strength
        ld.color = col
        ld.angle = math.radians(8)
        o = bpy.data.objects.new(name, ld)
        o.rotation_euler = rot
        sc.collection.objects.link(o)
        return o
    sun('key', (math.radians(35), 0, 0), 3.2, warm)
    sun('fill', (math.radians(-60), 0, 0), 0.9, (0.85, 0.9, 1.0))
    sun('top', (0, 0, 0), 1.2, (1, 1, 1))


# ---------------------------------------------------------------------------
# Materials
# ---------------------------------------------------------------------------

def mat_principled(name, col, rough=0.5, metal=0.0, spec=0.5, emit=None):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    b = m.node_tree.nodes['Principled BSDF']
    b.inputs['Base Color'].default_value = (*col, 1)
    b.inputs['Roughness'].default_value = rough
    b.inputs['Metallic'].default_value = metal
    b.inputs['Specular'].default_value = spec
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


def srgb_to_lin(c):
    return c / 12.92 if c <= 0.04045 else ((c + 0.055) / 1.055) ** 2.4


def hexc(h):
    h = h.lstrip('#')
    return tuple(srgb_to_lin(int(h[i:i + 2], 16) / 255.0) for i in (0, 2, 4))


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


def cyl(name, p0, p1, r, mat=None, parent=None, verts=24, r1=None):
    """a cylinder (or a cone, r1) from p0 to p1, game millimetres"""
    a, b = W(*p0), W(*p1)
    d = b - a
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, segments=verts, radius1=r / 1000.0,
                          radius2=(r if r1 is None else r1) / 1000.0, depth=d.length)
    o = mesh_obj(name, bm, mat, parent)
    o.location = (a + b) / 2
    o.rotation_mode = 'QUATERNION'
    o.rotation_quaternion = Vector((0, 0, 1)).rotation_difference(d.normalized())
    return o


def sphere(name, c, r, mat=None, parent=None, scale=(1, 1, 1), seg=24):
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=seg, v_segments=seg // 2, radius=r / 1000.0)
    o = mesh_obj(name, bm, mat, parent)
    o.location = W(*c)
    o.scale = scale
    return o


def render(path):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    sc.render.filepath = path
    bpy.ops.render.render(write_still=True)
    print('wrote', path)
