#!/usr/bin/env python3
"""The auxiliary board for the 40-pin header: its parts and nets, as a KiCad schematic.

    python3 hw/aux-board/gen.py            writes aux-board-th.kicad_sch
    python3 hw/aux-board/gen.py --check    and asks KiCad for its ERC and its netlist

docs/plan/PCB.md is the plan and docs/img/pinout.svg the pin map this follows.
This is the through-hole variant ("th"): every bridge a 2.54 mm jumper, the
parts a soldering iron can do. The SMD variant for assembly will be a second
list here over the same nets.

RS485 and CAN are not on the board: their transceivers are the modules of
the bench (an automatic "RS485 to TTL" one, an SN65HVD230), each on a pin
header in the module's own order. What is on the board reaches its GPIO
through a jumper, so a pin can be given back, and the pass-through socket J2
carries all forty pins whatever the jumpers say.
"""
import collections
import datetime
import os
import subprocess
import sys
import tempfile

import kisch
from kisch import Part, Sheet

HERE = os.path.dirname(os.path.abspath(__file__))

# The header, pin -> net (docs/img/pinout.svg). Pin 1 is 5 V.
HEADER = {
    1: "+5V", 2: "+3V3", 3: "+5V", 4: "SDA_BOARD", 5: "GND", 6: "SCL_BOARD",
    7: "GPIO37_CONSOLE", 8: "TGT_BOOT", 9: "GPIO38_CONSOLE", 10: "GND",
    11: "TGT_EN", 12: "UARTB_TX", 13: "GND", 14: "UARTB_RX", 15: "SDA_EXT", 16: "GPIO28",
    17: "SCL_EXT", 18: "+3V3", 19: "GND", 20: "UARTA_DE", 21: "USB_FS_DM", 22: "UARTA_TX",
    23: "USB_FS_DP", 24: "UARTA_RX", 25: "USB_HS_DM", 26: "GND", 27: "USB_HS_DP", 28: "GPIO34",
    29: "GND", 30: "GPIO35_BOOT", 31: "GPIO32", 32: "SPI_CS", 33: "GND", 34: "SPI_MOSI",
    35: "GPIO46", 36: "SPI_MISO", 37: "GPIO47", 38: "SPI_SCK", 39: "GPIO48", 40: "GND",
}

HDR = "Connector_PinHeader_2.54mm:PinHeader_1x%02d_P2.54mm_Vertical"
R_FP = "Resistor_THT:R_Axial_DIN0207_L6.3mm_D2.5mm_P10.16mm_Horizontal"
C_FP = "Capacitor_THT:C_Disc_D5.0mm_W2.5mm_P5.00mm"
CP_FP = "Capacitor_THT:CP_Radial_D6.3mm_P2.50mm"
TO92 = "Package_TO_SOT_THT:TO-92_Inline"
SCREW3 = "TerminalBlock_Phoenix:TerminalBlock_Phoenix_MKDS-1,5-3-5.08_1x03_P5.08mm_Horizontal"
USB_FP = "Connector_USB:USB_A_Molex_67643_Horizontal"
PTC_FP = "Fuse:Fuse_Bourns_MF-RG300"


def R(ref, value, a, b):
    return Part(ref, "Device:R", value, R_FP, {1: a, 2: b})


def C(ref, value, a, b):
    return Part(ref, "Device:C", value, C_FP, {1: a, 2: b})


def CP(ref, value, plus, minus, fp=CP_FP, dnp=False):
    return Part(ref, "Device:C_Polarized", value, fp, {1: plus, 2: minus}, dnp=dnp)


def conn(ref, value, nets):
    return Part(ref, "Connector_Generic:Conn_01x%02d" % len(nets), value, HDR % len(nets),
                {i + 1: n for i, n in enumerate(nets)})


def screw(ref, value, nets):
    return Part(ref, "Connector:Screw_Terminal_01x03", value, SCREW3, {i + 1: n for i, n in enumerate(nets)})


def bridge(ref, value, a, b, closed=True):
    """A two-pin header with a jumper on it (closed) or left bare (open)."""
    return Part(ref, "Jumper:Jumper_2_Bridged" if closed else "Jumper:Jumper_2_Open", value, HDR % 2, {1: a, 2: b})


def flag(ref, net):
    return Part(ref, "power:PWR_FLAG", "PWR_FLAG", "", {1: net})


