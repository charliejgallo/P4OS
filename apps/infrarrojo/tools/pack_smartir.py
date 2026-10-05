#!/usr/bin/env python3
"""Builds infrarrojo_p4.pak: SmartIR's code library, for the Infrarrojo app.

SmartIR (https://github.com/smartHomeHub/SmartIR, and the fork by
litinoveweedle it comes through) is MIT licensed: Copyright (c) 2019 Vassilis
Panos, 2024 Li Tin O've Weedle. Its device files - televisions, air
conditioners, fans and lights, by brand and model - are read here from a
local checkout and travel to the card as this pack; they are not in git.

    python3 apps/infrarrojo/tools/pack_smartir.py [--src DIR] [--sim]

--src is a checkout of SmartIR, or of the user's fork smartir-universal
(default: ../smartir-universal next to the repo). Its
custom_components/smartir/code_converter.py turns every stored encoding
(Broadlink base64 or hex, Pronto, raw, Tuya) into marks and spaces in
microseconds, mark first; that is the form kept here.

The JSON is 56 MB, and its climate files are most of it: an air conditioner
sends its whole state in every frame, so a device file has one code per
combination of mode, fan, swing and temperature, ~100 000 codes in all.
Two things bring it down to ~8 MB:

  - Within a device, every mark and every space is snapped to a few levels
    (a receiver tolerates 20 %; the levels are the median of what they
    replace, within 15 %), and a code becomes a list of (mark, space)
    pairs, each an index into the device's table of pairs.
  - The index is written in as few bits as the table needs: four or five
    for most air conditioners (header, zero, one, the gaps between frames,
    the closing mark, and what each remote does on top).

Format (little endian), read by ir_pack.c:

    "IRP1"  u32 count  u32 strings_offset  u32 strings_length
    count x { u16 id, u8 class, u8 0, u32 brand, u32 models, u32 blob }
        id: the SmartIR file's number; class 0 media player, 1 climate,
        2 fan, 3 light; brand and models are offsets into the strings,
        NUL-terminated UTF-8 (models joined with ", "); blob from the start
        of the file
    strings
    each blob:
        u32 json_length, json_length bytes of JSON: the device file without
            its codes, its "commands" tree with each code replaced by its
            index (a list of codes stays a list of indices)
        u16 pairs, pairs x { u16 mark, u16 space }   (space 0: the frame ends
            on that mark; 65535: a gap of at least that)
        u8 bits
        u16 codes, codes x u32 offset from the end of this table
        each code: u16 pair count, then the pair indices, `bits` each, packed
            from the low bit of each byte
"""
import argparse
import base64
import binascii
import json
import math
import os
import shutil
import struct
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
CLASSES = ["media_player", "climate", "fan", "light"]


def load_converter(src):
    sys.path.insert(0, os.path.join(src, "custom_components", "smartir"))
    try:
        import code_converter as cc  # the fork's
        return cc
    except ImportError:
        return None


def broadlink_pulses(data):
    """A Broadlink IR packet (type 0x26) to microseconds; None if it is RF."""
    if len(data) < 4 or data[0] != 0x26:
        return None
    length = data[2] | data[3] << 8
    payload = data[4:4 + length]
    out, i = [], 0
    while i < len(payload):
        v = payload[i]
        i += 1
        if v == 0:
            if i + 1 >= len(payload):
                break
            v = payload[i] << 8 | payload[i + 1]
            i += 2
        out.append(int(round(v * 8192.0 / 269.0)))
    return out


def b64(text):
    text = text.strip()
    return base64.b64decode(text + "=" * (-len(text) % 4))


def to_pulses(cc, code, enc):
    """The declared encoding first, then what the code looks like: some
    files say Raw and carry Tuya or Broadlink base64."""
    if not isinstance(code, str) or not code.strip():
        return None
    tries = [enc] + [e for e in ("Base64", "Tuya", "Raw", "Pronto", "Hex") if e != enc]
    for e in tries:
        try:
            if e == "Base64":
                p = broadlink_pulses(b64(code))
            elif e == "Hex":
                p = broadlink_pulses(binascii.unhexlify(code.replace(" ", "")))
            elif cc:
                p = cc.decode_to_pulses(code, e)
            else:
                continue
        except Exception:
            continue
        if p and len(p) >= 4 and all(0 < x < 1_000_000 for x in p):
            return [int(x) for x in p]
    return None


