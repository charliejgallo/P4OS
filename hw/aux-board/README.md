# The auxiliary board for the 40-pin header

A board that plugs onto the header and takes each port of P4OS to a
connector with a name. The plan, the blocks and the two variants are in
[docs/plan/PCB.md](../../docs/plan/PCB.md) (Spanish); the pin map it follows
is [docs/img/pinout.svg](../../docs/img/pinout.svg).

**Where it stands (2026-11-10): a first schematic of the through-hole
variant and a first placement of its parts, nothing routed, nothing
built.** Every part value and footprint
is a first choice to review against its datasheet. RS485 and CAN are not on
the board: their modules plug onto pin headers (the automatic "RS485 to
TTL" one on 3V3, TX, RX, GND; an SN65HVD230 on 3V3, GND, CTX, CRX).

| File | What it is |
|---|---|
| `gen.py` | the board as a list: its parts and the net on each pin |
| `kisch.py` | writes a KiCad schematic from that list, with KiCad's own symbols |
| `aux-board-th.kicad_sch` | the through-hole variant, generated: open it in KiCad 10 |
| `place.py` | puts those parts on a board with the P4's outline and holes, with their nets; it runs under KiCad's Python |
| `aux-board-th.kicad_pcb` | that board, generated: a draft to look at and to move, with no tracks |

<img src="draft-th-3d.jpg" width="520" alt="A 3D render of the draft: the 2x20 socket down the left edge, the I2C headers above it, two USB-A sockets on the bottom edge, two screw terminals on the right, pin headers for RS485, CAN, uart.b, SWD and SPI in the middle, the infrared receiver and LED at the top">

*The draft as KiCad renders it. The board is the size of the P4's own and
sits on its four holes; where each part goes is a first guess.*

```
python3 hw/aux-board/gen.py --check
```

writes the schematic and asks KiCad (`kicad-cli`) for its ERC and its
netlist, and compares that netlist with the list: 64 parts, 51 nets, no
violation, none different.

The schematic has no wires: each pin carries a label with its net's name,
and pins with the same name are joined. Change the list and generate again
rather than editing the `.kicad_sch` by hand, until the layout starts.

A KiCad that was never opened has no library tables yet, and its ERC warns
about that for every part; the check leaves those warnings out. Opening
KiCad once and accepting the default libraries makes them go away.
