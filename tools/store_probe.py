#!/usr/bin/env python3
"""
Measures the Tienda's pieces on a board or on the simulator, through the
calls of components/aos_portal/aos_portal_store.c (docs/plan/TIENDA.md,
phase 0). Nothing is installed for good: what it downloads it says where.

    tools/store_probe.py <board> fetch <url> <card path> [--sha256 H] [--size N]
                                       [--cancel-at BYTES]
        One download to the card. Prints its progress, then the time, the
        speed, the connections, and the lowest free internal RAM and PSRAM it
        saw while it ran (it asks four times a second).

    tools/store_probe.py <board> check <name>.so
        Would /apps/<name>.so load on that firmware, without loading it.

    tools/store_probe.py <board> hot <name>.so [--times 20]
        Takes the app off and puts it back, that many times, with no restart:
        the free PSRAM after each round says whether something is left behind.

<board> is p4os.local, an address, or localhost:8080 for the simulator. The
portal's token, for a board that asks for its password: P4OS_TOKEN=<token>.

For the fetch, against the make-believe GitHub on this machine:

    tools/fake_github.py --dir <folder with files> --port 8090
    tools/store_probe.py p4os.local fetch \\
        http://<this machine>:8090/fake/p4os/releases/latest/download/<file> /store-test/<file>

(fake_github.py listens on 127.0.0.1 only; for the board, run it with
--bind on the address of this machine in the board's network.)
"""
import argparse
import json
import os
import socket
import sys
import time
import urllib.error
import urllib.request


def ipv4(board):
    """The board's address, looked up once: p4os.local answers no AAAA, and
    urllib waits five seconds for one on every call (curl -4 is the same
    story in the shell scripts)."""
    host, _, port = board.partition(":")
    try:
        host = socket.gethostbyname(host)
    except OSError:
        pass
    return host + (":" + port if port else "")


def call(board, method, path, body=None, timeout=15):
    req = urllib.request.Request("http://%s/api/store/%s" % (board, path), method=method,
                                 data=json.dumps(body).encode() if body is not None else None)
    if body is not None:
        req.add_header("Content-Type", "application/json")
    token = os.environ.get("P4OS_TOKEN")
    if token:
        req.add_header("Authorization", "Bearer " + token)
    try:
        return json.loads(urllib.request.urlopen(req, timeout=timeout).read())
    except urllib.error.HTTPError as e:
        try:
            return json.loads(e.read())
        except ValueError:
            return {"ok": False, "error": "HTTP %d" % e.code}


def fetch(a):
    body = {"url": a.url, "path": a.path}
    if a.sha256:
        body["sha256"] = a.sha256
    if a.size:
        body["size"] = a.size
    r = call(a.board, "POST", "fetch", body)
    if not r.get("ok"):
        sys.exit("refused: %s" % r.get("error"))
    last = 0
    while True:
        time.sleep(0.25)
        r = call(a.board, "GET", "fetch")
        if r["state"] != "busy":
            break
        if a.cancel_at and r["got"] >= a.cancel_at:
            call(a.board, "POST", "fetch/cancel")
        if time.time() - last > 1:
            last = time.time()
            pct = " (%d %%)" % (100 * r["got"] // r["total"]) if r.get("total") else ""
            print("  %9d bytes%s from %s, internal RAM free %d" % (r["got"], pct, r.get("host", "?"), r["internal_free"]))
    print("%s: %s" % (r["state"], r.get("why") or "status %s" % r.get("status")))
    moved = r["got"] - r["resumed"]
    ms = max(1, r["ms"] - r["connect_ms"] - r["verify_ms"])
    print("  %d bytes on the card (%d were there), %.1f s in all" % (r["got"], r["resumed"], r["ms"] / 1000))
    print("  connecting %.1f s over %d connections (%d redirects, %d retries); checking the hash %.1f s" %
          (r["connect_ms"] / 1000, r["connects"], r["redirects"], r["retries"], r["verify_ms"] / 1000))
    print("  %.0f KB/s while it flowed" % (moved / 1024 / (ms / 1000)))
    print("  internal RAM free: %d at the start, %d at the lowest (%d less); PSRAM at the lowest %d" %
          (r["internal_at_start"], r["internal_min"], r["internal_at_start"] - r["internal_min"], r["psram_min"]))
    sys.exit(0 if r["state"] == "done" else 1)


def check(a):
    r = call(a.board, "GET", "check?file=" + a.file)
    if r.get("ok"):
        print("%s would load (%s ms)" % (a.file, r.get("ms")))
    elif r.get("missing"):
        print("%s needs %d symbols this firmware does not have: %s" % (a.file, len(r["missing"]), " ".join(r["missing"])))
    else:
        print("%s: %s" % (a.file, r.get("why") or r.get("error")))
    sys.exit(0 if r.get("ok") else 1)


def hot(a):
    first = None
    for i in range(a.times):
        u = call(a.board, "POST", "unload?file=" + a.file, timeout=30)
        if not u.get("ok"):
            sys.exit("round %d: could not take it off: %s" % (i + 1, u))
        l = call(a.board, "POST", "load?file=" + a.file, timeout=30)
        if not l.get("ok"):
            sys.exit("round %d: it did not come back: %s" % (i + 1, l))
        if first is None:
            first = l["psram_after"]
        print("  %2d  off in %4d ms, back in %4d ms   PSRAM free %d (%+d since the first round), internal %d" %
              (i + 1, u["ms"], l["ms"], l["psram_after"], l["psram_after"] - first, l["internal_free"]))
    print("%d rounds, the board still answers" % a.times)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("board")
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch")
    f.add_argument("url")
    f.add_argument("path")
    f.add_argument("--sha256")
    f.add_argument("--size", type=int, default=0)
    f.add_argument("--cancel-at", type=int, default=0)
    f.set_defaults(fn=fetch)
    c = sub.add_parser("check")
    c.add_argument("file")
    c.set_defaults(fn=check)
    h = sub.add_parser("hot")
    h.add_argument("file")
    h.add_argument("--times", type=int, default=20)
    h.set_defaults(fn=hot)
    a = ap.parse_args()
    a.board = ipv4(a.board)
    a.fn(a)


if __name__ == "__main__":
    main()