def clusters(values, rel):
    """Sorted greedy clustering: a level per run of values within rel of
    its first; each level is the median of its run."""
    levels = {}
    vals = sorted(set(values))
    run = []
    def flush():
        if run:
            med = run[len(run) // 2]
            for v in run:
                levels[v] = med
    for v in vals:
        if run and v > run[0] * (1 + rel) + 40:
            flush()
            run = []
        run.append(v)
    flush()
    return levels


def pack_device(cc, path, cls):
    with open(path, encoding="utf-8") as f:
        d = json.load(f)
    enc = d.get("commandsEncoding", "Base64")
    codes = []          # pulses, deduplicated
    seen = {}

    def walk(node):
        if isinstance(node, dict):
            out = {}
            for k, v in node.items():
                w = walk(v)
                if w is not None:
                    out[str(k)] = w
            return out or None
        if isinstance(node, list):
            out = [w for w in (walk(v) for v in node) if w is not None]
            return out or None
        p = to_pulses(cc, node, enc)
        if p is None:
            return None
        p = [min(x, 65535) for x in p]
        key = tuple(p)
        if key not in seen:
            seen[key] = len(codes)
            codes.append(p)
        return seen[key]

    tree = walk(d.get("commands", {}))
    if not codes:
        return None
    marks = [x for p in codes for x in p[0::2]]
    spaces = [x for p in codes for x in p[1::2]]
    mq = clusters(marks, 0.15)
    sq = clusters([s for s in spaces if s < 65535], 0.15)
    sq[65535] = 65535
    table, index = [], {}
    seqs = []
    for p in codes:
        seq = []
        for i in range(0, len(p), 2):
            m = mq[p[i]]
            s = sq[p[i + 1]] if i + 1 < len(p) else 0
            k = (m, s)
            if k not in index:
                index[k] = len(table)
                table.append(k)
            seq.append(index[k])
        seqs.append(seq)
    bits = max(1, math.ceil(math.log2(len(table)))) if len(table) > 1 else 1
    if len(table) > 65535 or len(codes) > 65535:
        return None

    meta = {k: v for k, v in d.items()
            if k not in ("commands", "commandsEncoding", "supportedController") and not k.startswith("_")}
    meta["commands"] = tree
    js = json.dumps(meta, ensure_ascii=False, separators=(",", ":")).encode("utf-8")

    body = bytearray()
    offsets = []
    for seq in seqs:
        offsets.append(len(body))
        body += struct.pack("<H", len(seq))
        acc = nb = 0
        out = bytearray()
        for v in seq:
            acc |= v << nb
            nb += bits
            while nb >= 8:
                out.append(acc & 0xFF)
                acc >>= 8
                nb -= 8
        if nb:
            out.append(acc & 0xFF)
        body += out
    blob = bytearray(struct.pack("<I", len(js)) + js)
    blob += struct.pack("<H", len(table))
    for m, s in table:
        blob += struct.pack("<HH", m, s)
    blob += struct.pack("<BH", bits, len(codes))
    for o in offsets:
        blob += struct.pack("<I", o)
    blob += body
    models = d.get("supportedModels") or []
    return {
        "id": int(os.path.splitext(os.path.basename(path))[0]),
        "cls": cls,
        "brand": str(d.get("manufacturer", "?")).strip(),
        "models": ", ".join(str(m).strip() for m in models),
        "blob": bytes(blob),
        "codes": len(codes),
        "pairs": len(table),
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default=os.path.join(ROOT, "..", "smartir-universal"))
    ap.add_argument("--out", default=os.path.join(ROOT, "apps", "infrarrojo", "build", "infrarrojo_p4.pak"))
    ap.add_argument("--sim", action="store_true", help="and a copy in sim/sim_fs/apps")
    a = ap.parse_args()
    src = os.path.abspath(a.src)
    codes_dir = os.path.join(src, "codes")
    if not os.path.isdir(codes_dir):
        codes_dir = os.path.join(src, "custom_components", "smartir", "codes")
    if not os.path.isdir(codes_dir):
        sys.exit(f"no SmartIR codes/ in {src}")
    cc = load_converter(src)
    if not cc:
        print("warning: no code_converter.py; only Broadlink codes will be read")

    devs = []
    skipped = 0
    for ci, cls in enumerate(CLASSES):
        d = os.path.join(codes_dir, cls)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d), key=lambda n: (len(n), n)):
            if not name.endswith(".json") or not name[:-5].isdigit():
                continue
            try:
                dev = pack_device(cc, os.path.join(d, name), ci)
            except Exception as e:
                print(f"  {cls}/{name}: {e}")
                dev = None
            if dev is None or dev["id"] > 65535:
                skipped += 1
                continue
            devs.append(dev)
    devs.sort(key=lambda v: (v["cls"], v["brand"].lower(), v["models"].lower(), v["id"]))

    strings = bytearray()
    soff = {}
    def s(text):
        if text not in soff:
            soff[text] = len(strings)
            strings.extend(text.encode("utf-8") + b"\0")
        return soff[text]
    entries = [(v, s(v["brand"]), s(v["models"])) for v in devs]
    head = 16
    index_len = 16 * len(devs)
    str_off = head + index_len
    blob_off = str_off + len(strings)
    out = bytearray(b"IRP1" + struct.pack("<III", len(devs), str_off, len(strings)))
    at = blob_off
    for v, b, m in entries:
        out += struct.pack("<HBBIII", v["id"], v["cls"], 0, b, m, at)
        at += len(v["blob"])
    out += strings
    for v, _, _ in entries:
        out += v["blob"]

    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "wb") as f:
        f.write(out)
    by = [sum(1 for v in devs if v["cls"] == i) for i in range(4)]
    print(f"{a.out}: {len(devs)} devices ({by[0]} TV/audio, {by[1]} air conditioners, "
          f"{by[2]} fans, {by[3]} lights), {sum(v['codes'] for v in devs)} codes, "
          f"{len(out) / 1e6:.1f} MB; {skipped} files without usable codes")
    if len(out) > 8 * 1024 * 1024:
        print("note: over 8 MB; install_apps.sh uploads it whole, which is fine for this one")
    if a.sim:
        dst = os.path.join(ROOT, "sim", "sim_fs", "apps")
        os.makedirs(dst, exist_ok=True)
        shutil.copy(a.out, dst)
        print(f"copied to {dst}")


if __name__ == "__main__":
    main()
