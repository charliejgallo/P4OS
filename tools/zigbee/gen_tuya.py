#!/usr/bin/env python3
"""What Tuya's Zigbee devices are and what their datapoints mean, for the
card's /zigbee/tuya.txt, from Zigbee2MQTT's device definitions.

    tools/zigbee/gen_tuya.py <tuya.ts> [out]   (out: build/zigbee/zigbee_tuya.txt)

tuya.ts is src/devices/tuya.ts of zigbee-herdsman-converters (MIT licence,
https://github.com/Koenkk/zigbee-herdsman-converters). It is read as text:
each definition (a "    {" ... "    }," block at the array's level) gives
its fingerprints (Zigbee model and manufacturer names), vendor, model and
description, and its datapoints: meta.tuyaDatapoints entries
[dp, "name", converter] and the dpNumeric / dpBinary / dpEnumLookup
extends. Converters this script does not know make a raw value ("r").

The layout, one record per line, tab-separated; components/aos_154/
aos_zigbee_tuya.c reads it:

    # comment
    S <set> <dp>:<name>:<kind>[:<arg>] ...      a set of datapoints
    M <zigbee model> <manufacturer> <set|-> <vendor>|<model>|<description>

kind: b a bool (arg "i": inverted), n a number (arg: divisor, or "x10" a
multiplier; "u<unit>" after a comma), e an enum (arg "0=Off,1=On"), r raw.
Many devices share a set, so the sets come first and are referred to.
"""
import os
import re
import sys

UNITS = [
    (r"(^|_)(local_)?temperature$|_temperature$|heating_setpoint|temperature_calibration|^temp", "°C"),
    (r"humidity", "%"), (r"battery$|battery_percentage", "%"), (r"illuminance|lux", "lx"),
    (r"voltage", "V"), (r"(^|_)current$", "A"), (r"(^|_)power$|active_power", "W"),
    (r"energy", "kWh"), (r"co2", "ppm"), (r"pm25|pm10|pm1$", "µg/m³"), (r"voc", "ppm"),
    (r"formaldehyd", "mg/m³"), (r"position|brightness_percent", "%"), (r"countdown|delay|duration|time$", "s"),
    (r"distance|range", "m"),
]

ITEM = re.compile(r'\[\s*(\d+)\s*,\s*("([^"]*)"|null)\s*,\s*([^\]]+?)\s*\]\s*,?\s*$')
MANUF = re.compile(r'"(_T[A-Z0-9]{2,6}_[A-Za-z0-9]{6,})"')


def unit_of(name):
    for rx, u in UNITS:
        if re.search(rx, name):
            return u
    return ""


def lookup_pairs(text):
    """{Off: 0, On: 1} or {"a": tuya.enum(0)} -> 0=Off,1=On"""
    pairs = []
    for m in re.finditer(r'("([^"]+)"|\'([^\']+)\'|([A-Za-z_][A-Za-z0-9_]*))\s*:\s*(?:tuya\.enum\()?\s*(-?\d+)', text):
        key = m.group(2) or m.group(3) or m.group(4)
        pairs.append(f"{m.group(5)}={key}".replace(",", " ").replace(":", " ").replace("\t", " "))
    return ",".join(pairs)


def kind_of(conv, name):
    c = conv.replace(" ", "")
    u = unit_of(name)
    if re.search(r"\.(onOff|onOffNotStrict|lockUnlock|trueFalse1|trueFalseEnum1)\b", c):
        return "b"
    if re.search(r"\.(trueFalse0|trueFalseEnum0|trueFalseInvert)\b", c):
        return "b:i"
    m = re.search(r"\.divideBy(\d+)\b", c) or re.search(r"divideBy\((\d+)\)", c)
    if m:
        return f"n:{m.group(1)}" + (f",u{u}" if u else "")
    if re.search(r"\.multiplyBy10\b", c):
        return "n:x10" + (f",u{u}" if u else "")
    if re.search(r"\.(raw|countdown|coverPosition|coverPositionInverted|batteryState)\b", c) or c.endswith(".raw"):
        return "n:1" + (f",u{u}" if u else "")
    m = re.search(r"lookup\((\{.*\})", conv)
    if m:
        p = lookup_pairs(m.group(1))
        return f"e:{p}" if p else "r"
    if re.search(r"\.(temperatureUnit|temperatureUnitEnum)\b", c):
        return "e:0=°C,1=°F"
    if re.search(r"\.(powerOnBehaviorEnum|powerOnBehavior)\b", c):
        return "e:0=off,1=on,2=previous"
    return "r"


