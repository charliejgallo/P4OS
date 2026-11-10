#!/usr/bin/env python3
"""A first placement of the through-hole variant: the parts of gen.py on a board, with their nets.

    <KiCad's python3> hw/aux-board/place.py          writes aux-board-th.kicad_pcb

It runs under KiCad's own Python (it needs pcbnew):
/Applications/KiCad/KiCad.app/Contents/Frameworks/Python.framework/Versions/Current/bin/python3

The board has the outline and the four holes of the P4's own board, as
measured on Waveshare's 3D model (docs/plan/PCB.md, "Mecánica"), seen from
the back of the P4: x to the right, y down, the USB-C connectors along the
top edge, the 40-pin header down the left edge with pin 1 at its bottom.
Nothing is routed. This is a draft to look at and to move by hand.
"""
import os

import pcbnew

import gen
import kisch

HERE = os.path.dirname(os.path.abspath(__file__))
W, H = 64.51, 118.49                    # the P4's board
HOLES = [(3.75, 3.24), (60.75, 3.24), (3.75, 115.24), (60.75, 115.24)]
PIN1 = (3.15, 104.32)                   # the header's pin 1; odd pins on the edge side, rows going up

# The W5500 Lite: where the middle of its two rows of pins goes, and how far apart the rows are.
# 20.32 mm (0.8 in) is a guess from the board's 23.5 mm width: measure the module before routing.
ETH, ETH_ROWS = (47.5, 78.3), 20.32

# ref -> (x, y, degrees): the middle of the part, mm from the top left corner
AT = {
    # i2c, down the left edge above the header
    "J5": (3, 13, 0), "J6": (3, 25, 0), "J7": (3, 37, 0), "J8": (3, 49, 0),
    "R4": (16, 10, 0), "R5": (16, 14, 0), "JP2": (25, 12, 0),
    "Q3": (13, 33, 0), "Q4": (13, 39, 0), "R6": (24, 33, 0), "R7": (24, 39, 0),
    "JP3": (11, 46, 90), "JP4": (11, 51, 90),
    # infrared, looking out of the top edge
    "U3": (32, 4, 180), "D1": (42, 4, 0), "R13": (32, 11, 0), "C8": (27, 18, 0), "JP13": (36, 19, 90),
    "R16": (53, 10, 0), "Q5": (44, 12, 0), "R14": (50, 17, 0), "R15": (50, 21, 0), "JP14": (48, 27, 90),
    # 1-wire and the strip, on the right edge
    "J15": (58.5, 38, 90), "R17": (46, 34, 0), "JP15": (46, 39, 90),
    "J16": (58.5, 58, 90), "R18": (48, 52, 0), "C9": (50, 61, 0),
    # the ports, in the middle
    "J9": (19, 64, 0), "J13": (25, 63, 0), "J11": (31, 66.5, 0), "J12": (36.5, 64, 0),
    "R10": (26, 80, 0), "R11": (26, 84, 0), "JP9": (35, 81, 0),
    "J14": (27, 50, 90),
    # ethernet: the module lies over ETH, its RJ45 past the right edge; two low parts go under it
    "J17": (ETH[0], ETH[1] - ETH_ROWS / 2, 90), "J18": (ETH[0], ETH[1] + ETH_ROWS / 2, 90),
    "R19": (ETH[0], ETH[1] - 3, 0), "C10": (ETH[0], ETH[1] + 3, 0),
    "JP16": (37.5, 50, 0), "J19": (44, 57.5, 90), "C11": (39.5, 43.5, 0),
    # the 5 V switch and the USB sockets, at the bottom
    "Q1": (21, 91, 0), "R1": (32.5, 88, 0), "R2": (32.5, 92, 0), "R3": (32.5, 96, 0), "Q2": (44, 94.5, 0),
    "JP1": (52, 94.5, 90), "C1": (60, 96, 0), "C2": (50, 99.5, 0),
    "F1": (20, 99, 0), "C3": (30, 101, 0), "F2": (42, 101, 0), "C4": (57, 104, 0),
    "J3": (24, 111.2, 0), "J4": (46, 111.2, 0),
}
SILK = {"J5": "I2C 3V3", "J6": "I2C 3V3", "J7": "I2C 5V", "J8": "BOARD I2C", "J9": "RS485", "J13": "CAN",
        "J11": "UART B", "J12": "SWD", "J14": "SPI", "J15": "1-WIRE", "J16": "LED / SERVO", "J3": "USB 2.0",
        "J4": "USB 1.1", "U3": "IR", "J2": "P4 PINS", "J17": "W5500"}


def mm(x, y):
    return pcbnew.VECTOR2I(pcbnew.FromMM(x), pcbnew.FromMM(y))


def middle(fp):
    bb = fp.GetBoundingBox(False)
    return pcbnew.ToMM(bb.GetCenter().x), pcbnew.ToMM(bb.GetCenter().y)


def main():
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

    holes = iter(HOLES)
    missing = []
    for _, _, _, parts in gen.blocks():
        for part, _, _ in parts:
            if not part.footprint:
                continue
            lib, name = part.footprint.split(":")
            fp = pcbnew.FootprintLoad(os.path.join(kisch.FOOTPRINTS, lib + ".pretty"), name)
            fp.SetReference(part.ref)
            fp.SetValue(part.value)
            board.Add(fp)
            if part.ref == "J1":            # underneath, its pins into the P4's socket
                fp.SetPosition(mm(*PIN1))
                fp.Flip(mm(*PIN1), pcbnew.FLIP_DIRECTION_LEFT_RIGHT)
                fp.SetOrientationDegrees(0)     # flipped and at 0: pin 2 inward, the rows going up, as the socket
            elif part.ref == "J2":          # the same socket again, beside it, pin for pin
                fp.SetPosition(mm(PIN1[0] + 8.2, PIN1[1]))
                fp.SetOrientationDegrees(180)
            elif part.ref.startswith("H"):
                fp.SetPosition(mm(*next(holes)))
            elif part.ref in AT:
                x, y, rot = AT[part.ref]
                fp.SetOrientationDegrees(rot)
                cx, cy = middle(fp)
                px, py = pcbnew.ToMM(fp.GetPosition().x), pcbnew.ToMM(fp.GetPosition().y)
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
            if part.ref in SILK:
                t = pcbnew.PCB_TEXT(board)
                t.SetText(SILK[part.ref])
                t.SetLayer(pcbnew.F_SilkS)
                t.SetTextSize(pcbnew.VECTOR2I(pcbnew.FromMM(1.2), pcbnew.FromMM(1.2)))
                t.SetTextThickness(pcbnew.FromMM(0.2))
                bb = fp.GetBoundingBox(False)
                wide = bb.GetWidth() >= bb.GetHeight()
                if wide:
                    t.SetPosition(pcbnew.VECTOR2I(bb.GetCenter().x, bb.GetTop() - pcbnew.FromMM(1.4)))
                else:
                    t.SetTextAngleDegrees(90)
                    t.SetPosition(pcbnew.VECTOR2I(bb.GetRight() + pcbnew.FromMM(1.4), bb.GetCenter().y))
                board.Add(t)
    if missing:
        raise SystemExit("no place for " + ", ".join(missing))
    out = os.path.join(HERE, "aux-board-th.kicad_pcb")
    pcbnew.SaveBoard(out, board)
    print("wrote", os.path.relpath(out), "with", len(board.GetFootprints()), "footprints and", len(nets), "nets")


if __name__ == "__main__":
    main()
