#!/usr/bin/env python3
"""A trial router for the auxiliary board: it shows where the placement is hard to route.

    <KiCad's python3> hw/aux-board/autoroute.py [th|smd] [passes]

It reads the board place.py wrote (aux-board-<variant>.kicad_pcb), routes every net but ground,
which the two planes carry, and writes trial-<variant>.kicad_pcb beside it, with KiCad's design
rule check on the result. The trial board is not kept in git: it is there to be opened in KiCad
and looked at, and to be made again when a part moves.

How it routes. The board is a grid of G mm on its two faces. Each node knows which net may pass:
a pad, a hole, a track or the board's edge keeps every other net at its clearance. A net's pads are
joined one at a time by the cheapest path (A*), on either face, changing face through one of its
own through-hole pads for free or through a via at a price. A change of heading has a price too,
a square corner more than a 45 degree one and a hairpin most of all, so a track runs straight
where it can; and it leaves a pad by its middle when the grid has a node there. A net that finds
no way looks again as if the tracks laid before it were not there, at a price for each one
crossed: the nets it crosses are taken off, it is routed, and they are routed again after it.

The nets are not routed one after another, each around the ones before: that makes the last ones
go the long way round. They are all routed as if alone, crossing each other at a price, and then
the ones that cross are routed again and again while the price of a node others hold goes up, and
the nodes that were fought over stay dearer for everyone. Each net ends up giving way where it
costs it least (negotiated congestion, as FPGA routers do it). What still crosses when the rounds
run out is routed the old way, one net after another, taking off whoever is in the way.

Ground is the planes' business. It is routed last and only where KiCad, with the planes filled,
says a ground pad was left on an island.

It is still a plain maze router: what it cannot route a person may.
"""
import heapq
import json
import math
import os
import shutil
import subprocess
import sys
import tempfile
import time

import pcbnew

HERE = os.path.dirname(os.path.abspath(__file__))
KICAD_CLI = os.environ.get("KICAD_CLI", "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli")

G = 0.3175                      # the grid: an eighth of the headers' 2.54 mm
CLR = 0.2                       # copper to copper, KiCad's default rule
EDGE = 0.5                      # copper to the board's edge
SIG, PWR, FAT = 0.25, 0.5, 0.8  # track widths: signals; supplies and ground where FAT finds no room; supplies
VIA, VIA_DRILL = 0.6, 0.3
POWER = ("+5V", "+5V_SW", "+3V3", "VBUS_HS", "VBUS_FS")
PLANES = ("GND",)               # routed last: the planes join most of them, tracks make sure of the rest
# What a via costs, in grid steps (at 12, 3.8 mm of detour), and how much more a step on the back,
# which is the ground's. The router is tried with each pair until one joins everything: which one
# does depends on the placement, and a run takes three minutes.
COSTS = ((12, 1.15), (30, 1.5), (20, 1.0), (12, 1.5), (8, 1.0), (16, 1.3))
VIA_COST, BACK = COSTS[0]
CROSS = 25                      # looking for whom to take off: the price of a node another net's track holds
RIPS = 5                        # how many times a net may have others taken off for it
FREE, CLASH = -1, -2
# The eight headings, in order round the compass, and what a change of heading costs, in grid
# steps, by how many eighths of a turn it is: nothing straight on, little for 45 degrees, a square
# corner dearer, a hairpin (which leaves an acute angle in the copper) dearest. Never straight back.
HEADINGS = ((1, 0), (1, 1), (0, 1), (-1, 1), (-1, 0), (-1, -1), (0, -1), (1, -1))
TURN = (0.0, 0.6, 2.5, 9.0)
ACROSS = 1.1                    # a step across a face's own direction: the front runs along x, the back along y
OFF = 2.0                       # leaving a pad one grid step off its middle
# The negotiation: what crossing a node another net holds costs in the first round, in grid steps,
# by how much that grows each round, what a node gains for good each round it is fought over, and
# how many rounds before the nets still crossing are routed one after another.
PRICE, GROWTH, MEMORY, ROUNDS = 1.0, 1.5, 0.5, 40
SLACK = 10.0                    # a way round is told of when it is this many mm over the net's direct way

class Map:
    """Who may pass each node of each face, for a track whose half width is 'half'."""

    def __init__(self, nx, ny, half):
        self.nx, self.ny, self.half = nx, ny, half
        self.owner = [[FREE] * (nx * ny), [FREE] * (nx * ny)]
        self.bare = None                # the owners before any routing, once frozen
        self.laid = [{}, {}]            # since then: node -> the nets whose routed copper keeps it
        self.nodes = {}                 # and net -> the (face, node) it keeps

    def copy(self):
        m = Map(self.nx, self.ny, self.half)
        m.owner = [list(self.owner[0]), list(self.owner[1])]
        return m

    def freeze(self):
        """From here on a claim is remembered with who made it, so a net's tracks can be taken off."""
        self.bare = [list(self.owner[0]), list(self.owner[1])]

    def claim(self, face, i, j, net):
        if 0 <= i < self.nx and 0 <= j < self.ny:
            own = self.owner[face]
            k = j * self.nx + i
            if own[k] == FREE:
                own[k] = net
            elif own[k] != net:
                own[k] = CLASH
            if self.bare is not None:
                self.laid[face].setdefault(k, []).append(net)
                self.nodes.setdefault(net, []).append((face, k))

    def release(self, net):
        """Takes a net's routed copper off: each node goes back to whoever else keeps it."""
        for face, k in self.nodes.pop(net, ()):
            laid = self.laid[face]
            who = laid.get(k)
            if not who or net not in who:
                continue
            who = [n for n in who if n != net]
            o = self.bare[face][k]
            for n in who:
                o = n if o == FREE else (o if o == n else CLASH)
            if who:
                laid[k] = who
            else:
                del laid[k]
            self.owner[face][k] = o

    def keep(self, faces, net, reach, dist, box):
        """Claims for 'net' every node nearer than 'reach' to a shape: dist(x, y) is the distance to
        it, box its bounds."""
        x0, y0, x1, y1 = box
        for j in range(int(math.floor((y0 - reach) / G)), int(math.ceil((y1 + reach) / G)) + 1):
            for i in range(int(math.floor((x0 - reach) / G)), int(math.ceil((x1 + reach) / G)) + 1):
                if dist(i * G, j * G) < reach:
                    for f in faces:
                        self.claim(f, i, j, net)


