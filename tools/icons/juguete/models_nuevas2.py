"""'juguete' models: Malla, Mensajes, Correo, Cielo and Bot (a, b, c each).

Mensajes and Bot talk to Telegram, but nothing of Telegram's mark (its API's
terms ask that of unofficial clients) and no paper plane anywhere: bubbles,
robots, envelopes.
"""

import math

from lib import (ball, capsule, circle, cyl, glass, glossy, glow, group, matte, metal, plastic, rbox,
                 rounded_poly, rrect, slab, star, text, tile, tube, waves)

R90 = math.radians(90)


def _bubble(cx, cy, w, h, m, z=0.04, tail=-1, depth=0.1):
    """A speech bubble: a rounded box with a tail at a bottom corner (tail
    -1 left, 1 right)."""
    x0, x1, y0, y1 = cx - w / 2, cx + w / 2, cy - h / 2, cy + h / 2
    tx = x0 + w * 0.22 if tail < 0 else x1 - w * 0.22
    pts = [(x0, y0), (tx - 0.04 * tail, y0), (tx - 0.02 * tail + (-0.08 if tail < 0 else 0.08), y0 - 0.1),
           (tx + 0.06 * tail, y0), (x1, y0), (x1, y1), (x0, y1)]
    r = min(w, h) * 0.32
    return slab([rounded_poly(pts, [r, 0.01, 0.012, 0.01, r, r, r])], depth, 0.03, m, z=z)


def _dots(cx, cy, z, s, m, n=3):
    return [ball((cx + (i - (n - 1) / 2) * 0.09 * s, cy, z), 0.032 * s, m, scale=(1, 1, 0.6)) for i in range(n)]


# -- Malla (Meshtastic: a LoRa mesh) ---------------------------------------

MA_TOP, MA_BOT = "#50D88E", "#0B6B44"


def _node(x, y, z, s, body, mini):
    parts = [rbox((x, y, z), (0.2 * s, 0.26 * s, 0.1 * s), 0.04 * s, body),
             capsule((x + 0.06 * s, y + 0.13 * s, z + 0.03 * s), (x + 0.06 * s, y + 0.3 * s, z + 0.04 * s), 0.018 * s,
                     plastic("#2C2F36")),
             ball((x - 0.04 * s, y + 0.06 * s, z + 0.055 * s), 0.02 * s, glow("#5CFF9A", 2.5))]
    if not mini:
        parts.append(rbox((x, y - 0.05 * s, z + 0.055 * s), (0.13 * s, 0.07 * s, 0.01), 0.01, glossy("#1C2A22")))
    return parts


def malla_a(mini):
    """Three radio nodes linked into a mesh."""
    tile(MA_TOP, MA_BOT)
    pts = [(-0.24, 0.16), (0.24, 0.2), (0.0, -0.24)]
    parts = []
    for i in range(3):
        a, b = pts[i], pts[(i + 1) % 3]
        parts.append(tube([(a[0], a[1], 0.05), (b[0], b[1], 0.05)], 0.018, plastic("#E9FFF0", 0.4), order=2))
    for x, y in pts:
        parts += _node(x, y, 0.08, 0.85 if not mini else 1.0, glossy("#FFFFFF"), mini)
    group(parts, loc=(0, 0, 0.04), rot=(-16, 10, 0), scale=1.0)


def malla_b(mini):
    """A handheld LoRa node, its stubby antenna sending waves."""
    tile(MA_TOP, MA_BOT)
    parts = _node(-0.06, -0.06, 0.1, 1.6, glossy("#FFFFFF"), mini)
    parts += waves(0.04, 0.42, glossy("#FFFFFF"), 2 if not mini else 1, 0.1, 0.09, 0.045, -30, 60, z=0.12)
    group(parts, loc=(0, -0.02, 0.02), rot=(-16, 10, 0), scale=1.0)


def malla_c(mini):
    """A speech bubble over a little mesh of nodes: messages through the mesh."""
    tile(MA_TOP, MA_BOT)
    parts = [_bubble(0.02, 0.14, 0.62, 0.38, glossy("#FFFFFF"), z=0.1)]
    parts += _dots(0.02, 0.14, 0.21, 1.4 if not mini else 1.7, glossy("#2EB86A"))
    nodes = [(-0.28, -0.28), (0.0, -0.34), (0.28, -0.26)]
    for i in range(2):
        a, b = nodes[i], nodes[i + 1]
        parts.append(tube([(a[0], a[1], 0.05), (b[0], b[1], 0.05)], 0.018, plastic("#E9FFF0", 0.4), order=2))
    parts += [ball((x, y, 0.06), 0.055, glossy("#FFFFFF")) for x, y in nodes]
    group(parts, loc=(0, 0, 0.04), rot=(-16, 10, 0), scale=1.0)


