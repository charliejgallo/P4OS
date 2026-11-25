"""Style B - chubby 3D toys, modelled by code and rendered in Blender.

    /Applications/Blender.app/Contents/MacOS/Blender -b -P tools/icons/style_b_chubby.py -- \
        [--only aos.settings,aos.clima] [--samples 128]
    python3 tools/icons/style_b_post.py       # 480/104 px renders -> 120, 108, 26

Blender 3.3, Cycles, always -b (no window). Nothing is loaded from disk: every
model is made here, the way apps/tanques/tools/blender/tanques_art.py makes
the tanks: rounded boxes, extruded outlines with a fat bevel, metaballs for
anything soft. Same family as Monster Hop, Tanques and Tortuga.

The scene, the same for every icon:

  - the tile: the firmware's rounded square (corner 22.5 % of the side),
    extruded and bevelled into a slab of soft plastic, its colour a vertical
    gradient of the app's two colours (lighter at the top);
  - the toy: one object, chubby, tilted so its volume shows, lifted off the
    tile so it casts a soft shadow on it;
  - light: a soft sun from the top left (a 35 deg disc, so the shadows are
    blurry), a white world for fill. Camera orthographic, straight down, the
    tile filling the frame exactly.

The 120 and 108 come from one 480 px render; the 26 from a 104 px render of
a simpler toy (bigger, the small parts left out), both shrunk 4x by
style_b_post.py with premultiplied alpha.
"""

import argparse
import math
import os
import sys

import bpy
import bmesh
from mathutils import Matrix, Vector as V

argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
ap = argparse.ArgumentParser()
ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "muestra", "b_chubby", "raw"))
ap.add_argument("--only", default="")
ap.add_argument("--samples", type=int, default=128)
ap.add_argument("--cpu", action="store_true")
ARGS = ap.parse_args(argv)
OUT = os.path.abspath(ARGS.out)
os.makedirs(OUT, exist_ok=True)

BIG, MINI = 480, 104
RADIUS = 0.225


def srgb(h):
    """'#RRGGBB' -> linear RGBA, which is what Blender's colour inputs take."""
    h = h.lstrip("#")
    c = [int(h[i:i + 2], 16) / 255 for i in (0, 2, 4)]
    lin = [x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4 for x in c]
    return (*lin, 1.0)


# --------------------------------------------------------------------------
# Scene

def reset(res):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    sc.render.engine = "CYCLES"
    if not ARGS.cpu:
        try:
            prefs = bpy.context.preferences.addons["cycles"].preferences
            prefs.compute_device_type = "METAL"
            prefs.get_devices()
            for d in prefs.devices:
                d.use = True
            sc.cycles.device = "GPU"
        except Exception as e:
            print("no GPU:", e)
    sc.cycles.samples = ARGS.samples
    sc.cycles.use_denoising = False
    sc.cycles.max_bounces = 6
    sc.render.resolution_x = sc.render.resolution_y = res
    sc.render.resolution_percentage = 100
    sc.render.film_transparent = True
    sc.render.filter_size = 1.2
    sc.view_settings.view_transform = "Standard"
    sc.view_settings.look = "None"
    sc.render.image_settings.file_format = "PNG"
    sc.render.image_settings.color_mode = "RGBA"
    sc.render.image_settings.color_depth = "16"
    w = bpy.data.worlds.new("w")
    w.use_nodes = True
    bgn = w.node_tree.nodes["Background"]
    bgn.inputs[0].default_value = (0.92, 0.95, 1.0, 1)
    bgn.inputs[1].default_value = 0.5
    sc.world = w
    cam = bpy.data.objects.new("cam", bpy.data.cameras.new("cam"))
    cam.data.type = "ORTHO"
    cam.data.ortho_scale = 1.0
    cam.location = (0, 0, 6)
    sc.collection.objects.link(cam)
    sc.camera = cam
    sun = bpy.data.objects.new("sun", bpy.data.lights.new("sun", "SUN"))
    sun.data.energy = 2.6
    sun.data.angle = math.radians(35)
    d = V((0.42, -0.55, -0.72)).normalized()      # from the top left, in front
    sun.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()
    sc.collection.objects.link(sun)
    # a dim light from the bottom right, for a rim on the toys
    rim = bpy.data.objects.new("rim", bpy.data.lights.new("rim", "SUN"))
    rim.data.energy = 0.9
    rim.data.angle = math.radians(20)
    d = V((-0.5, 0.6, -0.35)).normalized()
    rim.rotation_euler = d.to_track_quat("-Z", "Y").to_euler()
    sc.collection.objects.link(rim)
    return sc