def pad_dist(p):
    x, y, hx, hy, round_ = p["x"], p["y"], p["hx"], p["hy"], p["round"]
    if round_:
        return lambda px, py: math.hypot(px - x, py - y) - hx

    def d(px, py):
        qx, qy = abs(px - x) - hx, abs(py - y) - hy
        return math.hypot(max(qx, 0), max(qy, 0)) + min(max(qx, qy), 0)
    return d


def seg_dist(ax, ay, bx, by):
    dx, dy = bx - ax, by - ay
    L2 = dx * dx + dy * dy

    def d(px, py):
        t = 0 if L2 == 0 else max(0, min(1, ((px - ax) * dx + (py - ay) * dy) / L2))
        return math.hypot(px - ax - t * dx, py - ay - t * dy)
    return d


def read(board):
    """The board as the router sees it: pads, the tracks already there, its size."""
    mm = pcbnew.ToMM
    box = board.GetBoardEdgesBoundingBox()
    ox, oy = mm(box.GetLeft()), mm(box.GetTop())
    W, H = mm(box.GetWidth()), mm(box.GetHeight())
    # The grid starts on the 2x20 header's pin 1, not on the board's corner: only then is there a
    # node in the middle of the 0.84 mm between two of its pins, the one way out of its outer row.
    j1 = board.FindFootprintByReference("J1")
    if j1:
        pin1 = next(p for p in j1.Pads() if p.GetNumber() == "1").GetPosition()
        sx, sy = (mm(pin1.x) - ox) % G, (mm(pin1.y) - oy) % G
        ox, oy = ox + sx, oy + sy
    else:
        sx = sy = 0.0
    # KiCad saves the footprints in the order of their identifiers, which are new every time place.py
    # runs: taken by reference and pin, the same board gives the same routing
    every = sorted(((fp.GetReference(), pad.GetNumber(), pad.GetNetname(), fp, pad)
                    for fp in board.GetFootprints() for pad in fp.Pads()), key=lambda e: e[:3])
    ids = {name: k for k, name in enumerate(sorted({e[2] for e in every if e[2]}))}
    pads = []
    for _, _, _, fp, pad in every:
        bb = pad.GetBoundingBox()
        hx, hy = mm(bb.GetWidth()) / 2, mm(bb.GetHeight()) / 2
        drill = max(mm(pad.GetDrillSize().x), mm(pad.GetDrillSize().y)) / 2
        hx, hy = max(hx, drill), max(hy, drill)
        faces = [f for f, layer in ((0, pcbnew.F_Cu), (1, pcbnew.B_Cu)) if pad.IsOnLayer(layer)]
        if drill > 0:
            faces = [0, 1]          # a hole is in the way on both faces, plated or not
        name = pad.GetNetname()
        net = ids.setdefault(name, len(ids)) if name else 10000 + len(pads)
        shape = pad.GetShape(pcbnew.F_Cu if 0 in faces else pcbnew.B_Cu)
        pads.append({"ref": fp.GetReference(), "pin": pad.GetNumber(), "name": name, "net": net,
                     "x": mm(bb.GetCenter().x) - ox, "y": mm(bb.GetCenter().y) - oy, "hx": hx, "hy": hy,
                     "round": shape in (pcbnew.PAD_SHAPE_CIRCLE, pcbnew.PAD_SHAPE_OVAL) and abs(hx - hy) < 0.01,
                     "faces": faces, "through": drill > 0 and bool(name)})
    tracks = []
    for t in board.GetTracks():
        if t.GetClass() == "PCB_TRACK":
            tracks.append({"net": ids.setdefault(t.GetNetname(), len(ids)), "face": 0 if t.GetLayer() == pcbnew.F_Cu else 1,
                           "a": (mm(t.GetStart().x) - ox, mm(t.GetStart().y) - oy),
                           "b": (mm(t.GetEnd().x) - ox, mm(t.GetEnd().y) - oy), "w": mm(t.GetWidth()), "track": t})
    # the areas where the board says no track may go, and whether no via either
    areas = []
    for z in board.Zones():
        if z.GetIsRuleArea() and z.GetDoNotAllowTracks():
            zb = z.GetBoundingBox()
            areas.append({"faces": [f for f, layer in ((0, pcbnew.F_Cu), (1, pcbnew.B_Cu)) if z.IsOnLayer(layer)],
                          "box": (mm(zb.GetLeft()) - ox, mm(zb.GetTop()) - oy, mm(zb.GetRight()) - ox, mm(zb.GetBottom()) - oy),
                          "vias": z.GetDoNotAllowVias()})
    # x and y run from -sx and -sy: the strip before the first node is inside the edge's clearance
    return {"W": W - sx, "H": H - sy, "x0": -sx, "y0": -sy, "ox": ox, "oy": oy, "ids": ids, "pads": pads,
            "tracks": tracks, "areas": areas}


