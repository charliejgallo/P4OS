"""Chatarra's things at 2x: decorations, furniture, animals, people (ASSETS.md).

Each drawing is exactly twice the size of the 1x sprite it replaces and keeps
its anchor: whatever stood on the ground at the bottom of the 1x sprite
stands on the ground at the bottom of this one, because the game places both
at the same logical point and the solid cells are the 1x ones.

Light comes from the top left. Outlines are the darkest tone of the material
itself (sel-out), and black only where the thing meets the ground.
"""
import math
from random import Random
from lienzo import Lienzo


# ---------------------------------------------------------------- helpers
def copa(L, cx, cy, rx, ry, rnd, tonos=('4', 'F', 'f', '3', 'E', '2', '1')):
    """A round canopy of leaves: dark mass, then lighter clumps up and to the
    left, then a scatter of lit leaves on their tops."""
    o, dk, d, m, l, h, hh = tonos
    L.elipse(cx, cy, rx + 1, ry + 1, o)
    L.elipse(cx, cy, rx, ry, dk)
    for _ in range(9):
        a = rnd.uniform(0, 2 * math.pi)
        r = rnd.uniform(0, 0.6)
        L.disco(cx + math.cos(a) * rx * r, cy + math.sin(a) * ry * r,
                rnd.uniform(rx * 0.3, rx * 0.45), d, sobre=dk + d)
    for _ in range(8):
        a = rnd.uniform(math.pi * 0.9, math.pi * 1.7)
        r = rnd.uniform(0.2, 0.7)
        L.disco(cx + math.cos(a) * rx * r, cy + math.sin(a) * ry * r,
                rnd.uniform(rx * 0.22, rx * 0.35), m, sobre=d + m)
    for _ in range(6):
        a = rnd.uniform(math.pi * 1.05, math.pi * 1.6)
        r = rnd.uniform(0.3, 0.75)
        L.disco(cx + math.cos(a) * rx * r, cy + math.sin(a) * ry * r,
                rnd.uniform(rx * 0.12, rx * 0.2), l, sobre=m + l)
    for _ in range(int(rx * 1.4)):
        x = cx + rnd.uniform(-rx, rx * 0.3); y = cy + rnd.uniform(-ry, ry * 0.2)
        if L.get(int(x), int(y)) in (l, m):
            L.set(x, y, h)
            if rnd.random() < 0.3: L.set(x - 1, y, hh)
    for _ in range(int(rx * 1.2)):          # holes of shade in the lower right
        x = cx + rnd.uniform(-rx * 0.2, rx); y = cy + rnd.uniform(-ry * 0.1, ry)
        if L.get(int(x), int(y)) in (d, dk):
            L.set(x, y, o)


def mata(L, x, y, rnd, tonos=('3', 'E', '2')):
    """A clump of grass at the foot of something."""
    for i in range(5):
        h = rnd.choice((2, 3, 4))
        c = tonos[i % len(tonos)]
        L.vl(x + i, y - h, h, c)


