# The auxiliary board for the 40-pin header

A board that plugs onto the header and takes each port of P4OS to a
connector with a name. The plan, the blocks and the two variants are in
[docs/plan/PCB.md](../../docs/plan/PCB.md) (Spanish); the pin map it follows
is [docs/img/pinout.svg](../../docs/img/pinout.svg).

**Where it stands (2026-11-12): a first schematic of both variants, a first
placement of their parts and the first tracks; nothing built. The outline
printed in plastic (`fit-test-th-print.svg`) was tried on the P4: the four
holes and the header fit.** Every part
value and footprint is a first choice to review against its datasheet.

- **th**, for a soldering iron: axial resistors, TO-92 transistors, every
  bridge a 2.54 mm jumper. The board is two layers, ordered bare.
- **smd**, for assembly at the factory: 0603 passives, SOT-23 transistors,
  solder bridges, a Qwiic socket and ESD protection on the USB lines. Its
  connectors are the same through-hole ones, soldered by hand afterwards.

RS485 and CAN are not on the board in either: their modules plug onto pin
headers (the automatic "RS485 to TTL" one on 3V3, TX, RX, GND; an
SN65HVD230 on 3V3, GND, CTX, CRX). Ethernet is a "W5500 Lite" module
(HW-884) on two sockets of six.

**There is no second 2x20 socket with the forty pins again** (taken out on
2026-11-11: nothing used it). Beside the header, in one column, there are
four pins of switched 5 V and ground, four of 3.3 V and ground, and the
three GPIO that no other connector of the board carries: 35 (a strapping
pin) and 37 and 38 (the console's UART). The silkscreen names the header's
forty pins beside their solder joints.

## The Ethernet module's place

The module that answered on the bench (`docs/plan/ETHERNET.md`) has its
pins and its chip on one face and the RJ45 on the other, so on its sockets
it lies chip down with the RJ45 on top, its mouth to the board's right
edge. The silkscreen draws where it lies.

- The two rows are 20.32 mm apart (`ETH_ROWS` in `place.py`), measured.
- Pin 1 of each socket (G and GND) is the pin nearest the RJ45's mouth,
  as on the module. The row G, G, MO, SCK, CS, INT is the upper one.
- The first pin is 7.5 mm from the module's edge on the RJ45 side
  (`ETH_PIN1`), behind the module's two mounting holes: with it the
  module's edge is on the board's and the RJ45 sticks out past it. Found
  with the printed plate on 2026-11-12: the first guess, 2.5 mm read off
  photographs, left the sockets 5 mm too far out.
- Under the module there are 8.5 mm (the sockets' height): a resistor, a
  small capacitor or SMD parts fit, a pin header or an electrolytic do
  not. `place.py` refuses a tall part there.

| File | What it is |
|---|---|
| `gen.py` | the board as a list: its parts and the net on each pin, for the two variants |
| `kisch.py` | writes a KiCad schematic from that list, with KiCad's own symbols |
| `aux-board-th.kicad_sch`, `aux-board-smd.kicad_sch` | the schematics, generated: open them in KiCad 10 |
| `place.py` | puts those parts on a board with the P4's outline and holes, with their nets and the silkscreen; it runs under KiCad's Python |
| `aux-board-th.kicad_pcb`, `aux-board-smd.kicad_pcb` | those boards, generated: drafts to look at and to move, with the first tracks (below) |
| `paper-th.svg` | the through-hole board's front at its real size, outline, lettering and pads: printed on paper at 100 %, it is laid on the P4 and the parts are stood on it |
| `fit-test-th.svg`, `fit-test-th-print.svg` | the through-hole board's outline with every hole, 1 unit = 1 mm: a slicer extrudes it into a plate to try the fit on the P4. The second has each hole 0.4 mm wider, for a printer that closes small holes |

| Through-hole | SMD |
|---|---|
| <img src="draft-th-3d.jpg" width="400" alt="A 3D render of the through-hole draft on a black board: the header's forty solder joints down the left edge with the GPIO number of each pin beside them and three small headers for 5 V, 3.3 V and spare GPIO, the I2C headers above, two USB-A sockets on the bottom edge, two screw terminals on the right, pin headers for UART A / RS485, CAN, UART B, SWD and SPI in the middle with each pin named, two sockets for the W5500 module, the infrared receiver and LED at the top"> | <img src="draft-smd-3d.jpg" width="400" alt="The same board with surface-mount parts: small resistors, SOT-23 transistors, solder bridges and a Qwiic socket"> |

*The drafts as KiCad renders them, black as they would be ordered. The
board is the size of the P4's own and sits on its four holes. The
silkscreen names every connector, every pin and every bridge; where each
part goes is a first guess. `place.py` refuses a placement where two parts
overlap or one leaves the board (the USB sockets overhang on purpose).*

## The tracks so far

`place.py` lays what needs no choice, and lays it again every time it runs:

- **The header's pins that go to the column beside it:** 3.3 V from pin
  18 straight across on the back (0.8 mm), GPIO35 from pin 30 straight
  across on the front, GPIO38 and GPIO37 from pins 9 and 7 round their
  neighbours and up two lanes (0.3 mm).
- **A ground plane on each face.** The files keep the planes unfilled
  (filled, each file is more than twice the size): KiCad fills them with B.

It then asks KiCad for its design rule check on a copy with the planes
filled, and prints what is left:

    routed: 15 track segments and two ground planes; still to join: 113 connections
    design rules: silk_edge_clearance x4

Ground is joined everywhere by the planes; 113 connections of the other
53 nets are still to route (123 on the SMD variant). The four warnings are
the USB sockets' outlines, which overhang the edge on purpose.

**The boards are generated, so a track drawn by hand in KiCad is lost the
next time `place.py` runs.** While parts are still moving, tracks go in
the script; routing by hand starts once the placement is frozen (the
measures above, the fit test on the P4), and from then on `place.py` is
not run again.

```
python3 hw/aux-board/gen.py --check
```

writes the schematic and asks KiCad (`kicad-cli`) for its ERC and its
netlist, and compares each netlist with the list: 73 parts (th) or 75
(smd), the same 54 nets, no violation, none different.

The schematic has no wires: each pin carries a label with its net's name,
and pins with the same name are joined. Change the list and generate again
rather than editing the `.kicad_sch` by hand, until the layout starts.

A KiCad that was never opened has no library tables yet, and its ERC warns
about that for every part; the check leaves those warnings out. Opening
KiCad once and accepting the default libraries makes them go away.
