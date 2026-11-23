#!/usr/bin/env python3
"""The turtle game's reference model, in Python.

This is the specification the C code will follow, written so that it runs:
the world (a grid of tiles, or a sheet of paper to draw on), the level files
(levels/*.tl), the game's own LOGO ("Logo", in Spanish and English), the
piece counter that gives the stars, and runners for the same level written
in Lua and in Python. `check` runs every reference solution of every level
on every map variant, in the easy and the hard mode, and says whether it wins
and how many pieces it has.

    python3 apps/tortuga/tools/tg_ref.py check apps/tortuga/levels/muestra.tl
    python3 apps/tortuga/tools/tg_ref.py check --lua-lib <liblua.dylib> ...

The Lua runner needs Lua 5.4 as a shared library; the app's own copy builds
into one with:

    cc -O2 -shared -fPIC -o <scratch>/liblua.dylib apps/lua/main/lua/*.c
"""

import ctypes
import math
import os
import re
import sys

# ---------------------------------------------------------------- the API --

# canonical (Spanish) name, English name, other names (LOGO's classic
# ones), kind, minimum and maximum number of arguments. In Logo the turns
# are two words, "girar derecha" / "turn right" (LogoParser.statement).
API = [
    ("avanzar", "forward", ("av", "fd"), "cmd", 0, 1),
    ("retroceder", "back", ("re", "bk"), "cmd", 0, 1),
    ("girar_derecha", "turn_right", ("derecha", "right", "de", "rt"),
     "cmd", 0, 1),
    ("girar_izquierda", "turn_left", ("izquierda", "left", "iz", "lt"),
     "cmd", 0, 1),
    ("comer", "eat", (), "cmd", 0, 0),
    ("decir", "say", (), "cmd", 1, 1),
    ("bajar_lapiz", "pen_down", ("bl", "pd"), "cmd", 0, 0),
    ("subir_lapiz", "pen_up", ("sl", "pu"), "cmd", 0, 0),
    ("color", "color", (), "cmd", 1, 1),
    ("camino_adelante", "path_ahead", (), "sense", 0, 0),
    ("camino_derecha", "path_right", (), "sense", 0, 0),
    ("camino_izquierda", "path_left", (), "sense", 0, 0),
    ("agua_adelante", "water_ahead", (), "sense", 0, 0),
    ("hay_lechuga", "on_lettuce", (), "sense", 0, 0),
    ("en_meta", "at_goal", (), "sense", 0, 0),
    ("tengo_llave", "has_key", (), "sense", 0, 0),
]
SPEC = {a[0]: a for a in API}
ALIAS = {}
for a in API:
    ALIAS[a[0]] = a[0]
    ALIAS[a[1]] = a[0]
    for ab in a[2]:
        ALIAS[ab] = a[0]
CMDS = {a[0] for a in API if a[3] == "cmd"}
SENSES = {a[0] for a in API if a[3] == "sense"}

DIRS = ["N", "E", "S", "W"]
STEP = {"N": (0, -1), "E": (1, 0), "S": (0, 1), "W": (-1, 0)}
DIR_WORD = {"norte": "N", "este": "E", "sur": "S", "oeste": "W",
            "north": "N", "east": "E", "south": "S", "west": "W"}
TURTLE_CH = {"^": "N", ">": "E", "v": "S", "<": "W"}
WALKABLE = set(".=LkMp")

MAX_ACTIONS = 5000      # world actions (moves, turns, sensors) per run
MAX_STEPS = 200000      # interpreter steps per run (LOGO)
PAPER = 200             # the sheet is -200..200 on both axes


class Stop(Exception):
    """Ends a run: kind is 'win', 'error' or 'budget'."""

    def __init__(self, kind, msg="", line=0):
        super().__init__(msg)
        self.kind, self.msg, self.line = kind, msg, line


# -------------------------------------------------------------- the level --

class Level:
    def __init__(self):
        self.id = ""
        self.title = {}
        self.orders = set()
        self.goals = []
        self.stars = (0, 0)
        self.maps = []          # list of (rows, start or None)
        self.start = None       # (x, y, dir) for the drawing world
        self.figure = ""        # LOGO that draws the target figure
        self.hints = []
        self.solutions = {}     # language -> source

    @property
    def drawing(self):
        return "dibujo" in self.goals