def link(ob):
    bpy.context.scene.collection.objects.link(ob)
    return ob


def mat(name, color, rough=0.38, metal=0.0, coat=0.25, emit=None, sss=0.0):
    m = bpy.data.materials.new(name)
    m.use_nodes = True
    b = m.node_tree.nodes["Principled BSDF"]
    b.inputs["Base Color"].default_value = srgb(color) if isinstance(color, str) else color
    b.inputs["Roughness"].default_value = rough
    b.inputs["Metallic"].default_value = metal
    b.inputs["Clearcoat"].default_value = coat
    b.inputs["Clearcoat Roughness"].default_value = 0.25
    if sss:
        b.inputs["Subsurface"].default_value = sss
        b.inputs["Subsurface Color"].default_value = b.inputs["Base Color"].default_value
    if emit:
        b.inputs["Emission"].default_value = srgb(emit)
        b.inputs["Emission Strength"].default_value = 0.35
    return m


def mat_grad(name, top, bottom, rough=0.42, axis="Y", lo=-0.5, hi=0.5, stripes=0):
    """A colour ramp along an object-space axis: 'bottom' at lo, 'top' at hi.
    stripes > 0 lightens every other band across X (a mowed pitch)."""
    m = mat(name, "#FFFFFF", rough=rough, coat=0.3)
    nt = m.node_tree
    tc = nt.nodes.new("ShaderNodeTexCoord")
    sep = nt.nodes.new("ShaderNodeSeparateXYZ")
    mr = nt.nodes.new("ShaderNodeMapRange")
    mr.inputs["From Min"].default_value = lo
    mr.inputs["From Max"].default_value = hi
    ramp = nt.nodes.new("ShaderNodeValToRGB")
    ramp.color_ramp.elements[0].color = srgb(bottom)
    ramp.color_ramp.elements[1].color = srgb(top)
    nt.links.new(tc.outputs["Object"], sep.inputs[0])
    nt.links.new(sep.outputs[axis], mr.inputs["Value"])
    nt.links.new(mr.outputs["Result"], ramp.inputs["Fac"])
    color = ramp.outputs["Color"]
    if stripes:
        mul = nt.nodes.new("ShaderNodeMath")
        mul.operation = "MULTIPLY"
        mul.inputs[1].default_value = stripes
        flo = nt.nodes.new("ShaderNodeMath")
        flo.operation = "FLOOR"
        add = nt.nodes.new("ShaderNodeMath")
        add.operation = "ADD"
        add.inputs[1].default_value = 2 * stripes
        mod = nt.nodes.new("ShaderNodeMath")
        mod.operation = "MODULO"          # of floor(x * n) + 2n, never negative
        mod.inputs[1].default_value = 2.0
        mix = nt.nodes.new("ShaderNodeMixRGB")
        mix.blend_type = "SCREEN"
        mix.inputs["Color2"].default_value = (0.06, 0.08, 0.06, 1)
        nt.links.new(sep.outputs["X"], mul.inputs[0])
        nt.links.new(mul.outputs[0], flo.inputs[0])
        nt.links.new(flo.outputs[0], add.inputs[0])
        nt.links.new(add.outputs[0], mod.inputs[0])
        nt.links.new(mod.outputs[0], mix.inputs["Fac"])
        nt.links.new(color, mix.inputs["Color1"])
        color = mix.outputs["Color"]
    nt.links.new(color, nt.nodes["Principled BSDF"].inputs["Base Color"])
    return m


def setmat(ob, m):
    ob.data.materials.clear()
    ob.data.materials.append(m)
    return ob


def smooth(ob, angle=40):
    if ob.type == "MESH":
        for p in ob.data.polygons:
            p.use_smooth = True
        ob.data.use_auto_smooth = True
        ob.data.auto_smooth_angle = math.radians(angle)
    return ob


