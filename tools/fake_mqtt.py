#!/usr/bin/env python3
"""
A make-believe MQTT broker with a house behind it, for the MQTT app;
standard library only.

    tools/fake_mqtt.py [--port 1883] [--user U --password P] [--rate 1.0]
                       [--no-house] [--quiet] [--bind 127.0.0.1]

The broker: MQTT 3.1.1 (and 3.1's "MQIsdp"), CONNECT with an optional
user/password check (CONNACK 4 when wrong), clean and persistent sessions
treated alike, SUBSCRIBE / UNSUBSCRIBE with + and #, QoS 0 and 1 both ways
(QoS 2 from clients is accepted and delivered as 1), retained messages (an
empty retained payload deletes one), keepalive enforced at 1.5 times, $SYS
left alone by leading wildcards. Everything a client publishes is printed.

The house, every --rate seconds (values drift like real ones):

    zigbee2mqtt/<room>_temp    {"temperature":22.4,"humidity":51,"battery":87,"linkquality":120}
    zigbee2mqtt/kitchen_plug   {"state":"ON","power":..,"energy":..,"voltage":..}
    zigbee2mqtt/front_door     {"contact":true,"battery":..}
    zigbee2mqtt/bridge/state   {"state":"online"}                      (retained)
    home/power/w               1234                                     (plain numbers)
    home/solar/w, home/power/kwh_today, home/water/l_min
    tele/plug1/SENSOR          tasmota {"Time":..,"ENERGY":{..}}        (every 10 s)
    tele/plug1/STATE, tele/plug1/LWT "Online" (retained), stat/plug1/POWER
    garage-node/status         online (retained), ESPHome-ish sensors
    homeassistant/.../config   discovery configs (retained)

And it listens: zigbee2mqtt/kitchen_plug/set {"state":"OFF"|"ON"|"TOGGLE"},
cmnd/plug1/POWER ON|OFF|TOGGLE and garage-node/switch/relay/command ON|OFF
change the state the house reports.
"""
import argparse
import json
import math
import random
import socket
import struct
import sys
import threading
import time

LOCK = threading.RLock()
RETAINED = {}           # topic -> (payload bytes, qos)
CLIENTS = set()
ARGS = None


def log(*a):
    print(time.strftime('%H:%M:%S'), *a, flush=True)


def match(flt, topic):
    if topic.startswith('$') and flt[:1] in ('#', '+'):
        return False
    f = flt.split('/')
    t = topic.split('/')
    for i, part in enumerate(f):
        if part == '#':
            return True
        if i >= len(t):
            return False
        if part != '+' and part != t[i]:
            return False
    return len(f) == len(t)


def enc_len(n):
    out = bytearray()
    while True:
        d = n % 128
        n //= 128
        if n:
            d |= 0x80
        out.append(d)
        if not n:
            return bytes(out)


def mstr(b):
    return struct.pack('>H', len(b)) + b


class Client:
    def __init__(self, sock, addr):
        self.sock = sock
        self.addr = addr
        self.id = '?'
        self.subs = {}          # filter -> qos
        self.keepalive = 0
        self.last_rx = time.time()
        self.pid = 0
        self.wlock = threading.Lock()
        self.alive = True

    def send(self, data):
        try:
            with self.wlock:
                self.sock.sendall(data)
        except OSError:
            self.alive = False

    def next_pid(self):
        self.pid = self.pid % 65535 + 1
        return self.pid

    def deliver(self, topic, payload, qos, retain):
        body = mstr(topic.encode())
        if qos:
            body += struct.pack('>H', self.next_pid())
        body += payload
        self.send(bytes([0x30 | (qos << 1) | (1 if retain else 0)]) + enc_len(len(body)) + body)

    def recv_exact(self, n):
        buf = b''
        while len(buf) < n:
            chunk = self.sock.recv(n - len(buf))
            if not chunk:
                raise ConnectionError('closed')
            buf += chunk
        return buf

    def read_packet(self):
        h = self.recv_exact(1)[0]
        mul, rem = 1, 0
        for _ in range(4):
            d = self.recv_exact(1)[0]
            rem += (d & 0x7F) * mul
            mul *= 128
            if not d & 0x80:
                break
        else:
            raise ConnectionError('bad length')
        return h, self.recv_exact(rem) if rem else b''


