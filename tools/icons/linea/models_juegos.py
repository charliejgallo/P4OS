"""'linea' models: the games (juguete's juegos-1, juegos-2 and juegos-3)."""

import math

from lib import KO, TILE_C, C, L, P, R, arc, heart, poly, star, waves

PAIRS = {
    "aos.dice": ("#E0B0FF", "#9B4DFF"),
    "demo.mila": ("#FFB0D0", "#FF5AA0"),
    "demo.tanques": ("#B6FF6A", "#3FB0FF"),
    "demo.sudoku": ("#9ED0FF", "#4F6BFF"),
}


def dice(m):
    a = '<g transform="rotate(-12 44 46)">' + R(22, 24, 44, 44, 10) + "</g>"
    b = '<g transform="rotate(14 78 78)">' + R(58, 58, 40, 40, 9, KO) + "</g>"
    if m.mini:
        return a + b, C(44, 46, 5) + C(78, 78, 5)
    pa = "".join(C(44 + dx, 46 + dy, 4) for dx, dy in ((-11, -11), (11, -11), (0, 0), (-11, 11), (11, 11)))
    pb = '<g transform="rotate(14 78 78)">' + "".join(C(78 + dx, 78 + dy, 3.6) for dx, dy in ((-9, -9), (0, 0), (9, 9))) + "</g>"
    return a + b, '<g transform="rotate(-12 44 46)">' + pa + "</g>" + pb


def mila(m):
    """Mila: a round head, two ears, the eyes; whiskers on the big one."""
    head = P("M30,62 C30,40 42,30 60,30 C78,30 90,40 90,62 C90,84 78,96 60,96 C42,96 30,84 30,62 Z")
    ears = P("M34,46 L30,18 L52,32") + P("M86,46 L90,18 L68,32")
    lines = head + ears
    fills = (f'<ellipse cx="48" cy="62" rx="6" ry="8"/><ellipse cx="72" cy="62" rx="6" ry="8"/>')
    if not m.mini:
        lines += P("M22,72 L40,74 M22,82 L40,80 M98,72 L80,74 M98,82 L80,80")
        fills += poly([(56, 74), (64, 74), (60, 79)])
    return lines, fills


def tanques(m):
    body = R(26, 56, 68, 26, 10)
    tracks = R(18, 76, 84, 16, 8)
    turret = P("M42,56 C42,40 78,40 78,56")
    barrel = L(66, 46, 100, 30)
    lines = body + turret + barrel + tracks
    fills = ""
    if not m.mini:
        fills = "".join(C(x, 84, 3.5) for x in (32, 46, 60, 74, 88))
    return lines, fills


def sudoku(m):
    lines = R(20, 20, 80, 80, 12)
    if not m.mini:
        lines += L(46.7, 24, 46.7, 96) + L(73.3, 24, 73.3, 96) + L(24, 46.7, 96, 46.7) + L(24, 73.3, 96, 73.3)
        fills = ('<text x="60" y="70" font-family="Helvetica" font-weight="bold" font-size="24" '
                 'text-anchor="middle">9</text>'
                 '<text x="33.5" y="43" font-family="Helvetica" font-weight="bold" font-size="18" '
                 'text-anchor="middle">5</text>'
                 '<text x="86.5" y="96" font-family="Helvetica" font-weight="bold" font-size="18" '
                 'text-anchor="middle">1</text>')
        return lines, fills
    return lines + L(60, 24, 60, 96) + L(24, 60, 96, 60), ""


ART = {"aos.dice": dice, "demo.mila": mila, "demo.tanques": tanques, "demo.sudoku": sudoku}



def T(x, y, size, txt, extra=""):
    return (f'<text x="{x}" y="{y}" font-family="Helvetica" font-weight="bold" font-size="{size}" '
            f'text-anchor="middle" {extra}>{txt}</text>')


def mines(m):
    lines = C(54, 66, 30) + P("M74,44 Q80,30 92,28")
    return lines, star(96, 26, 11, 5, 8) + ("" if m.mini else C(44, 56, 5))


def simon(m):
    lines = C(60, 60, 42) + L(60, 18, 60, 102) + L(18, 60, 102, 60) + C(60, 60, 14, KO)
    return lines, P("M60,22 A38,38 0 0 1 98,60 H74 A14,14 0 0 0 60,46 Z")


