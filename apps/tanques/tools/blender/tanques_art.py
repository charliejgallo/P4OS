"""Tanques - the sample's art, built by code and rendered to sprites.

    Blender -b -P tanques_art.py -- --out ../../build/art [--only tank,tree] [--samples 48]

Blender 3.3, Cycles. Nothing is loaded from disk: every model is made here.

Three cameras, and the app knows all three (tq_view.h has the same numbers):

  top   the battle's third-person view: orthographic, yaw 20 deg, elevation
        55 deg, 32 px per metre. The world never turns under it, so a sprite
        drawn at its anchor lines up with the terrain the app renders.
        Tanks are rendered at 32 world yaws (11.25 deg apart).
  low   the first-person view of things far away: orthographic, elevation
        8 deg, 64 px per metre, the camera looking along +Y. Sprites are
        indexed by the yaw RELATIVE to the view (0 = we see the tank's back),
        and the app scales them by distance.
  fp    the gunner's own turret and barrel over the first-person view:
        perspective, 620 px focal length at the screen's size, one frame per
        5 deg of barrel elevation.

Tanks, trees and stones are not rendered in colour. Every sprite has four
planes the app colours itself, so one model comes in any paint:

  L   the shading on a neutral grey (0..255; a face in full sun is ~200)
  A   coverage (antialiased)
  ID  the region: 1 paint, 2 second paint, 3 tracks, 4 metal, 5 lamps,
      6 the badge (the logo, later), 8 leaves, 9 bark, 10 stone, 11 cut wood
  Z   depth along the view relative to the anchor, 128 = the anchor,
      1 step = 1/32 m, smaller = nearer (0 and 255 clamp)

Each sprite goes to <out>/<set>/<name>.spr: 'TSPR', u16 w, h, then i16 ax,
ay (the pixel corner where the anchor lands, from the cropped image's
top-left), then the four planes of w*h bytes. tools/pack.py makes the pack
and the contact sheets.
"""

import argparse
import math
import os
import struct
import sys
import tempfile

import bpy
import bmesh
import numpy as np
from mathutils import Matrix, Vector as V

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
ap = argparse.ArgumentParser()
ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "build", "art"))
ap.add_argument("--only", default="")
ap.add_argument("--samples", type=int, default=48)
ap.add_argument("--yaws", type=int, default=32)
ap.add_argument("--cpu", action="store_true")
ap.add_argument("--tanks", default="", help="which tanks (default: all six)")
ap.add_argument("--biomes", default="", help="which biomes (default: all five)")
ap.add_argument("--sets", default="top,low", help="which of top,low to render (tank, scenery)")
ARGS = ap.parse_args(argv)
OUT = os.path.abspath(ARGS.out)
ONLY = set(s for s in ARGS.only.split(",") if s)
TMP = tempfile.mkdtemp(prefix="tq_art_")

# ---------------------------------------------------------------------------
# The cameras (tq_view.h)

TOP_YAW, TOP_ELEV, TOP_S = 20.0, 55.0, 32.0
LOW_ELEV, LOW_S = 8.0, 48.0
FP_F = 620.0                       # px, at the screen's size
FP_EYE = V((-0.75, 0.52, 2.42))    # in the turret's frame (pivot on the ground): beside the barrel
FP_PITCH = -7.0                    # the eye looks this much down
SUN_AZ, SUN_ELEV = 225.0, 52.0     # towards the sun: behind the viewer, on the left
DEPTH_UNIT = 1.0 / 32.0


def view_axes(yaw_deg, elev_deg):
    """F (view direction), R (screen right), UP (screen up) for a camera whose
    horizontal heading is +Y turned by -yaw (yaw 0 looks along +Y)."""
    y, e = math.radians(yaw_deg), math.radians(elev_deg)
    F = V((-math.sin(y) * math.cos(e), math.cos(y) * math.cos(e), -math.sin(e)))
    R = V((math.cos(y), math.sin(y), 0.0))
    UP = R.cross(F)
    if UP.z < 0:
        UP = -UP
    return F, R, UP


# ---------------------------------------------------------------------------
# Scene

def reset():
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    sc.render.engine = "CYCLES"
    cy = sc.cycles
    if not ARGS.cpu:
        try:
            prefs = bpy.context.preferences.addons["cycles"].preferences
            prefs.compute_device_type = "METAL"
            prefs.get_devices()
            for d in prefs.devices:
                d.use = True
            cy.device = "GPU"
        except Exception as e:      # no Metal: the CPU renders it
            print("no GPU:", e)
    sc.render.film_transparent = True
    sc.view_settings.view_transform = "Standard"
    sc.view_settings.look = "None"
    sc.render.dither_intensity = 0.0
    sc.render.image_settings.file_format = "OPEN_EXR"
    sc.render.image_settings.color_depth = "32"
    sc.render.image_settings.color_mode = "RGBA"
    w = bpy.data.worlds.new("w")
    w.use_nodes = True
    w.node_tree.nodes["Background"].inputs[0].default_value = (1, 1, 1, 1)
    w.node_tree.nodes["Background"].inputs[1].default_value = 0.30
    sc.world = w
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    sc.collection.objects.link(cam)
    sc.camera = cam
    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
    sun.data.energy = 4.2
    sun.data.angle = math.radians(14)
    sc.collection.objects.link(sun)
    return sc


def set_sun(az_deg, elev_deg):
    sun = bpy.data.objects["sun"]
    a, e = math.radians(az_deg), math.radians(elev_deg)
    d = V((math.cos(e) * math.cos(a), math.cos(e) * math.sin(a), math.sin(e)))   # towards the sun
    # a sun light shines down its local -Z
    sun.matrix_world = d.to_track_quat("Z", "Y").to_matrix().to_4x4()


# ---------------------------------------------------------------------------
# Materials: a grey one per region for the shading, an emission one per
# region for the id and depth pass

REGION_LOOK = {   # id: (roughness, specular, emission)
    1: (0.42, 0.40, 0.0), 2: (0.42, 0.40, 0.0), 3: (0.85, 0.08, 0.0),
    4: (0.28, 0.65, 0.0), 5: (0.30, 0.50, 0.9), 6: (0.40, 0.40, 0.0),
    8: (0.80, 0.15, 0.0), 9: (0.85, 0.10, 0.0), 10: (0.75, 0.15, 0.0),
    11: (0.80, 0.10, 0.0), 7: (0.60, 0.30, 0.0), 12: (0.40, 0.30, 1.4),
}
_mats = {}


def mat_light(rid):
    key = ("L", rid)
    if key in _mats:
        return _mats[key]
    m = bpy.data.materials.new("L%d" % rid)
    m.use_nodes = True
    b = m.node_tree.nodes["Principled BSDF"]
    rough, spec, emit = REGION_LOOK[rid]
    b.inputs["Base Color"].default_value = (0.8, 0.8, 0.8, 1)
    b.inputs["Roughness"].default_value = rough
    b.inputs["Specular"].default_value = spec
    if emit:
        b.inputs["Emission"].default_value = (1, 1, 1, 1)
        b.inputs["Emission Strength"].default_value = emit
    _mats[key] = m
    return m


DEPTH_DIR = [V((0, 0, -1))]        # the view direction of the set being rendered


def mat_id(rid):
    key = ("I", rid)
    if key in _mats:
        return _mats[key]
    m = bpy.data.materials.new("I%d" % rid)
    m.use_nodes = True
    nt = m.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    out = nt.nodes.new("ShaderNodeOutputMaterial")
    em = nt.nodes.new("ShaderNodeEmission")
    geo = nt.nodes.new("ShaderNodeNewGeometry")
    dot = nt.nodes.new("ShaderNodeVectorMath")
    dot.operation = "DOT_PRODUCT"
    dot.name = "depth_dot"
    comb = nt.nodes.new("ShaderNodeCombineRGB")
    comb.inputs["R"].default_value = (rid + 0.5) / 16.0
    nt.links.new(geo.outputs["Position"], dot.inputs[0])
    nt.links.new(dot.outputs["Value"], comb.inputs["G"])
    if rid == 6:
        # the badge's own coordinates (its box, 0..1), 16 x 16 cells in B:
        # where the app draws the player's emblem
        tc = nt.nodes.new("ShaderNodeTexCoord")
        sep = nt.nodes.new("ShaderNodeSeparateXYZ")
        nt.links.new(tc.outputs["Generated"], sep.inputs[0])

        def op(kind, a, b):
            n = nt.nodes.new("ShaderNodeMath")
            n.operation = kind
            for i, v in enumerate((a, b)):
                if isinstance(v, float):
                    n.inputs[i].default_value = v
                else:
                    nt.links.new(v, n.inputs[i])
            return n.outputs[0]
        u = op("FLOOR", op("MULTIPLY", sep.outputs["X"], 15.999), 0.0)
        v = op("FLOOR", op("MULTIPLY", sep.outputs["Y"], 15.999), 0.0)
        b = op("DIVIDE", op("ADD", op("ADD", op("MULTIPLY", u, 16.0), v), 0.5), 256.0)
        nt.links.new(b, comb.inputs["B"])
    nt.links.new(comb.outputs["Image"], em.inputs["Color"])
    nt.links.new(em.outputs["Emission"], out.inputs["Surface"])
    _mats[key] = m
    return m


