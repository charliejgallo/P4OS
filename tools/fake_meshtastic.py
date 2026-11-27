#!/usr/bin/env python3
"""A make-believe Meshtastic node, for testing the Malla app (apps/malla)
without a radio: it speaks the client API (ToRadio / FromRadio protobufs,
framed as 0x94 0xC3 <len16 BE>) over TCP like a node with Wi-Fi, and over a
pty like a node on a serial port, with a mesh of made-up nodes around it.

    tools/fake_meshtastic.py                    # TCP on 127.0.0.1:4403
    tools/fake_meshtastic.py --port 14403       # another port
    tools/fake_meshtastic.py --pty              # also a pty: prints its path,
                                                # for P4_SIM_UART_A=/dev/ttysNNN
    tools/fake_meshtastic.py --log              # serial: debug text between frames

What it does, like a real node does it:
  - on want_config_id: my_info, metadata, a node_info per node, the config
    (device, LoRa, Bluetooth), its eight channels and config_complete_id;
  - the mesh talks by itself: text on the channels, positions, device and
    environment telemetry, now and then;
  - what the client sends: QueueStatus at once, then a Routing ACK (from the
    destination for a DM, the implicit one from the node for a broadcast),
    and a failure (MAX_RETRANSMIT) for the node that is out of reach; a DM
    is sometimes answered; traceroute, telemetry and position requests get
    their answers; admin messages change the owner, the LoRa settings and
    the channels, and a LoRa or channel change "restarts" the node: the TCP
    connection is closed, as a real ESP32 node does.

No dependencies: the protobuf wire format is written by hand below, with the
field numbers of meshtastic/protobufs v2.8.0 (apps/malla/proto). It listens on
127.0.0.1 only unless told otherwise, and is meant for the simulator.
"""
import argparse
import os
import random
import select
import socket
import struct
import sys
import threading
import time

# ---- the protobuf wire format, by hand ----


def varint(v):
    v &= (1 << 64) - 1
    out = bytearray()
    while True:
        b = v & 0x7F
        v >>= 7
        if v:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def key(field, wt):
    return varint((field << 3) | wt)


def f_varint(field, v):
    if v is None or v is False:
        return b""
    if v is True:
        v = 1
    if v < 0:
        v += 1 << 64
    return key(field, 0) + varint(v)


def f_fixed32(field, v):
    return b"" if v is None else key(field, 5) + struct.pack("<I", v & 0xFFFFFFFF)


def f_sfixed32(field, v):
    return b"" if v is None else key(field, 5) + struct.pack("<i", v)


def f_float(field, v):
    return b"" if v is None else key(field, 5) + struct.pack("<f", v)


def f_bytes(field, v):
    if v is None:
        return b""
    if isinstance(v, str):
        v = v.encode()
    return key(field, 2) + varint(len(v)) + v


def decode(buf):
    """field -> list of values: ints for varint/fixed, bytes for length-delimited"""
    out = {}
    i = 0
    while i < len(buf):
        k, i = read_varint(buf, i)
        field, wt = k >> 3, k & 7
        if wt == 0:
            v, i = read_varint(buf, i)
        elif wt == 5:
            v = struct.unpack_from("<I", buf, i)[0]
            i += 4
        elif wt == 1:
            v = struct.unpack_from("<Q", buf, i)[0]
            i += 8
        elif wt == 2:
            n, i = read_varint(buf, i)
            v = bytes(buf[i:i + n])
            i += n
        else:
            raise ValueError("wire type %d" % wt)
        out.setdefault(field, []).append(v)
    return out


def read_varint(buf, i):
    v = s = 0
    while True:
        b = buf[i]
        i += 1
        v |= (b & 0x7F) << s
        s += 7
        if not b & 0x80:
            return v, i


def one(d, f, default=None):
    return d[f][0] if f in d else default


# ---- the messages (field numbers from apps/malla/proto/meshtastic/*.proto) ----

PORT_TEXT, PORT_POSITION, PORT_NODEINFO, PORT_ROUTING, PORT_ADMIN = 1, 3, 4, 5, 6
PORT_TELEMETRY, PORT_TRACEROUTE = 67, 70
BROADCAST = 0xFFFFFFFF