def blocks(src):
    """each definition's text: the array's elements are at four spaces"""
    lines = src.split("\n")
    out, cur = [], None
    for ln in lines:
        if ln == "    {":
            cur = []
        elif cur is not None and ln.startswith("    },") and not ln.startswith("     "):
            out.append("\n".join(cur))
            cur = None
        elif cur is not None:
            cur.append(ln)
    return out


def field(block, key):
    m = re.search(r'^\s{8}' + key + r':\s*"([^"]*)"', block, re.M)
    return m.group(1) if m else ""


def fingerprints(block):
    """(zigbee model, manufacturer) pairs"""
    out = []
    for m in re.finditer(r'tuya\.fingerprint\(\s*"([^"]+)"\s*,\s*\[([^\]]*)\]', block):
        for mf in MANUF.finditer(m.group(2)):
            out.append((m.group(1), mf.group(1)))
    for m in re.finditer(r'modelID:\s*"([^"]+)"\s*,\s*manufacturerName:\s*"([^"]+)"', block):
        out.append((m.group(1), m.group(2)))
    for m in re.finditer(r'manufacturerName:\s*"([^"]+)"\s*,\s*modelID:\s*"([^"]+)"', block):
        out.append((m.group(2), m.group(1)))
    return out


def datapoints(block):
    dps = {}
    m = re.search(r"tuyaDatapoints:\s*\[(.*?)\n\s{12}\],", block, re.S)
    if m:
        for ln in m.group(1).split("\n"):
            it = ITEM.search(ln.strip())
            if not it or it.group(2) == "null":
                continue
            dp, name = int(it.group(1)), it.group(3)
            if dp in dps:
                continue                        # the first name a dp has
            dps[dp] = (name, kind_of(it.group(4), name))
    for m in re.finditer(r"dp(Numeric|Binary|EnumLookup)\(\s*\{", block):
        # the call's object, braces balanced
        i, depth = m.end(), 1
        while i < len(block) and depth:
            depth += {"{": 1, "}": -1}.get(block[i], 0)
            i += 1
        body = block[m.end():i - 1]
        dpm = re.search(r"\bdp:\s*(\d+)", body)
        nm = re.search(r'\bname:\s*"([^"]+)"', body)
        if not dpm or not nm:
            continue
        dp, name = int(dpm.group(1)), nm.group(1)
        if dp in dps:
            continue
        if m.group(1) == "Binary":
            k = "b"
            if re.search(r"valueOn:\s*\[\s*\"?ON\"?\s*,\s*(false|0)\b", body):
                k = "b:i"
        elif m.group(1) == "Numeric":
            sc = re.search(r"\bscale:\s*(\d+)", body)
            un = re.search(r'\bunit:\s*"([^"]+)"', body)
            u = un.group(1) if un else unit_of(name)
            k = f"n:{sc.group(1) if sc else 1}" + (f",u{u}" if u else "")
        else:
            lk = re.search(r"lookup:\s*(\{[^}]*\})", body)
            p = lookup_pairs(lk.group(1)) if lk else ""
            k = f"e:{p}" if p else "r"
        dps[dp] = (name, k)
    return dps


def clean(s):
    return " ".join(s.replace("\t", " ").replace("|", "/").split())


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    src = open(sys.argv[1], encoding="utf-8").read()
    out = sys.argv[2] if len(sys.argv) > 2 else "build/zigbee/zigbee_tuya.txt"
    sets, set_ix, recs, seen = [], {}, [], set()
    for b in blocks(src):
        fps = fingerprints(b)
        if not fps:
            continue
        dps = datapoints(b)
        key = " ".join(f"{dp}:{n.replace(':', '_').replace(' ', '_')}:{k}" for dp, (n, k) in sorted(dps.items()))
        si = "-"
        if key:
            if key not in set_ix:
                set_ix[key] = len(sets)
                sets.append(key)
            si = str(set_ix[key])
        ident = f"{clean(field(b, 'vendor'))}|{clean(field(b, 'model'))}|{clean(field(b, 'description'))}"
        for zm, mf in fps:
            if (zm, mf) in seen:
                continue                        # the first definition of a fingerprint wins, as in Z2M
            seen.add((zm, mf))
            recs.append(f"M\t{zm}\t{mf}\t{si}\t{ident}")
    os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
    with open(out, "w", encoding="utf-8") as f:
        f.write("# P4OS: Tuya Zigbee devices and their datapoints, from zigbee-herdsman-converters "
                "(MIT, Koenkk and contributors): tools/zigbee/gen_tuya.py\n")
        for i, s in enumerate(sets):
            f.write(f"S\t{i}\t{s}\n")
        for r in sorted(recs, key=lambda r: r.split("\t")[2]):
            f.write(r + "\n")
    print(f"{len(recs)} fingerprints, {len(sets)} sets of datapoints, {os.path.getsize(out)} bytes in {out}")


if __name__ == "__main__":
    main()
