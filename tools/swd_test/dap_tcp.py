#!/usr/bin/env python3
"""
The Programador's probe (components/aos_flasher/aos_flasher_dap.c) in the
simulator, driven by pyOCD as a debugger on the computer would drive the
board's CMSIS-DAP over USB.

The simulator moves the probe's packets over TCP (sim/usb_probe_sim.c: each
with a 2-byte length first, P4_SIM_PROBE_PORT, 4002 by default). This gives
pyOCD a CMSIS-DAP v2 interface on that socket and runs a debugger's first
minutes against the emulated target (sim/swd_sim.c, P4_SIM_SWD): connect,
halt, the core's registers, memory read and written, one step, resume.

    pip install pyocd              (a virtualenv is enough)
    P4_SIM_SWD=nrf52840 ... p4os_sim     then Programador > SWD > Probe
    python3 tools/swd_test/dap_tcp.py [--port 4002] [--ram 0x20000000]

Exits 0 when every check passed.
"""
import argparse
import queue
import socket
import struct
import sys
import threading

from pyocd.core.helpers import ConnectHelper  # noqa: F401  (pyOCD's own setup)
from pyocd.core.session import Session
from pyocd.core.target import Target
from pyocd.probe.cmsis_dap_probe import CMSISDAPProbe
from pyocd.probe.pydapaccess.dap_access_cmsis_dap import DAPAccessCMSISDAP
from pyocd.probe.pydapaccess.interface.interface import Interface


class TcpInterface(Interface):
    """A CMSIS-DAP v2 'bulk' interface whose endpoints are a TCP socket."""

    def __init__(self, host, port):
        super().__init__()
        self.host, self.port = host, port
        self.vendor_name = "P4OS"
        self.product_name = "P4OS CMSIS-DAP (simulator)"
        self.serial_number = f"sim-{port}"
        self.packet_size = 512
        self.sock = None
        self.rx = queue.Queue()

    @property
    def is_bulk(self):
        return True

    def open(self):
        # pyOCD opens the probe twice (a look at its info, then the
        # session): each opening has its own socket and its own queue
        self.sock = socket.create_connection((self.host, self.port))
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.rx = queue.Queue()
        threading.Thread(target=self._reader, args=(self.sock, self.rx), daemon=True).start()

    @staticmethod
    def _recv(sock, n):
        b = b""
        while len(b) < n:
            c = sock.recv(n - len(b))
            if not c:
                raise EOFError
            b += c
        return b

    def _reader(self, sock, rx):
        try:
            while True:
                (n,) = struct.unpack("<H", self._recv(sock, 2))
                rx.put(self._recv(sock, n))
        except (EOFError, OSError):
            rx.put(None)

    def close(self):
        if self.sock:
            self.sock.close()
            self.sock = None

    def write(self, data):
        self.sock.sendall(struct.pack("<H", len(data)) + bytes(data))

    def read(self):
        data = self.rx.get(timeout=10)
        if data is None:
            raise DAPAccessCMSISDAP.DeviceError("the simulator closed the probe")
        return data


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4002)
    ap.add_argument("--ram", type=lambda s: int(s, 0), default=0x20000000)
    ap.add_argument("--target", default="cortex_m")
    a = ap.parse_args()

    fails = 0

    def check(what, ok, detail=""):
        nonlocal fails
        print(("ok    " if ok else "FAIL  ") + what + (f": {detail}" if detail else ""))
        fails += 0 if ok else 1

    link = DAPAccessCMSISDAP(None, interface=TcpInterface(a.host, a.port))
    probe = CMSISDAPProbe(link)
    with Session(probe, target_override=a.target, options={"frequency": 2000000,
                                                            "resume_on_disconnect": True}) as s:
        t = s.target
        print(f"probe: {probe.vendor_name} {probe.product_name}, caps {sorted(c.name for c in probe.capabilities)}")
        print(f"DP IDR 0x{t.dp.dpidr.idr:08x}, APs {list(t.aps.keys())}")
        core = t.selected_core
        check("core found", core is not None, getattr(core, "name", ""))
        cpuid = t.read32(0xE000ED00)
        check("CPUID read", cpuid >> 24 == 0x41, f"0x{cpuid:08x}")

        t.halt()
        check("halted", t.get_state() == Target.State.HALTED)
        pc = t.read_core_register("pc")
        t.write_core_register("r0", 0x12345678)
        check("core register written and read", t.read_core_register("r0") == 0x12345678)

        pattern = bytes((i * 7 + 3) & 0xFF for i in range(1024))
        t.write_memory_block8(a.ram, pattern)
        back = bytes(t.read_memory_block8(a.ram, len(pattern)))
        check("1 KB to RAM and back (TransferBlock)", back == pattern)
        t.write32(a.ram + 0x400, 0xCAFEF00D)
        check("one word to RAM and back (Transfer)", t.read32(a.ram + 0x400) == 0xCAFEF00D)
        words = t.read_memory_block32(0, 16)
        check("flash's first words", len(words) == 16, " ".join(f"{w:08x}" for w in words[:4]))

        t.step()
        pc2 = t.read_core_register("pc")
        check("one step", pc2 == pc + 2, f"pc 0x{pc:08x} -> 0x{pc2:08x}")

        t.resume()
        check("running again", t.get_state() == Target.State.RUNNING)
    print("all good" if not fails else f"{fails} failed")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
