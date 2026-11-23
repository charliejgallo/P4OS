#!/usr/bin/env python3
"""
A make-believe house for P4OS's Estado de la casa (apps/estado): a handful
of services on 127.0.0.1 that go down and come back on request, answer
slowly, answer another code, lose the word the app looks for, or show a
certificate about to run out. Nothing here talks to a real device.

    apps/estado/tools/fake_estado.py [--base 18000] [--ctl 18999] [--cert-days 9]
                                     [--write-config sim/sim_fs/data/estado.txt]
                                     [--history sim/sim_fs/data] [--with-tools]

The services (port = --base + offset):

    domotica   +123  HTTP   /manifest.json says "Home Assistant" (the template's check)
    camaras    +500  HTTP   /api/config with "cameras" in it (Frigate's)
    nas        +445  TCP    accepts and closes
    impresora  +80   HTTP   /  200, a small page
    router     +81   HTTP   /  401 with Basic auth, as routers do (the template takes 200-499)
    web        +443  HTTPS  /  200, a self-signed certificate that ends in --cert-days days
    mqtt       +883  TCP    accepts and closes

Each one is driven from the control port, with curl or the browser:

    curl 127.0.0.1:18999/                  the state of all, as JSON
    curl 127.0.0.1:18999/nas/down          stops listening: "connection refused"
    curl 127.0.0.1:18999/nas/up            back
    curl 127.0.0.1:18999/domotica/hang     accepts and never answers: a timeout
    curl 127.0.0.1:18999/camaras/slow/1500 answers 1.5 s late
    curl 127.0.0.1:18999/impresora/code/503
    curl 127.0.0.1:18999/domotica/noword   200, but without the word
    curl 127.0.0.1:18999/web/tls13         TLS 1.3 only: the app cannot read its certificate
    curl 127.0.0.1:18999/router/flap/20    down and up every 20 s, by itself

--write-config writes an estado.txt with these services (and, with
--with-tools, the repo's tools/fake_ha.py on 8123 as a TCP port and
tools/fake_frigate.py on 5000 over HTTP). --history writes, next to it, an
estado_hist.bin and an estado_eventos.txt with a made-up week, for the
screenshots: the same format as apps/estado/main/ec_hist.c.

The certificate is made with the openssl command in a temporary folder,
never in the repo. The standard library and openssl only.
"""
import argparse
import json
import os
import random
import shutil
import socket
import ssl
import struct
import subprocess
import sys
import tempfile
import threading
import time

SERVICES = [
    # name        offset kind     group       tmpl       path              word              every slow
    ("domotica",  123,  "http",  "Casa",     "ha",      "/manifest.json", "Home Assistant", 60,   800),
    ("camaras",   500,  "http",  "Cámaras",  "frigate", "/api/config",    "cameras",        60,   800),
    ("nas",       445,  "tcp",   "Red",      "nas",     "",               "",               60,   200),
    ("impresora", 80,   "http",  "Taller",   "printer", "/",              "",               60,   1000),
    ("router",    81,   "http",  "Red",      "router",  "/",              "",               30,   300),
    ("web",       443,  "https", "Internet", "web",     "/",              "",               300,  1500),
    ("mqtt",      883,  "tcp",   "Casa",     "tcp",     "",               "",               60,   200),
]

TITLES = {"domotica": "Domótica", "camaras": "Grabador", "nas": "NAS", "impresora": "Impresora 3D",
          "router": "Router", "web": "Página web", "mqtt": "MQTT"}
# --english, for the store's screenshots
TITLES_EN = {"domotica": "Home hub", "camaras": "Recorder", "nas": "NAS", "impresora": "3D printer",
             "router": "Router", "web": "Web page", "mqtt": "MQTT"}
GROUPS_EN = {"Casa": "Home", "Cámaras": "Cameras", "Red": "Network", "Taller": "Workshop", "Internet": "Internet"}


def log(*a):
    print("[estado]", *a, file=sys.stderr, flush=True)


