#!/usr/bin/env python3
"""Vuelos: a fake ADS-B server for the simulator's tests.

Invented aircraft fly around a point, and every service the app talks to
answers in its own shape, so the app is tested without asking any of the
real ones:

  /v2/point/<lat>/<lon>/<nm>              airplanes.live
  /v2/lat/<lat>/lon/<lon>/dist/<nm>       adsb.lol (x-api-key is logged)
  /api/states/all?lamin=&lomin=&...       OpenSky (rows of 18 values)
  POST /auth/realms/opensky-network/protocol/openid-connect/token
                                          OpenSky's OAuth2 client credentials
  /data/aircraft.json                     readsb / dump1090 on one's own network
  /v0/callsign/<callsign>                 adsbdb's routes

Run it, then point the simulator at it:

  python3 apps/vuelos/tools/fake_adsb.py --port 8946 --lat 40.4168 --lon -3.7038
  VUELOS_FAKE=http://127.0.0.1:8946 ./sim/build-vuelos/p4os_sim

(the receiver is reached by its own address: url=http://127.0.0.1:8946/data/aircraft.json)

Everything here is invented: the callsigns, the registrations and the
routes. It logs every request with the time since the last one of the same
service, answers 429 to airplanes.live faster than one a second, and 426 to
adsb.lol asked in HTTP/1.0, as the real ones do.
"""
import argparse
import json
import math
import random
import re
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

KT = 0.514444
CITIES = [("MAD", "Madrid"), ("LIS", "Lisbon"), ("CDG", "Paris"), ("FCO", "Rome"), ("LHR", "London"),
          ("BCN", "Barcelona"), ("EZE", "Buenos Aires"), ("BOG", "Bogota"), ("AMS", "Amsterdam"),
          ("FRA", "Frankfurt am Main"), ("MUC", "Munich"), ("ZRH", "Zurich"), ("ATH", "Athens"), ("DUB", "Dublin"),
          ("OPO", "Porto"), ("SVQ", "Seville"), ("AGP", "Malaga"), ("PMI", "Palma De Mallorca"), ("TFN", "Tenerife"),
          ("GRU", "Sao Paulo"), ("MEX", "Mexico City"), ("JFK", "New York"), ("MIA", "Miami")]  # adsbdb's municipalities are English
PREFIX = ["ZVA", "ZKN", "ZPR", "ZTL", "ZMA", "ZQE"]
#                type   cat   kind      kt   alt range
TYPES = [("A320", "A3", "jet", 440, (24000, 38000)), ("B738", "A3", "jet", 450, (26000, 39000)),
         ("A21N", "A3", "jet", 450, (30000, 38000)), ("A359", "A5", "heavy", 490, (34000, 41000)),
         ("B77W", "A5", "heavy", 500, (33000, 40000)), ("E190", "A3", "prop", 420, (22000, 35000)),
         ("AT76", "A2", "prop", 270, (14000, 22000)), ("CRJ9", "A3", "prop", 430, (25000, 35000)),
         ("C172", "A1", "light", 105, (2000, 5500)), ("P28A", "A1", "light", 110, (2500, 6000))]


def dest(lat, lon, brg, km):
    """The point km away along brg (degrees), on a sphere."""
    r = 6371.0
    la, lo, b = math.radians(lat), math.radians(lon), math.radians(brg)
    d = km / r
    la2 = math.asin(math.sin(la) * math.cos(d) + math.cos(la) * math.sin(d) * math.cos(b))
    lo2 = lo + math.atan2(math.sin(b) * math.sin(d) * math.cos(la), math.cos(d) - math.sin(la) * math.sin(la2))
    return math.degrees(la2), (math.degrees(lo2) + 540) % 360 - 180


def dist_km(lat0, lon0, lat, lon):
    c = math.cos(math.radians((lat0 + lat) / 2))
    return math.hypot((lon - lon0) * 111.32 * c, (lat - lat0) * 110.57)