def set_depth_dir(F):
    for (kind, rid), m in _mats.items():
        if kind == "I":
            m.node_tree.nodes["depth_dot"].inputs[1].default_value = tuple(F)


def tag(ob, rid):
    ob["rid"] = rid
    ob.data.materials.clear()
    ob.data.materials.append(mat_light(rid))
    mat_id(rid)
    return ob


def use_pass(objs, kind):
    for ob in objs:
        if "rid" in ob:
            ob.data.materials[0] = mat_light(ob["rid"]) if kind == "L" else mat_id(ob["rid"])


# ---------------------------------------------------------------------------
# Geometry helpers

def smooth(ob):
    for p in ob.data.polygons:
        p.use_smooth = True
    ob.data.use_auto_smooth = True
    ob.data.auto_smooth_angle = math.radians(60)


def link(ob):
    bpy.context.scene.collection.objects.link(ob)
    return ob


def mesh_ob(name, bm):
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    bm.free()
    return link(bpy.data.objects.new(name, me))


def rbox(name, c, size, r, rid, seg=5, shape=None):
    """A box with rounded edges: centre c, full size, bevel radius r.
    shape(v) may move the cube's corners before the bevel (wedges)."""
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    for v in bm.verts:
        v.co = V((v.co.x * size[0], v.co.y * size[1], v.co.z * size[2]))
        if shape:
            shape(v)
    ob = mesh_ob(name, bm)
    ob.location = c
    md = ob.modifiers.new("bev", "BEVEL")
    md.width = r
    md.segments = seg
    md.limit_method = "NONE"
    md.profile = 0.5
    md.harden_normals = False
    sub = ob.modifiers.new("sub", "SUBSURF")
    sub.levels = 1
    sub.render_levels = 1
    smooth(ob)
    return tag(ob, rid)


def cyl(name, c, r, depth, axis, rid, verts=40, r2=None, bevel=0.0):
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=verts,
                          radius1=r, radius2=r if r2 is None else r2, depth=depth)
    ob = mesh_ob(name, bm)
    ob.location = c
    if axis == "X":
        ob.rotation_euler = (0, math.radians(90), 0)
    elif axis == "Y":
        ob.rotation_euler = (math.radians(90), 0, 0)
    if bevel:
        md = ob.modifiers.new("bev", "BEVEL")
        md.width = bevel
        md.segments = 4
        md.limit_method = "ANGLE"
        md.angle_limit = math.radians(40)
    smooth(ob)
    return tag(ob, rid)


def ball(name, c, r, rid, scale=(1, 1, 1), sub=3, noise=0.0, seed=1):
    bm = bmesh.new()
    bmesh.ops.create_icosphere(bm, subdivisions=sub, radius=r)
    if noise:
        rng = np.random.default_rng(seed)
        ph = rng.uniform(0, 6.28, 6)
        for v in bm.verts:
            n = v.co.normalized()
            k = (math.sin(3.1 * n.x + ph[0]) * math.sin(2.7 * n.y + ph[1]) +
                 0.6 * math.sin(5.3 * n.z + ph[2]) * math.sin(4.1 * n.x + ph[3]))
            v.co += n * (k * noise * r)
    ob = mesh_ob(name, bm)
    ob.location = c
    ob.scale = scale
    smooth(ob)
    return tag(ob, rid)


def parent_all(objs, root_name):
    root = link(bpy.data.objects.new(root_name, None))
    for ob in objs:
        ob.parent = root
    return root


# ---------------------------------------------------------------------------
# The six tanks. Origin: on the ground under the turret's pivot; +X forward.
# Each is a hull, a turret body and a barrel (with its mantlet) that pitches
# about its pivot. TANKS at the end has what the app must know too
# (tq_tanks.c, TQ_TANK: the barrel's pivot and muzzle, the gunner's eye).

def tracks(objs, y, length, width, height, wheels, wheel_r, x0=0.06):
    for s in (-1, 1):
        objs.append(rbox("track%d" % s, (x0, s * y, height / 2 + 0.01), (length, width, height), min(height / 2 - 0.03, 0.36), 3, seg=6))
        n = int(length / 0.23)
        for i in range(n):
            x = x0 - length / 2 + 0.2 + i * (length - 0.4) / max(1, n - 1)
            objs.append(rbox("tread%d_%d" % (s, i), (x, s * y, height + 0.02), (0.10, width - 0.02, 0.06), 0.025, 3, seg=2))
        for i in range(wheels):
            x = x0 - length / 2 + wheel_r + 0.12 + i * (length - 2 * wheel_r - 0.24) / max(1, wheels - 1)
            objs.append(cyl("wheel%d_%d" % (s, i), (x, s * (y + width / 2 + 0.01), height / 2 + 0.01), wheel_r, 0.08, "Y", 4, bevel=0.03))
            objs.append(cyl("hub%d_%d" % (s, i), (x, s * (y + width / 2 + 0.06), height / 2 + 0.01), wheel_r * 0.43, 0.05, "Y", 2, bevel=0.02))


def wedge(front, back):
    def f(v):
        if v.co.z > 0 and v.co.x > 0:
            v.co.x -= front
        if v.co.z > 0 and v.co.x < 0:
            v.co.x += back
    return f


def badges(objs, x, y, z, r):
    for s in (-1, 1):
        objs.append(cyl("badge%d" % s, (x, s * y, z), r, 0.03, "Y", 6, bevel=0.01))


def lamps(objs, x, y, z, r=0.10):
    for s in (-1, 1):
        objs.append(ball("lamp%d" % s, (x, s * y, z), r, 5))


# -- Bombo: the all-rounder (the sample's tank)

def hull_bombo():
    o = [rbox("body", (0.08, 0, 0.76), (2.50, 1.46, 0.70), 0.28, 1, shape=wedge(0.34, 0.14)),
         rbox("band", (0.10, 0, 0.62), (2.58, 1.54, 0.16), 0.08, 2, seg=3),
         rbox("rearbox", (-1.08, 0, 0.98), (0.28, 0.70, 0.28), 0.08, 2, seg=3),
         cyl("exhaust", (-1.12, -0.48, 0.80), 0.08, 0.26, "X", 4, bevel=0.02)]
    tracks(o, 0.95, 2.86, 0.56, 0.80, 4, 0.23)
    lamps(o, 1.10, 0.50, 0.98, 0.11)
    return o


def turret_bombo():
    o = [rbox("turret", (-0.10, 0, 1.40), (1.50, 1.24, 0.66), 0.31, 1, seg=6),
         rbox("ring", (-0.10, 0, 1.10), (1.40, 1.16, 0.12), 0.05, 2, seg=3),
         cyl("hatch", (-0.34, 0.22, 1.74), 0.27, 0.09, "Z", 2, bevel=0.035),
         ball("knob", (-0.34, 0.22, 1.81), 0.06, 4)]
    badges(o, -0.14, 0.625, 1.40, 0.24)
    return o


def barrel_bombo():
    return [rbox("mantlet", (0.66, 0, 1.38), (0.36, 0.60, 0.42), 0.14, 2, seg=4),
            cyl("barrel", (1.25, 0, 1.38), 0.13, 1.10, "X", 1),
            cyl("muzzle", (1.86, 0, 1.38), 0.18, 0.30, "X", 4, bevel=0.05),
            cyl("muzzle_in", (2.02, 0, 1.38), 0.085, 0.03, "X", 3)]