def user(n):
    return (f_bytes(1, "!%08x" % n["num"]) + f_bytes(2, n["long"]) + f_bytes(3, n["short"])
            + f_varint(5, n["hw"]) + f_varint(7, n.get("role", 0))
            + (f_bytes(8, n["pk"]) if n.get("pk") else b""))


def position(n):
    return (f_sfixed32(1, n["lat"]) + f_sfixed32(2, n["lon"]) + f_varint(3, n.get("alt", 0))
            + f_fixed32(4, int(time.time())) + f_varint(23, 32))


def device_metrics(n):
    return (f_varint(1, n["bat"]) + f_float(2, n["volt"]) + f_float(3, n["chu"]) + f_float(4, n["air"])
            + f_varint(5, int(time.time() - n["boot"])))


def env_metrics(n):
    return f_float(1, n["temp"]) + f_float(2, n["hum"]) + f_float(3, n["press"])


def data(port, payload, request_id=None, want_response=False, reply_id=None, emoji=None):
    return (f_varint(1, port) + f_bytes(2, payload) + f_varint(3, want_response)
            + f_fixed32(6, request_id) + f_fixed32(7, reply_id) + f_fixed32(8, emoji))


def mesh_packet(frm, to, decoded, ch=0, pid=None, hops=0, snr=None, rssi=None, hop_start=3):
    pid = pid or random.getrandbits(31) | 1
    return (f_fixed32(1, frm) + f_fixed32(2, to) + f_varint(3, ch) + f_bytes(4, decoded)
            + f_fixed32(6, pid) + f_fixed32(7, int(time.time()))
            + f_float(8, snr) + f_varint(9, max(hop_start - hops, 0)) + f_varint(12, rssi)
            + f_varint(15, hop_start))


def fr_packet(pkt):
    return f_varint(1, random.getrandbits(31)) + f_bytes(2, pkt)


def lora_cfg(c):
    return (f_varint(1, True) + f_varint(2, c["preset"]) + f_varint(7, c["region"])
            + f_varint(8, c["hops"]) + f_varint(9, True) + f_varint(10, 22))


def channel(i, ch):
    if ch is None:
        return f_varint(1, i) + f_varint(3, 0)
    s = f_bytes(2, ch["psk"]) + f_bytes(3, ch["name"])
    return f_varint(1, i) + f_bytes(2, s) + f_varint(3, 1 if i == 0 else 2)


def route(nums, snrs):
    b = b""
    for n in nums:
        b += f_fixed32(1, n)
    for s in snrs:
        b += f_varint(2, s)
    return b


# ---- the mesh ----

LAT0, LON0 = -411330000, -713100000      # a mountain town, made up for the test


def make_mesh():
    me = dict(num=0x4D51A0B1, long="Base del Lago", short="BASE", hw=43, lat=LAT0, lon=LON0, alt=790,
              bat=101, volt=4.95, chu=8.5, air=1.2, boot=time.time() - 7200, pk=os.urandom(32))
    others = [
        dict(num=0x8C2F11A4, long="Refugio Frey", short="FREY", hw=9, dlat=-162000, dlon=-412000, alt=1700,
             hops=1, snr=-6.5, rssi=-112, bat=76, role=2, env=True),
        dict(num=0x1B7D02C3, long="Cerro Catedral", short="CATE", hw=31, dlat=-118000, dlon=-238000, alt=2100,
             hops=0, snr=7.25, rssi=-88, bat=101, role=2),
        dict(num=0x55A3E901, long="Ana en bici", short="ANA", hw=71, dlat=21000, dlon=-46000, alt=800,
             hops=0, snr=9.5, rssi=-71, bat=58, pk=os.urandom(32)),
        dict(num=0x3E019F77, long="Juan mochila", short="JUAN", hw=7, dlat=-58000, dlon=93000, alt=840,
             hops=1, snr=-2.0, rssi=-104, bat=33),
        dict(num=0x9A44C0DE, long="Lago Gutiérrez", short="GUTI", hw=69, dlat=-221000, dlon=-12000, alt=770,
             hops=2, snr=-11.0, rssi=-119, bat=91, env=True),
        dict(num=0x26F0B812, long="Otto repetidora", short="OTTO", hw=4, dlat=-402000, dlon=318000, alt=1450,
             hops=3, snr=-14.0, rssi=-124, bat=12, unreachable=True),
        dict(num=0x70C3D5E8, long="Camioneta 🚙", short="🚙", hw=50, dlat=74000, dlon=151000, alt=760,
             hops=0, snr=3.0, rssi=-97, bat=101),
    ]
    for n in others:
        n.update(lat=LAT0 + n.pop("dlat"), lon=LON0 + n.pop("dlon"), volt=3.3 + n["bat"] / 100.0 * 0.9,
                 chu=random.uniform(3, 18), air=random.uniform(0.2, 4), boot=time.time() - random.randint(600, 90000),
                 temp=random.uniform(4, 14), hum=random.uniform(40, 80), press=random.uniform(890, 1010))
    return me, others