# --------------------------------------------------------------------------
# Shapes

def rounded_poly(pts, r, seg=8):
    """A polygon with every corner rounded by r (or a list of radii)."""
    n = len(pts)
    rs = r if isinstance(r, (list, tuple)) else [r] * n
    out = []
    for i in range(n):
        p0, p1, p2 = V(pts[i - 1]), V(pts[i]), V(pts[(i + 1) % n])
        a, b = (p0 - p1).normalized(), (p2 - p1).normalized()
        ang = math.acos(max(-1, min(1, a.dot(b))))
        ri = rs[i]
        if ri <= 0 or ang < 1e-3:
            out.append(tuple(p1))
            continue
        t = ri / math.tan(ang / 2)
        s, e = p1 + a * t, p1 + b * t
        bis = (a + b).normalized()
        c = p1 + bis * (ri / math.sin(ang / 2))
        a0 = math.atan2(s.y - c.y, s.x - c.x)
        a1 = math.atan2(e.y - c.y, e.x - c.x)
        da = a1 - a0
        while da > math.pi:
            da -= 2 * math.pi
        while da < -math.pi:
            da += 2 * math.pi
        for k in range(seg + 1):
            aa = a0 + da * k / seg
            out.append((c.x + ri * math.cos(aa), c.y + ri * math.sin(aa)))
    return out


def circle(cx, cy, r, n=64):
    return [(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n)) for i in range(n)]


def slab(name, loops, depth, bevel, z=0.0, m=None, res=5):
    """Outlines (the first the outside, the rest holes) extruded 'depth' and
    bevelled 'bevel' all round: the chubby primitive. Bottom face at z."""
    cu = bpy.data.curves.new(name, "CURVE")
    cu.dimensions = "2D"
    cu.fill_mode = "BOTH"
    cu.extrude = max(0.0, depth / 2 - bevel)
    cu.bevel_depth = bevel
    cu.bevel_resolution = res
    cu.offset = -bevel            # the bevel grows inward, so the outline is the size
    for loop in loops:
        sp = cu.splines.new("POLY")
        sp.points.add(len(loop) - 1)
        for i, (x, y) in enumerate(loop):
            sp.points[i].co = (x, y, 0, 1)
        sp.use_cyclic_u = True
        sp.use_smooth = True
    ob = link(bpy.data.objects.new(name, cu))
    ob.location.z = z + depth / 2
    if m:
        cu.materials.append(m)
    return ob


def rbox(name, c, size, r, m, seg=4):
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    for v in bm.verts:
        v.co = V((v.co.x * size[0], v.co.y * size[1], v.co.z * size[2]))
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    ob = link(bpy.data.objects.new(name, me))
    ob.location = c
    md = ob.modifiers.new("bev", "BEVEL")
    md.width = r
    md.segments = seg
    md.limit_method = "NONE"
    sub = ob.modifiers.new("sub", "SUBSURF")
    sub.levels = sub.render_levels = 1
    smooth(ob, 60)
    return setmat(ob, m)


def ball(name, c, r, m, scale=(1, 1, 1)):
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=48, v_segments=24, radius=r)
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    ob = link(bpy.data.objects.new(name, me))
    ob.location = c
    ob.scale = scale
    smooth(ob, 180)
    return setmat(ob, m)


def cyl(name, c, r, depth, m, rot=(0, 0, 0), r2=None, verts=48, bevel=0.0):
    bm = bmesh.new()
    bmesh.ops.create_cone(bm, cap_ends=True, cap_tris=False, segments=verts,
                          radius1=r, radius2=r if r2 is None else r2, depth=depth)
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    ob = link(bpy.data.objects.new(name, me))
    ob.location = c
    ob.rotation_euler = rot
    if bevel:
        md = ob.modifiers.new("bev", "BEVEL")
        md.width = bevel
        md.segments = 4
        md.limit_method = "ANGLE"
        md.angle_limit = math.radians(40)
    smooth(ob, 45)
    return setmat(ob, m)


