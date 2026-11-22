"""
Juegos de mesa - the boards and pieces of every game but chess (ajedrez.py):
draughts on 8 x 8 and 10 x 10, reversi, four in a row (standing, its frame
drawn over the discs), ludo and backgammon (their printed faces from
texturas.py), the dice, and each game's picture for the menu.

    python3 texturas.py
    $B -b -P tableros.py -- --set madera [--game damas,reversi,cuatro,ludo,backgammon,dados]
                                         [--only board,pieces,flat,thumb]
"""
import math
import os
import sys

import bpy

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mesa_common as mc  # noqa: E402

TEX = os.path.normpath(os.path.join(mc.HERE, '..', '..', 'assets', 'tex'))


def games():
    g = mc.arg('--game')
    return g.split(',') if g else ['damas', 'reversi', 'cuatro', 'ludo', 'backgammon', 'dados']


def image_mat(name, path, rough=0.5, clear=0.2):
    if name in mc.MATS:
        return mc.MATS[name]
    m, nt, b = mc._principled(name)
    tx = nt.nodes.new('ShaderNodeTexImage')
    tx.image = bpy.data.images.load(path)
    tc = nt.nodes.new('ShaderNodeTexCoord')
    nt.links.new(tc.outputs['UV'], tx.inputs['Vector'])
    nt.links.new(tx.outputs['Color'], b.inputs['Base Color'])
    b.inputs['Roughness'].default_value = rough
    b.inputs['Clearcoat'].default_value = clear
    mc.MATS[name] = m
    return m


def plane(name, w, h, m, loc):
    bpy.ops.mesh.primitive_plane_add(size=1.0, location=loc)
    o = bpy.context.active_object
    o.name = name
    o.scale = (w, h, 1)
    o.data.materials.append(m)
    return o


def frame_around(w, h, fr, width=0.5, t=0.34, inner=None):
    """a wooden frame around a w x h board whose top is z = 0, on the table"""
    cx, cy = w / 2, h / 2
    mc.box('fs', (w + 2 * width, width, t), fr, loc=(cx, -width / 2, -t / 2 + 0.06), bevel=0.05)
    mc.box('fn', (w + 2 * width, width, t), fr, loc=(cx, h + width / 2, -t / 2 + 0.06), bevel=0.05)
    mc.box('fw', (width, h, t), fr, loc=(-width / 2, cy, -t / 2 + 0.06), bevel=0.05)
    mc.box('fe', (width, h, t), fr, loc=(w + width / 2, cy, -t / 2 + 0.06), bevel=0.05)
    mc.box('under', (w, h, 0.2), fr, loc=(cx, cy, -0.12), bevel=0)
    mc.box('table', (80, 80, 0.2), mc.table(), loc=(cx, cy, -0.45), bevel=0)


def disc(name, r, h, m, rim=None, grooves=True):
    """a draughts or backgammon checker: a disc with rings on its face"""
    p = [(r * 0.0, 0), (r * 0.96, 0), (r, h * 0.12), (r, h * 0.88), (r * 0.96, h)]
    if grooves:
        p += [(r * 0.80, h), (r * 0.78, h * 0.86), (r * 0.74, h * 0.86), (r * 0.72, h), (r * 0.45, h),
              (r * 0.43, h * 0.9), (r * 0.40, h * 0.9), (r * 0.38, h)]
    p += [(0, h)]
    o = mc.lathe(name, p, m)
    return [o]


def group(objs, name='piece'):
    root = bpy.data.objects.new(name, None)
    mc.sc.collection.objects.link(root)
    for o in objs:
        if o.parent is None:
            o.parent = root
    return root


def sprite3d(name, make, S, w, h, ax, ay, E=None):
    mc.clear_objects()
    objs = make()
    group(objs)
    mc.render_sprite(name, objs, S, mc.E3D if E is None else E, w, h, ax, ay)