PHRASES = [
    "Buen día a todos desde el refugio 👋", "¿Alguien sube al Catedral hoy?", "Llego en 20 min",
    "Viento fuerte arriba, ojo", "Copiado 👍", "¿Me leen?", "Sin novedades, todo tranquilo",
    "Quedan 2 lugares en la camioneta", "Arranca a llover acá 🌧️", "Prueba de alcance desde el lago",
]
ANSWERS = ["¡Recibido!", "Dale, nos vemos 🤙", "Te leo fuerte y claro", "Ok 👌"]
PHRASES_EN = [
    "Morning all from the hut 👋", "Anyone heading up Catedral today?", "There in 20 min",
    "Strong wind up top, careful", "Copy that 👍", "Do you read me?", "All quiet here",
    "Two seats left in the truck", "Starting to rain here 🌧️", "Range test from the lake",
]
ANSWERS_EN = ["Got it!", "Sure, see you there 🤙", "Loud and clear", "Ok 👌"]


class Node:
    def __init__(self, args):
        self.args = args
        self.phrases = PHRASES_EN if args.en else PHRASES
        self.answers = ANSWERS_EN if args.en else ANSWERS
        self.me, self.others = make_mesh()
        self.cfg = dict(region=1, preset=0, hops=3)
        self.channels = [dict(name="", psk=b"\x01"), dict(name="Refugios", psk=os.urandom(32))] + [None] * 6
        self.clients = []           # (send function, closer)
        self.lock = threading.Lock()

    def by_num(self, num):
        for n in self.others:
            if n["num"] == num:
                return n
        return None

    # ---- to the client ----

    def push(self, fr, only=None):
        with self.lock:
            for c in list(self.clients):
                if only is None or c is only:
                    c.send(fr)

    def config_dump(self, client, nonce):
        me = self.me
        frs = [f_varint(1, 1) + f_bytes(3, f_varint(1, me["num"]) + f_varint(8, 3) + f_varint(11, 30200)
                                       + f_varint(15, len(self.others) + 1))]
        frs.append(f_bytes(13, f_bytes(1, "2.8.0.fake") + f_varint(2, 24) + f_varint(4, True) + f_varint(5, True)
                           + f_varint(9, me["hw"]) + f_varint(11, True)))
        frs.append(f_bytes(4, f_varint(1, me["num"]) + f_bytes(2, user(me)) + f_bytes(3, position(me))
                           + f_fixed32(5, int(time.time())) + f_bytes(6, device_metrics(me)) + f_varint(9, 0)))
        for n in self.others:
            ni = (f_varint(1, n["num"]) + f_bytes(2, user(n)) + f_bytes(3, position(n)) + f_float(4, n["snr"])
                  + f_fixed32(5, int(time.time()) - random.randint(30, 5400)) + f_bytes(6, device_metrics(n))
                  + f_varint(9, n["hops"]) + f_varint(10, n["num"] == self.others[1]["num"]))
            frs.append(f_bytes(4, ni))
        frs.append(f_bytes(5, f_bytes(1, f_varint(1, 0) + f_varint(2, True))))                   # device
        frs.append(f_bytes(5, f_bytes(6, lora_cfg(self.cfg))))                                    # lora
        frs.append(f_bytes(5, f_bytes(7, f_varint(1, True) + f_varint(2, 0) + f_varint(3, 123456))))  # bluetooth
        for i, ch in enumerate(self.channels):
            frs.append(f_bytes(10, channel(i, ch)))
        frs.append(f_varint(7, nonce))
        for fr in frs:
            client.send(fr)
            time.sleep(0.01)
        print("config sent (%d frames)" % len(frs))

    def from_node(self, n, port, payload, to=BROADCAST, ch=0, **kw):
        pkt = mesh_packet(n["num"], to, data(port, payload, **kw), ch=ch, hops=n["hops"], snr=n["snr"],
                          rssi=n["rssi"] if n["hops"] == 0 else None)
        self.push(fr_packet(pkt))

    def routing(self, frm, to, request_id, error=0):
        pkt = mesh_packet(frm, to, data(PORT_ROUTING, f_varint(3, error), request_id=request_id), hops=0)
        self.push(fr_packet(pkt))

    def queue_status(self, pid, client):
        client.send(f_bytes(11, f_varint(1, 0) + f_varint(2, 15) + f_varint(3, 16) + f_varint(4, pid)))

    # ---- from the client ----

    def to_radio(self, client, buf):
        try:
            tr = decode(buf)
        except Exception as e:  # noqa: BLE001 - a bad frame is printed and dropped
            print("bad ToRadio:", e)
            return
        if 3 in tr:
            print("want_config_id", hex(tr[3][0]))
            threading.Thread(target=self.config_dump, args=(client, tr[3][0]), daemon=True).start()
        elif 7 in tr:
            client.send(f_bytes(11, f_varint(1, 0) + f_varint(2, 16) + f_varint(3, 16)))
        elif 4 in tr:
            print("client said goodbye")
        elif 1 in tr:
            self.packet(client, decode(tr[1][0]))

    def packet(self, client, p):
        to = one(p, 2, 0)
        pid = one(p, 6, 0)
        ch = one(p, 3, 0)
        d = decode(one(p, 4, b""))
        port = one(d, 1, 0)
        payload = one(d, 2, b"")
        self.queue_status(pid, client)
        if port == PORT_TEXT:
            text = payload.decode(errors="replace")
            reaction = one(d, 8, 0)
            print("text to %08x ch %d%s: %s" % (to, ch, " (reaction)" if reaction else "", text))
            if to == BROADCAST:
                threading.Timer(1.2, self.routing, (self.me["num"], self.me["num"], pid)).start()
                if not reaction and random.random() < 0.6:
                    n = random.choice(self.others[:4])
                    threading.Timer(3.5, self.from_node, (n, PORT_TEXT, random.choice(self.answers).encode()),
                                    dict(ch=ch)).start()
            else:
                n = self.by_num(to)
                if not n or n.get("unreachable"):
                    threading.Timer(6.0, self.routing, (self.me["num"], self.me["num"], pid, 5)).start()
                else:
                    threading.Timer(1.0 + n["hops"], self.routing, (to, self.me["num"], pid)).start()
                    if not reaction:
                        threading.Timer(4.0 + n["hops"], self.from_node,
                                        (n, PORT_TEXT, random.choice(self.answers).encode()),
                                        dict(to=self.me["num"])).start()
        elif port == PORT_TRACEROUTE:
            n = self.by_num(to)
            print("traceroute to %08x" % to)
            if n and not n.get("unreachable"):
                via = [o["num"] for o in self.others if 0 < o["hops"] < n["hops"]][: n["hops"]]
                snr_to = [int(random.uniform(-40, 30)) for _ in range(len(via) + 1)]
                snr_back = [int(random.uniform(-40, 30)) for _ in range(len(via) + 1)]
                rd = route(via, snr_to) + b"".join(f_fixed32(3, v) for v in reversed(via)) + \
                    b"".join(f_varint(4, s) for s in snr_back)
                threading.Timer(2.0 + n["hops"], self.from_node, (n, PORT_TRACEROUTE, rd),
                                dict(to=self.me["num"], request_id=pid)).start()
            else:
                threading.Timer(8.0, self.routing, (self.me["num"], self.me["num"], pid, 8)).start()
        elif port == PORT_TELEMETRY:
            n = self.by_num(to) or self.me
            print("telemetry asked of %08x" % to)
            tel = f_fixed32(1, int(time.time())) + f_bytes(2, device_metrics(n))
            if n is self.me:
                client.send(fr_packet(mesh_packet(n["num"], self.me["num"], data(PORT_TELEMETRY, tel), hops=0)))
            else:
                threading.Timer(1.5, self.from_node, (n, PORT_TELEMETRY, tel),
                                dict(to=self.me["num"], request_id=pid)).start()
        elif port == PORT_POSITION:
            n = self.by_num(to)
            if n:
                threading.Timer(1.5, self.from_node, (n, PORT_POSITION, position(n)),
                                dict(to=self.me["num"], request_id=pid)).start()
        elif port == PORT_ADMIN:
            self.admin(client, decode(payload), pid)

    def admin(self, client, a, pid):
        restart = False
        if 32 in a:
            u = decode(a[32][0])
            self.me["long"] = one(u, 2, b"").decode()
            self.me["short"] = one(u, 3, b"").decode()
            print("owner:", self.me["long"], self.me["short"])
        if 34 in a:
            c = decode(a[34][0])
            if 6 in c:
                lc = decode(c[6][0])
                self.cfg = dict(region=one(lc, 7, 0), preset=one(lc, 2, 0), hops=one(lc, 8, 3))
                print("lora:", self.cfg)
                restart = True
        if 33 in a:
            c = decode(a[33][0])
            i = one(c, 1, 0)
            role = one(c, 3, 0)
            s = decode(one(c, 2, b""))
            self.channels[i] = None if role == 0 else dict(name=one(s, 3, b"").decode(), psk=one(s, 2, b""))
            print("channel %d:" % i, self.channels[i])
            restart = restart or not self.editing
        if 64 in a:
            self.editing = True
        if 65 in a:
            self.editing = False
            restart = True
        self.routing(self.me["num"], self.me["num"], pid)
        if restart and not self.editing:
            print("restarting (closing the clients)")
            threading.Timer(1.0, self.restart).start()

    editing = False

    def restart(self):
        with self.lock:
            gone = list(self.clients)
        for c in gone:              # close() takes the lock itself
            c.close()

    # ---- the mesh talking by itself ----

    def chatter(self):
        while True:
            time.sleep(random.uniform(self.args.every * 0.5, self.args.every * 1.5))
            with self.lock:
                if not self.clients:
                    continue
            n = random.choice(self.others)
            r = random.random()
            if r < 0.35:
                ch = 0 if random.random() < 0.7 else 1
                self.from_node(n, PORT_TEXT, random.choice(self.phrases).encode(), ch=ch)
            elif r < 0.55:
                n["lat"] += random.randint(-3000, 3000)
                n["lon"] += random.randint(-3000, 3000)
                self.from_node(n, PORT_POSITION, position(n))
            elif r < 0.85:
                n["bat"] = n["bat"] if n["bat"] > 100 else max(1, n["bat"] - random.randint(0, 2))
                n["chu"] = max(0.5, min(40, n["chu"] + random.uniform(-3, 3)))
                n["air"] = max(0.1, min(10, n["air"] + random.uniform(-0.6, 0.6)))
                n["volt"] = 3.3 + min(n["bat"], 100) / 100.0 * 0.9
                self.from_node(n, PORT_TELEMETRY, f_fixed32(1, int(time.time())) + f_bytes(2, device_metrics(n)))
            elif n.get("env"):
                n["temp"] += random.uniform(-0.6, 0.6)
                n["hum"] = max(10, min(99, n["hum"] + random.uniform(-2, 2)))
                n["press"] += random.uniform(-0.8, 0.8)
                self.from_node(n, PORT_TELEMETRY, f_fixed32(1, int(time.time())) + f_bytes(3, env_metrics(n)))
            else:
                self.from_node(n, PORT_NODEINFO, user(n))


