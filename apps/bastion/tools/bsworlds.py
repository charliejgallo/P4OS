"""
BASTION - the worlds: where the aliens walk, where towers can stand, who
attacks and when. Plain Python with no imports beyond the standard library,
so Blender's Python reads it too (tools/blender/worlds.py draws each world
from these numbers) and pack_p4.py writes each world's definition from it.

Units are metres on the ground: X across the screen (-9 left .. 9 right),
Y away from the camera (0 is the screen's bottom edge, 40 its top edge).
The HUD covers Y > 37 and the tower bar Y < 6, so what matters happens in
between.

    python3 bsworlds.py preview out.png      # every layout, drawn flat
    python3 bsworlds.py check                # the rules below, and the waves
"""
import math
import random
import sys

PATH_W = 2.2            # the road's width
PAD_R = 0.95            # a build pad's radius
FORT_R = 2.7            # the fort's footprint
XMAX = 8.2
YMIN, YMAX = 6.8, 36.4
STEP = 0.4              # the dense paths' step

# what each alien costs the wave generator, and from which world's number
# on it may appear (bosses are placed by hand at the end of a world)
ALIENS = {
    #  id        threat  first world
    'grub':    (1.0, 1),
    'runner':  (1.3, 1),
    'swarm':   (0.45, 1),
    'shell':   (3.2, 2),
    'wasp':    (1.8, 2),
    'split':   (3.0, 3),
    'shield':  (3.6, 4),
    'healer':  (4.0, 4),
    'brute':   (9.0, 5),
    'phase':   (3.4, 6),
    'jammer':  (5.0, 7),
    'carrier': (12.0, 8),
}
BOSSES = ['queen', 'titan', 'overmind']

# How tough each of the thirty sectors' aliens are, in percent of their
# base health: measured by the harness's bot (tools/harness.sh -c), which
# plays the campaign in order with the laboratory a player would have by
# then and looks for the health it only just holds. Paste its table here.
# (By hand since: the second world's first two sectors 10 % down. The user
# won the whole first world with the fort untouched and then kept 4 lives
# of 20 in the first sector of the second, where the fliers first come.)
HP = [404, 555, 598, 250, 285, 490, 336, 448, 539, 297, 230, 306, 223, 231, 341, 349, 511, 419, 360, 414, 551, 272, 295,
      326, 397, 468, 465, 286, 395, 261]
import os
# a boss's health against its sector's
BOSS_K = 1.0
# through a sector the aliens toughen from RAMP0 to RAMP1 of its health:
# the player starts with three towers and ends with every pad full
RAMP0 = float(os.environ.get('BS_R0', 0.6))
RAMP1 = float(os.environ.get('BS_R1', 2.0))
# and from sector to sector a wave brings this much more of them
GROW = float(os.environ.get('BS_GROW', 1.02))
# BS_FLAT=1: every sector at 100 %, for the harness to measure from
import os
FLAT = bool(os.environ.get('BS_FLAT'))


def fillet(pts, r=1.6):
    """a polyline with its corners rounded, as points STEP apart"""
    out = []
    n = len(pts)
    corners = [pts[0]]
    for i in range(1, n - 1):
        p0, p1, p2 = pts[i - 1], pts[i], pts[i + 1]
        d0 = (p0[0] - p1[0], p0[1] - p1[1])
        d1 = (p2[0] - p1[0], p2[1] - p1[1])
        l0, l1 = math.hypot(*d0), math.hypot(*d1)
        rr = min(r, l0 / 2.05, l1 / 2.05)
        a = (p1[0] + d0[0] / l0 * rr, p1[1] + d0[1] / l0 * rr)
        b = (p1[0] + d1[0] / l1 * rr, p1[1] + d1[1] / l1 * rr)
        corners.append(a)
        for k in range(1, 8):
            t = k / 8.0
            # a quadratic through the corner: close enough to an arc
            corners.append(((1 - t) ** 2 * a[0] + 2 * t * (1 - t) * p1[0] + t * t * b[0],
                            (1 - t) ** 2 * a[1] + 2 * t * (1 - t) * p1[1] + t * t * b[1]))
        corners.append(b)
    corners.append(pts[-1])
    # resample at STEP
    out = [corners[0]]
    carry = 0.0
    for i in range(len(corners) - 1):
        a, b = corners[i], corners[i + 1]
        seg = math.hypot(b[0] - a[0], b[1] - a[1])
        if seg < 1e-6:
            continue
        d = STEP - carry
        while d <= seg:
            t = d / seg
            out.append((a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t))
            d += STEP
        carry = seg - (d - STEP)
    if math.hypot(out[-1][0] - corners[-1][0], out[-1][1] - corners[-1][1]) > 0.05:
        out.append(corners[-1])
    return out