# -- Pulga: light and quick, a dome and a thin gun

def hull_pulga():
    o = [rbox("body", (0.05, 0, 0.62), (2.10, 1.28, 0.54), 0.25, 1, shape=wedge(0.30, 0.18)),
         rbox("band", (0.06, 0, 0.50), (2.16, 1.34, 0.12), 0.06, 2, seg=3),
         rbox("fin", (-0.98, 0, 0.98), (0.22, 1.00, 0.10), 0.05, 2, seg=3),
         rbox("finpost", (-0.98, 0, 0.86), (0.12, 0.12, 0.22), 0.04, 4, seg=2)]
    tracks(o, 0.78, 2.30, 0.42, 0.62, 3, 0.19, x0=0.02)
    lamps(o, 0.96, 0.42, 0.80, 0.09)
    return o


def turret_pulga():
    o = [ball("dome", (-0.05, 0, 1.02), 0.5, 1, scale=(1.08, 0.96, 0.66)),
         rbox("ring", (-0.05, 0, 0.90), (0.98, 0.86, 0.10), 0.04, 2, seg=3),
         cyl("hatch", (-0.22, 0.14, 1.32), 0.17, 0.06, "Z", 2, bevel=0.025),
         cyl("antenna", (-0.36, -0.26, 1.55), 0.03, 0.70, "Z", 4),
         ball("tip", (-0.36, -0.26, 1.92), 0.075, 5)]
    badges(o, -0.05, 0.465, 1.03, 0.18)
    return o


def barrel_pulga():
    return [ball("mantlet", (0.40, 0, 1.02), 0.17, 2),
            cyl("barrel", (0.90, 0, 1.02), 0.075, 0.95, "X", 1),
            cyl("muzzle", (1.40, 0, 1.02), 0.11, 0.18, "X", 4, bevel=0.03)]


# -- Mamut: heavy, armour skirts and twin guns

def hull_mamut():
    o = [rbox("body", (0.05, 0, 0.88), (2.90, 1.80, 0.80), 0.33, 1, shape=wedge(0.36, 0.14)),
         rbox("band", (0.06, 0, 0.70), (2.98, 1.88, 0.16), 0.08, 2, seg=3),
         rbox("rearbox_a", (-1.30, 0.40, 1.16), (0.30, 0.55, 0.30), 0.09, 2, seg=3),
         rbox("rearbox_b", (-1.30, -0.40, 1.16), (0.30, 0.55, 0.30), 0.09, 2, seg=3)]
    for s in (-1, 1):
        o.append(rbox("skirt%d" % s, (0.06, s * 1.43, 0.70), (3.10, 0.14, 0.40), 0.07, 2, seg=3))
    tracks(o, 1.12, 3.20, 0.62, 0.90, 5, 0.22)
    lamps(o, 1.35, 0.62, 1.06, 0.12)
    return o


def turret_mamut():
    o = [rbox("turret", (-0.15, 0, 1.62), (1.90, 1.56, 0.78), 0.34, 1, seg=6),
         rbox("ring", (-0.15, 0, 1.26), (1.80, 1.48, 0.14), 0.05, 2, seg=3)]
    for s in (-1, 1):
        o.append(cyl("hatch%d" % s, (-0.55, s * 0.36, 2.03), 0.25, 0.09, "Z", 2, bevel=0.035))
    o.append(ball("knob", (-0.55, 0.36, 2.10), 0.06, 4))
    badges(o, -0.22, 0.785, 1.62, 0.27)
    return o


def barrel_mamut():
    o = [rbox("mantlet", (0.86, 0, 1.62), (0.40, 0.96, 0.50), 0.16, 2, seg=4)]
    for s in (-1, 1):
        o.append(cyl("barrel%d" % s, (1.55, s * 0.25, 1.62), 0.12, 1.30, "X", 1))
        o.append(cyl("muzzle%d" % s, (2.25, s * 0.25, 1.62), 0.17, 0.28, "X", 4, bevel=0.05))
    return o


# -- Obus: artillery, a long deck and a fat mortar tube

def hull_obus():
    o = [rbox("body", (0.42, 0, 0.72), (3.00, 1.50, 0.60), 0.27, 1, shape=wedge(0.30, 0.10)),
         rbox("band", (0.44, 0, 0.58), (3.08, 1.58, 0.14), 0.07, 2, seg=3)]
    for s in (-1, 1):
        o.append(rbox("crate%d" % s, (1.25, s * 0.38, 1.12), (0.46, 0.40, 0.30), 0.08, 2, seg=3))
        o.append(rbox("leg%d" % s, (-1.10, s * 0.62, 0.42), (0.20, 0.18, 0.62), 0.06, 4, seg=2))
    tracks(o, 0.95, 3.20, 0.54, 0.76, 5, 0.21, x0=0.42)
    lamps(o, 1.85, 0.50, 0.92, 0.10)
    return o


def turret_obus():
    o = [rbox("casemate", (-0.10, 0, 1.30), (1.40, 1.36, 0.64), 0.29, 1, seg=6),
         rbox("ring", (-0.10, 0, 1.03), (1.32, 1.28, 0.12), 0.05, 2, seg=3)]
    badges(o, -0.25, 0.685, 1.30, 0.24)
    return o


def barrel_obus():
    return [ball("breech", (0.32, 0, 1.48), 0.32, 2),
            cyl("tube", (0.92, 0, 1.48), 0.20, 1.20, "X", 1),
            cyl("muzzle", (1.52, 0, 1.48), 0.26, 0.22, "X", 4, bevel=0.06),
            cyl("muzzle_in", (1.64, 0, 1.48), 0.15, 0.03, "X", 3)]


# -- Avispa: a missile pod on a mast

def hull_avispa():
    o = [rbox("body", (0.05, 0, 0.68), (2.36, 1.42, 0.58), 0.26, 1, shape=wedge(0.34, 0.12)),
         rbox("band", (0.06, 0, 0.56), (2.42, 1.48, 0.10), 0.05, 2, seg=3),
         rbox("stripe", (0.30, 0, 0.98), (0.90, 0.30, 0.04), 0.02, 2, seg=2)]
    tracks(o, 0.88, 2.56, 0.48, 0.68, 4, 0.20)
    lamps(o, 1.05, 0.46, 0.86, 0.10)
    return o


def turret_avispa():
    o = [cyl("base", (-0.05, 0, 1.06), 0.56, 0.20, "Z", 2, bevel=0.06),
         rbox("mast", (-0.10, 0, 1.28), (0.52, 0.62, 0.36), 0.13, 1, seg=4)]
    badges(o, -0.10, 0.315, 1.28, 0.15)
    return o


def barrel_avispa():
    o = [rbox("pod", (0.28, 0, 1.46), (1.10, 1.00, 0.62), 0.23, 1, seg=5),
         rbox("podband", (0.10, 0, 1.46), (0.20, 1.04, 0.66), 0.08, 2, seg=3)]
    for sy in (-1, 1):
        for sz in (-1, 1):
            y, z = sy * 0.22, 1.46 + sz * 0.15
            o.append(cyl("rim%d%d" % (sy, sz), (0.83, y, z), 0.12, 0.06, "X", 4, bevel=0.02))
            o.append(ball("nose%d%d" % (sy, sz), (0.85, y, z), 0.085, 2))
    return o


# -- Topo: the engineer, a dozer blade and a crane

def hull_topo():
    def tilt(v):
        if v.co.z > 0:
            v.co.x += 0.12
    o = [rbox("body", (0.00, 0, 0.70), (2.30, 1.46, 0.62), 0.27, 1, shape=wedge(0.28, 0.12)),
         rbox("band", (0.02, 0, 0.56), (2.36, 1.52, 0.12), 0.06, 2, seg=3),
         rbox("blade", (1.48, 0, 0.42), (0.16, 2.15, 0.64), 0.07, 4, seg=3, shape=tilt)]
    for s in (-1, 1):
        o.append(rbox("arm%d" % s, (1.12, s * 0.62, 0.50), (0.62, 0.12, 0.12), 0.04, 2, seg=2))
    tracks(o, 0.90, 2.50, 0.50, 0.70, 4, 0.20, x0=0.0)
    return o