def parse_levels(path):
    levels, cur, sect, buf = [], None, None, []

    def flush():
        if cur is None or sect is None:
            return
        text = "\n".join(buf).strip("\n")
        if sect == "mapa":
            cur.maps.append([r for r in text.split("\n")])
        elif sect == "figura":
            cur.figure = text
        elif sect in ("logo", "lua", "python"):
            cur.solutions[sect] = text + "\n"

    for raw in open(path, encoding="utf-8"):
        line = raw.rstrip("\n")
        if line.startswith("@--"):
            continue
        if line.startswith("@"):
            flush()
            sect, buf = None, []
            key, _, val = line[1:].partition(" ")
            val = val.strip()
            if key == "nivel":
                cur = Level()
                cur.id = val
                levels.append(cur)
            elif key == "titulo" and cur is not None:
                lang, _, t = val.partition(":")
                cur.title[lang.strip()] = t.strip()
            elif key == "ordenes":
                cur.orders = set(val.split())
            elif key == "meta":
                cur.goals = val.split()
            elif key == "estrellas":
                a, b = val.split()
                cur.stars = (int(a), int(b))
            elif key == "inicio":
                x, y, d = val.split()
                cur.start = (float(x), float(y), d)
            elif key in ("pista", "carta", "mundo", "idea"):
                if cur is not None and key == "pista":
                    cur.hints.append(val)
                if key == "mundo":
                    cur = None
            elif key in ("mapa", "figura", "logo", "lua", "python"):
                sect = key
            continue
        if sect:
            buf.append(line)
        # anything else between sections is a comment
    flush()
    return levels


# -------------------------------------------------------------- the world --