def _card(x, y, rot, inner, w=34, h=50):
    return f'<g transform="rotate({rot} {x} {y})">' + R(x - w / 2, y - h / 2, w, h, 5, KO) + inner + "</g>"


def truco(m):
    """Three Spanish cards in a fan (juguete's a)."""
    a = _card(36, 64, -16, C(36, 64, 6))
    b = _card(60, 58, 0, P("M60,46 V68 M54,64 H66"))
    c = _card(84, 64, 16, L(84, 50, 84, 76) + L(77, 70, 91, 70))
    if m.mini:
        return _card(44, 62, -12, "") + _card(76, 62, 12, L(76, 50, 76, 74)), ""
    return a + b + c, ""


def g2043(m):
    """A ship firing (juguete's b)."""
    ship = poly([(60, 34), (68, 62), (94, 78), (90, 88), (68, 82), (64, 96), (56, 96), (52, 82), (30, 88), (26, 78),
                 (52, 62)])
    shots = L(46, 24, 46, 12) + L(74, 24, 74, 12)
    return ship + ("" if m.mini else shots), C(60, 66, 5)


def arkanos(m):
    fills = "".join(R(18 + c * 22, 20 + r * 13, 18, 9, 3) for r in range(2) for c in range(4))
    lines = L(44, 96, 76, 96)
    return lines, fills + C(68, 70, 6)


def asteroides(m):
    ship = '<g transform="rotate(35 36 84)">' + poly([(36, 66), (48, 98), (36, 90), (24, 98)]) + "</g>"
    rock = poly([(66, 22), (90, 26), (102, 46), (92, 66), (72, 66), (60, 50), (62, 34)])
    return ship + rock + ("" if m.mini else L(52, 70, 60, 62)), ""


def atasco(m):
    """The red car among blocks (juguete's b)."""
    car = R(34, 48, 50, 26, 8) + R(46, 52, 16, 18, 4)
    blocks = R(14, 14, 20, 28, 6) + R(88, 62, 18, 42, 6)
    if not m.mini:
        blocks += R(44, 14, 58, 20, 6) + R(14, 84, 58, 20, 6)
    return car + blocks, ""


def bastion(m):
    """A shield with a star (juguete's d)."""
    shield = P("M26,22 H94 V58 C94,82 78,96 60,104 C42,96 26,82 26,58 Z")
    return shield, star(60, 56, 18, 8)


def blackjack(m):
    a = _card(46, 58, -10, "", 40, 58)
    b = _card(74, 62, 10, "", 40, 58)
    return a + b, T(44, 64, 22, "A") + T(76, 70, 22, "K")


def burbujas(m):
    """The board's bubbles and the arrow (juguete's e)."""
    lines = "".join(C(30 + i * 20, 28, 9) for i in range(4)) + "".join(C(40 + i * 20, 46, 9) for i in range(3))
    lines += P("M60,104 V70 M50,80 L60,70 L70,80")
    return lines, ""


def canos(m):
    """A pipe with a valve (juguete's b)."""
    lines = L(10, 80, 110, 80) + L(60, 80, 60, 54) + C(60, 42, 20) + L(42, 42, 78, 42) + L(60, 24, 60, 60)
    return lines, C(60, 42, 5)


def chatarra(m):
    lines = R(36, 26, 48, 34, 8) + R(30, 66, 60, 36, 8) + L(60, 26, 60, 14) + L(30, 78, 16, 92) + L(90, 78, 104, 66)
    return lines, C(60, 43, 7) + C(60, 12, 4)


def cjump(m):
    """The jumper, arms up, over a platform (juguete's e)."""
    lines = C(60, 46, 18) + L(46, 36, 32, 18) + L(74, 36, 88, 18) + L(52, 62, 46, 76) + L(68, 62, 74, 76)
    lines += L(28, 96, 92, 96)
    return lines, C(53, 44, 3.5) + C(67, 44, 3.5)


def _burst(cx, cy, r_out, r_in, n=8):
    pts = []
    for i in range(2 * n):
        r = r_out if i % 2 == 0 else r_in
        a = math.radians(-90 + i * 180 / n)
        pts.append((cx + r * math.cos(a), cy + r * math.sin(a)))
    return poly(pts)


def claudito(m):
    """A soft starburst with a face (juguete's g)."""
    lines = _burst(60, 60, 42, 30) + ("" if m.mini else P("M52,70 Q60,78 68,70"))
    return lines, C(50, 56, 5) + C(70, 56, 5)