def turret_topo():
    o = [rbox("cabin", (-0.20, 0, 1.28), (1.10, 1.06, 0.64), 0.27, 1, seg=6),
         rbox("ring", (-0.20, 0, 1.01), (1.04, 1.00, 0.10), 0.04, 2, seg=3),
         cyl("beacon", (-0.38, 0.26, 1.68), 0.10, 0.14, "Z", 5, bevel=0.03),
         rbox("crane_v", (-0.62, -0.30, 1.92), (0.14, 0.14, 0.80), 0.05, 2, seg=2),
         rbox("crane_h", (-0.30, -0.30, 2.30), (0.78, 0.13, 0.13), 0.05, 2, seg=2),
         ball("hook", (0.05, -0.30, 2.12), 0.08, 4)]
    badges(o, -0.20, 0.535, 1.28, 0.20)
    return o


def barrel_topo():
    return [ball("mantlet", (0.40, 0, 1.30), 0.21, 2),
            cyl("barrel", (0.78, 0, 1.30), 0.11, 0.70, "X", 1),
            cyl("muzzle", (1.16, 0, 1.30), 0.15, 0.20, "X", 4, bevel=0.04)]


# -- Tortuga (Misión a la Luna): a shell of plates on a low body, a coil gun

def hull_tortuga():
    o = [rbox("body", (0.0, 0, 0.62), (2.40, 1.56, 0.46), 0.22, 2, shape=wedge(0.20, 0.20)),
         cyl("rim", (-0.05, 0, 0.88), 1.18, 0.16, "Z", 1, verts=48, bevel=0.06),
         ball("head", (1.30, 0, 0.84), 0.36, 2, scale=(1.15, 1.0, 0.85)),
         ball("visor", (1.52, 0, 0.90), 0.20, 5, scale=(0.7, 1.35, 0.6))]
    for s_ in (-1, 1):
        o.append(rbox("pod%d" % s_, (0.0, s_ * 0.88, 0.36), (2.10, 0.48, 0.56), 0.22, 3, seg=5))
        for i in range(3):
            x = -0.70 + i * 0.70
            o.append(cyl("foot%d_%d" % (s_, i), (x, s_ * 1.13, 0.30), 0.22, 0.10, "Y", 4, bevel=0.03))
        o.append(cyl("tail%d" % s_, (-1.24, s_ * 0.26, 0.70), 0.08, 0.30, "X", 4, bevel=0.02))
    return o


def turret_tortuga():
    o = [ball("shell", (-0.10, 0, 1.10), 0.78, 1, scale=(1.25, 1.05, 0.62), sub=4),
         cyl("ring", (-0.10, 0, 1.02), 0.92, 0.10, "Z", 2, verts=48, bevel=0.03)]
    # the shell's plates
    for i, (x, y) in enumerate(((-0.10, 0.0), (0.42, 0.30), (0.42, -0.30), (-0.62, 0.30), (-0.62, -0.30),
                                (-0.10, 0.56), (-0.10, -0.56))):
        z = 1.10 + 0.40 * (1 - (x * x / 1.0 + y * y / 0.75)) ** 0.5
        o.append(ball("plate%d" % i, (x, y, z), 0.26, 2, scale=(1.0, 1.0, 0.32)))
    o.append(cyl("mast", (-0.55, -0.20, 1.80), 0.035, 0.80, "Z", 4))
    o.append(ball("mast_tip", (-0.55, -0.20, 2.22), 0.08, 5))
    badges(o, -0.10, 0.80, 1.12, 0.20)
    return o


def barrel_tortuga():
    o = [ball("mantlet", (0.55, 0, 1.30), 0.26, 2),
         cyl("barrel", (1.10, 0, 1.30), 0.10, 1.00, "X", 4)]
    for i in range(3):
        o.append(cyl("coil%d" % i, (0.88 + i * 0.22, 0, 1.30), 0.17, 0.07, "X", 1, bevel=0.02))
    o.append(ball("orb", (1.66, 0, 1.30), 0.15, 5))
    return o


# -- Anfibio (Día de playa): a boat on wheels, a propeller, twin guns

def hull_anfibio():
    def bow(v):
        if v.co.x > 0:
            v.co.y *= 0.55 if v.co.z < 0 else 0.85
            v.co.x += 0.25 if v.co.z > 0 else -0.05
        if v.co.z < 0:
            v.co.y *= 0.85
    o = [rbox("body", (0.05, 0, 0.82), (2.60, 1.50, 0.64), 0.26, 1, shape=bow),
         rbox("stripe", (0.0, 0, 0.62), (2.50, 1.38, 0.14), 0.06, 2, seg=3, shape=bow),
         cyl("prop_hub", (-1.36, 0, 0.62), 0.10, 0.20, "X", 4, bevel=0.03)]
    for k in range(3):
        b = rbox("blade%d" % k, (-1.44, 0, 0.62), (0.04, 0.10, 0.52), 0.02, 4, seg=2)
        b.rotation_euler = (math.radians(k * 120), 0, 0)
        o.append(b)
    for s_ in (-1, 1):
        for i in range(3):
            x = -0.80 + i * 0.80
            o.append(cyl("wheel%d_%d" % (s_, i), (x, s_ * 0.72, 0.34), 0.32, 0.26, "Y", 3, bevel=0.06))
            o.append(cyl("hub%d_%d" % (s_, i), (x, s_ * 0.86, 0.34), 0.13, 0.04, "Y", 4, bevel=0.02))
        o.append(cyl("buoy%d" % s_, (-0.55, s_ * 0.78, 1.00), 0.20, 0.10, "Y", 2, bevel=0.04))
    lamps(o, 1.20, 0.38, 1.02, 0.09)
    return o


def turret_anfibio():
    o = [rbox("cabin", (-0.20, 0, 1.34), (1.20, 1.04, 0.52), 0.24, 1, seg=6),
         rbox("ring", (-0.20, 0, 1.10), (1.14, 0.98, 0.10), 0.04, 2, seg=3),
         rbox("glass", (0.30, 0, 1.44), (0.26, 0.86, 0.24), 0.08, 4, seg=3),
         cyl("horn", (-0.55, 0.28, 1.72), 0.07, 0.22, "Z", 2, bevel=0.02),
         ball("flag_pole", (-0.62, -0.30, 1.70), 0.05, 4)]
    badges(o, -0.25, 0.525, 1.34, 0.19)
    return o


def barrel_anfibio():
    o = [rbox("mantlet", (0.45, 0, 1.25), (0.30, 0.66, 0.34), 0.12, 2, seg=4)]
    for s_ in (-1, 1):
        o.append(cyl("barrel%d" % s_, (1.00, s_ * 0.17, 1.25), 0.075, 1.00, "X", 1))
        o.append(cyl("muzzle%d" % s_, (1.52, s_ * 0.17, 1.25), 0.11, 0.18, "X", 4, bevel=0.03))
    return o


# name: (hull, turret, barrel, barrel pivot, the sprites' barrel pitch,
#        the gunner's eye, the muzzle's distance from the pivot)
TANKS = {
    "bombo":  (hull_bombo,  turret_bombo,  barrel_bombo,  V((0.64, 0, 1.38)), 8,  V((-0.75, 0.52, 2.42)), 1.40),
    "pulga":  (hull_pulga,  turret_pulga,  barrel_pulga,  V((0.40, 0, 1.02)), 8,  V((-0.80, 0.40, 1.92)), 1.10),
    "mamut":  (hull_mamut,  turret_mamut,  barrel_mamut,  V((0.82, 0, 1.62)), 6,  V((-1.20, 0.72, 2.80)), 1.55),
    "obus":   (hull_obus,   turret_obus,   barrel_obus,   V((0.32, 0, 1.48)), 32, V((-1.10, 0.62, 2.40)), 1.30),
    "avispa": (hull_avispa, turret_avispa, barrel_avispa, V((0.05, 0, 1.46)), 6,  V((-1.55, 0.78, 2.60)), 0.85),
    "topo":   (hull_topo,   turret_topo,   barrel_topo,   V((0.40, 0, 1.30)), 8,  V((-0.95, 0.50, 2.20)), 0.86),
    # the DLCs': Misión a la Luna, Día de playa
    "tortuga": (hull_tortuga, turret_tortuga, barrel_tortuga, V((0.55, 0, 1.30)), 8, V((-0.95, 0.55, 2.30)), 1.26),
    "anfibio": (hull_anfibio, turret_anfibio, barrel_anfibio, V((0.45, 0, 1.25)), 8, V((-0.90, 0.48, 2.22)), 1.16),
}