class Service:
    def __init__(self, name, port, kind, path, word, ctx12, ctx13):
        self.name, self.port, self.kind, self.path, self.word = name, port, kind, path, word
        self.ctx12, self.ctx13 = ctx12, ctx13
        self.mode = "up"
        self.slow_ms = 0
        self.code = 0
        self.noword = False
        self.tls13 = False
        self.flap = 0
        self.sock = None
        self.lock = threading.Lock()
        self.hits = 0

    def state(self):
        return {"port": self.port, "kind": self.kind, "mode": self.mode, "slow_ms": self.slow_ms,
                "code": self.code, "noword": self.noword, "tls13": self.tls13, "flap": self.flap,
                "hits": self.hits}

    # ---- listening ----
    def listen(self):
        with self.lock:
            if self.sock:
                return
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind(("127.0.0.1", self.port))
            s.listen(16)
            self.sock = s
        threading.Thread(target=self.accept_loop, args=(s,), daemon=True).start()

    def stop(self):
        with self.lock:
            s, self.sock = self.sock, None
        if s:
            try:
                s.close()
            except OSError:
                pass

    def accept_loop(self, s):
        while True:
            try:
                c, _ = s.accept()
            except OSError:
                return
            threading.Thread(target=self.serve, args=(c,), daemon=True).start()

    # ---- one connection ----
    def serve(self, c):
        self.hits += 1
        c.settimeout(10)
        try:
            if self.mode == "hang":
                time.sleep(30)
                return
            if self.kind == "tcp":
                return
            if self.kind == "https":
                ctx = self.ctx13 if self.tls13 else self.ctx12
                try:
                    c = ctx.wrap_socket(c, server_side=True)
                except (ssl.SSLError, OSError) as e:
                    log(self.name, "handshake:", e)
                    return
            req = b""
            while b"\r\n\r\n" not in req and len(req) < 16384:
                d = c.recv(4096)
                if not d:
                    return
                req += d
            if self.slow_ms:
                time.sleep(self.slow_ms / 1000.0)
            line = req.split(b"\r\n", 1)[0].decode("latin1")
            path = line.split(" ")[1] if line.count(" ") >= 2 else "/"
            code, body, extra = self.answer(path)
            reason = {200: "OK", 401: "Unauthorized", 404: "Not Found", 500: "Internal Server Error",
                      502: "Bad Gateway", 503: "Service Unavailable"}.get(code, "Status")
            head = "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %d\r\nConnection: close\r\n%s\r\n" % (
                code, reason, "application/json" if body[:1] in (b"{", b"[") else "text/html; charset=utf-8",
                len(body), extra)
            c.sendall(head.encode() + body)
        except OSError:
            pass
        finally:
            try:
                c.close()
            except OSError:
                pass

    def answer(self, path):
        word = "" if self.noword else self.word
        if self.code:
            return self.code, b"<html><body>error</body></html>", ""
        if self.name == "router":
            return 401, b"<html><body>Login</body></html>", 'WWW-Authenticate: Basic realm="router"\r\n'
        if self.name == "domotica" and path.startswith("/manifest.json"):
            return 200, json.dumps({"name": word or "House", "short_name": word or "House",
                                    "display": "standalone"}).encode(), ""
        if self.name == "camaras" and path.startswith("/api/config"):
            return 200, json.dumps({("cameras" if word else "cams"): {"entrada": {"enabled": True},
                                                                       "patio": {"enabled": True}},
                                    "mqtt": {"enabled": False}}).encode(), ""
        if path == self.path or self.path == "/":
            return 200, ("<html><head><title>%s</title></head><body>%s</body></html>" % (
                TITLES.get(self.name, self.name), word)).encode(), ""
        return 404, b"<html><body>not found</body></html>", ""

    # ---- control ----
    def set(self, mode, arg):
        if mode == "up":
            self.mode, self.slow_ms, self.code, self.noword, self.tls13, self.flap = "up", 0, 0, False, False, 0
            self.listen()
        elif mode == "down":
            self.mode = "down"
            self.stop()
        elif mode == "hang":
            self.mode = "hang"
            self.listen()
        elif mode == "slow":
            self.slow_ms = int(arg or 1500)
        elif mode == "code":
            self.code = int(arg or 503)
        elif mode == "noword":
            self.noword = True
        elif mode == "tls13":
            if not self.ctx13:
                log("this Python's ssl has no TLS 1.3")
                return False
            self.tls13 = True
        elif mode == "flap":
            self.flap = int(arg or 20)
        else:
            return False
        return True


