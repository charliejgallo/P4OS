#!/usr/bin/env python3
"""The art sample, put together the way the game will: a board drawn from
the tile sheets of tools/blender/board.py, row by row from the back, with
the turtle of tools/blender/turtle.py walking a fixed program on it.

    python3 apps/tortuga/tools/compose.py --art <dir with the sheets> --out <dir>

The program and the map are levels/arte.tl, run through the reference model
(tools/tg_ref.py), so the turtle does exactly what the game would make it
do. Writes:

    board.png              the board alone, the turtle at the start
    shot_portrait.png      a 720 x 1280 screen: the board, the controls, the program
    shot_landscape.png     a 1280 x 720 screen
    turtle.png             the turtle's animations on a strip of grass
    sample.gif             the program running on the board

The screens are a sketch of the layout of DESIGN.md (section 6), drawn
here with Pillow; the game draws its own with LVGL.
"""

import json
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFilter, ImageFont

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import tg_ref as T  # noqa: E402

TILE_W, TILE_D = 64, 48         # a tile on the screen: across, and deep
FONT = os.path.join(HERE, '..', '..', 'bastion', 'tools', 'fonts', 'Rubik-Medium.ttf')
MONO = '/System/Library/Fonts/Menlo.ttc'

YAW = {'N': 90, 'E': 0, 'S': 270, 'W': 180}


def arg(name, default=None):
    a = sys.argv
    return a[a.index(name) + 1] if name in a else default


class Sheet:
    def __init__(self, art, name):
        self.im = Image.open(os.path.join(art, name + '.png')).convert('RGBA')
        with open(os.path.join(art, name + '.json')) as f:
            j = json.load(f)
        self.cw, self.ch = j['cell']
        self.ax, self.ay = j['anchor']
        self.cols, self.rows = j['cols'], j['rows']
        self.cache = {}

    def cell(self, col, row=0):
        k = (col, row)
        if k not in self.cache:
            x, y = col * self.cw, row * self.ch
            self.cache[k] = self.im.crop((x, y, x + self.cw, y + self.ch))
        return self.cache[k]

    def draw(self, dst, col, row, x, y):
        """the cell's anchor on (x, y) of dst"""
        dst.alpha_composite(self.cell(col, row), (int(round(x - self.ax)), int(round(y - self.ay))))


class Art:
    def __init__(self, art):
        for n in ('ground', 'rock', 'lettuce', 'key', 'door', 'goal', 'bridge', 'walk', 'bump', 'eat', 'party'):
            setattr(self, n, Sheet(art, 'tg_' + n))


# ---------------------------------------------------------------------------
# The board
# ---------------------------------------------------------------------------

class Board:
    """a map's tiles as they are now (keys taken, doors opened, lettuce
    eaten change it while the program runs)"""

    def __init__(self, rows):
        self.h = len(rows)
        self.w = max(len(r) for r in rows)
        self.t = [list(r.ljust(self.w)) for r in rows]
        self.start = None
        for y, r in enumerate(self.t):
            for x, c in enumerate(r):
                if c in T.TURTLE_CH:
                    self.start = (x, y, T.TURTLE_CH[c])
                    r[x] = '.'

    def at(self, x, y):
        return self.t[y][x] if 0 <= y < self.h and 0 <= x < self.w else ' '

    def size(self):
        """the board's picture: the tiles, the soil under the front row and
        room above the back row for what stands on it"""
        return self.w * TILE_W, self.h * TILE_D + 26 + 80

    def origin(self):
        return 0, 80                # the back left corner of tile (0, 0) in the picture


def grass_variant(x, y):
    h = (x * 73856093) ^ (y * 19349663)
    h &= 0xffff
    return 2 + (h >> 4) % 2 if h % 7 == 0 else h % 2