# ---- the links ----

class Client:
    """One client on a stream (a TCP socket or a pty), with the framing."""

    def __init__(self, node, write, close, name, log=False):
        self.node, self._write, self._close, self.name, self.log = node, write, close, name, log
        self.buf = bytearray()
        self.wlock = threading.Lock()
        self.alive = True

    def send(self, fr):
        if not self.alive:
            return
        frame = bytes([0x94, 0xC3, len(fr) >> 8, len(fr) & 0xFF]) + fr
        try:
            with self.wlock:
                if self.log and random.random() < 0.2:
                    self._write(b"INFO  | ??:??:?? 12 [Router] fake log line between frames\r\n")
                self._write(frame)
        except OSError:
            self.close()

    def feed(self, data):
        self.buf += data
        while True:
            i = self.buf.find(b"\x94\xc3")
            if i < 0:
                if len(self.buf) > 1:
                    del self.buf[:-1]
                return
            if i:
                del self.buf[:i]
            if len(self.buf) < 4:
                return
            n = self.buf[2] << 8 | self.buf[3]
            if n > 512:
                del self.buf[:2]
                continue
            if len(self.buf) < 4 + n:
                return
            fr = bytes(self.buf[4:4 + n])
            del self.buf[:4 + n]
            self.node.to_radio(self, fr)

    def close(self):
        if self.alive:
            self.alive = False
            print("client gone:", self.name)
            try:
                self._close()
            except OSError:
                pass
            with self.node.lock:
                if self in self.node.clients:
                    self.node.clients.remove(self)