def blocks():
    """[(title, x, y, [(part, dx, dy)])]: a block's parts, placed from its corner."""
    b = []
    b.append(("THE HEADER: J1's pins go into the board's socket, J2 is the same socket again on top", 25, 25, [
        (Part("J1", "Connector_Generic:Conn_02x20_Odd_Even", "to the P4 (2x20 pins, underneath)",
              "Connector_PinHeader_2.54mm:PinHeader_2x20_P2.54mm_Vertical", HEADER), 45, 60),
        (Part("J2", "Connector_Generic:Conn_02x20_Odd_Even", "pass-through (2x20 socket)",
              "Connector_PinSocket_2.54mm:PinSocket_2x20_P2.54mm_Vertical", HEADER), 125, 60),
    ]))
    b.append(("5 V, SWITCHED BY THE 3V3 RAIL: off with the board, JP1 to bypass", 205, 25, [
        (Part("Q1", "Transistor_FET:IRF9540N", "logic-level P-FET, TO-220 (NDP6020P)",
              "Package_TO_SOT_THT:TO-220-3_Vertical", {"S": "+5V", "D": "+5V_SW", "G": "SW_GATE"}), 20, 40),
        (R("R1", "100k", "+5V", "SW_GATE"), 55, 40),
        (Part("Q2", "Transistor_BJT:BC337", "BC337", TO92, {"C": "SW_GATE", "B": "SW_BASE", "E": "GND"}), 85, 40),
        (R("R2", "10k", "+3V3", "SW_BASE"), 115, 40),
        (R("R3", "100k", "SW_BASE", "GND"), 135, 40),
        (bridge("JP1", "bypass", "+5V", "+5V_SW", closed=False), 165, 40),
        (CP("C1", "100u", "+5V_SW", "GND"), 195, 40),
        (C("C2", "100n", "+5V_SW", "GND"), 210, 40),
        (flag("FLG1", "+5V"), 20, 78), (flag("FLG2", "+3V3"), 45, 78), (flag("FLG3", "GND"), 70, 78),
        (flag("FLG4", "+5V_SW"), 95, 78), (flag("FLG5", "VBUS_HS"), 125, 78), (flag("FLG6", "VBUS_FS"), 155, 78),
        (flag("FLG7", "IR_VS"), 185, 78),
    ]))
    b.append(("USB HOST: J3 High Speed (pins 25/27), J4 Full Speed (pins 21/23)", 205, 125, [
        (Part("J3", "Connector:USB_A", "USB 2.0", USB_FP,
              {"VBUS": "VBUS_HS", "D-": "USB_HS_DM", "D+": "USB_HS_DP", "GND": "GND", "Shield": "GND"}), 20, 40),
        (Part("F1", "Device:Polyfuse", "500 mA", PTC_FP, {1: "+5V_SW", 2: "VBUS_HS"}), 60, 40),
        (CP("C3", "100u", "VBUS_HS", "GND"), 80, 40),
        (Part("J4", "Connector:USB_A", "USB 1.1", USB_FP,
              {"VBUS": "VBUS_FS", "D-": "USB_FS_DM", "D+": "USB_FS_DP", "GND": "GND", "Shield": "GND"}), 120, 40),
        (Part("F2", "Device:Polyfuse", "500 mA", PTC_FP, {1: "+5V_SW", 2: "VBUS_FS"}), 160, 40),
        (CP("C4", "100u", "VBUS_FS", "GND"), 180, 40),
    ]))
    b.append(("I2C: i2c.ext at 3.3 V and, through Q3/Q4, at 5 V; i2c.board at 3.3 V only", 435, 25, [
        (conn("J5", "i2c.ext 3V3", ["GND", "+3V3", "SDA_EXT", "SCL_EXT"]), 25, 35),
        (conn("J6", "i2c.ext 3V3", ["GND", "+3V3", "SDA_EXT", "SCL_EXT"]), 60, 35),
        (R("R4", "4k7", "PU_EXT", "SDA_EXT"), 85, 35),
        (R("R5", "4k7", "PU_EXT", "SCL_EXT"), 105, 35),
        (bridge("JP2", "pull-ups", "+3V3", "PU_EXT"), 130, 35),
        (Part("Q3", "Transistor_FET:2N7000", "2N7000", TO92, {"G": "+3V3", "S": "SDA_EXT", "D": "SDA_5V"}), 20, 85),
        (Part("Q4", "Transistor_FET:2N7000", "2N7000", TO92, {"G": "+3V3", "S": "SCL_EXT", "D": "SCL_5V"}), 55, 85),
        (R("R6", "4k7", "+5V_SW", "SDA_5V"), 85, 85),
        (R("R7", "4k7", "+5V_SW", "SCL_5V"), 105, 85),
        (conn("J7", "i2c.ext 5V", ["GND", "+5V_SW", "SDA_5V", "SCL_5V"]), 135, 85),
        (conn("J8", "i2c.board 3V3", ["GND", "+3V3", "SDA_BOARD_J", "SCL_BOARD_J"]), 25, 135),
        (bridge("JP3", "SDA", "SDA_BOARD", "SDA_BOARD_J"), 70, 135),
        (bridge("JP4", "SCL", "SCL_BOARD", "SCL_BOARD_J"), 115, 135),
    ]))
    b.append(("uart.a: an automatic RS485 module plugs on the first four pins (3V3 TX RX GND); DE is the fifth", 25, 150, [
        (conn("J9", "uart.a / RS485 module", ["+3V3", "UARTA_TX", "UARTA_RX", "GND", "UARTA_DE"]), 35, 45),
    ]))
    b.append(("uart.b, THE FLASHER'S TARGET, AND SWD ON THE SAME PINS", 25, 245, [
        (conn("J11", "uart.b + target", ["GND", "+3V3", "UARTB_TX", "UARTB_RX", "TGT_EN", "TGT_BOOT", "RX_FROM_5V"]), 35, 45),
        # a 5 V target's TX comes in on pin 7 through this divider; JP9 joins it to RX only then,
        # so SWD and a fast UART never see the resistors
        (R("R10", "10k", "RX_FROM_5V", "UARTB_RX_DIV"), 75, 45),
        (R("R11", "20k", "UARTB_RX_DIV", "GND"), 95, 45),
        (bridge("JP9", "5 V target", "UARTB_RX_DIV", "UARTB_RX", closed=False), 125, 45),
        (conn("J12", "SWD", ["+3V3", "UARTB_RX", "UARTB_TX", "TGT_EN", "GND"]), 175, 45),
    ]))
    b.append(("CAN: an SN65HVD230 module on four pins (3V3 GND CTX CRX)", 205, 245, [
        (conn("J13", "CAN module", ["+3V3", "GND", "GPIO46", "GPIO47"]), 40, 45),
    ]))
    b.append(("spi.a, AND GPIO46/47 WHEN NO CAN MODULE IS ON THEM", 435, 200, [
        (Part("J14", "Connector_Generic:Conn_02x04_Odd_Even", "spi.a",
              "Connector_PinHeader_2.54mm:PinHeader_2x04_P2.54mm_Vertical",
              {1: "+3V3", 2: "GND", 3: "SPI_SCK", 4: "SPI_MOSI", 5: "SPI_MISO", 6: "SPI_CS", 7: "GPIO46", 8: "GPIO47"}), 40, 35),
    ]))
    # The "W5500 Lite" / "Mini W5500" (HW-884): 28.5 x 23.5 mm, six pins down each long side, 3.3 V
    # only, about 200 mA. Its pins as printed, RJ45 end first: left G G MO SCK CS INT, right GND V V
    # NC RST MI. It is spi.a's chip while JP16 is on; its interrupt is not wired to a GPIO (the driver
    # can poll), INT and RST come out on J19.
    b.append(("ETHERNET: a W5500 Lite module on two sockets, spi.a's chip select through JP16", 435, 330, [
        (Part("J17", "Connector_Generic:Conn_01x06", "W5500 left: G G MO SCK CS INT",
              "Connector_PinSocket_2.54mm:PinSocket_1x06_P2.54mm_Vertical",
              {1: "GND", 2: "GND", 3: "SPI_MOSI", 4: "SPI_SCK", 5: "ETH_CS", 6: "ETH_INT"}), 25, 35),
        (Part("J18", "Connector_Generic:Conn_01x06", "W5500 right: GND V V NC RST MI",
              "Connector_PinSocket_2.54mm:PinSocket_1x06_P2.54mm_Vertical",
              {1: "GND", 2: "+3V3", 3: "+3V3", 5: "ETH_RST", 6: "SPI_MISO"}), 65, 35),
        (bridge("JP16", "chip select", "SPI_CS", "ETH_CS"), 100, 30),
        (conn("J19", "INT RST", ["ETH_INT", "ETH_RST"]), 100, 50),
        (R("R19", "10k", "+3V3", "ETH_RST"), 125, 35),
        (C("C10", "100n", "+3V3", "GND"), 140, 35),
        (CP("C11", "100u", "+3V3", "GND"), 152, 35),
    ]))
    b.append(("INFRARED: the receiver on GPIO28, the LED on GPIO32 through Q5", 435, 255, [
        (Part("U3", "Interface_Optical:TSOP382xx", "TSOP38238", "OptoDevice:Vishay_MINICAST-3Pin",
              {"OUT": "IR_OUT", "GND": "GND", "Vs": "IR_VS"}), 15, 40),
        (R("R13", "100", "+3V3", "IR_VS"), 55, 40),
        (CP("C8", "4u7", "IR_VS", "GND"), 75, 40),
        (bridge("JP13", "receiver", "GPIO28", "IR_OUT"), 110, 40),
        (bridge("JP14", "LED", "GPIO32", "IR_DRV"), 20, 90),
        (R("R14", "1k", "IR_DRV", "IR_BASE"), 50, 90),
        (R("R15", "10k", "IR_BASE", "GND"), 68, 90),
        (Part("Q5", "Transistor_BJT:BC337", "BC337", TO92, {"C": "IR_K", "B": "IR_BASE", "E": "GND"}), 92, 90),
        (Part("D1", "Device:LED", "IR 940 nm", "LED_THT:LED_D5.0mm", {"K": "IR_K", "A": "IR_A"}), 120, 90),
        (R("R16", "47", "+5V_SW", "IR_A"), 143, 90),
    ]))
    b.append(("1-WIRE ON GPIO34, A LED STRIP OR A SERVO ON GPIO48, THE HOLES", 25, 335, [
        (screw("J15", "1-Wire: 3V3 DQ GND", ["+3V3", "GPIO34", "GND"]), 30, 35),
        (R("R17", "4k7", "OW_PU", "GPIO34"), 60, 35),
        (bridge("JP15", "pull-up", "+3V3", "OW_PU"), 90, 35),
        (screw("J16", "strip: 5V DIN GND", ["+5V_SW", "LED_DATA", "GND"]), 140, 35),
        (R("R18", "330", "GPIO48", "LED_DATA"), 170, 35),
        (CP("C9", "470u", "+5V_SW", "GND", fp="Capacitor_THT:CP_Radial_D8.0mm_P3.50mm"), 190, 35),
    ] + [(Part("H%d" % k, "Mechanical:MountingHole", "M2.5", "MountingHole:MountingHole_2.7mm_M2.5", {}), 230 + 20 * k, 35)
         for k in range(1, 5)]))
    return b