def make_cert(days):
    d = tempfile.mkdtemp(prefix="fake_estado_")
    key, pem = os.path.join(d, "server.key"), os.path.join(d, "server.pem")
    cfg = os.path.join(d, "ext.cnf")
    with open(cfg, "w") as f:
        f.write("[req]\ndistinguished_name=dn\nx509_extensions=ext\nprompt=no\n[dn]\nCN=web.casa.test\n"
                "[ext]\nsubjectAltName=DNS:localhost,IP:127.0.0.1,DNS:127.0.0.1\nbasicConstraints=critical,CA:TRUE\n"
                "keyUsage=digitalSignature,keyEncipherment,keyCertSign\nextendedKeyUsage=serverAuth\n")
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", str(days),
                    "-keyout", key, "-out", pem, "-config", cfg], check=True,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    return d, pem, key


# ---- the configuration and a made-up week ----

GROUPS = {}


def config_text(base, with_tools):
    out = ["# Made-up services for the simulator (apps/estado/tools/fake_estado.py)",
           "fails = 2", "sound = 1", "boot = 1"]
    for name, off, kind, group, tmpl, path, word, every, slow in SERVICES:
        port = base + off
        out += ["", "[%s]" % TITLES[name], "group = %s" % GROUPS.get(group, group), "tmpl = %s" % tmpl]
        if kind == "tcp":
            out += ["type = tcp", "host = 127.0.0.1", "port = %d" % port]
        else:
            scheme = "https" if kind == "https" else "http"
            out += ["type = http", "url = %s://127.0.0.1:%d%s" % (scheme, port, path),
                    "expect = %s" % ("200-499" if name == "router" else "200"), "cert = 14"]
            if word:
                out.append("word = %s" % word)
        out += ["every = %d" % every, "slow = %d" % slow, "timeout = 4000"]
    if with_tools:
        out += ["", "[Home Assistant]", "group = Casa", "tmpl = ha", "type = tcp", "host = 127.0.0.1",
                "port = 8123", "every = 30", "slow = 200", "timeout = 4000",
                "", "[Frigate]", "group = Cámaras", "tmpl = frigate", "type = http",
                "url = http://127.0.0.1:5000/api/config", "expect = 200", "word = cameras",
                "every = 60", "slow = 800", "timeout = 4000"]
    return "\n".join(out) + "\n"


def fnv(name):
    h = 2166136261
    for b in name.encode("utf-8"):
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h or 1


