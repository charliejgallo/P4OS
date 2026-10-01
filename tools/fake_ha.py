#!/usr/bin/env python3
"""
A make-believe Home Assistant for P4OS's HA app: the WebSocket API only, on
ws://localhost:8123/api/websocket, with a house full of entities that answer
service calls and sensors that drift, so the app can be designed and tested
without touching a real installation or a real token.

    tools/fake_ha.py [--port 8123] [--token test-token] [--slow-states 0.0]

Speaks the parts of the protocol the app uses: auth, get_config, get_states,
config/area_registry/list, config/device_registry/list,
config/entity_registry/list_for_display, subscribe_events (state_changed),
unsubscribe_events, call_service, ping. Anything else gets an
"unknown_command" error, as HA does. The standard library only: the
WebSocket framing is written out below.

wss:// (HA behind Nabu Casa or a reverse proxy), tested with no outside
server: --tls --cert server.pem --key server.key serves the same thing over
TLS, and the simulator trusts a throwaway CA through P4_SIM_EXTRA_CA (sim
only, on top of /etc/ssl/cert.pem). Make the certificates in a scratch dir,
never in the repo:

    D=/some/scratch/dir/ha-tls; mkdir -p $D && cd $D
    # the CA
    openssl req -x509 -newkey rsa:2048 -nodes -days 30 -subj "/CN=P4OS test CA" \\
        -keyout ca.key -out ca.pem \\
        -addext basicConstraints=critical,CA:TRUE -addext keyUsage=critical,keyCertSign,cRLSign
    # the server, for 127.0.0.1 and localhost. 127.0.0.1 goes in twice, as IP
    # and as DNS: the simulator's mbedtls (Homebrew 3.4) does not match IP
    # entries, the board's (IDF 3.6) does.
    openssl req -newkey rsa:2048 -nodes -subj "/CN=127.0.0.1" -keyout server.key -out server.csr
    printf 'subjectAltName=IP:127.0.0.1,DNS:127.0.0.1,DNS:localhost\\nbasicConstraints=CA:FALSE\\n'\\
    'keyUsage=digitalSignature,keyEncipherment\\nextendedKeyUsage=serverAuth\\n' > server.ext
    openssl x509 -req -in server.csr -CA ca.pem -CAkey ca.key -CAcreateserial -days 30 \\
        -extfile server.ext -out server.pem
    # optional: a certificate from the same CA for another name, to see a
    # wrong hostname refused (serve it and connect to 127.0.0.1)
    openssl req -newkey rsa:2048 -nodes -subj "/CN=ha.example.com" -keyout other.key -out other.csr
    printf 'subjectAltName=DNS:ha.example.com\\nextendedKeyUsage=serverAuth\\n' > other.ext
    openssl x509 -req -in other.csr -CA ca.pem -CAkey ca.key -CAcreateserial -days 30 \\
        -extfile other.ext -out other.pem

    # from the repo root
    tools/fake_ha.py --tls --port 8443 --cert $D/server.pem --key $D/server.key
    # sim/sim_fs/ha.txt: url=https://127.0.0.1:8443 and token=test-token
    cd sim && P4_SIM_EXTRA_CA=$D/ca.pem ./build/p4os_sim
"""
import argparse
import asyncio
import base64
import hashlib
import json
import math
import random
import struct
import time

GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC11B85"

AREAS = [("living", "Living"), ("cocina", "Cocina"), ("dormitorio", "Dormitorio"),
         ("taller", "Taller"), ("patio", "Patio"), ("oficina", "Oficina")]

