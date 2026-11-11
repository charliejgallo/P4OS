#!/usr/bin/env python3
"""A first placement of both variants: the parts of gen.py on a board, with their nets and silkscreen.

    <KiCad's python3> hw/aux-board/place.py     writes aux-board-th.kicad_pcb and aux-board-smd.kicad_pcb

It runs under KiCad's own Python (it needs pcbnew):
/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3

The board has the outline and the four holes of the P4's own board, as
measured on Waveshare's 3D model (docs/plan/PCB.md, "Mecánica"), seen from
the back of the P4: x to the right, y down, the USB-C connectors along the
top edge, the 40-pin header down the left edge with pin 1 at its bottom.
Nothing is routed. This is a draft to look at and to move by hand.

The silkscreen names every connector and every pin of it, and every bridge.
The solder mask is black, as the boards will be ordered.
"""
import os

import pcbnew

import gen
import kisch

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 64.51, 118.49                    # the P4's board
HOLES = [(3.75, 3.24), (60.75, 3.24), (3.75, 115.24), (60.75, 115.24)]
PIN1 = (3.15, 104.32)                   # the header's pin 1; odd pins on the edge side, rows going up

# The W5500 Lite (HW-884), as it sits on its two sockets: chip side down, the RJ45 on top with its
# mouth to the board's right edge. ETH is the middle of its two rows of pins.
#   ETH_ROWS   20.32 mm between the rows: columns C and I of a breadboard, measured by the user.
#   the board  28.5 x 23.5 mm. Its pins are at the RJ45 end: G, the first, about ETH_PIN1 from that
#              edge, read off the user's photos and NOT measured: check with a ruler before routing.
#   the RJ45   sticks out ETH_JACK past that edge, also from the photos.
# The sockets are 8.5 mm tall: under the module only what is lower than that (a resistor, a small
# capacitor, SMD), never a pin header or an electrolytic. overlaps() holds the tall ones out.
ETH_ROWS, ETH_LEN, ETH_WID, ETH_PIN1, ETH_JACK = 20.32, 28.5, 23.5, 2.5, 3.5
ETH = (W - ETH_PIN1 - 6.35, 88.76)                      # the module's edge on the board's, the RJ45 past it
ETH_BOX = (ETH[0] + 6.35 + ETH_PIN1 - ETH_LEN, ETH[1] - ETH_WID / 2, ETH[0] + 6.35 + ETH_PIN1, ETH[1] + ETH_WID / 2)
LOW = ("R", "U")                                        # what may go under it, by its reference: a TO-92 "Q" may not