def publish(topic, payload, qos=0, retain=False, source=None):
    if isinstance(payload, str):
        payload = payload.encode()
    with LOCK:
        if retain:
            if payload:
                RETAINED[topic] = (payload, qos)
            else:
                RETAINED.pop(topic, None)
        targets = []
        for c in CLIENTS:
            best = -1
            for f, q in c.subs.items():
                if match(f, topic):
                    best = max(best, q)
            if best >= 0:
                targets.append((c, min(best, qos)))
    for c, q in targets:
        c.deliver(topic, payload, q, False)


def handle(c):
    sock = c.sock
    sock.settimeout(1.0)
    connected = False
    while c.alive:
        try:
            h, body = c.read_packet()
        except socket.timeout:
            if connected and c.keepalive and time.time() - c.last_rx > c.keepalive * 1.5:
                log(f'[{c.id}] keepalive expired, dropping')
                break
            continue
        except (ConnectionError, OSError):
            break
        c.last_rx = time.time()
        t = h >> 4
        if t == 1:                                  # CONNECT
            p = 0
            pl = struct.unpack('>H', body[p:p + 2])[0]
            proto = body[p + 2:p + 2 + pl].decode(errors='replace')
            p += 2 + pl
            level = body[p]
            flags = body[p + 1]
            c.keepalive = struct.unpack('>H', body[p + 2:p + 4])[0]
            p += 4

            def rstr():
                nonlocal p
                n = struct.unpack('>H', body[p:p + 2])[0]
                s = body[p + 2:p + 2 + n]
                p += 2 + n
                return s
            c.id = rstr().decode(errors='replace') or 'anon'
            if flags & 0x04:                        # will: read and ignore
                rstr()
                rstr()
            user = rstr().decode(errors='replace') if flags & 0x80 else None
            pw = rstr().decode(errors='replace') if flags & 0x40 else None
            if proto not in ('MQTT', 'MQIsdp') or level not in (3, 4):
                c.send(b'\x20\x02\x00\x01')
                break
            if ARGS.user is not None and (user != ARGS.user or pw != ARGS.password):
                log(f'[{c.id}] refused: user {user!r}')
                c.send(b'\x20\x02\x00\x04')
                break
            c.send(b'\x20\x02\x00\x00')
            connected = True
            log(f'[{c.id}] connected from {c.addr[0]}:{c.addr[1]} keepalive {c.keepalive}s'
                + (f' user {user}' if user else ''))
        elif not connected:
            break
        elif t == 3:                                # PUBLISH
            qos = (h >> 1) & 3
            retain = bool(h & 1)
            tl = struct.unpack('>H', body[:2])[0]
            topic = body[2:2 + tl].decode(errors='replace')
            p = 2 + tl
            pid = None
            if qos:
                pid = struct.unpack('>H', body[p:p + 2])[0]
                p += 2
            payload = body[p:]
            if qos == 1:
                c.send(b'\x40\x02' + struct.pack('>H', pid))
            elif qos == 2:
                c.send(b'\x50\x02' + struct.pack('>H', pid))
            txt = payload.decode(errors='replace')
            if len(txt) > 200:
                txt = txt[:200] + '…'
            log(f'[{c.id}] PUBLISH q{qos}{" retain" if retain else ""} {topic} {txt}')
            HOUSE.command(topic, payload)
            publish(topic, payload, min(qos, 1), retain, c)
        elif t == 6:                                # PUBREL
            c.send(b'\x70\x02' + body[:2])
        elif t in (4, 5, 7):                        # PUBACK/PUBREC/PUBCOMP from the client
            pass
        elif t == 8:                                # SUBSCRIBE
            pid = body[:2]
            p = 2
            codes = bytearray()
            new = []
            while p < len(body):
                n = struct.unpack('>H', body[p:p + 2])[0]
                f = body[p + 2:p + 2 + n].decode(errors='replace')
                q = body[p + 2 + n]
                p += 3 + n
                if not f or ('#' in f and not (f == '#' or f.endswith('/#'))):
                    codes.append(0x80)
                    continue
                g = min(q, 1)
                with LOCK:
                    c.subs[f] = g
                codes.append(g)
                new.append((f, g))
            c.send(bytes([0x90]) + enc_len(2 + len(codes)) + pid + bytes(codes))
            log(f'[{c.id}] SUBSCRIBE ' + ', '.join(f'{f} q{g}' for f, g in new))
            with LOCK:
                ret = list(RETAINED.items())
            for f, g in new:
                for topic, (payload, rq) in ret:
                    if match(f, topic):
                        c.deliver(topic, payload, min(g, rq), True)
        elif t == 10:                               # UNSUBSCRIBE
            pid = body[:2]
            p = 2
            gone = []
            while p < len(body):
                n = struct.unpack('>H', body[p:p + 2])[0]
                f = body[p + 2:p + 2 + n].decode(errors='replace')
                p += 2 + n
                with LOCK:
                    c.subs.pop(f, None)
                gone.append(f)
            c.send(b'\xb0\x02' + pid)
            log(f'[{c.id}] UNSUBSCRIBE ' + ', '.join(gone))
        elif t == 12:                               # PINGREQ
            c.send(b'\xd0\x00')
        elif t == 14:                               # DISCONNECT
            log(f'[{c.id}] disconnect')
            break
        else:
            log(f'[{c.id}] unexpected packet type {t}')
            break
    c.alive = False
    with LOCK:
        CLIENTS.discard(c)
    try:
        sock.close()
    except OSError:
        pass
    if connected:
        log(f'[{c.id}] gone')