# -- Mensajes (a Telegram client: no Telegram mark, no paper plane) -------

ME_TOP, ME_BOT = "#5AB4FF", "#1A5FB4"


def mensajes_a(mini):
    """Two chubby speech bubbles, one answering the other."""
    tile(ME_TOP, ME_BOT)
    parts = [_bubble(-0.1, 0.14, 0.56, 0.36, glossy("#FFFFFF"), z=0.04, tail=-1),
             _bubble(0.12, -0.16, 0.5, 0.32, glossy("#2F7BEA"), z=0.14, tail=1)]
    if not mini:
        parts += [rbox((-0.14, 0.18, 0.15), (0.32, 0.04, 0.01), 0.015, plastic("#BFD8F5")),
                  rbox((-0.18, 0.09, 0.15), (0.24, 0.04, 0.01), 0.015, plastic("#BFD8F5"))]
        parts += _dots(0.12, -0.15, 0.25, 1.0, glossy("#FFFFFF"))
    group(parts, loc=(0, 0, 0.04), rot=(-16, 10, 0), scale=1.0)


def mensajes_b(mini):
    """A chat window: a rounded screen with bubbles in it."""
    tile(ME_TOP, ME_BOT)
    parts = [rbox((0, 0, 0.05), (0.56, 0.78, 0.08), 0.1, glossy("#FFFFFF")),
             rbox((0, 0.31, 0.095), (0.56, 0.1, 0.01), 0.04, plastic("#2F7BEA"))]
    rows = ((-0.08, 0.14, 0.3, "#E3EEFB"), (0.08, 0.0, 0.3, "#2F7BEA"), (-0.06, -0.14, 0.34, "#E3EEFB"),
            (0.08, -0.27, 0.26, "#2F7BEA"))
    for x, y, w, c in (rows if not mini else rows[:3]):
        parts.append(rbox((x, y, 0.1), (w, 0.09, 0.03), 0.04, glossy(c)))
    group(parts, loc=(0, 0, 0.04), rot=(-16, 10, 0), scale=1.0)


def mensajes_c(mini):
    """One big bubble, someone typing in it."""
    tile(ME_TOP, ME_BOT)
    parts = [_bubble(0.0, 0.04, 0.74, 0.54, glossy("#FFFFFF"), z=0.04, tail=-1, depth=0.12)]
    parts += _dots(0.0, 0.04, 0.17, 1.8, glossy("#2F7BEA"))
    group(parts, loc=(0, 0.02, 0.04), rot=(-16, 10, 0), scale=1.0)


# -- Correo ----------------------------------------------------------------

CO_TOP, CO_BOT = "#5AB0FF", "#0A4FB8"


def _envelope(m_body, m_flap, s=1.0):
    parts = [rbox((0, 0, 0.05), (0.7 * s, 0.48 * s, 0.08), 0.04, m_body)]
    parts.append(slab([rounded_poly([(-0.35 * s, 0.24 * s), (0.35 * s, 0.24 * s), (0, -0.04 * s)], 0.03)], 0.03, 0.012,
                      m_flap, z=0.09))
    return parts


def correo_a(mini):
    """A closed envelope with a red seal."""
    tile(CO_TOP, CO_BOT)
    parts = _envelope(glossy("#FFFFFF"), plastic("#E3EEFB", 0.4), 1.1)
    parts.append(cyl((0, -0.03, 0.13), 0.07, 0.03, glossy("#E33030"), bevel=0.012))
    group(parts, loc=(0, 0, 0.04), rot=(-16, 10, 0), scale=1.0)