def tablones(L, x, y, w, h, alto, base='j', luz='0', sombra='J', junta='!', vertical=False):
    if vertical:
        for xx in range(x, x + w, alto):
            L.rect(xx, y, alto, h, base)
            L.vl(xx, y, h, luz); L.vl(xx + alto - 1, y, h, junta)
    else:
        for yy in range(y, y + h, alto):
            L.rect(x, yy, w, alto, base)
            L.hl(x, yy, w, luz); L.hl(x, yy + alto - 1, w, junta)
            L.set(x + (yy * 7) % max(w - 4, 1) + 2, yy + alto // 2, sombra)


def ventana(L, x, y, w, h, marco='0', marco2='J', vidrio='c', luz='[', sombra='C'):
    L.rect(x - 1, y - 1, w + 2, h + 2, marco2)
    L.rect(x, y, w, h, vidrio)
    L.hl(x - 1, y - 1, w + 2, marco)
    L.vl(x + w // 2, y, h, marco2)
    L.hl(x, y + h // 2, w, marco2)
    for i in range(min(w, h) // 2):          # the reflection, a diagonal
        L.set(x + 1 + i, y + h // 2 - 2 - i, luz) if y + h // 2 - 2 - i >= y else None
    L.hl(x, y + h - 1, w, sombra)
    L.hl(x - 2, y + h + 1, w + 4, marco)     # the sill
    L.hl(x - 2, y + h + 2, w + 4, '!')


# ---------------------------------------------------------------- nature
def arbol():
    rnd = Random(101)
    L = Lienzo(48, 72)
    # trunk and roots (1x: rows 22..33, x 8..15)
    L.poli([(19, 40), (29, 40), (30, 62), (35, 66), (13, 66), (18, 62)], '!')
    L.poli([(20, 40), (28, 40), (29, 62), (33, 65), (15, 65), (19, 62)], 'J')
    L.poli([(20, 40), (24, 40), (24, 64), (17, 64), (20, 61)], 'j')
    L.vl(21, 44, 18, '0')
    for y in (46, 52, 57):
        L.hl(22, y, 4, '!')
    L.set(26, 49, '!'); L.set(25, 50, '!')
    copa(L, 24, 22, 22, 21, rnd)
    for x in (8, 30, 16, 36):
        mata(L, x, 69, rnd)
    L.hl(10, 67, 28, '3')
    return L


def pino():
    rnd = Random(102)
    L = Lienzo(48, 72)
    L.rect(21, 56, 6, 12, 'J'); L.vl(21, 56, 12, '0'); L.vl(26, 56, 12, '!')
    capas = ((6, 22), (16, 34), (28, 46), (40, 60))
    for i, (y0, y1) in enumerate(capas):
        a = 8 + i * 5
        L.poli([(24, y0 - 6), (24 + a, y1), (24 - a, y1)], '4')
        L.poli([(24, y0 - 4), (24 + a - 2, y1 - 1), (24 - a + 2, y1 - 1)], 'f')
        L.poli([(24, y0 - 4), (24, y1 - 1), (24 - a + 2, y1 - 1)], '3')
        for k in range(3):                  # the drooping tips of the branches
            x = 24 - a + 4 + k * (2 * a - 8) // 2
            L.set(x, y1, '4'); L.set(x + 1, y1, 'F')
        for _ in range(6):
            x = rnd.randrange(24 - a + 3, 24); y = rnd.randrange(y0, y1 - 1)
            if L.get(x, y) == '3': L.set(x, y, 'E')
    L.set(24, 0, 'y'); L.set(23, 1, 'y'); L.set(25, 1, 'y'); L.set(24, 1, 'z')
    for x in (12, 30):
        mata(L, x, 70, rnd)
    return L


def fuente():
    L = Lienzo(72, 44)
    L.elipse(35, 28, 34, 15, '9')           # the basin's outer wall
    L.elipse(35, 26, 33, 14, 'I')
    L.elipse(35, 24, 32, 12.5, '8')
    L.elipse(35, 22.5, 30, 11, 'i')
    L.elipse(35, 21.5, 29, 10, '7')
    L.elipse(35, 22, 26, 8.5, 'L')          # the water
    L.elipse(35, 21.5, 25, 7.5, '%')
    L.elipse(35, 21, 23, 6.5, 'l')
    for i, r in enumerate((6, 11, 16)):     # ripples
        for a in range(200, 340, 6):
            L.set(35 + r * math.cos(math.radians(a)), 21 + r * 0.35 * math.sin(math.radians(a)), '$')
    L.rect(32, 8, 6, 14, '8')               # the spout
    L.vl(32, 8, 14, '7'); L.vl(37, 8, 14, 'I')
    L.rect(30, 6, 10, 3, 'i'); L.hl(30, 6, 10, '7')
    L.rect(34, 1, 2, 6, '$'); L.set(33, 2, 'c'); L.set(36, 3, 'c')
    L.hl(28, 20, 14, '$')
    L.elipse(35, 40, 20, 2.5, '9')          # the base
    return L


def roca_grande(L, cx, cy, rx, ry):
    L.elipse(cx, cy, rx, ry, '9')
    L.elipse(cx - 0.5, cy - 0.5, rx - 1, ry - 1, 'I')
    L.elipse(cx - 1.5, cy - 1.5, rx - 2.5, ry - 2.5, '8')
    L.elipse(cx - 3, cy - 3, rx * 0.45, ry * 0.4, 'i')
    L.elipse(cx - 4, cy - 4, rx * 0.2, ry * 0.18, '7')


# ---------------------------------------------------------------- buildings
def casa():
    rnd = Random(103)
    L = Lienzo(96, 72)
    x0, x1 = 2, 82                          # the walls (1x: x 1..41)
    # walls: siding boards, lit on top
    L.rect(x0, 16, x1 - x0, 48, 'Q')
    for y in range(18, 62, 4):
        L.hl(x0, y, x1 - x0, ')')
        L.hl(x0, y + 1, x1 - x0, '6') if y % 8 == 2 else None
    L.vl(x0, 16, 48, ')'); L.vl(x1 - 1, 16, 48, '5')
    L.rect(x1 - 6, 16, 5, 48, ')')          # the wall's shaded end
    # windows and the door (1x: windows x 4..9 / 26..31 at y 11..15, door 16..23)
    ventana(L, 10, 25, 10, 8)
    ventana(L, 54, 25, 10, 8)
    L.rect(31, 40, 18, 24, '!')
    L.rect(32, 41, 16, 23, 'J')
    for x in range(33, 47, 4):
        L.vl(x, 42, 21, 'j'); L.vl(x + 1, 42, 21, '0')
    L.rect(44, 52, 2, 2, 'y'); L.set(44, 52, 'W')
    L.hl(29, 39, 22, '0'); L.hl(29, 38, 22, 'j')
    # the roof: rows of tiles, overhanging the walls
    L.poli([(24, 0), (58, 0), (86, 16), (-2, 16)], '/')
    L.poli([(25, 1), (57, 1), (84, 15), (0, 15)], 'a')
    for y in range(3, 15, 3):
        a = 24 - (y * 24) // 16; b = 58 + (y * 28) // 16
        L.hl(a + 1, y, b - a - 2, 'A')
        for x in range(a + 2, b - 2, 5):
            L.set(x + (y % 2) * 2, y - 1, '-')
    L.hl(25, 1, 32, '-')
    L.hl(-1, 16, 88, 'A'); L.hl(0, 17, 84, '/')    # the eaves and their shadow
    L.hl(x0, 18, x1 - x0, ')')
    # chimney
    L.rect(66, 0, 7, 8, 'A'); L.rect(67, 0, 5, 7, 'a'); L.hl(65, 0, 9, '/')
    # foundation, path, grass
    L.rect(x0, 64, x1 - x0, 4, '8'); L.hl(x0, 64, x1 - x0, 'i'); L.hl(x0, 67, x1 - x0, '9')
    for x in range(x0 + 5, x1, 9):
        L.vl(x, 64, 4, 'I')
    L.rect(34, 68, 12, 4, 'h'); L.hl(34, 68, 12, '6'); L.rect(38, 70, 8, 2, 'H')
    for x in (8, 16, 64, 72):
        mata(L, x, 71, rnd)
    return L


def taller():
    rnd = Random(104)
    L = Lienzo(96, 72)
    x0, x1 = 2, 82
    L.rect(x0, 14, x1 - x0, 50, 'd')
    for x in range(x0, x1, 6):               # corrugated sheet
        L.vl(x, 14, 50, 'x'); L.vl(x + 1, 14, 50, 'D')
    L.vl(x0, 14, 50, 'K'); L.vl(x1 - 1, 14, 50, 'K')
    ventana(L, 8, 22, 10, 6, marco='g', marco2='x')
    ventana(L, 58, 22, 10, 6, marco='g', marco2='x')
    # the big door, orange frame, shutter inside (1x: x 9..27 rows 16..27)
    L.rect(18, 32, 40, 32, 'O')
    L.rect(19, 33, 38, 31, 'o')
    L.rect(22, 36, 32, 28, 'K')
    for y in range(37, 64, 3):
        L.hl(23, y, 30, '>'); L.hl(23, y + 1, 30, 'D')
    L.hl(18, 32, 40, 'y'); L.vl(18, 32, 32, 'y')
    L.rect(54, 31, 8, 3, 'o'); L.set(61, 31, 'y')   # the sign's arm
    # the roof: a flat metal lid with ribs
    L.rect(0, 2, 86, 12, 'D')
    L.hl(0, 2, 86, '<'); L.hl(0, 3, 86, 'g')
    for x in range(4, 84, 7):
        L.vl(x, 4, 9, 'd'); L.vl(x + 1, 4, 9, '>')
    L.hl(0, 13, 86, 'x'); L.hl(1, 14, 84, 'K')
    L.rect(70, 0, 8, 3, 'x')                 # a vent
    # the base and the concrete apron
    L.rect(x0, 64, x1 - x0, 4, 'x'); L.hl(x0, 64, x1 - x0, 'D')
    L.rect(14, 68, 48, 4, 'I'); L.hl(14, 68, 48, 'i')
    for x in (6, 70):
        mata(L, x, 71, rnd, tonos=('I', '8', 'i'))
    return L


def horno():
    L = Lienzo(96, 72)
    x0, x1 = 4, 88
    L.rect(x0, 6, x1 - x0, 60, '/')
    for fila in range(0, 30):
        y = 8 + fila * 2
        off = 0 if fila % 2 == 0 else 4
        for x in range(x0 + 1 + off, x1 - 1, 8):
            L.rect(x, y, 7, 1, 'a' if (x + y) % 3 else 'A')
    for fila in range(0, 60, 2):
        pass
    for y in range(8, 64, 2):
        L.hl(x0 + 1, y + 1, x1 - x0 - 2, '/')
    L.hl(x0, 6, x1 - x0, '-')
    L.rect(0, 2, 96, 5, 'A'); L.hl(0, 2, 96, '-'); L.hl(0, 6, 96, '/')
    # the mouth, glowing (1x: x 23..32 rows 21..28)
    L.poli([(30, 40), (66, 40), (70, 64), (26, 64)], 'Z')
    L.poli([(32, 42), (64, 42), (67, 64), (29, 64)], 'O')
    L.poli([(36, 46), (60, 46), (62, 64), (34, 64)], 'o')
    L.poli([(40, 50), (56, 50), (58, 64), (38, 64)], 'y')
    L.poli([(44, 54), (52, 54), (54, 64), (42, 64)], 'W')
    L.hl(28, 39, 40, 'I'); L.hl(28, 38, 40, 'i')
    for x in range(30, 66, 6):              # the arch's stones
        L.set(x, 39, '9')
    L.rect(x0, 64, x1 - x0, 6, '9'); L.hl(x0, 64, x1 - x0, 'I')
    L.rect(x0 + 2, 66, x1 - x0 - 4, 2, '8')
    return L


def barco():
    L = Lienzo(96, 72)
    # hull (1x: rows 25..33, wide)
    L.poli([(10, 44), (92, 44), (82, 66), (22, 66)], '!')
    L.poli([(12, 45), (90, 45), (81, 64), (23, 64)], 'j')
    for y in range(47, 64, 4):
        a = 12 + (y - 45) * 11 // 19; b = 90 - (y - 45) * 9 // 19
        L.hl(a, y, b - a, 'J')
        L.hl(a, y + 1, b - a, '0')
    L.hl(10, 44, 82, 'u'); L.hl(10, 43, 82, '*')
    L.hl(22, 66, 60, '&'); L.hl(18, 67, 70, '%')
    # the deckhouse
    L.rect(34, 30, 30, 14, 'g'); L.hl(34, 30, 30, 'G'); L.vl(63, 30, 14, '>')
    for x in (38, 46, 54):
        L.rect(x, 34, 5, 5, 'c'); L.set(x, 34, '[')
    L.rect(32, 27, 34, 3, 'D'); L.hl(32, 27, 34, 'g')
    # the crane: mast, jib, hook (1x: the T at the top)
    L.rect(47, 4, 4, 24, 'D'); L.vl(47, 4, 24, 'g')
    L.rect(26, 4, 46, 3, 'd'); L.hl(26, 4, 46, 'g')
    L.linea(28, 7, 28, 18, 'x'); L.rect(26, 18, 5, 3, 'y')
    L.linea(51, 6, 70, 4, '>')
    L.rect(47, 0, 4, 4, 'r'); L.set(47, 0, '_')
    return L


def torre():
    L = Lienzo(48, 72)
    for y in range(4, 68):
        a = 18 - (y * 10) // 68; b = 30 + (y * 10) // 68
        L.set(a, y, 'D'); L.set(b, y, 'D'); L.set(a + 1, y, 'g')
    for y in range(8, 64, 8):                # the lattice
        a = 18 - (y * 10) // 68; b = 30 + (y * 10) // 68
        L.linea(a, y, b, y + 8, '>'); L.linea(b, y, a, y + 8, '>')
        L.hl(a, y, b - a + 1, 'g')
    L.rect(14, 12, 20, 2, 'g'); L.rect(10, 26, 28, 2, 'g')     # cross arms
    for x in (14, 33, 10, 37):
        L.vl(x, 14 if x in (14, 33) else 28, 4, 'y')
    L.rect(22, 0, 4, 5, 'D'); L.set(23, 0, 'r'); L.set(24, 0, '_')
    L.rect(6, 66, 36, 4, 'I'); L.hl(6, 66, 36, 'i')
    return L


def estatua():
    L = Lienzo(48, 72)
    # pedestal (1x: rows 28..35)
    L.rect(6, 56, 36, 12, '8'); L.hl(6, 56, 36, '7'); L.vl(6, 56, 12, 'i')
    L.hl(6, 67, 36, '9'); L.vl(41, 56, 12, 'I')
    L.rect(10, 52, 28, 4, 'i'); L.hl(10, 52, 28, '7')
    L.rect(4, 68, 40, 3, '9')
    L.hl(16, 61, 16, 'I'); L.hl(16, 62, 16, '7')   # the plaque
    # the robot statue, in pale stone
    L.rect(16, 36, 7, 16, 'i'); L.rect(25, 36, 7, 16, 'i')        # legs
    L.vl(16, 36, 16, '7'); L.vl(31, 36, 16, '8')
    L.rect(12, 18, 24, 19, 'i'); L.hl(12, 18, 24, '7'); L.vl(12, 18, 19, '7')
    L.vl(35, 18, 19, '8'); L.hl(13, 36, 22, '8')
    L.rect(17, 23, 14, 8, '8'); L.rect(18, 24, 12, 6, 'i')     # chest plate
    L.rect(6, 18, 6, 16, 'i'); L.vl(6, 18, 16, '7'); L.hl(6, 33, 6, '8')  # arms
    L.rect(36, 12, 6, 12, 'i'); L.vl(41, 12, 12, '8')          # one arm raised
    L.rect(36, 8, 7, 5, '7')
    L.rect(15, 4, 18, 14, 'i'); L.hl(15, 4, 18, '7'); L.vl(15, 4, 14, '7')
    L.vl(32, 4, 14, '8'); L.hl(16, 17, 16, '8')
    L.rect(18, 9, 4, 3, 'I'); L.rect(26, 9, 4, 3, 'I')        # eyes
    L.hl(19, 14, 10, 'I')
    L.vl(24, 0, 4, '8'); L.set(24, 0, '7')
    L.contorno('9')
    return L


def servidor():
    L = Lienzo(48, 72)
    L.rect(4, 2, 40, 66, 'K')
    L.rect(5, 3, 38, 64, 'x')
    L.vl(5, 3, 64, 'd'); L.hl(5, 3, 38, 'd')
    for i in range(10):
        y = 6 + i * 6
        L.rect(8, y, 32, 5, 'S')
        L.hl(8, y, 32, 's')
        for x in range(10, 30, 3):
            L.set(x, y + 2, 'K')
        L.set(33, y + 2, 'v' if i % 3 else 'r')
        L.set(36, y + 2, 'y' if i % 2 else 'v')
        L.set(33, y + 1, '_' if i % 3 == 0 else '2')
    L.rect(4, 68, 40, 3, 'k')
    return L


def maquina():
    L = Lienzo(48, 48)
    L.rect(2, 2, 44, 42, 'K')
    L.rect(3, 3, 42, 40, 'd')
    L.hl(3, 3, 42, 'D'); L.vl(3, 3, 40, 'D')
    L.rect(8, 8, 32, 16, 'k')                # the screen
    L.rect(9, 9, 30, 14, 'C')
    L.rect(10, 10, 28, 12, 'c')
    L.hl(12, 13, 20, '^'); L.hl(12, 17, 14, '[')
    L.rect(26, 16, 8, 3, 'C')
    L.hl(10, 10, 28, '[')
    L.rect(8, 28, 28, 4, 'x')                 # the slot
    L.hl(8, 31, 28, 'D')
    for i, c in enumerate(('r', 'y', 'v', 'b')):   # the buttons
        L.rect(8 + i * 7, 35, 5, 4, c)
        L.set(8 + i * 7, 35, 'w')
        L.hl(8 + i * 7, 39, 5, 'K')
    L.rect(2, 44, 44, 3, 'k')
    return L


def pila():
    """The scrap pile: a heap of rusted plates and a wheel, with a handle."""
    rnd = Random(105)
    L = Lienzo(48, 48)
    L.poli([(4, 44), (24, 8), (44, 44)], ',')
    L.poli([(6, 43), (24, 11), (42, 43)], 'U')
    for _ in range(14):
        x, y = rnd.randrange(10, 38), rnd.randrange(16, 42)
        if L.get(x, y) == 'U':
            w = rnd.randrange(4, 9)
            L.poli([(x, y), (x + w, y - 2), (x + w + 1, y + 2), (x + 1, y + 3)],
                   rnd.choice(('u', 'u', 'j', 'D')), sobre='Uu jD,')
            L.linea(x, y, x + w, y - 2, '*')
    L.disco(30, 32, 5, 'x'); L.disco(30, 32, 3, 'D'); L.disco(30, 32, 1, 'g')
    L.rect(40, 20, 3, 7, 'u'); L.rect(40, 20, 6, 2, 'u'); L.set(45, 20, '*')
    L.hl(4, 44, 40, 'k')
    return L


def cartel():
    """The noticeboard: a framed board on two posts, papers pinned to it."""
    L = Lienzo(48, 28)
    L.rect(10, 18, 4, 10, 'J'); L.vl(10, 18, 10, '0')
    L.rect(34, 18, 4, 10, 'J'); L.vl(34, 18, 10, '0')
    L.rect(2, 0, 44, 20, '!')
    L.rect(3, 1, 42, 18, 'j'); L.hl(3, 1, 42, '0')
    L.rect(6, 4, 36, 12, 'G')
    L.hl(6, 4, 36, 'w'); L.hl(6, 15, 36, '<')
    for y in (7, 10, 13):
        L.hl(9, y, 30 - (y % 4) * 3, '>')
    L.set(9, 5, 'r'); L.set(38, 5, 'r')
    return L


def farola():
    L = Lienzo(24, 48)
    L.rect(10, 12, 4, 32, 'D'); L.vl(10, 12, 32, '<'); L.vl(13, 12, 32, 'x')
    L.rect(7, 42, 10, 4, 'x'); L.hl(7, 42, 10, 'D')
    L.rect(6, 46, 12, 2, 'k')
    L.disco(12, 7, 7, '`' if False else 'y')
    L.disco(12, 7, 6, 'W')
    L.disco(11, 6, 3, 'w')
    L.rect(6, 12, 12, 2, 'x'); L.hl(6, 12, 12, 'g')
    L.rect(9, 0, 6, 1, 'x')
    return L


# ---------------------------------------------------------------- furniture
def mesa():
    L = Lienzo(48, 36)
    L.rect(0, 6, 48, 6, '!')
    L.rect(1, 6, 46, 5, 'j'); L.hl(1, 6, 46, '0'); L.hl(1, 10, 46, 'J')
    L.rect(4, 0, 40, 7, 'G')                # the cloth
    L.hl(4, 0, 40, 'w'); L.hl(4, 6, 40, '<')
    for x in range(4, 44, 6):
        L.vl(x, 7, 3, '<')
    for x in (4, 40):
        L.rect(x, 12, 4, 22, 'J'); L.vl(x, 12, 22, 'j'); L.hl(x, 33, 4, '!')
    L.rect(22, 1, 4, 5, 'b'); L.set(22, 1, '=')   # a cup
    return L


def silla():
    L = Lienzo(24, 40)
    L.rect(4, 0, 16, 20, '!')
    L.rect(5, 1, 14, 18, 'j'); L.hl(5, 1, 14, '0')
    L.rect(8, 4, 8, 12, 'J'); L.rect(9, 5, 6, 10, 'A'); L.hl(9, 5, 6, 'a')
    L.rect(2, 20, 20, 5, '!'); L.rect(3, 20, 18, 4, 'j'); L.hl(3, 20, 18, '0')
    for x in (3, 18):
        L.rect(x, 25, 3, 14, 'J'); L.vl(x, 25, 14, 'j')
    return L


def estante():
    L = Lienzo(48, 64)
    L.rect(0, 0, 48, 64, '!')
    L.rect(2, 2, 44, 60, 'J')
    L.vl(2, 2, 60, 'j'); L.hl(2, 2, 44, 'j')
    colores = ('r', 'b', 'v', 'y', 'm', 'c', 'o', 'p')
    for s in range(4):
        y = 4 + s * 15
        L.rect(3, y + 12, 42, 3, 'j'); L.hl(3, y + 12, 42, '0')
        x = 4
        i = s * 3
        while x < 43:
            w = 3 + (i % 2)
            h = 9 + (i % 3)
            c = colores[i % len(colores)]
            L.rect(x, y + 12 - h, w, h, c)
            L.vl(x, y + 12 - h, h, 'w' if c in 'yc' else '7')
            L.hl(x, y + 12 - h + 2, w, 'k')
            x += w + 1; i += 1
    L.rect(0, 62, 48, 2, 'k')
    return L


def compu():
    L = Lienzo(24, 44)
    L.rect(1, 2, 22, 18, 'g'); L.hl(1, 2, 22, 'G'); L.vl(22, 2, 18, '>')
    L.rect(3, 4, 18, 13, 'k'); L.rect(4, 5, 16, 11, 'C')
    L.hl(5, 7, 10, '^'); L.hl(5, 10, 7, 'c'); L.hl(5, 13, 12, 'c')
    L.set(19, 18, 'v')
    L.rect(9, 20, 6, 4, '>'); L.rect(5, 24, 14, 2, 'g')
    L.rect(2, 30, 20, 5, 'D'); L.hl(2, 30, 20, 'g')
    for x in range(3, 21, 2):
        L.set(x, 32, 'x')
    L.rect(1, 36, 22, 8, 'j'); L.hl(1, 36, 22, '0'); L.hl(1, 43, 22, '!')
    return L


def planta():
    rnd = Random(106)
    L = Lienzo(24, 48)
    L.poli([(5, 34), (19, 34), (17, 47), (7, 47)], 'U')
    L.poli([(6, 34), (13, 34), (12, 47), (8, 47)], 'u')
    L.rect(4, 32, 16, 3, '*'); L.hl(4, 34, 16, ',')
    for i in range(7):                       # the leaves, fanning up
        a = math.radians(-150 + i * 20)
        x1, y1 = 12 + math.cos(a) * 11, 30 + math.sin(a) * 26
        L.linea(12, 32, x1, y1, '3')
        L.elipse((12 + x1) / 2 + 1, (32 + y1) / 2, 2.5, 3.5, 'E')
        L.set((12 + x1) / 2, (32 + y1) / 2 - 2, '2')
    L.contorno('4', de='3E2')
    return L


def vasija():
    L = Lienzo(24, 36)
    L.elipse(12, 22, 10, 12, ',')
    L.elipse(12, 22, 9, 11, 'U')
    L.elipse(11, 21, 7, 9, 'u')
    L.elipse(9, 18, 3, 4, '*')
    L.rect(7, 4, 10, 6, 'U'); L.rect(8, 4, 8, 5, 'u'); L.hl(6, 3, 12, '*')
    for y in (18, 26):
        L.hl(4, y, 16, 'y') if y == 18 else L.hl(4, y, 16, ',')
    L.set(8, 18, 'W')
    return L


def cuadro():
    L = Lienzo(24, 28)
    L.rect(0, 0, 24, 22, 'Y'); L.hl(0, 0, 24, 'y'); L.vl(0, 0, 22, 'y')
    L.hl(0, 21, 24, 'O')
    L.rect(3, 3, 18, 16, '=')                # a landscape: sky, hill, sun
    L.rect(3, 12, 18, 7, 'E')
    L.elipse(9, 14, 8, 3, '2', sobre='E=')
    L.disco(16, 7, 2, 'y')
    L.hl(3, 18, 18, '3')
    L.linea(10, 22, 12, 26, 'D'); L.linea(14, 22, 12, 26, 'D')
    return L


def cama():
    L = Lienzo(48, 60)
    L.rect(0, 0, 48, 12, '!'); L.rect(1, 1, 46, 10, 'j'); L.hl(1, 1, 46, '0')
    L.rect(2, 12, 44, 38, 'J')
    L.rect(4, 10, 40, 10, 'w'); L.hl(4, 10, 40, 'w'); L.hl(4, 19, 40, '<')   # pillow
    L.rect(4, 20, 40, 28, 'B')               # the quilt: squares
    for y in range(20, 48, 7):
        for x in range(4, 44, 8):
            L.rect(x, y, 4, 4, 'b')
            L.rect(x + 4, y + 3, 4, 4, 'b')
    L.hl(4, 20, 40, '='); L.hl(4, 47, 40, '`')
    for x in (2, 42):
        L.rect(x, 48, 4, 12, 'J'); L.vl(x, 48, 12, 'j')
    return L


def banco():
    L = Lienzo(48, 36)
    for y in (2, 8):                          # the backrest slats
        L.rect(2, y, 44, 4, 'g'); L.hl(2, y, 44, 'G'); L.hl(2, y + 3, 44, '>')
    L.rect(0, 16, 48, 5, 'J'); L.hl(0, 16, 48, '0'); L.rect(1, 17, 46, 3, 'j')
    L.hl(0, 21, 48, '!')
    for x in (4, 40):
        L.rect(x, 0, 4, 34, 'D'); L.vl(x, 0, 34, 'g'); L.vl(x + 3, 0, 34, 'x')
    L.rect(2, 34, 44, 2, 'k')
    return L


def cesto():
    L = Lienzo(24, 32)
    L.rect(2, 2, 20, 4, 'g'); L.hl(2, 2, 20, 'G'); L.hl(2, 5, 20, 'x')
    L.poli([(3, 6), (21, 6), (19, 30), (5, 30)], '>')
    L.poli([(4, 6), (20, 6), (18, 30), (6, 30)], 'g')
    for x in range(7, 18, 3):
        L.vl(x, 8, 20, '>')
    L.vl(6, 8, 20, 'G')
    L.hl(5, 30, 14, 'k')
    L.rect(9, 0, 6, 2, 'D')
    return L


def maceta():
    L = planta()
    M = Lienzo(24, 36)
    for y in range(36):
        for x in range(24):
            M.p[y][x] = L.p[y + 12][x]
    return M


# ---------------------------------------------------------------- animals
def gato():
    L = Lienzo(32, 28)
    L.elipse(16, 18, 11, 6.5, 'k')           # body
    L.elipse(14, 16, 8, 3.5, 'K')            # the sheen along the back
    L.rect(6, 22, 3, 5, 'k'); L.rect(22, 22, 3, 5, 'k')
    L.rect(11, 23, 3, 4, 'k'); L.rect(18, 23, 3, 4, 'k')
    L.disco(25, 11, 5.5, 'k')                # head
    L.poli([(20, 9), (21, 2), (24, 7)], 'k'); L.poli([(26, 7), (29, 2), (30, 9)], 'k')
    L.set(22, 5, 'M'); L.set(28, 5, 'M')     # inside the ears
    L.rect(22, 10, 2, 2, 'v'); L.rect(27, 10, 2, 2, 'v')   # eyes
    L.set(22, 10, '1'); L.set(27, 10, '1')
    L.set(25, 13, 'm')
    L.hl(29, 13, 3, 'D'); L.hl(29, 15, 3, 'D')
    for i in range(8):                        # the tail, curling up
        L.rect(5 - i // 3, 16 - i, 2, 2, 'k')
    L.set(22, 8, 'K')
    return L


def pajaro():
    L = Lienzo(28, 20)
    L.elipse(13, 11, 8, 5.5, 'b')
    L.elipse(12, 10, 6, 3.5, '=')
    L.elipse(14, 13, 5, 2.5, 'w')            # breast
    L.disco(20, 7, 4.5, 'b'); L.disco(19, 6, 2.5, '=')
    L.rect(21, 5, 2, 2, 'k'); L.set(21, 5, 'w')
    L.poli([(24, 7), (27, 8), (24, 9)], 'o')
    L.poli([(2, 8), (8, 9), (8, 12), (1, 11)], 'B')   # the tail
    L.poli([(8, 9), (15, 8), (13, 13)], 'B')          # wing
    L.linea(9, 10, 14, 9, '`')
    L.vl(12, 16, 3, 'o'); L.vl(15, 16, 3, 'o')
    L.hl(11, 19, 3, 'O'); L.hl(14, 19, 3, 'O')
    return L


# ---------------------------------------------------------------- people
def persona(pelo, pelo2, ropa, ropa2, abajo, abajo2, zapato, estilo=''):
    """A townsperson, 30x48, facing you. Big head, the game's proportions."""
    L = Lienzo(30, 48)
    k = '!'                                  # the outline: a warm dark
    # legs and shoes
    L.rect(9, 36, 5, 8, abajo); L.rect(16, 36, 5, 8, abajo)
    L.vl(13, 36, 8, abajo2); L.vl(20, 36, 8, abajo2)
    L.rect(8, 43, 7, 4, zapato); L.rect(15, 43, 7, 4, zapato)
    L.hl(8, 43, 6, 'D' if zapato in 'kK' else '0')
    # body
    L.poli([(6, 20), (24, 20), (26, 37), (4, 37)], ropa)
    L.poli([(17, 20), (24, 20), (26, 37), (19, 37)], ropa2)
    L.hl(7, 20, 16, '7' if ropa in 'wG' else ropa)
    L.rect(12, 20, 6, 3, ';')                # the neck's shadow / collar
    L.hl(12, 20, 6, ':')
    # arms and hands
    L.rect(2, 22, 4, 13, ropa); L.vl(2, 22, 13, ropa2)
    L.rect(24, 22, 4, 13, ropa2)
    L.rect(2, 34, 4, 3, 'h'); L.rect(24, 34, 4, 3, ';')
    # head
    L.rect(7, 4, 16, 16, 'h')
    L.rect(7, 4, 3, 16, ':')
    L.vl(22, 5, 14, ';'); L.hl(8, 19, 14, ';')
    L.rect(9, 10, 4, 4, 'w'); L.rect(17, 10, 4, 4, 'w')   # eyes
    L.rect(11, 11, 2, 3, 'k'); L.rect(19, 11, 2, 3, 'k')
    L.set(11, 11, 'w'); L.set(19, 11, 'w')
    L.rect(9, 15, 3, 1, '_'); L.rect(19, 15, 3, 1, '_')   # cheeks
    L.hl(13, 16, 4, 'A')                                  # mouth
    L.hl(13, 17, 4, ';')
    # hair
    L.rect(6, 0, 18, 5, pelo)
    L.rect(6, 5, 2, 7, pelo); L.rect(22, 5, 2, 7, pelo)
    L.hl(8, 1, 12, pelo2 if False else pelo)
    L.hl(8, 0, 10, pelo2)
    L.hl(7, 5, 16, pelo)
    L.set(9, 6, pelo); L.set(14, 6, pelo); L.set(20, 6, pelo)
    if 'gafas' in estilo:
        L.marco(8, 9, 6, 6, 'D'); L.marco(16, 9, 6, 6, 'D'); L.hl(14, 11, 2, 'D')
    if 'rodete' in estilo:
        L.disco(15, 0, 3.5, pelo); L.set(14, -1, pelo2)
    if 'gorra' in estilo:
        L.rect(5, 0, 20, 5, 'r'); L.hl(5, 0, 20, '_'); L.rect(15, 4, 12, 2, 'R')
    if 'bigote' in estilo:
        L.hl(12, 15, 6, pelo); L.hl(13, 16, 4, 'A')
    if 'largo' in estilo:
        L.rect(4, 4, 3, 22, pelo); L.rect(23, 4, 3, 22, pelo)
        L.vl(4, 4, 22, pelo2)
    if 'delantal' in estilo:
        L.poli([(10, 24), (20, 24), (21, 37), (9, 37)], 'w')
        L.hl(10, 24, 10, '<')
    L.contorno(k)
    return L


# ---------------------------------------------------------------- things
def cofre(abierto=False):
    L = Lienzo(36, 30)
    # the body: wood with gold bands
    L.rect(0, 12, 36, 12, '!')
    L.rect(1, 12, 34, 11, 'j'); L.hl(1, 12, 34, '0'); L.hl(1, 22, 34, 'J')
    for x in (3, 30):
        L.rect(x, 12, 3, 12, 'Y'); L.vl(x, 12, 12, 'y')
    L.rect(0, 23, 36, 2, 'Y'); L.hl(0, 23, 36, 'y')
    if abierto:
        L.rect(3, 0, 30, 10, '!'); L.rect(4, 1, 28, 8, 'J')      # the lid, open
        L.rect(6, 2, 24, 6, 'k'); L.hl(6, 2, 24, 'K')
        L.rect(2, 9, 32, 4, 'k')                                 # the empty inside
        L.hl(2, 12, 32, 'Y')
    else:
        L.rect(2, 2, 32, 10, '!')
        L.rect(3, 3, 30, 9, 'j'); L.hl(3, 3, 30, '0')
        L.hl(4, 2, 28, '0')
        for x in (3, 30):
            L.rect(x, 3, 3, 9, 'Y'); L.vl(x, 3, 9, 'y')
        L.rect(14, 8, 8, 8, 'Y'); L.rect(15, 9, 6, 6, 'y')      # the lock
        L.rect(17, 11, 2, 3, 'k'); L.set(15, 9, 'W')
    L.rect(4, 25, 28, 3, 'U'); L.hl(4, 27, 28, ',')
    return L


def signo():
    """A sign on a post: the thing that says something when touched."""
    L = Lienzo(30, 40)
    L.rect(12, 16, 6, 16, 'J'); L.vl(12, 16, 16, 'j'); L.vl(17, 16, 16, '!')
    L.rect(8, 26, 14, 6, 'J'); L.hl(8, 26, 14, 'j')
    L.rect(4, 0, 22, 18, '!')
    L.rect(5, 1, 20, 16, 'j'); L.hl(5, 1, 20, '0'); L.vl(5, 1, 16, '0')
    L.rect(7, 3, 16, 12, 'G'); L.hl(7, 3, 16, 'w'); L.hl(7, 14, 16, '<')
    for y in (6, 9, 12):
        L.hl(9, y, 12 - (y % 3) * 2, 'D')
    return L


def cabina():
    L = Lienzo(48, 84)
    # the aerial and its lamp
    L.rect(22, 0, 4, 10, 'x'); L.vl(22, 0, 10, 'D')
    L.disco(24, 3, 3, 'y'); L.set(23, 2, 'W')
    # the box
    L.rect(4, 10, 40, 68, '`')
    L.rect(5, 11, 38, 66, 'B')
    L.vl(5, 11, 66, 'b'); L.hl(5, 11, 38, 'b')
    L.rect(5, 11, 38, 8, 'B')
    L.rect(8, 13, 32, 4, 'y'); L.hl(8, 13, 32, 'W')     # the lit sign
    L.hl(12, 15, 24, 'Y')
    # the glass door, with its reflections
    L.rect(9, 21, 30, 50, '`')
    L.rect(10, 22, 28, 48, 'c')
    for y in range(22, 70):              # the reflections: diagonals on the glass
        for x in range(10, 38):
            d = (x + y) % 16
            if d == 0: L.set(x, y, '^')
            elif d == 1: L.set(x, y, '[')
    L.rect(10, 60, 28, 10, 'C')
    L.hl(10, 60, 28, ']')
    L.rect(26, 30, 8, 14, 'g'); L.rect(27, 31, 6, 12, '>')   # the phone
    L.rect(28, 33, 4, 2, 'k'); L.linea(26, 40, 22, 46, 'k')
    L.vl(24, 22, 48, '`')                              # the door's frame
    L.rect(34, 46, 2, 5, 'y')
    L.rect(4, 76, 40, 6, 'K'); L.hl(4, 76, 40, 'x')
    return L


def puesto_chatarra():
    """The scrap dealer's stall: a low bench with parts on it. 72x28 over
    the same box the watch filled with rectangles (x-6..30, y-9..10)."""
    L = Lienzo(72, 40)
    L.rect(0, 14, 72, 24, '!')
    L.rect(1, 15, 70, 22, 'J')
    for x in range(1, 71, 7):
        L.vl(x, 15, 22, 'j')
    L.rect(0, 10, 72, 6, 'j'); L.hl(0, 10, 72, '0'); L.hl(0, 15, 72, '!')
    for i, (c, c2) in enumerate((('g', '<'), ('u', '*'), ('D', 'g'), ('u', '*'))):
        x = 4 + i * 17
        L.rect(x, 0, 12, 10, 'k')
        L.rect(x + 1, 1, 10, 9, c)
        L.hl(x + 1, 1, 10, c2)
        L.rect(x + 3, 3, 3, 3, 'k'); L.set(x + 3, 3, c2)
    L.rect(0, 37, 72, 3, 'k')
    return L


def puesto_feria():
    """The fair's stall: a striped awning over a counter with the belt."""
    L = Lienzo(72, 56)
    for i in range(6):                        # the awning, scalloped
        x = i * 12
        L.rect(x, 0, 12, 18, 'r' if i % 2 else '(')
        L.vl(x, 0, 18, 'R' if i % 2 else 'q')
        L.elipse(x + 6, 18, 6, 3, 'r' if i % 2 else '(')
    L.hl(0, 0, 72, 'k'); L.hl(0, 1, 72, '_')
    L.rect(4, 22, 4, 16, 'g'); L.rect(64, 22, 4, 16, 'g')   # poles
    L.rect(0, 30, 72, 22, 'k')
    L.rect(1, 31, 70, 20, 'd'); L.hl(1, 31, 70, 'D')
    L.rect(1, 36, 70, 4, 'x'); L.hl(1, 36, 70, 'K')          # the belt
    for x in range(3, 70, 6):
        L.set(x, 38, '>')
    for i, c in enumerate(('g', 'u', 'g', 'v', 'u')):
        L.rect(6 + i * 13, 32, 8, 5, c); L.hl(6 + i * 13, 32, 8, 'w' if c == 'g' else c)
    L.rect(0, 52, 72, 3, 'k')
    return L


def barrera():
    """The control, closed: a striped barrier across the cell."""
    L = Lienzo(24, 24)
    L.rect(0, 1, 24, 22, 'k')
    L.rect(1, 2, 22, 20, 'x')
    for x in range(-4, 24, 8):
        L.poli([(x, 22), (x + 4, 22), (x + 12, 2), (x + 8, 2)], 'y', sobre='x')
    L.hl(1, 2, 22, 'W'); L.hl(1, 21, 22, 'K')
    L.rect(0, 0, 24, 1, 'k'); L.rect(0, 23, 24, 1, 'k')
    return L


def escotilla():
    """The dungeon's hatch: a grated trapdoor in a riveted frame."""
    L = Lienzo(24, 24)
    L.rect(0, 2, 24, 20, 'k')
    L.rect(1, 3, 22, 18, '>')
    L.hl(1, 3, 22, 'G'); L.vl(1, 3, 18, 'g'); L.hl(1, 20, 22, 'D')
    L.rect(3, 5, 18, 14, 'k')
    for x in range(4, 20, 3):
        L.vl(x, 5, 14, 'x'); L.vl(x + 1, 5, 14, 'D')
    L.hl(3, 5, 18, 'd')
    for x, y in ((2, 4), (20, 4), (2, 18), (20, 18)):
        L.set(x, y, 'w')
    L.rect(10, 11, 4, 3, 'y'); L.set(10, 11, 'W')
    return L


COSAS = {
    'SP_ARBOL':     arbol,
    'SP_CASA':      casa,
    'SP_TALLER':    taller,
    'SP_FUENTE':    fuente,
    'SP_CARTEL':    cartel,
    'SP_FAROLA':    farola,
    'SP_MAQUINA':   maquina,
    'SP_PILA':      pila,
    'SP_PINO':      pino,
    'SP_TORRE':     torre,
    'SP_ESTATUA':   estatua,
    'SP_SERVIDOR':  servidor,
    'SP_HORNO':     horno,
    'SP_BARCO':     barco,
    'MU_MESA_PX':   mesa,
    'MU_SILLA_PX':  silla,
    'MU_ESTANTE_PX': estante,
    'MU_COMPU_PX':  compu,
    'MU_PLANTA_PX': planta,
    'MU_VASIJA_PX': vasija,
    'MU_CUADRO_PX': cuadro,
    'MU_CAMA_PX':   cama,
    'MU_BANCO_PX':  banco,
    'MU_CESTO_PX':  cesto,
    'MU_MACETA_PX': maceta,
    'AN_GATO_D':    gato,
    'AN_GATO_I':    lambda: gato().espejo(),
    'AN_PAJARO_D':  pajaro,
    'AN_PAJARO_I':  lambda: pajaro().espejo(),
    'SP_ABUELA':    lambda: persona('?', 'w', 'p', 'P', 'p', 'P', 'd', 'gafas rodete'),
    'SP_CHICO':     lambda: persona('o', 'y', 'c', 'C', 'b', 'B', 'K', 'gorra'),
    'SP_VECINO':    lambda: persona('J', '0', 'v', 'V', 'j', 'J', 'K', 'bigote'),
    'SP_SENORA':    lambda: persona('Y', 'y', 'm', 'M', 'm', 'M', 'd', 'largo delantal'),
    'SP_COFRE':     lambda: cofre(False),
    'SP_COFRE_ABIERTO': lambda: cofre(True),
    'SP_SIGNO':     signo,
    'SP_CABINA':    cabina,
    # 2x only: the watch drew these with rectangles
    'SP_PUESTO_CHATARRA': puesto_chatarra,
    'SP_PUESTO_FERIA':    puesto_feria,
    'SP_BARRERA':         barrera,
    'SP_ESCOTILLA':       escotilla,
}