# entity_id: (area, state, attributes)
def house():
    now = time.time()
    e = {}
    def add(eid, area, state, **attrs):
        e[eid] = {"area": area, "state": state, "attributes": attrs, "last_changed": now}
    add("light.living_techo", "living", "on", friendly_name="Luz del techo", brightness=180,
        supported_color_modes=["brightness"], color_mode="brightness")
    add("light.living_pie", "living", "off", friendly_name="Lámpara de pie", supported_color_modes=["onoff"])
    add("light.cocina", "cocina", "on", friendly_name="Cocina", brightness=255, supported_color_modes=["brightness"])
    add("light.dormitorio", "dormitorio", "off", friendly_name="Velador", supported_color_modes=["brightness"])
    add("light.taller_banco", "taller", "on", friendly_name="Luz del banco", brightness=230,
        supported_color_modes=["brightness"])
    add("light.patio_guirnalda", "patio", "off", friendly_name="Guirnalda", supported_color_modes=["onoff"])
    add("switch.cafetera", "cocina", "off", friendly_name="Cafetera", icon="mdi:coffee-maker")
    add("switch.taller_fuente", "taller", "on", friendly_name="Toma de la fuente", device_class="outlet")
    add("switch.taller_soldador", "taller", "off", friendly_name="Soldador", device_class="outlet")
    add("input_boolean.modo_noche", None, "off", friendly_name="Modo noche")
    add("fan.dormitorio", "dormitorio", "on", friendly_name="Ventilador", percentage=66, percentage_step=33.3)
    add("cover.living_persiana", "living", "open", friendly_name="Persiana", current_position=60,
        device_class="shutter", supported_features=15)
    add("cover.garage", None, "closed", friendly_name="Portón", device_class="garage", supported_features=3)
    add("climate.living_aire", "living", "cool", friendly_name="Aire del living", current_temperature=26.5,
        temperature=24, min_temp=16, max_temp=30, target_temp_step=0.5,
        hvac_modes=["off", "cool", "heat", "dry", "fan_only", "auto"], hvac_action="cooling")
    add("climate.oficina", "oficina", "off", friendly_name="Calefactor", current_temperature=19.2,
        temperature=21, min_temp=7, max_temp=35, target_temp_step=0.5, hvac_modes=["off", "heat"], hvac_action="off")
    add("sensor.living_temperatura", "living", "23.4", friendly_name="Temperatura",
        unit_of_measurement="°C", device_class="temperature", state_class="measurement")
    add("sensor.living_humedad", "living", "55", friendly_name="Humedad",
        unit_of_measurement="%", device_class="humidity")
    add("sensor.patio_temperatura", "patio", "17.8", friendly_name="Temperatura exterior",
        unit_of_measurement="°C", device_class="temperature")
    add("sensor.taller_consumo", "taller", "412", friendly_name="Consumo del banco",
        unit_of_measurement="W", device_class="power")
    add("sensor.taller_riden_tension", "taller", "12.00", friendly_name="Riden: tensión",
        unit_of_measurement="V", device_class="voltage")
    add("sensor.energia_hoy", None, "6.42", friendly_name="Energía hoy",
        unit_of_measurement="kWh", device_class="energy")
    add("sensor.oficina_co2", "oficina", "640", friendly_name="CO₂", unit_of_measurement="ppm",
        device_class="carbon_dioxide")
    add("binary_sensor.puerta_entrada", None, "off", friendly_name="Puerta de entrada", device_class="door")
    add("binary_sensor.patio_movimiento", "patio", "off", friendly_name="Movimiento", device_class="motion")
    add("binary_sensor.taller_ventana", "taller", "on", friendly_name="Ventana", device_class="window")
    add("scene.cine", "living", "2026-09-27T21:00:00+00:00", friendly_name="Cine")
    add("scene.buenas_noches", None, "unknown", friendly_name="Buenas noches")
    add("script.regar_patio", "patio", "off", friendly_name="Regar el patio")
    add("lock.puerta_entrada", None, "locked", friendly_name="Cerradura")
    add("media_player.living_tv", "living", "playing", friendly_name="Tele", media_title="Noticias",
        volume_level=0.3)
    add("button.taller_reset_esp", "taller", "unknown", friendly_name="Reiniciar ESP del banco")
    # what the app must leave out
    add("sensor.cafetera_senal", "cocina", "-61", friendly_name="Cafetera señal", unit_of_measurement="dBm")
    add("update.cafetera_firmware", "cocina", "off", friendly_name="Firmware")
    add("light.oculta", "living", "off", friendly_name="Luz oculta")
    add("sensor.unavailable_one", "oficina", "unavailable", friendly_name="Sensor sin pila",
        unit_of_measurement="°C", device_class="temperature")
    return e

REGISTRY_EXTRA = {
    "sensor.cafetera_senal": {"ec": 1},     # diagnostic
    "light.oculta": {"hb": True},            # hidden
}