def draw_board(art, b, turtle=None, tick=0):
    """the board's picture; turtle = dict(x, y, yaw, sheet, frame) in tiles
    (fractions while it walks)"""
    W, H = b.size()
    img = Image.new('RGBA', (W, H), (0, 0, 0, 0))
    ox, oy = b.origin()
    water = [0, 1, 2, 3][tick // 5 % 4]
    tx_row = None
    if turtle:
        tx_row = int(math.ceil(turtle['y'] - 1e-6))
    for y in range(b.h):
        cy = oy + y * TILE_D + TILE_D / 2
        for x in range(b.w):
            c = b.at(x, y)
            if c == ' ':
                continue
            cx = ox + x * TILE_W + TILE_W / 2
            if c in '~=':
                art.ground.draw(img, 0, 4 + water, cx, cy)
            else:
                art.ground.draw(img, 0, grass_variant(x, y), cx, cy)
        for x in range(b.w):
            c = b.at(x, y)
            cx = ox + x * TILE_W + TILE_W / 2
            if c == '#':
                art.rock.draw(img, (x + y) % 2, 0, cx, cy)
            elif c == 'L':
                art.lettuce.draw(img, 0, 0, cx, cy)
            elif c == 'k':
                art.key.draw(img, tick // 4 % 4, 0, cx, cy)
            elif c in 'Pp':
                ns = b.at(x - 1, y) == '#' or b.at(x + 1, y) == '#'
                art.door.draw(img, (0 if ns else 2) + (c == 'p'), 0, cx, cy)
            elif c == 'M':
                art.goal.draw(img, tick // 3 % 4, 0, cx, cy)
            elif c == '=':
                ns = b.at(x - 1, y) == '~' or b.at(x + 1, y) == '~'
                art.bridge.draw(img, 1 if ns else 0, 0, cx, cy)
        if turtle and tx_row == y:
            px = ox + turtle['x'] * TILE_W + TILE_W / 2
            py = oy + turtle['y'] * TILE_D + TILE_D / 2
            shadow = Image.new('RGBA', (60, 30), (0, 0, 0, 0))
            ImageDraw.Draw(shadow).ellipse((6, 6, 54, 24), fill=(20, 40, 10, 90))
            shadow = shadow.filter(ImageFilter.GaussianBlur(3))
            img.alpha_composite(shadow, (int(px - 30), int(py - 13)))
            sh = getattr(art, turtle['sheet'])
            col = int(round((turtle['yaw'] % 360) / 22.5)) % 16 if sh.cols == 16 else \
                [0, 90, 180, 270].index(int(round(turtle['yaw'] / 90.0)) * 90 % 360)
            sh.draw(img, col, turtle['frame'], px, py - turtle.get('lift', 0))
    return img


# ---------------------------------------------------------------------------
# The program, run by the reference model, as a list of animation steps
# ---------------------------------------------------------------------------

class Recorder(T.World):
    """the reference world, writing down each step for the animation"""

    def __init__(self, level):
        super().__init__(level, 0)
        self.log = []

    def call(self, name, args, line):
        if name == 'avanzar':
            for _ in range(int(args[0] if args else 1)):
                x0, y0 = self.tx, self.ty
                try:
                    super().call(name, [1], line)
                finally:
                    if (self.tx, self.ty) != (x0, y0):
                        self.log.append(('move', line, (x0, y0), (self.tx, self.ty), self.dir))
            return None
        d0 = self.dir
        try:
            return super().call(name, args, line)
        finally:
            if name.startswith('girar'):
                self.log.append(('turn', line, d0, self.dir))
            elif name == 'comer':
                self.log.append(('eat', line, (self.tx, self.ty)))


def program_steps(level):
    w = Recorder(level)
    try:
        T.Logo(w).run(level.solutions['logo'])
        w.finish()
    except T.Stop as e:
        if e.kind != 'win':
            raise SystemExit('the sample program does not win: %s' % e.msg)
    w.log.append(('win', w.line))
    return w.log


def frames(level, art):
    """yield (board image, line, done) for every frame of the run"""
    b = Board(level.maps[0])
    x, y, d = b.start
    yaw = YAW[d]
    tick = 0
    for _ in range(14):
        yield draw_board(art, b, dict(x=x, y=y, yaw=yaw, sheet='walk', frame=0), tick), 0
        tick += 1
    for st in program_steps(level):
        kind, line = st[0], st[1]
        if kind == 'move':
            (x0, y0), (x1, y1) = st[2], st[3]
            c = b.at(x1, y1)
            if c == 'P':
                b.t[y1][x1] = 'p'
            n = 12
            for i in range(1, n + 1):
                f = i / n
                yield draw_board(art, b, dict(x=x0 + (x1 - x0) * f, y=y0 + (y1 - y0) * f, yaw=yaw, sheet='walk',
                                              frame=i % 6), tick), line
                tick += 1
                if c == 'k' and i == n // 2:
                    b.t[y1][x1] = '.'
            x, y = x1, y1
        elif kind == 'turn':
            y1 = YAW[st[3]]
            step = -22.5 if (yaw - y1) % 360 == 90 else 22.5
            for i in range(4):
                yaw = (yaw + step) % 360
                for _ in range(2):
                    yield draw_board(art, b, dict(x=x, y=y, yaw=yaw, sheet='walk', frame=0), tick), line
                    tick += 1
        elif kind == 'eat':
            for k in range(4):
                if k == 1:
                    b.t[y][x] = '.'
                for _ in range(4):
                    yield draw_board(art, b, dict(x=x, y=y, yaw=yaw, sheet='eat', frame=k), tick), line
                    tick += 1
        elif kind == 'win':
            for rep in range(2):
                for k in range(6):
                    for _ in range(3):
                        yield draw_board(art, b, dict(x=x, y=y, yaw=270, sheet='party', frame=k), tick), -1
                        tick += 1
            for _ in range(16):
                yield draw_board(art, b, dict(x=x, y=y, yaw=270, sheet='party', frame=5), tick), -1
                tick += 1


# ---------------------------------------------------------------------------
# The screens (a sketch of section 6 of DESIGN.md)
# ---------------------------------------------------------------------------

BG = (16, 19, 24)
CARD = (30, 35, 42)
CARD2 = (40, 47, 56)
TEXT = (238, 240, 243)
DIM = (140, 149, 160)
ACCENT = (255, 159, 67)
SKY0, SKY1 = (255, 238, 205), (190, 228, 246)


def font(size, mono=False):
    return ImageFont.truetype(MONO if mono else FONT, size)


def sky(w, h):
    im = Image.new('RGBA', (w, h))
    px = im.load()
    for yy in range(h):
        t = yy / max(1, h - 1)
        c = tuple(int(SKY1[i] + (SKY0[i] - SKY1[i]) * t) for i in range(3)) + (255,)
        for xx in range(w):
            px[xx, yy] = c
    return im


def rrect(d, box, r, fill):
    d.rounded_rectangle(box, r, fill=fill)


def play_icon(d, x, y, s, fill):
    d.polygon([(x, y - s), (x, y + s), (x + 1.7 * s, y)], fill=fill)


def step_icon(d, x, y, s, fill):
    d.polygon([(x, y - s), (x, y + s), (x + 1.5 * s, y)], fill=fill)
    d.rectangle((x + 1.6 * s, y - s, x + 2.1 * s, y + s), fill=fill)


def stop_icon(d, x, y, s, fill):
    d.rectangle((x, y - s, x + 2 * s, y + s), fill=fill)


def stars(d, x, y, n, of=3, r=13):
    for i in range(of):
        cx = x + i * (2 * r + 6)
        pts = []
        for k in range(10):
            a = -math.pi / 2 + k * math.pi / 5
            rr = r if k % 2 == 0 else r * 0.45
            pts.append((cx + rr * math.cos(a), y + rr * math.sin(a)))
        d.polygon(pts, fill=(255, 205, 64) if i < n else (70, 78, 90))


PROGRAM_LINES = None


def code_panel(d, box, lines, cur, size=26):
    x0, y0, x1, y1 = box
    rrect(d, box, 18, CARD)
    f, fn = font(size, True), font(size - 4, True)
    lh = int(size * 1.45)
    for i, ln in enumerate(lines):
        yy = y0 + 16 + i * lh
        if yy + lh > y1 - 8:
            break
        if i + 1 == cur:
            rrect(d, (x0 + 8, yy - 4, x1 - 8, yy + lh - 6), 8, (70, 52, 30))
            d.rectangle((x0 + 8, yy - 4, x0 + 13, yy + lh - 6), fill=ACCENT)
        d.text((x0 + 24, yy + 2), '%2d' % (i + 1), font=fn, fill=DIM)
        words = ln.split(' ')
        xx = x0 + 78
        for wd in words:
            col = TEXT
            if wd in ('avanzar', 'comer'):
                col = (126, 214, 120)
            elif wd in ('girar', 'derecha', 'izquierda'):
                col = (120, 190, 255)
            elif wd.isdigit():
                col = (255, 196, 110)
            d.text((xx, yy), wd, font=f, fill=col)
            xx += d.textlength(wd + ' ', font=f)


def palette(d, x, y, w, words, size=24):
    f = font(size)
    xx, yy = x, y
    for wd in words:
        tw = d.textlength(wd, font=f) + 32
        if xx + tw > x + w:
            xx, yy = x, yy + size + 30
        rrect(d, (xx, yy, xx + tw, yy + size + 18), 14, CARD2)
        d.text((xx + 16, yy + 8), wd, font=f, fill=TEXT)
        xx += tw + 12
    return yy + size + 18


def controls(d, x, y, w, running=True):
    h = 64
    bw = (w - 24) // 3
    labels = [('Correr', play_icon), ('Paso', step_icon), ('Parar', stop_icon)]
    f = font(24)
    for i, (lab, icon) in enumerate(labels):
        bx = x + i * (bw + 12)
        rrect(d, (bx, y, bx + bw, y + h), 16, ACCENT if i == 0 else CARD2)
        fill = (30, 22, 10) if i == 0 else TEXT
        icon(d, bx + 26, y + h / 2, 10, fill)
        d.text((bx + 60, y + h / 2 - 14), lab, font=f, fill=fill)
    return y + h


def speed(d, x, y, w):
    f = font(20)
    d.text((x, y), 'Lento', font=f, fill=DIM)
    tw = d.textlength('Rápido', font=f)
    d.text((x + w - tw, y), 'Rápido', font=f, fill=DIM)
    lx0, lx1 = x + 80, x + w - tw - 20
    d.rounded_rectangle((lx0, y + 10, lx1, y + 16), 3, fill=CARD2)
    k = lx0 + (lx1 - lx0) * 0.45
    d.rounded_rectangle((lx0, y + 10, k, y + 16), 3, fill=ACCENT)
    d.ellipse((k - 11, y + 2, k + 11, y + 24), fill=TEXT)


def status_bar(d, w):
    d.rectangle((0, 0, w, 48), fill=(0, 0, 0))
    d.text((24, 11), '10:24', font=font(24), fill=TEXT)


def header(d, x, y, w, title):
    f = font(30)
    d.polygon([(x + 14, y + 22), (x + 28, y + 8), (x + 28, y + 36)], fill=ACCENT)
    d.text((x + 50, y + 4), title, font=f, fill=TEXT)
    stars(d, x + w - 104, y + 22, 2)


def board_card(img, board_img, box):
    x0, y0, x1, y1 = box
    w, h = x1 - x0, y1 - y0
    card = sky(w, h)
    m = Image.new('L', (w, h), 0)
    ImageDraw.Draw(m).rounded_rectangle((0, 0, w - 1, h - 1), 22, fill=255)
    bx = (w - board_img.width) // 2
    by = (h - board_img.height) // 2
    card.alpha_composite(board_img, (bx, by))
    img.paste(card, (x0, y0), m)


def shot_portrait(board_img, lines, cur, title):
    W, H = 720, 1280
    img = Image.new('RGBA', (W, H), BG + (255,))
    d = ImageDraw.Draw(img)
    status_bar(d, W)
    header(d, 16, 64, W - 32, title)
    board_card(img, board_img, (16, 120, W - 16, 120 + 520))
    y = controls(d, 16, 656, W - 32)
    speed(d, 24, y + 18, W - 48)
    code_panel(d, (16, y + 62, W - 16, y + 62 + 412), lines, cur)
    palette(d, 24, y + 62 + 428, W - 48, ['avanzar', 'girar derecha', 'girar izquierda', 'comer'])
    return img


def shot_landscape(board_img, lines, cur, title):
    W, H = 1280, 720
    img = Image.new('RGBA', (W, H), BG + (255,))
    d = ImageDraw.Draw(img)
    d.rectangle((0, 0, W, 40), fill=(0, 0, 0))
    d.text((24, 8), '10:24', font=font(22), fill=TEXT)
    board_card(img, board_img, (16, 56, 16 + 680, H - 16))
    rx = 16 + 680 + 16
    rw = W - rx - 16
    header(d, rx, 56, rw, title)
    y = controls(d, rx, 112, rw)
    speed(d, rx + 8, y + 14, rw - 16)
    code_panel(d, (rx, y + 52, rx + rw, y + 52 + 340), lines, cur, size=22)
    palette(d, rx + 8, y + 52 + 352, rw - 16, ['avanzar', 'girar derecha', 'girar izquierda', 'comer'], size=22)
    return img


def turtle_strip(art):
    """the turtle's animations, each in a row over a strip of grass"""
    rows = [('caminar (una vuelta entera de a 22,5°)', 'walk', [(c, 0) for c in range(16)]),
            ('caminar (los 6 cuadros, mirando a la derecha)', 'walk', [(0, r) for r in range(6)]),
            ('chocar', 'bump', [(c, r) for c in (0, 3) for r in range(4)]),
            ('comer', 'eat', [(c, r) for c in (0, 3) for r in range(4)]),
            ('festejar', 'party', [(c, r) for c in (3, 0) for r in range(6)])]
    W = 16 * 64 + 40
    H = len(rows) * 150 + 20
    img = Image.new('RGBA', (W, H), BG + (255,))
    d = ImageDraw.Draw(img)
    for i, (lab, sheet, cells) in enumerate(rows):
        y0 = 20 + i * 150
        d.text((20, y0), lab, font=font(22), fill=TEXT)
        for k in range(16):
            art.ground.draw(img, 0, grass_variant(k, i), 20 + k * 64 + 32, y0 + 92)
        sh = getattr(art, sheet)
        step = (16 * 64) / len(cells)
        for j, (c, r) in enumerate(cells):
            sh.draw(img, c, r, 20 + step * j + step / 2, y0 + 96)
    return img


def main():
    art_dir = arg('--art')
    out = arg('--out')
    if not art_dir or not out:
        print(__doc__)
        sys.exit(2)
    os.makedirs(out, exist_ok=True)
    art = Art(art_dir)
    level = T.parse_levels(os.path.join(HERE, '..', 'levels', 'arte.tl'))[0]
    lines = level.solutions['logo'].strip('\n').split('\n')
    title = 'Muestra · La tortuga'
    run = list(frames(level, art))
    run[0][0].save(os.path.join(out, 'board.png'))
    # the screens: in the middle of the program (crossing the bridge) and at the end
    mid = next(i for i, (_, ln) in enumerate(run) if ln == 4) + 6
    img, ln = run[mid]
    shot_portrait(img, lines, ln, title).convert('RGB').save(os.path.join(out, 'shot_portrait.png'))
    img, ln = run[-30]
    shot_landscape(img, lines, ln, title).convert('RGB').save(os.path.join(out, 'shot_landscape.png'))
    turtle_strip(art).convert('RGB').save(os.path.join(out, 'turtle.png'))
    # the run as a gif: every other frame, on the sky
    gif = []
    for img, _ in run[::2]:
        bg = sky(img.width + 32, img.height + 32)
        bg.alpha_composite(img, (16, 16))
        gif.append(bg.convert('RGB').quantize(colors=200, method=Image.MEDIANCUT))
    gif[0].save(os.path.join(out, 'sample.gif'), save_all=True, append_images=gif[1:], duration=66, loop=0,
                optimize=True)
    print('wrote', out, len(run), 'frames')


if __name__ == '__main__':
    main()