# ref -> (x, y, degrees): the middle of the part, mm from the top left corner. Laid out on the
# larger of each part's two packages, so both variants share it; build() refuses an overlap.
AT = {
    # i2c, down the left edge above the header, and what serves it
    "J5": (3, 12.2, 0), "J6": (3, 23.6, 0), "J7": (3, 35, 0), "J8": (3, 46.4, 0),
    "R4": (17.5, 9, 0), "R5": (17.5, 12.5, 0), "JP2": (26.5, 10.6, 0),
    "Q3": (13.5, 20, 0), "Q4": (13.5, 26.8, 0), "R6": (24, 20, 0), "R7": (24, 26, 0),
    "JP3": (15, 44, 90), "JP4": (15, 48.5, 90),
    # infrared, looking out of the top edge
    "U3": (32, 4, 180), "D1": (42, 4, 0), "R13": (36, 9.5, 0), "C8": (34.5, 16.5, 0), "JP13": (42.6, 15.5, 90),
    "R16": (54, 9.3, 0), "R14": (57.7, 12.8, 0), "R15": (57.7, 16.3, 0), "Q5": (48.6, 15.5, 0),
    "JP14": (48, 22, 90), "J19": (58, 23, 90),
    # 1-wire and the strip, on the right edge
    "J15": (58.5, 38, 90), "R17": (43, 29, 0), "JP15": (44, 34.8, 90),
    "J16": (58.5, 58, 90), "R18": (45, 50.8, 0), "C9": (45.5, 44.5, 0),
    # the two bulk capacitors that found no room beside what they feed (a draft)
    "C1": (22.5, 33.5, 0), "C11": (32.4, 36, 0),
    # the ports, in the middle
    "J14": (27, 47.5, 90), "JP16": (36.5, 47.5, 0),
    "J9": (23, 64.9, 0), "J13": (30.3, 63.6, 0), "J11": (37.6, 67.4, 0), "J12": (44.9, 64.9, 0),
    "R10": (27.5, 74, 0), "R11": (27.5, 77.5, 0), "JP9": (25, 81.5, 90),
    # ethernet: the module lies over ETH, its RJ45 past the right edge; two low parts go under it
    # at 270 degrees a socket's pin 1 is at its right end: G and GND, the pins nearest the RJ45's mouth
    "J17": (ETH[0], ETH[1] - ETH_ROWS / 2, 270), "J18": (ETH[0], ETH[1] + ETH_ROWS / 2, 270),
    "R19": (ETH[0] - 12, ETH[1], 0), "C10": (ETH[0] + 2.5, ETH[1], 0),
    # the 5 V switch and the USB sockets, at the bottom
    "R1": (27.7, 86.5, 0), "R2": (27.7, 90, 0), "R3": (27.7, 93.5, 0), "Q1": (27, 98.7, 0),
    "Q2": (50.2, 74.4, 0),                              # beside the fuses: a TO-92 is too tall to go under the module
    "U4": (40.5, 84.5, 0), "U5": (42.5, 100.3, 0),
    "F1": (57.5, 74.3, 0), "F2": (57.5, 69.7, 0),       # the two fuses, between the terminal and the module
    "C3": (13, 111, 0), "C4": (60.5, 106.1, 90),
    "JP1": (33.5, 26.5, 90), "C2": (32.6, 81.4, 0),
    "J3": (27.5, 111.2, 0), "J4": (47.5, 111.2, 0),
}
# the Qwiic socket, its mouth to the left edge; and three parts whose smaller package leaves its
# reference or a pad under a neighbour's lettering
AT_SMD = {"J6": (3.6, 23.6, 270), "C9": (44.0, 44.3, 0), "R18": (45, 51.6, 0), "U5": (42.5, 97.6, 0)}

# what the silkscreen says of each connector: its name (one line or two) and its pins, pin 1 first
NAME = {"J5": ["I2C 3V3"], "J6": ["I2C 3V3"], "J7": ["I2C 5V"], "J8": ["BOARD I2C"], "J9": ["UART A", "RS485"],
        "J13": ["CAN"], "J11": ["UART B"], "J12": ["SWD"], "J15": ["1-WIRE"], "J16": ["LED / SERVO"]}
PINS = {
    "J5": ["GND", "3V3", "SDA", "SCL"], "J6": ["GND", "3V3", "SDA", "SCL"], "J7": ["GND", "5V", "SDA", "SCL"],
    "J8": ["GND", "3V3", "SDA", "SCL"], "J9": ["3V3", "TX", "RX", "GND", "DE"], "J13": ["3V3", "GND", "CTX", "CRX"],
    "J11": ["GND", "3V3", "TX", "RX", "EN", "BOOT", "RX5V"], "J12": ["3V3", "DIO", "CLK", "RST", "GND"],
    "J14": ["3V3", "GND", "SCK", "MOSI", "MISO", "CS", "46", "47"], "J15": ["3V3", "DQ", "GND"],
    "J16": ["5V", "DIN", "GND"], "J17": ["G", "G", "MO", "SCK", "CS", "INT"],
    "J18": ["GND", "V", "V", "NC", "RST", "MI"], "J19": ["INT", "RST"],
}
REF = {"D1": (37.2, 2.8), "Q2": (45.6, 74.6)}          # a reference that its part's own place would hide
SIDE = {"J17": "S", "J18": "N", "J19": "N", "J15": "W", "J16": "W"}        # where the pin names go; else east
BRIDGE = {"JP1": "5V BYPASS", "JP2": "PULL-UPS", "JP3": "SDA", "JP4": "SCL", "JP9": "RX 5V", "JP13": "IR RX",
          "JP14": "IR LED", "JP15": "1W PULL-UP", "JP16": "ETH CS"}
