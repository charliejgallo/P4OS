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
    11: (0.80, 0.10, 0.0),
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
# The tank. Origin: on the ground under the turret's pivot; +X forward.

BARREL_PIVOT = V((0.64, 0.0, 1.38))


def build_hull():
    objs = []

    def wedge(v):        # a sloped glacis and a softer rear
        if v.co.z > 0 and v.co.x > 0:
            v.co.x -= 0.34
        if v.co.z > 0 and v.co.x < 0:
            v.co.x += 0.14
    objs.append(rbox("body", (0.08, 0, 0.76), (2.50, 1.46, 0.70), 0.28, 1, shape=wedge))
    # a band of the second paint round the body, under the deck
    objs.append(rbox("band", (0.10, 0, 0.62), (2.58, 1.54, 0.16), 0.08, 2, seg=3))
    for s in (-1, 1):
        objs.append(rbox("track%d" % s, (0.06, s * 0.95, 0.40), (2.86, 0.56, 0.80), 0.34, 3, seg=6))
        for i in range(11):                      # tread ridges on the track's top
            x = -1.15 + i * 0.23
            objs.append(rbox("tread%d_%d" % (s, i), (x + 0.06, s * 0.95, 0.80), (0.10, 0.54, 0.06), 0.025, 3, seg=2))
        for i, x in enumerate((-0.92, -0.31, 0.31, 0.92)):
            objs.append(cyl("wheel%d_%d" % (s, i), (x + 0.06, s * 1.22, 0.38), 0.23, 0.08, "Y", 4, bevel=0.03))
            objs.append(cyl("hub%d_%d" % (s, i), (x + 0.06, s * 1.27, 0.38), 0.10, 0.05, "Y", 2, bevel=0.02))
        objs.append(ball("lamp%d" % s, (1.10, s * 0.50, 0.98), 0.11, 5))
    objs.append(rbox("rearbox", (-1.08, 0, 0.98), (0.28, 0.70, 0.28), 0.08, 2, seg=3))
    objs.append(cyl("exhaust", (-1.12, -0.48, 0.80), 0.08, 0.26, "X", 4, bevel=0.02))
    return objs


def build_turret(pitch_deg=8.0, with_barrel=True):
    objs = []
    objs.append(rbox("turret", (-0.10, 0, 1.40), (1.50, 1.24, 0.66), 0.31, 1, seg=6))
    objs.append(rbox("ring", (-0.10, 0, 1.10), (1.40, 1.16, 0.12), 0.05, 2, seg=3))
    objs.append(cyl("hatch", (-0.34, 0.22, 1.74), 0.27, 0.09, "Z", 2, bevel=0.035))
    objs.append(ball("knob", (-0.34, 0.22, 1.81), 0.06, 4))
    for s in (-1, 1):
        objs.append(cyl("badge%d" % s, (-0.14, s * 0.625, 1.40), 0.21, 0.03, "Y", 6, bevel=0.01))
    if with_barrel:
        objs += build_barrel(pitch_deg)
    return objs


def build_barrel(pitch_deg):
    parts = [
        rbox("mantlet", (0.66, 0, 1.38), (0.36, 0.60, 0.42), 0.14, 2, seg=4),
        cyl("barrel", (1.25, 0, 1.38), 0.13, 1.10, "X", 1, bevel=0.0),
        cyl("muzzle", (1.86, 0, 1.38), 0.18, 0.30, "X", 4, bevel=0.05),
        cyl("muzzle_in", (2.02, 0, 1.38), 0.085, 0.03, "X", 3),
    ]
    root = link(bpy.data.objects.new("barrel_pivot", None))
    root.location = BARREL_PIVOT
    for ob in parts:
        ob.parent = root
        ob.matrix_parent_inverse = Matrix.Translation(-BARREL_PIVOT)
    root.rotation_euler = (0, -math.radians(pitch_deg), 0)
    return parts + [root]


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
    rid, Z = grow(rid, Z, have, A > 0)
    A[rid == 0] = 0
    return L, A, rid.astype(np.uint8), Z.astype(np.uint8)


