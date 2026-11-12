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
own through-hole pads for free or through a via at a price. A net that finds no way looks again as
if the tracks laid before it were not there, at a price for each one crossed: the nets it crosses
are taken off, it is routed, and they are routed again after it. It is a plain maze router: what
it cannot route a person may, and its tracks are valid, not pretty.
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
SIG, PWR = 0.25, 0.5            # track widths
VIA, VIA_DRILL = 0.6, 0.3
POWER = ("+5V", "+5V_SW", "+3V3", "VBUS_HS", "VBUS_FS")
PLANES = ("GND",)               # routed last: the planes join most of them, tracks make sure of the rest
# What a via costs, in grid steps (at 12, 3.8 mm of detour), and how much more a step on the back,
# which is the ground's. The router is tried with each pair until one joins everything: which one
# does depends on the placement, and a run takes half a minute.
COSTS = ((12, 1.15), (30, 1.5), (20, 1.0), (12, 1.5), (8, 1.0), (16, 1.3))
VIA_COST, BACK = COSTS[0]
CROSS = 25                      # looking for whom to take off: the price of a node another net's track holds
RIPS = 5                        # how many times a net may have others taken off for it
FREE, CLASH = -1, -2


class Map:
    """Who may pass each node of each face, for a track whose half width is 'half'."""

    def __init__(self, nx, ny, half):
        self.nx, self.ny, self.half = nx, ny, half
        self.owner = [[FREE] * (nx * ny), [FREE] * (nx * ny)]

    def copy(self):
        m = Map(self.nx, self.ny, self.half)
        m.owner = [list(self.owner[0]), list(self.owner[1])]
        return m

    def claim(self, face, i, j, net):
        if 0 <= i < self.nx and 0 <= j < self.ny:
            own = self.owner[face]
            k = j * self.nx + i
            if own[k] == FREE:
                own[k] = net
            elif own[k] != net:
                own[k] = CLASH

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
                           "b": (mm(t.GetEnd().x) - ox, mm(t.GetEnd().y) - oy), "w": mm(t.GetWidth())})
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
    maps = {"sig": Map(nx, ny, SIG / 2), "wide": Map(nx, ny, max(PWR, VIA) / 2)}
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


