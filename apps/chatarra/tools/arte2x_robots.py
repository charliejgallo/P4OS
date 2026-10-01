"""Chatarra's robots at 2x: the 64 parts as sprites (ASSETS.md, section 6).

A robot is the sum of four parts drawn over one 26x40-unit box, and a part
carries no colour of its own: it is painted with the robot's SKIN and with its
own elemental TYPE. So these sprites are written in colour SLOTS, not colours:

    1  skin, lit edge        2  skin, the plate      3  skin, shaded side
    4  skin, dark / seams    5  skin, deepest        8  the outline
    6  visor / eye glow      7  its glint
    9  the part's type       0  the type, lit

and every other character is a fixed material from the game's palette
(chrome and steel greys, glass, rubber, rust, gold). ch_parts.c turns the
slots into colours for each robot.

Every canvas is in HALF units of the robot's box, at the box's own columns,
so parts combine exactly as the watch's did:

    head    52 x 34, row 0 = box y -5   (the aerials reach above the box)
    torso   52 x 36, row 0 = box y  9
    legs    52 x 30, row 0 = box y 25
    arm     24 x 40, row 0 = box y 12; the RIGHT arm, whose shoulder meets the
            torso at column 6 (box x 21); the left one is its mirror.

The shapes follow the watch's descriptors (CAB_FORMA, TOR_FORMA, BRA_TIPO,
PIE_TIPO...) so each part keeps its silhouette and size, and each one has
something of its own on top: a lamp, a saw, a drill, a boot.
"""
import math
from random import Random
from lienzo import Lienzo

W = 52                                   # the box, in half units

# ---------------------------------------------------------------- the pencil
def placa(L, x, y, w, h, r=2, c='2', luz='1', sombra='3', hondo='4', borde='8'):
    """A plate: rounded corners, an outline, a lit top-left edge and a shaded
    bottom-right one. Drawn over whatever is there."""
    x, y, w, h = int(x), int(y), int(w), int(h)

    def dentro(xx, yy):
        if not (x <= xx < x + w and y <= yy < y + h):
            return False
        dx = max(x + r - xx, 0, xx - (x + w - 1 - r))
        dy = max(y + r - yy, 0, yy - (y + h - 1 - r))
        return dx * dx + dy * dy <= r * r + (r > 0) * 0.5 * r

    for yy in range(y, y + h):
        for xx in range(x, x + w):
            if not dentro(xx, yy):
                continue
            borde_aqui = not all(dentro(xx + dx, yy + dy)
                                 for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)))
            if borde_aqui:
                L.set(xx, yy, borde)
                continue
            arriba = not dentro(xx, yy - 2)
            izq = not dentro(xx - 2, yy)
            abajo = not dentro(xx, yy + 2)
            der = not dentro(xx + 2, yy)
            if arriba or izq: L.set(xx, yy, luz)
            elif abajo: L.set(xx, yy, hondo)
            elif der: L.set(xx, yy, sombra)
            else:
                # THE VOLUME: the plate is a curved sheet lit from the top
                # left - a lit corner, the plate's own tone, a dithered
                # step and its shaded side - instead of one flat colour.
                u = (xx - x) / max(w - 1, 1); v = (yy - y) / max(h - 1, 1)
                d = u * 0.65 + v * 0.35
                if d < 0.16 and w > 6 and h > 6:
                    L.set(xx, yy, luz if (xx + yy) & 1 else c)
                elif d < 0.62 or w <= 6:
                    L.set(xx, yy, c)
                elif d < 0.74:
                    L.set(xx, yy, sombra if (xx + yy) & 1 else c)
                else:
                    L.set(xx, yy, sombra)


def perno(L, x, y, c='3'):
    L.set(x, y, '1'); L.set(x + 1, y, c); L.set(x, y + 1, c); L.set(x + 1, y + 1, '4')


def ojo(L, x, y, w, h, marco='5'):
    L.rect(x - 1, y - 1, w + 2, h + 2, marco)
    L.rect(x, y, w, h, '6')
    L.set(x, y, '7')
    if w > 2: L.set(x + 1, y, '7')
    L.hl(x, y + h - 1, w, '3') if h > 2 else None


def costura(L, x, y, w, c='4'):
    L.hl(x, y, w, c)


def rejilla(L, x, y, w, h):
    L.rect(x, y, w, h, '5')
    for xx in range(x + 1, x + w - 1, 2):
        L.vl(xx, y + 1, h - 2, '8')


# ================================================================ HEADS
# the watch's head traits, per variant
CAB_FORMA = [0, 1, 0, 2, 1, 3, 4, 2, 5, 4, 0, 3, 6, 5, 6, 2]
CAB_OJOS  = [0, 1, 0, 6, 0, 1, 2, 5, 7, 3, 4, 1, 2, 4, 5, 6]
CAB_ANT   = [0, 0, 1, 0, 2, 3, 0, 1, 5, 2, 4, 3, 1, 5, 4, 3]
HY = 10                                   # canvas row of box y 0