class World:
    """One run of one program on one map (or on the paper)."""

    def __init__(self, level, variant=0, drawing=None, hard=False):
        self.level = level
        # easy: the level is won the moment its goals are met (passing over
        # the goal counts); hard: only when the program ends, standing on it
        self.hard = hard
        self.actions = 0
        self.said = []
        self.line = 0
        self.drawing = level.drawing if drawing is None else drawing
        if self.drawing:
            x, y, d = level.start or (0, 0, "norte")
            self.x, self.y = x, y
            self.heading = {"N": 0, "E": 90, "S": 180, "W": 270}[DIR_WORD[d]]
            self.pen = True
            self.segs = []
            return
        rows = level.maps[variant]
        self.h = len(rows)
        self.w = max(len(r) for r in rows)
        self.t = [list(r.ljust(self.w)) for r in rows]
        self.keys = 0
        self.lettuce_total = 0
        for y, r in enumerate(self.t):
            for x, c in enumerate(r):
                if c in TURTLE_CH:
                    self.tx, self.ty, self.dir = x, y, TURTLE_CH[c]
                    r[x] = "."
                elif c == "L":
                    self.lettuce_total += 1
        if level.start:
            x, y, d = level.start
            self.tx, self.ty, self.dir = int(x), int(y), DIR_WORD[d]

    # -- helpers
    def tile(self, x, y):
        if 0 <= y < self.h and 0 <= x < self.w:
            return self.t[y][x]
        return " "

    def ahead(self, turn=0):
        d = DIRS[(DIRS.index(self.dir) + turn) % 4]
        dx, dy = STEP[d]
        return self.tx + dx, self.ty + dy

    def free(self, x, y):
        c = self.tile(x, y)
        return c in WALKABLE or (c == "P" and self.keys > 0)

    def goals_met(self):
        for g in self.level.goals:
            if g == "llegar" and self.tile(self.tx, self.ty) != "M":
                return False
            if g == "lechugas" and any("L" in r for r in self.t):
                return False
            if g == "decir=lechugas" and (not self.said or
                                          self.said[-1] != self.lettuce_total):
                return False
        return True

    def check_win(self):
        if not self.hard and self.goals_met():
            raise Stop("win")

    def fail(self, msg):
        raise Stop("error", msg, self.line)

    # -- one call from any language
    def call(self, name, args, line):
        self.line = line
        self.actions += 1
        if self.actions > MAX_ACTIONS:
            raise Stop("budget", "la tortuga se quedó dando vueltas", line)
        spec = SPEC[name]
        if self.level.orders and name not in self.level.orders:
            self.fail("en este nivel todavía no está «%s»" % name)
        if not (spec[4] <= len(args) <= spec[5]):
            self.fail("«%s» lleva %d dato(s)" % (name, spec[5]))
        if self.drawing:
            return self.call_paper(name, args)
        return self.call_grid(name, args)

    def call_grid(self, name, args):
        if name in ("avanzar", "retroceder"):
            n = args[0] if args else 1
            if n != int(n) or n < 0:
                self.fail("acá se avanza de a casillas enteras")
            back = name == "retroceder"
            for _ in range(int(n)):
                x, y = self.ahead(2 if back else 0)
                c = self.tile(x, y)
                if c in (" ", "#"):
                    self.fail("chocó con %s" % ("el borde" if c == " " else
                                                "una piedra"))
                if c == "~":
                    self.fail("se cayó al agua")
                if c == "P":
                    if not self.keys:
                        self.fail("la puerta está cerrada: falta la llave")
                    self.keys -= 1
                    self.t[y][x] = "p"
                if c == "k":
                    self.keys += 1
                    self.t[y][x] = "."
                self.tx, self.ty = x, y
                self.check_win()
            return None
        if name in ("girar_derecha", "girar_izquierda"):
            g = args[0] if args else 90
            if g % 90:
                self.fail("en este mundo la tortuga gira de a 90 grados")
            q = int(g // 90) * (1 if name == "girar_derecha" else -1)
            self.dir = DIRS[(DIRS.index(self.dir) + q) % 4]
            return None
        if name == "comer":
            if self.t[self.ty][self.tx] != "L":
                return None     # it chews grass: no harm, nothing gained
            self.t[self.ty][self.tx] = "."
            self.check_win()
            return None
        if name == "decir":
            self.said.append(args[0])
            self.check_win()
            return None
        if name in ("bajar_lapiz", "subir_lapiz", "color"):
            return None
        if name == "camino_adelante":
            return self.free(*self.ahead(0))
        if name == "camino_derecha":
            return self.free(*self.ahead(1))
        if name == "camino_izquierda":
            return self.free(*self.ahead(-1))
        if name == "agua_adelante":
            return self.tile(*self.ahead(0)) == "~"
        if name == "hay_lechuga":
            return self.t[self.ty][self.tx] == "L"
        if name == "en_meta":
            return self.t[self.ty][self.tx] == "M"
        if name == "tengo_llave":
            return self.keys > 0
        self.fail("«%s» no sirve en este mundo" % name)

    def call_paper(self, name, args):
        if name in ("avanzar", "retroceder"):
            n = args[0] if args else 1
            if name == "retroceder":
                n = -n
            r = math.radians(self.heading)
            nx = self.x + n * math.sin(r)
            ny = self.y + n * math.cos(r)
            if abs(nx) > PAPER + 1e-6 or abs(ny) > PAPER + 1e-6:
                self.fail("la tortuga se salió de la hoja")
            if self.pen:
                self.segs.append((self.x, self.y, nx, ny))
            self.x, self.y = nx, ny
            return None
        if name in ("girar_derecha", "girar_izquierda"):
            g = args[0] if args else 90
            self.heading = (self.heading + (g if name == "girar_derecha" else -g)) % 360
            return None
        if name == "bajar_lapiz":
            self.pen = True
            return None
        if name == "subir_lapiz":
            self.pen = False
            return None
        if name in ("color", "decir"):
            if name == "decir":
                self.said.append(args[0])
            return None
        self.fail("«%s» no sirve en la hoja" % name)

    # -- the end of the program
    def finish(self):
        if self.drawing:
            return
        if self.goals_met():
            raise Stop("win")
        if self.tile(self.tx, self.ty) != "M" and "llegar" in self.level.goals:
            self.fail("el programa terminó y la tortuga no está en la meta")
        self.fail("el programa terminó sin cumplir todo lo que pide el nivel")


def seg_dist(px, py, s):
    x0, y0, x1, y1 = s
    dx, dy = x1 - x0, y1 - y0
    L2 = dx * dx + dy * dy
    t = 0 if L2 == 0 else max(0, min(1, ((px - x0) * dx + (py - y0) * dy) / L2))
    return math.hypot(px - (x0 + t * dx), py - (y0 + t * dy))


def samples(segs, step=2.0):
    for s in segs:
        n = max(1, int(math.hypot(s[2] - s[0], s[3] - s[1]) / step))
        for i in range(n + 1):
            yield s[0] + (s[2] - s[0]) * i / n, s[1] + (s[3] - s[1]) * i / n


def same_drawing(drawn, target, tol=3.0):
    """Every point of the target is near the ink, and every point of the
    ink is near the target: order, direction and overlaps don't matter."""
    if not drawn:
        return False
    for px, py in samples(target):
        if min(seg_dist(px, py, s) for s in drawn) > tol:
            return False
    for px, py in samples(drawn):
        if min(seg_dist(px, py, s) for s in target) > tol:
            return False
    return True


# ---------------------------------------------------------------- Logo --

KW = {
    "para": "to", "to": "to", "fin": "end", "end": "end",
    "repetir": "repeat", "repeat": "repeat", "si": "if", "if": "if",
    "sino": "else", "else": "else", "mientras": "while", "while": "while",
    "haz": "make", "make": "make", "no": "not", "not": "not",
    "y": "and", "and": "and", "o": "or", "or": "or",
    "alto": "stop", "stop": "stop", "devolver": "output", "output": "output",
    "verdadero": "true", "true": "true", "falso": "false", "false": "false",
}

TOKEN = re.compile(r"""
    (?P<ws>[ \t\r]+) | (?P<nl>\n) | (?P<com>;[^\n]*)
  | (?P<num>\d+(?:\.\d+)?)
  | (?P<str>"[^"\n\s\[\]]*"?|"[^"\n]*")
  | (?P<op><=|>=|<>|==|\+=|-=|[-+*/<>=()\[\]])
  | (?P<name>:?[^\W\d]\w*)
""", re.X | re.U)


class LogoError(Exception):
    def __init__(self, msg, line):
        super().__init__(msg)
        self.msg, self.line = msg, line


def logo_tokens(src):
    out, line, pos = [], 1, 0
    while pos < len(src):
        m = TOKEN.match(src, pos)
        if not m:
            raise LogoError("no entiendo «%s»" % src[pos], line)
        pos = m.end()
        k = m.lastgroup
        v = m.group()
        if k == "nl":
            line += 1
        elif k in ("ws", "com"):
            pass
        elif k == "num":
            out.append(("num", float(v) if "." in v else int(v), line))
        elif k == "str":
            out.append(("str", v.strip('"'), line))
        elif k == "op":
            out.append(("op", v, line))
        else:
            v = v.lstrip(":").lower()
            if v in KW:
                out.append(("kw", KW[v], line))
            else:
                out.append(("name", v, line))
    out.append(("eof", None, line))
    return out


class LogoParser:
    def __init__(self, src):
        self.t = logo_tokens(src)
        self.i = 0
        self.procs = {}         # name -> number of parameters
        # first pass: the procedures, so a call may come before its definition;
        # the parameters are the names on the same line as "para"
        for j, tok in enumerate(self.t):
            if tok[:2] == ("kw", "to") and self.t[j + 1][0] == "name":
                name, ln = self.t[j + 1][1], tok[2]
                k, n = j + 2, 0
                while self.t[k][0] == "name" and self.t[k][2] == ln:
                    k, n = k + 1, n + 1
                self.procs[name] = n

    def peek(self):
        return self.t[self.i]

    def next(self):
        tok = self.t[self.i]
        self.i += 1
        return tok

    def expect(self, kind, val, what):
        tok = self.next()
        if tok[0] != kind or tok[1] != val:
            raise LogoError("falta %s" % what, tok[2])
        return tok

    def program(self):
        body = []
        while self.peek()[0] != "eof":
            body.append(self.statement())
        return body

    def block(self):
        self.expect("op", "[", "abrir el bloque con [")
        body = []
        while not (self.peek()[0] == "op" and self.peek()[1] == "]"):
            if self.peek()[0] == "eof":
                raise LogoError("falta cerrar el bloque con ]", self.peek()[2])
            body.append(self.statement())
        self.next()
        return body

    def starts_expr(self, tok):
        k, v = tok[0], tok[1]
        if k in ("num", "str"):
            return True
        if k == "op":
            return v in ("(", "-")
        if k == "kw":
            return v in ("not", "true", "false")
        if k == "name":
            after = self.t[self.i + 1]
            if after[0] == "op" and after[1] in ("=", "+=", "-="):
                return False        # the next statement is an assignment
            c = ALIAS.get(v)
            return not (c in CMDS or v in self.procs or v in ("girar", "turn"))
        return False

    def statement(self):
        tok = self.next()
        k, v, ln = tok
        if k == "kw":
            if v == "to":
                name = self.next()
                if name[0] != "name":
                    raise LogoError("después de «para» va el nombre", ln)
                params = []
                while self.peek()[0] == "name" and self.peek()[2] == ln:
                    params.append(self.next()[1])
                body = []
                while not (self.peek()[0] == "kw" and self.peek()[1] == "end"):
                    if self.peek()[0] == "eof":
                        raise LogoError("falta el «fin» de «%s»" % name[1], ln)
                    body.append(self.statement())
                self.next()
                return ("to", name[1], params, body, ln)
            if v == "repeat":
                n = self.expr()
                return ("repeat", n, self.block(), ln)
            if v == "if":
                c = self.expr()
                body = self.block()
                other = None
                if self.peek()[:2] == ("kw", "else"):
                    self.next()
                    if self.peek()[:2] == ("kw", "if"):
                        other = [self.statement()]
                    else:
                        other = self.block()
                return ("if", c, body, other, ln)
            if v == "while":
                c = self.expr()
                return ("while", c, self.block(), ln)
            if v == "make":
                name = self.next()
                if name[0] != "name":
                    raise LogoError("después de «haz» va el nombre", ln)
                return ("make", name[1], self.expr(), ln)
            if v == "stop":
                return ("stop", ln)
            if v == "output":
                return ("output", self.expr(), ln)
            raise LogoError("«%s» no va acá" % v, ln)
        if k == "name":
            nxt = self.peek()
            if nxt[0] == "op" and nxt[1] in ("=", "+=", "-="):
                # an assignment: cuenta = 0, cuenta += 1
                self.next()
                e = self.expr()
                if nxt[1] != "=":
                    e = ("bin", nxt[1][0], ("var", v, ln), e)
                return ("make", v, e, ln)
            if v in ("girar", "turn"):
                side = self.next()
                c = {"derecha": "girar_derecha", "right": "girar_derecha",
                     "izquierda": "girar_izquierda",
                     "left": "girar_izquierda"}.get(side[1]) \
                    if side[0] == "name" else None
                if not c:
                    raise LogoError("después de «%s» va derecha o izquierda"
                                    % v, ln)
                v = c
            c = ALIAS.get(v)
            if c in CMDS:
                spec = SPEC[c]
                args = []
                for j in range(spec[5]):
                    if j < spec[4] or self.starts_expr(self.peek()):
                        args.append(self.expr())
                return ("call", c, args, ln)
            if v in self.procs:
                args = [self.expr() for _ in range(self.procs[v])]
                return ("pcall", v, args, ln)
            if c in SENSES:
                raise LogoError("«%s» es una pregunta: va en un «si» o un "
                                "«mientras»" % v, ln)
            raise LogoError("no conozco «%s»" % v, ln)
        raise LogoError("no entiendo «%s»" % v, ln)

    # expressions
    def expr(self):
        a = self.and_()
        while self.peek()[:2] == ("kw", "or"):
            self.next()
            a = ("or", a, self.and_())
        return a

    def and_(self):
        a = self.not_()
        while self.peek()[:2] == ("kw", "and"):
            self.next()
            a = ("and", a, self.not_())
        return a

    def not_(self):
        if self.peek()[:2] == ("kw", "not"):
            self.next()
            return ("not", self.not_())
        return self.cmp()

    def cmp(self):
        a = self.add()
        if self.peek()[0] == "op" and self.peek()[1] in ("<", ">", "=", "==",
                                                        "<=", ">=", "<>"):
            op = self.next()[1]
            a = ("cmp", op, a, self.add())
        return a

    def add(self):
        a = self.mul()
        while self.peek()[0] == "op" and self.peek()[1] in "+-":
            op = self.next()[1]
            a = ("bin", op, a, self.mul())
        return a

    def mul(self):
        a = self.unary()
        while self.peek()[0] == "op" and self.peek()[1] in "*/":
            op = self.next()[1]
            a = ("bin", op, a, self.unary())
        return a

    def unary(self):
        if self.peek()[:2] == ("op", "-"):
            self.next()
            return ("neg", self.unary())
        return self.primary()

    def primary(self):
        k, v, ln = self.next()
        if k in ("num", "str"):
            return ("lit", v)
        if k == "kw" and v in ("true", "false"):
            return ("lit", v == "true")
        if (k, v) == ("op", "("):
            e = self.expr()
            self.expect("op", ")", "cerrar el paréntesis")
            return e
        if k == "name":
            c = ALIAS.get(v)
            if c in SENSES:
                return ("sense", c, ln)
            if v in self.procs:
                return ("fcall", v, [self.add() for _ in range(self.procs[v])],
                        ln)
            if c in CMDS:
                raise LogoError("«%s» es una orden, no un valor" % v, ln)
            return ("var", v, ln)
        raise LogoError("falta un valor", ln)


class Return(Exception):
    def __init__(self, value=None):
        self.value = value


class Logo:
    def __init__(self, world):
        self.world = world
        self.globals = {}
        self.frames = []
        self.procs = {}
        self.steps = 0

    def tick(self, ln):
        self.steps += 1
        if self.steps > MAX_STEPS:
            raise Stop("budget", "la tortuga se quedó pensando para siempre", ln)

    def run(self, src):
        p = LogoParser(src)
        prog = p.program()
        self.exec_body(prog)

    def exec_body(self, body):
        for st in body:
            self.exec(st)

    def exec(self, st):
        kind = st[0]
        ln = st[-1]
        self.tick(ln)
        if kind == "to":
            self.procs[st[1]] = st
        elif kind == "call":
            self.world.call(st[1], [self.eval(a) for a in st[2]], ln)
        elif kind == "pcall":
            self.call_proc(st[1], [self.eval(a) for a in st[2]], ln)
        elif kind == "repeat":
            n = self.eval(st[1])
            for _ in range(int(n)):
                self.exec_body(st[2])
        elif kind == "if":
            if self.truth(self.eval(st[1]), ln):
                self.exec_body(st[2])
            elif st[3]:
                self.exec_body(st[3])
        elif kind == "while":
            while self.truth(self.eval(st[1]), ln):
                self.exec_body(st[2])
                self.tick(ln)
        elif kind == "make":
            v = self.eval(st[2])
            if self.frames and st[1] in self.frames[-1]:
                self.frames[-1][st[1]] = v
            else:
                self.globals[st[1]] = v
        elif kind == "stop":
            raise Return()
        elif kind == "output":
            raise Return(self.eval(st[1]))

    def truth(self, v, ln):
        if not isinstance(v, bool):
            raise LogoError("acá va una pregunta de sí o no", ln)
        return v

    def call_proc(self, name, args, ln):
        st = self.procs.get(name)
        if not st:
            raise LogoError("«%s» todavía no está definido" % name, ln)
        if len(self.frames) > 100:
            raise LogoError("«%s» se llama a sí mismo demasiadas veces" % name,
                            ln)
        self.frames.append(dict(zip(st[2], args)))
        try:
            self.exec_body(st[3])
        except Return as r:
            return r.value
        finally:
            self.frames.pop()
        return None

    def eval(self, e):
        k = e[0]
        if k == "lit":
            return e[1]
        if k == "var":
            if self.frames and e[1] in self.frames[-1]:
                return self.frames[-1][e[1]]
            if e[1] in self.globals:
                return self.globals[e[1]]
            raise LogoError("«%s» no tiene valor todavía" % e[1], e[2])
        if k == "sense":
            return self.world.call(e[1], [], e[2])
        if k == "fcall":
            return self.call_proc(e[1], [self.eval(a) for a in e[2]], e[3])
        if k == "neg":
            return -self.eval(e[1])
        if k == "not":
            return not self.eval(e[1])
        if k == "and":
            return self.eval(e[1]) and self.eval(e[2])
        if k == "or":
            return self.eval(e[1]) or self.eval(e[2])
        if k == "bin":
            a, b = self.eval(e[2]), self.eval(e[3])
            if e[1] == "+":
                return a + b
            if e[1] == "-":
                return a - b
            if e[1] == "*":
                return a * b
            return a / b
        if k == "cmp":
            a, b = self.eval(e[2]), self.eval(e[3])
            return {"<": a < b, ">": a > b, "=": a == b, "<=": a <= b,
                    ">=": a >= b, "<>": a != b, "==": a == b}[e[1]]
        raise AssertionError(e)


def logo_pieces(src):
    """A piece is a statement: an order, a call, repetir, si, mientras, haz,
    para, alto, devolver. "sino" and what goes inside a condition are free."""
    def count(body):
        n = 0
        for st in body:
            n += 1
            if st[0] == "to":
                n += count(st[3])
            elif st[0] in ("repeat", "while"):
                n += count(st[2])
            elif st[0] == "if":
                n += count(st[2]) + (count(st[3]) if st[3] else 0)
        return n
    return count(LogoParser(src).program())


# ------------------------------------------------------- Python and Lua --

def text_pieces(src, lang):
    """The same count for Lua and Python, done on the tokens: a statement
    keyword, an assignment, or a call that starts a statement (an order or a
    function of the program's own)."""
    if lang == "python":
        src = re.sub(r"#[^\n]*", "", src)
        kws = {"for", "while", "if", "elif", "def", "return", "break"}
        defs = set(re.findall(r"\bdef\s+(\w+)", src))
        heads = {"else", ":", "\n"}
    else:
        src = re.sub(r"--[^\n]*", "", src)
        kws = {"for", "while", "if", "elseif", "function", "return", "break",
               "repeat"}
        defs = set(re.findall(r"\bfunction\s+(\w+)", src))
        heads = {"do", "then", "else", "end", ")", ";", "\n"}
    toks = re.findall(r'"[^"\n]*"|\'[^\'\n]*\'|\w+|==|~=|<=|>=|!=|[-+*/]=|\S|\n',
                      src)
    n, prev = 0, "\n"
    in_for = False
    for i, t in enumerate(toks):
        nxt = toks[i + 1] if i + 1 < len(toks) else ""
        if t in kws:
            n += 1
            in_for = t == "for"
        elif t in ("=", "+=", "-=", "*=", "/=") and not in_for:
            n += 1
        elif (nxt == "(" and (ALIAS.get(t) in CMDS or t in defs)
              and prev in heads):
            n += 1
        if t in ("do", ":", "\n"):
            in_for = False
        if t != "\n" or lang == "python":
            prev = t
        elif lang == "lua":
            prev = "\n"
    return n


def py_runner(src, world):
    def make(c):
        def f(*args):
            return world.call(c, list(args), sys._getframe(1).f_lineno)
        return f
    env = {"__builtins__": {"range": range, "len": len, "abs": abs,
                            "int": int, "print": print, "True": True,
                            "False": False}}
    for a in API:
        env[a[0]] = env[a[1]] = make(a[0])
    exec(compile(src, "programa.py", "exec"), env)


class LuaDebug(ctypes.Structure):
    _fields_ = [("event", ctypes.c_int), ("name", ctypes.c_char_p),
                ("namewhat", ctypes.c_char_p), ("what", ctypes.c_char_p),
                ("source", ctypes.c_char_p), ("srclen", ctypes.c_size_t),
                ("currentline", ctypes.c_int), ("linedefined", ctypes.c_int),
                ("lastlinedefined", ctypes.c_int), ("nups", ctypes.c_ubyte),
                ("nparams", ctypes.c_ubyte), ("isvararg", ctypes.c_char),
                ("istailcall", ctypes.c_char), ("ftransfer", ctypes.c_ushort),
                ("ntransfer", ctypes.c_ushort), ("short_src", ctypes.c_char * 60),
                ("i_ci", ctypes.c_void_p)]


LUA_PRELUDE = """
local tg = _tg
for _, n in ipairs(_tg_names) do
  _ENV[n] = function(...)
    local ok, v = tg(n, ...)
    if not ok then error("__tg_stop__", 0) end
    return v
  end
end
_tg = nil
_tg_names = nil
"""


class LuaRunner:
    def __init__(self, lib):
        L = self.lib = ctypes.CDLL(lib)
        vp, cp, ci = ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int
        L.luaL_newstate.restype = vp
        L.luaL_loadbufferx.argtypes = [vp, cp, ctypes.c_size_t, cp, cp]
        L.lua_pcallk.argtypes = [vp, ci, ci, ci, ctypes.c_ssize_t, vp]
        L.lua_tolstring.argtypes = [vp, ci, vp]
        L.lua_tolstring.restype = cp
        L.lua_tonumberx.argtypes = [vp, ci, vp]
        L.lua_tonumberx.restype = ctypes.c_double
        L.lua_type.argtypes = [vp, ci]
        L.lua_toboolean.argtypes = [vp, ci]
        L.lua_gettop.argtypes = [vp]
        L.lua_pushboolean.argtypes = [vp, ci]
        L.lua_pushnil.argtypes = [vp]
        L.lua_pushstring.argtypes = [vp, cp]
        L.lua_setglobal.argtypes = [vp, cp]
        L.lua_createtable.argtypes = [vp, ci, ci]
        L.lua_rawseti.argtypes = [vp, ci, ctypes.c_longlong]
        L.lua_getstack.argtypes = [vp, ci, ctypes.POINTER(LuaDebug)]
        L.lua_getinfo.argtypes = [vp, cp, ctypes.POINTER(LuaDebug)]
        L.lua_settop.argtypes = [vp, ci]
        L.lua_close.argtypes = [vp]
        self.CF = ctypes.CFUNCTYPE(ci, vp)
        L.lua_pushcclosure.argtypes = [vp, self.CF, ci]
        L.luaL_requiref.argtypes = [vp, cp, self.CF, ci]
        self.openers = [(b"_G", L.luaopen_base), (b"math", L.luaopen_math),
                        (b"string", L.luaopen_string),
                        (b"table", L.luaopen_table)]

    def run(self, src, world):
        L = self.lib
        st = L.luaL_newstate()
        stop = []

        def tg(s):
            try:
                name = L.lua_tolstring(s, 1, None).decode()
                args = []
                for i in range(2, L.lua_gettop(s) + 1):
                    if L.lua_type(s, i) == 3:
                        v = L.lua_tonumberx(s, i, None)
                        args.append(int(v) if v == int(v) else v)
                    else:
                        args.append(L.lua_tolstring(s, i, None).decode())
                ar = LuaDebug()
                line = 0
                if L.lua_getstack(s, 2, ctypes.byref(ar)):
                    L.lua_getinfo(s, b"l", ctypes.byref(ar))
                    line = ar.currentline
                v = world.call(name, args, line)
                L.lua_pushboolean(s, 1)
                if v is None:
                    L.lua_pushnil(s)
                else:
                    L.lua_pushboolean(s, 1 if v else 0)
                return 2
            except Stop as e:
                stop.append(e)
            except Exception as e:      # a bug in the model, not in the program
                stop.append(Stop("error", "modelo: %r" % e, 0))
            L.lua_pushboolean(s, 0)
            return 1

        cb = self.CF(tg)
        keep = []
        for name, fn in self.openers:
            f = self.CF(ctypes.cast(fn, ctypes.c_void_p).value)
            keep.append(f)
            L.luaL_requiref(st, name, f, 1)
            L.lua_settop(st, -2)
        L.lua_pushcclosure(st, cb, 0)
        L.lua_setglobal(st, b"_tg")
        L.lua_createtable(st, len(API) * 2, 0)
        i = 1
        for a in API:
            for n in (a[0], a[1]):
                L.lua_pushstring(st, n.encode())
                L.lua_rawseti(st, -2, i)
                i += 1
        L.lua_setglobal(st, b"_tg_names")
        try:
            for chunk, nm in ((LUA_PRELUDE, b"=prelude"), (src, b"=programa")):
                b = chunk.encode()
                if L.luaL_loadbufferx(st, b, len(b), nm, None) or \
                        L.lua_pcallk(st, 0, 0, 0, 0, None):
                    if stop:
                        raise stop[0]
                    msg = L.lua_tolstring(st, -1, None).decode()
                    raise Stop("error", "lua: " + msg, 0)
        finally:
            L.lua_close(st)


# ------------------------------------------------------------- running --

def run(level, lang, src, variant=0, lua=None, hard=False):
    w = World(level, variant, hard=hard)
    try:
        try:
            if lang == "logo":
                Logo(w).run(src)
            elif lang == "python":
                py_runner(src, w)
            elif lang == "lua":
                lua.run(src, w)
            w.finish()
        except LogoError as e:
            raise Stop("error", e.msg, e.line)
        except SyntaxError as e:
            raise Stop("error", "python: %s" % e.msg, e.lineno or 0)
    except Stop as e:
        if level.drawing and e.kind != "budget" and not e.msg:
            pass
        if not level.drawing or e.kind != "win":
            return e, w
    # the drawing world: compare with the figure
    target = World(level, drawing=True)
    Logo(target).run(level.figure)
    if same_drawing(w.segs, target.segs):
        return Stop("win"), w
    return Stop("error", "el dibujo no es igual a la figura"), w


def pieces(lang, src):
    return logo_pieces(src) if lang == "logo" else text_pieces(src, lang)


def check(path, lua_lib=None, gen=None):
    """gen: the folder where tools/tg_harness.c wrote every level's Logo
    tree as Lua and Python (--gen): a level that brings only its Logo is
    checked in the other two languages with those"""
    lua = LuaRunner(lua_lib) if lua_lib else None
    bad = 0
    for lv in parse_levels(path):
        variants = range(len(lv.maps)) if lv.maps else [0]
        row = []
        counts = set()
        for lang in ("logo", "lua", "python"):
            src = lv.solutions.get(lang)
            if src is None and gen and lang != "logo":
                p = os.path.join(gen, "%s.%s" % (lv.id, "lua" if lang == "lua" else "py"))
                if os.path.exists(p):
                    src = open(p).read()
            if src is None or (lang == "lua" and not lua):
                row.append("%s -" % lang)
                continue
            ok = True
            for v in variants:
                for hard in (False, True):
                    res, w = run(lv, lang, src, v, lua, hard)
                    if res.kind != "win":
                        ok = False
                        print("  %s %s mapa %d, modo %s: %s (línea %d)" % (
                            lv.id, lang, v + 1, "difícil" if hard else "fácil",
                            res.msg, res.line))
            p = pieces(lang, src)
            counts.add(p)
            row.append("%s %s %d" % (lang, "ok" if ok else "MAL", p))
            bad += not ok
        stars_ok = counts == {lv.stars[0]}
        if not stars_ok:
            bad += 1
        print("%-5s %-34s %s  piezas %s ★★★≤%d%s" % (
            lv.id, lv.title.get("es", ""), " | ".join(row),
            "/".join(str(c) for c in sorted(counts)), lv.stars[0],
            "" if stars_ok else "  <- no coincide"))
    return bad


def main():
    args = sys.argv[1:]
    lua_lib = None
    if "--lua-lib" in args:
        i = args.index("--lua-lib")
        lua_lib = args[i + 1]
        del args[i:i + 2]
    gen = None
    if "--gen" in args:
        i = args.index("--gen")
        gen = args[i + 1]
        del args[i:i + 2]
    if len(args) == 2 and args[0] == "check":
        sys.exit(1 if check(args[1], lua_lib, gen) else 0)
    print(__doc__)
    sys.exit(2)


if __name__ == "__main__":
    main()
