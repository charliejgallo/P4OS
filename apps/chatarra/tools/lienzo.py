"""A small pixel-art canvas for drawing Chatarra's 2x assets with code.

The assets are ASCII maps (ASSETS.md): one palette character per pixel, '.' is
transparent. This is the pencil the art in tools/arte2x.py is drawn with -
rectangles, discs, polygons, lines, outlines, bevels and speckle - so a tile
or a house is a short, readable drawing rather than a wall of letters, and it
can be changed and drawn again.

Everything is deterministic: speckle takes an explicit random.Random, so the
same script always draws the same art (the game repaints its background by
rectangles, and art that changed between two draws would boil).
"""
import math
import random


class Lienzo:
    def __init__(self, w, h, fondo='.', envolver=False):
        self.w, self.h = w, h
        self.envolver = envolver          # tiles: what leaves one side enters the other
        self.p = [[fondo] * w for _ in range(h)]

    # ---- pixels ---------------------------------------------------------
    def get(self, x, y, fuera='.'):
        if self.envolver:
            return self.p[y % self.h][x % self.w]
        if 0 <= x < self.w and 0 <= y < self.h:
            return self.p[y][x]
        return fuera

    def set(self, x, y, c):
        x, y = int(round(x)), int(round(y))
        if self.envolver:
            self.p[y % self.h][x % self.w] = c
        elif 0 <= x < self.w and 0 <= y < self.h:
            self.p[y][x] = c

    def set_si(self, x, y, c, sobre):
        """Paints only over the given characters (a mask by colour)."""
        v = self.get(int(round(x)), int(round(y)), None)
        if v is not None and v in sobre:
            self.set(x, y, c)

    # ---- shapes ---------------------------------------------------------
    def rect(self, x, y, w, h, c):
        for yy in range(int(y), int(y + h)):
            for xx in range(int(x), int(x + w)):
                self.set(xx, yy, c)

    def hl(self, x, y, n, c):
        self.rect(x, y, n, 1, c)

    def vl(self, x, y, n, c):
        self.rect(x, y, 1, n, c)

    def marco(self, x, y, w, h, c):
        self.hl(x, y, w, c); self.hl(x, y + h - 1, w, c)
        self.vl(x, y, h, c); self.vl(x + w - 1, y, h, c)

    def elipse(self, cx, cy, rx, ry, c, sobre=None):
        for yy in range(int(math.floor(cy - ry)) - 1, int(math.ceil(cy + ry)) + 2):
            for xx in range(int(math.floor(cx - rx)) - 1, int(math.ceil(cx + rx)) + 2):
                dx = (xx + 0.5 - cx) / rx if rx else 9
                dy = (yy + 0.5 - cy) / ry if ry else 9
                if dx * dx + dy * dy <= 1.0:
                    if sobre is None: self.set(xx, yy, c)
                    else: self.set_si(xx, yy, c, sobre)

    def disco(self, cx, cy, r, c, sobre=None):
        self.elipse(cx, cy, r, r, c, sobre)

    def linea(self, x0, y0, x1, y1, c):
        n = int(max(abs(x1 - x0), abs(y1 - y0))) or 1
        for i in range(n + 1):
            self.set(x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n, c)

    def poli(self, pts, c, sobre=None):
        ys = [p[1] for p in pts]
        for yy in range(int(math.floor(min(ys))), int(math.ceil(max(ys))) + 1):
            y = yy + 0.5
            xs = []
            for i in range(len(pts)):
                (x0, y0), (x1, y1) = pts[i], pts[(i + 1) % len(pts)]
                if (y0 <= y < y1) or (y1 <= y < y0):
                    xs.append(x0 + (y - y0) * (x1 - x0) / (y1 - y0))
            xs.sort()
            for a, b in zip(xs[::2], xs[1::2]):
                for xx in range(int(math.ceil(a - 0.5)), int(math.floor(b - 0.5)) + 1):
                    if sobre is None: self.set(xx, yy, c)
                    else: self.set_si(xx, yy, c, sobre)

    # ---- passes ---------------------------------------------------------
    def contorno(self, c, de=None, diagonal=False):
        """Outlines what is not transparent (or only the characters in `de`)
        with c, OUTSIDE the shape: the sprite grows by one pixel."""
        marcar = []
        for y in range(self.h):
            for x in range(self.w):
                if self.p[y][x] != '.':
                    continue
                vec = [(1, 0), (-1, 0), (0, 1), (0, -1)]
                if diagonal: vec += [(1, 1), (-1, 1), (1, -1), (-1, -1)]
                for dx, dy in vec:
                    v = self.get(x + dx, y + dy)
                    if v != '.' and v != c and (de is None or v in de):
                        marcar.append((x, y)); break
        for x, y in marcar:
            self.p[y][x] = c

    def bisel(self, region, luz=None, sombra=None, luz2=None, sombra2=None):
        """Inside a region (a set of characters): the pixels whose upper
        neighbour is outside it get `luz`, the lower edge `sombra`, and the
        left/right edges the softer two. Light from the top left."""
        cambios = []
        for y in range(self.h):
            for x in range(self.w):
                if self.p[y][x] not in region:
                    continue
                if luz and self.get(x, y - 1) not in region: cambios.append((x, y, luz))
                elif sombra and self.get(x, y + 1) not in region: cambios.append((x, y, sombra))
                elif luz2 and self.get(x - 1, y) not in region: cambios.append((x, y, luz2))
                elif sombra2 and self.get(x + 1, y) not in region: cambios.append((x, y, sombra2))
        for x, y, c in cambios:
            self.p[y][x] = c

    def salpicar(self, rnd, n, c, x=0, y=0, w=None, h=None, sobre=None):
        w = self.w if w is None else w
        h = self.h if h is None else h
        for _ in range(n):
            xx, yy = x + rnd.randrange(w), y + rnd.randrange(h)
            if sobre is None or self.get(xx, yy) in sobre:
                self.set(xx, yy, c)

    def tramado(self, x, y, w, h, c, sobre=None, fase=0):
        """A checkerboard of c: the transition between two tones."""
        for yy in range(y, y + h):
            for xx in range(x, x + w):
                if (xx + yy + fase) & 1 and (sobre is None or self.get(xx, yy) in sobre):
                    self.set(xx, yy, c)

    def cambiar(self, de, a):
        for fila in self.p:
            for i, v in enumerate(fila):
                if v == de: fila[i] = a

    def pegar(self, otro, x, y):
        for yy in range(otro.h):
            for xx in range(otro.w):
                v = otro.p[yy][xx]
                if v != '.': self.set(x + xx, y + yy, v)

    def espejo(self):
        n = Lienzo(self.w, self.h)
        n.p = [list(reversed(f)) for f in self.p]
        return n

    def filas(self):
        return [''.join(f) for f in self.p]