def doom(m):
    """The app's own monster: round, one eye, horns, teeth (juguete's g)."""
    lines = C(60, 64, 36) + P("M36,40 L28,16 L48,32") + P("M84,40 L92,16 L72,32") + C(60, 56, 12)
    lines += P("M42,82 H78")
    fills = C(60, 56, 5)
    if not m.mini:
        fills += poly([(48, 82), (52, 90), (56, 82)]) + poly([(64, 82), (68, 90), (72, 82)])
    return lines, fills


def duke3d(m):
    """The radiation sign (juguete's g)."""
    lines = C(60, 60, 42)
    blades = ""
    for c in (90, 210, 330):
        a0, a1 = math.radians(c - 30), math.radians(c + 30)
        r0, r1 = 12, 34
        blades += P(f"M{60 + r0 * math.cos(a0):.1f},{60 - r0 * math.sin(a0):.1f} "
                    f"L{60 + r1 * math.cos(a0):.1f},{60 - r1 * math.sin(a0):.1f} "
                    f"A{r1},{r1} 0 0 0 {60 + r1 * math.cos(a1):.1f},{60 - r1 * math.sin(a1):.1f} "
                    f"L{60 + r0 * math.cos(a1):.1f},{60 - r0 * math.sin(a1):.1f} "
                    f"A{r0},{r0} 0 0 1 {60 + r0 * math.cos(a0):.1f},{60 - r0 * math.sin(a0):.1f} Z")
    return lines, blades + C(60, 60, 7)


def emulador(m):
    lines = R(14, 60, 92, 40, 10) + L(40, 60, 40, 34)
    return lines, C(40, 28, 10) + C(70, 80, 6) + C(88, 74, 6) + ("" if m.mini else C(88, 90, 4))


def flappy(m):
    """The pipe and the bird (juguete's b)."""
    lines = R(64, 50, 30, 56, 4) + R(58, 40, 42, 14, 4) + C(36, 34, 16)
    return lines, C(41, 30, 3.5) + poly([(51, 34), (60, 37), (51, 40)])


def _gem(cx, cy, s):
    return (poly([(cx - 14 * s, cy - 8 * s), (cx - 7 * s, cy - 16 * s), (cx + 7 * s, cy - 16 * s),
                  (cx + 14 * s, cy - 8 * s), (cx, cy + 14 * s)])
            + L(cx - 14 * s, cy - 8 * s, cx + 14 * s, cy - 8 * s))


def gemas(m):
    """Three gems (juguete's b)."""
    return _gem(38, 46, 1.2) + _gem(82, 46, 1.2) + _gem(60, 80, 1.3), ""


def golf(m):
    """The flag in the hole (juguete's b)."""
    lines = L(56, 92, 56, 20) + P("M30,94 Q56,102 82,94")
    return lines, poly([(56, 20), (90, 32), (56, 44)]) + C(84, 86, 5)


def hockey(m):
    """The table (juguete's b)."""
    lines = R(26, 12, 68, 96, 14) + L(26, 60, 94, 60) + C(60, 60, 12) + L(48, 12, 72, 12)
    return lines, C(60, 86, 7) + C(60, 32, 7)


def mesa(m):
    """A pawn and a die (juguete's a)."""
    pawn = C(42, 34, 10) + P("M32,50 H52 L56,84 H28 Z") + L(22, 92, 62, 92)
    die = R(66, 58, 38, 38, 8)
    return pawn + die, C(77, 69, 4) + C(93, 85, 4) + C(85, 77, 4)


def metegol(m):
    """The pitch, two rods, two players each (juguete's a)."""
    lines = R(16, 22, 88, 76, 10) + L(60, 22, 60, 98) + C(60, 60, 11) + L(36, 12, 36, 108) + L(84, 12, 84, 108)
    fills = "".join(R(x - 7, y - 9, 14, 18, 5) for x in (36, 84) for y in (42, 78))
    return lines, fills + ("" if m.mini else C(70, 66, 4.5))


def monsterhop(m):
    """The vampire (juguete's e): a round head, the widow's peak, fangs, the cape's collar."""
    lines = C(60, 56, 26) + P("M38,46 Q48,34 60,46 Q72,34 82,46") + P("M24,104 L38,78 M96,104 L82,78")
    fills = poly([(52, 70), (55, 78), (58, 70)]) + poly([(62, 70), (65, 78), (68, 70)])
    return lines, fills + C(50, 56, 4) + C(70, 56, 4)


