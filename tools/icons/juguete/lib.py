"""The 'juguete' pack (style B): the scene and the parts every model is made of.

Runs inside Blender 3.3 (-b). The scene is the one style_b_chubby.py settled
for the sample: a plastic tile with the app's gradient, one chubby toy over
it, a soft sun from the top left, an orthographic camera straight down, the
tile filling the frame. A model is a function build(mini) that calls tile()
and then makes its toy with the parts below; 'mini' is the 26 px version
(bigger, fewer parts).

Units: the tile is 1.0 across, centred on the origin, its top face at z = 0;
+y is up in the picture, +z towards the camera.
"""

import math

import bpy
import bmesh
from mathutils import Matrix, Vector as V

RADIUS = 0.225          # the firmware's icon corner, of the side


def srgb(h):
    """'#RRGGBB' -> linear RGBA."""
    h = h.lstrip("#")
    c = [int(h[i:i + 2], 16) / 255 for i in (0, 2, 4)]
    lin = [x / 12.92 if x <= 0.04045 else ((x + 0.055) / 1.055) ** 2.4 for x in c]
    return (*lin, 1.0)


def mix(h1, h2, t):
    """Two '#RRGGBB' mixed, t = 0 -> h1."""
    a = [int(h1.lstrip("#")[i:i + 2], 16) for i in (0, 2, 4)]
    b = [int(h2.lstrip("#")[i:i + 2], 16) for i in (0, 2, 4)]
    return "#" + "".join(f"{round(x + (y - x) * t):02X}" for x, y in zip(a, b))


# --------------------------------------------------------------------------
# Scene

def reset(res, samples=128, cpu=False):
    bpy.ops.wm.read_factory_settings(use_empty=True)
    sc = bpy.context.scene
    sc.render.engine = "CYCLES"
    if not cpu:
        try:
            prefs = bpy.context.preferences.addons["cycles"].preferences
            prefs.compute_device_type = "METAL"
            prefs.get_devices()
            for d in prefs.devices:
                d.use = True
            sc.cycles.device = "GPU"
        except Exception as e:
            print("no GPU:", e)
    sc.cycles.samples = samples
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
    for name, energy, ang, d in (("sun", 2.6, 35, (0.42, -0.55, -0.72)),
                                 ("rim", 0.9, 20, (-0.5, 0.6, -0.35))):
        lt = bpy.data.objects.new(name, bpy.data.lights.new(name, "SUN"))
        lt.data.energy = energy
        lt.data.angle = math.radians(ang)
        lt.rotation_euler = V(d).normalized().to_track_quat("-Z", "Y").to_euler()
        sc.collection.objects.link(lt)
    return sc


def link(ob):
    bpy.context.scene.collection.objects.link(ob)
    return ob


# --------------------------------------------------------------------------
# Materials. Kept by name, so a colour used twice is one material.

def mat(color, rough=0.38, metal=0.0, coat=0.25, emit=0.0, alpha=1.0, name=None):
    key = name or f"m{color}{rough}{metal}{coat}{emit}{alpha}"
    if key in bpy.data.materials:
        return bpy.data.materials[key]
    m = bpy.data.materials.new(key)
    m.use_nodes = True
    b = m.node_tree.nodes["Principled BSDF"]
    b.inputs["Base Color"].default_value = srgb(color)
    b.inputs["Roughness"].default_value = rough
    b.inputs["Metallic"].default_value = metal
    b.inputs["Clearcoat"].default_value = coat
    b.inputs["Clearcoat Roughness"].default_value = 0.25
    if emit:
        b.inputs["Emission"].default_value = srgb(color)
        b.inputs["Emission Strength"].default_value = emit
    if alpha < 1.0:
        b.inputs["Transmission"].default_value = 1.0 - alpha
        b.inputs["Roughness"].default_value = min(rough, 0.15)
        b.inputs["IOR"].default_value = 1.35
    return m


# The usual plastics and metals, by role
def plastic(c, rough=0.38):
    return mat(c, rough=rough, coat=0.3)


def glossy(c):
    return mat(c, rough=0.22, coat=0.6)


def metal(c="#D7DBE2", rough=0.25):
    return mat(c, rough=rough, metal=0.9, coat=0.0)


def matte(c):
    return mat(c, rough=0.6, coat=0.0)


def glow(c, k=1.5):
    return mat(c, rough=0.3, coat=0.4, emit=k)


def glass(c):
    return mat(c, rough=0.05, coat=0.5, alpha=0.35)


