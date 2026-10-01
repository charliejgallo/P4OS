"""Chatarra's ground at 2x: the 24x24 tiles and the fringes (ASSETS.md).

Every function returns a Lienzo drawn in wrap mode, so a tile continues into
its own copy on every side: the map repeats them in rows and columns.
"""
from random import Random
from lienzo import Lienzo

T = 24                                   # a 12-unit cell, at 2x


def _base(c):
    return Lienzo(T, T, c, envolver=True)


def _manchas(L, rnd, n, c, rmin=2, rmax=4, sobre=None):
    for _ in range(n):
        L.elipse(rnd.uniform(0, T), rnd.uniform(0, T), rnd.uniform(rmin, rmax),
                 rnd.uniform(rmin * 0.6, rmax * 0.7), c, sobre)


def _mata(L, x, y, alto, pie, tallo, punta, rnd):
    """A tuft: three blades leaning out from one base."""
    for dx, h in ((-1, alto - 1), (0, alto), (1, alto - 2)):
        lean = -1 if dx < 0 else (1 if dx > 0 else 0)
        for k in range(h):
            L.set(x + dx + (lean if k >= h // 2 else 0), y - k, tallo)
        L.set(x + dx + (lean if h > 1 else 0), y - h + 1, punta)
    L.set(x, y + 1, pie)


# ---------------------------------------------------------------- grass
def pasto(seed, matas=7, flores=0):
    rnd = Random(seed)
    L = _base('e')
    _manchas(L, rnd, 4, '2', 2, 4)
    _manchas(L, rnd, 3, '1', 1, 2)
    # the mid-tone patches, thinned to one pixel in three: mottling, not blobs
    for y in range(T):
        for x in range(T):
            if L.get(x, y) == '2' and (x * 7 + y * 13 + seed) % 3:
                L.set(x, y, 'e')
    for _ in range(matas):
        _mata(L, rnd.randrange(T), rnd.randrange(T), rnd.choice((3, 4)), '3', 'E', '1', rnd)
    L.salpicar(rnd, 5, '1', sobre='e')
    flor = ('r', 'y', 'w', 'm', 'c')
    for i in range(flores):
        x, y = rnd.randrange(T), rnd.randrange(T)
        c = flor[(seed + i) % len(flor)]
        for dx, dy in ((0, -1), (-1, 0), (1, 0), (0, 1)):
            L.set(x + dx, y + dy, c)
        L.set(x, y, 'y' if c != 'y' else 'o')
        L.set(x + 1, y + 2, 'E')
    return L


def pasto_c(seed):
    """The town's denser grass: more tufts, darker."""
    L = pasto(seed, matas=13)
    rnd = Random(seed * 3 + 1)
    for _ in range(5):
        _mata(L, rnd.randrange(T), rnd.randrange(T), 5, '3', '3', 'e', rnd)
    return L


def flores(seed):
    rnd = Random(seed)
    L = pasto(seed, matas=4)
    colores = ('r', 'm', 'y', 'w', 'c', 'p')
    for i in range(6):
        x, y = rnd.randrange(T), rnd.randrange(T)
        c = colores[(i + seed) % len(colores)]
        L.set(x - 1, y + 2, 'E'); L.set(x, y + 1, 'E'); L.set(x + 1, y + 2, '3')
        for dx, dy in ((0, -1), (-1, 0), (1, 0), (0, 1)):
            L.set(x + dx, y + dy, c)
        L.set(x + 1, y - 1, {'r': '_', 'm': '~', 'y': 'z', 'w': '7',
                              'c': '[', 'p': '}'}[c])
        L.set(x, y, 'o' if c in 'wy' else 'y')
    return L


def alto(seed, base='E', tallos=('3', 'f', '2'), puntas=('1', 'e'), pie='F'):
    """Tall grass: dense upright blades. Scrub, snowy scrub and dry scrub are
    this same silhouette in other colours - all three hide creatures."""
    rnd = Random(seed)
    L = _base(base)
    _manchas(L, rnd, 5, pie, 2, 3)
    for fila in range(4):
        y0 = 5 + fila * 6
        for i in range(7):
            x = (i * 24 // 7 + fila * 2 + rnd.randrange(3)) % T
            h = rnd.choice((5, 6, 7))
            c = tallos[(i + fila) % len(tallos)]
            lean = rnd.choice((-1, 0, 1))
            for k in range(h):
                L.set(x + (lean if k > h // 2 else 0), y0 - k, c)
            L.set(x + lean, y0 - h + 1, puntas[(i + fila) % len(puntas)])
            L.set(x, y0 + 1, pie)
    return L


# ---------------------------------------------------------------- earth
def tierra(seed, base='h', claro='6', oscuro='H', hondo='5'):
    rnd = Random(seed)
    L = _base(base)
    _manchas(L, rnd, 4, claro, 2, 4)
    _manchas(L, rnd, 3, oscuro, 1, 3)
    for y in range(T):                  # both patches softened into dither
        for x in range(T):
            v = L.get(x, y)
            if (v == oscuro and (x + y) & 1) or (v == claro and (x * 3 + y) % 3):
                L.set(x, y, base)
    for _ in range(2):                  # pebbles of the same earth, lit
        x, y = rnd.randrange(T), rnd.randrange(T)
        w = rnd.choice((2, 3))
        L.rect(x, y, w, 2, oscuro)
        L.set(x, y, claro)
        L.hl(x, y + 2, w, hondo)
    L.salpicar(rnd, 8, hondo, sobre=base)
    L.salpicar(rnd, 6, claro, sobre=base)
    return L


def seco(seed):
    """The wasteland's cracked earth: a network of cracks, each lit on its
    upper lip."""
    rnd = Random(seed)
    L = _base('Q')
    _manchas(L, rnd, 3, '(', 1, 3)
    for y in range(T):
        for x in range(T):
            if L.get(x, y) == '(' and (x * 3 + y) % 3:
                L.set(x, y, 'Q')
    for _ in range(3):                  # the cracks: dark, forking
        x, y = rnd.randrange(T), rnd.randrange(T)
        for _k in range(10):
            L.set(x, y, ')')
            if rnd.random() < 0.2: L.set(x, y + 1, ')')
            x += 1; y += rnd.choice((-1, 0, 0, 1))
    L.salpicar(rnd, 5, 'U', sobre='Q')
    return L


def arena(seed):
    rnd = Random(seed)
    L = _base('q')
    for k in range(4):                  # ripples: a wavy dark line with light above
        y0 = 3 + k * 6 + rnd.randrange(2)
        for x in range(T):
            y = y0 + (1 if (x // 5 + k) % 2 else 0)
            L.set(x, y, 'Q'); L.set(x, y - 1, '(')
    L.salpicar(rnd, 6, ')', sobre='q')
    L.salpicar(rnd, 4, '6', sobre='q')
    return L


def grava(seed, base='I'):
    rnd = Random(seed)
    L = _base(base)
    for _ in range(22):
        x, y = rnd.randrange(T), rnd.randrange(T)
        c = rnd.choice(('8', 'i', '8', '9'))
        L.rect(x, y, 2, 2, c)
        if c != '9': L.set(x, y, '7' if c == 'i' else 'i')
        L.set(x + 1, y + 2, '9')
    return L


def volcan(seed):
    rnd = Random(seed)
    L = _base('S')
    _manchas(L, rnd, 4, 's', 1, 3)
    for _ in range(5):
        x, y = rnd.randrange(T), rnd.randrange(T)
        L.rect(x, y, 3, 2, 'x'); L.set(x, y, 'd'); L.hl(x, y + 2, 3, 'K')
    for _ in range(4):
        x, y = rnd.randrange(T), rnd.randrange(T)
        L.set(x, y, 'u'); L.set(x + 1, y, 'o' if rnd.random() < .5 else 'U')
    return L


def nieve(seed):
    rnd = Random(seed)
    L = _base('G')
    _manchas(L, rnd, 3, '?', 2, 4)
    for y in range(T):
        for x in range(T):
            if L.get(x, y) == '?' and (x + 2 * y) % 3:
                L.set(x, y, 'G')
    _manchas(L, rnd, 3, '7', 1, 3)
    L.salpicar(rnd, 7, 'w', sobre='G7')
    return L


def hielo(seed):
    rnd = Random(seed)
    L = _base('c')
    _manchas(L, rnd, 4, '[', 1, 3)
    for _ in range(2):                  # cracks: a thin diagonal
        x, y = rnd.randrange(T), rnd.randrange(T)
        for _k in range(9):
            L.set(x, y, 'C'); x += 1; y += 1 if rnd.random() < 0.6 else 0
    for _ in range(2):                  # and the light running across it
        x, y = rnd.randrange(T), rnd.randrange(T)
        for _k in range(5):
            L.set(x + _k, y - _k, '[')
    for _ in range(3):                  # glints
        x, y = rnd.randrange(T), rnd.randrange(T)
        L.set(x, y, 'w'); L.set(x + 1, y + 1, '[')
    return L


# ---------------------------------------------------------------- water, lava
def agua(seed, base='l'):
    """Water: bands of crests. The map ROLLS this tile downwards to make it
    flow (ch_tile_anim), so everything is horizontal and wraps vertically."""
    rnd = Random(seed)
    L = _base(base)
    for k in range(6):
        y = k * 4 + (k & 1)
        x0 = rnd.randrange(T)
        L.hl(x0, y, rnd.choice((5, 6, 7)), '%')
        L.hl(x0 + 1, y - 1, 3, '$')
        x1 = (x0 + 12) % T
        L.hl(x1, y + 2, 4, 'L')
    L.salpicar(rnd, 4, '$', sobre=base)
    return L


def vado(seed):
    """The ford: shallow water over pebbles you can see through it."""
    rnd = Random(seed)
    L = agua(seed, base='l')
    for _ in range(5):
        x, y = rnd.randrange(T), rnd.randrange(T)
        L.elipse(x, y, 2.2, 1.4, '=', sobre='l%L')
        L.set(x - 1, y - 1, '$')
    return L


def lava(seed):
    rnd = Random(seed)
    L = _base('O')
    _manchas(L, rnd, 6, 'o', 2, 4)
    for _ in range(3):                  # the hot veins
        x, y = rnd.randrange(T), rnd.randrange(T)
        for _k in range(10):
            L.set(x, y, 'y'); x += 1; y += rnd.choice((0, 0, 1, -1))
    L.salpicar(rnd, 6, 'W', sobre='y')
    for _ in range(3):                  # crust drifting on it
        x, y = rnd.randrange(T), rnd.randrange(T)
        L.elipse(x, y, 2.4, 1.3, 'X')
        L.set(x - 1, y - 1, 'U')
        L.hl(x - 1, y + 1, 3, 'Z')
    return L


def aceite(seed):
    rnd = Random(seed)
    L = _base('K')
    _manchas(L, rnd, 3, 'k', 2, 4)
    for cx, cy, r, c in ((8, 9, 5, 'P'), (16, 16, 4, '{')):   # the sheen
        for a in range(200, 340, 8):
            import math
            L.set(cx + r * math.cos(math.radians(a)),
                  cy + r * 0.55 * math.sin(math.radians(a)), c)
    L.set(6, 7, 'p')
    for x, y in ((0, 0), (1, 0), (0, 1), (23, 0), (22, 0), (23, 1),
                 (0, 23), (1, 23), (0, 22), (23, 23), (22, 23), (23, 22)):
        L.set(x, y, 'd')
    return L


# ---------------------------------------------------------------- built floors
def adoquin(seed):
    """Sandstone setts in staggered rows, each one domed and lit."""
    L = _base(')')
    for fila in range(3):
        y = fila * 8
        off = 0 if fila % 2 == 0 else 6
        for i in range(2):
            x = off + i * 12
            L.rect(x + 1, y + 1, 10, 6, 'q')
            L.hl(x + 2, y + 1, 8, '(')
            L.vl(x + 1, y + 2, 4, '6')
            L.hl(x + 2, y + 6, 9, 'Q')
            L.vl(x + 10, y + 2, 4, 'Q')
            L.set(x + 1, y + 1, ')'); L.set(x + 10, y + 1, ')')
            L.set(x + 1, y + 6, ')'); L.set(x + 10, y + 6, ')')
            L.set(x + 4 + (fila + i + seed) % 4, y + 3, 'Q')
    return L


def losa(seed):
    L = _base('>')
    for j in range(2):
        for i in range(2):
            x, y = i * 12, j * 12
            L.rect(x + 1, y + 1, 11, 11, 'G')
            L.hl(x + 1, y + 1, 11, 'w'); L.vl(x + 1, y + 1, 11, '7')
            L.hl(x + 2, y + 11, 10, '<'); L.vl(x + 11, y + 2, 10, '<')
    rnd = Random(seed)
    L.salpicar(rnd, 6, '<', sobre='G')
    return L


def baldosa(seed):
    L = _base('g')
    for j in range(2):
        for i in range(2):
            x, y = i * 12, j * 12
            L.rect(x + 1, y, 11, 11, 'G')
            L.hl(x + 1, y, 11, 'w')
            L.set(x + 2, y + 1, 'w')
            L.hl(x + 1, y + 10, 11, '<')
    return L


def ciudad(seed):
    L = _base('D')
    for j in range(2):
        for i in range(2):
            x, y = i * 12, j * 12
            L.rect(x + 1, y + 1, 10, 10, 'g')
            L.hl(x + 1, y + 1, 10, 'G'); L.vl(x + 1, y + 1, 10, '<')
            L.hl(x + 2, y + 10, 9, '>')
    L.rect(11, 11, 2, 2, '8')
    return L


def parquet(seed):
    L = _base('j')
    for fila in range(3):
        y = fila * 8
        cortes = ((4, 16), (10, 22), (0, 13))[fila]
        L.hl(0, y, T, '!')
        L.hl(0, y + 1, T, '0')
        L.hl(0, y + 7, T, 'J')
        for c in cortes:
            L.vl(c, y + 1, 7, '!')
            L.vl(c + 1, y + 1, 6, '0')
        rnd = Random(seed * 7 + fila)
        for _ in range(3):              # the grain
            x = rnd.randrange(T)
            L.hl(x, y + 3 + rnd.randrange(3), rnd.choice((3, 4, 5)), 'J')
    return L


def madera(seed):
    """The wooden floor: wide planks, nailed at the ends."""
    L = _base('j')
    for fila in range(3):
        y = fila * 8
        L.hl(0, y + 7, T, '!')
        L.hl(0, y, T, '0')
        corte = (18, 6, 12)[fila]
        L.vl(corte, y, 7, '!')
        L.set(corte - 2, y + 2, 'D'); L.set(corte - 2, y + 5, 'D')
        L.set(corte + 2, y + 2, 'D'); L.set(corte + 2, y + 5, 'D')
        rnd = Random(seed + fila)
        for _ in range(3):
            x = rnd.randrange(T)
            L.hl(x, y + 2 + rnd.randrange(4), rnd.choice((4, 5, 6)), 'J')
    return L


def puente(seed):
    L = _base('J')
    for k in range(4):
        y = k * 6
        L.rect(0, y + 1, T, 4, 'j')
        L.hl(0, y + 1, T, '0')
        L.hl(0, y + 5, T, '!')
        L.set(2, y + 3, 'D'); L.set(21, y + 3, 'D')
    rnd = Random(seed)
    for _ in range(4):
        x, y = rnd.randrange(T), rnd.randrange(T)
        L.set_si(x, y, 'J', 'j')
    return L


def muelle(seed):
    """The pier: planks running away from the water, and their gaps."""
    L = _base('!')
    for k in range(4):
        x = k * 6
        L.rect(x, 0, 5, T, 'j')
        L.vl(x, 0, T, '0')
        L.vl(x + 4, 0, T, 'J')
        L.set(x + 2, 3 + k * 5 % T, 'D')
        L.set(x + 2, 15 + k * 3 % 8, 'D')
    rnd = Random(seed)
    for _ in range(6):
        x, y = rnd.randrange(T), rnd.randrange(T)
        L.set_si(x, y, 'J', 'j')
    return L


def alfombra(seed):
    """The carpet: a quiet field with a dotted diamond every cell."""
    L = _base('A')
    for y in range(T):
        for x in range(T):
            d = abs(x - 12) + abs(y - 12)
            if d == 9 and (x + y) % 2 == 0:
                L.set(x, y, 'a')
    L.set(12, 12, '-'); L.set(11, 12, 'a'); L.set(13, 12, 'a')
    L.set(12, 11, 'a'); L.set(12, 13, 'a')
    for x, y in ((0, 0), (0, 12), (12, 0)):
        L.set(x, y, 'y')
    rnd = Random(seed)
    L.salpicar(rnd, 10, '/', sobre='A')
    return L


def rejilla(seed):
    L = _base('d')
    for j in range(4):
        for i in range(4):
            x, y = i * 6 + 1, j * 6 + 1
            L.rect(x, y, 4, 4, 'k')
            L.hl(x, y + 4, 4, 'D')
            L.vl(x + 4, y, 4, 'x')
            L.hl(x, y - 1, 4, '>')
    return L


def metal(seed, variante=False):
    L = _base('d')
    L.hl(0, 0, T, 'x'); L.vl(0, 0, T, 'x')
    L.hl(0, 1, T, 'D'); L.vl(1, 1, T - 1, 'D')
    for x, y in ((4, 4), (19, 4), (4, 19), (19, 19)):
        L.set(x, y, 'g'); L.set(x + 1, y + 1, 'x'); L.set(x - 1, y - 1, '<')
    if variante:                        # a hatch panel
        L.rect(6, 6, 12, 12, 'x')
        L.rect(7, 7, 10, 10, 'd')
        L.hl(7, 7, 10, 'D'); L.vl(7, 7, 10, 'D')
        L.hl(8, 16, 9, 'K'); L.vl(16, 8, 9, 'K')
        L.hl(9, 11, 6, 'x'); L.hl(9, 12, 6, 'D')
    rnd = Random(seed)
    L.salpicar(rnd, 5, 'x', sobre='d')
    return L


def chatarra(seed):
    """Scrap on the metal floor: ONE big chunk per cell, never loose bits -
    loose bits read as snow from two cells away."""
    L = metal(seed)
    L.poli([(4, 8), (13, 4), (19, 9), (16, 17), (7, 18), (3, 13)], 'u')
    L.poli([(6, 9), (12, 6), (15, 9), (8, 12)], '*')
    L.poli([(8, 15), (16, 12), (16, 17), (7, 18)], 'U')
    L.linea(4, 13, 7, 18, ',')
    L.linea(16, 17, 19, 9, ',')
    L.set(12, 11, 'g'); L.set(13, 12, 'x'); L.set(11, 10, '7')
    L.rect(15, 3, 6, 2, 'D'); L.hl(15, 3, 6, 'g'); L.hl(15, 5, 6, 'x')
    return L


def circuito(seed, pista=False):
    L = _base('k')
    rnd = Random(seed)
    for _ in range(3):
        x, y = rnd.randrange(T), rnd.randrange(T)
        L.vl(x, y, 4, 'K')
    if pista:
        L.linea(3, 2, 8, 2, 'N'); L.linea(8, 2, 8, 9, 'N')
        L.linea(8, 9, 14, 12, 'N'); L.linea(14, 12, 14, 18, 'N')
        L.linea(4, 3, 8, 3, '|'); L.linea(9, 3, 9, 9, '|')
        for x, y in ((3, 2), (14, 18)):
            L.rect(x - 1, y - 1, 3, 3, 'n'); L.set(x, y, '^')
    return L


def muro_circ(seed):
    L = _base('N')
    L.rect(1, 1, 22, 22, 'K')
    L.rect(2, 2, 20, 20, 'n')
    L.hl(2, 2, 20, '^'); L.vl(2, 2, 20, '^')
    L.hl(3, 21, 19, 'N'); L.vl(21, 3, 19, 'N')
    for y in (7, 12, 17):               # etched traces
        L.hl(5, y, 14, 'N')
        L.set(4, y, '|'); L.set(19, y, '^')
    L.rect(9, 9, 6, 6, '|'); L.rect(10, 10, 4, 4, 'N'); L.set(10, 10, '^')
    return L


def muro_ciu(seed):
    L = _base('g')
    L.rect(1, 1, 22, 22, 'G')
    L.rect(3, 3, 18, 18, 'D')
    L.hl(3, 3, 18, '>'); L.vl(3, 3, 18, '>')
    L.hl(4, 20, 17, 'd'); L.vl(20, 4, 17, 'd')
    for y in range(6, 19, 3):           # vents
        L.hl(6, y, 12, 'x'); L.hl(6, y + 1, 12, '8')
    return L


def cristal(seed):
    L = _base('c')
    L.poli([(0, 0), (14, 0), (0, 14)], '[')
    L.poli([(24, 10), (24, 24), (10, 24)], 'C')
    L.poli([(8, 24), (24, 8), (24, 12), (12, 24)], ']')
    for i in range(24):
        L.set(i, 23 - i, 'w') if i % 2 == 0 else None
    L.linea(2, 2, 10, 10, 'w')
    L.set(5, 3, 'w'); L.set(18, 16, '[')
    return L


# ---------------------------------------------------------------- walls
def piedra(seed):
    """Stone wall: dressed blocks, staggered, each one lit and shaded."""
    L = _base('9')
    for fila in range(3):
        y = fila * 8
        off = 0 if fila % 2 == 0 else 6
        for i in range(2):
            x = off + i * 12
            L.rect(x + 1, y + 1, 11, 7, 'i')
            L.hl(x + 1, y + 1, 11, '7'); L.vl(x + 1, y + 1, 7, '7')
            L.hl(x + 2, y + 7, 10, 'I'); L.vl(x + 11, y + 2, 6, '8')
            L.set(x + 4 + (i * 3 + fila) % 5, y + 4, '8')
    return L


def ladrillo(seed):
    L = _base('/')
    for fila in range(4):
        y = fila * 6
        off = 0 if fila % 2 == 0 else 4
        for i in range(3):
            x = off + i * 8
            L.rect(x + 1, y + 1, 7, 5, 'a')
            L.hl(x + 1, y + 1, 7, '-')
            L.hl(x + 1, y + 5, 7, 'A')
            L.set(x + 7, y + 2, 'A')
    rnd = Random(seed)
    L.salpicar(rnd, 4, 'A', sobre='a')
    return L


def muro(seed):
    """The dungeon's metal wall block."""
    L = _base('x')
    L.rect(1, 1, 22, 22, 'K')
    L.rect(2, 2, 20, 20, 'd')
    L.hl(2, 2, 20, 'D'); L.vl(2, 2, 20, 'D')
    L.hl(3, 21, 19, 'x'); L.vl(21, 3, 19, 'x')
    for x, y in ((4, 4), (19, 4), (4, 19), (19, 19)):
        L.set(x, y, 'g'); L.set(x + 1, y + 1, 'K')
    L.hl(6, 11, 12, 'x'); L.hl(6, 12, 12, 'D')
    return L


def pared(seed):
    """Interior wall: plain wallpaper with a faint stripe, a rail, panelling
    below. Quiet on purpose - it is a quarter of every room."""
    L = _base('Q')
    for x in range(0, T, 6):
        L.vl(x, 0, 10, ')')
    L.hl(0, 9, T, ')')
    L.hl(0, 10, T, '0'); L.hl(0, 11, T, 'j'); L.hl(0, 12, T, 'J'); L.hl(0, 13, T, '!')
    L.rect(0, 14, T, 10, ')')
    for x in (1, 13):
        L.rect(x, 16, 10, 6, 'Q')
        L.hl(x, 16, 10, '6')
        L.vl(x, 16, 6, '6')
    L.hl(0, 23, T, 'J')
    return L


def mostrador(seed):
    """The shop counter: a pale top, a wooden front of panels."""
    L = _base('j')
    L.rect(0, 0, T, 5, 'G'); L.hl(0, 0, T, 'w'); L.hl(0, 4, T, 'g')
    L.hl(0, 5, T, '!')
    for x in (1, 13):
        L.rect(x, 7, 10, 15, 'J')
        L.rect(x + 1, 8, 8, 13, 'j')
        L.hl(x + 1, 8, 8, '0')
    L.hl(0, 23, T, '!')
    return L


def cerca(seed):
    """A fence across grass: two posts, two rails and their shadow."""
    L = pasto(seed, matas=4)
    for y in (6, 14):
        L.rect(0, y + 3, T, 1, '3')      # shadow on the grass
        L.rect(0, y, T, 3, 'j')
        L.hl(0, y, T, '0'); L.hl(0, y + 2, T, 'J')
    for x in (4, 16):
        L.rect(x, 2, 4, 19, 'j')
        L.vl(x, 2, 19, '0'); L.vl(x + 3, 2, 19, 'J')
        L.hl(x, 2, 4, '0'); L.hl(x, 21, 4, '!')
        L.set(x + 1, 4, '!')
        L.hl(x + 1, 22, 4, '3')
    return L


def arbusto(seed):
    L = pasto(seed, matas=3)
    L.elipse(12, 14, 10, 6.5, '3')      # the shadow first
    L.elipse(11, 11, 10, 8, '4')
    L.elipse(11, 11, 9, 7, 'f')
    for cx, cy, r in ((7, 9, 4), (14, 8, 4.5), (10, 13, 4), (16, 13, 3)):
        L.disco(cx, cy, r, 'E', sobre='f')
    for cx, cy in ((6, 7), (13, 6), (9, 11)):
        L.disco(cx, cy, 1.8, '2', sobre='Ef')
        L.set(cx - 1, cy - 1, '1')
    L.set(15, 15, 'F'); L.set(8, 16, 'F'); L.set(17, 11, 'F')
    return L


def roca(seed):
    L = tierra(seed, base='Q', claro='(', oscuro=')', hondo=')')
    L.elipse(12, 18, 9, 3.5, ')')       # its shadow on the sand
    L.elipse(11.5, 12, 8.5, 7, '9')
    L.elipse(11.5, 11.5, 7.8, 6.5, 'I')
    L.elipse(10.5, 10.5, 6.5, 5.3, '8')
    L.elipse(9, 9, 4.5, 3.5, 'i')
    L.elipse(8, 8, 2.2, 1.5, '7')
    L.linea(14, 7, 16, 12, '9'); L.set(15, 9, 'I')
    return L


def negro(seed):
    return _base('k')


def nevado(seed):
    return alto(seed, base='G', tallos=('?', 'w', '7'), puntas=('w', 'c'), pie='?')


def paramo(seed):
    return alto(seed, base='Q', tallos=('V', '3', ')'), puntas=('2', 'E'), pie=')')


# ---------------------------------------------------------------- fringes
def borde(seed, c1, c2, sombra):
    """The north fringe of a ground that spills over its neighbour: a solid
    lip, then tongues of uneven length, each with a darker tip and a thin
    shadow under it on the ground it covers. ch_tile_borde() turns it for
    the other three sides."""
    rnd = Random(seed)
    L = Lienzo(T, T)
    L.rect(0, 0, T, 2, c1)
    x = 0
    while x < T:
        w = rnd.choice((2, 3, 3, 4))
        h = rnd.choice((2, 3, 4, 5, 6))
        for xx in range(x, min(x + w, T)):
            hh = h - (1 if xx in (x, x + w - 1) else 0)
            L.vl(xx, 2, hh, c1)
            L.set(xx, 2 + hh - 1, c2)
            if sombra: L.set(xx, 2 + hh, sombra)
        x += w + rnd.choice((0, 1))
    L.hl(0, 0, T, c1)
    for i in range(0, T, 5):
        L.set(i + (seed % 3), 1, c2)
    return L


SUELOS = {
    # name in ch_world.c  : drawing
    'PX_PASTO':     lambda: pasto(1),
    'PX_PASTO2':    lambda: pasto(2, flores=1),
    'PX_TIERRA':    lambda: tierra(3),
    'PX_TIERRA2':   lambda: tierra(4),
    'PX_PASTO_C':   lambda: pasto_c(5),
    'PX_PASTO_C2':  lambda: pasto_c(6),
    'PX_FLORES':    lambda: flores(7),
    'PX_FLORES2':   lambda: flores(8),
    'PX_ADOQUIN':   lambda: adoquin(9),
    'PX_LOSA':      lambda: losa(10),
    'PX_PARQUET':   lambda: parquet(11),
    'PX_ALTO':      lambda: alto(12),
    'PX_AGUA':      lambda: agua(13),
    'PX_CERCA':     lambda: cerca(14),
    'PX_PIEDRA':    lambda: piedra(15),
    'PX_LADRILLO':  lambda: ladrillo(16),
    'PX_MADERA':    lambda: madera(17),
    'PX_PARED':     lambda: pared(18),
    'PX_ALFOMBRA':  lambda: alfombra(19),
    'PX_MOSTRADOR': lambda: mostrador(20),
    'PX_METAL':     lambda: metal(21),
    'PX_METAL2':    lambda: metal(22, variante=True),
    'PX_MURO':      lambda: muro(23),
    'PX_CHATARRA':  lambda: chatarra(24),
    'PX_ACEITE':    lambda: aceite(25),
    'PX_ROCA':      lambda: roca(26),
    'PX_REJILLA':   lambda: rejilla(27),
    'PX_BALDOSA':   lambda: baldosa(28),
    'PX_NEGRO':     lambda: negro(29),
    'PX_PUENTE':    lambda: puente(30),
    'PX_ARBUSTO':   lambda: arbusto(31),
    'PX_NIEVE':     lambda: nieve(32),
    'PX_NEVADO':    lambda: nevado(33),
    'PX_HIELO':     lambda: hielo(34),
    'PX_LAVA':      lambda: lava(35),
    'PX_VOLCAN':    lambda: volcan(36),
    'PX_ARENA':     lambda: arena(37),
    'PX_MUELLE':    lambda: muelle(38),
    'PX_VADO':      lambda: vado(39),
    'PX_CIRCUITO':  lambda: circuito(40),
    'PX_CIRCUITO2': lambda: circuito(41, pista=True),
    'PX_MURO_CIRC': lambda: muro_circ(42),
    'PX_SECO':      lambda: seco(43),
    'PX_PARAMO':    lambda: paramo(44),
    'PX_GRAVA':     lambda: grava(45),
    'PX_CIUDAD':    lambda: ciudad(46),
    'PX_MURO_CIU':  lambda: muro_ciu(47),
    'PX_CRISTAL':   lambda: cristal(48),
    'BR_PASTO':     lambda: borde(1, 'e', 'E', '3'),
    'BR_TIERRA':    lambda: borde(2, 'h', 'H', '5'),
    'BR_ARENA':     lambda: borde(3, 'q', 'Q', ')'),
    'BR_NIEVE':     lambda: borde(4, 'G', '?', '8'),
    'BR_GRAVA':     lambda: borde(5, 'I', '9', 'k'),
    'BR_CIUDAD':    lambda: borde(6, 'p', 'P', None),
    'BR_VOLCAN':    lambda: borde(7, 'S', 's', 'k'),
}