def tube(name, pts, r, m):
    cu = bpy.data.curves.new(name, "CURVE")
    cu.dimensions = "3D"
    cu.bevel_depth = r
    cu.bevel_resolution = 6
    cu.use_fill_caps = True
    sp = cu.splines.new("NURBS")
    sp.points.add(len(pts) - 1)
    for i, p in enumerate(pts):
        sp.points[i].co = (*p, 1)
    sp.use_endpoint_u = True
    sp.order_u = 3
    sp.use_smooth = True
    ob = link(bpy.data.objects.new(name, cu))
    cu.materials.append(m)
    return ob


def group(name, objs, loc=(0, 0, 0), rot=(0, 0, 0), scale=1.0):
    root = link(bpy.data.objects.new(name, None))
    for o in objs:
        o.parent = root
    root.location = loc
    root.rotation_euler = [math.radians(a) for a in rot]
    root.scale = (scale, scale, scale)
    return root


def tile(top, bottom, stripes=0):
    """The rounded square, 1.0 across, as a plastic slab."""
    h = 0.5
    pts = rounded_poly([(-h, -h), (h, -h), (h, h), (-h, h)], RADIUS, seg=16)
    return slab("tile", [pts], 0.12, 0.035, z=-0.12,
                m=mat_grad("tile", top, bottom, stripes=stripes), res=6)


# --------------------------------------------------------------------------
# The toys. Each builds into the scene; 'mini' is the 26 px version.

def gear_loop(r_out, r_in, teeth, frac=0.5):
    pts = []
    pitch = 2 * math.pi / teeth
    top, root = pitch * frac * 0.5, pitch * (1 - frac) * 0.5
    for i in range(teeth):
        a = i * pitch + math.pi / 2
        for ang, r in ((a - top - root * 0.35, r_in), (a - top, r_out),
                       (a + top, r_out), (a + top + root * 0.35, r_in)):
            pts.append((r * math.cos(ang), r * math.sin(ang)))
    return rounded_poly(pts, [0.03, 0.04, 0.04, 0.03] * teeth, seg=6)


def settings(mini):
    tile("#A9ACB4", "#4E5157")
    white = mat("gear", "#F4F5F8", rough=0.32, coat=0.4)
    teeth = 8 if mini else 9
    s = 1.25 if mini else 1.0
    g = slab("gear", [gear_loop(0.34, 0.255, teeth, 0.52), circle(0, 0, 0.11 if mini else 0.13)],
             0.13, 0.028, m=white)
    parts = [g]
    if not mini:
        hub = mat("hub", "#7C8089", rough=0.3, metal=0.5)
        parts.append(cyl("axle", (0, 0, 0.04), 0.075, 0.09, hub, bevel=0.02))
    group("toy", parts, loc=(0, 0.01, 0.05), rot=(-22, 14, 8), scale=s)


def serial(mini):
    tile("#41505E", "#10161B")
    shell = mat("shell", "#E9EDF2", rough=0.28, metal=0.55, coat=0.2)
    face = mat("face", "#151C22", rough=0.5)
    gold = mat("gold", "#F2B92C", rough=0.25, metal=0.9)
    cable = mat("cable", "#2D7BF0", rough=0.45, coat=0.3)
    s = 1.25 if mini else 1.0
    outer = rounded_poly([(-0.36, 0.17), (0.36, 0.17), (0.29, -0.15), (-0.29, -0.15)], 0.07)
    inner = rounded_poly([(-0.27, 0.11), (0.27, 0.11), (0.22, -0.09), (-0.22, -0.09)], 0.045)
    parts = [slab("shell", [outer], 0.16, 0.045, m=shell),
             slab("face", [inner], 0.03, 0.01, z=0.15, m=face)]
    if not mini:
        for i in range(5):
            parts.append(ball(f"p{i}", (-0.16 + i * 0.08, 0.045, 0.18), 0.024, gold))
        for i in range(4):
            parts.append(ball(f"q{i}", (-0.12 + i * 0.08, -0.035, 0.18), 0.024, gold))
    else:
        for i in range(3):
            parts.append(ball(f"p{i}", (-0.12 + i * 0.12, 0.01, 0.18), 0.034, gold))
    # the boot and the cable going out of the bottom of the tile
    parts.append(rbox("boot", (0, -0.2, 0.07), (0.16, 0.12, 0.12), 0.04, shell))
    parts.append(tube("cable", [(0, -0.24, 0.07), (0, -0.33, 0.06), (0.06, -0.42, 0.03), (0.02, -0.56, 0.02)],
                      0.04 if not mini else 0.05, cable))
    group("toy", parts, loc=(0, 0.06, 0.02), rot=(-18, 0, 0), scale=s)


