#!/usr/bin/env python3
"""
A make-believe GitHub for P4OS's Tienda (docs/plan/TIENDA.md): the release
downloads of one repository, served on 127.0.0.1 unless told otherwise, with the redirects
the real one makes and the ways a download goes wrong, so aos_fetch.c and
the store can be built and measured without touching the network.

    tools/fake_github.py --dir <folder> [--port 8090] [--tag v9.9.9]
                         [--repo owner/name] [--tls cert.pem key.pem]
                         [--bind <address of this machine>]

Every file in --dir is an asset of the release <tag>, the latest one:

    /<owner>/<name>/releases/latest/download/<file>    302 to the next
    /<owner>/<name>/releases/download/<any tag>/<file> 302 to a signed address
    on the second port (--port + 1), the "other host":
    /assets/<token>/<file>?sig=...                     the bytes, with Range

As seen on github.com on 2026-11-20: two redirects, the second to another
host and ~1 KB long, a few kilobytes of headers on the way, and 206 with
Content-Range to a Range request even over HTTP/1.0. A signed address stops
working after --expire seconds (300 there), which is why a download that is
cut has to start again from the first address.

What goes wrong is set while it runs, so one server does for a whole test:

    curl -X POST 'http://127.0.0.1:8090/_fake?cut=200000&cuts=2'

    cut=N       close the connection after N bytes of an asset's body...
    cuts=K      ...the next K times (default 1 when cut is given)
    norange=1   ignore Range: always the whole file, 200
    chunked=1   Transfer-Encoding: chunked, and no Content-Length
    fail=503    answer this instead of the asset...
    fails=K     ...the next K times
    oneshot=1   a signed address works once: the next request to it gets 403
    loop=1      the first address redirects to itself
    down=1      the second redirect goes to http:// even when this is https
    kbps=N      no faster than this
    reset=1     all of the above off, counters to zero

GET /_fake answers what it has counted: {"requests", "assets", "bytes"}.

--bind makes it listen on another address of this machine instead, so the
board can reach it over the home network; its redirects then name that
address. Stop it when the measuring is done.

The standard library only. --tls takes a certificate for "localhost" and
its key; the simulator trusts its authority with P4_SIM_EXTRA_CA.
"""
import argparse
import json
import os
import secrets
import ssl
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse

# roughly what github.com sends with a redirect: a client with a small
# header buffer finds out here, not on the board
POLICY = "default-src 'none'; " + "; ".join(
    "%s-src github.githubassets.com avatars.githubusercontent.com objects-origin.githubusercontent.com "
    "release-assets.githubusercontent.com user-images.githubusercontent.com" % k
    for k in ("base", "child", "connect", "font", "form", "frame", "img", "manifest", "media", "script", "style",
              "worker"))