def neon(m):
    lines = P("M18,86 V54 H52 V86 H86 V32")
    return lines, C(86, 26, 7) + C(30, 28, 6)


def pinball(m):
    lines = P("M18,70 L30,58 L84,82 L80,90 Z")
    return lines, C(76, 38, 13)


def solitario(m):
    """Cards in a cascade (juguete's b)."""
    out, fills = "", ""
    for i, lab in enumerate(("K", "Q", "J")):
        x, y = 22 + i * 16, 14 + i * 22
        out += R(x, y, 50, 62, 6, KO)
        if not m.mini:
            fills += T(x + 12, y + 18, 15, lab)
    return out, fills


def sumar(m):
    lines = R(14, 30, 42, 42, 9) + R(64, 48, 42, 42, 9)
    return lines, T(35, 62, 28, "2") + T(85, 80, 28, "2")


def topos(m):
    """The mole's face peeking over the ground (juguete's g)."""
    lines = P("M24,90 C24,40 96,40 96,90") + L(10, 90, 110, 90)
    fills = C(48, 62, 4.5) + C(72, 62, 4.5) + P("M54,74 Q60,68 66,74 Q60,80 54,74 Z")
    if not m.mini:
        lines += L(52, 82, 52, 88) + L(60, 82, 60, 88) + L(68, 82, 68, 88)
    return lines, fills


def turbo(m):
    g = '<g transform="rotate(-30 60 60)">'
    lines = P("M44,18 H76 Q84,18 84,30 V92 Q84,102 74,102 H46 Q36,102 36,92 V30 Q36,18 44,18 Z") + R(44, 42, 32, 24, 6)
    fills = R(30, 30, 6, 18, 2) + R(84, 30, 6, 18, 2) + R(30, 76, 6, 18, 2) + R(84, 76, 6, 18, 2)
    return g + lines + "</g>", g + fills + "</g>"


def vitral(m):
    lines = R(20, 20, 80, 80, 10) + L(46.7, 20, 46.7, 100) + L(73.3, 20, 73.3, 100) + L(20, 46.7, 100, 46.7)
    lines += L(20, 73.3, 100, 73.3)
    fills = R(23, 23, 21, 21, 4) + R(76, 49, 21, 21, 4) + R(49, 76, 21, 21, 4)
    return lines, fills


def vuelo(m):
    g = '<g transform="rotate(-30 60 60)">'
    lines = L(60, 18, 60, 100) + P("M20,50 H100") + P("M44,92 H76") + L(52, 18, 68, 18)
    return g + lines + "</g>", ""


ART.update({
    "aos.mines": mines, "aos.simon": simon, "aos.truco": truco, "demo.2043": g2043, "demo.arkanos": arkanos,
    "demo.asteroides": asteroides, "demo.atasco": atasco, "demo.bastion": bastion, "demo.blackjack": blackjack,
    "demo.burbujas": burbujas, "demo.canos": canos, "demo.chatarra": chatarra, "demo.cjump": cjump,
    "demo.claudito": claudito, "demo.doom": doom, "demo.duke3d": duke3d, "demo.emulador": emulador,
    "demo.flappy": flappy, "demo.gemas": gemas, "demo.golf": golf, "demo.hockey": hockey, "demo.mesa": mesa,
    "demo.metegol": metegol, "demo.monsterhop": monsterhop, "demo.neon": neon, "demo.pinball": pinball,
    "demo.solitario": solitario, "demo.sumar": sumar, "demo.topos": topos, "demo.turbo": turbo,
    "demo.vitral": vitral, "demo.vuelo": vuelo,
})
PAIRS.update({
    "demo.doom": ("#FF9A7A", "#E3241C"), "demo.duke3d": ("#FFE36A", "#FF8A1A"), "demo.claudito": ("#FFC2A8", "#E0703E"),
    "demo.metegol": ("#B6FF6A", "#22C59A"), "demo.neon": ("#7CFFB2", "#FF4DA6"), "demo.vitral": ("#FF9AB0", "#4F86FF"),
    "aos.mines": ("#E6EAF2", "#FF9F0A"), "aos.simon": ("#7CFFB2", "#FF3B30"), "demo.mila": ("#FFB0D0", "#FF5AA0"),
})
