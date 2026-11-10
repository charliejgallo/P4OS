# The auxiliary board for the 40-pin header

A board that plugs onto the header and takes each port of P4OS to a
connector with a name. The plan, the blocks and the two variants are in
[docs/plan/PCB.md](../../docs/plan/PCB.md) (Spanish); the pin map it follows
is [docs/img/pinout.svg](../../docs/img/pinout.svg).

**Where it stands (2026-11-10): a first schematic of the through-hole
variant, nothing laid out, nothing built.** Every part value and footprint
is a first choice to review against its datasheet.

| File | What it is |
|---|---|
| `gen.py` | the board as a list: its parts and the net on each pin |
| `kisch.py` | writes a KiCad schematic from that list, with KiCad's own symbols |
| `aux-board-th.kicad_sch` | the through-hole variant, generated: open it in KiCad 10 |

```
python3 hw/aux-board/gen.py --check
```

writes the schematic and asks KiCad (`kicad-cli`) for its ERC and its
netlist, and compares that netlist with the list: 80 parts, 64 nets, no
violation, none different.

The schematic has no wires: each pin carries a label with its net's name,
and pins with the same name are joined. Change the list and generate again
rather than editing the `.kicad_sch` by hand, until the layout starts.

A KiCad that was never opened has no library tables yet, and its ERC warns
about that for every part; the check leaves those warnings out. Opening
KiCad once and accepting the default libraries makes them go away.