def mat_grad(top, bottom, rough=0.42, axis="Y", lo=-0.5, hi=0.5, stripes=0):
    """A ramp along an object axis ('bottom' at lo, 'top' at hi); stripes > 0
    lightens every other band across X."""
    m = mat("#FFFFFF", rough=rough, coat=0.3, name=f"grad{top}{bottom}{stripes}")
    nt = m.node_tree
    if len(nt.nodes) > 2:
        return m
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
        mod.operation = "MODULO"
        mod.inputs[1].default_value = 2.0
        mx = nt.nodes.new("ShaderNodeMixRGB")
        mx.blend_type = "SCREEN"
        mx.inputs["Color2"].default_value = (0.06, 0.08, 0.06, 1)
        nt.links.new(sep.outputs["X"], mul.inputs[0])
        nt.links.new(mul.outputs[0], flo.inputs[0])
        nt.links.new(flo.outputs[0], add.inputs[0])
        nt.links.new(add.outputs[0], mod.inputs[0])
        nt.links.new(mod.outputs[0], mx.inputs["Fac"])
        nt.links.new(color, mx.inputs["Color1"])
        color = mx.outputs["Color"]
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
# Outlines

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
        if ri <= 0 or ang < 1e-3 or ang > math.pi - 1e-3:
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
    return [(cx + r * math.cos(2 * math.pi * i / n), cy + r * math.sin(2 * math.pi * i / n))
            for i in range(n)]


def rrect(cx, cy, w, h, r):
    return rounded_poly([(cx - w / 2, cy - h / 2), (cx + w / 2, cy - h / 2),
                         (cx + w / 2, cy + h / 2), (cx - w / 2, cy + h / 2)], r)


def star(cx, cy, r_out, r_in, n=5, rot=90):
    pts = []
    for i in range(2 * n):
        r = r_out if i % 2 == 0 else r_in
        a = math.radians(rot + i * 180 / n)
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return pts


def gear_loop(r_out, r_in, teeth, frac=0.5, round_=(0.03, 0.04)):
    pts = []
    pitch = 2 * math.pi / teeth
    top, root = pitch * frac * 0.5, pitch * (1 - frac) * 0.5
    for i in range(teeth):
        a = i * pitch + math.pi / 2
        for ang, r in ((a - top - root * 0.35, r_in), (a - top, r_out),
                       (a + top, r_out), (a + top + root * 0.35, r_in)):
            pts.append((r * math.cos(ang), r * math.sin(ang)))
    r0, r1 = round_
    return rounded_poly(pts, [r0, r1, r1, r0] * teeth, seg=6)


# --------------------------------------------------------------------------
# Parts

def slab(loops, depth, bevel, m, z=0.0, res=5, name="slab"):
    """Outlines (the first the outside, the rest holes) extruded 'depth' and
    bevelled 'bevel': the chubby primitive. Bottom face at z."""
    cu = bpy.data.curves.new(name, "CURVE")
    cu.dimensions = "2D"
    cu.fill_mode = "BOTH"
    cu.extrude = max(0.0, depth / 2 - bevel)
    cu.bevel_depth = bevel
    cu.bevel_resolution = res
    cu.offset = -bevel
    for loop in loops:
        sp = cu.splines.new("POLY")
        sp.points.add(len(loop) - 1)
        for i, (x, y) in enumerate(loop):
            sp.points[i].co = (x, y, 0, 1)
        sp.use_cyclic_u = True
        sp.use_smooth = True
    ob = link(bpy.data.objects.new(name, cu))
    ob.location.z = z + depth / 2
    cu.materials.append(m)
    return ob


def rbox(c, size, r, m, seg=4, name="rbox"):
    bm = bmesh.new()
    bmesh.ops.create_cube(bm, size=1.0)
    for v in bm.verts:
        v.co = V((v.co.x * size[0], v.co.y * size[1], v.co.z * size[2]))
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    ob = link(bpy.data.objects.new(name, me))
    ob.location = c
    md = ob.modifiers.new("bev", "BEVEL")
    md.width = min(r, min(size) / 2 - 1e-4)
    md.segments = seg
    md.limit_method = "NONE"
    sub = ob.modifiers.new("sub", "SUBSURF")
    sub.levels = sub.render_levels = 1
    smooth(ob, 60)
    return setmat(ob, m)


def ball(c, r, m, scale=(1, 1, 1), name="ball"):
    bm = bmesh.new()
    bmesh.ops.create_uvsphere(bm, u_segments=48, v_segments=24, radius=r)
    me = bpy.data.meshes.new(name)
    bm.to_mesh(me)
    ob = link(bpy.data.objects.new(name, me))
    ob.location = c
    ob.scale = scale
    smooth(ob, 180)
    return setmat(ob, m)


def cyl(c, r, depth, m, rot=(0, 0, 0), r2=None, verts=48, bevel=0.0, name="cyl"):
    """A cylinder (or cone, with r2) along Z, then turned by rot (radians)."""
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


def capsule(p0, p1, r, m, name="capsule"):
    """A rod with round ends from p0 to p1 (3D points)."""
    p0, p1 = V(p0), V(p1)
    d = p1 - p0
    ob = tube([tuple(p0), tuple(p0 + d * 0.5), tuple(p1)], r, m, name=name)
    return ob