def cabeza(var):
    L = Lienzo(W, 34)
    forma, ojos, ant = CAB_FORMA[var], CAB_OJOS[var], CAB_ANT[var]
    w, h, y = {1: (12, 11, 0), 2: (12, 9, 2), 3: (14, 9, 2), 4: (10, 10, 1),
               5: (13, 10, 1), 6: (12, 10, 1)}.get(forma, (12, 10, 1))
    x = 13 - w // 2
    X, Y, WW, HH = x * 2, HY + y * 2, w * 2, h * 2
    cx = 26

    # --- the top finish goes BEHIND the head
    if ant == 1:                          # an aerial with a lit ball
        L.rect(cx - 1, Y - 8, 2, 9, '>'); L.vl(cx - 1, Y - 8, 9, 'G')
        L.disco(cx, Y - 9, 2.6, '8'); L.disco(cx, Y - 9, 1.8, '9'); L.set(cx - 1, Y - 10, '0')
    elif ant == 2:                        # two antennae
        for ax in (X + 4, X + WW - 6):
            L.rect(ax, Y - 6, 2, 7, '>'); L.vl(ax, Y - 6, 7, 'G')
            L.rect(ax - 1, Y - 8, 4, 3, '8'); L.rect(ax, Y - 7, 2, 1, '9'); L.set(ax, Y - 7, '0')
    elif ant == 3:                        # a dish on a mast
        L.rect(cx - 1, Y - 5, 2, 6, 'D'); L.vl(cx - 1, Y - 5, 6, 'g')
        L.poli([(cx - 9, Y - 10), (cx + 9, Y - 10), (cx + 6, Y - 5), (cx - 6, Y - 5)], '8')
        L.poli([(cx - 8, Y - 9), (cx + 8, Y - 9), (cx + 5, Y - 6), (cx - 5, Y - 6)], '3')
        L.hl(cx - 7, Y - 9, 14, '1'); L.set(cx, Y - 8, '9')
    elif ant == 4:                        # a fin
        L.poli([(cx - 3, Y + 1), (cx + 3, Y + 1), (cx + 1, Y - 9), (cx - 1, Y - 9)], '8')
        L.poli([(cx - 2, Y + 1), (cx + 2, Y + 1), (cx, Y - 8)], '3')
        L.linea(cx - 1, Y, cx, Y - 7, '1')
    elif ant == 5:                        # horns
        for s in (-1, 1):
            bx = cx + s * (WW // 2 - 3)
            L.poli([(bx - 3, Y + 2), (bx + 3, Y + 2), (bx + s * 4, Y - 8)], '8')
            L.poli([(bx - 2, Y + 2), (bx + 2, Y + 2), (bx + s * 3, Y - 6)], 'w')
            L.linea(bx - s, Y + 1, bx + s * 2, Y - 5, '<')

    # --- the shell
    r = {2: 5, 5: 3, 1: 3}.get(forma, 2)
    if forma == 2:                        # the dome: a half-disc over a band
        L.elipse(cx, Y + HH * 0.55, WW / 2, HH * 0.75, '8')
        L.rect(X, Y + HH // 2, WW, HH // 2, '8')
        L.elipse(cx, Y + HH * 0.55, WW / 2 - 1, HH * 0.75 - 1, '2')
        L.rect(X + 1, Y + HH // 2, WW - 2, HH // 2 - 1, '2')
        L.elipse(cx - 3, Y + HH * 0.35, WW / 4, HH * 0.28, '1', sobre='2')
        L.rect(X + WW - 5, Y + 5, 4, HH - 6, '3', ) if False else None
        for yy in range(Y, Y + HH):
            for xx in range(cx + 4, X + WW):
                if L.get(xx, yy) == '2' and L.get(xx + 2, yy) in '8.': L.set(xx, yy, '3')
        L.hl(X + 1, Y + HH - 2, WW - 2, '4')
    elif forma == 5:                      # the hexagon: bevelled corners
        L.poli([(X + 3, Y), (X + WW - 3, Y), (X + WW, Y + 4), (X + WW, Y + HH - 4),
                (X + WW - 3, Y + HH), (X + 3, Y + HH), (X, Y + HH - 4), (X, Y + 4)], '8')
        L.poli([(X + 4, Y + 1), (X + WW - 4, Y + 1), (X + WW - 1, Y + 4), (X + WW - 1, Y + HH - 4),
                (X + WW - 4, Y + HH - 1), (X + 4, Y + HH - 1), (X + 1, Y + HH - 4), (X + 1, Y + 4)], '2')
        L.linea(X + 4, Y + 1, X + WW - 4, Y + 1, '1'); L.linea(X + 1, Y + 4, X + 3, Y + 2, '1')
        L.vl(X + 1, Y + 4, HH - 8, '1')
        L.vl(X + WW - 2, Y + 4, HH - 8, '3'); L.vl(X + WW - 3, Y + 5, HH - 10, '3')
        L.hl(X + 4, Y + HH - 2, WW - 8, '4')
    else:
        placa(L, X, Y, WW, HH, r)
        L.vl(X + WW - 3, Y + 3, HH - 6, '3')
    if forma == 6:                        # the separate jaw
        L.rect(X + 3, Y + HH - 8, WW - 6, 7, '8')
        L.rect(X + 4, Y + HH - 7, WW - 8, 5, '3')
        L.hl(X + 4, Y + HH - 7, WW - 8, '2')
        L.set(X + 2, Y + HH - 6, 'g'); L.set(X + WW - 3, Y + HH - 6, 'g')

    # --- the face
    ey = Y + (HH * 4) // 10
    if ojos == 1:                         # a visor from side to side
        L.rect(X + 3, ey - 2, WW - 6, 6, '8')
        L.rect(X + 4, ey - 1, WW - 8, 4, '6')
        L.hl(X + 5, ey - 1, WW - 12, '7')
        L.linea(X + WW - 10, ey - 1, X + WW - 12, ey + 2, '7')
        L.hl(X + 4, ey + 2, WW - 8, '3')
    elif ojos == 2:                       # the cyclops' one eye
        L.disco(cx, ey + 1, 4.6, '8'); L.disco(cx, ey + 1, 3.6, '5')
        L.disco(cx, ey + 1, 2.6, '6'); L.disco(cx - 1, ey, 1.1, '7')
        L.set(cx + 1, ey + 2, '9')
    elif ojos == 3:                       # three eyes
        for ex, eyy in ((X + 4, ey + 1), (cx - 2, ey - 1), (X + WW - 8, ey + 1)):
            ojo(L, ex, eyy, 4, 3)
    elif ojos == 4:                       # angry brows
        L.linea(X + 3, ey - 3, X + 10, ey - 1, '8'); L.linea(X + 3, ey - 2, X + 10, ey, '8')
        L.linea(X + WW - 4, ey - 3, X + WW - 11, ey - 1, '8'); L.linea(X + WW - 4, ey - 2, X + WW - 11, ey, '8')
        ojo(L, X + 5, ey + 1, 4, 3); ojo(L, X + WW - 9, ey + 1, 4, 3)
    elif ojos == 5:                       # a slotted visor
        L.rect(X + 3, ey - 2, WW - 6, 8, '8')
        L.rect(X + 4, ey - 1, WW - 8, 6, '5')
        for k in range(X + 5, X + WW - 5, 3):
            L.vl(k, ey, 4, '6'); L.set(k, ey, '7')
    elif ojos == 6:                       # two big eyes
        for ex in (X + 3, X + WW - 10):
            L.rect(ex - 1, ey - 2, 8, 7, '8')
            L.rect(ex, ey - 1, 6, 5, 'w')
            L.rect(ex + 2, ey, 3, 3, '6'); L.set(ex + 2, ey, '7')
            L.rect(ex + 3, ey + 1, 2, 2, '8')
    elif ojos == 7:                       # crossed blades
        for s, ex in ((1, X + 3), (-1, X + WW - 4)):
            L.linea(ex, ey - 2, ex + s * 7, ey + 2, '6'); L.linea(ex, ey - 1, ex + s * 7, ey + 3, '6')
            L.linea(ex, ey - 3, ex + s * 7, ey + 1, '8')
            L.set(ex + s * 2, ey - 1, '7')
    else:                                 # two dots
        ojo(L, X + 5, ey, 4, 4); ojo(L, X + WW - 9, ey, 4, 4)

    # --- the mouth: a grille nearly every head has
    if ojos != 5 and HH >= 18:
        my = Y + HH - 6
        rejilla(L, cx - 5, my, 10, 3)
    # rivets at the temples, a seam round the jaw
    perno(L, X + 2, Y + HH - 5); perno(L, X + WW - 4, Y + HH - 5)
    if forma not in (2, 6):
        costura(L, X + 3, Y + HH - 8, WW - 6)

    # --- what makes each one itself
    extra = {
        0: lambda: L.set(cx, Y + 2, '9'),                               # Ojo Simple
        1: lambda: L.rect(X + 1, Y + 2, 2, 5, '9'),                     # Visor: a side light
        2: lambda: (L.hl(X + 2, Y + 3, WW - 4, '4'), L.hl(X + 2, Y + 2, WW - 4, '1')),  # Casco: a brim
        3: lambda: L.elipse(cx, Y + 4, 6, 2.5, '[', sobre='12'),       # Cupula: glass
        4: lambda: [L.set(X + 4 + i * 2, Y + HH - 3, '9') for i in range(3)],  # Antena: lamps
        5: lambda: (L.rect(X - 2, ey, 3, 4, '8'), L.rect(X - 1, ey + 1, 1, 2, '9'),
                    L.rect(X + WW - 1, ey, 3, 4, '8'), L.rect(X + WW, ey + 1, 1, 2, '9')),  # Radar
        6: lambda: L.hl(cx - 3, Y + 2, 6, '9'),                         # Ciclope: a brow light
        7: lambda: (L.disco(cx, Y + 3, 2.5, '8'), L.disco(cx, Y + 3, 1.6, 'y'),
                    L.set(cx - 1, Y + 2, 'W')),                         # Faro: a lamp
        8: lambda: [L.vl(cx - 5 + i * 3, Y + HH - 4, 3, 'w') for i in range(4)],  # Craneo: teeth
        9: lambda: L.set(cx, Y + 2, '9'),                               # Tri-Ojo
        10: lambda: (L.rect(cx - 1, ey - 4, 2, 9, '8'), L.vl(cx - 1, ey - 3, 7, '3')),  # Yelmo: nose guard
        11: lambda: [L.set(X + 3 + i * 3, Y + 3, '9') for i in range(4)],  # Sensor: a row of leds
        12: lambda: (L.rect(cx - 5, Y - 1, 10, 4, '8'), L.rect(cx - 4, Y, 8, 2, 'y'),
                     L.hl(cx - 3, Y, 5, 'W')),                          # Farola: a lamp on top
        13: lambda: [L.hl(X + 4, Y + HH - 5 + i * 2, WW - 8, '4') for i in range(2)],  # Mascara: mask bars
        14: lambda: (L.rect(X + WW - 2, ey - 1, 6, 3, '8'), L.rect(X + WW - 1, ey, 5, 1, 'g'),
                     L.set(X + WW + 3, ey, '9')),                       # Torreta: a barrel
        15: lambda: (L.disco(cx, Y + 3, 2.4, '8'), L.disco(cx, Y + 3, 1.5, '9'), L.set(cx - 1, Y + 2, '0')),  # Nucleo
    }
    extra[var]()
    return L


# ================================================================ TORSOS
TOR_FORMA = [0, 1, 0, 2, 3, 0, 4, 2, 3, 0, 2, 5, 1, 3, 5, 4]
TOR_PECHO = [6, 2, 2, 0, 3, 6, 5, 1, 3, 4, 0, 5, 1, 4, 0, 1]
TOR_HOMB  = [0, 0, 1, 0, 1, 2, 1, 0, 3, 2, 1, 3, 2, 1, 3, 3]
TY = 9                                    # box y of the canvas's row 0


def torso(var):
    L = Lienzo(W, 36)
    forma, pecho, hom = TOR_FORMA[var], TOR_PECHO[var], TOR_HOMB[var]
    w, h = {1: (18, 16), 2: (14, 16), 3: (17, 16), 4: (16, 15), 5: (19, 16)}.get(forma, (16, 16))
    x = 13 - w // 2
    X, Y, WW, HH = x * 2, (11 - TY) * 2, w * 2, h * 2
    cx = 26

    # the neck: a column of rings
    L.rect(cx - 4, Y - 3, 8, 4, '8'); L.rect(cx - 3, Y - 2, 6, 3, 'D')
    L.hl(cx - 3, Y - 2, 6, 'g'); L.hl(cx - 3, Y - 1, 6, 'x')

    # the body
    if forma == 1:                        # a barrel: round sides and hoops
        L.elipse(cx, Y + HH / 2, WW / 2, HH / 2 + 2, '8')
        L.rect(X + 2, Y, WW - 4, HH, '8')
        L.elipse(cx, Y + HH / 2, WW / 2 - 1, HH / 2 + 1, '2')
        L.rect(X + 3, Y + 1, WW - 6, HH - 2, '2')
        for yy in range(Y, Y + HH):
            for xx in range(X, X + WW):
                v = L.get(xx, yy)
                if v == '2':
                    d = (xx - cx) / (WW / 2)
                    L.set(xx, yy, '1' if d < -0.62 else ('3' if d > 0.45 else ('4' if d > 0.75 else '2')))
        for yy in (Y + 4, Y + HH - 6):
            L.hl(X + 2, yy, WW - 4, '4'); L.hl(X + 2, yy + 1, WW - 4, '1')
    elif forma == 3:                      # a trapezoid, wide at the shoulders
        L.poli([(X - 1, Y), (X + WW + 1, Y), (X + WW - 3, Y + HH), (X + 3, Y + HH)], '8')
        L.poli([(X, Y + 1), (X + WW, Y + 1), (X + WW - 4, Y + HH - 1), (X + 4, Y + HH - 1)], '2')
        L.hl(X + 1, Y + 1, WW - 2, '1')
        L.linea(X, Y + 1, X + 3, Y + HH - 2, '1')
        L.linea(X + WW - 1, Y + 2, X + WW - 4, Y + HH - 2, '3')
        L.linea(X + WW - 2, Y + 2, X + WW - 5, Y + HH - 2, '3')
        L.hl(X + 5, Y + HH - 2, WW - 10, '4')
    else:
        placa(L, X, Y, WW, HH, 3 if forma == 5 else 2)
        L.vl(X + WW - 4, Y + 3, HH - 6, '3')
        if forma == 5:                    # the solid one: thick side plates
            L.vl(X + 5, Y + 2, HH - 4, '4'); L.vl(X + WW - 6, Y + 2, HH - 4, '4')

    # the chest
    py = Y + 8
    if pecho == 0:                        # a core, glowing with the part's type
        L.disco(cx, py + 5, 6.2, '8'); L.disco(cx, py + 5, 5.2, '5')
        L.disco(cx, py + 5, 4, '9'); L.disco(cx - 1, py + 4, 2.2, '0')
        L.set(cx - 2, py + 3, 'w')
        for a in range(0, 360, 45):
            L.set(cx + 5.7 * math.cos(math.radians(a)), py + 5 + 5.7 * math.sin(math.radians(a)), 'g')
    elif pecho == 1:                      # a rhombus crystal
        L.poli([(cx, py - 2), (cx + 7, py + 5), (cx, py + 12), (cx - 7, py + 5)], '8')
        L.poli([(cx, py), (cx + 5, py + 5), (cx, py + 10), (cx - 5, py + 5)], '9')
        L.poli([(cx, py), (cx - 5, py + 5), (cx, py + 5)], '0')
        L.set(cx - 2, py + 3, 'w')
    elif pecho == 2:                      # a grille of vents
        L.rect(X + 5, py - 1, WW - 10, 11, '8')
        for i in range(5):
            L.hl(X + 6, py + i * 2, WW - 12, '5')
            L.hl(X + 6, py + i * 2 + 1, WW - 12, '3')
    elif pecho == 3:                      # a panel with lights and a dial
        L.rect(X + 5, py - 1, WW - 10, 12, '8')
        L.rect(X + 6, py, WW - 12, 10, '4')
        L.hl(X + 6, py, WW - 12, '3')
        for i, c in enumerate(('9', 'r', 'y')):
            L.rect(X + 8 + i * 4, py + 2, 2, 2, c); L.set(X + 8 + i * 4, py + 2, 'w')
        L.disco(X + WW - 10, py + 6, 2.6, 'k'); L.disco(X + WW - 10, py + 6, 1.8, 'G')
        L.linea(X + WW - 10, py + 6, X + WW - 9, py + 4, 'r')
        L.hl(X + 8, py + 7, 8, '5')
    elif pecho == 4:                      # a screen with a readout
        L.rect(X + 5, py - 2, WW - 10, 13, '8')
        L.rect(X + 6, py - 1, WW - 12, 11, 'K')
        L.rect(X + 7, py, WW - 14, 9, '|')
        for i in range(WW - 14):
            yy = py + 5 + int(2.5 * math.sin(i / 1.8))
            L.set(X + 7 + i, yy, '9')
        L.hl(X + 7, py, WW - 14, 'N')
    elif pecho == 5:                      # a cross plate
        L.rect(cx - 3, py - 2, 6, 14, '8'); L.rect(X + 5, py + 3, WW - 10, 5, '8')
        L.rect(cx - 2, py - 1, 4, 12, '4'); L.rect(X + 6, py + 4, WW - 12, 3, '4')
        L.rect(cx - 2, py + 4, 4, 3, '9'); L.set(cx - 2, py + 4, '0')
    else:                                 # riveted plates
        L.rect(X + 4, py - 2, WW - 8, 14, '4')
        L.rect(X + 5, py - 1, WW - 10, 12, '2')
        L.hl(X + 5, py - 1, WW - 10, '1')
        L.vl(cx, py - 1, 12, '4')
        for px, pyy in ((X + 6, py), (X + WW - 8, py), (X + 6, py + 8), (X + WW - 8, py + 8)):
            perno(L, px, pyy)

    # the panel lines: a groove under the collar and one down each flank,
    # each with a lit lip - the plate is made of pieces
    for xx in range(X + 3, X + WW - 3):
        if L.get(xx, Y + 5) in '123' and L.get(xx, Y + 6) in '123':
            L.set(xx, Y + 5, '4'); L.set(xx, Y + 6, '1')
    for fx in (X + 3, X + WW - 5):
        for yy in range(Y + 7, Y + HH - 6):
            if L.get(fx, yy) in '123': L.set(fx, yy, '4')
    # the waist: a belt of darker plate with a buckle
    L.rect(X + 3, Y + HH - 5, WW - 6, 3, '4')
    L.hl(X + 3, Y + HH - 5, WW - 6, '3')
    L.rect(cx - 2, Y + HH - 5, 4, 3, 'g'); L.set(cx - 2, Y + HH - 5, 'w')
    perno(L, X + 2, Y + 2); perno(L, X + WW - 4, Y + 2)

    # the shoulders
    if hom in (1, 2):
        for s in (-1, 1):
            sx = X - 5 if s < 0 else X + WW - 3
            placa(L, sx, Y - 1, 8, 10 if hom == 1 else 8, 2)
            if hom == 2:                  # spikes
                bx = sx + (0 if s < 0 else 7)
                L.poli([(bx - 1, Y + 1), (bx + 2, Y + 1), (bx + s * 3, Y - 5)], '8')
                L.poli([(bx, Y + 1), (bx + 1, Y + 1), (bx + s * 2, Y - 3)], 'w')
    elif hom == 3:                        # rounded pauldrons
        for s in (-1, 1):
            sx = X if s < 0 else X + WW
            L.disco(sx, Y + 4, 6.5, '8'); L.disco(sx, Y + 4, 5.5, '3')
            L.disco(sx - 1, Y + 3, 3.5, '2'); L.disco(sx - 2, Y + 2, 1.5, '1')
            perno(L, sx - 1, Y + 6)

    # what makes each one itself
    if var == 1:                          # Barril: a bung
        L.rect(cx + 6, Y + 3, 3, 3, 'j'); L.set(cx + 6, Y + 3, '0')
    elif var == 3 or var == 10:           # Reactor, Celda: cables to the core
        L.linea(X + 3, Y + 6, cx - 6, py + 5, 'x'); L.linea(X + WW - 4, Y + 6, cx + 6, py + 5, 'x')
    elif var == 5:                        # Blindado: extra plates
        L.rect(X + 2, Y + HH - 10, 4, 4, '4'); L.rect(X + WW - 6, Y + HH - 10, 4, 4, '4')
    elif var == 9:                        # Fragua: a chimney and embers
        L.rect(X + WW - 8, Y - 4, 4, 5, '8'); L.rect(X + WW - 7, Y - 3, 2, 4, 'x')
        L.set(X + WW - 6, Y - 5, 'o'); L.set(X + WW - 7, Y - 6, 'y')
    elif var == 12:                       # Turbina: a fan in the side
        L.disco(X + 6, Y + HH - 11, 3.5, '8'); L.disco(X + 6, Y + HH - 11, 2.5, 'D')
        L.linea(X + 4, Y + HH - 13, X + 8, Y + HH - 9, 'g'); L.linea(X + 8, Y + HH - 13, X + 4, Y + HH - 9, 'g')
    elif var == 13:                       # Placa Madre: circuit traces
        for i in range(3):
            L.hl(X + 4, Y + HH - 9 + i * 2, 5 + i * 2, '9')
    elif var == 14:                       # Motor: exhaust pipes
        for s in (-1, 1):
            ex = cx + s * 9
            L.rect(ex - 1, Y - 5, 3, 6, '8'); L.rect(ex, Y - 4, 1, 5, 'g')
    elif var == 15:                       # Prisma: facets
        L.linea(X + 4, Y + 3, X + 10, Y + 9, '1'); L.linea(X + WW - 5, Y + 3, X + WW - 11, Y + 9, '3')
    elif var == 0:                        # Caja: a stencilled number
        L.rect(X + WW - 9, Y + HH - 12, 4, 2, '4')
    return L


# ================================================================ ARMS
BRA_TIPO = [2, 3, 5, 4, 6, 2, 7, 7, 1, 5, 6, 8, 4, 4, 4, 1]
BRA_GRUE = [3, 3, 4, 3, 3, 4, 4, 4, 5, 4, 3, 5, 4, 4, 4, 5]
AI = 6                                    # the canvas column of the shoulder's inner edge
AY = 12                                   # box y of row 0


def brazo(var):
    """The RIGHT arm: shoulder at the top, the tool at the end."""
    L = Lienzo(24, 40)
    tipo, gr = BRA_TIPO[var], BRA_GRUE[var]
    X, G = AI, gr * 2
    y0 = (13 - AY) * 2                    # the shoulder (box y 13)
    cx = X + G // 2

    # the shoulder joint: a ball
    L.disco(cx, y0 + 3, G / 2 + 1, '8'); L.disco(cx, y0 + 3, G / 2, '3')
    L.disco(cx - 1, y0 + 2, G / 2 - 1.5, '2'); L.set(cx - 2, y0 + 1, '1')
    # the upper arm
    placa(L, X, y0 + 6, G, 10, 1)
    # the elbow
    L.rect(X + 1, y0 + 16, G - 2, 3, '8'); L.rect(X + 2, y0 + 17, G - 4, 1, 'g')
    # the forearm
    fy = y0 + 19
    if tipo == 1:                         # long and straight, a padded glove
        placa(L, X, fy, G, 7, 1)
        placa(L, X - 1, fy + 6, G + 2, 7, 2, c='2')
        for k in range(3): L.vl(X + 1 + k * 3, fy + 7, 5, '4')
    elif tipo == 2:                       # a claw
        placa(L, X, fy, G, 6, 1)
        for s in (0, 1):
            bx = X + (0 if s == 0 else G - 2)
            L.poli([(bx, fy + 5), (bx + 2, fy + 5), (bx + 1 + (1 if s else -1), fy + 12)], '8')
            L.poli([(bx + 0.5, fy + 5), (bx + 1.5, fy + 5), (bx + 1 + (1 if s else -1) * 0.5, fy + 10)], 'w')
        L.set(cx, fy + 7, '9')
    elif tipo == 3:                       # a pincer
        placa(L, X, fy, G, 5, 1)
        L.poli([(X - 1, fy + 5), (X + G + 1, fy + 5), (X + G + 2, fy + 12), (X + G - 1, fy + 12),
                (X + G - 1, fy + 8), (X + 1, fy + 8), (X + 1, fy + 12), (X - 2, fy + 12)], '8')
        L.rect(X, fy + 6, G, 2, '2'); L.vl(X - 1, fy + 8, 3, '3'); L.vl(X + G, fy + 8, 3, '3')
    elif tipo == 4:                       # a cannon (and the lance, magnet, torch)
        placa(L, X - 1, fy, G + 2, 12, 2, c='3', luz='2')
        L.rect(X, fy + 11, G, 2, '8')
        L.rect(X + 1, fy + 11, G - 2, 1, 'k')
        if var == 12:                     # Lanzas: a long point
            L.poli([(X + 1, fy + 12), (X + G - 1, fy + 12), (cx, fy + 17)], '8')
            L.poli([(X + 2, fy + 12), (X + G - 2, fy + 12), (cx, fy + 16)], '[')
        elif var == 13:                   # Imanes: a horseshoe
            L.rect(X - 1, fy + 11, G + 2, 5, '8'); L.rect(X, fy + 11, G, 4, 'r')
            L.rect(X + 2, fy + 13, G - 4, 3, '.'); L.hl(X, fy + 14, 2, 'w'); L.hl(X + G - 2, fy + 14, 2, 'w')
        elif var == 14:                   # Sopletes: a nozzle and its flame
            L.disco(cx, fy + 14, 2.2, 'o'); L.disco(cx, fy + 14, 1.2, 'W')
        else:                             # Canones: the muzzle glows
            L.rect(X + 1, fy + 11, G - 2, 1, '9')
        for k in (fy + 3, fy + 7):
            L.hl(X, k, G, '4')
    elif tipo == 5:                       # a hammer (and the drill)
        placa(L, X, fy, G - 1, 6, 1)
        if var == 9:                      # Taladros: a drill bit
            L.poli([(X - 1, fy + 6), (X + G + 1, fy + 6), (cx, fy + 16)], '8')
            L.poli([(X, fy + 6), (X + G, fy + 6), (cx, fy + 14)], 'g')
            for k in range(3):
                L.linea(X + 1 + k, fy + 7 + k * 2, X + G - 1 - k, fy + 8 + k * 2, 'D')
        else:
            placa(L, X - 4, fy + 6, G + 6, 9, 1, c='g', luz='w', sombra='>', hondo='D')
            L.hl(X - 3, fy + 10, G + 4, 'D')
    elif tipo == 6:                       # a spring (and the whip)
        for i in range(5):
            L.elipse(cx, fy + 1 + i * 2, G / 2 + 0.5, 1.2, '8')
            L.elipse(cx, fy + 1 + i * 2, G / 2 - 0.5, 0.6, '<')
        if var == 10:                     # Latigos: a lash with sparks
            L.linea(cx, fy + 10, cx + 3, fy + 15, '8'); L.linea(cx + 3, fy + 15, cx - 1, fy + 16, '8')
            L.set(cx + 4, fy + 14, '9'); L.set(cx - 2, fy + 16, '0')
        else:
            placa(L, X - 1, fy + 10, G + 2, 6, 2)
    elif tipo == 7:                       # spikes (and the saw)
        placa(L, X, fy, G, 9, 1)
        for yy in (fy + 2, fy + 6):
            L.poli([(X + G - 1, yy), (X + G - 1, yy + 3), (X + G + 3, yy + 1)], '8')
            L.set(X + G, yy + 1, 'w')
        if var == 6:                      # Sierras: a round blade
            L.disco(cx, fy + 13, 4.5, '8'); L.disco(cx, fy + 13, 3.6, 'G')
            for a in range(0, 360, 40):
                L.set(cx + 4.8 * math.cos(math.radians(a)), fy + 13 + 4.8 * math.sin(math.radians(a)), 'w')
            L.disco(cx, fy + 13, 1.2, 'D')
        else:
            placa(L, X, fy + 9, G, 5, 1)
    elif tipo == 8:                       # a shield
        placa(L, X, fy, G, 6, 1)
        L.poli([(X - 1, fy + 2), (X + G + 5, fy + 2), (X + G + 5, fy + 12), (X + G / 2 + 2, fy + 17),
                (X - 1, fy + 12)], '8')
        L.poli([(X, fy + 3), (X + G + 4, fy + 3), (X + G + 4, fy + 12), (X + G / 2 + 2, fy + 16),
                (X, fy + 12)], '3')
        L.poli([(X, fy + 3), (X + G / 2 + 2, fy + 3), (X + G / 2 + 2, fy + 16), (X, fy + 12)], '2')
        L.hl(X, fy + 3, G + 4, '1')
        L.rect(X + G / 2, fy + 6, 3, 3, '9'); L.set(X + G / 2, fy + 6, '0')
    else:                                 # straight, with a fist
        placa(L, X, fy, G, 6, 1)
        placa(L, X - 1, fy + 5, G + 2, 7, 2)
        L.hl(X, fy + 8, G, '4')
    if var == 15:                         # Rotores: blades on the wrist
        L.hl(X - 5, fy + 3, G + 10, '8'); L.hl(X - 4, fy + 3, G + 8, 'G')
        L.disco(cx, fy + 3, 1.5, 'D')
    return L


# ================================================================ LEGS
PIE_TIPO = [0, 1, 2, 3, 4, 5, 6, 7, 0, 2, 3, 6, 5, 6, 7, 5]
PY = 25


def piernas(var):
    L = Lienzo(W, 30)
    tipo = PIE_TIPO[var]
    y = (26 - PY) * 2                     # the hips (box y 26)
    cx = 26

    def pierna(lx, largo, pie_w=12, bota=False):
        placa(L, lx, y + 2, 8, largo // 2, 1)
        L.rect(lx + 1, y + 2 + largo // 2 - 1, 6, 3, '8'); L.rect(lx + 2, y + 2 + largo // 2, 4, 1, 'g')
        perno(L, lx + 3, y + 2 + largo // 2)
        placa(L, lx, y + 4 + largo // 2, 8, largo // 2 - 2, 1, c='3', luz='2')
        fx = lx - (pie_w - 8) // 2
        if bota:
            placa(L, fx, y + largo, pie_w + 2, 7, 2, c='4', luz='3', sombra='5', hondo='5')
            L.hl(fx, y + largo + 6, pie_w + 2, 'k')
            L.rect(fx + 2, y + largo + 2, 4, 2, '9')
        else:
            placa(L, fx, y + largo + 1, pie_w, 5, 2)
            L.hl(fx + 1, y + largo + 5, pie_w - 2, 'k')

    if tipo == 1:                         # stilts
        for lx in (cx - 12, cx + 6):
            L.rect(lx, y + 2, 6, 20, '8'); L.rect(lx + 1, y + 2, 4, 20, 'D'); L.vl(lx + 1, y + 2, 20, 'g')
            L.rect(lx - 3, y + 21, 12, 6, '8'); L.rect(lx - 2, y + 22, 10, 4, '2'); L.hl(lx - 2, y + 22, 10, '1')
            L.hl(lx - 2, y + 26, 12, 'k')
    elif tipo == 2:                       # tracks
        L.rect(cx - 20, y + 8, 40, 20, '8')
        L.rect(cx - 19, y + 9, 38, 18, 'x')
        for i in range(9):                # the treads
            L.rect(cx - 19 + i * 4, y + 8, 2, 1, 'D'); L.rect(cx - 19 + i * 4, y + 27, 2, 1, 'D')
        for i in range(5):                # the wheels inside
            wx = cx - 14 + i * 7
            L.disco(wx, y + 18, 3.6, '8'); L.disco(wx, y + 18, 2.8, '3'); L.disco(wx - 1, y + 17, 1.2, '1')
        placa(L, cx - 14, y + 1, 28, 8, 2)
        if var == 9:                      # Cadenas: chain links
            for i in range(6):
                L.marco(cx - 17 + i * 6, y + 25, 4, 3, 'g')
    elif tipo == 3:                       # a single wheel (Rueda, Monociclo)
        L.rect(cx - 2, y, 4, 6, '8'); L.rect(cx - 1, y, 2, 6, 'g')
        L.disco(cx, y + 16, 13.5, '8'); L.disco(cx, y + 16, 12.5, 'x')
        L.disco(cx, y + 16, 9.5, '8'); L.disco(cx, y + 16, 8.5, '3')
        L.disco(cx - 2, y + 14, 4.5, '2'); L.disco(cx, y + 16, 3, 'D'); L.disco(cx, y + 16, 1.5, 'g')
        for a in range(0, 360, 30):       # the tyre's tread
            L.set(cx + 12.9 * math.cos(math.radians(a)), y + 16 + 12.9 * math.sin(math.radians(a)), 'K')
        if var == 10:                     # Monociclo: a sparking hub
            L.set(cx + 1, y + 15, '9'); L.set(cx - 1, y + 17, '0')
    elif tipo == 4:                       # a tricycle
        placa(L, cx - 16, y + 3, 32, 8, 2)
        for wx in (cx - 12, cx + 12):
            L.disco(wx, y + 19, 8, '8'); L.disco(wx, y + 19, 7, 'x')
            L.disco(wx, y + 19, 4, '3'); L.disco(wx - 1, y + 18, 1.8, '1')
        L.disco(cx, y + 22, 5, '8'); L.disco(cx, y + 22, 4, 'x'); L.disco(cx, y + 22, 2, 'g')
    elif tipo == 5:                       # hover (Flotador, Cohetes, Deslizador)
        placa(L, cx - 14, y + 2, 28, 10, 3)
        L.rect(cx - 10, y + 11, 20, 3, '8'); L.rect(cx - 9, y + 12, 18, 1, 'x')
        if var == 12:                     # Cohetes: two nozzles and their fire
            for nx in (cx - 8, cx + 4):
                L.rect(nx, y + 13, 4, 4, '8'); L.rect(nx + 1, y + 13, 2, 3, 'D')
                L.poli([(nx, y + 17), (nx + 4, y + 17), (nx + 2, y + 27)], 'o')
                L.poli([(nx + 1, y + 17), (nx + 3, y + 17), (nx + 2, y + 23)], 'W')
        else:                             # a glow under it
            L.elipse(cx, y + 18, 12, 2.5, '9')
            L.elipse(cx, y + 18, 8, 1.4, '0')
            L.elipse(cx, y + 23, 6, 1.2, '9')
        if var == 15:                     # Deslizador: fins
            L.poli([(cx - 14, y + 4), (cx - 20, y + 12), (cx - 14, y + 11)], '8')
            L.poli([(cx + 14, y + 4), (cx + 20, y + 12), (cx + 14, y + 11)], '8')
    elif tipo == 6:                       # arachnid (Aracnidas, Cuadrupedas, Garras Sup.)
        placa(L, cx - 10, y + 2, 20, 8, 2)
        for s in (-1, 1):
            for i in range(2):
                dx = 10 + i * 6
                kx = cx + s * dx
                L.linea(cx + s * 6, y + 6, kx, y + 4, '8'); L.linea(cx + s * 6, y + 7, kx, y + 5, '8')
                L.linea(cx + s * 6, y + 6, kx, y + 4, '3')
                L.disco(kx, y + 5, 2, '8'); L.set(kx, y + 5, 'g')
                L.linea(kx, y + 6, kx + s * 2, y + 26, '8'); L.linea(kx + s, y + 6, kx + s * 3, y + 26, '8')
                L.linea(kx, y + 7, kx + s * 2, y + 25, '3')
                if var == 13:             # Garras Sup.: claws for feet
                    L.linea(kx + s * 2, y + 26, kx + s * 5, y + 27, 'w')
                else:
                    L.rect(kx + s * 2 - 1, y + 25, 3, 2, 'k')
        if var == 11:                     # Cuadrupedas: armour on top
            L.hl(cx - 9, y + 3, 18, '1'); perno(L, cx - 2, y + 5)
    elif tipo == 7:                       # springs (Resortes, Pistones)
        for lx in (cx - 12, cx + 4):
            if var == 14:                 # Pistones: a piston in a cylinder
                L.rect(lx, y + 2, 8, 12, '8'); L.rect(lx + 1, y + 3, 6, 10, '3'); L.vl(lx + 1, y + 3, 10, '2')
                L.rect(lx + 2, y + 14, 4, 8, '8'); L.rect(lx + 3, y + 14, 2, 8, 'G')
            else:
                for i in range(6):
                    L.elipse(lx + 4, y + 4 + i * 3, 4.5, 1.3, '8'); L.elipse(lx + 4, y + 4 + i * 3, 3.5, 0.6, '<')
            placa(L, lx - 2, y + 21, 12, 6, 2)
            L.hl(lx - 1, y + 26, 10, 'k')
    else:                                 # two legs (Patas, Botas)
        for lx in (cx - 12, cx + 4):
            pierna(lx, 20, 12, bota=(var == 8))
    # the hips
    if tipo in (0, 1, 7):
        placa(L, cx - 12, y - 1, 24, 5, 2, c='3', luz='2')
    return L


# ================================================================ wear
# The zone each variant starts appearing in (ch_partes' 'nivel'): the same
# pattern in the four categories, two variants a zone.
NIVEL = [1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8]


def desgaste(L, var, semilla):
    """What a part has been through. The first zones' parts are scrap - a
    rust stain, a scratch, a dent - the middle ones are clean, and the last
    ones are polished: a specular streak and glints along their lit edges.
    It is what makes a part you tore off in the Summit look like a prize next
    to one from Villa Tuerca."""
    rnd = Random(semilla * 31 + var)
    n = NIVEL[var]
    placa_px = [(x, y) for y in range(L.h) for x in range(L.w) if L.p[y][x] in '23']
    if not placa_px:
        return
    if n <= 2:
        for _ in range(2):                # rust stains
            x, y = rnd.choice(placa_px)
            for dx, dy, c in ((0, 0, 'u'), (1, 0, 'U'), (0, 1, '*'), (-1, 1, 'u'), (1, 1, 'U')):
                if L.get(x + dx, y + dy) in '23': L.set(x + dx, y + dy, c)
        for _ in range(2):                # scratches
            x, y = rnd.choice(placa_px)
            for k in range(3):
                if L.get(x + k, y + k) in '23': L.set(x + k, y + k, '4')
            if L.get(x + 1, y) in '23': L.set(x + 1, y, '1')
    elif n >= 6:
        for y in range(L.h):              # glints along the lit edges
            for x in range(L.w):
                if L.p[y][x] == '1' and (x * 5 + y * 3) % 13 == 0:
                    L.p[y][x] = 'w'
        for _ in range(2):                # a specular streak on a big plate
            x, y = rnd.choice(placa_px)
            for k in range(4):
                if L.get(x + k, y - k) in '23': L.set(x + k, y - k, '1')
    else:
        x, y = rnd.choice(placa_px)       # a single dent
        if L.get(x, y) in '23': L.set(x, y, '4')
        if L.get(x - 1, y - 1) in '23': L.set(x - 1, y - 1, '1')


def seams(L, x0, y0, w, h):
    """Panel lines across a big plate: a dark groove with a lit lip under it."""
    for y in range(y0, y0 + h):
        for x in range(x0, x0 + w):
            if L.get(x, y) in '23' and L.get(x, y + 1) in '23':
                pass
    return L


# ================================================================ tables
def todas():
    d = {}
    for v in range(16):
        c, t, b, p = cabeza(v), torso(v), brazo(v), piernas(v)
        for i, L in enumerate((c, t, b, p)):
            desgaste(L, v, i)
        d['CAB_%02d' % v] = c
        d['TOR_%02d' % v] = t
        d['BRA_%02d' % v] = b
        d['PIE_%02d' % v] = p
    return d

SLOTS = '1234567890'


# ================================================================ output and preview
CANVAS = {'CAB': (52, 34, -5 * 2), 'TOR': (52, 36, 9 * 2), 'PIE': (52, 30, 25 * 2),
          'BRA': (24, 40, 12 * 2)}


def escribir_c(path):
    d = todas()
    out = ['/* GENERATED by tools/arte2x_robots.py - the 64 robot parts at 2x',
           ' * (ASSETS.md, section 6). Colour SLOTS, not colours: 1..5 the skin from',
           ' * lit to deepest, 8 the outline, 6/7 the visor and its glint, 9/0 the',
           ' * part\'s type and its light; any other character is the palette. */', '']
    for nombre, L in d.items():
        out.append('static const char *const HD_%s[%d] = {' % (nombre, L.h))
        for r in L.filas():
            out.append('    "%s",' % r.replace('\\\\', '\\\\\\\\').replace('"', '\\\\"').replace('?', '\\\\?'))
        out.append('};')
    for cat in ('CAB', 'TOR', 'BRA', 'PIE'):
        out.append('static const char *const *const HD_%s[PVAR] = {' % cat)
        out.append('    ' + ', '.join('HD_%s_%02d' % (cat, v) for v in range(16)) + ',')
        out.append('};')
    open(path, 'w').write('\n'.join(out) + '\n')


def pintar_robot(partes, skin, tipos, pal, z=4, espejo=False):
    """The composition ch_robot_draw() does, for the preview."""
    from PIL import Image
    im = Image.new('RGB', (52 * z, 90 * z), (30, 34, 46))
    px = im.load()
    def tono(c, f):
        t = (255, 255, 255) if f > 0 else (0, 0, 0)
        f = abs(f)
        return tuple(int(a + (b - a) * f / 16) for a, b in zip(c, t))
    def hexa(h): return ((h >> 16) & 255, (h >> 8) & 255, h & 255)
    cl, md, os_, br = [hexa(v) for v in skin]
    def color(ch, tipo):
        m = {'1': tono(cl, 5), '2': cl, '3': md, '4': os_, '5': tono(os_, -6), '8': (5, 6, 12),
             '6': br, '7': tono(br, 9), '9': hexa(tipo), '0': tono(hexa(tipo), 7)}
        return m.get(ch, pal.get(ch))
    def poner(L, x0, y0, tipo, mir=False):
        for yy in range(L.h):
            for xx in range(L.w):
                ch = L.p[yy][L.w - 1 - xx] if mir else L.p[yy][xx]
                if ch == '.': continue
                c = color(ch, tipo)
                if c is None: continue
                X, Y = x0 + xx, y0 + yy + 10
                if 0 <= X < 52 and 0 <= Y < 90:
                    for a in range(z):
                        for b in range(z):
                            px[X * z + a, Y * z + b] = c
    cab, tor, bra, pie = partes
    d = todas()
    poner(d['PIE_%02d' % pie], 0, 25 * 2, tipos[3])
    poner(d['BRA_%02d' % bra], 15 - 23, 12 * 2, tipos[2], mir=True)      # left arm, mirrored
    poner(d['TOR_%02d' % tor], 0, 9 * 2, tipos[1])
    poner(d['CAB_%02d' % cab], 0, -5 * 2, tipos[0])
    poner(d['BRA_%02d' % bra], 36, 12 * 2, tipos[2])
    if espejo: im = im.transpose(Image.FLIP_LEFT_RIGHT)
    return im


if __name__ == '__main__':
    import sys, os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    from hoja import paleta
    from PIL import Image, ImageDraw
    AQUI = os.path.dirname(os.path.abspath(__file__))
    escribir_c(os.path.join(AQUI, '..', 'main', 'ch_parts2x.inc'))
    if len(sys.argv) > 1:
        pal = paleta()
        SKINS = [(0x9CC8F7, 0x4A7BC8, 0x24406E, 0x7BE9FF), (0xA8E890, 0x4FA85C, 0x235C30, 0xC8FF8A),
                 (0xF7B25C, 0xC8792A, 0x6E4014, 0xFFE45E), (0xF29A94, 0xC04A40, 0x6E221C, 0xFF9F8A),
                 (0xCDB4F0, 0x8158C0, 0x452C6E, 0xE0A8FF), (0xD5DCEB, 0x8A93AB, 0x454C60, 0xFFFFFF),
                 (0x8EE6DC, 0x2FA396, 0x125650, 0x6FFFEE), (0xEBC9A0, 0xA07A4E, 0x513A22, 0xFFDFA8)]
        TIPO = [0xD5DCEB, 0xB072F0, 0xFF6A1E, 0x7BE9FF, 0xFFE45E, 0x4ADE80]
        z = 3
        hoja = Image.new('RGB', (8 * 52 * z + 80, 2 * 90 * z + 20), (12, 12, 16))
        for v in range(16):
            rnd = Random(v)
            im = pintar_robot((v, v, v, v), SKINS[v % 8], [TIPO[rnd.randrange(6)] for _ in range(4)], pal, z)
            hoja.paste(im, ((v % 8) * (52 * z + 10), (v // 8) * (90 * z + 10)))
        hoja.save(sys.argv[1])