def base_maps(b):
    """The two maps before any routing: one for signal tracks, one for power tracks and vias."""
    nx, ny = int(b["W"] / G) + 1, int(b["H"] / G) + 1
    maps = {"sig": Map(nx, ny, SIG / 2), "wide": Map(nx, ny, max(PWR, VIA) / 2), "fat": Map(nx, ny, FAT / 2)}
    for m in maps.values():
        rim = EDGE + m.half + 0.1       # the edge's own line is 0.1 mm wide, and KiCad measures from its middle
        for j in range(ny):
            for i in range(nx):
                x, y = i * G, j * G
                if x < b["x0"] + rim or y < b["y0"] + rim or x > b["W"] - rim or y > b["H"] - rim:
                    for f in (0, 1):
                        m.owner[f][j * nx + i] = CLASH
        for p in b["pads"]:
            # 0.09 over the rule: a track may cut the corner between two nodes that are both clear
            m.keep(p["faces"], p["net"], CLR + m.half + 0.09, pad_dist(p),
                   (p["x"] - p["hx"], p["y"] - p["hy"], p["x"] + p["hx"], p["y"] + p["hy"]))
        for t in b["tracks"]:
            lay_track(maps, t["net"], t["face"], t["a"], t["b"], t["w"], only=m)
        for a in b["areas"]:
            x0, y0, x1, y1 = a["box"]
            r = m.half + 0.1            # the track's edge stays out of the area, not only its middle
            for j in range(max(0, int((y0 - r) / G)), min(ny, int((y1 + r) / G) + 2)):
                for i in range(max(0, int((x0 - r) / G)), min(nx, int((x1 + r) / G) + 2)):
                    if x0 - r <= i * G <= x1 + r and y0 - r <= j * G <= y1 + r:
                        for f in a["faces"]:
                            m.owner[f][j * nx + i] = CLASH
    novia = bytearray(nx * ny)          # 1 where a via may not go
    for a in b["areas"]:
        if a["vias"]:
            x0, y0, x1, y1 = a["box"]
            for j in range(max(0, int((y0 - VIA / 2) / G)), min(ny, int((y1 + VIA / 2) / G) + 2)):
                for i in range(max(0, int((x0 - VIA / 2) / G)), min(nx, int((x1 + VIA / 2) / G) + 2)):
                    novia[j * nx + i] = 1
    b["novia"] = novia
    return maps


def lay_track(maps, net, face, a, b, w, only=None):
    for m in ([only] if only else maps.values()):
        m.keep([face], net, w / 2 + CLR + m.half + 0.03, seg_dist(a[0], a[1], b[0], b[1]),
               (min(a[0], b[0]), min(a[1], b[1]), max(a[0], b[0]), max(a[1], b[1])))


def lay_via(maps, net, x, y):
    for m in maps.values():
        m.keep([0, 1], net, VIA / 2 + CLR + m.half + 0.03, lambda px, py: math.hypot(px - x, py - y),
               (x, y, x, y))


def groups(b, nx):
    """Each net's pads, in the groups the tracks already on the board join, and for each group the
    nodes of those tracks, where a new track may start as well: {net: [([pad index], [(face, node)])]}."""
    parent = list(range(len(b["pads"])))

    def find(a):
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    link, ends = {}, {}                 # the tracks' ends, joined into chains; a chain -> a pad on it

    def key(pt, net):
        return (net, round(pt[0], 2), round(pt[1], 2))

    def root(k):
        while link.get(k, k) != k:
            k = link[k]
        return k
    for t in b["tracks"]:
        ka, kb = root(key(t["a"], t["net"])), root(key(t["b"], t["net"]))
        link.setdefault(ka, ka)
        link.setdefault(kb, kb)
        if ka != kb:
            link[ka] = kb
    for n, p in enumerate(b["pads"]):
        d = pad_dist(p)
        for k in list(link):
            if k[0] == p["net"] and d(k[1], k[2]) <= 0.01:
                r = root(k)
                if r in ends:
                    parent[find(n)] = find(ends[r])
                ends[r] = n
    extra = {}
    for t in b["tracks"]:
        r = root(key(t["a"], t["net"]))
        if r not in ends:
            continue                    # a track that hangs from no pad
        (ax, ay), (bx, by) = t["a"], t["b"]
        steps = max(1, int(round(math.hypot(bx - ax, by - ay) / (G / 2))))
        for q in range(steps + 1):
            x, y = ax + (bx - ax) * q / steps, ay + (by - ay) * q / steps
            i, j = int(round(x / G)), int(round(y / G))
            if abs(i * G - x) < 0.02 and abs(j * G - y) < 0.02:
                extra.setdefault(find(ends[r]), set()).add((t["face"], j * nx + i))
    out = {}
    for n, p in enumerate(b["pads"]):
        if p["name"]:
            out.setdefault(p["net"], {}).setdefault(find(n), []).append(n)
    return {net: [(pads, sorted(extra.get(r, ()))) for r, pads in g.items()] for net, g in out.items()}


def pad_nodes(b, m, n):
    """The states (face, node) where a track of this map may start on a pad: inside its copper."""
    p = b["pads"][n]
    d = pad_dist(p)
    found = []
    for j in range(int((p["y"] - p["hy"]) / G), int((p["y"] + p["hy"]) / G) + 2):
        for i in range(int((p["x"] - p["hx"]) / G), int((p["x"] + p["hx"]) / G) + 2):
            if 0 <= i < m.nx and 0 <= j < m.ny and d(i * G, j * G) <= -0.05:
                for f in p["faces"]:
                    if m.owner[f][j * m.nx + i] in (FREE, p["net"]):
                        found.append((f, j * m.nx + i))
    return found