# free text: (text, x, y, degrees, size)
TEXT = [("USB 2.0", 12.5, 116.6, 0, 1.0), ("USB 1.1", 60.4, 111.9, 0, 0.9), ("SPI A", 24, 40.2, 0, 0.9),
        ("IR RX", 24.5, 5.4, 0, 0.8), ("IR LED", 49.5, 4, 0, 0.8), ("GPIO", 17.3, 53.3, 0, 0.8),
        ("W5500 INT RST", 58, 26.3, 0, 0.8),
        ("W5500 LITE", ETH[0] - 4, ETH[1] - 3.6, 0, 1.0), ("RJ45 >", ETH[0] - 4, ETH[1] + 3.6, 0, 0.8)]
# the forty pins as the pass-through socket shows them: GPIO numbers, G for ground
GPIO = {1: "5V", 2: "3V3", 3: "5V", 4: "7", 5: "G", 6: "8", 7: "37", 8: "2", 9: "38", 10: "G", 11: "5", 12: "3",
        13: "G", 14: "4", 15: "21", 16: "28", 17: "22", 18: "3V3", 19: "G", 20: "29", 21: "24", 22: "30",
        23: "25", 24: "31", 25: "D-", 26: "G", 27: "D+", 28: "34", 29: "G", 30: "35", 31: "32", 32: "49",
        33: "G", 34: "50", 35: "46", 36: "51", 37: "47", 38: "52", 39: "48", 40: "G"}

STACKUP = """		(stackup
			(layer "F.SilkS" (type "Top Silk Screen") (color "White"))
			(layer "F.Paste" (type "Top Solder Paste"))
			(layer "F.Mask" (type "Top Solder Mask") (color "Black") (thickness 0.01))
			(layer "F.Cu" (type "copper") (thickness 0.035))
			(layer "dielectric 1" (type "core") (thickness 1.51) (material "FR4") (epsilon_r 4.5) (loss_tangent 0.02))
			(layer "B.Cu" (type "copper") (thickness 0.035))
			(layer "B.Mask" (type "Bottom Solder Mask") (color "Black") (thickness 0.01))
			(layer "B.Paste" (type "Bottom Solder Paste"))
			(layer "B.SilkS" (type "Bottom Silk Screen") (color "White"))
			(copper_finish "HAL lead-free")
			(dielectric_constraints no)
		)
"""


def mm(x, y):
    return pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y))


def xy(v):
    return pcbnew.ToMM(v.x), pcbnew.ToMM(v.y)


def text(board, s, x, y, angle=0, size=0.8, just="c"):
    t = pcbnew.PCB_TEXT(board)
    t.SetText(s)
    t.SetLayer(pcbnew.F_SilkS)
    t.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(size), pcbnew.FromMM(size)))
    t.SetTextThickness(pcbnew.FromMM(0.15))
    t.SetHorizJustify({"c": pcbnew.GR_TEXT_H_ALIGN_CENTER, "l": pcbnew.GR_TEXT_H_ALIGN_LEFT,
                       "r": pcbnew.GR_TEXT_H_ALIGN_RIGHT}[just])
    t.SetVertJustify(pcbnew.GR_TEXT_V_ALIGN_CENTER)
    t.SetTextAngleDegrees(angle)
    t.SetPosition(mm(x, y))
    board.Add(t)


def pin_names(board, fp, names, side):
    """The name of each pad beside it: east or west of a column, north or south of a row."""
    bb = fp.GetBoundingBox(False)
    cx, cy = xy(bb.GetCenter())
    half_w, half_h = pcbnew.ToMM(bb.GetWidth()) / 2, pcbnew.ToMM(bb.GetHeight()) / 2
    pads = {p.GetNumber(): xy(p.GetPosition()) for p in fp.Pads()}
    two_rows = len({round(v[0], 1) for v in pads.values()}) > 1 and len({round(v[1], 1) for v in pads.values()}) > 1
    for i, name in enumerate(names):
        px, py = pads[str(i + 1)]
        s = side
        if two_rows:                        # each row's names on its own outer side
            s = ("N" if py < cy else "S") if half_w > half_h else ("W" if px < cx else "E")
        if s == "E":
            text(board, name, cx + half_w + 0.5, py, 0, 0.8, "l")
        elif s == "W":
            text(board, name, cx - half_w - 0.5, py, 0, 0.8, "r")
        elif s == "N":
            text(board, name, px, cy - half_h - 0.5, 90, 0.8, "l")
        else:
            text(board, name, px, cy + half_h + 0.5, 90, 0.8, "r")


