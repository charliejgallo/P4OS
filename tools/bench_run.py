#!/usr/bin/env python3
"""
Drive p4bench from the Mac and keep what it measures.

    tools/bench_run.py first                 # the unattended first-day suite
    tools/bench_run.py cmd "lcd rate" "psram"
    tools/bench_run.py --http p4bench.local cmd "info"
    tools/bench_run.py monitor               # just print the console

Over serial it talks to the "UART" USB-C port (CH343, VID 1A86 PID 55D3),
opening it with DTR/RTS low so the auto-reset circuit does not reboot the
board. Over --http it uses /api/cmd once the bench is on Wi-Fi.

Every BENCH line goes to bench/results/raw/<date>.log and the suite's lines to
bench/results/<date>-<suite>.txt, which is what HARDWARE.md gets filled from.
"""
import argparse
import datetime
import os
import sys
import time
import urllib.parse
import urllib.request

# pyserial lives in the ESP-IDF Python environment; re-run there if needed.
try:
    import serial  # noqa: F401
except ImportError:
    import glob
    if not os.environ.get("BENCH_RUN_REEXEC"):
        os.environ["BENCH_RUN_REEXEC"] = "1"
        for py in sorted(glob.glob(os.path.expanduser("~/.espressif/python_env/*/bin/python"))):
            os.execv(py, [py] + sys.argv)
    sys.exit("pyserial missing: pip install pyserial")

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(ROOT, "bench", "results")

# Commands that need nobody at the board, with their timeout in seconds.
SUITES = {
    "first": [
        ("info", 5), ("psram", 20), ("temp", 5),
        ("lcd info", 5), ("lcd rate", 8), ("lcd pattern 3000", 8), ("lcd bars 4000", 8),
        ("lcd fill", 30), ("lcd tear 3", 15),
        ("touch info", 5), ("touch rst", 5),
        ("i2c scan", 10),
        ("sd info", 10), ("sd speed 16", 120),
        ("jpeg", 20), ("ppa", 20),
        ("audio tone 1000 1000", 8), ("duplex 3", 15),
        ("bat", 5), ("rtc check", 5),
        ("lvgl stats", 5),
    ],
    "lvgl": [("lvgl stats", 5), ("lvgl bench", 150), ("lvgl stats", 5), ("lvgl ui", 5)],
    "net": [("wifi up", 30), ("wifi status", 5), ("ntp", 20), ("tls", 30), ("hosted fw", 10)],
    # need a person: each prints what to do on the screen and the console
    "hands": [("touch int 6", 12), ("touch rate 5", 10), ("touch paint 20", 30),
              ("lcd blsweep", 30), ("mic 5", 12), ("audio sweep", 12)],
}


def find_port():
    from serial.tools import list_ports
    ports = list(list_ports.comports())
    for p in ports:                              # CH343P on the board
        if p.vid == 0x1A86 and p.pid == 0x55D3:
            return p.device
    for p in ports:
        if "usbmodem" in p.device or "wchusbserial" in p.device:
            return p.device
    sys.exit("no board found; pass --port (ports: %s)" % ", ".join(p.device for p in ports))


class Serial:
    PROMPT = b"p4bench> "

    def __init__(self, port, baud):
        import serial
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, baud, 0.1
        self.s.dtr = False
        self.s.rts = False
        self.s.open()
        self.buf = b""
        # Opening the CH343 on macOS resets the board: wait until it boots to
        # the prompt, or the first command is swallowed by the boot.
        end, seen = time.time() + 8, b""
        while time.time() < end:
            seen += self.s.read(4096)
            if self.PROMPT.rstrip() in seen or (b"Type 'help'" in seen and time.time() > end - 6):
                time.sleep(0.5)
                break

    def run(self, cmd, timeout):
        self.s.reset_input_buffer()
        self.s.write(cmd.encode() + b"\r\n")
        out, end = b"", time.time() + timeout
        while time.time() < end:
            chunk = self.s.read(4096)
            if chunk:
                out += chunk
                sys.stdout.write(chunk.decode(errors="replace"))
                sys.stdout.flush()
                # the prompt comes back when the command returns
                if out.rstrip().endswith(self.PROMPT.rstrip()) and len(out) > len(cmd) + 12:
                    break
        return out.decode(errors="replace")

    def monitor(self):
        while True:
            chunk = self.s.read(4096)
            if chunk:
                sys.stdout.write(chunk.decode(errors="replace"))
                sys.stdout.flush()


class Http:
    def __init__(self, host):
        self.base = "http://%s" % host

    def run(self, cmd, timeout):
        url = "%s/api/cmd?c=%s" % (self.base, urllib.parse.quote_plus(cmd))
        with urllib.request.urlopen(url, timeout=timeout + 5) as r:
            text = r.read().decode(errors="replace")
        print(text, end="")
        return text


def bench_lines(text):
    return [l.strip() for l in text.splitlines() if l.strip().startswith("BENCH ")]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--http", help="host of a bench on Wi-Fi, e.g. p4bench.local")
    ap.add_argument("what", help="a suite (%s), 'cmd' or 'monitor'" % ", ".join(SUITES))
    ap.add_argument("cmds", nargs="*")
    a = ap.parse_args()

    link = Http(a.http) if a.http else Serial(a.port or find_port(), a.baud)
    if a.what == "monitor":
        link.monitor()
        return

    if a.what == "cmd":
        plan = [(c, 120) for c in a.cmds]
        name = "cmd"
    elif a.what in SUITES:
        plan = SUITES[a.what]
        name = a.what
    else:
        sys.exit("unknown: %s" % a.what)

    os.makedirs(os.path.join(RESULTS, "raw"), exist_ok=True)
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    raw = open(os.path.join(RESULTS, "raw", stamp + ".log"), "w")
    found = []
    for cmd, timeout in plan:
        print("\n### %s" % cmd)
        text = link.run(cmd, timeout)
        raw.write("### %s\n%s\n" % (cmd, text))
        found += bench_lines(text)
    raw.close()
    out = os.path.join(RESULTS, "%s-%s.txt" % (stamp, name))
    with open(out, "w") as f:
        f.write("# p4bench %s, %s\n" % (name, stamp))
        f.write("\n".join(found) + "\n")
    print("\n%d BENCH lines -> %s" % (len(found), os.path.relpath(out, ROOT)))


if __name__ == "__main__":
    main()
