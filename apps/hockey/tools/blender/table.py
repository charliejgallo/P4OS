"""
HOCKEY - a table, its two mallets and its puck, for one theme

    Blender -b -P table.py -- --theme bar [--samples 96] [--only pieces]

  assets/tables/<theme>/bg.png      the room and the table, 720 x 1280: the
                                    playing surface with its markings and its
                                    air holes, the rails with the goals' slots,
                                    the score windows, the cabinet
  assets/tables/<theme>/near.png    which pixels are the near end of the rails
  assets/tables/<theme>/far.png     and the far end: drawn again over a mallet
                                    or a puck they hide
  assets/tables/<theme>/m<side>_<size>.png   the mallets, side 0 (player 1,
                                    near) and 1, small, medium and large
  assets/tables/<theme>/puck.png    the puck
  assets/tables/<theme>/pieces.json the sprites' anchors (the centre of their
                                    base on the surface)

The themes: 'bar' (a classic table, wood and white plastic), 'neon' (a
dark table whose lines and rails glow) and 'hielo' (an ice rink).
"""
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bpy  # noqa: E402
import bmesh  # noqa: E402
import numpy as np  # noqa: E402
from mathutils import Vector  # noqa: E402

import hk_common as C  # noqa: E402

FX, FY, CR = C.FX, C.FY, C.CORNER_R
RH = C.RAIL_H
CAB_Z = -300.0

THEMES = {
    'bar': dict(
        surface='DCE4EA', holes='9AA6B0', lines=[('C8202A', 'center'), ('1F4FA8', 'crease')],
        rail='15171B', rail_rough=0.18, cab='6B3F22', cab_dark='3A2112', floor=('2A1E16', '4A3626'),
        plate='C9CED6', plate_metal=1.0, window='160404',
        mallet=('D3262B', '1E5BD8'), knob=('D3262B', '1E5BD8'), puck='F4B400', puck_ring='C98A00',
        world=((0.16, 0.13, 0.10), 0.35), sun=1.0, glow=False),
    'neon': dict(
        surface='07080F', holes='161A2A', lines=[('FF2BD6', 'far'), ('22E6FF', 'near'), ('B56BFF', 'center')],
        rail='0B0C14', rail_rough=0.25, cab='10111C', cab_dark='07070C', floor=('08080E', '131426'),
        plate='22E6FF', plate_metal=0.0, window='05050A',
        mallet=('0E2A33', '33102C'), knob=('22E6FF', 'FF2BD6'), puck='7CFF4F', puck_ring='3FA81F',
        world=((0.03, 0.02, 0.06), 0.3), sun=0.35, glow=True),
    'hielo': dict(
        surface='EEF5FA', holes='C7D5E0', lines=[('C8102E', 'center'), ('0B3D91', 'blue'), ('C8102E', 'circles'),
                                                ('7FB8E6', 'crease')],
        rail='F4F6F8', rail_rough=0.3, cab='1C3F7A', cab_dark='112650', floor=('1B2530', '2C3A48'),
        plate='C8102E', plate_metal=0.0, window='0A0C12',
        mallet=('C8102E', '0B3D91'), knob=('F2F2F2', 'F2F2F2'), puck='111214', puck_ring='2A2C30',
        world=((0.55, 0.62, 0.72), 0.3), sun=0.9, glow=False),
}

PX_MM = 2                       # the surface's image: two pixels a millimetre


# ---------------------------------------------------------------------------
# The playing surface
# ---------------------------------------------------------------------------