def overlaps(board):
    """Pairs of parts whose bodies and pads share board: [(ref, ref, mm of overlap)]. Text does not count."""
    boxes = []
    for fp in board.GetFootprints():
        bb = fp.GetBoundingBox(False)
        boxes.append((fp.GetReference(), pcbnew.ToMM(bb.GetLeft()), pcbnew.ToMM(bb.GetTop()),
                      pcbnew.ToMM(bb.GetRight()), pcbnew.ToMM(bb.GetBottom())))
    out = []
    for i, a in enumerate(boxes):
        for b in boxes[i + 1:]:
            w = min(a[3], b[3]) - max(a[1], b[1])
            h = min(a[4], b[4]) - max(a[2], b[2])
            if w > 0.05 and h > 0.05:
                out.append((a[0], b[0], round(min(w, h), 2)))
    off = [b[0] for b in boxes if b[1] < -0.01 or b[2] < -0.01 or b[3] > W + 0.01
           or (b[4] > H + 0.01 and b[0] not in ("J3", "J4"))]       # the USB sockets stick out on purpose
    # and under the Ethernet module only what is low: its own two sockets and what LOW allows
    x0, y0, x1, y1 = ETH_BOX
    for b in boxes:
        w = min(b[3], x1) - max(b[1], x0)
        h = min(b[4], y1) - max(b[2], y0)
        if w > 0.6 and h > 0.6 and b[0] not in ("J17", "J18") and not b[0].startswith(LOW) and b[0] != "C10":
            out.append((b[0], "W5500", round(min(w, h), 2)))
    return out, off


def holes_svg(board, path, grow=0.0):
    """The board's outline and every hole through it (pads' drills, mounting holes) as one filled
    shape, 1 unit = 1 mm: a slicer extrudes it into a plate to try the fit on the P4. 'grow' widens
    each hole's diameter, for a printer that closes small holes."""
    d = ["M0 0H%.3fV%.3fH0Z" % (W, H)]
    n = 0
    for fp in board.GetFootprints():
        for pad in fp.Pads():
            dx, dy = pcbnew.ToMM(pad.GetDrillSize().x), pcbnew.ToMM(pad.GetDrillSize().y)
            if dx <= 0:
                continue
            x, y = xy(pad.GetPosition())
            n += 1
            if abs(dx - dy) < 0.01:
                r = (dx + grow) / 2
                d.append("M%.3f %.3fa%.3f %.3f 0 1 0 %.3f 0a%.3f %.3f 0 1 0 %.3f 0Z" % (x - r, y, r, r, 2 * r, r, r, -2 * r))
            else:                           # a slot: two half circles and their sides, turned with the pad
                import math
                a = math.radians(-pad.GetOrientation().AsDegrees())
                long_, short = max(dx, dy), min(dx, dy) + grow
                if dy > dx:
                    a += math.pi / 2
                r, half = short / 2, (long_ - min(dx, dy)) / 2
                ux, uy = math.cos(a), math.sin(a)
                x1, y1, x2, y2 = x - ux * half, y - uy * half, x + ux * half, y + uy * half
                px, py = -uy * r, ux * r
                d.append("M%.3f %.3fL%.3f %.3fA%.3f %.3f 0 0 1 %.3f %.3fL%.3f %.3fA%.3f %.3f 0 0 1 %.3f %.3fZ" % (
                    x1 + px, y1 + py, x2 + px, y2 + py, r, r, x2 - px, y2 - py, x1 - px, y1 - py, r, r, x1 + px, y1 + py))
    with open(path, "w", encoding="utf-8") as f:
        f.write('<svg xmlns="http://www.w3.org/2000/svg" width="%.2fmm" height="%.2fmm" viewBox="0 0 %.2f %.2f">\n'
                '<path fill="#000" fill-rule="evenodd" d="%s"/>\n</svg>\n' % (W, H, W, H, " ".join(d)))
    return n