def correo_b(mini):
    """An open envelope, a letter coming out of it."""
    tile(CO_TOP, CO_BOT)
    parts = [rbox((0, 0.12, 0.04), (0.54, 0.5, 0.03), 0.02, plastic("#FFFFFF", 0.45))]
    if not mini:
        for i, w in enumerate((0.38, 0.38, 0.24)):
            parts.append(rbox((-0.19 + w / 2, 0.28 - i * 0.08, 0.06), (w, 0.03, 0.01), 0.01, plastic("#C3C8D1")))
    env = [rbox((0, -0.14, 0.09), (0.7, 0.4, 0.1), 0.04, glossy("#FFD860")),
           slab([rounded_poly([(-0.35, -0.34), (0.35, -0.34), (0.0, -0.08)], 0.03)], 0.03, 0.012, glossy("#F2B830"), z=0.14)]
    group(parts + env, loc=(0, 0.02, 0.04), rot=(-16, 10, 0), scale=1.0)


def correo_c(mini):
    """A mailbox on its post, the red flag up."""
    tile(CO_TOP, CO_BOT)
    body = slab([rounded_poly([(-0.26, -0.12), (0.26, -0.12), (0.26, 0.14), (0.0, 0.3), (-0.26, 0.14)],
                              [0.03, 0.03, 0.12, 0.2, 0.12])], 0.3, 0.06, glossy("#F4F5F8"), z=0.04)
    parts = [body, rbox((0, -0.3, 0.06), (0.1, 0.2, 0.08), 0.03, plastic("#7A5A3C")),
             rbox((0.3, 0.12, 0.26), (0.04, 0.26, 0.04), 0.015, glossy("#E33030")),
             rbox((0.36, 0.22, 0.26), (0.12, 0.08, 0.04), 0.015, glossy("#E33030")),
             rbox((-0.1, 0.0, 0.35), (0.16, 0.04, 0.02), 0.015, plastic("#2C2F36"))]
    group(parts, loc=(-0.04, 0.02, 0.02), rot=(-16, 10, 0), scale=1.0)


# -- Cielo -----------------------------------------------------------------

CI_TOP, CI_BOT = "#2E4A8A", "#070D1C"


def _stars(pts, s=1.0, k=2.0):
    return [slab([rounded_poly(star(x, y, r * s, r * 0.42 * s), 0.006)], 0.03, 0.008, glow("#FFF2B0", k), z=0.02)
            for x, y, r in pts]


def cielo_a(mini):
    """A telescope on its tripod, stars over it."""
    tile(CI_TOP, CI_BOT)
    white = glossy("#F4F5F8")
    tub = [cyl((0, 0, 0), 0.08, 0.5, white, rot=(0, R90, 0), bevel=0.02),
           cyl((0.27, 0, 0), 0.1, 0.06, glossy("#2C2F36"), rot=(0, R90, 0), bevel=0.015),
           cyl((-0.26, 0, 0), 0.04, 0.06, glossy("#2C2F36"), rot=(0, R90, 0))]
    parts = [group(tub, loc=(0.0, 0.04, 0.24), rot=(0, 0, 30), name="tube")]
    for dx in (-0.16, 0.0, 0.16):
        parts.append(capsule((0.0, 0.0, 0.2), (dx, -0.36, 0.04), 0.018, plastic("#8E939C")))
    stars = ((-0.3, 0.3, 0.06), (0.3, 0.34, 0.05), (-0.36, -0.06, 0.04)) if not mini else ((-0.28, 0.28, 0.08),)
    group(parts + _stars(stars), loc=(0, 0, 0.04), rot=(-14, 8, 0), scale=1.0)


def cielo_b(mini):
    """A ringed planet among stars."""
    tile(CI_TOP, CI_BOT)
    parts = [ball((0, 0, 0.18), 0.22, glossy("#FFB04A"))]
    ring = slab([circle(0, 0, 0.38), circle(0, 0, 0.29)], 0.02, 0.008, glossy("#FFE2A8"))
    ring.location = (0, 0, 0.18)
    ring.rotation_euler = (math.radians(70), math.radians(-20), 0)
    parts.append(ring)
    if not mini:
        parts += [ball((-0.08, 0.06, 0.38), 0.04, matte("#E08A2A"), scale=(1.6, 1, 0.3))]
    stars = ((-0.32, 0.3, 0.06), (0.3, -0.32, 0.05), (0.34, 0.3, 0.04)) if not mini else ((-0.3, 0.3, 0.08),)
    group(parts + _stars(stars), loc=(0, 0, 0.0), rot=(-10, 6, 0), scale=1.0)