def build_barrel(kind, pitch_deg):
    t = TANKS[kind]
    parts = t[2]()
    root = link(bpy.data.objects.new("barrel_pivot", None))
    root.location = t[3]
    for ob in parts:
        ob.parent = root
        ob.matrix_parent_inverse = Matrix.Translation(-t[3])
    root.rotation_euler = (0, -math.radians(pitch_deg), 0)
    return parts + [root]


def build_tank(kind, pitch_deg=None, hull=True, turret=True, barrel=True):
    t = TANKS[kind]
    o = []
    if hull:
        o += t[0]()
    if turret:
        o += t[1]()
    if barrel:
        o += build_barrel(kind, t[4] if pitch_deg is None else pitch_deg)
    return o


# ---------------------------------------------------------------------------
# Scenery

def build_tree_round(seed=3):
    objs = [cyl("trunk", (0, 0, 0.85), 0.24, 1.7, "Z", 9, r2=0.15, bevel=0.04)]
    rng = np.random.default_rng(seed)
    blobs = [((0.0, 0.0, 2.55), 1.05), ((0.55, 0.25, 2.15), 0.78), ((-0.5, -0.3, 2.2), 0.80),
             ((-0.15, 0.5, 2.05), 0.70), ((0.2, -0.45, 3.05), 0.66)]
    for i, (c, r) in enumerate(blobs):
        objs.append(ball("leaf%d" % i, c, r, 8, noise=0.06, seed=seed * 10 + i))
    return objs


def build_tree_pine(seed=5):
    objs = [cyl("trunk", (0, 0, 0.6), 0.2, 1.2, "Z", 9, r2=0.14, bevel=0.03)]
    for i, (z, r, h) in enumerate(((1.15, 1.15, 1.25), (1.95, 0.92, 1.1), (2.7, 0.66, 1.0), (3.35, 0.38, 0.8))):
        ob = cyl("tier%d" % i, (0, 0, z + h / 2), r, h, "Z", 8, r2=0.08, verts=28, bevel=0.10)
        md = ob.modifiers.new("sub", "SUBSURF")
        md.levels = md.render_levels = 1
        objs.append(ob)
    return objs


def build_stump(seed=7):
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, segments=24, radius1=0.26, radius2=0.2, depth=0.6)
    rng = np.random.default_rng(seed)
    for v in bm.verts:
        if v.co.z > 0:
            v.co.z += float(rng.uniform(-0.10, 0.14))
    ob = mesh_ob("stump", bm)
    ob.location = (0, 0, 0.3)
    smooth(ob)
    tag(ob, 9)
    top = cyl("cut", (0, 0, 0.60), 0.17, 0.05, "Z", 11)
    return [ob, top]


def build_block(seed=1):
    rng = np.random.default_rng(seed)
    s = [float(x) for x in rng.uniform(0.92, 0.98, 3)]

    def jitter(v):
        v.co += V(tuple(float(x) for x in rng.uniform(-0.025, 0.025, 3)))
    ob = rbox("block", (0, 0, s[2] / 2), (s[0], s[1], s[2]), 0.13, 10, seg=4, shape=jitter)
    return [ob]


def jagged_cyl(name, c, r, h, rid, seed, amount=0.12, r2=None):
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, segments=24, radius1=r, radius2=r if r2 is None else r2, depth=h)
    rng = np.random.default_rng(seed)
    for v in bm.verts:
        if v.co.z > 0:
            v.co.z += float(rng.uniform(-amount, amount))
    ob = mesh_ob(name, bm)
    ob.location = c
    smooth(ob)
    return tag(ob, rid)


def cactus_arm(o, s, x, z0, up):
    o.append(cyl("armh%d" % s, (s * (0.32 + x / 2), 0, z0), 0.2, x, "X", 8, bevel=0.06))
    o.append(cyl("armv%d" % s, (s * (0.32 + x), 0, z0 + up / 2), 0.2, up, "Z", 8, bevel=0.06))
    o.append(ball("armtop%d" % s, (s * (0.32 + x), 0, z0 + up), 0.2, 8))


def build_saguaro(seed=1):
    o = [cyl("col", (0, 0, 1.3), 0.33, 2.6, "Z", 8, bevel=0.08), ball("top", (0, 0, 2.6), 0.33, 8)]
    cactus_arm(o, 1, 0.35, 1.25, 0.8)
    cactus_arm(o, -1, 0.3, 1.6, 0.6)
    for i, (x, z) in enumerate(((0.12, 2.9), (-0.15, 2.85))):
        o.append(ball("flower%d" % i, (x, 0.05, z), 0.09, 5))
    return o


def build_barrel_cactus(seed=2):
    o = []
    for i, (x, y, r) in enumerate(((0, 0, 0.55), (0.62, 0.25, 0.40), (-0.5, 0.35, 0.34))):
        o.append(ball("barrel%d" % i, (x, y, r * 0.85), r, 8, scale=(1, 1, 1.15), noise=0.05, seed=seed + i))
        o.append(ball("bloom%d" % i, (x, y, r * 1.85), r * 0.22, 5))
    return o


def build_cactus_stump(seed=3):
    return [jagged_cyl("stub", (0, 0, 0.35), 0.33, 0.7, 8, seed), cyl("cut", (0, 0, 0.68), 0.24, 0.04, "Z", 11)]


def build_snow_pine(seed=5):
    o = [cyl("trunk", (0, 0, 0.6), 0.2, 1.2, "Z", 9, r2=0.14, bevel=0.03)]
    for i, (z, r, h) in enumerate(((1.15, 1.15, 1.25), (1.95, 0.92, 1.1), (2.7, 0.66, 1.0), (3.35, 0.38, 0.8))):
        ob = cyl("tier%d" % i, (0, 0, z + h / 2), r, h, "Z", 8, r2=0.08, verts=28, bevel=0.10)
        md = ob.modifiers.new("sub", "SUBSURF")
        md.levels = md.render_levels = 1
        o.append(ob)
        cap = cyl("snow%d" % i, (0, 0, z + h * 0.62), r * 0.80, h * 0.50, "Z", 7, r2=0.1, verts=28, bevel=0.08)
        md = cap.modifiers.new("sub", "SUBSURF")
        md.levels = md.render_levels = 1
        o.append(cap)
    return o


def build_snowman(seed=6):
    o = [ball("b0", (0, 0, 0.55), 0.62, 7), ball("b1", (0, 0, 1.4), 0.45, 7), ball("b2", (0, 0, 2.05), 0.33, 7),
         cyl("nose", (0.42, 0, 2.05), 0.07, 0.3, "X", 12, r2=0.01),
         ball("eye0", (0.27, 0.12, 2.15), 0.05, 3), ball("eye1", (0.27, -0.12, 2.15), 0.05, 3),
         rbox("scarf", (0, 0, 1.75), (0.72, 0.72, 0.14), 0.07, 11, seg=3)]
    for i in range(3):
        o.append(ball("btn%d" % i, (0.43, 0, 1.15 + i * 0.22), 0.05, 3))
    return o


def build_snow_stump(seed=7):
    o = build_stump(seed)
    o.append(ball("snowcap", (0, 0, 0.62), 0.27, 7, scale=(1, 1, 0.45)))
    return o


def build_column(seed=8):
    return [rbox("base", (0, 0, 0.2), (0.95, 0.95, 0.4), 0.08, 10, seg=3),
            jagged_cyl("shaft", (0, 0, 1.35), 0.34, 1.9, 10, seed, amount=0.25),
            cyl("ring", (0, 0, 0.5), 0.40, 0.12, "Z", 11, bevel=0.03)]


def build_lamp(seed=9):
    return [cyl("post", (0, 0, 1.5), 0.11, 3.0, "Z", 4, bevel=0.03),
            cyl("foot", (0, 0, 0.15), 0.25, 0.3, "Z", 4, bevel=0.06),
            rbox("head", (0.25, 0, 3.0), (0.65, 0.32, 0.18), 0.07, 4, seg=3),
            ball("bulb", (0.42, 0, 2.86), 0.13, 5)]