def grow(rid, Z, have, want):
    rid = rid.copy()
    Z = Z.copy()
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
            have = have | take
            need = need & ~take
    return rid, Z


def save_spr(set_name, name, planes, ax, ay):
    L, A, ID, Z = planes
    ys, xs = np.nonzero(A)
    if len(xs) == 0:
        print("EMPTY sprite", set_name, name)
        return
    x0, x1, y0, y1 = xs.min(), xs.max() + 1, ys.min(), ys.max() + 1
    crop = [p[y0:y1, x0:x1] for p in (L, A, ID, Z)]
    d = os.path.join(OUT, set_name)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name + ".spr"), "wb") as f:
        f.write(b"TSPR")
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


def do_tank():
    n = ARGS.yaws
    step = 360.0 / n
    for part in ("hull", "turret"):
        clear_scene_objects()
        objs = build_hull() if part == "hull" else build_turret()
        for k in range(n):
            yaw = k * step
            rotate_all(objs, yaw)
            if "top" in ARGS.sets:
                render_top(objs, "%s_%02d" % (part, k), 224, 224, 0.8)
            # low: relative yaw k*step; the camera looks along +Y (world
            # heading 90 deg), so the object turns to 90 + rel
            rotate_all(objs, 90.0 + yaw)
            if "low" in ARGS.sets:
                render_low(objs, "%s_%02d" % (part, k), 272, 176, 0.95)


def do_fp():
    """The gunner's view at half the screen's size (the first-person view is
    drawn at half and doubled): the turret's roof once, and the barrel with
    its mantlet per elevation. Their depth planes keep the roof over the
    barrel's root."""
    W = H = 640
    p = math.radians(FP_PITCH)
    F = V((math.cos(p), 0.0, math.sin(p)))
    R = V((0.0, -1.0, 0.0))
    UP = R.cross(F)
    for k in range(-1, 14):
        clear_scene_objects()
        if k < 0:
            objs = build_turret(with_barrel=False)
        else:
            objs = build_barrel(-5 + 5 * k)
        place_camera_persp(FP_EYE, F, R, UP, FP_F / 2, W, H)
        set_sun(135.0, 55.0)
        planes = render_planes(objs, F, FP_EYE)
        # the anchor is the principal point: the image centre
        save_spr("fp", "roof" if k < 0 else "barrel_%02d" % k, planes, W / 2, H / 2)


def do_scenery():
    items = [("tree_round", build_tree_round, 192, 256, 1.9),
             ("tree_pine", build_tree_pine, 192, 256, 1.9),
             ("stump", build_stump, 96, 96, 0.3)]
    for name, fn, w, h, lift in items:
        clear_scene_objects()
        objs = fn()
        rotate_all(objs, 0)
        if "top" in ARGS.sets:
            render_top(objs, name, w, h, lift)
        if "low" in ARGS.sets:
            render_low(objs, name, w * 3 // 2, h * 3 // 2, lift)
    for v in (1, 2):
        clear_scene_objects()
        objs = build_block(seed=v * 11)
        rotate_all(objs, 0)
        if "top" in ARGS.sets:
            render_top(objs, "block%d" % v, 96, 96, 0.5)
        # a block looks the same every 90 deg: 8 relative yaws cover it
        for k in range(8):
            rotate_all(objs, 90.0 + k * 11.25)
            if "low" in ARGS.sets:
                render_low(objs, "block%d_%02d" % (v, k), 128, 128, 0.5)


def export_obj():
    """The tank as triangles, for the real-time rasteriser's bench
    (tools/bench.c): full detail and decimated."""
    for ratio, tag_ in ((1.0, "full"), (0.25, "mid"), (0.08, "low")):
        clear_scene_objects()
        objs = build_hull() + build_turret()
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
if want("fp"):
    do_fp()
print("done ->", OUT)