def clima(mini):
    tile("#45A6FF", "#0A57D0")
    sunm = mat("sun", "#FFC21A", rough=0.3, coat=0.5, emit="#FF9A00")
    cloud = mat("cloud", "#FFFFFF", rough=0.45, coat=0.2)
    s = 1.15 if mini else 1.0
    parts = [ball("sun", (0.12, 0.15, 0.05), 0.18, sunm)]
    if not mini:
        for i in range(8):
            a = math.radians(i * 45)
            x, y = 0.12 + 0.265 * math.cos(a), 0.15 + 0.265 * math.sin(a)
            parts.append(cyl(f"ray{i}", (x, y, 0.06), 0.03, 0.07, sunm,
                             rot=(0, math.radians(90), a), bevel=0.02))
    # the cloud: a metaball, so the bumps melt into each other
    mb = bpy.data.metaballs.new("cloud")
    mb.resolution = 0.012
    mb.render_resolution = 0.008
    for (x, y, r) in ((-0.2, -0.12, 0.15), (-0.04, -0.02, 0.2), (0.15, -0.1, 0.16),
                      (0.27, -0.16, 0.11), (-0.3, -0.17, 0.1), (0.04, -0.17, 0.15)):
        el = mb.elements.new()
        el.co = (x, y, 0.15)
        el.radius = r * 1.25
    mb.threshold = 0.6
    ob = link(bpy.data.objects.new("cloud", mb))
    mb.materials.append(cloud)
    ob.scale = (1, 1, 0.7)
    parts.append(ob)
    group("toy", parts, loc=(0, 0.0, 0.0), rot=(-8, 0, 0), scale=s)


def notas(mini):
    tile("#FFE55E", "#FFA20A")
    paper = mat("paper", "#FFFFFF", rough=0.5, coat=0.15)
    band = mat("band", "#FF8A00", rough=0.35, coat=0.4)
    line = mat("line", "#C3C8D1", rough=0.5)
    s = 1.12 if mini else 1.0
    parts = [rbox("pad", (0, -0.01, 0.03), (0.58, 0.7, 0.07), 0.035, paper),
             rbox("band", (0, 0.29, 0.06), (0.6, 0.14, 0.1), 0.04, band)]
    rows = ((0.1, 0.4), (-0.07, 0.4), (-0.24, 0.26)) if not mini else ((0.06, 0.38), (-0.16, 0.26))
    for i, (y, w) in enumerate(rows):
        parts.append(rbox(f"l{i}", (-0.2 + w / 2 - 0.02 + 0.02, y, 0.07),
                          (w, 0.035 if not mini else 0.06, 0.012), 0.012, line))
    if not mini:
        pen = mat("pen", "#FF6A3D", rough=0.35, coat=0.4)
        wood = mat("wood", "#F2C38C", rough=0.6)
        lead = mat("lead", "#3A3A3C", rough=0.5)
        pink = mat("pink", "#FF7DA8", rough=0.5)
        metal = mat("metal", "#D7DBE2", rough=0.25, metal=0.9)
        r = 0.05
        pp = [cyl("body", (0, 0, 0), r, 0.42, pen, verts=6, bevel=0.012),
              cyl("tip", (0, -0.25, 0), r * 0.98, 0.09, wood, r2=0.012, rot=(math.radians(90), 0, 0), verts=24),
              cyl("lead", (0, -0.293, 0), 0.016, 0.02, lead, r2=0.003, rot=(math.radians(90), 0, 0), verts=16),
              cyl("ferrule", (0, 0.235, 0), r * 1.02, 0.05, metal, rot=(math.radians(90), 0, 0), bevel=0.008),
              cyl("eraser", (0, 0.285, 0), r * 1.0, 0.06, pink, rot=(math.radians(90), 0, 0), bevel=0.02)]
        pp[0].rotation_euler = (math.radians(90), 0, 0)
        parts.append(group("pencil", pp, loc=(0.2, -0.16, 0.16), rot=(0, 18, 40)))
    group("toy", parts, loc=(0, 0.0, 0.02), rot=(-10, 6, -4), scale=s)