def build_rubble(seed=10):
    rng = np.random.default_rng(seed)
    o = []
    for i in range(6):
        c = (float(rng.uniform(-0.45, 0.45)), float(rng.uniform(-0.45, 0.45)), float(rng.uniform(0.1, 0.35)))
        sz = tuple(float(x) for x in rng.uniform(0.25, 0.5, 3))
        o.append(rbox("rub%d" % i, c, sz, 0.06, 10 if i % 3 else 11, seg=2))
    return o


def build_brick(seed=11):
    o = build_block(seed)
    for k in range(3):
        o.append(rbox("mortar%d" % k, (0, 0, 0.24 + k * 0.24), (0.99, 0.99, 0.035), 0.012, 11, seg=1))
    return o


def build_dead_tree(seed=12):
    o = [cyl("trunk", (0, 0, 1.2), 0.26, 2.4, "Z", 9, r2=0.12, bevel=0.04)]
    for i, (ang, z, l, tilt) in enumerate(((20, 1.7, 1.1, 45), (150, 2.0, 0.9, 50), (260, 1.4, 0.8, 40))):
        ob = cyl("branch%d" % i, (0, 0, 0), 0.09, l, "Z", 9, r2=0.04)
        a, t = math.radians(ang), math.radians(tilt)
        ob.rotation_euler = (0, t, a)
        ob.location = (math.cos(a) * math.sin(t) * l / 2, math.sin(a) * math.sin(t) * l / 2, z + math.cos(t) * l / 2)
        o.append(ob)
    return o


def build_lava_rock(seed=13):
    o = [ball("rock", (0, 0, 1.1), 0.75, 10, scale=(1, 0.9, 1.6), noise=0.12, seed=seed)]
    rng = np.random.default_rng(seed)
    for i in range(5):
        a = float(rng.uniform(0, 6.28))
        z = float(rng.uniform(0.5, 1.9))
        o.append(ball("ember%d" % i, (math.cos(a) * 0.62, math.sin(a) * 0.55, z), 0.12, 12))
    return o


def build_char_stump(seed=14):
    o = build_stump(seed)
    o[0]["rid"] = 9
    o.append(ball("glow", (0.05, 0.02, 0.64), 0.1, 12))
    return o


# -- the Moon: a dish on a mast, crystals, boulders and crates

def build_dish(seed=15):
    o = [cyl("foot", (0, 0, 0.18), 0.45, 0.36, "Z", 4, bevel=0.06),
         cyl("mast", (0, 0, 1.20), 0.10, 1.80, "Z", 4),
         cyl("joint", (0, 0, 2.12), 0.18, 0.20, "Y", 3, bevel=0.04)]
    d = cyl("dish", (0.18, 0, 2.55), 0.95, 0.30, "Z", 11, verts=40, r2=0.30, bevel=0.06)
    d.rotation_euler = (0, math.radians(-55), 0)
    o.append(d)
    o.append(cyl("feed", (0.62, 0, 2.85), 0.04, 0.70, "X", 4))
    o.append(ball("lamp", (0.98, 0, 2.86), 0.09, 5))
    return o


def build_crystals(seed=16):
    rng = np.random.default_rng(seed)
    o = [ball("bed", (0, 0, 0.20), 0.70, 10, scale=(1, 1, 0.45), noise=0.15, seed=seed)]
    for i in range(6):
        a = float(rng.uniform(0, 6.28))
        r = float(rng.uniform(0.0, 0.45))
        h = float(rng.uniform(1.0, 2.4)) * (1.3 if i == 0 else 1.0)
        c = cyl("cr%d" % i, (0, 0, 0), float(rng.uniform(0.16, 0.26)), h, "Z", 8, verts=6, r2=0.02)
        t = float(rng.uniform(5, 28)) * (0 if i == 0 else 1)
        c.rotation_euler = (0, math.radians(t), a)
        c.location = (math.cos(a) * (r + math.sin(math.radians(t)) * h / 2),
                      math.sin(a) * (r + math.sin(math.radians(t)) * h / 2), 0.25 + math.cos(math.radians(t)) * h / 2)
        o.append(c)
    return o


def build_moon_stump(seed=17):
    return [cyl("foot", (0, 0, 0.18), 0.45, 0.36, "Z", 4, bevel=0.06),
            jagged_cyl("stub", (0, 0, 0.55), 0.10, 0.55, 4, seed, amount=0.15),
            ball("debris", (0.30, 0.20, 0.10), 0.14, 11, scale=(1.4, 1, 0.5))]


def build_moon_rock(seed=18):
    return [ball("rock", (0, 0, 0.46), 0.56, 10, scale=(1.0, 0.95, 0.85), noise=0.18, seed=seed)]


# -- the beach: palms, an umbrella, crates and rocks

def build_palm(seed=19, lean=14.0):
    rng = np.random.default_rng(seed)
    o = []
    n = 7
    x = z = 0.0
    a = math.radians(lean)
    for i in range(n):
        h = 0.48
        seg = cyl("seg%d" % i, (0, 0, 0), 0.19 - i * 0.012, h, "Z", 9, r2=0.15 - i * 0.012, bevel=0.03)
        t = a * (i / n) * 1.6
        seg.rotation_euler = (0, t, 0)
        seg.location = (x + math.sin(t) * h / 2, 0, z + math.cos(t) * h / 2)
        x += math.sin(t) * h
        z += math.cos(t) * h * 0.97
        o.append(seg)
    top = V((x, 0, z))
    for k in range(7):
        ang = k * 2 * math.pi / 7 + float(rng.uniform(-0.2, 0.2))
        f = ball("frond%d" % k, (0, 0, 0), 0.5, 8, scale=(1.9, 0.42, 0.10))
        f.rotation_euler = (0, math.radians(22), ang)
        f.location = (top.x + math.cos(ang) * 0.78, top.y + math.sin(ang) * 0.78, top.z - 0.12)
        o.append(f)
    for k in range(3):
        ang = k * 2.1
        o.append(ball("nut%d" % k, (top.x + math.cos(ang) * 0.18, math.sin(ang) * 0.18, top.z - 0.22), 0.12, 9))
    return o


def build_umbrella(seed=20):
    o = [cyl("pole", (0, 0, 1.25), 0.05, 2.50, "Z", 4),
         cyl("foot", (0, 0, 0.06), 0.30, 0.12, "Z", 4, bevel=0.03),
         cyl("canopy", (0, 0, 2.40), 1.30, 0.50, "Z", 2, verts=16, r2=0.06, bevel=0.04),
         cyl("rim", (0, 0, 2.15), 1.32, 0.08, "Z", 7, verts=16, bevel=0.02),
         ball("top", (0, 0, 2.68), 0.09, 7)]
    return o


def build_palm_stump(seed=21):
    return [jagged_cyl("stub", (0, 0, 0.32), 0.19, 0.64, 9, seed, amount=0.14),
            cyl("cut", (0, 0, 0.62), 0.14, 0.04, "Z", 11),
            ball("frond", (0.35, 0.1, 0.06), 0.4, 8, scale=(1.4, 0.4, 0.12))]


def build_crate(seed=22):
    o = [rbox("crate", (0, 0, 0.47), (0.92, 0.92, 0.92), 0.06, 9, seg=3)]
    for k in range(2):
        o.append(rbox("slat%d" % k, (0, 0, 0.22 + k * 0.50), (0.96, 0.96, 0.10), 0.03, 11, seg=2))
    return o


# biome: (tree a, tree b, what a broken tree leaves, block, block seed)
BIOMES = {
    "pradera":  (build_tree_round, build_tree_pine, build_stump, build_block),
    "desierto": (build_saguaro, build_barrel_cactus, build_cactus_stump, build_block),
    "nieve":    (build_snow_pine, build_snowman, build_snow_stump, build_block),
    "ruinas":   (build_column, build_lamp, build_rubble, build_brick),
    "volcan":   (build_dead_tree, build_lava_rock, build_char_stump, build_block),
    "luna":     (build_dish, build_crystals, build_moon_stump, build_moon_rock),
    "playa":    (build_palm, build_umbrella, build_palm_stump, build_crate),
}


# -- the modes' marks: a team's flag (painted with the team's colour, region
# 1), a chest, the hill's beacon