class World:
    def __init__(self, wid, order, names, descs, theme, fort, paths, npads=16, hue=0, music=0, boss='queen',
                 accent='#40c8ff', ground='#404860', road='#8890a8', sky='#101428', avoid=(), extra=()):
        self.id = wid
        self.order = order
        self.names = names          # (es, en, de)
        self.descs = descs
        self.theme = theme          # what tools/blender/worlds.py builds
        self.fort = fort
        self.way = paths            # waypoints, as written
        self.paths = [fillet(p) for p in paths]
        self.hue = hue              # the aliens' colours turned by this many degrees, so they
                                    # stand out from this world's ground
        self.music = music
        self.boss = boss
        self.accent, self.ground, self.road, self.sky = accent, ground, road, sky
        # fliers cut the road's corners: from where each road starts, over
        # the points a third and two thirds along it, to the fort. A
        # straight line to the fort could run along an edge of the map,
        # out of every pad's reach.
        self.air = []
        for p in self.paths:
            n = len(p)
            way = [p[0], p[n // 3], p[2 * n // 3], fort]
            self.air.append(fillet(way, 3.0))
        self.pads = self._pads(npads, avoid, extra)

    def dist_path(self, x, y):
        return min(math.hypot(x - px, y - py) for p in self.paths for (px, py) in p)

    def _pads(self, n, avoid, extra):
        """the n best places beside the road: each sees as much road as it
        can, none crowds another, and they spread over the whole way"""
        pts = [q for p in self.paths for q in p]
        cand = []
        gx = [i * 0.5 for i in range(int(-XMAX * 2), int(XMAX * 2) + 1)]
        gy = [YMIN + i * 0.5 for i in range(int((YMAX - YMIN) * 2) + 1)]
        for x in gx:
            for y in gy:
                d = min(math.hypot(x - px, y - py) for (px, py) in pts)
                if d < PATH_W / 2 + PAD_R + 0.25 or d > 3.4:
                    continue
                if math.hypot(x - self.fort[0], y - self.fort[1]) < FORT_R + PAD_R + 0.3:
                    continue
                if any(math.hypot(x - ax, y - ay) < ar for (ax, ay, ar) in avoid):
                    continue
                cover = sum(1 for (px, py) in pts if math.hypot(x - px, y - py) < 4.6 and YMIN - 1 < py < YMAX + 1)
                cand.append((cover, x, y))
        cand.sort(reverse=True)
        chosen = [(x, y) for (x, y) in extra]
        # in thirds of the road's length, so the start is not left bare
        total = sum(len(p) for p in self.paths)
        for (cover, x, y) in cand:
            if len(chosen) >= n:
                break
            if all(math.hypot(x - cx, y - cy) >= 2.5 for (cx, cy) in chosen):
                chosen.append((x, y))
        del total
        return sorted(chosen, key=lambda p: (-p[1], p[0]))

    # ---- waves -----------------------------------------------------------
    def roster(self, sector, wave, nwaves):
        """which aliens may come in this wave: the world's own, the newer
        ones later in the sector"""
        r = [a for a, (cost, first) in ALIENS.items() if first <= self.order]
        r.sort(key=lambda a: ALIENS[a][1] * 10 + ALIENS[a][0])
        # the newest two kinds wait for the second third of the sector
        if wave < nwaves // 3 and len(r) > 3:
            r = r[:-2]
        return r

    def waves(self, sector):
        """the waves of sector 0, 1 or 2: a list of waves, each a list of
        groups (alien, count, gap seconds, delay seconds, path, hp percent).

        How many come grows with the wave and a little with the level; how
        tough they are is HP[level], which tools/harness.sh measured with
        its bot (the level's number alone would not say what a player with
        that many pads and that laboratory can stop), rising through the
        sector; and they come closer together towards the end."""
        rnd = random.Random(self.order * 100 + sector)
        nw = 8 + sector * 2 + (self.order - 1) // 3
        level = (self.order - 1) * 3 + sector          # 0..29
        base = 10.0 * (GROW ** level)
        hp0 = 100 if FLAT else HP[level]
        out = []
        for w in range(nw):
            u = w / float(nw - 1)
            budget = base * (1.0 + 0.5 * w)
            hp = max(10, int(hp0 * RAMP0 * (RAMP1 / RAMP0) ** u))
            ros = self.roster(sector, w, nw)
            groups = []
            last = w == nw - 1
            kinds = 1 if w < 2 else (2 if w < 5 else 3)
            if last and sector == 2:
                # the world's boss and its escort
                groups.append((self.boss, 1 + (1 if self.order >= 8 else 0), 9.0, 4.0, 0, max(10, int(hp0 * BOSS_K))))
                budget *= 0.6
            # every third wave is of one kind: a rush, a swarm, a wall
            if w % 3 == 2 and not last:
                kinds = 1
            pool = ros[:]
            rnd.shuffle(pool)
            # the newest kind the world brings shows up often
            newest = [a for a in ros if ALIENS[a][1] == self.order]
            if newest and w >= 1 and rnd.random() < 0.6:
                pool.remove(newest[0])
                pool.insert(0, newest[0])
            picks = pool[:kinds]
            delay = 0.0
            for k, a in enumerate(picks):
                cost = ALIENS[a][0]
                share = budget / len(picks)
                cnt = max(1, int(round(share / cost)))
                cnt = min(cnt, 50)
                gap = {'swarm': 0.30, 'runner': 0.6, 'grub': 0.75, 'wasp': 0.85}.get(a, 1.15)
                if cost >= 9:
                    gap = 2.8
                gap *= 1.0 - 0.35 * u
                path = (w + k) % len(self.paths) if len(self.paths) > 1 else 0
                if len(self.paths) > 1 and w >= 3 and cnt >= 6:
                    # every way at once
                    n = len(self.paths)
                    for q in range(n):
                        part = cnt // n + (1 if q < cnt % n else 0)
                        groups.append((a, part, gap * n * 0.8, delay + 0.3 * q, (path + q) % n, hp))
                else:
                    groups.append((a, cnt, gap, delay, path, hp))
                delay += min(7.0, cnt * gap * 0.5) + 1.0
            out.append(groups)
        return out

    def start_credits(self, sector):
        return 260 + 30 * sector + 12 * (self.order - 1)

    def def_text(self):
        L = []
        L.append('id %s' % self.id)
        L.append('order %d' % self.order)
        for k, lang in enumerate(('es', 'en', 'de')):
            L.append('name %s|%s' % (lang, self.names[k]))
            L.append('desc %s|%s' % (lang, self.descs[k]))
        L.append('accent %s' % self.accent.lstrip('#'))
        L.append('sky %s' % self.sky.lstrip('#'))
        L.append('hue %d' % self.hue)
        L.append('music %d' % self.music)
        L.append('boss %s' % self.boss)
        L.append('fort %.2f %.2f' % self.fort)
        for i, p in enumerate(self.paths):
            L.append('path %d %d %s' % (i, len(p), ' '.join('%.2f %.2f' % q for q in p)))
        for i, p in enumerate(self.air):
            L.append('air %d %d %s' % (i, len(p), ' '.join('%.2f %.2f' % q for q in p)))
        for (x, y) in self.pads:
            L.append('pad %.2f %.2f' % (x, y))
        for s in range(3):
            ws = self.waves(s)
            L.append('sector %d %d %d' % (s, len(ws), self.start_credits(s)))
            for w, groups in enumerate(ws):
                L.append('wave %d %d %d' % (s, w, len(groups)))
                for (a, cnt, gap, delay, path, hp) in groups:
                    L.append('g %s %d %.2f %.2f %d %d' % (a, cnt, gap, delay, path, hp))
        return '\n'.join(L) + '\n'


def _worlds():
    W = []
    W.append(World(
        'luna', 1,
        ('Cráter Selene', 'Selene Crater', 'Selene-Krater'),
        ('Una luna gris bajo un planeta azul. El primer puesto de avanzada.',
         'A grey moon under a blue planet. The first outpost.',
         'Ein grauer Mond unter einem blauen Planeten. Der erste Vorposten.'),
        'moon', (3.0, 9.6),
        [[(-5, 41), (-5, 31.5), (5, 31.5), (5, 23.5), (-5, 23.5), (-5, 15.5), (3, 15.5), (3, 12.0)]],
        npads=14, hue=0, music=0, boss='queen', accent='#38d8ff', ground='#6c7488', road='#a8b0c4', sky='#05060f'))
    W.append(World(
        'selva', 2,
        ('Selva de Neón', 'Neon Jungle', 'Neon-Dschungel'),
        ('Plantas que brillan en la oscuridad y dos senderos que se juntan.',
         'Plants that glow in the dark and two trails that join.',
         'Leuchtende Pflanzen im Dunkeln und zwei Pfade, die sich treffen.'),
        'jungle', (0.0, 9.6),
        [[(-6.5, 41), (-6.5, 29), (0, 25), (0, 19), (-5, 19), (-5, 13.5), (0, 13.5), (0, 12.2)],
         [(6.5, 41), (6.5, 29), (0, 25), (0, 19), (-5, 19), (-5, 13.5), (0, 13.5), (0, 12.2)]],
        npads=15, hue=-50, music=1, boss='queen', accent='#ff3bd4', ground='#123a3a', road='#2a6a5c', sky='#020a10'))
    W.append(World(
        'magma', 3,
        ('Mar de Magma', 'Magma Sea', 'Magma-Meer'),
        ('Roca negra sobre ríos de lava. Acá llega el primer jefe.',
         'Black rock over rivers of lava. The first boss comes here.',
         'Schwarzer Fels über Lavaströmen. Hier kommt der erste Boss.'),
        'lava', (5.2, 11.0),
        [[(11, 33.5), (-5.5, 33.5), (-5.5, 26.5), (5.5, 26.5), (5.5, 19.5), (-5.5, 19.5), (-5.5, 12.5), (2.5, 12.5)]],
        npads=16, hue=160, music=2, boss='titan', accent='#ff7a1a', ground='#2a1c20', road='#5a4a4c', sky='#1a0604'))
    W.append(World(
        'hielo', 4,
        ('Glaciar de Metano', 'Methane Glacier', 'Methan-Gletscher'),
        ('El fuerte en el centro y el camino que lo rodea en espiral.',
         'The fort in the middle and the road spiralling round it.',
         'Die Festung in der Mitte, der Weg windet sich um sie.'),
        'ice', (0.0, 20.5),
        [[(-2, 41), (-2, 35), (-7, 35), (-7, 10.5), (7, 10.5), (7, 30.5), (2.2, 30.5), (2.2, 25.5), (1.2, 23.4)]],
        npads=16, hue=20, music=3, boss='overmind', accent='#a8f4ff', ground='#b8d4ec', road='#7fa8d0', sky='#061224'))
    W.append(World(
        'orbita', 5,
        ('Estación Órbita', 'Orbit Station', 'Station Orbit'),
        ('Cubiertas de metal en el vacío. Atacan por los dos pasillos.',
         'Metal decks in the void. They attack down both corridors.',
         'Metalldecks im All. Sie greifen durch beide Gänge an.'),
        'station', (0.0, 9.8),
        [[(-6, 41), (-6, 27), (-2, 27), (-2, 17.5), (-6, 17.5), (-6, 10.2), (-2.9, 10.2)],
         [(6, 41), (6, 27), (2, 27), (2, 17.5), (6, 17.5), (6, 10.2), (2.9, 10.2)]],
        npads=16, hue=40, music=0, boss='titan', accent='#5a8cff', ground='#39435c', road='#6a7896', sky='#02030a'))
    W.append(World(
        'cristal', 6,
        ('Dunas de Cristal', 'Crystal Dunes', 'Kristalldünen'),
        ('Arena rosa y agujas de amatista. El camino baja en zigzag.',
         'Pink sand and amethyst spires. The road zigzags down.',
         'Rosa Sand und Amethystnadeln. Der Weg führt im Zickzack hinab.'),
        'crystal', (5.6, 10.0),
        [[(-11, 34), (-2, 34), (6, 29), (6, 24.5), (-6, 19.5), (-6, 14.5), (2.8, 10.9)]],
        npads=15, hue=70, music=1, boss='overmind', accent='#ff6fd8', ground='#d08aa8', road='#f0c8b0', sky='#1c0a28'))
    W.append(World(
        'cienaga', 7,
        ('Ciénaga Tóxica', 'Toxic Mire', 'Giftsumpf'),
        ('Charcos de ácido verde. Entran por arriba y por el costado.',
         'Pools of green acid. They come from above and from the side.',
         'Grüne Säuretümpel. Sie kommen von oben und von der Seite.'),
        'swamp', (0.0, 9.6),
        [[(3.5, 41), (3.5, 31), (-4, 31), (-4, 22.5), (4, 22.5), (4, 15), (0, 15), (0, 12.2)],
         [(-11, 26.5), (-4, 26.5), (-4, 22.5), (4, 22.5), (4, 15), (0, 15), (0, 12.2)]],
        npads=16, hue=-70, music=2, boss='queen', accent='#a8ff3c', ground='#2c3a22', road='#6a6a3c', sky='#0a1206'))
    W.append(World(
        'nebula', 8,
        ('Anillos de Nébula', 'Nebula Rings', 'Nebelringe'),
        ('Plataformas flotando sobre una nebulosa. El camino más largo.',
         'Platforms floating over a nebula. The longest road.',
         'Plattformen über einem Nebel. Der längste Weg.'),
        'nebula', (0.0, 9.8),
        [[(6, 41), (6, 34), (-6, 34), (-6, 28), (2, 28), (2, 22), (-6, 22), (-6, 16), (6, 16), (6, 10.4), (2.9, 10.4)]],
        npads=17, hue=20, music=3, boss='titan', accent='#b070ff', ground='#3a2c5c', road='#7a68b0', sky='#0c0420'))
    W.append(World(
        'colmena', 9,
        ('Colmena Xil', 'Xil Hive', 'Xil-Stock'),
        ('El nido de ellos. Salen de tres túneles a la vez.',
         'Their nest. They pour out of three tunnels at once.',
         'Ihr Nest. Sie strömen aus drei Tunneln zugleich.'),
        'hive', (0.0, 9.6),
        [[(-11, 31), (-4.5, 31), (-4.5, 25), (0, 22.5), (0, 17.5), (5, 17.5), (5, 13.2), (0, 13.2), (0, 12.2)],
         [(11, 31), (4.5, 31), (4.5, 25), (0, 22.5), (0, 17.5), (5, 17.5), (5, 13.2), (0, 13.2), (0, 12.2)],
         [(0, 41), (0, 22.5), (0, 17.5), (5, 17.5), (5, 13.2), (0, 13.2), (0, 12.2)]],
        npads=17, hue=130, music=2, boss='queen', accent='#ffd83a', ground='#4a3418', road='#8a6a2c', sky='#120a02'))
    W.append(World(
        'nucleo', 10,
        ('Núcleo Oscuro', 'Dark Core', 'Dunkler Kern'),
        ('Obsidiana y energía roja bajo un agujero negro. El último bastión.',
         'Obsidian and red energy under a black hole. The last bastion.',
         'Obsidian und rote Energie unter einem Schwarzen Loch. Die letzte Bastion.'),
        'core', (0.0, 21.0),
        [[(-7.5, 41), (-7.5, 11.5), (-2, 11.5), (-2, 16.5), (-1.2, 18.4)],
         [(7.5, 41), (7.5, 31), (2, 31), (2, 25.6), (1.2, 23.7)]],
        npads=18, hue=110, music=3, boss='overmind', accent='#ff2848', ground='#1a1420', road='#4a3848', sky='#000000'))
    return W


WORLDS = _worlds()
BY_ID = {w.id: w for w in WORLDS}


def check():
    ok = True
    for w in WORLDS:
        for (x, y) in w.pads:
            d = w.dist_path(x, y)
            if d < PATH_W / 2 + PAD_R:
                print(w.id, 'pad on the road', x, y, d)
                ok = False
        if len(w.pads) < 12:
            print(w.id, 'only', len(w.pads), 'pads')
            ok = False
        for p in w.paths:
            end = p[-1]
            d = math.hypot(end[0] - w.fort[0], end[1] - w.fort[1])
            if d > FORT_R + 0.6:
                print(w.id, 'the road ends %.1f m from the fort' % d)
                ok = False
        tot = [sum(ALIENS.get(g[0], (40,))[0] * g[1] for g in wave) for wave in w.waves(2)]
        length = sum(len(p) for p in w.paths) * STEP / len(w.paths)
        print('%-8s pads %2d  road %5.1f m  waves %d/%d/%d  threat of the last sector %s' % (
            w.id, len(w.pads), length, len(w.waves(0)), len(w.waves(1)), len(w.waves(2)),
            ' '.join('%d' % t for t in tot)))
    return ok


def preview(path):
    from PIL import Image, ImageDraw
    S = 0.5
    im = Image.new('RGB', (int(720 * S) * 5, int(1280 * S) * 2), (0, 0, 0))
    for i, w in enumerate(WORLDS):
        ox, oy = (i % 5) * int(720 * S), (i // 5) * int(1280 * S)
        d = ImageDraw.Draw(im)

        def P(x, y):
            return (ox + (360 + x * 40) * S, oy + (1280 - y * 32) * S)
        d.rectangle([ox, oy, ox + 720 * S - 1, oy + 1280 * S - 1], fill=w.ground)
        d.rectangle([ox, oy, ox + 720 * S - 1, oy + 96 * S], fill=(20, 20, 20))
        d.rectangle([ox, oy + 1090 * S, ox + 720 * S - 1, oy + 1280 * S - 1], fill=(20, 20, 20))
        for p in w.paths:
            for (x, y) in p:
                cx, cy = P(x, y)
                d.ellipse([cx - 44 * S, cy - 35 * S, cx + 44 * S, cy + 35 * S], fill=w.road)
        for p in w.air:
            d.line([P(*q) for q in p], fill=(255, 255, 255), width=1)
        for (x, y) in w.pads:
            cx, cy = P(x, y)
            d.ellipse([cx - 38 * S, cy - 30 * S, cx + 38 * S, cy + 30 * S], outline=w.accent, width=2)
        cx, cy = P(*w.fort)
        d.ellipse([cx - 108 * S, cy - 86 * S, cx + 108 * S, cy + 86 * S], outline=(255, 255, 255), width=3)
        d.text((ox + 8, oy + 8), '%d %s  %d pads' % (w.order, w.id, len(w.pads)), fill=(255, 255, 255))
    im.save(path)


if __name__ == '__main__':
    if len(sys.argv) > 2 and sys.argv[1] == 'preview':
        preview(sys.argv[2])
    elif len(sys.argv) > 1 and sys.argv[1] == 'check':
        sys.exit(0 if check() else 1)
    else:
        print(__doc__)