def check(sch):
    """KiCad's own word on the file: its ERC, and its netlist against the list above."""
    cli = os.environ.get("KICAD_CLI", "/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli")
    with tempfile.TemporaryDirectory() as tmp:
        rpt, net = os.path.join(tmp, "erc.rpt"), os.path.join(tmp, "out.net")
        subprocess.run([cli, "sch", "erc", "--severity-all", "-o", rpt, sch], check=True, capture_output=True)
        subprocess.run([cli, "sch", "export", "netlist", "-o", net, sch], check=True, capture_output=True)
        # a KiCad that was never opened has no library tables and says so for every part: not this file's fault
        found = [b.split("\n")[0] for b in open(rpt, encoding="utf-8").read().split("\n[")[1:]
                 if not b.startswith(("footprint_link_issues", "lib_symbol_issues"))]
        tree = kisch.parse(open(net, encoding="utf-8").read())
    got = collections.defaultdict(set)
    for n in next(e for e in tree if isinstance(e, list) and e[0] == "nets")[1:]:
        name = next(a for a in n if isinstance(a, list) and a[0] == "name")[1].lstrip("/")
        if name.startswith("unconnected-"):         # KiCad's own name for a pin marked no-connect
            continue
        for a in n:
            if isinstance(a, list) and a[0] == "node":
                ref = next(x for x in a if isinstance(x, list) and x[0] == "ref")[1]
                if not ref.startswith("FLG"):
                    got[name].add((ref, str(next(x for x in a if isinstance(x, list) and x[0] == "pin")[1])))
    want = collections.defaultdict(set)
    for _, _, _, parts in blocks():
        for part, _, _ in parts:
            lib, name = part.sym.split(":")
            for key, n in part.pins.items():
                for p in kisch.symbol_pins(kisch.lib_symbol(lib, name)):
                    if str(key) in (p[0], p[1]) and n and not part.ref.startswith("FLG"):
                        want[n].add((part.ref, p[0]))
    wrong = [n for n in sorted(set(want) | set(got)) if want.get(n) != got.get(n)]
    lonely = [n for n, v in want.items() if len(v) < 2]
    print("ERC: %d violations%s" % (len(found), "".join("\n  [" + f for f in found)))
    print("netlist: %d nets, %d differ from the list%s" % (len(got), len(wrong), " " + str(wrong) if wrong else ""))
    print("nets with a single pin:", lonely or "none")
    return not (found or wrong or lonely)


def main():
    sh = Sheet("aux-board-th", "P4OS auxiliary board for the 40-pin header, through-hole", "0.1")
    for title, bx, by, parts in blocks():
        sh.text(bx, by, title)
        for part, dx, dy in parts:
            # on the 2.54 mm grid, where KiCad's pins are
            sh.place(part, round((bx + dx) / 2.54) * 2.54, round((by + dy) / 2.54) * 2.54)
    out = os.path.join(HERE, "aux-board-th.kicad_sch")
    sh.write(out, datetime.date.today().isoformat())
    print("wrote", os.path.relpath(out), "with", len(sh.refs), "parts")
    if "--check" in sys.argv and not check(out):
        sys.exit(1)


if __name__ == "__main__":
    main()