# The tracks' widths, mm. Between two pads of a 2.54 mm header there are 0.84 mm: a 0.4 mm track
# passes with 0.22 mm to each side, over the 0.2 mm the default rules ask for.
SIGNAL, POWER = 0.3, 0.4
SUPPLY = ("GND", "+5V", "+3V3")


def track(board, net, layer, points, width):
    for a, b in zip(points, points[1:]):
        t = pcbnew.PCB_TRACK(board)
        t.SetStart(mm(*a))
        t.SetEnd(mm(*b))
        t.SetWidth(pcbnew.FromMM(width))
        t.SetLayer(layer)
        t.SetNet(net)
        board.Add(t)


def route(board, net):
    """The tracks a script can lay without choosing anything: each of the header's forty pins across
    to the same pin of the socket beside it, and a ground plane on each face. Returns how many tracks.

    An odd pin has the even pin of its own row in the way, and an even pin the socket's odd one, so
    each goes round through the gap to the next row: the odd ones above their row on the front, the
    even ones below theirs on the back. On one face the two would meet in the same gap."""
    pads = {r: {p.GetNumber(): xy(p.GetPosition()) for p in board.FindFootprintByReference(r).Pads()}
            for r in ("J1", "J2")}
    half = 1.27
    for n in range(1, 41):
        (x1, y), (x2, _) = pads["J1"][str(n)], pads["J2"][str(n)]
        name = gen.HEADER[n]
        lane = y - half if n % 2 else y + half
        layer = pcbnew.F_Cu if n % 2 else pcbnew.B_Cu
        track(board, net(name), layer, [(x1, y), (x1 + half, lane), (x2 - half, lane), (x2, y)],
              POWER if name in SUPPLY else SIGNAL)
    for layer in (pcbnew.F_Cu, pcbnew.B_Cu):
        zone = pcbnew.ZONE(board)
        zone.SetLayer(layer)
        zone.SetNet(net("GND"))
        zone.SetZoneName("GND")
        outline = zone.Outline()
        outline.NewOutline()
        for x, y in ((0, 0), (W, 0), (W, H), (0, H)):
            outline.Append(pcbnew.FromMM(x), pcbnew.FromMM(y))
        zone.SetLocalClearance(pcbnew.FromMM(0.3))
        zone.SetMinThickness(pcbnew.FromMM(0.25))
        zone.SetPadConnection(pcbnew.ZONE_CONNECTION_THERMAL)
        zone.SetThermalReliefGap(pcbnew.FromMM(0.3))
        zone.SetThermalReliefSpokeWidth(pcbnew.FromMM(0.5))
        zone.SetIslandRemovalMode(pcbnew.ISLAND_REMOVAL_MODE_ALWAYS)
        board.Add(zone)                     # left unfilled: KiCad fills it on opening (B), and drc() on a copy
    return len(board.GetTracks())


def drc(path):
    """KiCad's design rule check on a copy of the board with its planes filled: what breaks a rule,
    and what is still to be joined. The board in the repository keeps its planes empty: filled, each
    regeneration would add hundreds of kilobytes to the history."""
    import json, shutil, subprocess, tempfile
    with tempfile.TemporaryDirectory() as tmp:
        copy = os.path.join(tmp, os.path.basename(path))
        shutil.copy(path, copy)
        report = os.path.join(tmp, "drc.json")
        subprocess.run([os.environ.get("KICAD_CLI", "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"), "pcb", "drc", "--refill-zones", "--save-board", "--severity-all",
                        "--format", "json", "-o", report, copy], capture_output=True, check=False)
        with open(report, encoding="utf-8") as f:
            found = json.load(f)
    kinds = {}
    for v in found.get("violations", []):
        kinds[v["type"]] = kinds.get(v["type"], 0) + 1
    return kinds, len(found.get("unconnected_items", []))


