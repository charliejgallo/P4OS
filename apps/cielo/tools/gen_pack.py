#!/usr/bin/env python3
"""Cielo: the sky pack, cielo_p4.pak, from public-domain catalogues.

What goes in, and where each part comes from:

  stars       the Yale Bright Star Catalogue, 5th revised edition (Hoffleit
              and Warren, 1991), in the public domain: every star with a
              position and a magnitude (9096 of 9110), J2000.
  Messier     the Messier catalogue as NASA's HEASARC distributes it (table
              "messier"), a work of the US government: 109 objects.
  figures     lines.txt, next to this script: the constellations drawn for
              this app from the catalogue's stars.
  names       names.txt, next to this script: the stars' proper names.

The two catalogues are not in git. Both come from HEASARC's TAP service as
pipe-separated text:

  curl -o bsc5p.txt "https://heasarc.gsfc.nasa.gov/xamin/vo/tap/sync?REQUEST=doQuery&LANG=ADQL&FORMAT=text&MAXREC=20000&QUERY=SELECT%20hr,name,alt_name,ra,dec,vmag,bv_color,spect_type,pmra,pmdec,parallax,radvel,hd%20FROM%20bsc5p%20ORDER%20BY%20hr"
  curl -o messier.txt "https://heasarc.gsfc.nasa.gov/xamin/vo/tap/sync?REQUEST=doQuery&LANG=ADQL&FORMAT=text&QUERY=SELECT%20*%20FROM%20messier"
  python3 apps/cielo/tools/gen_pack.py bsc5p.txt messier.txt -o cielo_p4.pak

The format is described in apps/cielo/main/cl_cat.h. Every star the figures
and the names name must be found: the script stops and says which is not.
"""
import argparse
import math
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

GREEK = ["Alp", "Bet", "Gam", "Del", "Eps", "Zet", "Eta", "The", "Iot", "Kap", "Lam", "Mu", "Nu",
         "Xi", "Omi", "Pi", "Rho", "Sig", "Tau", "Ups", "Phi", "Chi", "Psi", "Ome"]

# Messier types, as cl_cat.h numbers them
MTYPE = {"OC": 1, "GB": 2, "DI": 3, "PL": 4, "S": 5, "E": 6, "E?": 6, "IR": 7}

# the IAU's 88, abbreviation and Latin name
CONS = [
    ("And", "Andromeda"), ("Ant", "Antlia"), ("Aps", "Apus"), ("Aqr", "Aquarius"), ("Aql", "Aquila"),
    ("Ara", "Ara"), ("Ari", "Aries"), ("Aur", "Auriga"), ("Boo", "Bootes"), ("Cae", "Caelum"),
    ("Cam", "Camelopardalis"), ("Cnc", "Cancer"), ("CVn", "Canes Venatici"), ("CMa", "Canis Major"),
    ("CMi", "Canis Minor"), ("Cap", "Capricornus"), ("Car", "Carina"), ("Cas", "Cassiopeia"),
    ("Cen", "Centaurus"), ("Cep", "Cepheus"), ("Cet", "Cetus"), ("Cha", "Chamaeleon"), ("Cir", "Circinus"),
    ("Col", "Columba"), ("Com", "Coma Berenices"), ("CrA", "Corona Australis"), ("CrB", "Corona Borealis"),
    ("Crv", "Corvus"), ("Crt", "Crater"), ("Cru", "Crux"), ("Cyg", "Cygnus"), ("Del", "Delphinus"),
    ("Dor", "Dorado"), ("Dra", "Draco"), ("Equ", "Equuleus"), ("Eri", "Eridanus"), ("For", "Fornax"),
    ("Gem", "Gemini"), ("Gru", "Grus"), ("Her", "Hercules"), ("Hor", "Horologium"), ("Hya", "Hydra"),
    ("Hyi", "Hydrus"), ("Ind", "Indus"), ("Lac", "Lacerta"), ("Leo", "Leo"), ("LMi", "Leo Minor"),
    ("Lep", "Lepus"), ("Lib", "Libra"), ("Lup", "Lupus"), ("Lyn", "Lynx"), ("Lyr", "Lyra"),
    ("Men", "Mensa"), ("Mic", "Microscopium"), ("Mon", "Monoceros"), ("Mus", "Musca"), ("Nor", "Norma"),
    ("Oct", "Octans"), ("Oph", "Ophiuchus"), ("Ori", "Orion"), ("Pav", "Pavo"), ("Peg", "Pegasus"),
    ("Per", "Perseus"), ("Phe", "Phoenix"), ("Pic", "Pictor"), ("PsA", "Piscis Austrinus"),
    ("Psc", "Pisces"), ("Pup", "Puppis"), ("Pyx", "Pyxis"), ("Ret", "Reticulum"), ("Sge", "Sagitta"),
    ("Sgr", "Sagittarius"), ("Sco", "Scorpius"), ("Scl", "Sculptor"), ("Sct", "Scutum"),
    ("Ser", "Serpens"), ("Sex", "Sextans"), ("Tau", "Taurus"), ("Tel", "Telescopium"),
    ("Tri", "Triangulum"), ("TrA", "Triangulum Australe"), ("Tuc", "Tucana"), ("UMa", "Ursa Major"),
    ("UMi", "Ursa Minor"), ("Vel", "Vela"), ("Vir", "Virgo"), ("Vol", "Volans"), ("Vul", "Vulpecula"),
]

