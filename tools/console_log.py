#!/usr/bin/env python3
"""Records the board's serial console (the CH340 on the USB-C marked UART) to a file.

    tools/console_log.py /dev/cu.wchusbserial<n> out.txt 1200

What the portal cannot show: the board with no network at all (away from
its Wi-Fi, a saved network that does not exist), a boot that restarts over
and over, a panic's text. It is how the restart loop away from home was
seen on 2026-10-09 (docs/MEMORY.md).

OPENING THE PORT RESTARTS THE BOARD, even with DTR and RTS held off before
opening, as here: on this board the CH340 lines reach EN on open. Open it
first and do what is to be watched afterwards; and on a board in use, ask
first. Needs pyserial: ESP-IDF's Python has it
(~/.espressif/python_env/*/bin/python tools/console_log.py ...).
"""
import sys
import time

import serial

port, out, secs = sys.argv[1], sys.argv[2], float(sys.argv[3]) if len(sys.argv) > 3 else 600
s = serial.Serial()
s.port = port
s.baudrate = 115200
s.timeout = 0.5
s.dtr = False       # before open: the auto-reset circuit must see no change (it restarts anyway)
s.rts = False
s.open()
end = time.time() + secs
with open(out, "ab") as f:
    while time.time() < end:
        b = s.read(4096)
        if b:
            f.write(b)
            f.flush()
s.close()