def search(m, wide, net, sources, targets, through, novia, hard=None):
    """A*: the cheapest way from any of 'sources' to any of 'targets', as a list of (face, node).
    With 'hard', the map before any routing, other nets' tracks may be crossed at a price: the way
    found says which tracks are in the way."""
    nx, ny = m.nx, m.ny
    N = nx * ny
    ti = [k % nx for _, k in targets]
    tj = [k // nx for _, k in targets]
    i0, i1, j0, j1 = min(ti), max(ti), min(tj), max(tj)
    goal = {f * N + k for f, k in targets}
    # a face prefers one direction, so tracks on the two faces cross instead of blocking each other
    moves = ([(1, 0, 1.0), (-1, 0, 1.0), (0, 1, 1.25), (0, -1, 1.25),
              (1, 1, 1.5), (1, -1, 1.5), (-1, 1, 1.5), (-1, -1, 1.5)],
             [(di, dj, c * BACK) for di, dj, c in
              ((1, 0, 1.25), (-1, 0, 1.25), (0, 1, 1.0), (0, -1, 1.0), (1, 1, 1.5), (1, -1, 1.5), (-1, 1, 1.5), (-1, -1, 1.5))])
    best, parent, heap = {}, {}, []
    for f, k in sources:
        s = f * N + k
        best[s] = 0.0
        heap.append((0.0, 0.0, s))
    heapq.heapify(heap)
    while heap:
        _, g, s = heapq.heappop(heap)
        if g > best[s]:
            continue
        if s in goal:
            path = [s]
            while s in parent:
                s = parent[s]
                path.append(s)
            return [divmod(s, N) for s in reversed(path)]
        f, k = divmod(s, N)
        j, i = divmod(k, nx)
        own = m.owner[f]
        for di, dj, cost in moves[f]:
            ni, nj = i + di, j + dj
            if not (0 <= ni < nx and 0 <= nj < ny):
                continue
            nk = nj * nx + ni
            o = own[nk]
            if o != FREE and o != net:
                if hard is None or hard.owner[f][nk] not in (FREE, net):
                    continue
                cost += CROSS if o >= 0 else 3 * CROSS
            ns, ng = f * N + nk, g + cost
            if ng < best.get(ns, 1e18):
                best[ns] = ng
                parent[ns] = s
                dx = i0 - ni if ni < i0 else (ni - i1 if ni > i1 else 0)
                dy = j0 - nj if nj < j0 else (nj - j1 if nj > j1 else 0)
                heapq.heappush(heap, (ng + max(dx, dy) + 0.5 * min(dx, dy), ng, ns))
        # to the other face: through one of the net's own holes, or a via where one fits
        o = m.owner[1 - f][k]
        if o == FREE or o == net or (hard is not None and k in through and hard.owner[1 - f][k] in (FREE, net)):
            if k in through:
                cost = 0.05
            elif not novia[k] and all(wide.owner[q][k] in (FREE, net) for q in (0, 1)):
                cost = VIA_COST
            else:
                continue
            ns, ng = (1 - f) * N + k, g + cost
            if ng < best.get(ns, 1e18):
                best[ns] = ng
                parent[ns] = s
                dx = i0 - i if i < i0 else (i - i1 if i > i1 else 0)
                dy = j0 - j if j < j0 else (j - j1 if j > j1 else 0)
                heapq.heappush(heap, (ng + max(dx, dy) + 0.5 * min(dx, dy), ng, ns))
    return None


def route_net(b, maps, net, grouped, name, out, base=None):
    """Joins a net's groups of pads; what it could not join is in out["failed"]. Returns the nets
    whose tracks stand in the way of those."""
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

    def roomy(states):                  # the states where the wide track fits too
        return [s for s in states if wide.owner[s[0]][s[1]] in (FREE, net)]
    order = sorted(range(len(grouped)), key=lambda q: -len(grouped[q]))
    tree = {s for _, s in nodes[order[0]]}
    left, victims = order[1:], set()
    while left:
        # the nearest group first, as the crow flies from the pads already joined
        done = [b["pads"][of[s]] for s in tree if s in of] or [b["pads"][grouped[order[0]][0]]]
        q = min(left, key=lambda q: min(math.hypot(b["pads"][n]["x"] - p["x"], b["pads"][n]["y"] - p["y"])
                                        for n in grouped[q] for p in done[:40]))
        left.remove(q)
        targets = [s for _, s in nodes[q]]
        path, width = None, SIG
        if fat and roomy(targets) and roomy(tree):
            path, width = search(wide, wide, net, roomy(tree), roomy(targets), through, b["novia"]), PWR
        if not path and targets and tree:
            path, width = search(sig, wide, net, tree, targets, through, b["novia"]), SIG
            if path and fat:            # a pad or a passage with no room for the wide track
                out["thin"].append(name)
        if not path:
            a = b["pads"][grouped[q][0]]
            if name in PLANES:          # no track reaches it: the plane may, and KiCad's check says
                out["plane"].append("%s.%s" % (a["ref"], a["pin"]))
            else:
                out["failed"].append("%s: %s.%s" % (name, a["ref"], a["pin"]))
                way = search(sig, wide, net, tree, targets, through, b["novia"], hard=base["sig"]) if base and targets else None
                for f, k in way or []:
                    o = sig.owner[f][k]
                    if 0 <= o < 10000 and o != net:
                        victims.add(o)
            continue
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
                    out["vias"].append((name, cur[1], cur[2]))
                    lay_via(maps, net, cur[1], cur[2])
                run = [cur]
            else:
                run.append(cur)
        flush(maps, net, name, run, width, out)
        tree |= set(path) | {s for _, s in nodes[q]}
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


def route(b, passes):
    base = base_maps(b)
    grouped = groups(b, base["sig"].nx)
    names = {v: k for k, v in b["ids"].items()}

    def span(net):
        ps = [b["pads"][n] for g, _ in grouped[net] for n in g]
        return (max(p["x"] for p in ps) - min(p["x"] for p in ps)) + (max(p["y"] for p in ps) - min(p["y"] for p in ps))
    def header(net):                    # does the net start on the 2x20 header?
        return any(b["pads"][n]["ref"] == "J1" for g, _ in grouped[net] for n in g)
    # power first, while there is room for its width; then the header's nets, which have the least
    # room to leave by; then the shortest; ground last, by track wherever the planes might not reach
    order = sorted((n for n in grouped if len(grouped[n]) > 1),
                   key=lambda n: (names[n] in PLANES, names[n] not in POWER, not header(n), span(n)))
    def blank():
        return {"tracks": [], "vias": [], "failed": [], "thin": [], "plane": []}

    maps = {key: m.copy() for key, m in base.items()}
    done, tries, queue, rips, t0 = {}, {}, list(order), 0, time.time()
    while queue:
        net = queue.pop(0)
        out = blank()
        victims = {v for v in route_net(b, maps, net, grouped[net], names[net], out, base)
                   if v in done and names[v] not in PLANES}
        if out["failed"] and victims and tries.get(net, 0) < passes:
            # take them off, and this net's own half-made tracks; lay the rest again on a clean map
            tries[net] = tries.get(net, 0) + 1
            rips += 1
            for v in victims:
                del done[v]
            maps = {key: m.copy() for key, m in base.items()}
            for n, o in done.items():
                for _, face, p, q, w in o["tracks"]:
                    lay_track(maps, n, face, p, q, w)
                for _, x, y in o["vias"]:
                    lay_via(maps, n, x, y)
            queue = [net] + sorted(victims, key=order.index) + [q for q in queue if q not in victims]
            continue
        done[net] = out
    best = blank()
    for net in order:
        for key in best:
            best[key] += done[net][key]
    length = sum(math.hypot(t[3][0] - t[2][0], t[3][1] - t[2][1]) for t in best["tracks"])
    print("  %d joins not made, %d vias, %.0f mm of track, %d times a net was taken off for another, %.0f s" % (
        len(best["failed"]), len(best["vias"]), length, rips, time.time() - t0), flush=True)
    return best


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


def drc(path):
    """KiCad's check on a copy with the planes filled: {kind: count}, and what is still not joined."""
    with tempfile.TemporaryDirectory() as tmp:
        copy = os.path.join(tmp, os.path.basename(path))
        shutil.copy(path, copy)
        report = os.path.join(tmp, "drc.json")
        subprocess.run([KICAD_CLI, "pcb", "drc", "--refill-zones", "--save-board", "--severity-all", "--format", "json",
                        "-o", report, copy], capture_output=True, check=False)
        with open(report, encoding="utf-8") as f:
            found = json.load(f)
    kinds, where = {}, []
    for v in found.get("violations", []):
        kinds[v["type"]] = kinds.get(v["type"], 0) + 1
        if v["type"] not in ("silk_edge_clearance",):
            where.append("%s: %s" % (v["type"], " / ".join(i["description"] for i in v["items"][:2])))
    loose = [" / ".join(i["description"] for i in u["items"][:2]) for u in found.get("unconnected_items", [])]
    return kinds, where, loose


def main():
    variant = sys.argv[1] if len(sys.argv) > 1 else "th"
    passes = int(sys.argv[2]) if len(sys.argv) > 2 else RIPS
    src = os.path.join(HERE, "aux-board-%s.kicad_pcb" % variant)
    dst = os.path.join(HERE, "trial-%s.kicad_pcb" % variant)
    board = pcbnew.LoadBoard(src)
    b = read(board)
    print("routing %s: %d pads, %d tracks already there" % (os.path.basename(src), len(b["pads"]), len(b["tracks"])), flush=True)
    global VIA_COST, BACK
    out = None
    for VIA_COST, BACK in COSTS:
        print(" a via costs %g steps, the back %g:" % (VIA_COST, BACK), flush=True)
        got = route(b, passes)
        if out is None or (len(got["failed"]), len(got["vias"])) < (len(out["failed"]), len(out["vias"])):
            out = got
        if not got["failed"]:
            break
    write(board, b, out, dst)
    print("wrote %s: %d tracks, %d vias" % (os.path.basename(dst), len(out["tracks"]), len(out["vias"])))
    if out["thin"]:
        thin = {n: out["thin"].count(n) for n in sorted(set(out["thin"]))}
        print("power joins made with the thin track, no room for %.1f mm: %s" % (
            PWR, ", ".join("%s x%d" % kv for kv in thin.items())))
    for f in out["failed"]:
        print("  not joined:", f)
    if out["plane"]:
        print("ground pads no track reached, left to the planes:", ", ".join(out["plane"]))
    kinds, where, loose = drc(dst)
    print("design rules:", ", ".join("%s x%d" % k for k in sorted(kinds.items())) or "clean")
    for w in where[:40]:
        print("  ", w)
    print("still not joined, by KiCad's count: %d" % len(loose))
    for u in loose[:40]:
        print("  ", u)


if __name__ == "__main__":
    main()