NAME_RE = re.compile(r"^(\d+)?\s*([A-Z][a-z]{1,2})?\s*(\d)?\s*([A-Z][A-Za-z]{2})$")


def num(s):
    s = s.strip()
    if not s or s == "null":
        return None
    return float(s)


def read_table(path):
    rows = []
    with open(path, encoding="utf-8", errors="replace") as f:
        head = None
        for line in f:
            if "|" not in line:
                continue
            cells = [c.strip() for c in line.rstrip("\n").split("|")]
            if head is None:
                head = cells
                continue
            if len(cells) < len(head):
                continue
            rows.append(dict(zip(head, cells)))
    return rows


def parse_desig(alt):
    """'9Alp CMa' -> (9, 'Alp', '', 'CMa'); None if it is not that form."""
    if not alt or alt == "null":
        return None
    m = NAME_RE.match(alt.strip())
    if not m:
        return None
    fl, bay, sup, con = m.groups()
    if bay and bay not in GREEK:
        return None
    return (int(fl) if fl else None, bay or "", sup or "", con)


def unit(ra, dec):
    r, d = math.radians(ra), math.radians(dec)
    return (math.cos(d) * math.cos(r), math.cos(d) * math.sin(r), math.sin(d))


def q16(v):
    return max(-32767, min(32767, int(round(v * 32767))))