def write_history(folder, with_tools):
    rnd = random.Random(7)
    now = int(time.time())
    names = [(TITLES[s[0]], s) for s in SERVICES]
    base_ms = {"domotica": 22, "camaras": 48, "nas": 3, "impresora": 140, "router": 5, "web": 210, "mqtt": 2}
    # outages: name -> list of (start seconds ago, length seconds, word)
    outs = {
        "impresora": [(26 * 3600, 2 * 3600 + 900, "refused"), (4 * 86400, 5400, "refused")],
        "nas": [(20 * 3600, 14 * 60, "timeout")],
        "camaras": [(3 * 86400 + 7200, 360, "code")],
        "web": [(5 * 86400, 1500, "timeout")],
    }
    recs = []
    events = []
    for title, (name, off, kind, group, tmpl, path, word, every, slow) in names:
        per5 = max(1, 300 // every)
        b5_at, h_at = now // 300, now // 3600
        b5 = [(0, 0, 0, 0)] * 288
        hh = [[0, 0, 0, 0] for _ in range(168)]
        down = outs.get(name, [])

        def is_down(t):
            return any(now - s <= t < now - s + ln for s, ln, _ in down)

        for k in range(7 * 288):
            slot = b5_at - k
            t0 = slot * 300
            n = ok = 0
            tot = mx = 0
            for j in range(per5):
                t = t0 + j * every
                if t > now:
                    continue
                n += 1
                if is_down(t):
                    continue
                hour = time.localtime(t).tm_hour
                ms = base_ms[name] * (1.0 + 0.35 * (hour in (20, 21, 22))) * rnd.uniform(0.75, 1.35)
                if name == "web" and rnd.random() < 0.02:
                    ms *= 8
                ms = int(ms)
                ok += 1
                tot += ms
                mx = max(mx, ms)
            if not n:
                continue
            avg = tot // ok if ok else 0
            if k < 288:
                b5[slot % 288] = (n, ok, avg, mx)
            hs = (t0 // 3600) % 168
            h = hh[hs]
            if h[1] + ok:
                h[2] = (h[2] * h[1] + tot) // (h[1] + ok)
            h[0] += n
            h[1] += ok
            h[3] = max(h[3], mx)
        olist = []
        for s, ln, w in sorted(down, key=lambda x: -x[0]):
            st = now - s
            err = {"timeout": 3, "refused": 2, "code": 6}[w]
            olist.append((st, st + ln, err))
            events.append((st, title, "down", "503" if w == "code" else w, -1))
            events.append((st + ln, title, "up", str(ln), base_ms[name]))
        rec = struct.pack("<IIIHH", fnv(title), b5_at, h_at, len(olist), 0)
        rec += b"".join(struct.pack("<HHHH", *b) for b in b5)
        rec += b"".join(struct.pack("<HHHH", *b) for b in hh)
        for k in range(12):
            o = olist[k] if k < len(olist) else (0, 0, 0)
            rec += struct.pack("<IIB3x", *o)
        assert len(rec) == 3808
        recs.append(rec)
    if with_tools:
        pass                    # the two of the repo's tools start with no past
    with open(os.path.join(folder, "estado_hist.bin"), "wb") as f:
        f.write(struct.pack("<IIII", 0x31484345, 3808, len(recs), now))
        f.write(b"".join(recs))
    with open(os.path.join(folder, "estado_eventos.txt"), "w") as f:
        for t, title, what, detail, ms in sorted(events):
            f.write("%s\t%s\t%s\t%s\t%d\n" % (time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(t)),
                                              title, what, detail, ms))
    log("history for %d services in %s" % (len(recs), folder))


# ---- the control port ----

def control(port, svcs):
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(8)
    while True:
        c, _ = srv.accept()
        try:
            req = c.recv(4096).decode("latin1")
            path = req.split(" ")[1] if req.count(" ") >= 2 else "/"
            parts = [p for p in path.split("/") if p]
            code, out = 200, None
            if parts and parts[0] in svcs and len(parts) >= 2:
                ok = svcs[parts[0]].set(parts[1], parts[2] if len(parts) > 2 else None)
                if not ok:
                    code = 400
                log("%s -> %s" % (parts[0], "/".join(parts[1:])))
            elif parts:
                code = 404
            out = json.dumps({k: v.state() for k, v in svcs.items()}, indent=1).encode()
            c.sendall(b"HTTP/1.1 %d X\r\nContent-Type: application/json\r\nContent-Length: %d\r\n"
                      b"Connection: close\r\n\r\n" % (code, len(out)) + out)
        except OSError:
            pass
        finally:
            c.close()


def flapper(svcs):
    while True:
        time.sleep(1)
        for s in svcs.values():
            if s.flap and int(time.time()) % s.flap == 0:
                if s.sock:
                    s.stop()
                    log(s.name, "flap: down")
                else:
                    s.listen()
                    log(s.name, "flap: up")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--base", type=int, default=18000)
    ap.add_argument("--ctl", type=int, default=18999)
    ap.add_argument("--cert-days", type=int, default=9)
    ap.add_argument("--write-config", metavar="PATH")
    ap.add_argument("--history", metavar="DIR")
    ap.add_argument("--with-tools", action="store_true")
    ap.add_argument("--no-serve", action="store_true", help="write the files and leave")
    ap.add_argument("--english", action="store_true", help="the made-up names in English (screenshots)")
    a = ap.parse_args()
    if a.english:
        TITLES.update(TITLES_EN)
        GROUPS.update(GROUPS_EN)
    if a.write_config:
        with open(a.write_config, "w") as f:
            f.write(config_text(a.base, a.with_tools))
        log("wrote", a.write_config)
    if a.history:
        write_history(a.history, a.with_tools)
    if a.no_serve:
        return
    tmp, pem, key = make_cert(a.cert_days)
    ctx12 = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx12.load_cert_chain(pem, key)
    ctx13 = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    try:
        ctx13.minimum_version = ssl.TLSVersion.TLSv1_3
        ctx13.load_cert_chain(pem, key)
    except ValueError:
        ctx13 = None            # an ssl module without TLS 1.3 (Xcode's Python 3.9)
    svcs = {}
    for name, off, kind, group, tmpl, path, word, every, slow in SERVICES:
        s = Service(name, a.base + off, kind, path, word, ctx12, ctx13)
        s.listen()
        svcs[name] = s
    threading.Thread(target=flapper, args=(svcs,), daemon=True).start()
    log("services on 127.0.0.1:%d.., control on 127.0.0.1:%d (certificate ends in %d days)" % (
        a.base, a.ctl, a.cert_days))
    try:
        control(a.ctl, svcs)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