def paper(path, out):
    """The front as a sheet of paper shows it, at its real size: the outline, the lettering, every pad
    with its hole. Printed at 100 %, it is laid on the P4 and the parts are stood on it."""
    import subprocess
    subprocess.run([os.environ.get("KICAD_CLI", "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli"), "pcb",
                    "export", "svg", "--layers", "F.Mask,F.SilkS,Edge.Cuts", "--black-and-white",
                    "--exclude-drawing-sheet", "--page-size-mode", "2", "--drill-shape-opt", "2", "--mode-single",
                    "-o", out, path], capture_output=True, check=True)


def build(variant):
    board = pcbnew.BOARD()
    for a, b in (((0, 0), (W, 0)), ((W, 0), (W, H)), ((W, H), (0, H)), ((0, H), (0, 0))):
        seg = pcbnew.PCB_SHAPE(board)
        seg.SetShape(pcbnew.SHAPE_T_SEGMENT)
        seg.SetStart(mm(*a))
        seg.SetEnd(mm(*b))
        seg.SetLayer(pcbnew.Edge_Cuts)
        seg.SetWidth(pcbnew.FromMM(0.1))
        board.Add(seg)
    nets = {}

    def net(name):
        if name not in nets:
            nets[name] = pcbnew.NETINFO_ITEM(board, name)
            board.Add(nets[name])
        return nets[name]

    at = dict(AT, **(AT_SMD if variant == "smd" else {}))
    holes = iter(HOLES)
    missing = []
    for _, _, _, parts in gen.blocks(variant):
        for part, _, _ in parts:
            if not part.footprint:
                continue
            lib, name = part.footprint.split(":")
            fp = pcbnew.FootprintLoad(os.path.join(kisch.FOOTPRINTS, lib + ".pretty"), name)
            fp.SetReference(part.ref)
            fp.SetValue(part.value)
            board.Add(fp)
            if part.ref[0] in "JH":         # connectors, bridges and holes: named by what they are, or not at all
                fp.Reference().SetVisible(False)
            centred = name.startswith(("R_Axial", "C_Disc", "Fuse_Bourns"))    # set once the part is in its place, below
            if part.ref == "J1":            # underneath, its pins into the P4's socket
                fp.SetPosition(mm(*PIN1))
                fp.Flip(mm(*PIN1), pcbnew.FLIP_DIRECTION_LEFT_RIGHT)
                fp.SetOrientationDegrees(0)     # flipped and at 0: pin 2 inward, the rows going up, as the socket
            elif part.ref == "J2":          # the same socket again, beside it, pin for pin
                fp.SetPosition(mm(PIN1[0] + 7.62, PIN1[1]))
                fp.SetOrientationDegrees(180)
                for n in range(1, 40, 2):
                    px, py = xy(next(p for p in fp.Pads() if p.GetNumber() == str(n)).GetPosition())
                    text(board, "%s %s" % (GPIO[n], GPIO[n + 1]), px + 4.6, py, 0, 0.8, "l")
            elif part.ref.startswith("H"):
                fp.SetPosition(mm(*next(holes)))
            elif part.ref in at:
                x, y, rot = at[part.ref]
                fp.SetOrientationDegrees(rot)
                cx, cy = xy(fp.GetBoundingBox(False).GetCenter())
                px, py = xy(fp.GetPosition())
                fp.SetPosition(mm(px + x - cx, py + y - cy))
            else:
                missing.append(part.ref)
            sym_lib, sym = part.sym.split(":")
            pins = kisch.symbol_pins(kisch.lib_symbol(sym_lib, sym))
            for key, n in part.pins.items():
                for p in pins:
                    if str(key) in (p[0], p[1]) and n:
                        for pad in fp.Pads():
                            if pad.GetNumber() == p[0]:
                                pad.SetNet(net(n))
            bb = fp.GetBoundingBox(False)
            cx, cy = xy(bb.GetCenter())
            top, bot = pcbnew.ToMM(bb.GetTop()), pcbnew.ToMM(bb.GetBottom())
            if part.ref in REF:
                fp.Reference().SetPosition(mm(*REF[part.ref]))
            if centred:                     # a part that lies between its two holes: its reference under its
                ref = fp.Reference()        # body, where no neighbour's covers it; read while soldering
                ref.SetPosition(mm(cx, cy))
                ref.SetTextAngleDegrees(0)
                ref.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(0.8), pcbnew.FromMM(0.8)))
                ref.SetTextThickness(pcbnew.FromMM(0.15))
            if part.ref in PINS and not (variant == "smd" and part.ref == "J6"):
                pin_names(board, fp, PINS[part.ref], SIDE.get(part.ref, "E"))
            for k, line in enumerate(reversed(NAME.get(part.ref, []))):
                if cx < 6:                  # against the left edge: its name stands beside its pins' names
                    text(board, line, 9.3, cy, 90, 0.9)
                else:
                    text(board, line, cx, top - 0.9 - 1.3 * k, 0, 0.9)
            if part.ref in BRIDGE:
                if part.ref in ("JP3", "JP4"):
                    text(board, BRIDGE[part.ref], pcbnew.ToMM(bb.GetRight()) + 0.6, cy, 0, 0.8, "l")
                else:
                    text(board, BRIDGE[part.ref], cx, bot + 0.9, 0, 0.8)
    for s, x, y, angle, size in TEXT:
        text(board, s, x, y, angle, size)
    # where the Ethernet module lies over its sockets, the part past the board's edge left out, and
    # how far its RJ45 reaches
    x0, y0, x1, y1 = ETH_BOX
    xe = min(x1, W - 0.15)
    for a, b in (((xe, y0), (x0, y0)), ((x0, y0), (x0, y1)), ((x0, y1), (xe, y1))):
        seg = pcbnew.PCB_SHAPE(board)
        seg.SetShape(pcbnew.SHAPE_T_SEGMENT)
        seg.SetStart(mm(*a))
        seg.SetEnd(mm(*b))
        seg.SetLayer(pcbnew.F_SilkS)
        seg.SetWidth(pcbnew.FromMM(0.15))
        board.Add(seg)
    text(board, "P4OS AUX BOARD 0.1 " + variant.upper(), 17, 2.3, 0, 1.0)
    if missing:
        raise SystemExit("no place for " + ", ".join(missing))
    tracks = route(board, net)
    out = os.path.join(HERE, "aux-board-%s.kicad_pcb" % variant)
    pcbnew.SaveBoard(out, board)
    # the mask's colour lives in the stackup, which the scripting interface does not reach
    with open(out, encoding="utf-8") as f:
        doc = f.read()
    with open(out, "w", encoding="utf-8") as f:
        f.write(doc.replace("\t(setup\n", "\t(setup\n" + STACKUP, 1))
    print("wrote", os.path.relpath(out), "with", len(board.GetFootprints()), "footprints and", len(nets), "nets")
    if variant == "th":
        n = holes_svg(board, os.path.join(HERE, "fit-test-th.svg"))
        holes_svg(board, os.path.join(HERE, "fit-test-th-print.svg"), grow=0.4)
        print("  fit-test-th.svg and fit-test-th-print.svg (holes 0.4 mm wider):", n, "holes")
        paper(out, os.path.join(HERE, "paper-th.svg"))
        print("  paper-th.svg: the front at its real size, to print on paper")
    clash, off = overlaps(board)
    print("  parts that overlap:", ", ".join("%s/%s %.1f mm" % c for c in clash) or "none")
    print("  parts past the edge:", ", ".join(off) or "none")
    kinds, loose = drc(out)
    print("  routed: %d track segments and two ground planes; still to join: %d connections" % (tracks, loose))
    print("  design rules:", ", ".join("%s x%d" % k for k in sorted(kinds.items())) or "clean")
    return not clash and not off


if __name__ == "__main__":
    results = [build(v) for v in ("th", "smd")]
    if not all(results):
        raise SystemExit(1)