class Strings:
    def __init__(self):
        self.buf = bytearray()
        self.at = {}

    def add(self, s):
        if s in self.at:
            return self.at[s]
        off = len(self.buf)
        self.buf += s.encode("utf-8") + b"\0"
        self.at[s] = off
        return off


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("bsc", help="bsc5p.txt from HEASARC")
    ap.add_argument("messier", help="messier.txt from HEASARC")
    ap.add_argument("-o", "--out", default="cielo_p4.pak")
    args = ap.parse_args()

    stars = []
    for r in read_table(args.bsc):
        ra, dec, mag = num(r["ra"]), num(r["dec"]), num(r["vmag"])
        if ra is None or dec is None or mag is None:
            continue
        hr = int(r["hr"])
        d = parse_desig(r.get("alt_name", ""))
        bv = num(r["bv_color"])
        plx = num(r["parallax"])
        stars.append({"hr": hr, "ra": ra, "dec": dec, "mag": mag, "bv": bv, "plx": plx, "d": d,
                      "sp": r.get("spect_type", "")})
    stars.sort(key=lambda s: (s["mag"], s["hr"]))
    for i, s in enumerate(stars):
        s["i"] = i

    # lookup: (const, bayer, sup) and (const, flamsteed), and HR
    by_bay, by_fl, by_hr = {}, {}, {}
    for s in stars:
        by_hr[s["hr"]] = s
        d = s["d"]
        if not d:
            continue
        fl, bay, sup, con = d
        if bay:
            by_bay.setdefault((con, bay, sup), s)
            # a plain "Alp" also finds alpha-1 when there is no plain alpha
            if sup:
                by_bay.setdefault((con, bay, "*"), s)
        if fl:
            by_fl[(con, fl)] = s

    def find(tok, con):
        if "." in tok:
            tok, con = tok.split(".", 1)
        if tok.startswith("HR"):
            return by_hr.get(int(tok[2:]))
        if tok.isdigit():
            return by_fl.get((con, int(tok)))
        m = re.match(r"^([A-Z][a-z]{1,2})(\d)?$", tok)
        if not m:
            return None
        bay, sup = m.group(1), m.group(2) or ""
        return by_bay.get((con, bay, sup)) or (None if sup else by_bay.get((con, bay, "*")))

    bad = []
    # the figures
    cons_idx = {c: i for i, (c, _) in enumerate(CONS)}
    figs = {}
    with open(os.path.join(HERE, "lines.txt"), encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            con, rest = line.split(None, 1)
            if con not in cons_idx:
                bad.append("unknown constellation " + con)
                continue
            segs = []
            for chain in rest.split(";"):
                toks = [t.strip() for t in chain.split("-") if t.strip()]
                prev = None
                for t in toks:
                    s = find(t, con)
                    if not s:
                        bad.append("%s: %s" % (con, t))
                        prev = None
                        continue
                    if prev is not None and prev["i"] != s["i"]:
                        segs.append((prev["i"], s["i"]))
                    prev = s
            figs[con] = segs
    missing = [c for c, _ in CONS if c not in figs]
    if missing:
        bad.append("no figure for " + " ".join(missing))

    # the names
    names = []
    with open(os.path.join(HERE, "names.txt"), encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            tok, con, name = parts[0], parts[1], " ".join(parts[2:])
            s = find(tok, con)
            if not s:
                bad.append("name %s: %s %s" % (name, tok, con))
                continue
            s["name"] = len(names)
            names.append(name)

    if bad:
        for b in bad:
            print("not found:", b, file=sys.stderr)
        sys.exit(1)

    strs = Strings()
    out = {}

    # STAR
    b = bytearray(struct.pack("<I", len(stars)))
    for s in stars:
        x, y, z = unit(s["ra"], s["dec"])
        d = s["d"]
        if d:
            fl, bay, sup, con = d
            parts = []
            if bay:
                parts.append(bay + sup + " " + con)
            if fl:
                parts.append("%d %s" % (fl, con))
            desig = " · ".join(parts + ["HR %d" % s["hr"]])
        else:
            desig = "HR %d" % s["hr"]
        bv = 32767 if s["bv"] is None else int(round(s["bv"] * 1000))
        plx = 0 if not s["plx"] or s["plx"] <= 0 else min(65535, int(round(s["plx"] * 10000)))
        sp = (s["sp"] or "?")[0]
        b += struct.pack("<hhhhhHHHIBBH", q16(x), q16(y), q16(z), int(round(s["mag"] * 100)), bv, s["hr"],
                         plx, s.get("name", 0xFFFF), strs.add(desig), ord(sp) if ord(sp) < 128 else ord("?"),
                         0, 0)
    out[b"STAR"] = b

    # SNAM
    b = bytearray(struct.pack("<I", len(names)))
    for n in names:
        b += struct.pack("<I", strs.add(n))
    out[b"SNAM"] = b

    # CONS and SEGS
    cb = bytearray(struct.pack("<I", len(CONS)))
    sb = bytearray()
    nseg = 0
    for con, latin in CONS:
        segs = figs[con]
        pts = set()
        for a, c in segs:
            pts.add(a)
            pts.add(c)
        vx = vy = vz = 0.0
        for i in pts:
            x, y, z = unit(stars[i]["ra"], stars[i]["dec"])
            vx, vy, vz = vx + x, vy + y, vz + z
        n = math.sqrt(vx * vx + vy * vy + vz * vz) or 1
        cb += struct.pack("<4shhhHHHI", con.encode().ljust(4, b"\0"), q16(vx / n), q16(vy / n), q16(vz / n),
                          nseg, len(segs), 0, strs.add(latin))
        for a, c in segs:
            sb += struct.pack("<HH", a, c)
        nseg += len(segs)
    out[b"CONS"] = cb
    out[b"SEGS"] = struct.pack("<I", nseg) + sb

    # MESS
    mess = []
    for r in read_table(args.messier):
        nm = r.get("name", "")
        m = re.match(r"M\s*(\d+)", nm)
        ra, dec = num(r["ra"]), num(r["dec"])
        if not m or ra is None or dec is None:
            continue
        mn = int(m.group(1))
        t = MTYPE.get(r.get("object_type", "").strip(), 9)
        if mn == 1:
            t = 8           # a supernova remnant
        mag = num(r.get("vmag", ""))
        dim = r.get("dimension", "")
        dm = re.match(r"([\d.]+)", dim or "")
        size = float(dm.group(1)) if dm else 0
        ngc = r.get("alt_name", "")
        ngc = "" if ngc in ("", "null") else re.sub(r"^NGC\s*", "NGC ", re.sub(r"^IC\s*", "IC ", ngc))
        mess.append((mn, t, mag, ra, dec, size, ngc))
    mess.sort()
    b = bytearray(struct.pack("<I", len(mess)))
    for mn, t, mag, ra, dec, size, ngc in mess:
        x, y, z = unit(ra, dec)
        b += struct.pack("<BBhhhhHI", mn, t, 32767 if mag is None else int(round(mag * 100)), q16(x), q16(y),
                         q16(z), int(round(size * 10)), strs.add(ngc))
    out[b"MESS"] = b
    out[b"STRS"] = bytes(strs.buf)

    order = [b"STAR", b"SNAM", b"CONS", b"SEGS", b"MESS", b"STRS"]
    head = 12 + 12 * len(order)
    blob = bytearray()
    table = bytearray()
    for tag in order:
        while (head + len(blob)) % 4:
            blob += b"\0"
        table += struct.pack("<4sII", tag, head + len(blob), len(out[tag]))
        blob += out[tag]
    data = b"CIELOPK1" + struct.pack("<I", len(order)) + bytes(table) + bytes(blob)
    with open(args.out, "wb") as f:
        f.write(data)
    print("%s: %d bytes, %d stars (to mag %.2f), %d names, %d constellations, %d segments, %d Messier"
          % (args.out, len(data), len(stars), stars[-1]["mag"], len(names), len(CONS), nseg, len(mess)))


if __name__ == "__main__":
    main()
