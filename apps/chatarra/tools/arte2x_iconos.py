#!/usr/bin/env python3
"""Chatarra's UI icons at 2x (ASSETS.md): 24x24 drawings of the 12x12 icons
in main/ch_ui.c's ICONOS[], written to main/ch_ui2x.inc.

    python3 tools/arte2x_iconos.py                  # writes main/ch_ui2x.inc
    python3 tools/arte2x_iconos.py --hoja out.png   # and a sheet: 1x | 2x | sizes

An icon is drawn with ch_blit2m(..., esc) and covers exactly the 12*esc units
the 1x icon covered, so nothing around it moves. Every drawing keeps its 1x
icon's silhouette and colours and adds what twelve pixels could not hold: a
glint, rivets, a thread, a label. Light from the top left; outlines in k for
machines and in the material's darkest tone for wood, rust and gold-free
materials; three or four tones per surface.
"""
import math
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from lienzo import Lienzo

AQUI = os.path.dirname(os.path.abspath(__file__))
MAIN = os.path.join(AQUI, '..', 'main')
N = 24


# ---------------------------------------------------------------- helpers
def ic():
    return Lienzo(N, N)


def dentro(L, x, y, region):
    return L.get(x, y) in region


def borde(L, region, luz, sombra, luz2=None, sombra2=None):
    """The block's bevel: top edge (and left, softer) lit, bottom (and right)
    in shade. Light from the top left."""
    L.bisel(set(region), luz, sombra, luz2 if luz2 else luz, sombra2 if sombra2 else sombra)


def volumen(L, region, cx, cy, rx, ry, tonos, sesgo=0.0):
    """A rounded body: each pixel of the region takes a tone of `tonos`
    (light to dark) by where it sits, lit from the top left."""
    n = len(tonos)
    cambios = []
    for y in range(L.h):
        for x in range(L.w):
            if L.p[y][x] not in region:
                continue
            nx = (x + 0.5 - cx) / rx
            ny = (y + 0.5 - cy) / ry
            v = (nx * 0.55 + ny * 0.85) * 0.5 + 0.5 + sesgo   # 0 lit .. 1 dark
            i = max(0, min(n - 1, int(v * n)))
            cambios.append((x, y, tonos[i]))
    for x, y, c in cambios:
        L.p[y][x] = c


