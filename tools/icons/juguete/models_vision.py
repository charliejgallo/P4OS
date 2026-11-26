"""'juguete' models: Visión (aos.vision), the camera that understands what it sees.

The app's green (0x30D158 -> 0x0A6E3A). Its own icon today is an eye between
a frame's four corners; the variants keep that idea, or the webcam, or what
the app draws over the picture (a box around what it found, with its label).
"""

import math

from lib import ball, capsule, cyl, glass, glossy, glow, group, metal, plastic, rbox, rounded_poly, slab, tile

VI_TOP, VI_BOT = "#4CDC72", "#0A6E3A"


def _corners(m, e=0.37, arm=0.14, w=0.05, z=0.03, mini=False):
    """A frame's four corners, as on a camera's screen."""
    parts = []
    k = 1.1 if mini else 1.0
    for sx in (-1, 1):
        for sy in (-1, 1):
            parts.append(rbox((sx * (e - arm * k / 2), sy * e, z), (arm * k, w * k, 0.05), w * k / 2, m))
            parts.append(rbox((sx * e, sy * (e - arm * k / 2), z), (w * k, arm * k, 0.05), w * k / 2, m))
    return parts


def _eye(s=1.0, z=0.08):
    white = glossy("#FFFFFF")
    pts = []
    for i in range(48):
        t = 2 * math.pi * i / 48
        pts.append((0.28 * s * math.cos(t), 0.16 * s * math.sin(t) * (1.0 if math.sin(t) >= 0 else 1.0)))
    almond = rounded_poly([(-0.3 * s, 0), (-0.12 * s, 0.15 * s), (0.12 * s, 0.15 * s), (0.3 * s, 0),
                           (0.12 * s, -0.15 * s), (-0.12 * s, -0.15 * s)], 0.08 * s, seg=10)
    return [slab([almond], 0.1, 0.035, white, z=z - 0.05),
            cyl((0, 0, z + 0.06), 0.11 * s, 0.03, glossy("#1FB04A"), verts=64),
            cyl((0, 0, z + 0.075), 0.05 * s, 0.02, plastic("#101010", 0.3), verts=48),
            ball((-0.035 * s, 0.035 * s, z + 0.09), 0.02 * s, glow("#FFFFFF", 1.0), scale=(1, 1, 0.4))]


def vision_a(mini):
    """An eye between a frame's four corners (the app's icon today)."""
    tile(VI_TOP, VI_BOT)
    parts = _corners(glossy("#FFFFFF"), mini=mini) + _eye(1.15 if not mini else 1.3)
    group(parts, loc=(0, 0, 0.04), rot=(-16, 10, 0), scale=1.0)


def vision_b(mini):
    """A round USB webcam on its clip, inside a detection box."""
    tile(VI_TOP, VI_BOT)
    dark = plastic("#2C2F36", 0.35)
    cam = [ball((0, 0.04, 0.16), 0.2, glossy("#F4F5F8"), scale=(1.15, 1, 0.8)),
           cyl((0, 0.04, 0.28), 0.12, 0.04, dark, bevel=0.015, verts=64),
           cyl((0, 0.04, 0.3), 0.08, 0.02, glass("#5ED38A"), verts=64),
           ball((0.16, 0.12, 0.27), 0.022, glow("#34C759", 2.5)),
           rbox((0, -0.2, 0.08), (0.06, 0.12, 0.06), 0.02, dark),
           rbox((0, -0.27, 0.05), (0.3, 0.06, 0.06), 0.03, dark)]
    if not mini:
        cam.append(capsule((0.12, -0.27, 0.04), (0.34, -0.36, 0.03), 0.018, dark))
    parts = cam + _corners(glossy("#FFFFFF"), e=0.4, arm=0.12, w=0.045, mini=mini)
    group(parts, loc=(0, 0, 0.04), rot=(-16, 10, 0), scale=1.0)


def vision_c(mini):
    """What the app draws: a box around a found face, its label tab on top."""
    tile(VI_TOP, VI_BOT)
    face = [ball((0, -0.04, 0.12), 0.2, glossy("#FFD2A8"), scale=(1, 1.1, 0.7))]
    for e in (-1, 1):
        face.append(ball((e * 0.07, 0.0, 0.25), 0.03, plastic("#1C1C1E")))
    if not mini:
        face.append(capsule((-0.06, -0.12, 0.25), (0.06, -0.12, 0.25), 0.014, plastic("#8A3A20")))
    box = []
    w = 0.045 if not mini else 0.06
    for sy in (-1, 1):
        box.append(rbox((0, sy * 0.3 - 0.04, 0.05), (0.62, w, 0.05), w / 2, glossy("#FFFFFF")))
    for sx in (-1, 1):
        box.append(rbox((sx * 0.31, -0.04, 0.05), (w, 0.64, 0.05), w / 2, glossy("#FFFFFF")))
    box.append(rbox((-0.16, 0.33, 0.06), (0.3, 0.1, 0.06), 0.03, glossy("#FFFFFF")))
    if not mini:
        box.append(rbox((-0.16, 0.33, 0.095), (0.2, 0.03, 0.01), 0.012, plastic("#1FB04A")))
    group(face + box, loc=(0, 0, 0.04), rot=(-16, 10, 0), scale=1.0)


MODELS = {"aos.vision": {"a": vision_a, "b": vision_b, "c": vision_c}}