class Drift:
    """A value that wanders around a centre, like a real sensor."""

    def __init__(self, centre, spread, step, lo=None, hi=None, dec=1):
        self.v = centre + random.uniform(-spread, spread) / 2
        self.c, self.s, self.st, self.lo, self.hi, self.dec = centre, spread, step, lo, hi, dec

    def __call__(self):
        self.v += random.uniform(-self.st, self.st) + (self.c - self.v) * 0.05
        if self.lo is not None:
            self.v = max(self.lo, self.v)
        if self.hi is not None:
            self.v = min(self.hi, self.v)
        return round(self.v, self.dec) if self.dec else int(round(self.v))


class House:
    ROOMS = ('living', 'bedroom', 'office')

    def __init__(self):
        self.temp = {r: Drift(22 + i, 2, 0.15) for i, r in enumerate(self.ROOMS)}
        self.hum = {r: Drift(50 + 4 * i, 8, 0.6, 20, 90, 0) for i, r in enumerate(self.ROOMS)}
        self.batt = {r: 90 - 7 * i for i, r in enumerate(self.ROOMS)}
        self.lq = {r: Drift(120, 60, 8, 0, 255, 0) for r in self.ROOMS}
        self.plug_on = True
        self.plug_energy = 12.34
        self.plug_pw = Drift(64, 20, 3, 0)
        self.volt = Drift(225, 6, 0.8, dec=1)
        self.house_w = Drift(1250, 500, 60, 150)
        self.solar_w = Drift(900, 600, 40, 0)
        self.kwh = 5.2
        self.water = Drift(0.0, 0.0, 0.0)
        self.door = False
        self.door_batt = 76
        self.t1_on = True
        self.t1_total = 101.542
        self.t1_pw = Drift(38, 12, 2, 0)
        self.relay = False
        self.garage_t = Drift(17.5, 3, 0.1)
        self.wifi = Drift(-61, 10, 1.5, -90, -30, 0)
        self.tick = 0
        self.start = time.time()

    def command(self, topic, payload):
        txt = payload.decode(errors='replace').strip()
        if topic == 'zigbee2mqtt/kitchen_plug/set':
            try:
                st = json.loads(txt).get('state', '')
            except (ValueError, AttributeError):
                st = txt
            st = str(st).upper()
            self.plug_on = (not self.plug_on) if st == 'TOGGLE' else st == 'ON'
            threading.Timer(0.2, self.publish_plug).start()
        elif topic == 'cmnd/plug1/POWER':
            st = txt.upper()
            self.t1_on = (not self.t1_on) if st == 'TOGGLE' else st in ('ON', '1')
            threading.Timer(0.2, lambda: publish('stat/plug1/POWER', 'ON' if self.t1_on else 'OFF')).start()
        elif topic == 'garage-node/switch/relay/command':
            self.relay = txt.upper() == 'ON'
            threading.Timer(0.2, lambda: publish('garage-node/switch/relay/state', 'ON' if self.relay else 'OFF', retain=True)).start()

    def publish_plug(self):
        p = self.plug_pw() if self.plug_on else 0
        publish('zigbee2mqtt/kitchen_plug', json.dumps({
            'state': 'ON' if self.plug_on else 'OFF', 'power': p, 'energy': round(self.plug_energy, 3),
            'voltage': self.volt(), 'current': round(p / 225, 3), 'linkquality': 150}))

    def discovery(self):
        dev = {'identifiers': ['zigbee2mqtt_0x00158d0001'], 'name': 'living_temp', 'manufacturer': 'Aqara',
               'model': 'WSDCGQ11LM'}
        for r in self.ROOMS:
            for k, unit, dc in (('temperature', '°C', 'temperature'), ('humidity', '%', 'humidity')):
                cfg = {'name': f'{r} {k}', 'state_topic': f'zigbee2mqtt/{r}_temp', 'unit_of_measurement': unit,
                       'device_class': dc, 'value_template': '{{ value_json.%s }}' % k,
                       'unique_id': f'{r}_temp_{k}_zigbee2mqtt', 'device': dict(dev, name=f'{r}_temp')}
                publish(f'homeassistant/sensor/{r}_temp/{k}/config', json.dumps(cfg), retain=True)
        publish('homeassistant/switch/kitchen_plug/switch/config', json.dumps({
            'name': 'kitchen plug', 'state_topic': 'zigbee2mqtt/kitchen_plug',
            'command_topic': 'zigbee2mqtt/kitchen_plug/set', 'value_template': '{{ value_json.state }}',
            'payload_on': '{"state":"ON"}', 'payload_off': '{"state":"OFF"}', 'unique_id': 'kitchen_plug_switch'}),
            retain=True)
        publish('homeassistant/binary_sensor/front_door/contact/config', json.dumps({
            'name': 'front door', 'state_topic': 'zigbee2mqtt/front_door', 'device_class': 'door',
            'value_template': '{{ value_json.contact }}', 'payload_on': False, 'payload_off': True}), retain=True)
        publish('homeassistant/sensor/garage-node/temperature/config', json.dumps({
            'name': 'garage temperature', 'state_topic': 'garage-node/sensor/temperature/state',
            'unit_of_measurement': '°C', 'device_class': 'temperature', 'unique_id': 'garage_node_temp',
            'availability_topic': 'garage-node/status'}), retain=True)
        publish('zigbee2mqtt/bridge/state', '{"state":"online"}', retain=True)
        publish('zigbee2mqtt/bridge/info', json.dumps({'version': '1.40.2', 'coordinator': {'type': 'zStack3x0'},
                                                       'network': {'channel': 15, 'pan_id': 6754}}), retain=True)
        publish('tele/plug1/LWT', 'Online', retain=True)
        publish('garage-node/status', 'online', retain=True)
        publish('garage-node/switch/relay/state', 'OFF', retain=True)

    def step(self):
        self.tick += 1
        n = self.tick
        for i, r in enumerate(self.ROOMS):
            if (n + i) % 2 == 0:
                publish(f'zigbee2mqtt/{r}_temp', json.dumps({
                    'temperature': self.temp[r](), 'humidity': self.hum[r](), 'battery': self.batt[r],
                    'linkquality': self.lq[r]()}))
        if self.plug_on:
            self.plug_energy += self.plug_pw.v / 3600000 * ARGS.rate * 60
        self.publish_plug()
        w = self.house_w()
        s = self.solar_w() * (0.5 + 0.5 * math.sin((time.time() - self.start) / 60))
        publish('home/power/w', str(w))
        publish('home/solar/w', str(int(max(0, s))))
        self.kwh += w / 3600000 * ARGS.rate * 60
        publish('home/power/kwh_today', f'{self.kwh:.3f}')
        flow = max(0.0, 6 * math.sin(n / 7)) if n % 40 < 12 else 0.0
        publish('home/water/l_min', f'{flow:.1f}')
        if n % 15 == 0:
            self.door = not self.door
            publish('zigbee2mqtt/front_door', json.dumps({'contact': not self.door, 'battery': self.door_batt,
                                                          'linkquality': 87}))
        if n % 10 == 0:
            p = self.t1_pw() if self.t1_on else 0
            self.t1_total += p / 3600000 * 10 * ARGS.rate * 60
            publish('tele/plug1/SENSOR', json.dumps({
                'Time': time.strftime('%Y-%m-%dT%H:%M:%S'),
                'ENERGY': {'TotalStartTime': '2026-01-10T12:00:00', 'Total': round(self.t1_total, 3),
                           'Yesterday': 0.412, 'Today': 0.128, 'Power': p, 'ApparentPower': p + 4,
                           'ReactivePower': 12, 'Factor': 0.92, 'Voltage': int(self.volt()),
                           'Current': round(p / 225, 3)}}))
            publish('tele/plug1/STATE', json.dumps({
                'Time': time.strftime('%Y-%m-%dT%H:%M:%S'), 'Uptime': '0T%02d:%02d:%02d' % ((n // 3600) % 24, (n // 60) % 60, n % 60),
                'Heap': 25, 'LoadAvg': 19, 'POWER': 'ON' if self.t1_on else 'OFF',
                'Wifi': {'AP': 1, 'SSId': 'casa', 'RSSI': 76, 'Signal': self.wifi(), 'Channel': 6}}))
            publish('stat/plug1/POWER', 'ON' if self.t1_on else 'OFF')
        if n % 5 == 0:
            publish('garage-node/sensor/temperature/state', f'{self.garage_t():.1f}')
            publish('garage-node/sensor/wifi_signal/state', str(self.wifi()))
            publish('garage-node/sensor/uptime/state', str(int(time.time() - self.start)))
        if n % 30 == 0:
            publish('garage-node/debug', f'[D][sensor:094]: \'garage temperature\': Sending state {self.garage_t.v:.2f} °C')


HOUSE = None


def house_loop():
    HOUSE.discovery()
    while True:
        time.sleep(ARGS.rate)
        try:
            HOUSE.step()
        except Exception as e:     # a client gone mid-send must not stop the house
            log('house:', e)


def main():
    global ARGS, HOUSE
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--port', type=int, default=1883)
    ap.add_argument('--bind', default='127.0.0.1')
    ap.add_argument('--user')
    ap.add_argument('--password')
    ap.add_argument('--rate', type=float, default=1.0, help='seconds between house updates')
    ap.add_argument('--no-house', action='store_true')
    ap.add_argument('--quiet', action='store_true', help='do not print client publishes')
    ARGS = ap.parse_args()
    if ARGS.password is not None and ARGS.user is None:
        ARGS.user = ''
    HOUSE = House()
    if ARGS.quiet:
        global log
        real = log
        log = lambda *a: None if a and 'PUBLISH' in str(a[0]) else real(*a)   # noqa: E731
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((ARGS.bind, ARGS.port))
    srv.listen(8)
    log(f'fake MQTT broker on {ARGS.bind}:{ARGS.port}' + (f' (user {ARGS.user!r})' if ARGS.user is not None else ''))
    if not ARGS.no_house:
        threading.Thread(target=house_loop, daemon=True).start()
    try:
        while True:
            s, addr = srv.accept()
            s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            c = Client(s, addr)
            with LOCK:
                CLIENTS.add(c)
            threading.Thread(target=handle, args=(c,), daemon=True).start()
    except KeyboardInterrupt:
        pass


if __name__ == '__main__':
    main()