class Sky:
    """The invented traffic: straight lines across a disc of radius R, each
    one coming back from the other side, plus a few that stay put."""

    def __init__(self, lat, lon, n, radius_km, seed, emergency):
        self.lat, self.lon, self.R = lat, lon, radius_km
        self.t0 = time.time()
        rnd = random.Random(seed)
        self.ac = []
        for i in range(n):
            t, cat, kind, kt, (a0, a1) = rnd.choice(TYPES)
            light = kind == "light"
            cs = f"{rnd.choice(PREFIX)}{rnd.randint(10, 9899)}" if not light else ""
            reg = "EC-" + "".join(rnd.choice("ABCDEFGHJKLMNPRSTUVWXYZ") for _ in range(3))
            self.ac.append({
                "hex": f"{0x340000 + rnd.randint(0, 0xFFFF):06x}", "flight": cs, "r": reg, "t": t, "category": cat,
                "kt": kt * rnd.uniform(0.9, 1.08), "trk": rnd.uniform(0, 360),
                "off": rnd.uniform(-0.9, 0.9) * (radius_km * (0.25 if light else 0.95)),
                "phase": rnd.uniform(0, 1), "alt": rnd.randint(a0 // 100, a1 // 100) * 100,
                "vr": 0, "sq": f"{rnd.randint(0, 7)}{rnd.randint(0, 7)}{rnd.randint(0, 7)}{rnd.randint(0, 7)}",
                "kind": "line", "span": radius_km * (0.5 if light else 2.0),
            })
        # arrivals and departures through the centre, low and changing height
        for k in range(4):
            a = self.ac[k]
            t, cat, _, kt, _ = TYPES[k % 5]
            a.update(t=t, category=cat, kt=kt * 0.6, off=rnd.uniform(-3, 3), kind="approach", alt=0, span=200,
                     trk=(rnd.choice([180, 0, 330, 150]) + rnd.uniform(-8, 8)) % 360,
                     flight=a["flight"] or f"{rnd.choice(PREFIX)}{rnd.randint(10, 9899)}")
        # one that passes right overhead, low
        a = self.ac[4]
        a.update(t="A320", category="A3", kt=180, off=0.4, phase=0.35, alt=2800, vr=-700, kind="line",
                 span=60, flight="ZVA316", trk=95)
        # a helicopter going round a hospital, a glider in a thermal, a balloon
        self.ac += [
            {"hex": "34f0a1", "flight": "", "r": "EC-HLP", "t": "EC35", "category": "A7", "kt": 90, "kind": "circle",
             "cx": 4.0, "cy": -2.5, "rad": 2.2, "alt": 1500, "vr": 0, "sq": "0020", "phase": 0, "trk": 0, "off": 0},
            {"hex": "34f0b2", "flight": "", "r": "EC-GLD", "t": "GLID", "category": "B1", "kt": 50, "kind": "circle",
             "cx": -18.0, "cy": 22.0, "rad": 0.6, "alt": 5200, "vr": 300, "sq": "7000", "phase": 0, "trk": 0, "off": 0},
            {"hex": "34f0c3", "flight": "", "r": "EC-BLN", "t": "BALL", "category": "B2", "kt": 9, "kind": "line",
             "off": -14.0, "alt": 3100, "vr": 0, "sq": "7000", "phase": 0.5, "trk": 60, "span": 40},
            {"hex": "ae01c7", "flight": "ZMIL21", "r": "", "t": "A400", "category": "A5", "kt": 300, "kind": "line",
             "off": 25.0, "alt": 21000, "vr": 0, "sq": "4617", "phase": 0.2, "trk": 250, "span": 300, "mil": 1},
        ]
        if emergency:
            e = self.ac[6]
            e.update(sq="7700", t="A320", category="A3", kt=300, alt=16000, vr=-1800, kind="line",
                     flight=e["flight"] or "ZQE777")
        self.routes = {}
        for a in self.ac:
            if a["flight"] and rnd.random() < 0.85:
                o, d = rnd.sample(CITIES, 2)
                if rnd.random() < 0.5:
                    o = CITIES[0]
                else:
                    d = CITIES[0]
                self.routes[a["flight"]] = (o, d)

    def state(self, a, now):
        el = now - self.t0
        if a["kind"] == "circle":
            w = a["kt"] * KT / 1000 / a["rad"]            # rad/s
            th = el * w + a["phase"]
            cx, cy = dest(self.lat, self.lon, 90, a["cx"])
            cx, cy = dest(cx, cy, 0, a["cy"])
            lat, lon = dest(cx, cy, math.degrees(th), a["rad"])
            trk = (math.degrees(th) + 90) % 360
            return lat, lon, trk, a["alt"] + 40 * math.sin(el / 30), a["vr"]
        span = a["span"]
        km_s = a["kt"] * KT / 1000
        s = ((a["phase"] * span + el * km_s) % span) - span / 2   # along the line, through the centre
        base_lat, base_lon = dest(self.lat, self.lon, a["trk"] + 90, a["off"])
        lat, lon = dest(base_lat, base_lon, a["trk"], s)
        alt, vr = a["alt"], a["vr"]
        if a["kind"] == "approach":
            # climbing out after the centre, descending before it: ~300 ft a km
            alt = max(0, min(36000, abs(s) * 330))
            vr = int((1 if s > 0 else -1) * a["kt"] * KT / 1000 * 330 * 60)
            if alt < 50:
                vr = 0
        elif a["vr"]:
            alt = max(500, a["alt"] + a["vr"] / 60 * (s / km_s if km_s else 0))
        return lat, lon, a["trk"], alt, vr

    def near(self, lat, lon, km):
        now = time.time()
        out = []
        for a in self.ac:
            la, lo, trk, alt, vr = self.state(a, now)
            d = dist_km(lat, lon, la, lo)
            if d <= km:
                out.append((a, la, lo, trk, alt, vr, d))
        return out


def v2(a, la, lo, trk, alt, vr, d, readsb=False):
    ground = alt < 30
    o = {"hex": a["hex"], "type": "adsb_icao", "flight": (a["flight"] + "        ")[:8]}
    if not readsb or a.get("r"):
        if a.get("r"):
            o["r"] = a["r"]
        o["t"] = a["t"]
    o.update({"alt_baro": "ground" if ground else int(alt), "alt_geom": int(alt + 125), "gs": round(a["kt"], 1),
              "track": round(trk, 1), "baro_rate": int(vr) // 64 * 64, "squawk": a["sq"], "emergency":
              "general" if a["sq"] == "7700" else "none", "category": a["category"], "lat": round(la, 6),
              "lon": round(lo, 6), "nic": 8, "seen_pos": round(random.uniform(0.1, 2.5), 1), "seen": 0.3,
              "rssi": -12.5, "messages": 1234})
    if not readsb:
        o["dbFlags"] = a.get("mil", 0)
        o["dst"] = round(d / 1.852, 3)
    return o


def opensky(a, la, lo, trk, alt, vr, now):
    cat = {"A1": 2, "A2": 3, "A3": 4, "A5": 6, "A7": 8, "B1": 9, "B2": 10}.get(a["category"], 1)
    ground = alt < 30
    return [a["hex"], (a["flight"] + "        ")[:8], "Spain", int(now) - 2, int(now) - 1, round(lo, 4), round(la, 4),
            None if ground else round(alt * 0.3048, 1), ground, round(a["kt"] * KT, 2), round(trk, 1),
            round(vr * 0.3048 / 60, 2), None, round((alt + 125) * 0.3048, 1), a["sq"], False, 0, cat]


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    chunked = False
    keep = False
    refuse = False
    sky = None
    last = {}
    token = None
    lock = threading.Lock()

    def log_message(self, fmt, *args):
        pass

    def note(self, what, extra=""):
        with self.lock:
            now = time.time()
            prev = self.last.get(what)
            self.last[what] = now
        gap = f"{now - prev:6.1f} s after the last" if prev else "first"
        print(f"{time.strftime('%H:%M:%S')} {what:15s} {gap} {extra}", flush=True)
        return now - prev if prev else None

    def send(self, code, obj, ctype="application/json"):
        body = (json.dumps(obj, ensure_ascii=False) if not isinstance(obj, (bytes, str)) else obj)
        if isinstance(body, str):
            body = body.encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        if Handler.chunked and self.request_version != "HTTP/1.0":
            # in pieces of an odd size, as a server that does not know the length yet
            self.send_header("Transfer-Encoding", "chunked")
            self.end_headers()
            for i in range(0, len(body), 1777):
                piece = body[i:i + 1777]
                self.wfile.write(b"%x\r\n" % len(piece) + piece + b"\r\n")
            self.wfile.write(b"0\r\n\r\n")
        else:
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        if Handler.keep:
            # a server that does not close, whatever "Connection: close" asked
            self.wfile.flush()
            self.close_connection = False

    def do_POST(self):
        u = urlparse(self.path)
        n = int(self.headers.get("Content-Length") or 0)
        form = parse_qs(self.rfile.read(n).decode())
        if u.path.endswith("/protocol/openid-connect/token"):
            ok = form.get("grant_type") == ["client_credentials"] and form.get("client_id") and form.get("client_secret")
            self.note("opensky token", "ok" if ok else "refused")
            if not ok:
                return self.send(401, {"error": "invalid_client"})
            Handler.token = f"fake-{random.randint(0, 1 << 30):x}" + "x" * 900   # as long as a real JWT
            return self.send(200, {"access_token": Handler.token, "expires_in": 1800, "token_type": "Bearer"})
        self.send(404, {"error": "not found"})

    def do_GET(self):
        u = urlparse(self.path)
        p = u.path
        now = time.time()
        m = re.match(r"^/v2/point/(-?[\d.]+)/(-?[\d.]+)/(\d+)$", p)
        if m:
            gap = self.note("airplanes.live", p)
            if Handler.refuse:
                # what it answers to some networks, whatever is asked (2026-11-22)
                return self.send(403, "Please contact us", "text/plain")
            if gap is not None and gap < 1.0:
                return self.send(429, {"msg": "rate limited"})
            lat, lon, nm = float(m[1]), float(m[2]), min(250, int(m[3]))
            ac = [v2(*x) for x in self.sky.near(lat, lon, nm * 1.852)]
            return self.send(200, {"ac": ac, "msg": "No error", "now": int(now * 1000), "total": len(ac),
                                   "ctime": int(now * 1000), "ptime": 3})
        m = re.match(r"^/v2/lat/(-?[\d.]+)/lon/(-?[\d.]+)/dist/(\d+)/?$", p)
        if m:
            key = self.headers.get("x-api-key")
            self.note("adsb.lol", p + (" with a key" if key else "") + " " + self.request_version)
            if self.request_version == "HTTP/1.0":
                # the real one, 2026-11-22: 426 and these sixteen bytes
                return self.send(426, "Upgrade Required", "text/plain")
            lat, lon, nm = float(m[1]), float(m[2]), min(250, int(m[3]))
            ac = [v2(*x) for x in self.sky.near(lat, lon, nm * 1.852)]
            return self.send(200, {"ac": ac, "msg": "No error", "now": int(now * 1000), "total": len(ac),
                                   "ctime": int(now * 1000), "ptime": 2})
        if p == "/api/states/all":
            q = parse_qs(u.query)
            auth = self.headers.get("Authorization", "")
            if auth and auth != f"Bearer {Handler.token}":
                self.note("opensky", "bad token")
                return self.send(401, {"error": "unauthorized"})
            try:
                la0, lo0, la1, lo1 = (float(q[k][0]) for k in ("lamin", "lomin", "lamax", "lomax"))
            except (KeyError, ValueError):
                return self.send(400, {"error": "bad box"})
            area = (la1 - la0) * (lo1 - lo0)
            self.note("opensky", f"box {area:.1f} sq deg{' with a token' if auth else ''}")
            rows = []
            for a, la, lo, trk, alt, vr, d in self.sky.near((la0 + la1) / 2, (lo0 + lo1) / 2, 1e9):
                if la0 <= la <= la1 and lo0 <= lo <= lo1:
                    rows.append(opensky(a, la, lo, trk, alt, vr, now))
            return self.send(200, {"time": int(now), "states": rows or None})
        if p.endswith("/aircraft.json"):
            self.note("receiver", p)
            ac = [v2(*x, readsb=True) for x in self.sky.near(self.sky.lat, self.sky.lon, 400)]
            return self.send(200, {"now": round(now, 1), "messages": 123456, "aircraft": ac})
        m = re.match(r"^/v0/callsign/([A-Za-z0-9]+)$", p)
        if m:
            cs = m[1].upper()
            r = self.sky.routes.get(cs)
            self.note("adsbdb", cs + ("" if r else " (unknown)"))
            if not r:
                return self.send(404, {"response": "unknown callsign"})
            (oi, oc), (di, dc) = r

            def ap(iata, city):
                return {"country_iso_name": "XX", "country_name": "", "elevation": 0, "iata_code": iata,
                        "icao_code": "", "latitude": 0, "longitude": 0, "municipality": city, "name": city}

            return self.send(200, {"response": {"flightroute": {
                "callsign": cs, "callsign_icao": cs, "callsign_iata": None,
                "airline": None, "origin": ap(oi, oc), "destination": ap(di, dc)}}})
        self.send(404, {"error": "not found"})


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=8946)
    ap.add_argument("--lat", type=float, default=40.4168)
    ap.add_argument("--lon", type=float, default=-3.7038)
    ap.add_argument("--count", type=int, default=36)
    ap.add_argument("--radius", type=float, default=160, help="km the traffic spreads over")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--emergency", action="store_true", help="one of them squawks 7700")
    ap.add_argument("--refuse-airplanes", action="store_true", help="airplanes.live answers 403 to everything")
    ap.add_argument("--chunked", action="store_true", help="answer HTTP/1.1 requests in chunks, with no Content-Length")
    ap.add_argument("--keep", action="store_true", help="never close a connection: the client has to know the answer is whole")
    a = ap.parse_args()
    Handler.sky = Sky(a.lat, a.lon, a.count, a.radius, a.seed, a.emergency)
    Handler.chunked, Handler.keep, Handler.refuse = a.chunked, a.keep, a.refuse_airplanes
    srv = ThreadingHTTPServer(("127.0.0.1", a.port), Handler)
    print(f"fake ADS-B on http://127.0.0.1:{a.port}: {len(Handler.sky.ac)} aircraft around {a.lat}, {a.lon}",
          flush=True)
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        sys.exit(0)


if __name__ == "__main__":
    main()