def cielo_c(mini):
    """The Moon and a space station passing by."""
    tile(CI_TOP, CI_BOT)
    parts = [ball((-0.14, 0.12, 0.14), 0.2, glossy("#F2EEDC")),
             ball((-0.2, 0.18, 0.31), 0.04, matte("#C8C2A8"), scale=(1, 1, 0.3)),
             ball((-0.06, 0.06, 0.32), 0.03, matte("#C8C2A8"), scale=(1, 1, 0.3))]
    st = [rbox((0, 0, 0), (0.08, 0.08, 0.06), 0.02, metal("#D7DBE2")),
          rbox((0, 0, 0), (0.42, 0.02, 0.02), 0.008, metal("#B0B6C0"))]
    for x in (-0.18, 0.18):
        st.append(rbox((x, 0, 0.01), (0.1, 0.2, 0.01), 0.008, glossy("#2D5BD8")))
    parts.append(group(st, loc=(0.18, -0.2, 0.2), rot=(0, 0, -25), scale=1.0 if not mini else 1.3, name="station"))
    if not mini:
        parts += _stars(((0.3, 0.32, 0.05), (-0.36, -0.3, 0.04)))
    group(parts, loc=(0, 0, 0.02), rot=(-12, 8, 0), scale=1.0)


# -- Bot (the board's Telegram bot: no Telegram mark) ----------------------

BO_TOP, BO_BOT = "#6ED2FF", "#1C7ED6"


def _robot(x, y, z, s, mini, face="#1C2A44"):
    white = glossy("#F4F5F8")
    parts = [rbox((x, y, z), (0.5 * s, 0.4 * s, 0.24 * s), 0.1 * s, white),
             rbox((x, y - 0.01 * s, z + 0.1 * s), (0.38 * s, 0.24 * s, 0.06 * s), 0.05 * s, glossy(face))]
    for e in (-1, 1):
        parts.append(ball((x + e * 0.08 * s, y + 0.0, z + 0.14 * s), 0.04 * s, glow("#7FE3FF", 2.0), scale=(1, 1.2, 0.6)))
    parts += [capsule((x, y + 0.2 * s, z), (x, y + 0.3 * s, z + 0.02 * s), 0.016 * s, metal()),
              ball((x, y + 0.32 * s, z + 0.02 * s), 0.04 * s, glossy("#FFD60A"))]
    if not mini:
        for e in (-1, 1):
            parts.append(cyl((x + e * 0.26 * s, y, z), 0.05 * s, 0.05 * s, glossy("#5AC8FA"), rot=(0, R90, 0),
                             bevel=0.012))
    return parts


def bot_a(mini):
    """A robot's head, its antenna up."""
    tile(BO_TOP, BO_BOT)
    group(_robot(0, -0.04, 0.12, 1.25, mini), loc=(0, 0, 0), rot=(-14, 10, 0), scale=1.0)


def bot_b(mini):
    """A robot's head with a speech bubble: the bot that answers."""
    tile(BO_TOP, BO_BOT)
    parts = _robot(-0.1, -0.12, 0.12, 0.95, True)
    parts.append(_bubble(0.2, 0.26, 0.34, 0.24, glossy("#FFFFFF"), z=0.16, tail=-1, depth=0.08))
    parts += _dots(0.2, 0.26, 0.25, 0.75, glossy("#1C7ED6"))
    group(parts, loc=(0, 0, 0), rot=(-14, 10, 0), scale=1.0)


def bot_c(mini):
    """A robot's head with a gear beside it: the rules it follows."""
    tile(BO_TOP, BO_BOT)
    from lib import gear_loop
    parts = _robot(-0.08, 0.04, 0.12, 1.0, True)
    g = slab([gear_loop(0.15, 0.11, 8, 0.5, (0.018, 0.024)), circle(0, 0, 0.05)], 0.08, 0.02, glossy("#FFD60A"))
    g.location.x, g.location.y = 0.24, -0.26
    parts.append(g)
    group(parts, loc=(0, 0, 0), rot=(-14, 10, 0), scale=1.0)


MODELS = {
    "aos.malla": {"a": malla_a, "b": malla_b, "c": malla_c},
    "aos.mensajes": {"a": mensajes_a, "b": mensajes_b, "c": mensajes_c},
    "aos.correo": {"a": correo_a, "b": correo_b, "c": correo_c},
    "aos.cielo": {"a": cielo_a, "b": cielo_b, "c": cielo_c},
    "aos.bot": {"a": bot_a, "b": bot_b, "c": bot_c},
}


# -- Grabador (a webcam as a security camera that records on motion) -------