def metegol(mini):
    tile("#41B862", "#127231", stripes=0 if mini else 4)
    chalk = mat("chalk", "#FFFFFF", rough=0.6, coat=0.0)
    rodm = mat("rod", "#E3E7EC", rough=0.2, metal=1.0)
    red = mat("red", "#F2392C", rough=0.35, coat=0.5)
    blue = mat("blue", "#2E63F0", rough=0.35, coat=0.5)
    skin = mat("skin", "#FFD2A8", rough=0.5, coat=0.2)
    white = mat("ball", "#FFFFFF", rough=0.3, coat=0.5)
    # the pitch's lines lie on the tile
    ring = slab("ring", [circle(0, 0, 0.14), circle(0, 0, 0.12)], 0.004, 0.0, z=0.0, m=chalk)
    for y0, y1 in ((0.14, 0.47), (-0.47, -0.14)):
        slab("mid", [[(-0.01, y0), (0.01, y0), (0.01, y1), (-0.01, y1)]], 0.004, 0.0, z=0.0, m=chalk)

    def player(x, y, m):
        return [rbox("body", (x, y, 0.14), (0.15, 0.2, 0.11), 0.05, m),
                ball("head", (x, y + 0.06, 0.22), 0.055, skin),
                rbox("feet", (x, y - 0.12, 0.08), (0.12, 0.08, 0.06), 0.025, m)]
    parts = []
    for x, m in ((-0.24, blue), (0.24, red)):
        parts.append(cyl(f"rod{x}", (x, 0, 0.14), 0.022, 0.97, rodm, rot=(math.radians(90), 0, 0)))
        ys = (0.2, -0.22) if not mini else (0.0,)
        for y in ys:
            parts += player(x, y, m)
    parts.append(ball("ball", (0.04, -0.12 if not mini else -0.26, 0.06), 0.06 if not mini else 0.085, white))
    if mini:
        for p in parts:
            if p.name.startswith("head"):
                p.scale = (1.3, 1.3, 1.3)
    group("toy", parts, loc=(0, 0, 0), rot=(0, 0, 0), scale=1.0 if not mini else 1.25)


def store(mini):
    tile("#4DAFFF", "#0A5ADB")
    box = mat("box", "#E2A665", rough=0.5, coat=0.15)
    tape = mat("tape", "#F7E2B8", rough=0.3, coat=0.5)
    s = 1.18 if mini else 1.0
    e = 0.46
    parts = [rbox("box", (0, 0, 0), (e, e, e), 0.04, box)]
    # tape over the top and down two sides, a hair proud of the box
    parts.append(rbox("tape1", (0, 0, 0), (0.1, e + 0.012, e + 0.012), 0.02, tape))
    if not mini:
        parts.append(rbox("tag", (0.1, -e / 2 - 0.004, -0.08), (0.14, 0.012, 0.1), 0.006,
                          mat("tag", "#FFFFFF", rough=0.4)))
    root = group("toy", parts, loc=(0, -0.03, 0.05), scale=s)
    # turn it 45 deg about the vertical, then tip it toward us: three faces show
    root.rotation_euler = (Matrix.Rotation(math.radians(-50), 4, "X") @
                           Matrix.Rotation(math.radians(45), 4, "Z")).to_euler()


