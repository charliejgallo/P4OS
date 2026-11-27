#!/usr/bin/env python3
"""Cielo: a fake orbital elements service, for the simulator's tests.

Answers any GET with a TLE file in CelesTrak's "gp.php ... FORMAT=tle" form,
three lines per satellite (name, line 1, line 2). The elements are made up
here: orbits like those of the ISS, China's station, the Hubble telescope and
a few others, with their epoch at the moment the server starts (or --epoch),
so the passes the app predicts come out over the next days. The checksums
are right, so the app takes them as real ones.

    python3 apps/cielo/tools/fake_tle.py --port 8738
    # in <data>/cielo.txt of the simulator:
    #   tle_url=http://127.0.0.1:8738/NORAD/elements/gp.php?GROUP=visual&FORMAT=tle

--fail N answers the first N requests with 503, --html with an HTML page
(an error page instead of elements), to see what the app says.
"""
import argparse
import datetime as dt
import http.server

SATS = [
    # name, norad, inclination, raan, ecc*1e7, argp, mean anomaly, rev/day, bstar mantissa/exp
    ("ISS (ZARYA)", 25544, 51.6416, 247.4627, 6703, 130.5360, 325.0288, 15.50121410, "17895-3"),
    ("CSS (TIANHE)", 48274, 41.4690, 210.3307, 5524, 290.2161, 69.8222, 15.60455312, "28463-3"),
    ("HST", 20580, 28.4699, 288.8102, 2419, 76.4568, 283.6703, 15.09876543, "65432-4"),
    ("NOAA 19", 33591, 99.1910, 120.0040, 13860, 241.2215, 118.7516, 14.12795876, "10203-3"),
    ("SL-16 R/B", 22285, 71.0050, 160.1111, 9101, 85.1414, 275.0221, 14.15712345, "90123-5"),
    ("TERRA", 25994, 98.2101, 30.0303, 1234, 99.9999, 260.1234, 14.57111111, "23456-4"),
    ("COSMOS 2251 DEB", 34454, 74.0352, 10.5050, 12345, 33.3333, 327.1111, 14.40000000, "12345-3"),
]


def checksum(line):
    s = 0
    for c in line[:68]:
        if c.isdigit():
            s += int(c)
        elif c == "-":
            s += 1
    return str(s % 10)


def tle(name, norad, inc, raan, ecc, argp, ma, n, bstar, epoch):
    yy = epoch.year % 100
    start = dt.datetime(epoch.year, 1, 1, tzinfo=dt.timezone.utc)
    day = (epoch - start).total_seconds() / 86400 + 1
    l1 = "1 %05dU 98067A   %02d%012.8f  .00010000  00000-0 %8s 0  999" % (norad, yy, day, bstar.rjust(8))
    l1 = l1[:68].ljust(68)
    l1 += checksum(l1)
    l2 = "2 %05d %8.4f %8.4f %07d %8.4f %8.4f %11.8f%5d" % (norad, inc, raan, ecc, argp, ma, n, 12345)
    l2 = l2[:68].ljust(68)
    l2 += checksum(l2)
    return "%s\n%s\n%s\n" % (name, l1, l2)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8738)
    ap.add_argument("--epoch", help="ISO time of the elements (default: now)")
    ap.add_argument("--fail", type=int, default=0, help="answer the first N requests with 503")
    ap.add_argument("--html", action="store_true", help="answer with an HTML page, not elements")
    args = ap.parse_args()
    epoch = dt.datetime.fromisoformat(args.epoch) if args.epoch else dt.datetime.now(dt.timezone.utc)
    if epoch.tzinfo is None:
        epoch = epoch.replace(tzinfo=dt.timezone.utc)
    body = "".join(tle(*s, epoch=epoch) for s in SATS).encode()
    state = {"fail": args.fail, "n": 0}

    class H(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            state["n"] += 1
            if state["fail"] > 0:
                state["fail"] -= 1
                self.send_response(503)
                self.end_headers()
                self.wfile.write(b"busy")
                return
            if args.html:
                b = b"<html><body>No GP data found</body></html>"
                ctype = "text/html"
            else:
                b = body
                ctype = "text/plain"
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(b)))
            self.end_headers()
            self.wfile.write(b)

        def log_message(self, fmt, *a):
            print("fake_tle: request %d: %s" % (state["n"], self.path), flush=True)

    print("fake_tle: %d satellites, epoch %s, on port %d" % (len(SATS), epoch.isoformat(), args.port), flush=True)
    http.server.ThreadingHTTPServer(("127.0.0.1", args.port), H).serve_forever()


if __name__ == "__main__":
    main()