def build_flag(seed=23):
    o = [cyl("base", (0, 0, 0.10), 0.36, 0.20, "Z", 3, bevel=0.05),
         cyl("pole", (0, 0, 1.40), 0.05, 2.60, "Z", 4),
         ball("knob", (0, 0, 2.74), 0.08, 5)]
    cloth = rbox("cloth", (0.62, 0, 2.30), (1.10, 0.05, 0.70), 0.02, 1, seg=2)
    o.append(cloth)
    o.append(rbox("stripe", (0.62, 0, 2.30), (1.12, 0.07, 0.16), 0.02, 2, seg=2))
    return o


def build_chest(seed=24):
    o = [rbox("box", (0, 0, 0.32), (1.00, 0.70, 0.56), 0.06, 9, seg=3),
         cyl("lid", (0, 0, 0.62), 0.35, 1.00, "X", 9, verts=24, bevel=0.03)]
    for x in (-0.36, 0.36):
        o.append(rbox("band%d" % int(x * 10), (x, 0, 0.45), (0.10, 0.74, 0.62), 0.02, 4, seg=2))
    o.append(rbox("lock", (0, -0.37, 0.56), (0.16, 0.06, 0.20), 0.02, 5, seg=2))
    for k in range(4):
        o.append(cyl("coin%d" % k, (-0.30 + k * 0.2, 0.1, 0.92), 0.10, 0.03, "Z", 5))
    return o


def build_beacon(seed=25):
    o = [cyl("base", (0, 0, 0.12), 0.55, 0.24, "Z", 3, bevel=0.06)]
    for k in range(3):
        a = k * 2 * math.pi / 3
        leg = cyl("leg%d" % k, (0, 0, 0), 0.05, 2.6, "Z", 4)
        leg.rotation_euler = (0, math.radians(8), a + math.pi)
        leg.location = (math.cos(a) * 0.20, math.sin(a) * 0.20, 1.40)
        o.append(leg)
    o.append(cyl("cap", (0, 0, 2.70), 0.22, 0.12, "Z", 4, bevel=0.03))
    o.append(ball("light", (0, 0, 2.95), 0.22, 5))
    return o


MARKS = {"fl": build_flag, "ch": build_chest, "bc": build_beacon}


# ---------------------------------------------------------------------------
# Rendering

def place_camera_ortho(F, R, UP, target, px_per_m, w, h):
    sc = bpy.context.scene
    cam = sc.camera
    cam.data.type = "ORTHO"
    cam.data.ortho_scale = max(w, h) / px_per_m
    cam.data.clip_start = 0.1
    cam.data.clip_end = 100
    m = Matrix((R, UP, -F)).transposed().to_4x4()
    m.translation = V(target) - F * 30.0
    cam.matrix_world = m
    sc.render.resolution_x = w
    sc.render.resolution_y = h
    sc.render.resolution_percentage = 100


def place_camera_persp(eye, F, R, UP, f_px, w, h):
    sc = bpy.context.scene
    cam = sc.camera
    cam.data.type = "PERSP"
    cam.data.sensor_fit = "HORIZONTAL"
    cam.data.sensor_width = 36.0
    cam.data.lens = f_px / w * 36.0
    cam.data.clip_start = 0.05
    cam.data.clip_end = 100
    m = Matrix((R, UP, -F)).transposed().to_4x4()
    m.translation = eye
    cam.matrix_world = m
    sc.render.resolution_x = w
    sc.render.resolution_y = h
    sc.render.resolution_percentage = 100


def render_exr(tag_):
    sc = bpy.context.scene
    path = os.path.join(TMP, tag_ + ".exr")
    sc.render.filepath = path
    bpy.ops.render.render(write_still=True)
    img = bpy.data.images.load(path)
    w, h = img.size
    a = np.empty(w * h * 4, np.float32)
    img.pixels.foreach_get(a)
    bpy.data.images.remove(img)
    os.remove(path)
    return a.reshape(h, w, 4)[::-1]


def srgb(x):
    x = np.clip(x, 0, None)
    return np.where(x <= 0.0031308, 12.92 * x, 1.055 * np.power(x, 1 / 2.4) - 0.055)


EXPOSURE = 0.50        # a top face in full sun lands near L = 200


def render_planes(objs, F, depth_origin):
    """Both passes of the current camera: L, A, ID, Z (uint8 planes)."""
    sc = bpy.context.scene
    cy = sc.cycles
    use_pass(objs, "L")
    cy.samples = ARGS.samples
    cy.use_denoising = True
    try:
        cy.denoiser = "OPENIMAGEDENOISE"
    except Exception:
        pass
    cy.max_bounces = 4
    sc.render.filter_size = 1.5
    sc.world.node_tree.nodes["Background"].inputs[1].default_value = 0.30
    try:
        lit = render_exr("lit")
    except RuntimeError:
        cy.use_denoising = False
        lit = render_exr("lit")
    use_pass(objs, "I")
    set_depth_dir(F)
    cy.samples = 1
    cy.use_denoising = False
    cy.max_bounces = 0
    sc.render.filter_size = 0.01
    sc.world.node_tree.nodes["Background"].inputs[1].default_value = 0.0
    idz = render_exr("idz")
    use_pass(objs, "L")

    alpha = np.clip(lit[..., 3], 0, 1)
    lum = (0.2126 * lit[..., 0] + 0.7152 * lit[..., 1] + 0.0722 * lit[..., 2])
    lum = np.where(alpha > 1e-4, lum / np.maximum(alpha, 1e-4), 0)     # straight, not premultiplied
    L = np.clip(srgb(lum * EXPOSURE) * 255.0 + 0.5, 0, 255).astype(np.uint8)
    A = np.clip(alpha * 255.0 + 0.5, 0, 255).astype(np.uint8)
    have = idz[..., 3] > 0.5
    rid = np.where(have, np.floor(idz[..., 0] * 16.0), 0).astype(np.int32)
    d = idz[..., 1] - float(V(depth_origin).dot(F))
    Z = np.where(have, np.clip(np.round(128 + d / DEPTH_UNIT), 0, 254), 255).astype(np.int32)
    # the antialiased edge reaches pixels whose centre missed the object:
    # they take the nearest covered pixel's id and depth
    UV = np.where(have & (rid == 6), np.floor(idz[..., 2] * 256.0), 0).astype(np.int32) & 255
    rid, Z, UV = grow(rid, Z, have, A > 0, UV)
    A[rid == 0] = 0
    return L, A, rid.astype(np.uint8), Z.astype(np.uint8), UV.astype(np.uint8)


def grow(rid, Z, have, want, UV):
    rid = rid.copy()
    Z = Z.copy()
    UV = UV.copy()
    have = have.copy()
    for _ in range(3):
        need = want & ~have
        if not need.any():
            break
        for dy, dx in ((0, 1), (0, -1), (1, 0), (-1, 0), (1, 1), (-1, -1), (1, -1), (-1, 1)):
            src_have = np.roll(np.roll(have, dy, 0), dx, 1)
            take = need & src_have
            rid[take] = np.roll(np.roll(rid, dy, 0), dx, 1)[take]
            Z[take] = np.roll(np.roll(Z, dy, 0), dx, 1)[take]
            UV[take] = np.roll(np.roll(UV, dy, 0), dx, 1)[take]
            have = have | take
            need = need & ~take
    return rid, Z, UV


def save_spr(set_name, name, planes, ax, ay):
    L, A, ID, Z, UV = planes
    has_uv = bool((ID[A > 0] == 6).any())
    ys, xs = np.nonzero(A)
    if len(xs) == 0:
        print("EMPTY sprite", set_name, name)
        return
    x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
    crop = [p[y0:y1, x0:x1] for p in ((L, A, ID, Z, UV) if has_uv else (L, A, ID, Z))]
    d = os.path.join(OUT, set_name)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name + ".spr"), "wb") as f:
        f.write(b"TSP5" if has_uv else b"TSPR")
        f.write(struct.pack("<HHhh", x1 - x0, y1 - y0, int(round(ax - x0)), int(round(ay - y0))))
        for p in crop:
            f.write(np.ascontiguousarray(p, dtype=np.uint8).tobytes())
    print("sprite %s/%s %dx%d" % (set_name, name, x1 - x0, y1 - y0))


def rotate_all(objs, yaw_deg):
    root = bpy.data.objects.get("_root")
    if root is None:
        root = link(bpy.data.objects.new("_root", None))
    for ob in objs:
        if ob.parent is None and ob is not root:
            ob.parent = root
    root.rotation_euler = (0, 0, math.radians(yaw_deg))
    bpy.context.view_layer.update()


