#!/usr/bin/env python3
"""Builds notas_p4.pak: the bold, italic and bold italic Inter the Notes app
draws with.

The firmware carries Inter Medium only (components/aos_fonts), which is the
app's regular weight. The other three styles, in the same four sizes, would
weigh ~0.9 MB inside the .so, and every .so on the card is opened at boot:
so they travel as a pack the app reads when a note first asks for them, and
only the ones it asks for. The regular weight goes in the pack too, with the
same glyphs as the other three: the firmware's has no dashes and no curly
quotes, and a note pasted from a computer is full of them. Without the pack
the app falls back to the firmware's Medium for everything (bold drawn
twice, a pixel apart).

The pack holds lv_font_conv's own tables, unchanged: the app rebuilds the
lv_font_fmt_txt_dsc_t around them (nt_fonts.c) and LVGL draws them with its
usual lv_font_get_glyph_dsc_fmt_txt / lv_font_get_bitmap_fmt_txt.

    python3 apps/notas/tools/pack_fonts.py            -> apps/notas/build/notas_p4.pak
    python3 apps/notas/tools/pack_fonts.py --sim      and a copy in sim/sim_fs/apps

Needs lv_font_conv (npm install -g lv_font_conv) and Inter 4.1, which
tools/gen_fonts.py already keeps in ~/.cache/p4os-fonts/inter.zip (it is
downloaded here too if it is not). Inter is SIL OFL 1.1.

Format (little endian), read by nt_fonts.c:

    "NTF1"  u32 count
    count x { u8 style, u8 px, u16 0, u32 offset, u32 length }
    each font:
        i16 line_height, i16 base_line, i8 underline_position,
        i8 underline_thickness, u8 bpp, u8 kern_classes,
        u16 kern_scale, u16 cmap_num, u16 glyph_count, u8 left_cnt,
        u8 right_cnt, u32 bitmap_len
        bitmap_len bytes of bitmap
        glyph_count x { u32 bitmap_index, u16 adv_w, u8 box_w, u8 box_h,
                        i8 ofs_x, i8 ofs_y }
        cmap_num x { u32 range_start, u16 range_length, u16 glyph_id_start,
                     u16 list_length, u8 type, u8 0,
                     u16 unicode_list[list_length]  (sparse types)
                     ofs list: u8[range_length] (format0 full) or
                               u16[list_length] (sparse full) }
        kern: u8 left_map[glyph_count], u8 right_map[glyph_count],
              i8 values[left_cnt * right_cnt]

Style numbers: 0 regular, 1 bold, 2 italic, 3 bold italic.
"""

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import urllib.request
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
APP = os.path.dirname(HERE)
REPO = os.path.dirname(os.path.dirname(APP))
CACHE = os.path.expanduser("~/.cache/p4os-fonts")
ZIP = os.path.join(CACHE, "inter.zip")
ZIP_URL = "https://github.com/rsms/inter/releases/download/v4.1/Inter-4.1.zip"

STYLES = {0: "Inter-Medium.ttf", 1: "Inter-Bold.ttf", 2: "Inter-MediumItalic.ttf", 3: "Inter-BoldItalic.ttf"}
SIZES = [24, 28, 36, 48]          # the app's four text sizes (nt_fonts.h)
RANGE = ("0x20-0x7E,0xA0-0xFF,0x2013,0x2014,0x2018,0x2019,0x201C,0x201D,"
         "0x2022,0x2026,0x20AC")
TYPES = {
    "LV_FONT_FMT_TXT_CMAP_FORMAT0_FULL": 0,
    "LV_FONT_FMT_TXT_CMAP_SPARSE_FULL": 1,
    "LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY": 2,
    "LV_FONT_FMT_TXT_CMAP_SPARSE_TINY": 3,
}


def die(msg):
    sys.exit("pack_fonts: " + msg)


def ttf(name, tmp):
    if not os.path.exists(ZIP):
        os.makedirs(CACHE, exist_ok=True)
        urllib.request.urlretrieve(ZIP_URL, ZIP)
    with zipfile.ZipFile(ZIP) as z:
        return z.extract("extras/ttf/" + name, tmp)


def array(src, name):
    """The body of 'static ... name[] = { ... };' as a list of ints."""
    m = re.search(r"\b%s\[\]\s*=\s*\{(.*?)\};" % re.escape(name), src, re.S)
    if not m:
        return None
    body = re.sub(r"/\*.*?\*/", "", m.group(1), flags=re.S)
    return [int(v, 0) for v in re.findall(r"-?0x[0-9a-fA-F]+|-?\d+", body)]