class House:
    def __init__(self):
        self.e = house()
        self.subs = []  # (queue, sub_id)
        self.notif_subs = []
        self.notifications = {"bienvenida": {"notification_id": "bienvenida", "title": "Home Assistant",
                              "message": "Conectado al **HA falso** de P4OS.", "created_at": "2026-09-28T10:00:00+00:00"}}

    def history(self, m):
        """A made-up past for numeric sensors: a slow wave plus noise, a point every 5 minutes."""
        start = time.time() - 24 * 3600
        try:
            from datetime import datetime
            start = datetime.fromisoformat(m.get("start_time")).timestamp()
        except Exception:
            pass
        out = {}
        for eid in m.get("entity_ids", []):
            if eid not in self.e:
                continue
            try:
                now_v = float(self.e[eid]["state"])
            except ValueError:
                out[eid] = [{"s": self.e[eid]["state"], "lu": start}]
                continue
            pts, t = [], start
            rnd = random.Random(eid)
            while t < time.time():
                x = (t - start) / 3600
                v = now_v + now_v * 0.08 * math.sin(x / 3.8) + rnd.uniform(-1, 1) * now_v * 0.01
                pts.append({"s": "%.2f" % v, "lu": t})
                t += 300
            pts.append({"s": self.e[eid]["state"], "lu": time.time()})
            out[eid] = pts
        return out

    def notify(self, nid, title, message):
        n = {"notification_id": nid, "title": title, "message": message,
             "created_at": time.strftime("%Y-%m-%dT%H:%M:%S+00:00", time.gmtime())}
        self.notifications[nid] = n
        for q, sid in self.notif_subs:
            q.put_nowait({"id": sid, "type": "event", "event": {"type": "added", "notifications": {nid: n}}})

    def fire(self, event_type, data):
        for q, sid in self.subs_by_type.get(event_type, []):
            q.put_nowait({"id": sid, "type": "event", "event": {"event_type": event_type, "data": data,
                          "origin": "LOCAL", "time_fired": time.strftime("%Y-%m-%dT%H:%M:%S+00:00", time.gmtime())}})

    subs_by_type = {}

    def state_obj(self, eid):
        s = self.e[eid]
        iso = time.strftime("%Y-%m-%dT%H:%M:%S+00:00", time.gmtime(s["last_changed"]))
        return {"entity_id": eid, "state": s["state"], "attributes": s["attributes"],
                "last_changed": iso, "last_updated": iso, "context": {"id": "x"}}

    def set(self, eid, state=None, **attrs):
        s = self.e[eid]
        old = self.state_obj(eid)
        if state is not None and state != s["state"]:
            s["state"] = state
            s["last_changed"] = time.time()
        for k, v in attrs.items():
            if v is None:
                s["attributes"].pop(k, None)
            else:
                s["attributes"][k] = v
        new = self.state_obj(eid)
        for q, sid in self.subs:
            q.put_nowait({"id": sid, "type": "event", "event": {
                "event_type": "state_changed", "data": {"entity_id": eid, "old_state": old, "new_state": new},
                "origin": "LOCAL", "time_fired": new["last_updated"], "context": {"id": "x"}}})

    def dismiss(self, nid):
        n = self.notifications.pop(nid, None)
        if n is None:
            return
        print("notification %r dismissed" % nid)
        for q, sid in self.notif_subs:
            q.put_nowait({"id": sid, "type": "event", "event": {"type": "removed", "notifications": {nid: n}}})

    def service(self, domain, service, target, data):
        if domain == "persistent_notification" and service == "dismiss":
            self.dismiss(data.get("notification_id", ""))
            return
        ids = target.get("entity_id") or data.get("entity_id") or []
        if isinstance(ids, str):
            ids = [ids]
        for eid in ids:
            if eid not in self.e:
                raise KeyError(eid)
            st = self.e[eid]["state"]
            a = self.e[eid]["attributes"]
            dom = eid.split(".")[0]
            on = st in ("on", "open", "playing", "unlocked")
            if service == "toggle":
                service = "turn_off" if on else "turn_on"
                if dom == "cover":
                    service = "close_cover" if st == "open" else "open_cover"
            if dom == "light":
                if service == "turn_on":
                    b = a.get("brightness", 255)
                    if "brightness_pct" in data:
                        b = round(int(data["brightness_pct"]) * 255 / 100)
                    if "brightness" in data:
                        b = int(data["brightness"])
                    extra = {"brightness": b} if "brightness" in a.get("supported_color_modes", []) else {}
                    self.set(eid, "on", **extra)
                else:
                    self.set(eid, "off", brightness=None)
            elif dom in ("switch", "input_boolean", "fan", "script", "automation"):
                if dom == "fan" and service == "set_percentage":
                    p = int(data.get("percentage", 0))
                    self.set(eid, "on" if p else "off", percentage=p)
                elif dom == "script" and service == "turn_on":
                    self.set(eid, "on")
                    asyncio.get_event_loop().call_later(3, lambda eid=eid: self.set(eid, "off"))
                else:
                    self.set(eid, "on" if service == "turn_on" else "off")
            elif dom == "cover":
                if service == "open_cover":
                    self.set(eid, "open", current_position=100 if "current_position" in a else None)
                elif service == "close_cover":
                    self.set(eid, "closed", current_position=0 if "current_position" in a else None)
                elif service == "set_cover_position":
                    p = int(data.get("position", 0))
                    self.set(eid, "open" if p else "closed", current_position=p)
            elif dom == "climate":
                if service == "set_temperature":
                    self.set(eid, None, temperature=float(data["temperature"]))
                elif service == "set_hvac_mode":
                    m = data["hvac_mode"]
                    self.set(eid, m, hvac_action="off" if m == "off" else ("cooling" if m == "cool" else "heating"))
                elif service in ("turn_on", "turn_off"):
                    self.set(eid, "off" if service == "turn_off" else a["hvac_modes"][1])
            elif dom in ("scene", "button"):
                self.set(eid, time.strftime("%Y-%m-%dT%H:%M:%S+00:00", time.gmtime()))
            elif dom == "lock":
                self.set(eid, "locked" if service == "lock" else "unlocked")
            elif dom == "media_player":
                if service == "media_play_pause":
                    self.set(eid, "paused" if st == "playing" else "playing")
                elif service in ("turn_on", "turn_off"):
                    self.set(eid, "on" if service == "turn_on" else "off")
            print("  call %s.%s %s %s" % (domain, service, eid, data or ""))

    async def drift(self):
        n = 0
        while True:
            await asyncio.sleep(3)
            n += 1
            if n == 3:
                self.notify("lavarropas", "Lavarropas", "Terminó el lavado. Sacá la ropa antes de que se arrugue.")
            if n == 5:
                self.fire("p4os_notify", {"title": "Taller", "message": "La fuente del banco lleva 2 h encendida."})
            for eid, lo, hi, nd in (("sensor.living_temperatura", 21, 26, 1), ("sensor.taller_consumo", 60, 900, 0),
                                    ("sensor.patio_temperatura", 12, 22, 1), ("sensor.oficina_co2", 420, 1200, 0)):
                v = float(self.e[eid]["state"]) + random.uniform(-1, 1) * (hi - lo) / 20
                v = min(hi, max(lo, v))
                self.set(eid, ("%.*f" % (nd, v)))
            if random.random() < 0.15:
                m = "binary_sensor.patio_movimiento"
                self.set(m, "off" if self.e[m]["state"] == "on" else "on")