def clock(mini):
    tile("#3C3C41", "#0A0A0C")
    rim = mat("rim", "#FF9F0A", rough=0.3, coat=0.5)
    face = mat("face", "#FFFFFF", rough=0.45, coat=0.3)
    ink = mat("ink", "#1C1C1E", rough=0.4)
    s = 1.15 if mini else 1.0
    parts = [cyl("rim", (0, 0, 0.04), 0.36, 0.12, rim, bevel=0.04, verts=96),
             cyl("face", (0, 0, 0.07), 0.3, 0.1, face, bevel=0.02, verts=96)]
    if not mini:
        for i in range(12):
            a = math.radians(i * 30)
            parts.append(ball(f"t{i}", (0.24 * math.sin(a), 0.24 * math.cos(a), 0.12),
                              0.022 if i % 3 else 0.03, ink))
    hand = lambda n, ang, ln, w: rbox(n, (0, 0, 0), (w, ln, 0.03), 0.014, ink)
    h1 = hand("hour", 0, 0.15, 0.05)
    h2 = hand("min", 0, 0.22, 0.04)
    for h, ang, ln in ((h1, 120, 0.15), (h2, 0, 0.22)):
        a = math.radians(ang)
        h.location = (ln / 2 * math.sin(a), ln / 2 * math.cos(a), 0.14)
        h.rotation_euler = (0, 0, -a)
        parts.append(h)
    parts.append(cyl("pin", (0, 0, 0.15), 0.03, 0.04, rim, bevel=0.01))
    group("toy", parts, loc=(0, 0.01, 0.02), rot=(-16, 10, 0), scale=s)


def calc(mini):
    tile("#F4BC3E", "#86562B")
    body = mat("body", "#FFFFFF", rough=0.4, coat=0.3)
    lcd = mat("lcd", "#3E4B36", rough=0.2, coat=0.6)
    key = mat("key", "#D9DCE3", rough=0.4, coat=0.3)
    amber = mat("amber", "#FF9F0A", rough=0.35, coat=0.4)
    s = 1.1 if mini else 1.0
    parts = [rbox("body", (0, 0, 0.04), (0.56, 0.78, 0.1), 0.05, body),
             rbox("lcd", (0, 0.24, 0.09), (0.42, 0.16, 0.03), 0.02, lcd)]
    n = 2 if mini else 3
    k = 0.15 if mini else 0.1
    for r in range(n):
        for c in range(n):
            x = (c - (n - 1) / 2) * (k + 0.035)
            y = 0.06 - r * (k + 0.035) - (0.02 if mini else 0)
            parts.append(rbox(f"k{r}{c}", (x, y, 0.1), (k, k * 0.9, 0.05), 0.022,
                              amber if c == n - 1 else key))
    group("toy", parts, loc=(0, 0, 0.02), rot=(-14, 8, -6), scale=s)


def photos(mini):
    tile("#FFC83D", "#EC4899")
    frame = mat("frame", "#FFFFFF", rough=0.4, coat=0.3)
    sky = mat("sky", "#CFE4FF", rough=0.5)
    pink = mat("pink", "#F0447F", rough=0.4, coat=0.3)
    orange = mat("orange", "#FF8A2A", rough=0.4, coat=0.3)
    sunm = mat("sunp", "#FFC21A", rough=0.3, coat=0.5, emit="#FF9A00")
    parts = [rbox("frame", (0, 0, 0.03), (0.74, 0.6, 0.08), 0.05, frame),
             rbox("sky", (0, 0, 0.07), (0.6, 0.46, 0.02), 0.02, sky)]
    m1 = slab("m1", [rounded_poly([(-0.3, -0.23), (-0.06, 0.1), (0.14, -0.23)], 0.03)], 0.06, 0.02, z=0.07, m=pink)
    m2 = slab("m2", [rounded_poly([(-0.02, -0.23), (0.14, 0.0), (0.3, -0.23)], 0.03)], 0.07, 0.02, z=0.075, m=orange)
    parts += [m1, m2, ball("sun", (0.17, 0.12, 0.1), 0.06, sunm)]
    group("toy", parts, loc=(0, 0, 0.02), rot=(-14, 10, 4), scale=1.18 if mini else 1.0)


BUILD = {
    "aos.settings": settings, "aos.serial": serial, "aos.clima": clima,
    "aos.notas": notas, "demo.metegol": metegol, "aos.store": store,
    "aos.clock": clock, "aos.calc": calc, "aos.photos": photos,
}


def main():
    only = [s for s in ARGS.only.split(",") if s]
    for app_id, fn in BUILD.items():
        if only and app_id not in only:
            continue
        for mini, res in ((False, BIG), (True, MINI)):
            sc = reset(res)
            fn(mini)
            sc.render.filepath = os.path.join(OUT, f"{app_id}_{'mini' if mini else 'big'}.png")
            bpy.ops.render.render(write_still=True)
            print("rendered", sc.render.filepath, flush=True)


main()