def field(src, name):
    m = re.search(r"\.%s\s*=\s*(-?\w+)" % name, src)
    if not m:
        die("no .%s in lv_font_conv's output" % name)
    v = m.group(1)
    return int(v, 0) if re.match(r"-?\d|0x", v) else v


def convert(path, px, tmp):
    out = os.path.join(tmp, "f%d.c" % px)
    subprocess.run(["lv_font_conv", "--no-compress", "--no-prefilter", "--bpp", "4",
                    "--size", str(px), "--font", path, "-r", RANGE,
                    "--format", "lvgl", "-o", out, "--force-fast-kern-format"],
                   check=True, stdout=subprocess.DEVNULL)
    src = open(out, encoding="utf-8").read()

    bitmap = bytes(v & 0xFF for v in array(src, "glyph_bitmap"))
    glyphs = re.findall(r"\{\.bitmap_index = (\d+), \.adv_w = (\d+), \.box_w = (\d+), "
                        r"\.box_h = (\d+), \.ofs_x = (-?\d+), \.ofs_y = (-?\d+)\}", src)
    gl = b"".join(struct.pack("<IHBBbb", *map(int, g)) for g in glyphs)

    cm = re.search(r"cmaps\[\]\s*=\s*\{(.*)\n\};", src, re.S).group(1)
    cmaps = re.findall(r"\.range_start = (\d+), \.range_length = (\d+), \.glyph_id_start = (\d+),"
                       r"\s*\.unicode_list = (\w+), \.glyph_id_ofs_list = (\w+), "
                       r"\.list_length = (\d+), \.type = (\w+)", cm)
    cb = b""
    for start, length, gid, ulist, olist, llen, typ in cmaps:
        t = TYPES[typ]
        cb += struct.pack("<IHHHBB", int(start), int(length), int(gid), int(llen), t, 0)
        if ulist != "NULL":
            cb += struct.pack("<%dH" % int(llen), *array(src, ulist))
        if olist != "NULL":
            vals = array(src, olist)
            cb += struct.pack("<%dB" % len(vals), *vals) if t == 0 else \
                struct.pack("<%dH" % len(vals), *vals)

    kern = b""
    left = right = 0
    has_kern = "kern_classes = 1" in src
    if has_kern:
        lm = array(src, "kern_left_class_mapping")
        rm = array(src, "kern_right_class_mapping")
        kv = array(src, "kern_class_values")
        left, right = field(src, "left_class_cnt"), field(src, "right_class_cnt")
        if len(lm) != len(glyphs) or len(rm) != len(glyphs) or len(kv) != left * right:
            die("kerning tables do not add up at %d px" % px)
        kern = bytes(lm) + bytes(rm) + struct.pack("<%db" % len(kv), *kv)

    head = struct.pack("<hhbbBBHHHBBI", field(src, "line_height"), field(src, "base_line"),
                       field(src, "underline_position"), field(src, "underline_thickness"),
                       field(src, "bpp"), 1 if has_kern else 0, field(src, "kern_scale"),
                       len(cmaps), len(glyphs), left, right, len(bitmap))
    return head + bitmap + gl + cb + kern


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sim", action="store_true", help="copy it to sim/sim_fs/apps too")
    args = ap.parse_args()
    if not shutil.which("lv_font_conv"):
        die("lv_font_conv not found (npm install -g lv_font_conv)")

    fonts = []
    with tempfile.TemporaryDirectory() as tmp:
        for style, name in STYLES.items():
            path = ttf(name, tmp)
            for px in SIZES:
                fonts.append((style, px, convert(path, px, tmp)))

    table = b""
    blob = b""
    base = 8 + 12 * len(fonts)
    for style, px, data in fonts:
        table += struct.pack("<BBHII", style, px, 0, base + len(blob), len(data))
        blob += data
        blob += b"\0" * (-len(blob) % 4)
    pak = b"NTF1" + struct.pack("<I", len(fonts)) + table + blob

    out = os.path.join(APP, "build", "notas_p4.pak")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    open(out, "wb").write(pak)
    print("%s: %d fonts, %.0f KB" % (os.path.relpath(out, REPO), len(fonts), len(pak) / 1024))
    if args.sim:
        dst = os.path.join(REPO, "sim", "sim_fs", "apps")
        os.makedirs(dst, exist_ok=True)
        shutil.copy(out, dst)
        print("copied to sim/sim_fs/apps")


if __name__ == "__main__":
    main()