def surface_images(th):
    w, h = int(2 * FX * PX_MM), int(2 * FY * PX_MM)
    x = (np.arange(w)[None, :] + 0.5) / PX_MM - FX
    y = (np.arange(h)[:, None] + 0.5) / PX_MM - FY
    base = np.array(C.hexc(th['surface'])) * np.ones((h, w, 1))
    rng = np.random.default_rng(11)
    if th is THEMES['hielo']:
        # ice: cloudy, with the skates' scratches
        n = rng.random((h // 8 + 1, w // 8 + 1))
        n = np.kron(n, np.ones((8, 8)))[:h, :w]
        base *= (0.95 + 0.05 * n[..., None])
        for _ in range(260):
            x0, y0 = rng.uniform(-FX, FX), rng.uniform(-FY, FY)
            a = rng.uniform(0, math.pi)
            ln = rng.uniform(30, 160)
            t = np.linspace(0, ln, int(ln * PX_MM))
            px = ((x0 + np.cos(a) * t + FX) * PX_MM).astype(int)
            py = ((y0 + np.sin(a) * t + FY) * PX_MM).astype(int)
            ok = (px >= 0) & (px < w) & (py >= 0) & (py < h)
            base[py[ok], px[ok]] *= 0.93
    else:
        base *= (0.97 + 0.03 * rng.random((h, w, 1)))
    # the air holes: a grid, every 22 mm
    gx = np.abs(((x + FX) % 22.0) - 11.0)
    gy = np.abs(((y + FY) % 22.0) - 11.0)
    hole = (gx * gx + gy * gy) < 1.1 ** 2
    holes = np.array(C.hexc(th['holes']))
    if th is not THEMES['hielo']:
        base = np.where(hole[..., None], holes, base)
    emit = np.zeros((h, w, 3))
    r = np.sqrt(x * x + y * y)
    ay = np.abs(y)
    for col, what in th['lines']:
        c = np.array(C.hexc(col))
        m = np.zeros((h, w), bool)
        if what in ('center', 'near', 'far'):
            if what == 'center':
                m |= ay < 4.0
                m |= np.abs(r - 95.0) < 3.0
                m |= r < 7.0
            else:
                side = (y > 0) if what == 'near' else (y < 0)
                dg = np.sqrt(x * x + (ay - FY) ** 2)
                m |= side & (np.abs(dg - 150.0) < 3.0)
                m |= side & (np.abs(ay - (FY - 4.0)) < 2.0)
                m |= side & (np.abs(np.abs(x) - (FX - 4.0)) < 2.0)
                m |= side & (np.abs(r - 95.0) < 3.0)
        elif what == 'crease':
            dg = np.sqrt(x * x + (ay - FY) ** 2)
            if th is THEMES['hielo']:
                fill = dg < 110.0
                base = np.where(fill[..., None], base * 0.6 + c * 0.4, base)
                m |= np.abs(dg - 110.0) < 2.0
                c = np.array(C.hexc('C8102E'))
            else:
                m |= np.abs(dg - 150.0) < 3.0
        elif what == 'blue':
            m |= np.abs(ay - 175.0) < 6.0
        elif what == 'circles':
            for cx_, cy_ in ((-150, -340), (150, -340), (-150, 340), (150, 340)):
                d = np.sqrt((x - cx_) ** 2 + (y - cy_) ** 2)
                m |= np.abs(d - 62.0) < 2.0
                m |= d < 6.0
        base = np.where(m[..., None], c, base)
        if th['glow']:
            emit = np.where(m[..., None], c, emit)
    if th is THEMES['hielo']:
        # the centre line dashed white, as on a rink
        dash = (ay < 4.0) & (((x + FX) % 30.0) < 6.0)
        base = np.where(dash[..., None], np.array(C.hexc('F4F4F4')), base)
    img = np.concatenate([base, np.ones((h, w, 1))], -1)
    em = np.concatenate([emit, np.ones((h, w, 1))], -1)
    return C.image('surface', img), (C.image('surface_emit', em) if th['glow'] else None)


def surface(th):
    col, em = surface_images(th)
    rough = 0.18 if th is THEMES['hielo'] else 0.5
    m = C.mat_image('surface', col, rough=rough, spec=0.3, emit_img=em, emit_strength=3.5 if em else 0.0,
                    clearcoat=0.25 if th is THEMES['hielo'] else 0.0)
    bm = bmesh.new()
    uv = bm.loops.layers.uv.new('uv')
    vs = [bm.verts.new(C.W(px, py, 0)) for (px, py) in ((-FX, -FY), (FX, -FY), (FX, FY), (-FX, FY))]
    f = bm.faces.new(vs)
    for lp in f.loops:
        co = lp.vert.co
        gx, gy = co.x * 1000.0, -co.y * 1000.0
        lp[uv].uv = ((gx + FX) / (2 * FX), 1.0 - (gy + FY) / (2 * FY))
    return C.mesh_obj('surface', bm, m, smooth=False)


# ---------------------------------------------------------------------------
# The rails: one ring round the surface, its corners rounded inside, with
# a slot at each end where the goal is
# ---------------------------------------------------------------------------

def inner_point(t):
    """the rounded rectangle's edge, t in 0..1 round it (from the right
    rail's middle, counter-clockwise as seen from above with y down)"""
    sx, sy = FX - CR, FY - CR
    L = [2 * sy, math.pi * CR / 2, 2 * sx, math.pi * CR / 2, 2 * sy, math.pi * CR / 2, 2 * sx, math.pi * CR / 2]
    tot = sum(L)
    d = (t % 1.0) * tot
    # start at (FX, 0) going +y
    segs = [('line', (FX, -sy), (FX, sy)), ('arc', (sx, sy), 0.0), ('line', (sx, FY), (-sx, FY)),
            ('arc', (-sx, sy), math.pi / 2), ('line', (-FX, sy), (-FX, -sy)), ('arc', (-sx, -sy), math.pi),
            ('line', (-sx, -FY), (sx, -FY)), ('arc', (sx, -sy), 3 * math.pi / 2)]
    d += sy                     # (FX, 0) is half way along the first segment
    i = 0
    while d > L[i % 8]:
        d -= L[i % 8]
        i += 1
    kind = segs[i % 8]
    k = d / L[i % 8]
    if kind[0] == 'line':
        (x0, y0), (x1, y1) = kind[1], kind[2]
        return x0 + (x1 - x0) * k, y0 + (y1 - y0) * k
    (cx, cy), a0 = kind[1], kind[2]
    a = a0 + k * math.pi / 2
    return cx + CR * math.cos(a), cy + CR * math.sin(a)


def outer_point(px, py):
    """the outer rectangle's point straight out from an inner one"""
    if abs(px) >= FX - 1e-6 and abs(py) <= FY - CR:
        return math.copysign(C.OUT_X, px), py
    if abs(py) >= FY - 1e-6 and abs(px) <= FX - CR:
        return px, math.copysign(C.OUT_Y, py)
    # a corner: out along the diagonal to the rectangle's corner region
    cx, cy = math.copysign(FX - CR, px), math.copysign(FY - CR, py)
    dx, dy = px - cx, py - cy
    a = math.atan2(abs(dy), abs(dx))
    # blend between the side's and the end's outer edge
    k = a / (math.pi / 2)
    ox = math.copysign(C.OUT_X, px)
    oy = math.copysign(C.OUT_Y, py)
    if k < 0.5:
        return ox, cy + (oy - cy) * (k * 2)
    return cx + (ox - cx) * ((1 - k) * 2), oy


def rails(th, mats):
    N = 480
    bm = bmesh.new()
    inner, outer = [], []
    for i in range(N):
        px, py = inner_point(i / N)
        ox, oy = outer_point(px, py)
        inner.append(bm.verts.new(C.W(px, py, 0)))
        outer.append(bm.verts.new(C.W(ox, oy, 0)))
    faces = []
    for i in range(N):
        j = (i + 1) % N
        faces.append(bm.faces.new((inner[i], inner[j], outer[j], outer[i])))
    bmesh.ops.recalc_face_normals(bm, faces=faces)
    for f in faces:
        if f.normal.z > 0:
            f.normal_flip()
    r = bmesh.ops.extrude_face_region(bm, geom=faces)
    top = [e for e in r['geom'] if isinstance(e, bmesh.types.BMVert)]
    bmesh.ops.translate(bm, verts=top, vec=Vector((0, 0, RH / 1000.0)))
    bmesh.ops.recalc_face_normals(bm, faces=bm.faces[:])
    # which end each face belongs to, for the masks: 0 sides, 1 near, 2 far
    for f in bm.faces:
        cy = -f.calc_center_median().y * 1000.0
        f.material_index = 1 if cy > FY - CR else 2 if cy < -(FY - CR) else 0
    o = C.mesh_obj('rails', bm, None, smooth=True)
    for m in mats:
        o.data.materials.append(m)
    bv = o.modifiers.new('bevel', 'BEVEL')
    bv.width = 4.0 / 1000.0
    bv.segments = 3
    bv.limit_method = 'ANGLE'
    bv.angle_limit = math.radians(50)
    return o


def table(th):
    rail = C.mat_principled('rail', C.hexc(th['rail']), rough=th['rail_rough'], spec=0.6, clearcoat=0.5)
    rails_o = rails(th, [rail, rail, rail])
    surface(th)
    # the goals' slots: dark mouths in the end rails' inner faces
    dark = C.mat_principled('slot', (0.003, 0.003, 0.004), rough=0.9, spec=0.05)
    for s in (-1, 1):
        y0, y1 = (FY - 0.6, FY + 8) if s > 0 else (-FY - 8, -FY + 0.6)
        C.box('mouth%d' % s, -C.GOAL_X, y0, -0.5, C.GOAL_X, y1, C.SLOT_H, dark)
    # the goals' plates on the rails, over the slots, and their lights
    plates = []
    for s in (-1, 1):
        if th['glow']:
            col = C.hexc(th['knob'][0 if s > 0 else 1])
            pm = C.mat_emit('plate%d' % s, col, 4.0)
        else:
            pm = C.mat_principled('plate%d' % s, C.hexc(th['plate']), rough=0.25, metal=th['plate_metal'], spec=0.6)
        yc = s * (FY + 22)
        plates.append(C.box('plate%d' % s, -C.GOAL_X - 14, yc - 6, RH - 0.5, C.GOAL_X + 14, yc + 6, RH + 1.8, pm,
                            bevel=1.2))
    # neon: a glowing tube along the rails' inner edge, each half its side's colour
    if th['glow']:
        for s in (0, 1):
            col = C.hexc(th['knob'][s])
            tm = C.mat_emit('tube%d' % s, col, 6.0)
            cd = bpy.data.curves.new('tube%d' % s, 'CURVE')
            cd.dimensions = '3D'
            cd.bevel_depth = 3.5 / 1000.0
            cd.bevel_resolution = 3
            sp = cd.splines.new('POLY')
            n = 240
            sp.points.add(n)
            for i in range(n + 1):
                t = (0.0 if s == 0 else 0.5) + (i / n) * 0.5
                px, py = inner_point(t)
                v = C.W(px, py, RH + 1.5)
                sp.points[i].co = (v.x, v.y, v.z, 1.0)
            cu = bpy.data.objects.new('tube%d' % s, cd)
            C.link(cu)
            cu.data.materials.append(tm)
    # the score windows (left rail) and the clock's window and the pause bezel (right rail)
    win = C.mat_principled('window', C.hexc(th['window']), rough=0.3, spec=0.3, clearcoat=0.2)
    bez = C.mat_principled('bezel', C.hexc('B8BEC6'), rough=0.2, metal=1.0, spec=0.6)
    lx = -(FX + C.SIDE_W / 2)
    for yc in (-52.0, 52.0):
        C.box('frame%d' % yc, lx - 27, yc - 31, RH - 1, lx + 27, yc + 31, RH + 1.0, bez, bevel=1.5)
        C.box('win%d' % yc, lx - 24, yc - 28, RH, lx + 24, yc + 28, RH + 1.6, win, bevel=1.0)
    rx = FX + C.SIDE_W / 2
    C.box('clockf', rx - 27, -55 - 21, RH - 1, rx + 27, -55 + 21, RH + 1.0, bez, bevel=1.5)
    C.box('clockw', rx - 24, -55 - 18, RH, rx + 24, -55 + 18, RH + 1.6, win, bevel=1.0)
    ring = C.lathe('pause', [(0.0, RH), (17.0, RH), (22.0, RH + 1.0), (24.0, RH + 3.0), (22.0, RH + 4.5),
                             (18.0, RH + 4.0), (16.0, RH + 2.0), (0.0, RH + 2.0)], bez, x=rx, y=55.0)
    # the cabinet, and the room's floor
    if th is THEMES['neon']:
        cab = C.mat_principled('cab', C.hexc(th['cab']), rough=0.35, spec=0.5, clearcoat=0.6)
    else:
        cab = C.mat_wood('cab', th['cab'], th['cab_dark'], scale=1.6) if th is THEMES['bar'] else \
            C.mat_principled('cab', C.hexc(th['cab']), rough=0.35, spec=0.5, clearcoat=0.5)
    C.box('cabinet', -C.OUT_X - 4, -C.OUT_Y - 4, CAB_Z, C.OUT_X + 4, C.OUT_Y + 4, -0.2, cab, bevel=6, seg=3)
    # a trim along the cabinet's top edge
    trim = C.mat_principled('trim', C.hexc('C9CED6') if th is not THEMES['neon'] else C.hexc('22E6FF'),
                            rough=0.2, metal=1.0 if th is not THEMES['neon'] else 0.0, spec=0.6,
                            emit=((C.hexc('22E6FF'), 2.0) if th is THEMES['neon'] else None))
    C.box('trim', -C.OUT_X - 5, C.OUT_Y + 1, -18, C.OUT_X + 5, C.OUT_Y + 5.5, -10, trim, bevel=1.5)
    fl = C.mat_wood('floor', th['floor'][1], th['floor'][0], scale=0.6) if th is THEMES['bar'] else \
        C.mat_principled('floor', C.hexc(th['floor'][1]), rough=0.6, spec=0.3)
    C.box('floor', -3000, -3000, -820, 3000, 3000, -800, fl)
    return rails_o, plates


def lights(th):
    def sun(name, rot, energy, col):
        ld = bpy.data.lights.new(name, 'SUN')
        ld.energy = energy * th['sun']
        ld.color = col
        ld.angle = math.radians(10)
        o = bpy.data.objects.new(name, ld)
        o.rotation_euler = rot
        C.sc.collection.objects.link(o)
    sun('key', (math.radians(18), math.radians(-8), 0), 0.8, (1.0, 0.96, 0.9))
    sun('fill', (math.radians(-35), math.radians(10), 0), 0.15, (0.85, 0.9, 1.0))
    C.world(*th['world'])
    # a lamp over the table: its highlight on the glossy parts
    C.area('lamp', (0, -60, 1600), (800, 1300), 14.0 * th['sun'], (1.0, 0.95, 0.88))


# ---------------------------------------------------------------------------
# The mallets and the puck
# ---------------------------------------------------------------------------

def mallet_profile(R):
    H = 46.0 + 0.3 * R
    return H, [(0.0, 0.0), (R - 2.0, 0.0), (R, 2.0), (R, 13.0), (R - 1.0, 15.5), (R - 3.5, 17.0), (0.82 * R, 17.0),
               (0.76 * R, 15.0), (0.66 * R, 15.0), (0.58 * R, 16.5), (0.4 * R, 19.5), (0.31 * R, 23.0),
               (0.29 * R, H - 19.0), (0.34 * R, H - 15.0), (0.45 * R, H - 11.0), (0.47 * R, H - 7.0),
               (0.43 * R, H - 2.8), (0.3 * R, H - 0.5), (0.0, H)]


def puck_profile():
    R, H = C.PUCK_R, C.PUCK_H + (2.0 if THEME == 'hielo' else 0.0)
    if THEME == 'hielo':
        return H, [(0.0, 0.0), (R - 1.5, 0.0), (R, 1.5), (R, H - 1.5), (R - 1.5, H), (0.0, H)]
    return H, [(0.0, 0.0), (R - 1.0, 0.0), (R, 1.0), (R, H - 1.0), (R - 1.0, H), (0.86 * R, H), (0.82 * R, H - 0.9),
               (0.62 * R, H - 0.9), (0.58 * R, H), (0.0, H)]


def sprite(path, R, H):
    """renders what is in the scene at the origin, framed round its base"""
    top = -(R * C.COS_T + H * C.SIN_T) - 6
    bot = R * C.COS_T + 6
    w = int(math.ceil(2 * R + 12))
    w += w & 1
    h = int(math.ceil(bot - top))
    h += h & 1
    cy = C.CY + (top + bot) / 2
    C.camera(C.CX, cy, w, h)
    C.render(path)
    return dict(w=w, h=h, anchor=[w // 2, int(round(C.CY - (cy - h / 2)))])


def pieces(th, out):
    info = {}
    for s in (0, 1):
        for size, R in C.MALLETS.items():
            C.setup(samples=SAMPLES, transparent=True)
            lights(th)
            H, prof = mallet_profile(R)
            if th['glow']:
                body = C.mat_principled('body', C.hexc(th['mallet'][s]), rough=0.15, spec=0.8, clearcoat=1.0)
                knob = C.mat_emit('knob', C.hexc(th['knob'][s]), 5.0)
            else:
                body = C.mat_principled('body', C.hexc(th['mallet'][s]), rough=0.22, spec=0.6, clearcoat=0.6)
                knob = C.mat_principled('knob', C.hexc(th['knob'][s]), rough=0.25, spec=0.6, clearcoat=0.6)
            o = C.lathe('mallet', prof, body)
            o.data.materials.append(knob)
            # the knob's faces take the second material
            for p in o.data.polygons:
                if p.center.z * 1000.0 > 22.0:
                    p.material_index = 1
            if th['glow']:
                ring = C.mat_emit('ring', C.hexc(th['knob'][s]), 6.0)
                C.lathe('ring', [(R + 0.4, 5.0), (R + 1.6, 6.0), (R + 1.6, 9.0), (R + 0.4, 10.0)], ring)
            name = 'm%d_%s' % (s, size)
            info[name] = sprite(os.path.join(out, name + '.png'), R + 2.0, H)
    C.setup(samples=SAMPLES, transparent=True)
    lights(th)
    H, prof = puck_profile()
    if th['glow']:
        pm = C.mat_emit('puck', C.hexc(th['puck']), 3.0)
    elif THEME == 'hielo':
        pm = C.mat_principled('puck', C.hexc(th['puck']), rough=0.55, spec=0.3)
    else:
        pm = C.mat_principled('puck', C.hexc(th['puck']), rough=0.3, spec=0.5, clearcoat=0.4)
    C.lathe('puck', prof, pm)
    info['puck'] = sprite(os.path.join(out, 'puck.png'), C.PUCK_R + 1.0, H)
    return info


def mask_mat(which):
    """white where a surface lies at that end of the table, held out elsewhere"""
    m = bpy.data.materials.new('mask_' + which)
    m.use_nodes = True
    nt = m.node_tree
    for n in list(nt.nodes):
        nt.nodes.remove(n)
    geo = nt.nodes.new('ShaderNodeNewGeometry')
    sep = nt.nodes.new('ShaderNodeSeparateXYZ')
    nt.links.new(geo.outputs['Position'], sep.inputs[0])
    cmp_ = nt.nodes.new('ShaderNodeMath')
    # Blender's Y is -y: the near end is Y < -(FY - CR), the far one Y > FY - CR
    lim = (FY - CR - 1.0) / 1000.0
    if which == 'near':
        cmp_.operation = 'LESS_THAN'
        nt.links.new(sep.outputs['Y'], cmp_.inputs[0])
        cmp_.inputs[1].default_value = -lim
    else:
        cmp_.operation = 'GREATER_THAN'
        nt.links.new(sep.outputs['Y'], cmp_.inputs[0])
        cmp_.inputs[1].default_value = lim
    em = nt.nodes.new('ShaderNodeEmission')
    em.inputs[0].default_value = (1, 1, 1, 1)
    hold = nt.nodes.new('ShaderNodeHoldout')
    mix = nt.nodes.new('ShaderNodeMixShader')
    nt.links.new(cmp_.outputs[0], mix.inputs[0])
    nt.links.new(hold.outputs[0], mix.inputs[1])
    nt.links.new(em.outputs[0], mix.inputs[2])
    out = nt.nodes.new('ShaderNodeOutputMaterial')
    nt.links.new(mix.outputs[0], out.inputs[0])
    return m


def masks(out, rails_o, plates):
    """near.png and far.png: the ends of the rails alone, white on nothing"""
    keep = {rails_o.name, plates[0].name, plates[1].name}
    for o in C.sc.objects:
        if o.type in ('MESH', 'CURVE') and o.name not in keep:
            o.hide_render = True
    C.sc.render.film_transparent = True
    C.sc.cycles.samples = 16
    for which in ('near', 'far'):
        mm = mask_mat(which)
        for o in (rails_o, plates[0], plates[1]):
            for i in range(len(o.data.materials)):
                o.data.materials[i] = mm
        C.render(os.path.join(out, which + '.png'))


THEME = C.arg('--theme', 'bar')
SAMPLES = int(C.arg('--samples', '96'))


def main():
    th = THEMES[THEME]
    out = os.path.join(C.ASSETS, 'tables', THEME)
    os.makedirs(out, exist_ok=True)
    only = C.arg('--only', '')
    info = {}
    if only in ('', 'table'):
        C.setup(samples=SAMPLES, transparent=False)
        lights(th)
        rails_o, plates = table(th)
        C.camera(C.CX, C.SCREEN_H / 2, C.SCREEN_W, C.SCREEN_H)
        C.render(os.path.join(out, 'bg.png'))
        masks(out, rails_o, plates)
    if only in ('', 'pieces'):
        info = pieces(th, out)
        with open(os.path.join(out, 'pieces.json'), 'w') as f:
            json.dump(info, f, indent=1)


main()
