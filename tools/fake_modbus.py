#!/usr/bin/env python3
"""
Make-believe Modbus slaves for the Modbus app, standard library only.

    tools/fake_modbus.py [--rtu] [--tcp 1502]

--rtu   a Riden RD6012 on a pty (it prints the device: run the simulator
        with P4_SIM_UART_A=<it>), 115200 8N1, unit 1, with the register map
        measured on the real unit (riden-psu/README.md) and its habits:
          - out-of-range writes rejected in silence (the register keeps its
            value, no exception);
          - the voltage ceiling is the input minus ~1 V;
          - a load of 27 ohm on the output: CV below the current limit, CC
            above it, power and Ah/Wh counters that integrate.
--tcp   a DOMCOM gateway (Modbus TCP) on that port: relays and aux modules
        as coils, dimmers as holding registers, sensors that drift as input
        registers, two magnetic sensors as discrete inputs.

Function codes 1, 2, 3, 4, 5, 6, 15 and 16; exception 01 for the rest and 02
for addresses outside a map.
"""
import argparse
import math
import os
import pty
import random
import select
import socket
import struct
import threading
import time
import tty


def crc16(data: bytes) -> int:
    c = 0xFFFF
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0xA001 if c & 1 else c >> 1
    return c


class Slave:
    """A register space; subclasses fill it and react to writes."""
    def __init__(self):
        self.coils, self.discrete, self.holding, self.input = {}, {}, {}, {}
        self.lock = threading.Lock()

    def tick(self):
        pass

    def write_reg(self, a, v):          # may refuse in silence
        if a not in self.holding:
            raise KeyError
        self.holding[a] = v

    def write_coil(self, a, v):
        if a not in self.coils:
            raise KeyError
        self.coils[a] = v

    def pdu(self, req: bytes) -> bytes:
        fc = req[0]
        try:
            with self.lock:
                self.tick()
                if fc in (1, 2):
                    a, n = struct.unpack(">HH", req[1:5])
                    space = self.coils if fc == 1 else self.discrete
                    bits = [space[a + i] for i in range(n)]
                    out = bytearray((n + 7) // 8)
                    for i, b in enumerate(bits):
                        if b:
                            out[i // 8] |= 1 << (i % 8)
                    return bytes([fc, len(out)]) + bytes(out)
                if fc in (3, 4):
                    a, n = struct.unpack(">HH", req[1:5])
                    space = self.holding if fc == 3 else self.input
                    vals = [space[a + i] & 0xFFFF for i in range(n)]
                    return bytes([fc, 2 * n]) + b"".join(struct.pack(">H", v) for v in vals)
                if fc == 5:
                    a, v = struct.unpack(">HH", req[1:5])
                    self.write_coil(a, 1 if v == 0xFF00 else 0)
                    return req[:5]
                if fc == 6:
                    a, v = struct.unpack(">HH", req[1:5])
                    self.write_reg(a, v)
                    return req[:5]
                if fc == 15:
                    a, n = struct.unpack(">HH", req[1:5])
                    data = req[6:]
                    for i in range(n):
                        self.write_coil(a + i, (data[i // 8] >> (i % 8)) & 1)
                    return req[:5]
                if fc == 16:
                    a, n = struct.unpack(">HH", req[1:5])
                    for i in range(n):
                        self.write_reg(a + i, struct.unpack(">H", req[6 + 2 * i:8 + 2 * i])[0])
                    return req[:5]
                return bytes([fc | 0x80, 1])
        except (KeyError, IndexError):
            return bytes([fc | 0x80, 2])


class Riden(Slave):
    """RD6012: V and I x100, power 32-bit x100, Ah/Wh 32-bit x1000."""
    R_LOAD = 27.0

    def __init__(self):
        super().__init__()
        h = self.holding
        for a in range(0, 120):
            h[a] = 0
        h[0] = 60121; h[1] = 0; h[2] = 4546; h[3] = 131
        h[4] = 0; h[5] = 31; h[6] = 0; h[7] = 88
        h[8] = 1200; h[9] = 50; h[14] = 4825; h[15] = 1; h[19] = 0
        h[34] = 0; h[35] = 26; h[36] = 0; h[37] = 79
        for m in range(10):         # presets M0..M9: V, I, OVP, OCP
            v, i = [(500, 100), (330, 50), (500, 100), (1200, 100), (1200, 200), (2400, 100), (1500, 50), (900, 100), (1000, 100), (1200, 50)][m]
            for k, x in enumerate((v, i, 6200, 1220)):
                h[80 + 4 * m + k] = x
        self.ah = self.wh = 0.0
        self.last = time.time()

    def write_reg(self, a, v):
        h = self.holding
        vin = h[14] / 100
        if a == 8 and (v > round((vin - 1.01) * 100) or v > 6000):
            print("  riden: V_SET %.2f refused (input %.2f V)" % (v / 100, vin))
            return                                  # in silence, like the real one
        if a == 9 and v > 1210:
            print("  riden: I_SET %.2f refused" % (v / 100))
            return
        if a == 18 and v not in (0, 1):
            return
        if a == 19 and v <= 9:                      # recall a preset
            h[8], h[9] = h[80 + 4 * v], h[81 + 4 * v]
        if a in (0, 1, 2, 3, 10, 11, 12, 13, 14):
            return                                  # read-only, quietly
        h[a] = v
        print("  riden: reg %d = %d" % (a, v))

    def tick(self):
        h = self.holding
        now = time.time()
        dt, self.last = now - self.last, now
        h[14] = 4825 + random.randint(-3, 3)       # the input wanders a little
        if h[18]:
            vset, iset = h[8] / 100, h[9] / 100
            i_cv = vset / self.R_LOAD
            if i_cv > iset:                         # constant current
                i, v, cc = iset, iset * self.R_LOAD, 1
            else:
                i, v, cc = i_cv, vset, 0
            i *= 1 + random.uniform(-0.004, 0.004)
            h[10], h[11], h[17] = round(v * 100), round(i * 100), cc
            p = v * i
            self.ah += i * dt / 3600
            self.wh += p * dt / 3600
        else:
            h[10] = h[11] = h[17] = 0
            p = 0
        pw = round(p * 100)
        h[12], h[13] = pw >> 16, pw & 0xFFFF
        ah, wh = round(self.ah * 1000), round(self.wh * 1000)
        h[38], h[39], h[40], h[41] = ah >> 16, ah & 0xFFFF, wh >> 16, wh & 0xFFFF
        h[5] = 31 + int(p / 20)                     # it warms with the load
        t = time.localtime()
        for k, x in enumerate((t.tm_year, t.tm_mon, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec)):
            h[48 + k] = x


class Gateway(Slave):
    def __init__(self):
        super().__init__()
        for a in list(range(0, 4)) + list(range(20, 24)):
            self.coils[a] = 0
        self.coils[0] = 1
        for a in range(0, 4):
            self.holding[a] = [60, 0, 100, 25][a]
        self.holding[23] = 5
        for a in range(30, 37):
            self.holding[a] = 0
        self.holding[31] = 24
        for a in range(8, 10):
            self.discrete[a] = 0
        for a in range(0, 25):
            self.input[a] = 0
        self.t0 = time.time()

    def tick(self):
        t = time.time() - self.t0
        i = self.input
        i[0] = int((23.4 + math.sin(t / 30) * 0.8 + random.uniform(-0.05, 0.05)) * 100) & 0xFFFF
        i[1] = int((51 + math.sin(t / 45) * 4) * 100)
        i[2] = 0
        i[3] = int(t) & 0xFFFF
        i[5] = int((1013.2 + math.sin(t / 90)) * 10)
        i[6] = int(620 + math.sin(t / 20) * 80)
        i[7] = 184
        i[8] = 42
        i[9] = (-61 + random.randint(-2, 2)) & 0xFFFF
        i[20], i[21] = 2375 + random.randint(-5, 5), 1812 + random.randint(-5, 5)
        i[22] = i[23] = 0x8000
        i[24] = 2
        self.discrete[8] = 1 if (int(t) // 20) % 3 == 0 else 0

    def write_reg(self, a, v):
        if 0 <= a <= 3 and v > 100:
            raise KeyError
        super().write_reg(a, v)
        print("  gateway: holding %d = %d" % (a, v))

    def write_coil(self, a, v):
        super().write_coil(a, v)
        print("  gateway: coil %d = %d" % (a, v))


def serve_rtu(slave: Slave, unit=1):
    master, slave_fd = pty.openpty()
    tty.setraw(master)
    print("PTY", os.ttyname(slave_fd), flush=True)
    print("ENV P4_SIM_UART_A=%s" % os.ttyname(slave_fd), flush=True)
    buf = b""
    last = time.time()
    while True:
        r, _, _ = select.select([master], [], [], 0.005)
        if r:
            buf += os.read(master, 256)
            last = time.time()
            continue
        if not buf or time.time() - last < 0.004:   # a frame ends with silence (t3.5)
            continue
        frame, buf = buf, b""
        if len(frame) < 4 or crc16(frame[:-2]) != struct.unpack("<H", frame[-2:])[0]:
            print("  rtu: bad frame", frame.hex())
            continue
        if frame[0] != unit:
            continue
        rsp = bytes([unit]) + slave.pdu(frame[1:-2])
        rsp += struct.pack("<H", crc16(rsp))
        time.sleep(0.003)
        os.write(master, rsp)


def serve_tcp(slave: Slave, port: int):
    ls = socket.socket()
    ls.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    ls.bind(("127.0.0.1", port))
    ls.listen(4)
    print("TCP 127.0.0.1:%d (gateway)" % port, flush=True)

    def client(c):
        with c:
            while True:
                h = c.recv(7, socket.MSG_WAITALL)
                if len(h) < 7:
                    return
                tid, proto, ln, unit = struct.unpack(">HHHB", h)
                pdu = c.recv(ln - 1, socket.MSG_WAITALL)
                rsp = slave.pdu(pdu)
                c.sendall(struct.pack(">HHHB", tid, 0, len(rsp) + 1, unit) + rsp)

    while True:
        c, _ = ls.accept()
        threading.Thread(target=client, args=(c,), daemon=True).start()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rtu", action="store_true", help="a Riden RD6012 on a pty")
    ap.add_argument("--tcp", type=int, default=0, help="a DOMCOM gateway on this TCP port")
    a = ap.parse_args()
    if not a.rtu and not a.tcp:
        a.rtu, a.tcp = True, 1502
    if a.tcp:
        threading.Thread(target=serve_tcp, args=(Gateway(), a.tcp), daemon=True).start()
    if a.rtu:
        serve_rtu(Riden())
    else:
        while True:
            time.sleep(1)


if __name__ == "__main__":
    main()
