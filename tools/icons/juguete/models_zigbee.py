"""'juguete' models: the Zigbee app (aos.zigbee), the 802.15.4 radio.

The app's red (C_ZB, 0xEB0443 -> 0x9E0230). Mesh, radio and devices; nothing
of the Zigbee logo (a registered mark).
"""

import math

from lib import (ball, capsule, cyl, glossy, glow, group, metal, plastic, rbox, rounded_poly, slab, tile,
                 tube, waves)

ZB_TOP, ZB_BOT = "#F5245C", "#9E0230"


def _link(a, b, z, r, m):
    return tube([(a[0], a[1], z), (b[0], b[1], z)], r, m, order=2)


def zigbee_a(mini):
    """A mesh: white nodes, each linked to its neighbours, the coordinator
    glowing in the middle."""
    tile(ZB_TOP, ZB_BOT)
    ring = [(0.3 * math.cos(math.radians(90 + i * 72)), 0.3 * math.sin(math.radians(90 + i * 72))) for i in range(5)]
    if mini:
        ring = ring[::2] + [ring[1]]
    links = [(i, (i + 1) % len(ring)) for i in range(len(ring))]
    m_link = metal("#F4D7DE", 0.3)
    parts = [_link(ring[a], ring[b], 0.07, 0.02 if not mini else 0.03, m_link) for a, b in links]
    parts += [_link((0, 0), p, 0.07, 0.02 if not mini else 0.03, m_link) for p in ring]
    parts += [ball((x, y, 0.08), 0.08 if not mini else 0.1, glossy("#FFFFFF")) for x, y in ring]
    parts.append(ball((0, 0, 0.1), 0.12 if not mini else 0.14, glow("#FFD0DC", 0.9)))
    group(parts, loc=(0, 0.0, 0.04), rot=(-16, 10, 0), scale=1.0)


def zigbee_b(mini):
    """The radio itself: a little module, its chip and its zigzag antenna,
    sending waves out."""
    tile(ZB_TOP, ZB_BOT)
    parts = [rbox((-0.04, -0.06, 0.04), (0.5, 0.6, 0.06), 0.03, plastic("#1F2A33", 0.4)),
             rbox((-0.04, -0.14, 0.1), (0.26, 0.26, 0.06), 0.025, metal("#C9CED6", 0.3))]
    zig = [(-0.24, 0.12, 0.08), (-0.24, 0.2, 0.08), (-0.14, 0.2, 0.08), (-0.14, 0.12, 0.08), (-0.04, 0.12, 0.08),
           (-0.04, 0.2, 0.08), (0.06, 0.2, 0.08), (0.06, 0.12, 0.08), (0.16, 0.12, 0.08)]
    parts.append(tube(zig if not mini else zig[::2], 0.016 if not mini else 0.025, metal("#F2C14E", 0.25), order=2))
    if not mini:
        for i in range(5):
            parts.append(rbox((-0.25 + i * 0.105, -0.36, 0.05), (0.05, 0.05, 0.02), 0.01, metal("#F2C14E", 0.25)))
        parts.append(ball((0.14, -0.06, 0.09), 0.025, glow("#5CFF9A", 2.0)))
    parts += waves(0.2, 0.16, glossy("#FFFFFF"), 2 if not mini else 1, 0.1, 0.09, 0.045, -40, 70, z=0.06)
    group(parts, loc=(0, 0.0, 0.02), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


def zigbee_c(mini):
    """A hub in the middle and the house's devices around it: a bulb, a
    sensor, a plug, linked to it."""
    tile(ZB_TOP, ZB_BOT)
    hub = [cyl((0, 0, 0.08), 0.17, 0.14, glossy("#FFFFFF"), bevel=0.06, verts=64),
           cyl((0, 0, 0.15), 0.07, 0.02, glow("#FF6A8A", 1.5), verts=48)]
    dev = [(-0.27, 0.22), (0.28, 0.2), (0.0, -0.32)]
    m_link = metal("#F4D7DE", 0.3)
    parts = hub + [_link((0, 0), d, 0.06, 0.02 if not mini else 0.03, m_link) for d in dev]
    # a bulb
    x, y = dev[0]
    parts += [ball((x, y + 0.03, 0.1), 0.09, glow("#FFE07A", 1.4)),
              cyl((x, y - 0.07, 0.1), 0.045, 0.06, metal(), rot=(math.radians(90), 0, 0))]
    # a sensor: a small white puck with a dot
    x, y = dev[1]
    parts += [rbox((x, y, 0.07), (0.17, 0.17, 0.1), 0.05, glossy("#F4F5F8")),
              ball((x, y, 0.12), 0.025, glow("#5CFF9A", 2.0))]
    # a plug
    x, y = dev[2]
    parts += [rbox((x, y, 0.07), (0.18, 0.16, 0.12), 0.06, glossy("#F4F5F8"))]
    if not mini:
        for dx in (-0.035, 0.035):
            parts.append(rbox((x + dx, y, 0.135), (0.02, 0.05, 0.01), 0.008, plastic("#2C2C2E")))
    group(parts, loc=(0, 0.0, 0.04), rot=(-16, 10, 0), scale=1.05 if mini else 1.0)


def zigbee_d(mini):
    """A Z drawn by a mesh: four nodes at its corners, linked along the
    letter, radio waves leaving the last one. Not the Zigbee mark's shape."""
    tile(ZB_TOP, ZB_BOT)
    pts = [(-0.24, 0.22), (0.22, 0.22), (-0.22, -0.22), (0.24, -0.22)]
    m_link = glossy("#FFFFFF")
    r = 0.05 if not mini else 0.065
    parts = [_link(pts[i], pts[i + 1], 0.08, r, m_link) for i in range(3)]
    parts += [ball((x, y, 0.08), 0.085 if not mini else 0.1, glossy("#FFFFFF")) for x, y in pts]
    parts.append(ball((pts[3][0], pts[3][1], 0.1), 0.05, glow("#FFD0DC", 1.2)))
    if not mini:
        parts += waves(pts[1][0], pts[1][1], glossy("#FFD0DC"), 2, 0.12, 0.08, 0.035, -20, 70, z=0.06)
    group(parts, loc=(0, 0.0, 0.04), rot=(-16, 10, 0), scale=1.0)


def zigbee_e(mini):
    """The app's own glyph today (AOS_SYM_ACCESS_POINT, '((o))'): a dot in the
    middle and two pairs of arcs, one each side, white on the app's red."""
    tile(ZB_TOP, ZB_BOT)
    white = glossy("#FFFFFF")
    w = 0.06 if not mini else 0.075
    parts = [ball((0, 0, 0.08), 0.1 if not mini else 0.12, white)]
    n = 2
    r0, step = (0.17, 0.12) if not mini else (0.19, 0.13)
    parts += waves(0, 0, white, n, r0, step, w, -45, 45, z=0.05)
    parts += waves(0, 0, white, n, r0, step, w, 135, 225, z=0.05)
    group(parts, loc=(0, 0.0, 0.04), rot=(-16, 10, 0), scale=1.0)


MODELS = {"aos.zigbee": {"a": zigbee_a, "b": zigbee_b, "c": zigbee_c, "d": zigbee_d, "e": zigbee_e}}