def render_top(objs, name, w, h, lift):
    """The battle's camera, the anchor at the origin."""
    F, R, UP = view_axes(TOP_YAW, TOP_ELEV)
    target = V((0, 0, lift))
    place_camera_ortho(F, R, UP, target, TOP_S, w, h)
    set_sun(SUN_AZ, SUN_ELEV)
    planes = render_planes(objs, F, (0, 0, 0))
    # the anchor's pixel: the image centre, moved by the anchor's offset
    ax = w / 2 + TOP_S * (V((0, 0, 0)) - target).dot(R)
    ay = h / 2 - TOP_S * (V((0, 0, 0)) - target).dot(UP)
    save_spr("top", name, planes, ax, ay)


def render_low(objs, name, w, h, lift):
    F, R, UP = view_axes(0.0, LOW_ELEV)
    target = V((0, 0, lift))
    place_camera_ortho(F, R, UP, target, LOW_S, w, h)
    set_sun(SUN_AZ, SUN_ELEV)
    planes = render_planes(objs, F, (0, 0, 0))
    ax = w / 2 + LOW_S * (V((0, 0, 0)) - target).dot(R)
    ay = h / 2 - LOW_S * (V((0, 0, 0)) - target).dot(UP)
    save_spr("low", name, planes, ax, ay)


def clear_scene_objects():
    for ob in list(bpy.data.objects):
        if ob.name not in ("cam", "sun"):
            bpy.data.objects.remove(ob, do_unlink=True)


def want(k):
    return not ONLY or k in ONLY


def tank_list():
    return [t for t in ARGS.tanks.split(",") if t in TANKS] if ARGS.tanks else list(TANKS)


def do_tank():
    """Each tank's hull and turret (with its barrel at the sprites' pitch),
    32 yaws, from the battle's camera and from low"""
    n = ARGS.yaws
    step = 360.0 / n
    for kind in tank_list():
        for part in ("h", "t"):
            clear_scene_objects()
            objs = build_tank(kind, hull=part == "h", turret=part == "t", barrel=part == "t")
            for k in range(n):
                yaw = k * step
                rotate_all(objs, yaw)
                if "top" in ARGS.sets:
                    render_top(objs, "%s_%s_%02d" % (kind, part, k), 256, 256, 0.9)
                # low: relative yaw k*step; the camera looks along +Y (world
                # heading 90 deg), so the object turns to 90 + rel
                rotate_all(objs, 90.0 + yaw)
                if "low" in ARGS.sets:
                    render_low(objs, "%s_%s_%02d" % (kind, part, k), 288, 192, 1.0)


def do_show():
    """The hangar's turntable: the whole tank, 16 yaws, close and low"""
    F, R, UP = view_axes(0.0, 20.0)
    for kind in tank_list():
        clear_scene_objects()
        objs = build_tank(kind)
        for k in range(16):
            rotate_all(objs, 90.0 + 200.0 + k * 22.5)
            target = V((0, 0, 1.0))
            place_camera_ortho(F, R, UP, target, 96.0, 576, 384)
            set_sun(SUN_AZ, SUN_ELEV)
            planes = render_planes(objs, F, (0, 0, 0))
            ax = 576 / 2 + 96.0 * (V((0, 0, 0)) - target).dot(R)
            ay = 384 / 2 - 96.0 * (V((0, 0, 0)) - target).dot(UP)
            save_spr("show", "%s_%02d" % (kind, k), planes, ax, ay)


def do_fp():
    """The gunner's view at half the screen's size (the first-person view is
    drawn at half and doubled): the turret's roof once, and the barrel with
    its mantlet per elevation, both from that tank's gunner's eye"""
    W = H = 640
    p = math.radians(FP_PITCH)
    F = V((math.cos(p), 0.0, math.sin(p)))
    R = V((0.0, -1.0, 0.0))
    UP = R.cross(F)
    for kind in tank_list():
        eye = TANKS[kind][5]
        for k in range(-1, 14):
            clear_scene_objects()
            if k < 0:
                objs = build_tank(kind, hull=False, barrel=False)
            else:
                objs = build_barrel(kind, -5 + 5 * k)
            place_camera_persp(eye, F, R, UP, FP_F / 2, W, H)
            set_sun(135.0, 55.0)
            planes = render_planes(objs, F, eye)
            save_spr("fp", "%s_roof" % kind if k < 0 else "%s_b_%02d" % (kind, k), planes, W / 2, H / 2)


def do_scenery():
    """Each biome's five pieces: two kinds of tree (or what stands for one),
    what a broken one leaves, two stones. Their colours are the biome's
    (the app's scenery paints), not the model's."""
    biomes = [b for b in ARGS.biomes.split(",") if b in BIOMES] if ARGS.biomes else list(BIOMES)
    for biome in biomes:
        ta, tb, st, blk = BIOMES[biome]
        for slot, fn, w, h, lift in (("ta", ta, 192, 256, 1.9), ("tb", tb, 192, 256, 1.9), ("st", st, 112, 112, 0.3)):
            clear_scene_objects()
            objs = fn()
            rotate_all(objs, 0)
            name = "%s_%s" % (biome, slot)
            if "top" in ARGS.sets:
                render_top(objs, name, w, h, lift)
            if "low" in ARGS.sets:
                rotate_all(objs, 90.0)
                render_low(objs, name, w * 3 // 2, h * 3 // 2, lift)
        for v in (1, 2):
            clear_scene_objects()
            objs = blk(seed=v * 11 + len(biome))
            rotate_all(objs, 0)
            if "top" in ARGS.sets:
                render_top(objs, "%s_b%d" % (biome, v), 96, 96, 0.5)
            # a block looks the same every 90 deg: 8 relative yaws cover it
            for k in range(8):
                rotate_all(objs, 90.0 + k * 11.25)
                if "low" in ARGS.sets:
                    render_low(objs, "%s_b%d_%02d" % (biome, v, k), 128, 128, 0.5)


def do_marks():
    """The modes' marks, like a tree: one top and one low sprite each"""
    for slot, fn in MARKS.items():
        clear_scene_objects()
        objs = fn()
        rotate_all(objs, 0)
        name = "marks_%s" % slot
        if "top" in ARGS.sets:
            render_top(objs, name, 160, 192, 1.4)
        if "low" in ARGS.sets:
            rotate_all(objs, 90.0)
            render_low(objs, name, 240, 288, 1.4)


def export_obj():
    """The tank as triangles, for the real-time rasteriser's bench
    (tools/bench.c): full detail and decimated."""
    for ratio, tag_ in ((1.0, "full"), (0.25, "mid"), (0.08, "low")):
        clear_scene_objects()
        objs = build_tank("bombo")
        dg = bpy.context.evaluated_depsgraph_get()
        verts, faces, cols = [], [], []
        for ob in objs:
            if ob.type != "MESH":
                continue
            if ratio < 1.0:
                md = ob.modifiers.new("dec", "DECIMATE")
                md.ratio = ratio
                dg = bpy.context.evaluated_depsgraph_get()
            ev = ob.evaluated_get(dg)
            me = ev.to_mesh()
            me.calc_loop_triangles()
            base = len(verts)
            mw = ob.matrix_world
            for v in me.vertices:
                verts.append(mw @ v.co)
            for t in me.loop_triangles:
                faces.append((base + t.vertices[0], base + t.vertices[1], base + t.vertices[2]))
                cols.append(ob["rid"])
            ev.to_mesh_clear()
        os.makedirs(os.path.join(OUT, "mesh"), exist_ok=True)
        with open(os.path.join(OUT, "mesh", "tank_%s.obj" % tag_), "w") as f:
            for v in verts:
                f.write("v %.4f %.4f %.4f\n" % (v.x, v.y, v.z))
            for (a, b, c), rid in zip(faces, cols):
                f.write("f %d %d %d %d\n" % (a + 1, b + 1, c + 1, rid))
        print("mesh tank_%s: %d triangles" % (tag_, len(faces)))


reset()
if want("mesh"):
    export_obj()
if want("tank"):
    do_tank()
if want("scenery"):
    do_scenery()
if want("marks"):
    do_marks()
if want("fp"):
    do_fp()
if want("show"):
    do_show()
print("done ->", OUT)