GR_TOP, GR_BOT = "#7C8CA3", "#1E293B"


def _rec_dot(x, y, z, r):
    return [ball((x, y, z), r, glow("#FF3B30", 2.5)),
            ball((x - r * 0.3, y + r * 0.3, z + r * 0.8), r * 0.3, glow("#FFFFFF", 0.8), scale=(1, 1, 0.4))]


def grabador_a(mini):
    """A dark security camera on its arm, a big red recording light on it
    (white is Cámaras' camera; this one is black and records)."""
    tile(GR_TOP, GR_BOT)
    body = glossy("#2C2F36")
    parts = [rbox((0.04, 0.08, 0.18), (0.6, 0.28, 0.24), 0.1, body),
             cyl((-0.27, 0.08, 0.18), 0.12, 0.06, plastic("#16161A"), rot=(0, R90, 0), bevel=0.02),
             cyl((-0.3, 0.08, 0.18), 0.08, 0.04, glass("#6FA8FF"), rot=(0, R90, 0)),
             rbox((0.18, -0.14, 0.08), (0.08, 0.2, 0.08), 0.03, body),
             rbox((0.2, -0.28, 0.04), (0.26, 0.12, 0.06), 0.04, body)]
    parts += _rec_dot(0.12, 0.1, 0.33, 0.08 if not mini else 0.1)
    group(parts, loc=(0.02, 0.02, 0.0), rot=(-28, 0, -24), scale=1.05 if mini else 1.0)


def grabador_b(mini):
    """A round webcam watching, a red REC badge beside it."""
    tile(GR_TOP, GR_BOT)
    dark = plastic("#2C2F36", 0.35)
    parts = [ball((-0.06, 0.06, 0.16), 0.2, glossy("#F4F5F8"), scale=(1.15, 1, 0.8)),
             cyl((-0.06, 0.06, 0.28), 0.12, 0.04, dark, bevel=0.015, verts=64),
             cyl((-0.06, 0.06, 0.3), 0.08, 0.02, glass("#6FA8FF"), verts=64),
             rbox((-0.06, -0.2, 0.08), (0.06, 0.12, 0.06), 0.02, dark),
             rbox((-0.06, -0.27, 0.05), (0.3, 0.06, 0.06), 0.03, dark),
             rbox((0.24, 0.3, 0.1), (0.26, 0.13, 0.06), 0.06, glossy("#FFFFFF"))]
    parts += _rec_dot(0.17, 0.3, 0.15, 0.035 if not mini else 0.05)
    if not mini:
        parts.append(rbox((0.28, 0.3, 0.135), (0.1, 0.035, 0.01), 0.015, plastic("#2C2F36")))
    group(parts, loc=(0, 0, 0.04), rot=(-16, 10, 0), scale=1.0)


def grabador_c(mini):
    """A screen showing someone walking by, boxed by the motion detector,
    and the red dot: recording."""
    tile(GR_TOP, GR_BOT)
    parts = [rbox((0, 0.02, 0.05), (0.78, 0.6, 0.1), 0.06, plastic("#2C2F36", 0.35)),
             rbox((0, 0.02, 0.1), (0.68, 0.5, 0.02), 0.04, glossy("#1C2A44"))]
    person = [ball((0.02, 0.11, 0.13), 0.065, glossy("#E9EEF5"), scale=(1, 1, 0.5)),
              rbox((0.02, -0.05, 0.12), (0.16, 0.2, 0.03), 0.07, glossy("#E9EEF5"))]
    w = 0.02 if not mini else 0.03
    box = [rbox((0.02, 0.18, 0.12), (0.24, w, 0.01), 0.008, glow("#5CFF9A", 1.5)),
           rbox((0.02, -0.14, 0.12), (0.24, w, 0.01), 0.008, glow("#5CFF9A", 1.5)),
           rbox((-0.1, 0.02, 0.12), (w, 0.34, 0.01), 0.008, glow("#5CFF9A", 1.5)),
           rbox((0.14, 0.02, 0.12), (w, 0.34, 0.01), 0.008, glow("#5CFF9A", 1.5))]
    parts += person + box + _rec_dot(-0.25, 0.18, 0.13, 0.04 if not mini else 0.06)
    group(parts, loc=(0, 0, 0.06), rot=(-16, 10, 0), scale=1.0)


MODELS["aos.grabador"] = {"a": grabador_a, "b": grabador_b, "c": grabador_c}