def serve_tcp(node, host, port):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((host, port))
    srv.listen(4)
    print("TCP on %s:%d" % (host, port))
    while True:
        s, addr = srv.accept()
        print("client from", addr)
        c = Client(node, s.sendall, lambda s=s: (s.shutdown(socket.SHUT_RDWR), s.close()), "tcp %s:%d" % addr)
        with node.lock:
            node.clients.append(c)

        def rx(c=c, s=s):
            while c.alive:
                try:
                    d = s.recv(4096)
                except OSError:
                    break
                if not d:
                    break
                c.feed(d)
            c.close()
        threading.Thread(target=rx, daemon=True).start()


def serve_pty(node, log):
    import tty
    master, slave = os.openpty()
    tty.setraw(slave)
    print("serial on", os.ttyname(slave), flush=True)
    c = Client(node, lambda b: os.write(master, b), lambda: None, "pty", log=log)
    with node.lock:
        node.clients.append(c)
    while True:
        r, _, _ = select.select([master], [], [], 1.0)
        if r:
            try:
                c.feed(os.read(master, 4096))
            except OSError:
                time.sleep(0.5)
        if not c.alive:             # a "restart": the port comes back
            time.sleep(2)
            c = Client(node, lambda b: os.write(master, b), lambda: None, "pty", log=log)
            with node.lock:
                node.clients.append(c)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=4403)
    ap.add_argument("--pty", action="store_true", help="also a serial port (a pty)")
    ap.add_argument("--no-tcp", action="store_true")
    ap.add_argument("--log", action="store_true", help="serial: debug text between frames, as a node with its log on")
    ap.add_argument("--every", type=float, default=6.0, help="seconds between the mesh's own packets (about)")
    ap.add_argument("--seed", type=int, default=None)
    ap.add_argument("--en", action="store_true", help="the mesh talks in English (for the store's screenshots)")
    args = ap.parse_args()
    if args.seed is not None:
        random.seed(args.seed)
    node = Node(args)
    threading.Thread(target=node.chatter, daemon=True).start()
    if args.pty:
        threading.Thread(target=serve_pty, args=(node, args.log), daemon=True).start()
    if args.no_tcp:
        while True:
            time.sleep(3600)
    serve_tcp(node, args.host, args.port)


if __name__ == "__main__":
    try:
        sys.stdout.reconfigure(line_buffering=True)
        main()
    except KeyboardInterrupt:
        pass
