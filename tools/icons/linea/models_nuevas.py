"""'linea' models: Malla, Mensajes, Correo, Cielo and Bot.

VARIANTS holds the options shown to the user; ART holds the chosen one for
each app (what build.py renders into the pack). Nothing of Telegram's mark,
no paper plane.
"""

import math

from lib import KO, C, L, P, R, arc, poly, star, waves

PAIRS = {
    "aos.malla": ("#9CFFB0", "#22C55E"),
    "aos.mensajes": ("#9ED8FF", "#2F7BEA"),
    "aos.correo": ("#FFE36A", "#3AA0FF"),
    "aos.cielo": ("#FFF2B0", "#7A8CFF"),
    "aos.bot": ("#9EF0FF", "#1C9BE0"),
}


def _bubble(x0, y0, x1, y1, tail_left=True, extra=""):
    r = 10
    tx = x0 + 18 if tail_left else x1 - 18
    t = (f"L{tx + 6},{y1} L{tx - 6},{y1 + 12} L{tx - 2},{y1}" if tail_left
         else f"L{tx + 2},{y1} L{tx + 6},{y1 + 12} L{tx - 6},{y1}")
    return P(f"M{x0 + r},{y0} H{x1 - r} Q{x1},{y0} {x1},{y0 + r} V{y1 - r} Q{x1},{y1} {x1 - r},{y1} "
             f"H{tx + 6 if not tail_left else tx + 6} {t} H{x0 + r} Q{x0},{y1} {x0},{y1 - r} V{y0 + r} "
             f"Q{x0},{y0} {x0 + r},{y0} Z", extra)


def malla_a(m):
    """Three nodes linked into a mesh, one sending."""
    pts = [(30, 36), (90, 42), (58, 92)]
    lines = "".join(L(*pts[i], *pts[(i + 1) % 3]) for i in range(3))
    lines += waves(90, 42, 2 if not m.mini else 1, 12, 10, -20, 70)
    return lines, "".join(C(x, y, 9 if not m.mini else 11) for x, y in pts)


def malla_b(m):
    """A handheld node, its antenna sending."""
    lines = R(34, 48, 40, 54, 8) + L(64, 48, 64, 24) + waves(64, 22, 2 if not m.mini else 1, 12, 11, -40, 60)
    return lines, C(46, 62, 4) + ("" if m.mini else R(42, 76, 24, 12, 3))


def mensajes_a(m):
    """Two bubbles, one answering the other."""
    a = _bubble(14, 18, 74, 58)
    b = _bubble(46, 56, 106, 94, tail_left=False, extra=KO)
    fills = "" if m.mini else "".join(C(64 + i * 12, 75, 3.5) for i in range(3))
    return a + b, fills


def mensajes_b(m):
    """One bubble, someone typing."""
    return _bubble(16, 24, 104, 86), "".join(C(42 + i * 18, 55, 6 if not m.mini else 7) for i in range(3))


def correo_a(m):
    """An envelope."""
    return R(16, 30, 88, 62, 8) + P("M18,34 L60,66 L102,34"), ""


def correo_b(m):
    """An @ sign."""
    lines = C(60, 60, 14) + P("M74,48 V66 Q74,78 86,74 Q100,68 100,58 C100,34 82,18 60,18 C36,18 18,36 18,60 "
                              "C18,84 36,102 60,102 C72,102 82,98 90,92")
    return lines, ""


def cielo_a(m):
    """The Moon and stars."""
    moon = P("M70,22 C48,24 32,40 32,62 C32,84 50,100 72,100 C84,100 94,94 100,86 C78,88 58,72 58,50 "
             "C58,38 62,28 70,22 Z")
    return moon, star(86, 38, 11, 4.5) + ("" if m.mini else star(100, 60, 6, 2.5) + star(22, 30, 5, 2))


def cielo_b(m):
    """A telescope on its tripod, a star."""
    g = '<g transform="rotate(-28 56 50)">'
    lines = g + R(26, 40, 62, 18, 7) + R(84, 36, 10, 26, 3) + "</g>"
    lines += L(56, 58, 40, 100) + L(56, 58, 72, 100) + L(56, 58, 56, 98)
    return lines, star(92, 22, 10, 4)


def bot_a(m):
    """A robot's head, its antenna up."""
    lines = R(22, 36, 76, 58, 16) + L(60, 36, 60, 22)
    if not m.mini:
        lines += L(48, 80, 72, 80) + R(14, 56, 8, 18, 4) + R(98, 56, 8, 18, 4)
    return lines, C(60, 18, 5) + C(45, 62, 7) + C(75, 62, 7)


def bot_b(m):
    """A robot's head in a speech bubble: the bot that answers."""
    lines = _bubble(14, 22, 106, 90) + R(36, 38, 48, 36, 10) + L(60, 38, 60, 30)
    return lines, C(50, 56, 5) + C(70, 56, 5) + C(60, 28, 4)


VARIANTS = {
    "aos.malla": {"a": malla_a, "b": malla_b},
    "aos.mensajes": {"a": mensajes_a, "b": mensajes_b},
    "aos.correo": {"a": correo_a, "b": correo_b},
    "aos.cielo": {"a": cielo_a, "b": cielo_b},
    "aos.bot": {"a": bot_a, "b": bot_b},
}

# The chosen one per app: set once the user picks (build.py renders only the
# apps in elegidos.json).
ART = {"aos.malla": malla_a, "aos.mensajes": mensajes_a, "aos.correo": correo_a, "aos.cielo": cielo_a,
       "aos.bot": bot_b}