def frame(payload: bytes, opcode=1) -> bytes:
    n = len(payload)
    if n < 126:
        h = struct.pack("!BB", 0x80 | opcode, n)
    elif n < 65536:
        h = struct.pack("!BBH", 0x80 | opcode, 126, n)
    else:
        h = struct.pack("!BBQ", 0x80 | opcode, 127, n)
    return h + payload


async def read_frame(r: asyncio.StreamReader):
    b0, b1 = await r.readexactly(2)
    op, n = b0 & 0x0F, b1 & 0x7F
    if n == 126:
        n = struct.unpack("!H", await r.readexactly(2))[0]
    elif n == 127:
        n = struct.unpack("!Q", await r.readexactly(8))[0]
    mask = await r.readexactly(4) if b1 & 0x80 else b"\0\0\0\0"
    data = bytearray(await r.readexactly(n))
    for i in range(n):
        data[i] ^= mask[i & 3]
    return op, bytes(data)


async def client(r, w, h: House, args):
    peer = w.get_extra_info("peername")
    req = await r.readuntil(b"\r\n\r\n")
    lines = req.decode("latin1").split("\r\n")
    hdr = {l.split(":", 1)[0].lower(): l.split(":", 1)[1].strip() for l in lines[1:] if ":" in l}
    if not lines[0].startswith("GET /api/websocket") or "sec-websocket-key" not in hdr:
        w.write(b"HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n")
        await w.drain()
        w.close()
        return
    acc = base64.b64encode(hashlib.sha1((hdr["sec-websocket-key"] + GUID).encode()).digest()).decode()
    w.write(("HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
             "Sec-WebSocket-Accept: %s\r\n\r\n" % acc).encode())
    print("%s connected" % (peer,))
    out = asyncio.Queue()

    async def writer():
        while True:
            m = await out.get()
            if m is None:
                return
            data = json.dumps(m, ensure_ascii=False).encode()
            w.write(frame(data))
            await w.drain()

    wt = asyncio.create_task(writer())
    out.put_nowait({"type": "auth_required", "ha_version": "2026.9.2"})
    authed = False
    my_subs = []
    try:
        while True:
            op, data = await read_frame(r)
            if op == 8:
                break
            if op == 9:
                w.write(frame(data, 0xA))
                continue
            if op != 1:
                continue
            m = json.loads(data)
            t = m.get("type")
            if not authed:
                if t == "auth" and m.get("access_token") == args.token:
                    authed = True
                    out.put_nowait({"type": "auth_ok", "ha_version": "2026.9.2"})
                    print("  auth ok")
                else:
                    out.put_nowait({"type": "auth_invalid", "message": "Invalid access token or password"})
                    print("  auth refused")
                    await asyncio.sleep(0.2)
                    break
                continue
            mid = m.get("id")
            def ok(result=None):
                out.put_nowait({"id": mid, "type": "result", "success": True, "result": result})
            def err(code, msg):
                out.put_nowait({"id": mid, "type": "result", "success": False, "error": {"code": code, "message": msg}})
            print("  <- %s" % t)
            if t == "ping":
                out.put_nowait({"id": mid, "type": "pong"})
            elif t == "get_config":
                ok({"location_name": "Casa", "unit_system": {"temperature": "°C"}, "version": "2026.9.2",
                    "time_zone": "America/Argentina/Buenos_Aires"})
            elif t == "get_states":
                if args.slow_states:
                    await asyncio.sleep(args.slow_states)
                ok([h.state_obj(e) for e in h.e])
            elif t == "config/area_registry/list":
                ok([{"area_id": a, "name": n, "icon": None, "floor_id": None} for a, n in AREAS])
            elif t == "config/device_registry/list":
                ok([{"id": "dev_" + e.split(".")[1], "area_id": h.e[e]["area"], "name": h.e[e]["attributes"]["friendly_name"]}
                    for e in h.e])
            elif t == "config/entity_registry/list_for_display":
                ents = []
                for e in h.e:
                    d = {"ei": e, "di": "dev_" + e.split(".")[1]}
                    d.update(REGISTRY_EXTRA.get(e, {}))
                    ents.append(d)
                ok({"entity_categories": {"0": "config", "1": "diagnostic"}, "entities": ents})
            elif t == "subscribe_events":
                sub = (out, mid)
                et = m.get("event_type", "state_changed")
                if et == "state_changed":
                    h.subs.append(sub)
                else:
                    House.subs_by_type.setdefault(et, []).append(sub)
                my_subs.append(sub)
                ok(None)
            elif t == "unsubscribe_events":
                for s in list(my_subs):
                    if s[1] == m.get("subscription"):
                        h.subs.remove(s)
                        my_subs.remove(s)
                ok(None)
            elif t == "history/history_during_period":
                ok(h.history(m))
            elif t == "persistent_notification/subscribe":
                sub = (out, mid)
                h.notif_subs.append(sub)
                my_subs.append(sub)
                ok(None)
                out.put_nowait({"id": mid, "type": "event", "event": {"type": "current", "notifications": h.notifications}})
            elif t == "call_service":
                try:
                    h.service(m["domain"], m["service"], m.get("target", {}), m.get("service_data", {}))
                    ok({"context": {"id": "x"}})
                except KeyError as ex:
                    err("not_found", "Entity %s not found" % ex)
            else:
                err("unknown_command", "Unknown command.")
    except (asyncio.IncompleteReadError, ConnectionError):
        pass
    finally:
        for s in my_subs:
            for lst in [h.subs, h.notif_subs] + list(House.subs_by_type.values()):
                if s in lst:
                    lst.remove(s)
        out.put_nowait(None)
        await wt
        w.close()
        print("%s gone" % (peer,))


async def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8123)
    ap.add_argument("--token", default="test-token")
    ap.add_argument("--slow-states", type=float, default=0.0, help="seconds to delay get_states")
    ap.add_argument("--tls", action="store_true", help="serve wss:// (needs --cert and --key)")
    ap.add_argument("--cert", help="server certificate chain, PEM (with --tls)")
    ap.add_argument("--key", help="server private key, PEM (with --tls)")
    args = ap.parse_args()
    ctx = None
    if args.tls:
        import ssl
        if not args.cert or not args.key:
            ap.error("--tls needs --cert and --key (see the docstring for the openssl commands)")
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(args.cert, args.key)
    h = House()
    srv = await asyncio.start_server(lambda r, w: client(r, w, h, args), "127.0.0.1", args.port, ssl=ctx)
    print("fake Home Assistant on %s://127.0.0.1:%d/api/websocket (token %r)"
          % ("wss" if ctx else "ws", args.port, args.token))
    asyncio.create_task(h.drift())
    async with srv:
        await srv.serve_forever()


if __name__ == "__main__":
    asyncio.run(main())