def tube(pts, r, m, name="tube", order=3):
    """A round tube along points (NURBS, so it passes near, not through,
    the middle ones). Ends are round caps."""
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
    sp.order_u = min(order, len(pts))
    sp.use_smooth = True
    ob = link(bpy.data.objects.new(name, cu))
    cu.materials.append(m)
    # round ends: a ball at each end
    ball(pts[0], r, m, name=name + "0").parent = ob
    ball(pts[-1], r, m, name=name + "1").parent = ob
    return ob


def blob(spheres, m, threshold=0.6, scale=(1, 1, 1), res=0.012, name="blob"):
    """Metaballs: (x, y, z, r) spheres melted into one soft body (clouds,
    animals, bodies)."""
    mb = bpy.data.metaballs.new(name)
    mb.resolution = res
    mb.render_resolution = res * 0.66
    mb.threshold = threshold
    for (x, y, z, r) in spheres:
        el = mb.elements.new()
        el.co = (x, y, z)
        el.radius = r
    ob = link(bpy.data.objects.new(name, mb))
    mb.materials.append(m)
    ob.scale = scale
    return ob


def text(s, m, size=0.4, depth=0.08, bevel=0.02, c=(0, 0, 0), bold=True, name="text"):
    """Chunky extruded letters in Blender's own font, centred on c."""
    cu = bpy.data.curves.new(name, "FONT")
    cu.body = s
    cu.size = size
    cu.extrude = max(0.0, depth / 2 - bevel)
    cu.bevel_depth = bevel
    cu.bevel_resolution = 4
    cu.offset = -bevel * 0.4
    cu.align_x = "CENTER"
    cu.align_y = "CENTER"
    cu.space_character = 1.05
    ob = link(bpy.data.objects.new(name, cu))
    ob.location = (c[0], c[1], c[2] + depth / 2)
    cu.materials.append(m)
    return ob


def group(objs, loc=(0, 0, 0), rot=(0, 0, 0), scale=1.0, name="toy"):
    """Parent objects to an empty; rot in degrees (X, Y, Z)."""
    root = link(bpy.data.objects.new(name, None))
    for o in objs:
        if o.parent is None:
            o.parent = root
    root.location = loc
    root.rotation_euler = [math.radians(a) for a in rot]
    root.scale = (scale, scale, scale)
    return root


def turn(root, *steps):
    """Rotate a group by matrices in order, e.g. turn(r, ("Z", 45), ("X", -50))."""
    m = Matrix.Identity(4)
    for axis, deg in steps:
        m = Matrix.Rotation(math.radians(deg), 4, axis) @ m
    root.rotation_euler = m.to_euler()
    return root


def tile(top, bottom, stripes=0):
    """The rounded square, 1.0 across, as a plastic slab, its top at z = 0."""
    h = 0.5
    pts = rounded_poly([(-h, -h), (h, -h), (h, h), (-h, h)], RADIUS, seg=16)
    return slab([pts], 0.12, 0.035, mat_grad(top, bottom, stripes=stripes), z=-0.12, res=6, name="tile")


def on_tile(objs):
    """Things that lie on the tile (lines of a pitch, a shadow) - no tilt."""
    return objs


def arc_band(cx, cy, r_in, r_out, a0, a1, n=24):
    """A ring sector (degrees a0..a1): radio waves, rainbows, dials."""
    outer = [(cx + r_out * math.cos(math.radians(a0 + (a1 - a0) * i / n)),
              cy + r_out * math.sin(math.radians(a0 + (a1 - a0) * i / n))) for i in range(n + 1)]
    inner = [(cx + r_in * math.cos(math.radians(a1 - (a1 - a0) * i / n)),
              cy + r_in * math.sin(math.radians(a1 - (a1 - a0) * i / n))) for i in range(n + 1)]
    return outer + inner


BOLT = [(0.06, 0.36), (-0.2, -0.02), (-0.02, -0.02), (-0.08, -0.36), (0.2, 0.06), (0.02, 0.06)]


def waves(cx, cy, m, n=2, r0=0.12, step=0.09, w=0.045, a0=-50, a1=50, depth=0.05, z=0.0):
    """Radio waves: n arcs (round tubes) around (cx, cy), from a0 to a1 degrees.
    depth is unused (kept so old calls work): a tube is as thick as it is wide."""
    out = []
    for i in range(n):
        r = r0 + i * step + w / 2
        pts = [(cx + r * math.cos(math.radians(a0 + (a1 - a0) * k / 8)),
                cy + r * math.sin(math.radians(a0 + (a1 - a0) * k / 8)), z + w / 2) for k in range(9)]
        out.append(tube(pts, w / 2, m, name="wave"))
    return out