def aro_luz(L, region, cx, cy, claro, oscuro, umbral=0.35):
    """The rim of a round thing: its outer pixels facing the light get
    `claro`, the ones facing away `oscuro`."""
    cambios = []
    for y in range(L.h):
        for x in range(L.w):
            if L.p[y][x] not in region:
                continue
            fuera = any(L.get(x + dx, y + dy) not in region
                        for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
            if not fuera:
                continue
            dx, dy = x + 0.5 - cx, y + 0.5 - cy
            r = math.hypot(dx, dy) or 1
            d = -(dx + dy) / (r * math.sqrt(2))
            if d > umbral:
                cambios.append((x, y, claro))
            elif d < -umbral:
                cambios.append((x, y, oscuro))
    for x, y, c in cambios:
        L.p[y][x] = c


def mascara(L, filas, c, dx=0, dy=0, marca='#'):
    for y, f in enumerate(filas):
        for x, ch in enumerate(f):
            if ch == marca:
                L.set(x + dx, y + dy, c)


def anillo(L, cx, cy, r_out, r_in, c, sobre=None):
    for y in range(L.h):
        for x in range(L.w):
            d = math.hypot(x + 0.5 - cx, y + 0.5 - cy)
            if r_in < d <= r_out:
                if sobre is None or L.get(x, y) in sobre:
                    L.set(x, y, c)


def barra(L, x0, y0, x1, y1, ancho, c):
    """A thick straight stroke from (x0, y0) to (x1, y1)."""
    ax, ay = x1 - x0, y1 - y0
    lon = math.hypot(ax, ay) or 1
    for y in range(L.h):
        for x in range(L.w):
            px, py = x + 0.5 - x0, y + 0.5 - y0
            t = (px * ax + py * ay) / (lon * lon)
            if 0 <= t <= 1:
                d = abs(px * ay - py * ax) / lon
                if d <= ancho / 2:
                    L.set(x, y, c)


def redondear(L, x, y, w, h, fondo='.'):
    """Takes the four corner pixels off a rectangle."""
    for xx, yy in ((x, y), (x + w - 1, y), (x, y + h - 1), (x + w - 1, y + h - 1)):
        L.set(xx, yy, fondo)


# ---------------------------------------------------------------- the menu
def taller():
    """A hex nut seen from the front: the town is Villa Tuerca. The chamfer
    ring, the threaded hole and its lit inner wall."""
    L = ic()
    cx = cy = 12
    pts = [(cx + 11.4 * math.cos(math.radians(a)), cy + 11.4 * math.sin(math.radians(a)))
           for a in range(0, 360, 60)]
    L.poli(pts, 'g')
    # the six flats: the upper-left ones catch the light
    for y in range(N):
        for x in range(N):
            if L.get(x, y) != 'g':
                continue
            a = math.degrees(math.atan2(y + 0.5 - cy, x + 0.5 - cx)) % 360
            f = int(a // 60)          # 0 lower right .. 5 upper right
            L.set(x, y, {0: '>', 1: 'D', 2: 'g', 3: '<', 4: 'G', 5: 'g'}[f])
    L.disco(cx, cy, 8.6, '<')         # the chamfered face
    aro_luz(L, '<', cx, cy, 'w', 'g', 0.3)
    L.disco(cx, cy, 4.8, 'K')         # the hole
    for y in range(N):                # its inner wall, lit on the far side
        for x in range(N):
            dx, dy = x + 0.5 - cx, y + 0.5 - cy
            d = math.hypot(dx, dy)
            if 3.2 < d <= 4.8 and dx + dy > 1.5:
                L.set(x, y, 'D' if d > 4 else 'd')
            elif d <= 3.2:
                L.set(x, y, 'x')
    for r, c in ((2.2, 'd'),):        # a turn of the thread down in it
        for a in range(10, 120, 10):
            L.set(cx - 0.5 + r * math.cos(math.radians(a)), cy - 0.5 + r * math.sin(math.radians(a)), c)
    L.set(cx - 3, cy - 3, 'k'); L.set(cx - 4, cy - 2, 'k'); L.set(cx - 2, cy - 4, 'k')
    L.set(8, 6, 'w'); L.set(7, 7, 'w'); L.set(9, 6, 'G')    # the glint
    L.contorno('k')
    return L


def objetos():
    """The bag: a leather satchel with two handles, a flap with stitches and
    a brass buckle."""
    L = ic()
    for y in range(8):                # the handle, one arch
        for x in range(N):
            dx, dy = x + 0.5 - 12, y + 0.5 - 8
            if (dx / 7.5) ** 2 + (dy / 7) ** 2 <= 1 and (dx / 4.6) ** 2 + (dy / 4.2) ** 2 > 1:
                L.set(x, y, 'J' if dy > -5.2 and (dx / 6.2) ** 2 + (dy / 5.8) ** 2 > 1 else 'j')
    L.hl(9, 1, 4, '0'); L.set(7, 2, '0'); L.set(8, 2, '0')
    L.rect(0, 7, N, 17, '.')
    L.rect(2, 7, 20, 15, 'j')         # the body
    L.rect(3, 21, 18, 1, 'j')
    redondear(L, 2, 7, 20, 15)
    L.set(3, 21, '.'); L.set(20, 21, '.')
    volumen(L, 'j', 10, 12, 12, 12, '0jjJ', -0.05)
    L.rect(2, 7, 20, 7, '0')          # the flap
    for x in range(3, 21):
        yb = 13 + (1 if 6 <= x <= 17 else 0) + (1 if 9 <= x <= 14 else 0)
        L.vl(x, 7, yb - 7, 'j')
        L.set(x, yb, 'J')
        L.set(x, yb + 1, '!') if 9 <= x <= 14 else None
    L.hl(3, 7, 18, '0'); L.vl(2, 8, 5, '0')
    L.set(2, 7, '.'); L.set(21, 7, '.')
    for x in range(4, 20, 2):         # the stitches along the flap
        L.set(x, 8, '6')
    L.rect(10, 13, 4, 4, 'y')         # the buckle
    L.hl(10, 13, 4, 'W'); L.vl(13, 13, 4, 'Y'); L.hl(10, 16, 4, 'Y')
    L.rect(11, 14, 2, 2, 'X')
    L.hl(5, 20, 14, 'J')              # the seam at the foot
    L.contorno('!')
    return L


def equipo():
    """The roster: three robots, each with its portrait and its bar."""
    L = ic()
    llenos = (12, 9, 6)
    for i, y in enumerate((1, 9, 17)):
        L.rect(1, y, 7, 7, 'k')                       # the portrait
        L.rect(2, y + 1, 5, 5, 'x')
        L.rect(2, y + 2, 5, 4, 'g')
        L.hl(2, y + 2, 5, 'G'); L.vl(6, y + 3, 3, '>')
        L.set(3, y + 3, 'c'); L.set(5, y + 3, 'c')    # its eyes
        L.hl(3, y + 5, 3, 'D')
        L.set(4, y + 1, 'r' if i == 0 else ('y' if i == 1 else 'v'))
        L.rect(9, y + 1, 14, 5, 'k')                  # the bar
        L.rect(10, y + 2, 12, 3, 'x')
        n = llenos[i]
        L.rect(10, y + 2, n, 3, 'c')
        L.hl(10, y + 2, n, '['); L.hl(10, y + 4, n, 'C')
        L.hl(10 + n, y + 4, 12 - n, 'K')
    return L


def registro():
    """The log: a page with a dog-ear, a margin and three ticked lines."""
    L = ic()
    L.rect(3, 1, 18, 22, 'q')
    L.poli([(15, 1), (21, 1), (21, 7)], '.')          # the dog-ear's hole
    L.poli([(15, 1), (15, 7), (21, 7)], 'Q')          # the dog-ear
    L.linea(15, 1, 20, 6, ')')
    L.hl(4, 1, 11, '('); L.vl(3, 1, 22, '(')
    L.hl(4, 22, 17, 'Q'); L.vl(20, 8, 15, 'Q')
    L.vl(7, 2, 20, '_')                               # the margin
    for i, y in enumerate((6, 10, 14, 18)):
        n = (8, 11, 10, 6)[i] if i else 6
        L.hl(9, y, n, '>')
        L.hl(9, y + 1, n, 'G')
        if i < 3:                                     # a tick in the margin
            L.set(4, y, 'V'); L.set(5, y + 1, 'V'); L.set(6, y, 'V'); L.set(6, y - 1, 'v')
        else:
            L.marco(4, y - 1, 3, 3, 'g')
    L.contorno(')')
    return L


def mapa():
    """The map: a sheet folded in three, a river, a dashed path and the red
    pin where you are."""
    L = ic()
    paneles = ((1, 7, 'e', 3), (8, 7, '2', 4), (15, 8, 'e', 3))
    for x, w, c, y0 in paneles:
        L.rect(x, y0, w, 19, c)
    for x, w, c, y0 in paneles:
        L.hl(x, y0, w, '1' if c == 'e' else 'e')
        L.hl(x, y0 + 18, w, '2' if c == 'e' else 'E')
    L.vl(8, 4, 19, 'E'); L.vl(14, 4, 19, 'E')         # the folds' shade
    # the river, from the top left down to the bottom
    for y in range(4, 23):
        x = 4 + int(2.2 * math.sin(y * 0.45))
        L.hl(x, y, 2, 'l'); L.set(x + 2, y, '%')
    # trees
    for tx, ty in ((18, 17), (20, 15), (11, 19)):
        L.set(tx, ty, '3'); L.set(tx + 1, ty, 'f'); L.set(tx, ty - 1, 'E')
    # the dashed path to the pin
    for i, (x, y) in enumerate(((7, 20), (9, 18), (10, 16), (12, 15), (13, 13))):
        L.set(x, y, 'H'); L.set(x + 1, y, 'H')
    L.contorno('4')
    # the pin: a red ball on a needle, with its shadow on the paper
    L.hl(15, 14, 3, '3')
    L.vl(16, 9, 5, 'D'); L.vl(17, 10, 3, 'd')
    L.disco(16.5, 6.5, 4, 'k')
    L.disco(16.5, 6.5, 3.2, 'r')
    volumen(L, 'r', 16.5, 6.5, 3.4, 3.4, '_rrR')
    L.set(15, 4, 'w'); L.set(14, 5, '_')
    return L


def ayuda():
    """A fat question mark, lit from the top left."""
    L = ic()
    Q = [
        "........######..........",
        "......##########........",
        ".....############.......",
        "....#####....#####......",
        "....####......####......",
        "....####......####......",
        "..............####......",
        ".............#####......",
        "............#####.......",
        "..........######........",
        ".........#####..........",
        ".........####...........",
        ".........####...........",
        ".........####...........",
        "........................",
        "........................",
        ".........####...........",
        ".........####...........",
        ".........####...........",
        ".........####...........",
    ]
    mascara(L, Q, 'y', 1, 2)
    borde(L, 'y', 'W', 'Y', 'W', 'O')
    L.set(10, 4, 'w'); L.set(11, 3, 'w')
    L.contorno('!')
    return L


def sonido():
    """A speaker and three waves of sound."""
    L = ic()
    L.rect(2, 8, 5, 8, 'g')                         # the magnet
    L.poli([(6, 8), (12, 2), (12, 22), (6, 16)], 'g')  # the cone
    borde(L, 'g', 'G', '>', '<', 'D')
    L.hl(3, 8, 3, 'w'); L.linea(7, 7, 11, 3, 'w')
    L.vl(6, 9, 6, 'D')                              # where the cone meets it
    L.contorno('k')
    for i, r in enumerate((5.5, 8.5, 11.5)):        # the waves
        for y in range(N):
            for x in range(13, N):
                d = math.hypot(x + 0.5 - 11, y + 0.5 - 12)
                a = abs(math.degrees(math.atan2(y + 0.5 - 12, x + 0.5 - 11)))
                if r - 1.5 < d <= r and a < 50 - i * 4:
                    L.set(x, y, '[' if y < 10 else ('c' if y < 15 else 'C'))
    return L


def guardar():
    """A floppy: the metal shutter with its slot, the label with a red band."""
    L = ic()
    L.rect(1, 1, 22, 22, 'd')
    L.poli([(19, 1), (23, 1), (23, 5)], '.')        # the clipped corner
    borde(L, 'd', 'D', 'x')
    L.rect(6, 1, 11, 8, '<')                        # the shutter
    L.hl(6, 1, 11, 'w'); L.vl(6, 1, 8, 'G'); L.vl(16, 1, 8, '>'); L.hl(6, 8, 11, 'g')
    L.rect(12, 3, 3, 4, 'x'); L.hl(12, 3, 3, 'K')
    L.rect(4, 12, 16, 10, 'G')                      # the label
    L.hl(4, 12, 16, 'r'); L.hl(4, 13, 16, 'R')
    L.hl(4, 14, 16, 'w')
    L.vl(19, 14, 8, 'g'); L.hl(4, 21, 16, '<')
    for y in (16, 19):
        L.hl(6, y, 11 if y == 16 else 7, 'g')
    L.set(2, 20, 'K'); L.set(21, 20, 'K')           # the write-protect holes
    L.contorno('k')
    return L


def cerrar():
    """The way out: a dark doorway in a stone frame, and the arrow into it."""
    L = ic()
    L.rect(1, 1, 11, 22, '8')                       # the frame
    borde(L, '8', 'i', 'I', '7', 'I')
    L.rect(3, 3, 7, 20, 'K')                        # the doorway
    L.hl(3, 3, 7, 'k'); L.vl(3, 4, 19, 'k')
    L.rect(4, 19, 6, 4, 'x')                        # light spilling on the floor
    L.hl(4, 22, 6, 'd')
    L.contorno('9')
    # the arrow, pointing into the room beyond
    A = ic()
    A.poli([(4, 12), (11, 5), (11, 19)], 'y')
    A.rect(10, 10, 12, 4, 'y')
    borde(A, 'y', 'W', 'O', 'W', 'Y')
    A.contorno('k')
    L.pegar(A, 0, 0)
    return L


def mochila():
    """You and your robot: a head with an antenna, a screen face and two feet.
    Also the HUD's MENU button, where it is drawn at its smallest."""
    L = ic()
    L.vl(11, 2, 4, 'g'); L.vl(12, 2, 4, 'D')        # the antenna
    L.rect(2, 5, 20, 14, 'g')                       # the head
    redondear(L, 2, 5, 20, 14)
    borde(L, 'g', 'G', 'D', '<', '>')
    L.rect(0, 9, 2, 5, 'D'); L.vl(0, 9, 5, 'g')     # the ears
    L.rect(22, 9, 2, 5, 'd')
    L.rect(5, 8, 14, 8, 'x')                        # the screen
    L.rect(6, 9, 12, 6, 'K')
    L.rect(7, 10, 3, 3, 'c'); L.set(7, 10, '[')     # the eyes
    L.rect(14, 10, 3, 3, 'c'); L.set(14, 10, '[')
    L.hl(11, 14, 2, 'c'); L.set(10, 13, 'C'); L.set(13, 13, 'C')   # the smile
    L.set(6, 9, 'd')
    L.rect(5, 19, 4, 4, 'D'); L.hl(5, 19, 4, 'g')   # the feet
    L.rect(15, 19, 4, 4, 'D'); L.hl(15, 19, 4, 'g')
    L.contorno('k')
    L.disco(11.5, 1.5, 1.6, 'r')                    # the lamp on the antenna
    L.set(11, 1, '_'); L.set(12, 2, 'R')
    return L


def ajustes():
    """Settings: three sliders, the knobs lit, the tracks filled in yellow."""
    L = ic()
    for cy, kx in ((3, 12), (11, 4), (19, 15)):
        L.rect(0, cy - 1, 24, 4, 'k')               # the groove
        L.hl(1, cy, 22, 'K'); L.hl(1, cy + 1, 22, 'x')
        L.hl(1, cy, kx - 1, 'W'); L.hl(1, cy + 1, kx - 1, 'y')
        L.rect(kx, cy - 3, 6, 8, 'k')               # the knob
        L.rect(kx + 1, cy - 2, 4, 6, '<')
        L.hl(kx + 1, cy - 2, 4, 'w'); L.vl(kx + 1, cy - 1, 4, 'G')
        L.hl(kx + 1, cy + 3, 4, '>'); L.vl(kx + 4, cy - 1, 4, 'g')
        L.vl(kx + 2, cy, 2, 'D'); L.vl(kx + 3, cy, 2, 'G')   # its grip
    return L


# ---------------------------------------------------------------- the items
def aceite():
    """An oil can: the domed body, the long spout with a drop, the handle."""
    L = ic()
    anillo(L, 17.5, 15, 4.8, 2.7, 'O')              # the handle
    L.rect(12, 9, 5, 13, '.')
    L.elipse(9.5, 18, 8.4, 8.6, 'o')                # the body, a dome
    L.rect(0, 20, 24, 4, '.')
    L.rect(1, 19, 18, 3, 'o')
    volumen(L, 'o', 8, 15, 9, 8, 'yooOO', -0.05)
    L.set(6, 12, 'W'); L.set(5, 13, 'W'); L.set(7, 12, 'y'); L.set(4, 14, 'y')
    L.hl(1, 19, 18, 'y'); L.hl(1, 20, 18, 'O'); L.hl(1, 21, 18, 'X')   # the foot
    L.set(17, 19, 'o'); L.set(18, 19, 'O'); L.set(1, 19, 'o')
    L.rect(8, 8, 4, 3, 'g'); L.hl(8, 8, 4, 'G')     # the cap
    L.vl(11, 8, 3, 'D')
    barra(L, 12, 11, 21, 1.5, 2.2, 'g')             # the spout
    barra(L, 11.5, 10.5, 20.5, 1, 0.9, 'G')
    L.contorno('k')
    L.set(22, 4, 'o'); L.set(22, 5, 'O'); L.set(21, 5, 'y')   # the drop
    return L


def bateria():
    """A battery: two terminals marked + and -, the lightning in the body."""
    L = ic()
    L.rect(5, 1, 4, 3, 'G'); L.rect(15, 1, 4, 3, 'g')     # terminals
    L.hl(5, 1, 4, 'w'); L.hl(15, 1, 4, 'G')
    L.rect(3, 4, 18, 19, 'v')
    for x in range(3, 21):                                  # the can, round
        t = (x - 3) / 17
        c = '1' if t < 0.12 else ('v' if t < 0.62 else ('2' if t < 0.85 else 'V'))
        L.vl(x, 4, 19, c)
    L.rect(3, 4, 18, 3, 'd'); L.hl(3, 4, 18, 'D')           # the top band
    L.hl(5, 5, 3, 'r'); L.set(6, 4, 'r'); L.set(6, 6, 'r')  # +
    L.hl(16, 5, 3, 'G')                                     # -
    L.hl(3, 21, 18, 'V')
    B = [
        "......###",
        ".....###.",
        "....###..",
        "...###...",
        "..#######",
        ".....###.",
        "....###..",
        "...###...",
        "..##.....",
        "..#......",
    ]
    L.contorno('k')
    R = ic()                                                # the bolt, cut in
    mascara(R, B, 'y', 6, 9)
    borde(R, 'y', 'W', 'Y', 'W', 'Y')
    R.contorno('F')
    L.pegar(R, 0, 0)
    return L


def soldador():
    """A soldering iron, bottom left to top right: wooden grip with rings,
    metal collar, the shaft, and the tip glowing red to white."""
    L = ic()
    x0, y0, x1, y1 = 1.5, 22.5, 21.5, 2.5
    ax, ay = x1 - x0, y1 - y0
    lon = math.hypot(ax, ay)
    for y in range(N):
        for x in range(N):
            px, py = x + 0.5 - x0, y + 0.5 - y0
            t = (px * ax + py * ay) / (lon * lon)
            d = (px * ay - py * ax) / lon          # > 0 is the lower right side
            if t < 0 or t > 1:
                continue
            if t < 0.46:
                w = 2.6
                tonos = ('0', 'j', 'J')
                if int(t * 60) % 6 == 5: tonos = ('j', 'J', '!')
            elif t < 0.54:
                w = 3.1; tonos = ('w', 'G', 'g')
            elif t < 0.84:
                w = 1.5; tonos = ('G', 'g', 'D')
            else:
                w = 1.5 * (1 - (t - 0.84) / 0.2)
                tonos = ('y', 'o', 'r') if t < 0.93 else ('W', 'y', 'o')
            if abs(d) <= w:
                c = tonos[0] if d < -w / 3 else (tonos[1] if d < w / 3 else tonos[2])
                L.set(x, y, c)
    L.contorno('k')
    for sx, sy, c in ((22, 0, 'y'), (23, 3, 'o'), (19, 0, 'o'), (23, 1, 'W')):   # sparks
        L.set(sx, sy, c)
    return L


def chip():
    """A chip: four legs a side, the notch, the die with its traces."""
    L = ic()
    for p in (5, 9, 13, 17):
        L.rect(p, 1, 2, 3, 'G'); L.rect(p, 20, 2, 3, 'g')
        L.rect(1, p, 3, 2, 'G'); L.rect(20, p, 3, 2, 'g')
        L.set(p + 1, 1, 'g'); L.set(p + 1, 22, 'D'); L.set(1, p + 1, 'g'); L.set(22, p + 1, 'D')
    L.rect(4, 4, 16, 16, 'C')
    borde(L, 'C', 'c', ']')
    L.set(4, 4, 'c')
    L.rect(8, 8, 8, 8, 'k')                        # the die
    L.rect(9, 9, 6, 6, 'x')
    L.hl(9, 9, 6, 'd'); L.vl(9, 9, 6, 'd')
    L.rect(11, 11, 2, 2, 'c'); L.set(11, 11, '[')
    for a, b in ((6, 12), (12, 6), (17, 12), (12, 17)):    # traces
        L.set(a, b, '['); L.set(a + (1 if a < 12 else -1) * (a != 12), b + (1 if b < 12 else -1) * (b != 12), '[')
    L.set(6, 6, ']'); L.set(6, 7, ']'); L.set(7, 6, ']')   # the notch
    L.contorno('k')
    return L


def iman():
    """A horseshoe magnet, open upwards, with silver poles and a spark."""
    L = ic()
    for y in range(N):
        for x in range(N):
            px, py = x + 0.5, y + 0.5
            if 3 <= py <= 13:
                if 2 <= px <= 9 or 14 <= px <= 21:
                    L.set(x, y, 'm')
            elif py > 13:
                d = math.hypot(px - 12, py - 13)
                if 2 <= d <= 10:
                    L.set(x, y, 'm')
    for y in range(N):                              # round, lit from the left
        for x in range(N):
            if L.get(x, y) != 'm':
                continue
            if y + 0.5 <= 13:
                t = (x + 0.5 - (2 if x < 12 else 14)) / 7
            else:
                d = math.hypot(x + 0.5 - 12, y + 0.5 - 13)
                t = (d - 2) / 8
                t = t if x < 12 else 1 - t * 0.5 + 0.2
            L.set(x, y, '~' if t < 0.25 else ('m' if t < 0.7 else 'M'))
    for x0 in (2, 14):                              # the poles
        L.rect(x0, 3, 8, 4, 'G')
        L.hl(x0, 3, 8, 'w'); L.vl(x0, 3, 4, 'w'); L.vl(x0 + 7, 4, 3, 'g')
        L.hl(x0, 7, 8, 'D')
    L.contorno('k')
    for sx, sy, c in ((11, 1, 'y'), (12, 3, 'y'), (11, 5, 'W'), (12, 0, 'W')):   # the pull
        L.set(sx, sy, c)
    return L


def llave():
    """A golden key: the bow with its hole, the shank and a two-toothed bit."""
    L = ic()
    L.disco(11, 6, 5.4, 'y')
    L.disco(11, 6, 2.1, '.')
    L.rect(9, 11, 4, 11, 'y')                       # the shank
    L.rect(13, 15, 4, 2, 'y'); L.rect(13, 19, 3, 3, 'y')   # the bit
    L.hl(13, 15, 4, 'W')
    borde(L, 'y', 'W', 'Y', 'W', 'Y')
    aro_luz(L, 'yWY', 11, 6, 'W', 'O', 0.4)
    L.vl(10, 11, 11, 'W')
    L.set(8, 3, 'w'); L.set(7, 4, 'w')              # the glint
    L.contorno('k')
    return L


def pase():
    """A pass: the clip slot, a photo of you, two lines of text and the
    yellow band with a barcode."""
    L = ic()
    L.rect(1, 3, 22, 18, 'C')
    redondear(L, 1, 3, 22, 18)
    borde(L, 'C', 'c', ']')
    L.rect(9, 5, 6, 2, 'K'); L.hl(9, 6, 6, 'x')     # the clip slot
    L.rect(3, 7, 7, 8, 'w')                         # the photo
    L.rect(4, 8, 5, 6, '=')
    L.rect(5, 9, 3, 3, 'g'); L.set(5, 10, 'k'); L.set(7, 10, 'k')
    L.rect(4, 12, 5, 2, 'b')
    L.hl(11, 8, 9, 'G'); L.hl(11, 10, 7, 'G'); L.hl(11, 12, 8, 'c')
    L.rect(3, 15, 18, 4, 'y')                       # the band
    L.hl(3, 15, 18, 'W'); L.hl(3, 18, 18, 'Y')
    for x in (5, 6, 8, 11, 12, 14, 16, 17, 19):
        L.vl(x, 16, 2, 'K')
    L.contorno('k')
    return L


def tornillos():
    """A box of screws: three of them standing up out of it, heads slotted
    and threads catching the light, over the front board with its nails."""
    L = ic()
    L.rect(1, 9, 22, 3, 'J'); L.hl(1, 9, 22, 'j')    # the back board
    L.rect(2, 11, 20, 2, 'K')                       # the inside
    for sx, top, ranura in ((3, 2, '-'), (10, 0, '+'), (17, 3, '-')):
        L.rect(sx + 1, top + 3, 3, 12 - top, 'g')   # the shank
        L.vl(sx + 1, top + 3, 12 - top, '<'); L.vl(sx + 3, top + 3, 12 - top, 'D')
        for y in range(top + 4, 14, 2):             # the thread
            L.set(sx + 1, y, 'G'); L.set(sx + 2, y + 1, '>'); L.set(sx + 3, y + 1, 'd')
        L.rect(sx, top, 5, 3, '<')                  # the head
        L.hl(sx, top, 5, 'w'); L.hl(sx, top + 2, 5, 'g'); L.set(sx + 4, top + 1, 'g')
        L.set(sx, top, '.'); L.set(sx + 4, top, '.')
        L.set(sx + 1, top, 'G'); L.set(sx + 3, top, 'G')
        if ranura == '+': L.vl(sx + 2, top, 2, 'x'); L.set(sx + 1, top + 1, 'x'); L.set(sx + 3, top + 1, 'x')
        else: L.hl(sx + 1, top + 1, 3, 'x')
    L.rect(1, 13, 22, 10, 'j')                      # the front board
    L.hl(1, 13, 22, '0'); L.hl(1, 17, 22, 'J'); L.hl(1, 18, 22, '0')
    L.hl(1, 22, 22, 'J'); L.vl(22, 13, 10, 'J'); L.vl(1, 13, 10, '0')
    for x, y in ((3, 15), (20, 15), (3, 20), (20, 20)):
        L.set(x, y, 'G'); L.set(x + 1, y, 'D')
    L.contorno('!')
    return L


def ancla():
    """An anchor: ring, stock, shank, the curved arms and their flukes."""
    L = ic()
    anillo(L, 12, 3, 2.9, 1.2, 'u')                 # the ring
    L.rect(4, 7, 16, 3, 'u')                        # the stock
    L.rect(10, 5, 4, 15, 'u')                       # the shank
    anillo(L, 12, 12, 10, 7.2, 'u')                 # the crown and arms
    L.rect(0, 0, N, 13, '.') if False else None
    for y in range(0, 13):
        for x in range(N):
            d = math.hypot(x + 0.5 - 12, y + 0.5 - 12)
            if 7.2 < d <= 10 and L.get(x, y) == 'u' and not (4 <= x <= 19 and 7 <= y <= 9) and not (10 <= x <= 13):
                L.set(x, y, '.')
    L.poli([(0.5, 15), (4, 9.5), (6.5, 15)], 'u')     # the flukes
    L.poli([(23.5, 15), (20, 9.5), (17.5, 15)], 'u')
    borde(L, 'u', '*', 'U', '*', 'U')
    L.vl(11, 10, 9, '*')
    L.set(3, 11, '*')
    L.contorno(',')
    return L


def herramienta():
    """A toolbox: the handle, the lid with a screwdriver sticking out, the
    latch and the yellow stripe. The errands' icon."""
    L = ic()
    L.rect(16, 0, 3, 6, 'r'); L.vl(16, 0, 6, '_'); L.vl(18, 0, 6, 'R')   # the screwdriver
    L.rect(8, 2, 8, 2, 'D'); L.rect(8, 2, 2, 5, 'D'); L.rect(14, 2, 2, 5, 'D')   # the handle
    L.hl(9, 2, 6, 'g')
    L.rect(2, 6, 20, 5, '<')                        # the lid
    L.hl(2, 6, 20, 'w'); L.hl(2, 10, 20, 'g')
    L.rect(1, 11, 22, 11, 'g')                      # the body
    borde(L, 'g', '<', 'D', '<', '>')
    L.rect(1, 15, 22, 3, 'y')                       # the stripe
    L.hl(1, 15, 22, 'W'); L.hl(1, 17, 22, 'Y')
    L.rect(9, 10, 6, 5, 'D'); L.rect(10, 11, 4, 3, 'G')    # the latch
    L.set(11, 12, 'k'); L.set(12, 12, 'k')
    L.contorno('k')
    return L


def barril():
    """An oil drum: round, two hoops, the lid's rim and bung, a label with
    a drop."""
    L = ic()
    L.rect(3, 1, 18, 22, 'o')
    for y in (1, 22):
        L.set(3, y, '.'); L.set(20, y, '.')
    for x in range(3, 21):
        t = (x - 3) / 17
        c = 'O' if t < 0.08 else ('y' if t < 0.26 else ('o' if t < 0.66 else ('O' if t < 0.9 else 'X')))
        for y in range(1, 23):
            if L.get(x, y) != '.': L.set(x, y, c)
    for y in (6, 16):                                # the hoops
        for x in range(2, 22):
            L.set(x, y, 'O' if L.get(x, y) != 'X' else 'X')
            L.set(x, y + 1, 'X')
        L.set(2, y, 'O'); L.set(21, y, 'X'); L.set(2, y + 1, 'X'); L.set(21, y + 1, 'X')
        L.hl(3, y, 4, 'o')
    L.hl(4, 1, 16, 'O'); L.hl(5, 2, 14, 'X')         # the lid
    L.set(15, 2, 'K'); L.set(16, 2, 'K')
    L.rect(7, 9, 10, 6, 'W')                         # the label
    L.hl(7, 14, 10, 'y'); L.vl(16, 9, 6, 'y')
    L.set(11, 10, 'k'); L.hl(11, 11, 2, 'k'); L.rect(10, 12, 4, 2, 'k')   # the drop
    L.set(11, 12, 'd')
    L.contorno('k')
    return L


def combate():
    """A fight: two steel rams meeting, and the spark between them."""
    L = ic()
    L.poli([(1, 1), (10.5, 10), (10.5, 14), (1, 23)], 'G')
    L.poli([(23, 1), (13.5, 10), (13.5, 14), (23, 23)], 'g')
    borde(L, 'G', 'w', 'g', 'w', '<')
    for y in range(N):
        for x in range(12, N):
            if L.get(x, y) == 'g':
                if L.get(x, y - 1) == '.': L.set(x, y, '<')
                elif L.get(x, y + 1) == '.': L.set(x, y, 'D')
    L.set(4, 12, 'D'); L.set(4, 11, 'g'); L.set(19, 12, 'd'); L.set(19, 11, '>')   # rivets
    L.contorno('k')
    # the spark
    S = [
        "...r..r...",
        "...o..o...",
        "r...oo...r",
        ".o..yy..o.",
        "..oyyyyo..",
        "...yWWy...",
        "..yWwwWy..",
        "..yWwwWy..",
        "...yWWy...",
        "..oyyyyo..",
        ".o..yy..o.",
        "r...oo...r",
        "...o..o...",
        "...r..r...",
    ]
    for y, f in enumerate(S):
        for x, c in enumerate(f):
            if c != '.': L.set(7 + x, 5 + y, c)
    return L


def trueque():
    """Trading: green goes up, teal comes down."""
    L = ic()
    A = ic()
    A.poli([(0.5, 9), (6, 1), (11.5, 9)], 'v')
    A.rect(4, 9, 4, 11, 'v')
    borde(A, 'v', '1', 'V', '1', 'V')
    A.contorno('F')
    B = ic()
    B.rect(16, 4, 4, 11, 'n')
    B.poli([(12.5, 14), (18, 22), (23.5, 14)], 'n')
    borde(B, 'n', '^', 'N', '^', 'N')
    B.contorno('|')
    L.pegar(A, 0, 1)
    L.pegar(B, 0, 0)
    return L


def pieza():
    """A loose head: two eyes, the grille, bolts - and the wires where the
    neck was."""
    L = ic()
    L.rect(2, 2, 20, 15, 'g')
    redondear(L, 2, 2, 20, 15)
    borde(L, 'g', 'G', 'D', '<', '>')
    L.hl(4, 5, 16, '>'); L.hl(4, 6, 16, 'G')        # the brow seam
    for ex in (5, 14):                              # the eyes
        L.rect(ex, 8, 5, 4, 'k')
        L.rect(ex + 1, 9, 3, 2, 'c'); L.set(ex + 1, 9, '[')
    L.rect(8, 13, 8, 2, 'x')                        # the grille
    for x in (9, 11, 13): L.set(x, 13, 'D')
    L.set(3, 3, 'w'); L.set(20, 3, 'D'); L.set(3, 15, 'D'); L.set(20, 15, 'd')   # bolts
    L.rect(7, 17, 10, 2, 'd'); L.hl(7, 17, 10, 'D')  # the neck ring
    L.contorno('k')
    for pts, c, c2 in (((9, 19, 8, 21, 9, 23), 'r', 'R'), ((12, 19, 12, 22, 13, 23), 'y', 'Y'),
                       ((15, 19, 16, 21, 15, 22), 'b', 'B')):
        L.linea(pts[0], pts[1], pts[2], pts[3], c)
        L.linea(pts[2], pts[3], pts[4], pts[5], c2)
    L.set(12, 23, 'W')
    return L


def colgar():
    """Hang up: the blue handset, and the red arrow down."""
    L = ic()
    H = ic()
    for y in range(N):                                   # the grip, an arch
        for x in range(N):
            d = math.hypot((x + 0.5 - 12) / 1.25, y + 0.5 - 12)
            if 5.2 < d <= 9 and y < 9:
                H.set(x, y, 'b')
    H.poli([(0.5, 8), (7.5, 5), (8.5, 9), (1.5, 11)], 'b')     # the ear piece
    H.poli([(23.5, 8), (16.5, 5), (15.5, 9), (22.5, 11)], 'b')  # the mouth piece
    borde(H, 'b', '=', 'B', '=', 'B')
    H.set(9, 4, 'w'); H.set(10, 3, 'w')
    H.contorno('`')
    L.pegar(H, 0, 0)
    A = ic()
    A.rect(10, 12, 4, 5, 'r')
    A.poli([(5.5, 16.5), (18.5, 16.5), (12, 23)], 'r')
    borde(A, 'r', '_', 'R', '_', 'R')
    A.contorno('k')
    L.pegar(A, 0, 0)
    return L


def diario():
    """The journal: a leather notebook with a label, the spine's stitches,
    the pages showing and a red ribbon."""
    L = ic()
    L.rect(4, 1, 18, 21, 'q'); L.vl(21, 2, 20, 'Q')   # the pages
    for y in range(3, 21, 2): L.set(21, y, ')')
    L.rect(2, 1, 18, 21, 'j')                        # the cover
    borde(L, 'j', '0', 'J', '0', 'J')
    L.rect(2, 1, 4, 21, 'J'); L.vl(2, 1, 21, 'j')    # the spine
    for y in range(3, 21, 3): L.set(4, y, 'y')
    L.rect(9, 5, 8, 6, 'y')                          # the label
    L.hl(9, 5, 8, 'W'); L.hl(9, 10, 8, 'Y'); L.vl(16, 5, 6, 'Y')
    L.hl(10, 7, 5, 'O'); L.hl(10, 9, 3, 'O')
    L.contorno('!')
    L.rect(14, 22, 2, 2, 'r'); L.set(14, 23, 'R')    # the ribbon
    L.set(15, 21, 'r')
    return L


ICONOS = {
    'IC_TALLER': taller, 'IC_OBJETOS': objetos, 'IC_EQUIPO': equipo,
    'IC_REGISTRO': registro, 'IC_MAPA': mapa, 'IC_AYUDA': ayuda,
    'IC_SONIDO': sonido, 'IC_GUARDAR': guardar, 'IC_CERRAR': cerrar,
    'IC_MOCHILA': mochila, 'IC_AJUSTES': ajustes,
    'IC_ACEITE': aceite, 'IC_BATERIA': bateria, 'IC_SOLDADOR': soldador,
    'IC_CHIP': chip, 'IC_IMAN': iman, 'IC_LLAVE': llave, 'IC_PASE': pase,
    'IC_TORNILLOS': tornillos, 'IC_ANCLA': ancla, 'IC_HERRAMIENTA': herramienta,
    'IC_BARRIL': barril, 'IC_COMBATE': combate, 'IC_TRUEQUE': trueque,
    'IC_PIEZA': pieza, 'IC_COLGAR': colgar, 'IC_DIARIO': diario,
}


# ---------------------------------------------------------------- output
def unos():
    """The 1x icons of ch_ui.c, '#' and '+' turned into their two colours."""
    src = open(os.path.join(MAIN, 'ch_ui.c')).read()
    out = {}
    for n, cuerpo, c, d in re.findall(
            r'\[(IC_\w+)\] = \{ \{[^\n]*\n(.*?)\}, 0x(\w+), 0x(\w+) \}', src, re.S):
        out[n] = (re.findall(r'"([^"]*)"', cuerpo), c, d)
    return out


def escribir(pal):
    out = ['/* GENERATED by tools/arte2x_iconos.py - the 2x UI icons (ASSETS.md).',
           ' * One 24x24 array per 12x12 icon of ICONOS[] in ch_ui.c, drawn with',
           ' * ch_blit2m(..., esc) over the same box. Do not edit here: edit the',
           ' * drawing in the script, or take it out of the script first. */', '']
    for nombre, f in ICONOS.items():
        L = f()
        filas = L.filas()
        assert (L.w, L.h) == (N, N), nombre
        for r in filas:
            for c in r:
                assert c == '.' or c in pal, (nombre, c)
        out.append('static const char *const HD_%s[%d] = {' % (nombre, N))
        for r in filas:
            out.append('    "%s",' % r.replace('\\', '\\\\').replace('"', '\\"').replace('?', '\\?'))
        out.append('};')
        out.append('')
    out.append('/* which 2x drawing each icon has (NULL: the 1x through the upscaler) */')
    out.append('static const char *const *const ICONOS_HD[NICONOS] = {')
    for nombre in ICONOS:
        out.append('    [%s] = HD_%s,' % (nombre, nombre))
    out.append('};')
    open(os.path.join(MAIN, 'ch_ui2x.inc'), 'w').write('\n'.join(out) + '\n')
    return len(ICONOS)


def hoja(pal, path):
    from PIL import Image, ImageDraw
    fondo = (0x0E, 0x11, 0x1A)
    uno = unos()

    def rgb(h):
        return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))

    def pinta(im, filas, ox, oy, z, c1=None, c2=None):
        d = ImageDraw.Draw(im)
        for y, r in enumerate(filas):
            for x, ch in enumerate(r):
                if ch == '.': continue
                col = rgb(c1) if ch == '#' else (rgb(c2) if ch == '+' else pal.get(ch, (255, 0, 255)))
                d.rectangle([ox + x * z, oy + y * z, ox + x * z + z - 1, oy + y * z + z - 1], fill=col)

    z = 7
    celda_w = 12 * z * 2 + 8 + 24 * z + 8 + 48 + 4 + 96 + 24
    cols = 3
    celda_h = 24 * z + 22
    nombres = list(ICONOS)
    filas_n = (len(nombres) + cols - 1) // cols
    im = Image.new('RGB', (cols * celda_w, filas_n * celda_h), fondo)
    d = ImageDraw.Draw(im)
    for i, n in enumerate(nombres):
        ox, oy = (i % cols) * celda_w, (i // cols) * celda_h
        d.text((ox, oy), n, fill=(220, 220, 220))
        if n in uno:
            f1, c1, c2 = uno[n]
            pinta(im, f1, ox, oy + 14, z * 2, c1, c2)
        filas = ICONOS[n]().filas()
        x = ox + 24 * z + 8
        pinta(im, filas, x, oy + 14, z)
        x += 24 * z + 8
        pinta(im, filas, x, oy + 14, 2)            # esc 1 on the glass: the HUD
        pinta(im, filas, x + 52, oy + 14, 4)       # esc 2: the lists
    im.save(path)


if __name__ == '__main__':
    from hoja import paleta
    pal = paleta()
    n = escribir(pal)
    print('wrote %d icons to main/ch_ui2x.inc' % n)
    if '--hoja' in sys.argv:
        hoja(pal, sys.argv[sys.argv.index('--hoja') + 1])