class State:
    def __init__(self, args):
        self.args = args
        self.lock = threading.Lock()
        self.tokens = {}        # token -> [file, born, used]
        self.reset()

    def reset(self):
        self.cut = 0
        self.cuts = 0
        self.norange = False
        self.chunked = False
        self.fail = 0
        self.fails = 0
        self.oneshot = False
        self.loop = False
        self.down = False
        self.kbps = 0
        self.requests = 0
        self.assets = 0
        self.bytes = 0

    def set(self, q):
        with self.lock:
            if "reset" in q:
                self.reset()
            if "cut" in q:
                self.cut = int(q["cut"][0])
                self.cuts = int(q.get("cuts", ["1"])[0])
            if "fail" in q:
                self.fail = int(q["fail"][0])
                self.fails = int(q.get("fails", ["1"])[0])
            for k in ("norange", "chunked", "oneshot", "loop", "down"):
                if k in q:
                    setattr(self, k, q[k][0] not in ("0", ""))
            if "kbps" in q:
                self.kbps = int(q["kbps"][0])


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"
    st = None           # State
    assets = False      # this is the second port

    def log_message(self, fmt, *a):
        if not self.st.args.quiet:
            sys.stderr.write("%s %s\n" % ("assets" if self.assets else "github", fmt % a))

    def plain(self, code, text, extra=()):
        body = text.encode()
        self.send_response(code)
        for k, v in extra:
            self.send_header(k, v)
        self.send_header("Content-Type", "text/plain; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Connection", "close")
        self.end_headers()
        self.wfile.write(body)
        self.close_connection = True

    def redirect(self, where):
        self.plain(302, "", (("Location", where), ("Content-Security-Policy", POLICY),
                             ("Strict-Transport-Security", "max-age=31536000; includeSubdomains; preload"),
                             ("X-Frame-Options", "deny"), ("Vary", "Accept-Encoding, Accept, X-Requested-With")))

    def origin(self, port, tls):
        return "%s://%s:%d" % ("https" if tls else "http", self.st.args.name, port)

    def do_POST(self):
        u = urlparse(self.path)
        if u.path != "/_fake":
            return self.plain(404, "Not Found")
        self.st.set(parse_qs(u.query, keep_blank_values=True))
        self.plain(200, "ok")

    def do_HEAD(self):
        self.do_GET()

    def do_GET(self):
        st = self.st
        u = urlparse(self.path)
        with st.lock:
            st.requests += 1
        if u.path == "/_fake":
            with st.lock:
                return self.plain(200, json.dumps({"requests": st.requests, "assets": st.assets, "bytes": st.bytes}))
        if self.assets:
            return self.asset(u)
        a = st.args
        base = "/%s/releases/" % a.repo
        if not u.path.startswith(base):
            return self.plain(404, "Not Found")
        rest = u.path[len(base):]
        tls = bool(a.tls)
        if rest.startswith("latest/download/"):
            name = rest[len("latest/download/"):]
            if st.loop:
                return self.redirect(self.origin(a.port, tls) + u.path)
            return self.redirect("%s%sdownload/%s/%s" % (self.origin(a.port, tls), base, a.tag, name))
        if rest.startswith("download/") and rest.count("/") == 2:
            # any tag has every file: a catalogue names packs of older releases
            name = rest.split("/", 2)[2]
            if "/" in name or name.startswith(".") or not os.path.isfile(os.path.join(a.dir, name)):
                return self.plain(404, "Not Found")
            token = secrets.token_hex(16)
            with st.lock:
                st.tokens[token] = [name, time.time(), False]
            # a real one is a JWT of about this size
            sig = secrets.token_urlsafe(700)
            return self.redirect("%s/assets/%s/%s?sp=r&sv=2018-11-09&sr=b&sig=%s&response-content-disposition="
                                 "attachment%%3B%%20filename%%3D%s" %
                                 (self.origin(a.port + 1, tls and not st.down), token, name, sig, name))
        self.plain(404, "Not Found")

    def asset(self, u):
        st = self.st
        parts = u.path.split("/")
        if len(parts) != 4 or parts[1] != "assets":
            return self.plain(404, "Not Found")
        with st.lock:
            t = st.tokens.get(parts[2])
            ok = t and t[0] == parts[3] and time.time() - t[1] < st.args.expire and not (st.oneshot and t[2])
            if ok:
                t[2] = True
            fail = 0
            if ok and st.fails > 0:
                st.fails -= 1
                fail = st.fail
        if not ok:
            return self.plain(403, "Signature expired or not valid")
        if fail:
            return self.plain(fail, "Something went wrong")
        path = os.path.join(st.args.dir, parts[3])
        size = os.path.getsize(path)
        start, end, code = 0, size - 1, 200
        rng = self.headers.get("Range")
        if rng and not st.norange and rng.startswith("bytes="):
            a, _, b = rng[6:].partition("-")
            try:
                start = int(a)
                end = int(b) if b else size - 1
            except ValueError:
                start = size
            if start >= size or end < start:
                return self.plain(416, "Requested Range Not Satisfiable", (("Content-Range", "bytes */%d" % size),))
            end = min(end, size - 1)
            code = 206
        n = end - start + 1
        with st.lock:
            st.assets += 1
            cut = 0
            if st.cuts > 0 and st.cut < n:
                st.cuts -= 1
                cut = st.cut
            chunked, kbps = st.chunked, st.kbps
        self.send_response(code)
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Accept-Ranges", "bytes")
        if code == 206:
            self.send_header("Content-Range", "bytes %d-%d/%d" % (start, end, size))
        if chunked:
            self.send_header("Transfer-Encoding", "chunked")
        else:
            self.send_header("Content-Length", str(n))
        self.send_header("Connection", "close")
        self.end_headers()
        self.close_connection = True
        if self.command == "HEAD":
            return
        sent = 0
        with open(path, "rb") as f:
            f.seek(start)
            step = 7001 if chunked else 16384   # chunks that never line up with a block
            while sent < n:
                want = min(step, n - sent)
                if cut:
                    want = min(want, cut - sent)
                    if want <= 0:
                        break
                data = f.read(want)
                try:
                    if chunked:
                        self.wfile.write(b"%x\r\n" % len(data) + data + b"\r\n")
                    else:
                        self.wfile.write(data)
                    self.wfile.flush()
                except OSError:
                    break
                sent += len(data)
                with st.lock:
                    st.bytes += len(data)
                if kbps:
                    time.sleep(len(data) / (kbps * 1024.0))
            else:
                if chunked:
                    self.wfile.write(b"0\r\n\r\n")


def serve(st, port, assets, tls):
    h = type("H", (Handler,), {"st": st, "assets": assets})
    srv = ThreadingHTTPServer((st.args.bind, port), h)
    srv.daemon_threads = True
    if tls:
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(st.args.tls[0], st.args.tls[1])
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--dir", required=True, help="the release's assets")
    ap.add_argument("--port", type=int, default=8090, help="github's; the assets' host is the next one")
    ap.add_argument("--tag", default="v9.9.9")
    ap.add_argument("--repo", default="fake/p4os")
    ap.add_argument("--expire", type=float, default=300, help="seconds a signed address lasts")
    ap.add_argument("--tls", nargs=2, metavar=("CERT", "KEY"), help="serve https with this certificate (for localhost)")
    ap.add_argument("--bind", default="127.0.0.1", help="listen here instead (an address of this machine)")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()
    # what the redirects call this machine: the name the certificate is for
    args.name = "localhost" if args.bind == "127.0.0.1" else args.bind
    st = State(args)
    serve(st, args.port, False, bool(args.tls))
    serve(st, args.port + 1, True, bool(args.tls))
    scheme = "https" if args.tls else "http"
    print("%s://%s:%d/%s/releases/latest/download/<file>   (%s, tag %s)" %
          (scheme, args.name, args.port, args.repo, args.dir, args.tag), flush=True)
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
