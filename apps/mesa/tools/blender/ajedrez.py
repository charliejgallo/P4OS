"""
Juegos de mesa - chess: the board, the pieces, the flat view, the menu's
picture. Staunton proportions: the king a square and two thirds tall.

    $B -b -P ajedrez.py -- --set madera [--only board,pieces,flat,thumb]
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mesa_common as mc  # noqa: E402

S = 78.0
SPR_W, SPR_H, SPR_AX, SPR_AY = 120, 150, 60, 124
FLAT_S = 52.0


def profile(kind):
    P = {
        'p': [(0.33, 0), (0.33, 0.05), (0.30, 0.09), (0.24, 0.12), (0.16, 0.16), (0.12, 0.40), (0.18, 0.44),
              (0.18, 0.47), (0.11, 0.50), (0.17, 0.56), (0.19, 0.64), (0.17, 0.72), (0.11, 0.78), (0.0, 0.80)],
        'r': [(0.36, 0), (0.36, 0.06), (0.32, 0.10), (0.26, 0.14), (0.21, 0.20), (0.19, 0.62), (0.25, 0.66),
              (0.26, 0.70), (0.24, 0.72), (0.24, 0.88), (0.0, 0.88)],
        'b': [(0.35, 0), (0.35, 0.06), (0.31, 0.10), (0.24, 0.14), (0.15, 0.20), (0.11, 0.60), (0.19, 0.64),
              (0.19, 0.67), (0.12, 0.70), (0.16, 0.78), (0.18, 0.88), (0.15, 0.98), (0.09, 1.06), (0.04, 1.10),
              (0.07, 1.13), (0.06, 1.17), (0.0, 1.19)],
        'q': [(0.38, 0), (0.38, 0.06), (0.34, 0.10), (0.27, 0.15), (0.17, 0.22), (0.12, 0.80), (0.21, 0.84),
              (0.21, 0.87), (0.13, 0.90), (0.16, 1.00), (0.23, 1.16), (0.20, 1.20), (0.10, 1.24), (0.0, 1.26)],
        'k': [(0.38, 0), (0.38, 0.06), (0.34, 0.10), (0.27, 0.15), (0.17, 0.22), (0.13, 0.86), (0.22, 0.90),
              (0.22, 0.93), (0.14, 0.96), (0.17, 1.06), (0.22, 1.22), (0.20, 1.27), (0.12, 1.30), (0.0, 1.31)],
        'n': [(0.36, 0), (0.36, 0.06), (0.32, 0.10), (0.25, 0.14), (0.24, 0.20), (0.22, 0.24), (0.0, 0.24)],
    }
    pts = P[kind]
    base = pts[:3]
    return base + mc.smooth_profile(pts[2:], 3)[1:]


def make_piece(kind, m):
    """the piece of 'kind' at the origin; its parts"""
    objs = [mc.lathe('body', profile(kind), m)]
    if kind == 'r':
        for i in range(5):
            a = 2 * math.pi * i / 5
            objs.append(mc.box('merlon', (0.13, 0.09, 0.12), m, loc=(0.19 * math.cos(a), 0.19 * math.sin(a), 0.93),
                               rot=(0, 0, math.degrees(a) + 90), bevel=0.012))
    elif kind == 'b':
        # the mitre's cut
        cut = mc.box('cut', (0.05, 0.6, 0.12), None, loc=(0.06, 0, 0.96), rot=(0, 35, 0), bevel=0)
        md = objs[0].modifiers.new('slit', 'BOOLEAN')
        md.operation = 'DIFFERENCE'
        md.object = cut
        cut.hide_render = True
    elif kind == 'q':
        for i in range(8):
            a = 2 * math.pi * i / 8
            o = mc.lathe('pearl', [(0.0, -0.045), (0.035, -0.035), (0.045, 0.0), (0.035, 0.035), (0.0, 0.045)], m,
                         segs=16, loc=(0.20 * math.cos(a), 0.20 * math.sin(a), 1.2))
            objs.append(o)
        objs.append(mc.lathe('ball', [(0.0, -0.06), (0.05, -0.04), (0.065, 0.0), (0.05, 0.04), (0.0, 0.06)], m,
                             segs=24, loc=(0, 0, 1.31)))
    elif kind == 'k':
        objs.append(mc.box('cross_v', (0.07, 0.07, 0.30), m, loc=(0, 0, 1.44), bevel=0.012))
        objs.append(mc.box('cross_h', (0.22, 0.07, 0.07), m, loc=(0, 0, 1.48), bevel=0.012))
    elif kind == 'n':
        head = [(-0.24, 0.20), (0.20, 0.20), (0.19, 0.34), (0.10, 0.48), (0.18, 0.56), (0.30, 0.64), (0.34, 0.72),
                (0.30, 0.79), (0.18, 0.82), (0.08, 0.86), (0.04, 0.98), (-0.02, 0.94), (-0.06, 1.00), (-0.11, 0.90),
                (-0.20, 0.78), (-0.26, 0.60), (-0.28, 0.40)]
        h = mc.extrude('head', head, 0.34, m, bevel=0.07)
        objs.append(h)
        for side in (-1, 1):
            e = mc.lathe('eye', [(0.0, -0.025), (0.025, 0.0), (0.0, 0.025)], mc.mat('eye', '#101010', 0.2), segs=12,
                         loc=(0.13, side * 0.175, 0.74))
            objs.append(e)
    return objs


def group(objs):
    root = mc.bpy.data.objects.new('piece', None)
    mc.sc.collection.objects.link(root)
    for o in objs:
        if o.parent is None:
            o.parent = root
    return root


def pieces():
    light, dark = mc.piece_mats('aj')
    for side, m in (('w', light), ('b', dark)):
        for kind in 'pnbrqk':
            mc.clear_objects()
            objs = make_piece(kind, m)
            root = group(objs)
            name = 'aj_%s%s' % (side, kind)
            if kind == 'n':
                # frame 0 looks right (towards the board's middle from the left), 1 left; both a little towards us
                def f0(r=root):
                    r.rotation_euler = (0, 0, math.radians(-12))

                def f1(r=root):
                    r.rotation_euler = (0, 0, math.radians(192))
                mc.render_sprite(name, objs, S, mc.E3D, SPR_W, SPR_H, SPR_AX, SPR_AY, frames=[f0, f1])
            else:
                mc.render_sprite(name, objs, S, mc.E3D, SPR_W, SPR_H, SPR_AX, SPR_AY)
            # the flat view's: from the front, smaller, centred on the cell
            if kind == 'n':
                root.rotation_euler = (0, 0, math.radians(0 if side == 'w' else 180))
            else:
                root.rotation_euler = (0, 0, 0)
            mc.transparent(True)
            mc.aim(80, 96, FLAT_S, 8.0, (0, 0, 0.0), 40, 90)
            d = mc.out_dir()
            mc.render(os.path.join(d, name + '_f.png'))
            mc.write_json(os.path.join(d, name + '_f.json'), dict(S=FLAT_S, E=8, w=80, h=96, ax=40, ay=50,
                                                                     frames=[name + '_f.png']))


START = ['rnbqkbnr', 'pppppppp', '', '', '', '', 'PPPPPPPP', 'RNBQKBNR']


def thumb():
    mc.clear_objects()
    mc.grid_board('aj')
    light, dark = mc.piece_mats('aj')
    pos = {(4, 3): 'P', (3, 4): 'p', (5, 2): 'N', (2, 5): 'n', (2, 3): 'B', (4, 7): 'k', (4, 0): 'K', (3, 0): 'Q',
           (3, 7): 'q', (0, 0): 'R', (7, 0): 'R', (0, 7): 'r', (7, 7): 'r', (0, 1): 'P', (1, 1): 'P', (5, 1): 'P',
           (6, 1): 'P', (7, 1): 'P', (0, 6): 'p', (1, 6): 'p', (5, 6): 'p', (6, 6): 'p', (7, 6): 'p', (2, 7): 'b',
           (6, 7): 'n', (6, 0): 'N', (2, 0): 'B'}
    for (f, r), k in pos.items():
        objs = make_piece(k.lower(), light if k.isupper() else dark)
        root = group(objs)
        root.location = (f + 0.5, r + 0.5, 0)
        if k.lower() == 'n':
            root.rotation_euler = (0, 0, math.radians(-30 if f < 4 else 210))
    mc.transparent(False)
    mc.aim(320, 240, 52.0, 38.0, (4.0, 3.6, 0.3))
    mc.render(os.path.join(mc.out_dir(), 'aj_th.png'))


def main():
    mc.setup()
    mc.studio()
    if mc.only('board'):
        mc.clear_objects()
        mc.grid_board('aj')
        mc.render_board('aj_bg', S, mc.E3D, (4, 4, 0))
    if mc.only('flat'):
        mc.clear_objects()
        mc.grid_board('aj')
        mc.render_board('aj_fl', S, 90.0, (4, 4, 0))
    if mc.only('pieces'):
        pieces()
    if mc.only('thumb'):
        thumb()


main()