def sprite_flat(name, make, S, size):
    """from above, centred on its cell"""
    mc.clear_objects()
    objs = make()
    group(objs)
    mc.transparent(True)
    mc.aim(size, size, S, 90.0, (0, 0, 0))
    d = mc.out_dir()
    mc.render(os.path.join(d, name + '.png'))
    mc.write_json(os.path.join(d, name + '.json'), dict(S=S, E=90, w=size, h=size, ax=size // 2, ay=size // 2,
                                                       frames=[name + '.png']))


def thumb(name, centre, S, E=38.0, w=320, h=240):
    mc.transparent(False)
    mc.aim(w, h, S, E, centre)
    mc.render(os.path.join(mc.out_dir(), name + '.png'))


# ---- draughts -------------------------------------------------------------------------

def checker(m, king, S_unit=1.0):
    objs = disc('man', 0.40, 0.13, m)
    if king:
        top = disc('man2', 0.40, 0.13, m)
        top[0].location.z = 0.135
        objs += top
        gold = mc.mat('crown', '#d8a838', rough=0.25, metal=1.0)
        objs.append(mc.lathe('crown', [(0.0, 0.265), (0.16, 0.265), (0.17, 0.285), (0.0, 0.29)], gold))
    return objs


def damas():
    light, dark = mc.piece_mats('dm')
    for n, S, name in ((8, 78.0, 'dm8'), (10, 62.0, 'dm10')):
        if mc.only('board'):
            mc.clear_objects()
            mc.grid_board(name, n)
            mc.render_board(name + '_bg', S, mc.E3D, (n / 2, n / 2, 0))
        if mc.only('flat'):
            mc.clear_objects()
            mc.grid_board(name, n)
            mc.render_board(name + '_fl', S, 90.0, (n / 2, n / 2, 0))
        if mc.only('pieces') or mc.only('flat'):
            sz = int(S * 1.2) // 2 * 2
            for k, m, king in (('w', light, False), ('wk', light, True), ('b', dark, False), ('bk', dark, True)):
                if mc.only('pieces'):
                    sprite3d('%s_%s' % (name, k), lambda m=m, king=king: checker(m, king), S, sz, sz, sz // 2,
                             int(sz * 0.68))
                if mc.only('flat'):
                    sprite_flat('%s_%s_f' % (name, k), lambda m=m, king=king: checker(m, king), S, sz)
    if mc.only('thumb'):
        mc.clear_objects()
        mc.grid_board('dm8', 8)
        for sq in range(64):
            r, f = sq // 8, sq % 8
            if (r + f) % 2 or r in (3, 4):
                continue
            if (r, f) in ((2, 2), (5, 5)):
                continue
            root = group(checker(light if r < 3 else dark, (r, f) == (6, 2)))
            root.location = (f + 0.5, r + 0.5, 0)
        root = group(checker(light, True))
        root.location = (3.5, 4.5, 0)
        thumb('dm_th', (4, 3.7, 0.2), 52.0)


# ---- reversi -------------------------------------------------------------------------

def felt():
    s = mc.set_name()
    if s == 'marmol':
        return mc.marble('rv_felt', '#1e5a3a', '#2e7a4e', 1.0, rough=0.2)
    if s == 'vidrio':
        return mc.mat('rv_felt', '#1a5a40', rough=0.1, trans=0.2)
    if s == 'metal':
        return mc.metal('rv_felt', '#2a6a4a', 0.45)
    return mc.mat('rv_felt', '#1f6a40', rough=0.85)


def rv_board():
    f = felt()
    mc.grid_board('rv', 8, light=f, dark=f)
    line = mc.mat('rv_line', '#0c2a18', rough=0.8)
    for i in range(9):
        mc.box('lx', (8.0, 0.035, 0.01), line, loc=(4, i, 0.002), bevel=0)
        mc.box('ly', (0.035, 8.0, 0.01), line, loc=(i, 4, 0.002), bevel=0)
    for (x, y) in ((2, 2), (6, 2), (2, 6), (6, 6)):
        mc.cyl('dot', 0.07, 0.01, line, loc=(x, y, 0.0))