def held(m, face, k, net, cross):
    """How many other nets' routed copper keeps a track of this map off a node: 0 when it is free,
    -1 when it may not pass at all (a pad, a hole, the edge, or any other net when not crossing)."""
    o = m.owner[face][k]
    if o == FREE or o == net:
        return 0
    if cross is None or m.bare[face][k] not in (FREE, net):
        return -1
    who = m.laid[face].get(k, ())
    return 1 if len(who) == 1 else len({n for n in who if n != net})


def search(m, wide, net, sources, targets, through, novia, cross=None):
    """A*: the cheapest way from any of 'sources' to any of 'targets', as a list of (face, node).
    Both are {(face, node): what starting or ending there costs}. A state is a node with the heading
    it was reached by, so a turn can be priced. With 'cross', (price, memory), other nets' routed
    tracks may be crossed at that price a net, and memory, when given, is what each node costs more
    for having been fought over."""
    nx, ny = m.nx, m.ny
    N = nx * ny
    ti = [k % nx for _, k in targets]
    tj = [k // nx for _, k in targets]
    i0, i1, j0, j1 = min(ti), max(ti), min(tj), max(tj)
    goal = {f * N + k: c for (f, k), c in targets.items()}
    # a face prefers one direction, so tracks on the two faces cross instead of blocking each other
    moves = ([(di, dj, 1.5 if di and dj else (1.0 if di else ACROSS), a) for a, (di, dj) in enumerate(HEADINGS)],
             [(di, dj, BACK * (1.5 if di and dj else (1.0 if dj else ACROSS)), a) for a, (di, dj) in enumerate(HEADINGS)])
    END = -1
    price, memory = cross if cross is not None else (0.0, None)
    best, parent, heap = {}, {}, []
    for (f, k), c in sources.items():
        s = (f * N + k) * 9 + 8         # heading 8: none yet
        i, j = k % nx, k // nx
        dx = i0 - i if i < i0 else (i - i1 if i > i1 else 0)
        dy = j0 - j if j < j0 else (j - j1 if j > j1 else 0)
        best[s] = c
        heap.append((c + max(dx, dy) + 0.5 * min(dx, dy), -c, s))
    heapq.heapify(heap)
    while heap:
        _, g, s = heapq.heappop(heap)       # of two states as promising, the one further along first
        g = -g
        if s == END:
            path = []
            s = parent[END]
            while True:
                path.append(divmod(s // 9, N))
                if s not in parent:
                    break
                s = parent[s]
            return path[::-1]
        if g > best[s]:
            continue
        fk, a = divmod(s, 9)
        if fk in goal and g + goal[fk] < best.get(END, 1e18):
            best[END] = g + goal[fk]
            parent[END] = s
            heapq.heappush(heap, (g + goal[fk], -(g + goal[fk]), END))
        f, k = divmod(fk, N)
        j, i = divmod(k, nx)
        own = m.owner[f]
        bare = m.bare[f] if cross is not None else None
        laid = m.laid[f]
        mem = memory[f] if memory is not None else None
        for di, dj, cost, na in moves[f]:
            if a != 8:
                d = na - a if na > a else a - na
                if d > 4:
                    d = 8 - d
                if d == 4:
                    continue
                cost += TURN[d]
            ni, nj = i + di, j + dj
            if not (0 <= ni < nx and 0 <= nj < ny):
                continue
            nk = nj * nx + ni
            o = own[nk]
            if o != FREE and o != net:
                if bare is None or (bare[nk] != FREE and bare[nk] != net):
                    continue
                who = laid.get(nk, ())
                cost += price * (1 if len(who) == 1 else len({n for n in who if n != net}))
            if mem is not None:
                cost += mem[nk]
            ns, ng = (f * N + nk) * 9 + na, g + cost
            if ng < best.get(ns, 1e18):
                best[ns] = ng
                parent[ns] = s
                dx = i0 - ni if ni < i0 else (ni - i1 if ni > i1 else 0)
                dy = j0 - nj if nj < j0 else (nj - j1 if nj > j1 else 0)
                heapq.heappush(heap, (ng + max(dx, dy) + 0.5 * min(dx, dy), -ng, ns))
        # to the other face: through one of the net's own holes, or a via where one fits
        n = held(m, 1 - f, k, net, cross)
        if n >= 0:
            if k in through:
                cost = 0.05 + price * n
            elif not novia[k]:
                v0, v1 = held(wide, 0, k, net, cross), held(wide, 1, k, net, cross)
                if v0 < 0 or v1 < 0:
                    continue
                cost = VIA_COST + price * (n + v0 + v1)
            else:
                continue
            ns, ng = ((1 - f) * N + k) * 9 + 8, g + cost
            if ng < best.get(ns, 1e18):
                best[ns] = ng
                parent[ns] = s
                dx = i0 - i if i < i0 else (i - i1 if i > i1 else 0)
                dy = j0 - j if j < j0 else (j - j1 if j > j1 else 0)
                heapq.heappush(heap, (ng + max(dx, dy) + 0.5 * min(dx, dy), -ng, ns))
    return None


def shut_in(m, wide, net, sources, targets, through, novia, cross=None, cap=8000):
    """True when the targets are in a pocket that no track of this map leaves and no source is in:
    a flood from them that ends early. The search would find the same by trying every state there
    is, from the other side."""
    nx, ny = m.nx, m.ny
    N = nx * ny
    owner, wowner = (m.owner, wide.owner) if cross is None else (m.bare, wide.bare)
    seen = {f * N + k for f, k in targets}
    if any(f * N + k in seen for f, k in sources):
        return False
    stack = list(seen)
    while stack:
        if len(seen) > cap:
            return False
        f, k = divmod(stack.pop(), N)
        j, i = divmod(k, nx)
        own = owner[f]
        for di, dj in HEADINGS:
            ni, nj = i + di, j + dj
            if 0 <= ni < nx and 0 <= nj < ny:
                nk = nj * nx + ni
                if own[nk] == FREE or own[nk] == net:
                    ns = f * N + nk
                    if ns not in seen:
                        if (f, nk) in sources:
                            return False
                        seen.add(ns)
                        stack.append(ns)
        o = owner[1 - f][k]
        if (o == FREE or o == net) and (k in through or (not novia[k] and all(wowner[q][k] in (FREE, net) for q in (0, 1)))):
            ns = (1 - f) * N + k
            if ns not in seen:
                if (1 - f, k) in sources:
                    return False
                seen.add(ns)
                stack.append(ns)
    return True


def find(m, wide, net, sources, targets, through, novia, cross=None):
    if not sources or not targets or shut_in(m, wide, net, sources, targets, through, novia, cross):
        return None
    return search(m, wide, net, sources, targets, through, novia, cross)


def route_net(b, maps, net, grouped, name, out, victims_too=False, cross=None):
    """Joins a net's groups of pads; what it could not join is in out["failed"]. With 'victims_too'
    it returns the nets whose tracks stand in the way of those; with 'cross' it crosses other
    nets' tracks at that price (search)."""
    wide, sig = maps["wide"], maps["sig"]
    fat = name in POWER or name in PLANES
    nx = sig.nx
    hung = [extra for _, extra in grouped]      # the nodes of the tracks each group already has
    grouped = [pads for pads, _ in grouped]
    nodes = [[(n, s) for n in g for s in pad_nodes(b, sig, n)] for g in grouped]
    through = {k for g in grouped for n in g if b["pads"][n]["through"] for _, k in pad_nodes(b, sig, n)}
    of = {}                             # state -> the pad it is on
    for g in nodes:
        for n, s in g:
            of.setdefault(s, n)
    for q, extra in enumerate(hung):
        nodes[q] += [(None, s) for s in extra if sig.owner[s[0]][s[1]] in (FREE, net)]

    def roomy(states, m):               # the states where that map's track fits too
        own = m.owner if cross is None else m.bare
        return {s: c for s, c in states.items() if own[s[0]][s[1]] in (FREE, net)}

    def nodes_of(q):                    # a group's nodes, a pad's dearer the further from its middle
        e = {}
        for n, s in nodes[q]:
            c = 0.0
            if n is not None:
                p = b["pads"][n]
                c = OFF * math.hypot(p["x"] - (s[1] % nx) * G, p["y"] - (s[1] // nx) * G) / G
            e[s] = min(c, e.get(s, 1e9))
        return e
    order = sorted(range(len(grouped)), key=lambda q: -len(grouped[q]))
    tree = nodes_of(order[0])
    left, victims = order[1:], set()
    while left:
        # the nearest group first, as the crow flies from the pads already joined
        done = [b["pads"][of[s]] for s in tree if s in of] or [b["pads"][grouped[order[0]][0]]]
        q = min(left, key=lambda q: min(math.hypot(b["pads"][n]["x"] - p["x"], b["pads"][n]["y"] - p["y"])
                                        for n in grouped[q] for p in done[:40]))
        left.remove(q)
        targets = nodes_of(q)
        path, width = None, SIG
        for w, m in ((FAT, maps["fat"]), (PWR, wide)) if fat else ():
            if not path:
                path, width = find(m, wide, net, roomy(tree, m), roomy(targets, m), through, b["novia"], cross), w
        if not path:
            path, width = find(sig, wide, net, tree, targets, through, b["novia"], cross), SIG
        if path and fat:                # how wide each join of a supply came out, and at which pads
            ends = sorted({"%s.%s" % (b["pads"][of[e]]["ref"], b["pads"][of[e]]["pin"])
                           for e in (path[0], path[-1]) if of.get(e) is not None})
            out["thin"].append((name, width, ends))
        if not path:
            a = b["pads"][grouped[q][0]]
            if name in PLANES:          # no track reaches it: the plane may, and KiCad's check says
                out["plane"].append("%s.%s" % (a["ref"], a["pin"]))
            else:
                out["failed"].append("%s: %s.%s" % (name, a["ref"], a["pin"]))
                way = search(sig, wide, net, tree, targets, through, b["novia"], (CROSS, None)) if victims_too and targets else None
                for f, k in way or []:
                    o = sig.owner[f][k]
                    if 0 <= o < 10000 and o != net:
                        victims.add(o)
            continue
        key = "fat" if width >= FAT else ("wide" if width >= PWR else "sig")
        out["nodes"] += [(key, f, k) for f, k in path]
        pts = [(f, (k % nx) * G, (k // nx) * G, k) for f, k in path]
        for end in (path[0], path[-1]):         # from the pad's own middle to the node the path took
            if end in of:
                p = b["pads"][of[end]]
                x, y = (end[1] % nx) * G, (end[1] // nx) * G
                if math.hypot(p["x"] - x, p["y"] - y) > 0.01:
                    out["tracks"].append((name, end[0], (p["x"], p["y"]), (x, y), width))
        run = [pts[0]]
        for prev, cur in zip(pts, pts[1:]):
            if cur[0] != prev[0]:               # the face changes here
                flush(maps, net, name, run, width, out)
                if cur[3] not in through:
                    out["nodes"] += [("wide", 0, cur[3]), ("wide", 1, cur[3])]
                    out["vias"].append((name, cur[1], cur[2]))
                    lay_via(maps, net, cur[1], cur[2])
                run = [cur]
            else:
                run.append(cur)
        flush(maps, net, name, run, width, out)
        for s in path:
            tree[s] = 0.0
        for s, c in targets.items():
            tree.setdefault(s, c)
    return victims


def flush(maps, net, name, run, width, out):
    """A run of nodes on one face becomes tracks, one per straight stretch."""
    if len(run) < 2:
        return
    start, prev = run[0], run[0]
    heading = None
    for cur in run[1:]:
        h = (round(cur[1] - prev[1], 4), round(cur[2] - prev[2], 4))
        if heading is not None and h != heading:
            out["tracks"].append((name, start[0], (start[1], start[2]), (prev[1], prev[2]), width))
            lay_track(maps, net, start[0], (start[1], start[2]), (prev[1], prev[2]), width)
            start = prev
        heading, prev = h, cur
    out["tracks"].append((name, start[0], (start[1], start[2]), (prev[1], prev[2]), width))
    lay_track(maps, net, start[0], (start[1], start[2]), (prev[1], prev[2]), width)


def unlay(maps, net):
    for m in maps.values():
        m.release(net)


def weigh(o):
    """What a net's tracks cost, in mm: their length, the back's dearer, and each via as the detour it is worth."""
    return (sum(math.hypot(t[3][0] - t[2][0], t[3][1] - t[2][1]) * (BACK if t[1] else 1.0) for t in o["tracks"])
            + VIA_COST * G * len(o["vias"]))


def route(b, passes):
    base = base_maps(b)
    grouped = groups(b, base["sig"].nx)
    names = {v: k for k, v in b["ids"].items()}
    N = base["sig"].nx * base["sig"].ny

    def span(net):
        ps = [b["pads"][n] for g, _ in grouped[net] for n in g]
        return (max(p["x"] for p in ps) - min(p["x"] for p in ps)) + (max(p["y"] for p in ps) - min(p["y"] for p in ps))
    def header(net):                    # does the net start on the 2x20 header?
        return any(b["pads"][n]["ref"] == "J1" for g, _ in grouped[net] for n in g)
    # power first, while there is room for its width; then the header's nets, which have the least
    # room to leave by; then the shortest. Ground is not here: see ground() below
    order = sorted((n for n in grouped if len(grouped[n]) > 1 and names[n] not in PLANES),
                   key=lambda n: (names[n] not in POWER, not header(n), span(n)))
    def blank():
        return {"tracks": [], "vias": [], "failed": [], "thin": [], "plane": [], "nodes": []}

    def lay(net, victims_too=False, cross=None):
        o = blank()
        victims = route_net(b, maps, net, grouped[net], names[net], o, victims_too, cross)
        return o, victims

    maps = {key: m.copy() for key, m in base.items()}
    for m in maps.values():
        m.freeze()
    t0 = time.time()
    # each net alone on the bare board: its direct way, to measure the others' against
    alone = {}
    for net in order:
        o, _ = lay(net)
        if not o["failed"]:
            alone[net] = weigh(o)
        unlay(maps, net)

    done = {}

    def total():
        o = blank()
        for net in order:
            for key in o:
                o[key] += done[net][key]
        return o

    def say(what):
        o = total()
        length = sum(math.hypot(t[3][0] - t[2][0], t[3][1] - t[2][1]) for t in o["tracks"])
        back = sum(math.hypot(t[3][0] - t[2][0], t[3][1] - t[2][1]) for t in o["tracks"] if t[1])
        print("  %s: %d vias, %.0f mm of track (%.0f on the back) in %d stretches, %.0f s" % (
            what, len(o["vias"]), length, back, len(o["tracks"]), time.time() - t0), flush=True)

    # the negotiation
    memory = [[0.0] * N, [0.0] * N]
    price, again, crossing = PRICE, list(order), set()
    for rnd in range(ROUNDS):
        for net in again:
            unlay(maps, net)
            done[net], _ = lay(net, cross=(price, memory))
        crossing, fought = set(), 0
        for net in order:
            for key, f, k in done[net]["nodes"]:
                who = maps[key].laid[f].get(k)
                if who and any(n != net for n in who):
                    crossing.add(net)
                    crossing.update(n for n in who if n != net)
                    memory[f][k] += MEMORY
                    fought += 1
        say("round %d, %d nets routed, %d still cross at %d nodes" % (rnd + 1, len(again), len(crossing), fought))
        if not crossing:
            break
        again = sorted(crossing, key=order.index)
        price *= GROWTH

    # what still crosses, and what found no way: one after another, taking off whoever is in the way
    queue = sorted(crossing | {n for n in order if done[n]["failed"]}, key=order.index)
    for net in queue:
        unlay(maps, net)
        del done[net]
    tries, rips = {}, 0
    while queue:
        net = queue.pop(0)
        out, victims = lay(net, True)
        victims = {v for v in victims if v in done}
        if out["failed"] and victims and tries.get(net, 0) < passes:
            tries[net] = tries.get(net, 0) + 1
            rips += 1
            unlay(maps, net)
            for v in victims:
                unlay(maps, v)
                del done[v]
            queue = [net] + sorted(victims, key=order.index) + [q for q in queue if q not in victims]
            continue
        done[net] = out
    best = total()
    say("in all, %d joins not made, %d nets routed one after another" % (len(best["failed"]), len(tries) + rips if rips else 0))
    best["detours"] = sorted(((weigh(done[n]) - alone[n], names[n], weigh(done[n]), alone[n]) for n in order
                              if n in alone and not done[n]["failed"] and weigh(done[n]) - alone[n] > SLACK), reverse=True)

    def ground(parts):
        """Tracks that join the ground's parts, as ground_parts() found them, to the biggest one."""
        o = blank()
        where = {(p["ref"], p["pin"], round(p["x"] + b["ox"], 2), round(p["y"] + b["oy"], 2)): n
                 for n, p in enumerate(b["pads"]) if p["name"] in PLANES}
        sets = [[where[k] for k in sorted(part) if k in where] for part in parts]
        sets = [g for g in sets if g]
        if len(sets) > 1:
            gnd = b["pads"][sets[0][0]]["net"]
            route_net(b, maps, gnd, [(g, []) for g in sets], names[gnd], o)
        return o
    return best, ground


def write(board, b, out, path):
    FromMM = pcbnew.FromMM
    for name, face, a, c, w in out["tracks"]:
        t = pcbnew.PCB_TRACK(board)
        t.SetStart(pcbnew.VECTOR2I(FromMM(a[0] + b["ox"]), FromMM(a[1] + b["oy"])))
        t.SetEnd(pcbnew.VECTOR2I(FromMM(c[0] + b["ox"]), FromMM(c[1] + b["oy"])))
        t.SetWidth(FromMM(w))
        t.SetLayer(pcbnew.F_Cu if face == 0 else pcbnew.B_Cu)
        t.SetNet(board.FindNet(name))
        board.Add(t)
    for name, x, y in out["vias"]:
        v = pcbnew.PCB_VIA(board)
        v.SetPosition(pcbnew.VECTOR2I(FromMM(x + b["ox"]), FromMM(y + b["oy"])))
        v.SetWidth(pcbnew.F_Cu, FromMM(VIA))
        v.SetDrill(FromMM(VIA_DRILL))
        v.SetLayerPair(pcbnew.F_Cu, pcbnew.B_Cu)
        v.SetNet(board.FindNet(name))
        board.Add(v)
    pcbnew.SaveBoard(path, board)


def prune(board, b, out):
    """The tracks place.py laid and the routing did not use (a header pin that left the other way):
    off the trial board, from their loose end back to the first thing that joins them."""
    def at(p, q):
        return abs(p[0] - q[0]) < 0.01 and abs(p[1] - q[1]) < 0.01
    joins = {}                          # net -> the points where routed copper touches
    for name, face, p, q, w in out["tracks"]:
        joins.setdefault(name, []).extend(((face, p), (face, q)))
    for name, x, y in out["vias"]:
        joins.setdefault(name, []).extend(((0, (x, y)), (1, (x, y))))
    names = {v: k for k, v in b["ids"].items()}
    kept, gone = list(b["tracks"]), 0
    again = True
    while again:
        again = False
        for t in list(kept):
            name = names[t["net"]]
            pads = [pad_dist(p) for p in b["pads"] if p["name"] == name and t["face"] in p["faces"]]
            for end, other in ((t["a"], t["b"]), (t["b"], t["a"])):
                if any(d(end[0], end[1]) <= 0.01 for d in pads):
                    continue            # on a pad
                if any(u is not t and u["net"] == t["net"] and u["face"] == t["face"] and (at(u["a"], end) or at(u["b"], end))
                       for u in kept):
                    continue            # another of place.py's tracks goes on from here
                on = seg_dist(t["a"][0], t["a"][1], t["b"][0], t["b"][1])
                touch = [pt for f, pt in joins.get(name, ()) if f == t["face"] and on(pt[0], pt[1]) < 0.01 and not at(pt, other)]
                if any(at(pt, end) for pt in touch):
                    continue            # routed copper starts at this end
                if touch:               # it joins further in: the track ends there
                    far = max(touch, key=lambda pt: math.hypot(pt[0] - other[0], pt[1] - other[1]))
                    new = pcbnew.VECTOR2I(pcbnew.FromMM(far[0] + b["ox"]), pcbnew.FromMM(far[1] + b["oy"]))
                    if end is t["a"]:
                        t["a"] = far
                        t["track"].SetStart(new)
                    else:
                        t["b"] = far
                        t["track"].SetEnd(new)
                else:
                    board.Remove(t["track"])
                    kept.remove(t)
                    gone += 1
                    again = True
                break
    return gone


def ground_parts(board):
    """The ground as the filled planes leave it: its pads in the groups the copper joins, the
    biggest first. A pad is (reference, pin, x, y). The planes' pieces are KiCad's own fill; which
    pads, vias and tracks touch which piece is worked out here."""
    mm, to = pcbnew.ToMM, pcbnew.FromMM
    faces = (pcbnew.F_Cu, pcbnew.B_Cu)
    parent = {}

    def find(a):
        parent.setdefault(a, a)
        while parent[a] != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a

    def join(a, c):
        parent[find(a)] = find(c)
    fills = {}
    for n, z in enumerate(board.Zones()):
        if not z.GetIsRuleArea() and z.GetNetname() in PLANES:
            for layer in faces:
                if z.IsOnLayer(layer):
                    fills.setdefault(layer, []).append((n, z.GetFilledPolysList(layer)))

    def pieces(layer, pt, reach):
        return [(layer, n, i) for n, ps in fills.get(layer, ()) for i in range(ps.OutlineCount()) if ps.Contains(pt, i, reach)]
    pads = []
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            if pad.GetNetname() in PLANES:
                bb = pad.GetBoundingBox()
                c = bb.GetCenter()
                key = (fp.GetReference(), pad.GetNumber(), round(mm(c.x), 2), round(mm(c.y), 2))
                pads.append((key, pad, c, max(bb.GetWidth(), bb.GetHeight()) // 2))
                find(key)
                for layer in faces:
                    if pad.IsOnLayer(layer):
                        for piece in pieces(layer, c, max(bb.GetWidth(), bb.GetHeight()) // 2 + to(0.05)):
                            join(key, piece)
    for n, t in enumerate(board.GetTracks()):
        if t.GetNetname() not in PLANES:
            continue
        me = ("track", n)
        if t.GetClass() == "PCB_VIA":
            spots = [(layer, t.GetPosition()) for layer in faces]
        else:
            a, c = t.GetStart(), t.GetEnd()
            steps = max(1, int(mm(max(abs(c.x - a.x), abs(c.y - a.y))) / 0.4))
            spots = [(t.GetLayer(), pcbnew.VECTOR2I(a.x + (c.x - a.x) * q // steps, a.y + (c.y - a.y) * q // steps))
                     for q in range(steps + 1)]
        half = t.GetWidth(t.GetLayer()) // 2 if t.GetClass() == "PCB_VIA" else t.GetWidth() // 2
        for layer, pt in spots:
            for piece in pieces(layer, pt, half):
                join(me, piece)
            for key, pad, c, r in pads:
                if pad.IsOnLayer(layer) and abs(pt.x - c.x) <= r and abs(pt.y - c.y) <= r:
                    join(me, key)
        # and the tracks and vias that meet end to end
        for layer, pt in (spots[0], spots[-1]):
            join(me, ("at", layer, pt.x // to(0.02), pt.y // to(0.02)))
    groups = {}
    for key, _, _, _ in pads:
        groups.setdefault(find(key), set()).add(key)
    return sorted(groups.values(), key=lambda g: (-len(g), sorted(g)))


def drc(path):
    """KiCad's check on a copy with the planes filled: {kind: count}, what is still not joined, and
    the ground's pads in the groups the planes and its tracks join."""
    with tempfile.TemporaryDirectory() as tmp:
        copy = os.path.join(tmp, os.path.basename(path))
        shutil.copy(path, copy)
        report = os.path.join(tmp, "drc.json")
        subprocess.run([KICAD_CLI, "pcb", "drc", "--refill-zones", "--save-board", "--severity-all", "--format", "json",
                        "-o", report, copy], capture_output=True, check=False)
        with open(report, encoding="utf-8") as f:
            found = json.load(f)
        parts = ground_parts(pcbnew.LoadBoard(copy))
    kinds, where = {}, []
    for v in found.get("violations", []):
        kinds[v["type"]] = kinds.get(v["type"], 0) + 1
        if v["type"] not in ("silk_edge_clearance",):
            where.append("%s: %s" % (v["type"], " / ".join(i["description"] for i in v["items"][:2])))
    loose = [" / ".join(i["description"] for i in u["items"][:2]) for u in found.get("unconnected_items", [])]
    return kinds, where, loose, parts


def main():
    variant = sys.argv[1] if len(sys.argv) > 1 else "th"
    passes = int(sys.argv[2]) if len(sys.argv) > 2 else RIPS
    src = os.path.join(HERE, "aux-board-%s.kicad_pcb" % variant)
    dst = os.path.join(HERE, "trial-%s.kicad_pcb" % variant)
    board = pcbnew.LoadBoard(src)
    b = read(board)
    print("routing %s: %d pads, %d tracks already there" % (os.path.basename(src), len(b["pads"]), len(b["tracks"])), flush=True)
    global VIA_COST, BACK
    out = ground = None
    for VIA_COST, BACK in COSTS:
        print(" a via costs %g steps, the back %g:" % (VIA_COST, BACK), flush=True)
        got, g = route(b, passes)
        if out is None or (len(got["failed"]), len(got["vias"])) < (len(out["failed"]), len(out["vias"])):
            out, ground = got, g
        if not got["failed"]:
            break
    gone = prune(board, b, out)
    write(board, b, out, dst)
    print("wrote %s: %d tracks, %d vias%s" % (os.path.basename(dst), len(out["tracks"]), len(out["vias"]),
                                              "; %d of place.py's tracks went unused and were left out" % gone if gone else ""))
    # ground: the planes, filled, join nearly all of it; a track only for the pads they left out,
    # and again for the ones those tracks show to be on an island of their own
    for _ in range(4):
        kinds, where, loose, parts = drc(dst)
        if len(parts) < 2:
            break
        g = ground(parts)
        write(board, b, g, dst)
        out["thin"] += g["thin"]
        print("ground: the planes leave %d groups of pads apart from the rest (%s); %d tracks and %d vias to join them%s" % (
            len(parts) - 1, "; ".join(", ".join(sorted("%s.%s" % k[:2] for k in part)) for part in parts[1:]),
            len(g["tracks"]), len(g["vias"]), ", no way to %s" % ", ".join(g["plane"]) if g["plane"] else ""))
        if not g["tracks"]:
            break
    widths = {}
    for name, width, ends in out["thin"]:
        widths.setdefault(name, {}).setdefault(width, []).extend(ends)
    for name in sorted(widths):
        print("  %-8s %s" % (name, "; ".join("%d joins at %.2f mm%s" % (
            sum(1 for n, w, _ in out["thin"] if n == name and w == width), width,
            " (%s)" % ", ".join(sorted(set(widths[name][width]))) if width < FAT else "")
            for width in sorted(widths[name], reverse=True))))
    for f in out["failed"]:
        print("  not joined:", f)
    if out["detours"]:
        print("the longest ways round (mm over the net's direct way, counting a via as %.1f mm):" % (VIA_COST * G))
        for over, name, is_, direct in out["detours"][:10]:
            print("  %-16s +%.0f (%.0f against %.0f)" % (name, over, is_, direct))
    print("design rules:", ", ".join("%s x%d" % k for k in sorted(kinds.items())) or "clean")
    for w in where[:40]:
        print("  ", w)
    print("still not joined, by KiCad's count: %d" % len(loose))
    for u in loose[:40]:
        print("  ", u)


if __name__ == "__main__":
    main()
