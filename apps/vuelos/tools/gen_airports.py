#!/usr/bin/env python3
"""Vuelos: the airports pack, vuelos_p4.pak, from OurAirports' airports.csv.

OurAirports' data are in the public domain (https://ourairports.com/data/,
mirrored at https://davidmegginson.github.io/ourairports-data/airports.csv).
The CSV is not in git; download it and pass it here:

    python3 apps/vuelos/tools/gen_airports.py airports.csv -o vuelos_p4.pak

What goes in: every large and medium airport, the small ones that have a
four-letter ICAO-style code (the ones that show on charts), and, with
--heliports, heliports with such a code. Closed ones and seaplane bases
stay out. The format is in apps/vuelos/main/vl_air.c.

sample_airports.csv, next to this script, is a handful of rows of that
CSV around Madrid, the city the simulator's tests and captures use.
"""
import argparse
import csv
import struct
import sys

KIND = {"large_airport": 0, "medium_airport": 1, "small_airport": 2, "heliport": 3}


def fit(s, n):
    """UTF-8, cut to n bytes on a character boundary, NUL-padded."""
    b = s.encode("utf-8")
    while len(b) > n:
        s = s[:-1]
        b = s.encode("utf-8")
    return b + b"\0" * (n - len(b))


def is_code(s):
    return len(s) == 4 and s.isalpha() and s.isupper()


def read(path, heliports):
    out = []
    with open(path, newline="", encoding="utf-8") as f:
        for r in csv.DictReader(f):
            t = r.get("type", "")
            if t not in KIND or (t == "heliport" and not heliports):
                continue
            code = (r.get("icao_code") or r.get("gps_code") or "").strip()
            if not is_code(code):
                code = r.get("ident", "").strip()
            if t in ("small_airport", "heliport") and not is_code(code):
                continue
            try:
                lat, lon = float(r["latitude_deg"]), float(r["longitude_deg"])
            except (KeyError, ValueError):
                continue
            iata = (r.get("iata_code") or "").strip()
            out.append((lat, lon, KIND[t], code[:4], iata[:3], r.get("name", "").strip()))
    out.sort(key=lambda a: a[0])
    return out


def write(rows, path):
    with open(path, "wb") as f:
        f.write(b"VLA1" + struct.pack("<I", len(rows)))
        for lat, lon, k, icao, iata, name in rows:
            f.write(struct.pack("<iiB", round(lat * 1e5), round(lon * 1e5), k))
            f.write(fit(icao, 4) + fit(iata, 3) + fit(name, 32))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("csv", help="OurAirports' airports.csv")
    ap.add_argument("-o", "--out", default="vuelos_p4.pak")
    ap.add_argument("--heliports", action="store_true")
    a = ap.parse_args()
    rows = read(a.csv, a.heliports)
    if not rows:
        sys.exit("no airports read: is it OurAirports' airports.csv?")
    write(rows, a.out)
    kinds = [sum(1 for r in rows if r[2] == k) for k in range(4)]
    size = 8 + 48 * len(rows)
    print(f"{a.out}: {len(rows)} airports (large {kinds[0]}, medium {kinds[1]}, small {kinds[2]}, "
          f"heliports {kinds[3]}), {size / 1024:.0f} KB")


if __name__ == "__main__":
    main()