def rv_disc(top):
    s = mc.set_name()
    bk = mc.mat('rv_black', '#141414', rough=0.3, clear=0.5) if s != 'metal' else mc.metal('rv_black', '#2a2a2a', 0.3)
    wh = mc.mat('rv_white', '#f2f0ea', rough=0.3, clear=0.5) if s != 'metal' else mc.metal('rv_white', '#e0e0d8', 0.2)
    a, b = (bk, wh) if top == 'b' else (wh, bk)
    lo = mc.lathe('lo', [(0, 0), (0.37, 0), (0.38, 0.02), (0.38, 0.05), (0, 0.05)], b)
    hi = mc.lathe('hi', [(0, 0.05), (0.38, 0.05), (0.38, 0.08), (0.37, 0.10), (0, 0.10)], a)
    return [lo, hi]


def reversi():
    S = 78.0
    if mc.only('board'):
        mc.clear_objects()
        rv_board()
        mc.render_board('rv_bg', S, mc.E3D, (4, 4, 0))
    if mc.only('flat'):
        mc.clear_objects()
        rv_board()
        mc.render_board('rv_fl', S, 90.0, (4, 4, 0))
    sz = 94
    for k in ('b', 'w'):
        if mc.only('pieces'):
            sprite3d('rv_' + k, lambda k=k: rv_disc(k), S, sz, sz, sz // 2, int(sz * 0.66))
        if mc.only('flat'):
            sprite_flat('rv_%s_f' % k, lambda k=k: rv_disc(k), S, sz)
    if mc.only('thumb'):
        mc.clear_objects()
        rv_board()
        layout = ['........', '...b....', '..bbw...', '..bwwb..', '..wwbbw.', '...wb...', '....w...', '........']
        for r, row in enumerate(layout):
            for f, c in enumerate(row):
                if c in 'bw':
                    root = group(rv_disc(c))
                    root.location = (f + 0.5, 7 - r + 0.5, 0)
        thumb('rv_th', (4, 3.8, 0), 52.0)


# ---- four in a row: a standing frame, seen from the front ------------------------------------

C4S = 86.0


def c4_frame():
    s = mc.set_name()
    if s == 'madera':
        m = mc.wood('c4_frame', '#8a5a2a', '#5a3414', 1.6, ring=6.0)
    elif s == 'marmol':
        m = mc.marble('c4_frame', '#e8e2d8', '#8a8278', 1.0)
    elif s == 'vidrio':
        m = mc.glass('c4_frame', '#9ac8f0', 0.05)
    else:
        m = mc.metal('c4_frame', '#3a5a9a', 0.3)
    w, h = 7.0, 6.0
    plate = mc.box('plate', (w + 0.5, 0.26, h + 0.45), m, loc=(3.5, 0, 3.0 - 0.02), bevel=0.06)
    holes = bpy.data.collections.new('holes')
    mc.sc.collection.children.link(holes)
    for r in range(6):
        for c in range(7):
            bpy.ops.mesh.primitive_cylinder_add(radius=0.42, depth=1.0, location=(c + 0.5, 0, r + 0.5),
                                                rotation=(math.pi / 2, 0, 0))
            o = bpy.context.active_object
            for col in o.users_collection:
                col.objects.unlink(o)
            holes.objects.link(o)
            o.hide_render = True
    md = plate.modifiers.new('holes', 'BOOLEAN')
    md.operation = 'DIFFERENCE'
    md.operand_type = 'COLLECTION'
    md.collection = holes
    # feet
    mc.box('foot_l', (0.35, 1.6, 0.5), m, loc=(-0.15, 0, -0.45), bevel=0.05)
    mc.box('foot_r', (0.35, 1.6, 0.5), m, loc=(7.15, 0, -0.45), bevel=0.05)
    return plate


def c4_backdrop():
    mc.box('wall', (60, 0.2, 40), mc.table(), loc=(3.5, 3.0, 3.0), bevel=0)
    mc.box('floor', (60, 20, 0.2), mc.table(), loc=(3.5, 0, -0.8), bevel=0)


def c4_disc(color):
    s = mc.set_name()
    m = mc.mat('c4_' + color, '#d8281c' if color == 'r' else '#f0c020', rough=0.25, clear=0.6)
    if s == 'vidrio':
        m = mc.glass('c4_' + color, '#e83020' if color == 'r' else '#f8d040', 0.05)
    elif s == 'metal':
        m = mc.metal('c4_' + color, '#c8301c' if color == 'r' else '#e0b028', 0.3)
    o = disc('d', 0.40, 0.16, m)[0]
    o.rotation_euler = (math.pi / 2, 0, 0)
    o.location = (0, 0.08, 0)
    return [o]


def cuatro():
    S = C4S
    if mc.only('board'):
        mc.clear_objects()
        c4_backdrop()
        c4_frame()
        mc.render_board('c4_bg', S, 0.0, (3.5, 0, 3.0))
        # the frame alone, for over the discs
        mc.clear_objects()
        c4_frame()
        mc.transparent(True)
        mc.aim(mc.PIC, mc.PIC, S, 0.0, (3.5, 0, 3.0))
        d = mc.out_dir()
        mc.render(os.path.join(d, 'c4_over.png'))
        mc.write_json(os.path.join(d, 'c4_over.json'), dict(S=S, E=0, w=mc.PIC, h=mc.PIC, ax=mc.PIC // 2,
                                                           ay=mc.PIC // 2, frames=['c4_over.png']))
    sz = 92
    for k in ('r', 'y'):
        if mc.only('pieces'):
            mc.clear_objects()
            group(c4_disc(k))
            mc.transparent(True)
            mc.aim(sz, sz, S, 0.0, (0, 0, 0))
            d = mc.out_dir()
            mc.render(os.path.join(d, 'c4_%s.png' % k))
            mc.write_json(os.path.join(d, 'c4_%s.json' % k), dict(S=S, E=0, w=sz, h=sz, ax=sz // 2, ay=sz // 2,
                                                                 frames=['c4_%s.png' % k]))
    if mc.only('thumb'):
        mc.clear_objects()
        c4_backdrop()
        c4_frame()
        cols = {0: 'r', 1: 'y'}
        moves = [3, 3, 2, 4, 2, 2, 4, 5, 1, 4, 4, 3, 5]
        hgt = [0] * 7
        for i, c in enumerate(moves):
            root = group(c4_disc(cols[i % 2]))
            root.location = (c + 0.5, 0, hgt[c] + 0.5)
            hgt[c] += 1
        thumb('c4_th', (3.5, 0, 3.0), 38.0, E=0.0)


# ---- ludo ---------------------------------------------------------------------------------------

LDS = 42.0
LD_COL = ['#d0281e', '#2a9a3c', '#e8b818', '#2a5cc8']


def ld_board():
    m = image_mat('ld_face', os.path.join(TEX, 'ludo.png'), rough=0.45, clear=0.3)
    p = plane('face', 15, 15, m, (7.5, 7.5, 0.0))
    p.scale = (15, 15, 1)
    _, _, fr = mc.board_mats('ld')
    frame_around(15, 15, fr, 0.6)


def ld_token(i):
    s = mc.set_name()
    if s == 'vidrio':
        m = mc.glass('ld_%d' % i, LD_COL[i])
    elif s == 'metal':
        m = mc.metal('ld_%d' % i, LD_COL[i], 0.3)
    else:
        m = mc.mat('ld_%d' % i, LD_COL[i], rough=0.3, clear=0.6)
    prof = mc.smooth_profile([(0.36, 0), (0.36, 0.06), (0.30, 0.12), (0.16, 0.28), (0.11, 0.48), (0.17, 0.54),
                              (0.20, 0.64), (0.17, 0.74), (0.08, 0.80), (0.0, 0.81)], 3)
    return [mc.lathe('tok', [(0.36, 0)] + prof[1:], m)]


def ludo():
    S = LDS
    if mc.only('board'):
        mc.clear_objects()
        ld_board()
        mc.render_board('ld_bg', S, mc.E3D, (7.5, 7.5, 0))
    if mc.only('flat'):
        mc.clear_objects()
        ld_board()
        mc.render_board('ld_fl', S, 90.0, (7.5, 7.5, 0))
    for i, k in enumerate(('r', 'g', 'y', 'b')):
        if mc.only('pieces'):
            sprite3d('ld_' + k, lambda i=i: ld_token(i), S, 48, 64, 24, 50)
        if mc.only('flat'):
            sprite_flat('ld_%s_f' % k, lambda i=i: ld_token(i), S, 48)
    if mc.only('thumb'):
        mc.clear_objects()
        ld_board()
        spots = [(0, 2.5, 6.5), (0, 3.5, 2.5), (1, 8.5, 3.5), (1, 10.5, 2.5), (2, 12.5, 8.5), (2, 7.5, 12.5),
                 (3, 3.5, 10.5), (3, 6.5, 9.5), (0, 7.5, 0.5)]
        for (i, x, y) in spots:
            root = group(ld_token(i))
            root.location = (x, y, 0)
        thumb('ld_th', (7.5, 7.2, 0), 21.0, E=50.0)


# ---- backgammon -------------------------------------------------------------------------------

BGS = 50.0


def bg_board():
    m = image_mat('bg_face', os.path.join(TEX, 'backgammon.png'), rough=0.8, clear=0.0)
    plane('face', 15, 11, m, (7.5, 5.5, 0.0))
    _, _, fr = mc.board_mats('bg')
    frame_around(15, 11, fr, 0.5)
    # the bar, the tray's wall and the frame between the halves
    mc.box('bar', (1.0, 11.0, 0.16), fr, loc=(7.0, 5.5, 0.08), bevel=0.04)
    mc.box('tray_wall', (0.16, 11.0, 0.16), fr, loc=(13.55, 5.5, 0.08), bevel=0.03)
    mc.box('tray_mid', (1.4, 0.16, 0.16), fr, loc=(14.3, 5.5, 0.08), bevel=0.03)
    mc.box('left_edge', (0.5, 11.0, 0.16), fr, loc=(0.25, 5.5, 0.08), bevel=0.03)


def bg_checker(k):
    s = mc.set_name()
    if s == 'madera':
        m = mc.wood('bgc_' + k, '#efe2c4', '#d8c49a', 3.0, ring=5.0) if k == 'w' else \
            mc.wood('bgc_' + k, '#7a2216', '#4a0e08', 3.0, ring=5.0)
    else:
        light, dark = mc.piece_mats('bgc')
        m = light if k == 'w' else dark
    return disc('c', 0.42, 0.12, m)


def backgammon():
    S = BGS
    if mc.only('board'):
        mc.clear_objects()
        bg_board()
        mc.render_board('bg_bg', S, mc.E3D, (7.5, 5.5, 0))
        # upright the board lies turned a quarter (rot 1: its u away from us)
        root = bpy.data.objects.new('turn', None)
        mc.sc.collection.objects.link(root)
        root.location = (7.5, 5.5, 0)
        for o in list(bpy.data.objects):
            if o.type == 'MESH' and o.parent is None and o.name not in ('table',):
                o.parent = root
                o.location = (o.location.x - 7.5, o.location.y - 5.5, o.location.z)
        root.rotation_euler = (0, 0, math.pi / 2)
        mc.render_board('bg_bgp', S, mc.E3D, (7.5, 5.5, 0), rot=1)
    if mc.only('flat'):
        mc.clear_objects()
        bg_board()
        mc.render_board('bg_fl', S, 90.0, (7.5, 5.5, 0))
    sz = 56
    for k in ('w', 'b'):
        if mc.only('pieces'):
            sprite3d('bg_' + k, lambda k=k: bg_checker(k), S, sz, sz, sz // 2, int(sz * 0.66))
        if mc.only('flat'):
            sprite_flat('bg_%s_f' % k, lambda k=k: bg_checker(k), S, sz)
    if mc.only('thumb'):
        mc.clear_objects()
        bg_board()

        def px(i):
            return 13.0 - i if i <= 5 else 12.0 - i if i <= 11 else 1.0 + (i - 12) if i <= 17 else 8.0 + (i - 18)
        for (i, n, k) in ((23, 2, 'w'), (12, 5, 'w'), (7, 3, 'w'), (5, 5, 'w'), (0, 2, 'b'), (11, 5, 'b'),
                          (16, 3, 'b'), (18, 5, 'b')):
            for s in range(n):
                root = group(bg_checker(k))
                root.location = (px(i), 0.95 + s * 0.88 if i < 12 else 10.05 - s * 0.88, 0)
        thumb('bg_th', (7.5, 5.2, 0), 21.0, E=50.0)


# ---- dice ----------------------------------------------------------------------------------

def die():
    s = mc.set_name()
    body = mc.mat('die', '#f4efe4', rough=0.25, clear=0.6) if s != 'metal' else mc.metal('die', '#d8d8d0', 0.25)
    if s == 'vidrio':
        body = mc.glass('die', '#f0f0f0', 0.06)
    pip = mc.mat('pip', '#1a120c', rough=0.4)
    a = 0.66
    cube = mc.box('cube', (a, a, a), body, loc=(0, 0, a / 2), bevel=0.09)
    faces = {1: [(0, 0)], 2: [(-1, -1), (1, 1)], 3: [(-1, -1), (0, 0), (1, 1)], 4: [(-1, -1), (1, -1), (-1, 1), (1, 1)],
             5: [(-1, -1), (1, -1), (0, 0), (-1, 1), (1, 1)], 6: [(-1, -1), (1, -1), (-1, 0), (1, 0), (-1, 1), (1, 1)]}
    # +Z 1, -Z 6, +X 2, -X 5, +Y 3, -Y 4
    axes = {1: ('z', 1), 6: ('z', -1), 2: ('x', 1), 5: ('x', -1), 3: ('y', 1), 4: ('y', -1)}
    objs = [cube]
    k = 0.17
    for n, pts in faces.items():
        ax, sg = axes[n]
        for (u, v) in pts:
            if ax == 'z':
                loc = (u * k, v * k, a / 2 + sg * a / 2)
            elif ax == 'x':
                loc = (sg * a / 2, u * k, a / 2 + v * k)
            else:
                loc = (u * k, sg * a / 2, a / 2 + v * k)
            o = mc.lathe('pip', [(0.0, -0.05), (0.055, -0.035), (0.065, 0.0), (0.055, 0.035), (0.0, 0.05)], pip,
                         segs=16, loc=loc)
            o.parent = cube
            o.location = (loc[0], loc[1], loc[2] - a / 2)
            objs.append(o)
    return cube, objs


# turns that bring face n up (and a little round so it reads as a cube)
UP = {1: (0, 0, 0), 6: (180, 0, 0), 2: (0, -90, 0), 5: (0, 90, 0), 3: (90, 0, 0), 4: (-90, 0, 0)}


def dados():
    S = 46.0
    for flat in (False, True):
        if flat and not mc.only('flat'):
            continue
        if not flat and not mc.only('pieces'):
            continue
        mc.clear_objects()
        cube, objs = die()
        root = group([cube])
        frames = []
        for n in range(1, 7):
            def f(n=n, r=root, c=cube):
                rx, ry, rz = UP[n]
                c.rotation_euler = (math.radians(rx), math.radians(ry), math.radians(rz))
                c.location = (0, 0, 0.33)
                r.rotation_euler = (0, 0, math.radians(0 if flat else 24))
            frames.append(f)
        if flat:
            mc.transparent(True)
            mc.aim(48, 48, S, 90.0, (0, 0, 0))
            d = mc.out_dir()
            files = []
            for i, f in enumerate(frames):
                f()
                p = os.path.join(d, 'dado_f.%d.png' % i)
                mc.render(p)
                files.append(os.path.basename(p))
            mc.write_json(os.path.join(d, 'dado_f.json'), dict(S=S, E=90, w=48, h=48, ax=24, ay=24, frames=files))
        else:
            mc.render_sprite('dado', objs, S, mc.E3D, 64, 72, 32, 54, frames=frames)


def main():
    mc.setup()
    mc.studio(10.0)
    for g in games():
        dict(damas=damas, reversi=reversi, cuatro=cuatro, ludo=ludo, backgammon=backgammon, dados=dados)[g]()


main()
